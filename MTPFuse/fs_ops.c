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
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Skip eager prefetch on open for huge files (SD-card videos): Finder
 * touching them during folder browse won't pull gigabytes up front. */
#define MTP_OP_OPEN_PREFETCH_MAX ((uint64_t)64 * 1024 * 1024)

static pthread_mutex_t g_stage_mu = PTHREAD_MUTEX_INITIALIZER;

static double fuse_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

typedef struct {
    int   fd;          /* fd of temp staging file */
    char *path;        /* logical mount path, owned */
    int   dirty;       /* set on any write */
    int   created;     /* file did not exist on device at open */
    int   cache_ready; /* 1: temp fd has full device payload (see op_read) */
} handle_t;

static int op_getattr(const char *path, struct stat *st)
{
    static unsigned long getattr_seq;
    double t0 = fuse_mono_ms();
    memset(st, 0, sizeof(*st));
    mtp_stat_t s;
    int rc = mtp_stat(path, &s);
    mtp_debug_log("fuse getattr #%lu path=%s mtp_rc=%d %.2fms",
                  ++getattr_seq, path, rc, fuse_mono_ms() - t0);
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

/* libfuse: filler's last arg is the dirent offset for seekdir; must be
 * unique and monotonic. Incoming `off` is the continuation cookie. */
static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t off, struct fuse_file_info *fi)
{
    (void)fi;
    static unsigned long readdir_seq;
    unsigned long seq = ++readdir_seq;
    double t0 = fuse_mono_ms();
    mtp_debug_log("fuse readdir #%lu BEGIN path=%s off=%lld",
                  seq, path, (long long)off);

    mtp_dirent_t *entries = NULL;
    size_t nent = 0;
    int rc = mtp_readdir_snapshot(path, &entries, &nent);
    if (rc != 0) {
        mtp_debug_log("fuse readdir #%lu snapshot FAIL path=%s rc=%d %.2fms",
                      seq, path, rc, fuse_mono_ms() - t0);
        return rc;
    }

    struct stat st_dir;
    memset(&st_dir, 0, sizeof(st_dir));
    st_dir.st_mode = S_IFDIR | 0755;

    int buf_full = 0;
    off_t cookie = 1;
    if (cookie > off) {
        if (filler(buf, ".", &st_dir, cookie)) {
            buf_full = 1;
            goto out;
        }
    }
    cookie++;

    if (cookie > off) {
        if (filler(buf, "..", &st_dir, cookie)) {
            buf_full = 1;
            goto out;
        }
    }
    cookie++;

    for (size_t i = 0; i < nent; i++) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_mode = entries[i].is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
        if (cookie > off) {
            if (filler(buf, entries[i].name, &st, cookie)) {
                buf_full = 1;
                goto out;
            }
        }
        cookie++;
    }

out:
    mtp_readdir_snapshot_free(entries, nent);
    mtp_debug_log("fuse readdir #%lu END path=%s nent=%zu buf_full=%d %.2fms",
                  seq, path, nent, buf_full, fuse_mono_ms() - t0);
    return 0;
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
    double t0 = fuse_mono_ms();
    mtp_stat_t s;
    int rc = mtp_stat(path, &s);
    if (rc != 0) return rc;
    if (s.is_dir) return -EISDIR;

    handle_t *h = make_handle(path);
    if (!h) return -ENOMEM;

    int did_prefetch = 0;
    h->cache_ready = 0;
    /* If we may read from it, pull the contents down once (not for giants). */
    if ((fi->flags & O_ACCMODE) != O_WRONLY && s.size > 0 &&
        s.size <= MTP_OP_OPEN_PREFETCH_MAX) {
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
        did_prefetch = 1;
        h->cache_ready = 1;
    }

    if (fi->flags & O_TRUNC) {
        ftruncate(h->fd, 0);
        h->dirty = 1;
        h->cache_ready = 1; /* local empty; do not re-download from device */
    }
    fi->fh = (uint64_t)(uintptr_t)h;
    mtp_debug_log("fuse open path=%s flags=0x%x size=%llu prefetch=%d cache_ready=%d %.2fms",
                  path, fi->flags, (unsigned long long)s.size, did_prefetch,
                  h->cache_ready, fuse_mono_ms() - t0);
    return 0;
}

static int op_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)mode;
    handle_t *h = make_handle(path);
    if (!h) return -ENOMEM;
    h->dirty = 1;
    h->created = 1;
    h->cache_ready = 1; /* empty staging file; not on device until release */
    fi->fh = (uint64_t)(uintptr_t)h;
    return 0;
}

static int op_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi)
{
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (!h) return -EBADF;
    if (!h->cache_ready) {
        if (h->created) {
            /* op_create: nothing on device to pull */
            h->cache_ready = 1;
        } else {
            pthread_mutex_lock(&g_stage_mu);
            if (!h->cache_ready) {
                int st = mtp_download_to_fd(path, h->fd);
                if (st != 0) {
                    pthread_mutex_unlock(&g_stage_mu);
                    return st;
                }
                h->cache_ready = 1;
            }
            pthread_mutex_unlock(&g_stage_mu);
        }
    }
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

static int op_flush(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    /* Staging fd is private; nothing to push until release unless dirty. */
    (void)fi;
    return 0;
}

static int op_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)path;
    (void)datasync;
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (!h) return 0;
    if (fsync(h->fd) < 0) return -errno;
    return 0;
}

static int op_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (!h) return 0;
    double t0 = fuse_mono_ms();
    int was_dirty = h->dirty;
    int rc = 0;
    if (h->dirty) {
        off_t end = lseek(h->fd, 0, SEEK_END);
        if (end < 0)
            rc = -errno;
        else {
            int wr = mtp_write_full_fd(h->path, h->fd, (size_t)end);
            if (wr < 0) rc = wr;
        }
    }
    close(h->fd);
    mtp_debug_log("fuse release path=%s had_dirty=%d rc=%d %.2fms",
                  h->path, was_dirty, rc, fuse_mono_ms() - t0);
    free(h->path);
    free(h);
    return rc;
}

static int op_rename(const char *from, const char *to)
{
    return mtp_rename(from, to);
}

static int op_unlink(const char *path)        { return mtp_unlink(path); }
static int op_mkdir (const char *path, mode_t m){ (void)m; return mtp_mkdir(path); }
static int op_rmdir (const char *path)        { return mtp_rmdir(path); }

static int op_chmod (const char *p, mode_t m) { (void)p;(void)m; return 0; }
static int op_chown (const char *p, uid_t u, gid_t g) { (void)p;(void)u;(void)g; return 0; }
static int op_utimens(const char *p, const struct timespec t[2]) { (void)p;(void)t; return 0; }

static int op_statfs(const char *path, struct statvfs *st)
{
    static unsigned long statfs_seq;
    if ((++statfs_seq % 10UL) == 0UL)
        mtp_debug_log("fuse statfs #%lu path=%s", statfs_seq, path);
    memset(st, 0, sizeof(*st));
    st->f_bsize  = 4096;
    st->f_frsize = 4096;
    st->f_blocks = 1024 * 1024 * 64;
    st->f_bfree  = 1024 * 1024 * 32;
    st->f_bavail = 1024 * 1024 * 32;
    st->f_namemax = 255;
    return 0;
}

#ifdef __APPLE__
static int op_getxattr(const char *path, const char *name, char *value, size_t size,
                       uint32_t position)
{
    (void)position;
#else
static int op_getxattr(const char *path, const char *name, char *value, size_t size)
{
#endif
    static unsigned long xattr_seq;
    mtp_debug_log("fuse getxattr #%lu path=%s name=%s", ++xattr_seq, path, name ? name : "?");
    (void)value;
    (void)size;
#ifdef ENOATTR
    return -ENOATTR;
#else
    return -ENODATA;
#endif
}

static int op_listxattr(const char *path, char *list, size_t size)
{
    (void)path;
    (void)list;
    (void)size;
    return 0;
}

#ifdef __APPLE__
static int op_setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags, uint32_t position)
{
    (void)position;
#else
static int op_setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags)
{
#endif
    (void)path;
    (void)name;
    (void)value;
    (void)size;
    (void)flags;
    return -ENOTSUP;
}

static int op_removexattr(const char *path, const char *name)
{
    (void)path;
    (void)name;
    return -ENOTSUP;
}

struct fuse_operations mtpfuse_ops = {
    .getattr     = op_getattr,
    .readdir     = op_readdir,
    .open        = op_open,
    .create      = op_create,
    .read        = op_read,
    .write       = op_write,
    .flush       = op_flush,
    .fsync       = op_fsync,
    .release     = op_release,
    .truncate    = op_truncate,
    .ftruncate   = op_ftruncate,
    .rename      = op_rename,
    .unlink      = op_unlink,
    .mkdir       = op_mkdir,
    .rmdir       = op_rmdir,
    .chmod       = op_chmod,
    .chown       = op_chown,
    .utimens     = op_utimens,
    .statfs      = op_statfs,
    .getxattr    = op_getxattr,
    .listxattr   = op_listxattr,
    .setxattr    = op_setxattr,
    .removexattr = op_removexattr,
};
