/*
 * fs_ops.c — FUSE 2.x operation table.
 *
 * Strategy for file IO: MTP is not a streaming file system. Each open
 * creates a backing temp file that we either populate from the device
 * (for read access) or leave empty (for create). All read/write hit the
 * temp file. On release, if the file was written, the temp file is
 * pushed back to the device with mtp_write_full().
 */

#define _DARWIN_C_SOURCE 1
#include "fs_ops.h"
#include "mtp_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    int   fd;          /* fd of temp staging file */
    char *path;        /* logical mount path, owned */
    int   dirty;       /* set on any write */
    int   created;     /* file did not exist on device at open */
} handle_t;

static int op_getattr(const char *path, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    mtp_stat_t s;
    int rc = mtp_stat(path, &s);
    if (rc != 0) return rc;
    if (s.is_dir) {
        st->st_mode  = S_IFDIR | 0755;
        st->st_nlink = 2;
    } else {
        st->st_mode  = S_IFREG | 0644;
        st->st_nlink = 1;
        st->st_size  = (off_t)s.size;
    }
    st->st_uid = getuid();
    st->st_gid = getgid();
    if (s.mtime) {
        st->st_mtime = (time_t)s.mtime;
        st->st_atime = (time_t)s.mtime;
        st->st_ctime = (time_t)s.mtime;
    }
    return 0;
}

struct rd_ctx { void *buf; fuse_fill_dir_t filler; };

static void rd_cb(const char *name, int is_dir, void *vctx)
{
    struct rd_ctx *c = vctx;
    struct stat st; memset(&st, 0, sizeof(st));
    st.st_mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    c->filler(c->buf, name, &st, 0);
}

static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi)
{
    (void)offset; (void)fi;
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_mode = S_IFDIR | 0755; filler(buf, ".",  &st, 0);
    filler(buf, "..", &st, 0);
    struct rd_ctx c = { buf, filler };
    return mtp_readdir(path, rd_cb, &c);
}

static handle_t *make_handle(const char *path)
{
    handle_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->path = strdup(path);
    char tmpl[] = "/tmp/mtpfuse_hXXXXXX";
    h->fd = mkstemp(tmpl);
    if (h->fd < 0) { free(h->path); free(h); return NULL; }
    unlink(tmpl);
    return h;
}

static int op_open(const char *path, struct fuse_file_info *fi)
{
    mtp_stat_t s;
    int rc = mtp_stat(path, &s);
    if (rc != 0) return rc;
    if (s.is_dir) return -EISDIR;

    handle_t *h = make_handle(path);
    if (!h) return -ENOMEM;

    /* If we may read from it, pull the contents down once. */
    if ((fi->flags & O_ACCMODE) != O_WRONLY && s.size > 0) {
        char *buf = malloc((size_t)s.size);
        if (!buf) { close(h->fd); free(h->path); free(h); return -ENOMEM; }
        int got = mtp_read(path, buf, (size_t)s.size, 0);
        if (got < 0) {
            free(buf); close(h->fd); free(h->path); free(h); return got;
        }
        ssize_t off = 0;
        while (off < got) {
            ssize_t w = write(h->fd, buf + off, got - off);
            if (w <= 0) {
                free(buf); close(h->fd); free(h->path); free(h);
                return -EIO;
            }
            off += w;
        }
        free(buf);
    }

    if (fi->flags & O_TRUNC) {
        ftruncate(h->fd, 0);
        h->dirty = 1;
    }
    fi->fh = (uint64_t)(uintptr_t)h;
    return 0;
}

static int op_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)mode;
    handle_t *h = make_handle(path);
    if (!h) return -ENOMEM;
    h->dirty = 1;
    h->created = 1;
    fi->fh = (uint64_t)(uintptr_t)h;
    return 0;
}

static int op_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi)
{
    (void)path;
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (!h) return -EBADF;
    ssize_t r = pread(h->fd, buf, size, offset);
    return (r < 0) ? -errno : (int)r;
}

static int op_write(const char *path, const char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
    (void)path;
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (!h) return -EBADF;
    ssize_t w = pwrite(h->fd, buf, size, offset);
    if (w < 0) return -errno;
    h->dirty = 1;
    return (int)w;
}

static int op_truncate(const char *path, off_t size)
{
    /* Best-effort: only meaningful while a handle is open. We allow it
     * silently so that editors that truncate-then-write can succeed. */
    (void)path; (void)size;
    return 0;
}

static int op_ftruncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void)path;
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (!h) return -EBADF;
    if (ftruncate(h->fd, size) < 0) return -errno;
    h->dirty = 1;
    return 0;
}

static int op_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (!h) return 0;
    int rc = 0;
    if (h->dirty) {
        off_t end = lseek(h->fd, 0, SEEK_END);
        if (end < 0) end = 0;
        char *buf = malloc((size_t)end);
        if (buf) {
            ssize_t off = 0;
            while (off < end) {
                ssize_t r = pread(h->fd, buf + off, end - off, off);
                if (r <= 0) break;
                off += r;
            }
            int wr = mtp_write_full(h->path, buf, (size_t)off);
            if (wr < 0) rc = wr;
            free(buf);
        }
    }
    close(h->fd);
    free(h->path);
    free(h);
    return rc;
}

static int op_unlink(const char *path)        { return mtp_unlink(path); }
static int op_mkdir (const char *path, mode_t m){ (void)m; return mtp_mkdir(path); }
static int op_rmdir (const char *path)        { return mtp_rmdir(path); }

static int op_chmod (const char *p, mode_t m) { (void)p;(void)m; return 0; }
static int op_chown (const char *p, uid_t u, gid_t g) { (void)p;(void)u;(void)g; return 0; }
static int op_utimens(const char *p, const struct timespec t[2]) { (void)p;(void)t; return 0; }

static int op_statfs(const char *path, struct statvfs *st)
{
    (void)path;
    memset(st, 0, sizeof(*st));
    st->f_bsize  = 4096;
    st->f_frsize = 4096;
    st->f_blocks = 1024 * 1024 * 64;
    st->f_bfree  = 1024 * 1024 * 32;
    st->f_bavail = 1024 * 1024 * 32;
    st->f_namemax = 255;
    return 0;
}

struct fuse_operations mtpfuse_ops = {
    .getattr   = op_getattr,
    .readdir   = op_readdir,
    .open      = op_open,
    .create    = op_create,
    .read      = op_read,
    .write     = op_write,
    .release   = op_release,
    .truncate  = op_truncate,
    .ftruncate = op_ftruncate,
    .unlink    = op_unlink,
    .mkdir     = op_mkdir,
    .rmdir     = op_rmdir,
    .chmod     = op_chmod,
    .chown     = op_chown,
    .utimens   = op_utimens,
    .statfs    = op_statfs,
};
