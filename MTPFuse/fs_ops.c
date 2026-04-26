/*
 * fs_ops.c — FUSE 2.x operation table.
 *
 * Strategy for file IO: MTP is not a streaming file system. Each open
 * creates a backing temp file that we either populate from the device
 * (for read access) or leave empty (for create). Reads/writes normally
 * hit the staging fd. When the device supports GetPartialObject and the
 * file is larger than MTP_OP_OPEN_PREFETCH_MAX, read-only (or read‑first
 * RDWR) opens stream via libmtp partial reads instead of a multi‑GB
 * Get_File, which avoids Finder “device disappeared” on long USB pulls.
 * On release, if the file was written, the staging file is pushed with
 * mtp_write_full_fd().
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
#include <limits.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/mount.h>
#include <sys/xattr.h>
#endif

/* Skip eager prefetch on open for huge files (SD-card videos): Finder
 * touching them during folder browse won't pull gigabytes up front. */
#define MTP_OP_OPEN_PREFETCH_MAX ((uint64_t)64 * 1024 * 1024)

/* Serializes lazy mtp_download_to_fd for one handle; never nest with g_lock. */
static pthread_mutex_t g_stage_mu = PTHREAD_MUTEX_INITIALIZER;

static double fuse_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Stable inode: real MTP handles, synthetic bit for storage volume nodes. */
static ino_t mtp_stat_ino(const mtp_stat_t *s)
{
    if (s->object_id)
        return (ino_t)s->object_id;
    if (s->storage_id)
        return (ino_t)(0xC000000000000000ULL | (uint64_t)s->storage_id);
    return 1;
}

/* Fill struct stat from bridge metadata (used by getattr and readdir). */
static void mtp_fill_stat(const mtp_stat_t *s, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    if (s->is_dir) {
        st->st_mode  = S_IFDIR | 0777;
        st->st_nlink = 2;
    } else {
        st->st_mode  = S_IFREG | 0666;
        st->st_nlink = 1;
        st->st_size  = (off_t)s->size;
        if (s->size)
            st->st_blocks = (blkcnt_t)((s->size + 511) / 512);
    }
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_ino = mtp_stat_ino(s);

    time_t mt = (time_t)s->mtime;
    if (mt == 0 && s->is_dir)
        mt = time(NULL);

    st->st_mtime = mt;
    st->st_atime = mt;
    st->st_ctime = mt;
#if defined(__APPLE__)
    /* LIBMTP_file_t only has modificationdate; use it for birthtime too. */
    st->st_birthtime = mt;
#endif
}

static void parent_path_for_readdir(const char *path, char *out, size_t cap)
{
    if (!path || path[0] != '/' || cap < 2) {
        if (cap) out[0] = '\0';
        return;
    }
    if (path[1] == '\0') {
        out[0] = '/';
        out[1] = '\0';
        return;
    }
    const char *slash = strrchr(path + 1, '/');
    if (!slash) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }
    if (slash == path + 1) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n + 1 > cap) {
        out[0] = '\0';
        return;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

typedef struct {
    int   fd;          /* fd of temp staging file */
    char *path;        /* logical mount path, owned */
    int   dirty;       /* set on any write */
    int   created;     /* file did not exist on device at open */
    int   cache_ready; /* 1: temp fd has full device payload (see op_read) */
    int   use_partial; /* 1: reads use mtp_read_partial until write/truncate */
    uint32_t object_id;
    uint64_t remote_size;
} handle_t;

/* Paths with op_create() open but not yet on the device: Finder stat()s the
 * destination before close(); mtp_stat() would ENOENT without this. */
typedef struct pending_create {
    char *path;
    handle_t *h;
    struct pending_create *next;
} pending_create_t;

static pthread_mutex_t g_pending_mu = PTHREAD_MUTEX_INITIALIZER;
static pending_create_t *g_pending_creates;

static void pending_add(const char *path, handle_t *h)
{
    pending_create_t *e = malloc(sizeof(*e));
    if (!e) return;
    e->path = strdup(path);
    if (!e->path) { free(e); return; }
    e->h = h;
    pthread_mutex_lock(&g_pending_mu);
    e->next = g_pending_creates;
    g_pending_creates = e;
    pthread_mutex_unlock(&g_pending_mu);
}

static void pending_remove(handle_t *h)
{
    pthread_mutex_lock(&g_pending_mu);
    pending_create_t **pp = &g_pending_creates;
    while (*pp) {
        if ((*pp)->h == h) {
            pending_create_t *d = *pp;
            *pp = d->next;
            free(d->path);
            free(d);
            pthread_mutex_unlock(&g_pending_mu);
            return;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_pending_mu);
}

/* Pending getattr: 0 = filled st, 1 = not a pending create, <0 = -errno (fstat). */
static int pending_getattr(const char *path, struct stat *st)
{
    pthread_mutex_lock(&g_pending_mu);
    for (pending_create_t *e = g_pending_creates; e; e = e->next) {
        if (strcmp(e->path, path) != 0) continue;
        handle_t *ph = e->h;
        if (!ph->created) continue;
        struct stat fst;
        if (fstat(ph->fd, &fst) < 0) {
            int ecopy = errno;
            pthread_mutex_unlock(&g_pending_mu);
            return -ecopy;
        }
        memset(st, 0, sizeof(*st));
        st->st_mode  = S_IFREG | 0666;
        st->st_nlink = 1;
        st->st_size  = fst.st_size;
        if (fst.st_size)
            st->st_blocks = (blkcnt_t)((fst.st_size + 511) / 512);
        st->st_uid = getuid();
        st->st_gid = getgid();
        st->st_ino = (ino_t)(uintptr_t)ph;
        time_t t = time(NULL);
        st->st_mtime = st->st_atime = st->st_ctime = t;
#if defined(__APPLE__)
        st->st_birthtime = t;
#endif
        pthread_mutex_unlock(&g_pending_mu);
        return 0;
    }
    pthread_mutex_unlock(&g_pending_mu);
    return 1;
}

/* Finder copies metadata with setxattr then reads it back with getxattr. We
 * previously returned success from setxattr but ENOATTR from getxattr, which
 * breaks the post-copy metadata phase (“no permission” after bytes copied). */
typedef struct xa_ent {
    struct xa_ent *next;
    char          *path;
    char          *name;
    unsigned char *val;
    size_t         vlen;
} xa_ent_t;

static xa_ent_t       *g_xa;
static pthread_mutex_t g_xa_mu = PTHREAD_MUTEX_INITIALIZER;

enum { XA_MAX_NODES = 450, XA_MAX_VALUE = 65536 };

static void xa_free_ent(xa_ent_t *e)
{
    if (!e)
        return;
    free(e->path);
    free(e->name);
    free(e->val);
    free(e);
}

static size_t xa_count_unlocked(void)
{
    size_t n = 0;
    for (xa_ent_t *e = g_xa; e; e = e->next)
        n++;
    return n;
}

static void xa_evict_tail_unlocked(void)
{
    if (!g_xa)
        return;
    xa_ent_t **pp = &g_xa;
    while ((*pp)->next)
        pp = &(*pp)->next;
    xa_ent_t *d = *pp;
    *pp = NULL;
    xa_free_ent(d);
}

static int path_has_prefix(const char *path, const char *prefix)
{
    size_t pl = strlen(prefix);
    if (pl == 0)
        return 0;
    if (strncmp(path, prefix, pl) != 0)
        return 0;
    return path[pl] == '\0' || path[pl] == '/';
}

static void xa_rename_paths(const char *from, const char *to)
{
    size_t fl = strlen(from);
    pthread_mutex_lock(&g_xa_mu);
    for (xa_ent_t *e = g_xa; e; e = e->next) {
        const char *p = e->path;
        char       *np = NULL;
        if (strcmp(p, from) == 0) {
            np = strdup(to);
        } else if (path_has_prefix(p, from)) {
            size_t tlen = strlen(to);
            size_t slen = strlen(p + fl);
            np = malloc(tlen + slen + 1);
            if (np) {
                memcpy(np, to, tlen);
                memcpy(np + tlen, p + fl, slen + 1);
            }
        }
        if (np) {
            free(e->path);
            e->path = np;
        }
    }
    pthread_mutex_unlock(&g_xa_mu);
}

static void xa_purge_path(const char *path, int is_dir)
{
    size_t L = strlen(path);
    pthread_mutex_lock(&g_xa_mu);
    xa_ent_t **pp = &g_xa;
    while (*pp) {
        int drop = 0;
        if (strcmp((*pp)->path, path) == 0)
            drop = 1;
        else if (is_dir && L > 0 && strncmp((*pp)->path, path, L) == 0 &&
                 (*pp)->path[L] == '/')
            drop = 1;
        if (drop) {
            xa_ent_t *d = *pp;
            *pp = d->next;
            xa_free_ent(d);
        } else {
            pp = &(*pp)->next;
        }
    }
    pthread_mutex_unlock(&g_xa_mu);
}

static int xa_get(const char *path, const char *name, char *value, size_t size)
{
    if (!name)
        return -EINVAL;
    pthread_mutex_lock(&g_xa_mu);
    xa_ent_t *found = NULL;
    for (xa_ent_t *e = g_xa; e; e = e->next) {
        if (strcmp(e->path, path) == 0 && strcmp(e->name, name) == 0) {
            found = e;
            break;
        }
    }
    if (!found) {
        pthread_mutex_unlock(&g_xa_mu);
#ifdef ENOATTR
        return -ENOATTR;
#else
        return -ENODATA;
#endif
    }
    if (size == 0) {
        size_t L = found->vlen;
        pthread_mutex_unlock(&g_xa_mu);
        return (int)L;
    }
    if (size < found->vlen) {
        pthread_mutex_unlock(&g_xa_mu);
        return -ERANGE;
    }
    memcpy(value, found->val, found->vlen);
    pthread_mutex_unlock(&g_xa_mu);
    return (int)found->vlen;
}

static int xa_set(const char *path, const char *name, const char *value,
                  size_t size, int flags)
{
    if (!path || !name || !*name)
        return -EINVAL;
    if (size > (size_t)XA_MAX_VALUE)
        return -E2BIG;
#if defined(__APPLE__)
    {
        int c = (flags & XATTR_CREATE) != 0;
        int r = (flags & XATTR_REPLACE) != 0;
        if (c && r)
            return -EINVAL;
    }
#endif
    pthread_mutex_lock(&g_xa_mu);
    xa_ent_t **slot = &g_xa;
    xa_ent_t  *found = NULL;
    while (*slot) {
        if (strcmp((*slot)->path, path) == 0 && strcmp((*slot)->name, name) == 0) {
            found = *slot;
            break;
        }
        slot = &(*slot)->next;
    }
#if defined(__APPLE__)
    if ((flags & XATTR_CREATE) && found) {
        pthread_mutex_unlock(&g_xa_mu);
        return -EEXIST;
    }
    if ((flags & XATTR_REPLACE) && !found) {
        pthread_mutex_unlock(&g_xa_mu);
#ifdef ENOATTR
        return -ENOATTR;
#else
        return -ENODATA;
#endif
    }
#endif
    unsigned char *nv = NULL;
    if (size > 0) {
        nv = malloc(size);
        if (!nv) {
            pthread_mutex_unlock(&g_xa_mu);
            return -ENOMEM;
        }
        memcpy(nv, value, size);
    }
    if (found) {
        free(found->val);
        found->val = nv;
        found->vlen = size;
        pthread_mutex_unlock(&g_xa_mu);
        return 0;
    }
    while (xa_count_unlocked() >= (size_t)XA_MAX_NODES)
        xa_evict_tail_unlocked();
    xa_ent_t *ne = calloc(1, sizeof(*ne));
    if (!ne) {
        free(nv);
        pthread_mutex_unlock(&g_xa_mu);
        return -ENOMEM;
    }
    ne->path = strdup(path);
    ne->name = strdup(name);
    ne->val  = nv;
    ne->vlen = size;
    if (!ne->path || !ne->name) {
        xa_free_ent(ne);
        pthread_mutex_unlock(&g_xa_mu);
        return -ENOMEM;
    }
    ne->next = g_xa;
    g_xa = ne;
    pthread_mutex_unlock(&g_xa_mu);
    return 0;
}

static int xa_list(const char *path, char *list, size_t size)
{
    pthread_mutex_lock(&g_xa_mu);
    size_t need = 0;
    for (xa_ent_t *e = g_xa; e; e = e->next) {
        if (strcmp(e->path, path) != 0)
            continue;
        need += strlen(e->name) + 1;
    }
    if (size == 0) {
        pthread_mutex_unlock(&g_xa_mu);
        return (int)need;
    }
    if (size < need) {
        pthread_mutex_unlock(&g_xa_mu);
        return -ERANGE;
    }
    char *p = list;
    for (xa_ent_t *e = g_xa; e; e = e->next) {
        if (strcmp(e->path, path) != 0)
            continue;
        size_t nl = strlen(e->name) + 1;
        memcpy(p, e->name, nl);
        p += nl;
    }
    pthread_mutex_unlock(&g_xa_mu);
    return (int)need;
}

static int xa_remove_one(const char *path, const char *name)
{
    pthread_mutex_lock(&g_xa_mu);
    xa_ent_t **pp = &g_xa;
    while (*pp) {
        if (strcmp((*pp)->path, path) == 0 && strcmp((*pp)->name, name) == 0) {
            xa_ent_t *d = *pp;
            *pp = d->next;
            xa_free_ent(d);
            pthread_mutex_unlock(&g_xa_mu);
            return 0;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_xa_mu);
#ifdef ENOATTR
    return -ENOATTR;
#else
    return -ENODATA;
#endif
}

static int mtp_rename_with_xattr(const char *from, const char *to)
{
    int rc = mtp_rename(from, to);
    if (rc == 0)
        xa_rename_paths(from, to);
    return rc;
}

static int op_getattr(const char *path, struct stat *st)
{
    static unsigned long getattr_seq;
    double t0 = fuse_mono_ms();
    mtp_stat_t s;
    int rc = mtp_stat(path, &s);
    mtp_debug_log("fuse getattr #%lu path=%s mtp_rc=%d %.2fms",
                  ++getattr_seq, path, rc, fuse_mono_ms() - t0);
    if (rc == 0) {
        mtp_fill_stat(&s, st);
        return 0;
    }
    if (rc == -ENOENT) {
        int pr = pending_getattr(path, st);
        if (pr == 0)
            return 0;
        if (pr < 0)
            return pr; /* fstat failure */
        /* pr == 1: not pending; must return real ENOENT, never -1 (ambiguous w/ EPERM). */
    }
    return rc;
}

static int op_fgetattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    (void)fi;
    return op_getattr(path, st);
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

    mtp_stat_t s_here;
    struct stat st_dot;
    if (mtp_stat(path, &s_here) != 0) {
        memset(&s_here, 0, sizeof(s_here));
        s_here.is_dir = 1;
    }
    mtp_fill_stat(&s_here, &st_dot);

    char parent[PATH_MAX];
    parent_path_for_readdir(path, parent, sizeof(parent));
    mtp_stat_t s_up;
    struct stat st_dotdot;
    if (parent[0] && mtp_stat(parent, &s_up) == 0)
        mtp_fill_stat(&s_up, &st_dotdot);
    else {
        memset(&st_dotdot, 0, sizeof(st_dotdot));
        st_dotdot.st_mode  = S_IFDIR | 0755;
        st_dotdot.st_nlink = 2;
        st_dotdot.st_uid   = getuid();
        st_dotdot.st_gid   = getgid();
        st_dotdot.st_ino   = 1;
    }

    int buf_full = 0;
    off_t cookie = 1;
    if (cookie > off) {
        if (filler(buf, ".", &st_dot, cookie)) {
            buf_full = 1;
            goto out;
        }
    }
    cookie++;

    if (cookie > off) {
        if (filler(buf, "..", &st_dotdot, cookie)) {
            buf_full = 1;
            goto out;
        }
    }
    cookie++;

    for (size_t i = 0; i < nent; i++) {
        struct stat st;
        mtp_stat_t se;
        memset(&se, 0, sizeof(se));
        se.is_dir     = entries[i].is_dir;
        se.size       = entries[i].size;
        se.mtime      = entries[i].mtime;
        se.object_id  = entries[i].object_id;
        se.storage_id = entries[i].storage_id;
        mtp_fill_stat(&se, &st);
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

/* RDWR large-file opens may read via GetPartialObject until the first write. */
static int ensure_staging_from_partial(const char *path, handle_t *h)
{
    if (!h->use_partial)
        return 0;
    pthread_mutex_lock(&g_stage_mu);
    if (!h->cache_ready) {
        int st = mtp_download_to_fd(path, h->fd);
        if (st != 0) {
            pthread_mutex_unlock(&g_stage_mu);
            return st;
        }
        h->cache_ready = 1;
    }
    h->use_partial = 0;
    pthread_mutex_unlock(&g_stage_mu);
    return 0;
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

    h->object_id = s.object_id;
    h->remote_size = s.size;
    h->use_partial = 0;
    int acc = fi->flags & O_ACCMODE;
    if (!(fi->flags & O_TRUNC) && s.object_id && s.size > MTP_OP_OPEN_PREFETCH_MAX &&
        (acc == O_RDONLY || acc == O_RDWR) && mtp_supports_partial_read())
        h->use_partial = 1;

    int did_prefetch = 0;
    h->cache_ready = 0;
    /* Pull device payload once into staging fd (not for giants). */
    if (!h->use_partial && (fi->flags & O_ACCMODE) != O_WRONLY && s.size > 0 &&
        s.size <= MTP_OP_OPEN_PREFETCH_MAX) {
        int st = mtp_download_to_fd(path, h->fd);
        if (st != 0) {
            close(h->fd); free(h->path); free(h); return st;
        }
        did_prefetch = 1;
        h->cache_ready = 1;
    }

    if (fi->flags & O_TRUNC) {
        ftruncate(h->fd, 0);
        h->dirty = 1;
        h->cache_ready = 1; /* local empty; do not re-download from device */
        h->use_partial = 0;
    }
    fi->fh = (uint64_t)(uintptr_t)h;
    mtp_debug_log("fuse open path=%s flags=0x%x size=%llu prefetch=%d partial=%d cache_ready=%d %.2fms",
                  path, fi->flags, (unsigned long long)s.size, did_prefetch,
                  h->use_partial, h->cache_ready, fuse_mono_ms() - t0);
    return 0;
}

static int op_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)mode;
    handle_t *h = make_handle(path);
    if (!h) return -ENOMEM;
    h->created = 1;
    h->cache_ready = 1; /* empty staging file; not on device until release */
    pending_add(path, h);
    fi->fh = (uint64_t)(uintptr_t)h;
    return 0;
}

static int op_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi)
{
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (!h) return -EBADF;
    if (h->use_partial && !h->created) {
        int pr = mtp_read_partial(h->object_id, h->remote_size, buf, size, offset);
        return pr;
    }
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
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (!h) return -EBADF;
    if (h->use_partial) {
        int st = ensure_staging_from_partial(path, h);
        if (st != 0)
            return st;
    }
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
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (!h) return -EBADF;
    if (h->use_partial) {
        int st = ensure_staging_from_partial(path, h);
        if (st != 0)
            return st;
    }
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
    (void)fi;
    /* Staging file is an unlinked temp; after Finder shows 100% it often
     * fsync(2)s the fd — real fsync can fail in ways that show as
     * “device disappeared” even though the read path succeeded. */
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
    /* Upload new or modified files. Created-but-empty must still be sent or
     * the object never appears on the device. */
    if (h->dirty || h->created) {
        off_t end = lseek(h->fd, 0, SEEK_END);
        if (end < 0)
            rc = -errno;
        else {
            int wr = mtp_write_full_fd(h->path, h->fd, (size_t)end);
            if (wr < 0) rc = wr;
        }
    }
    if (h->created)
        pending_remove(h);
    close(h->fd);
    mtp_debug_log("fuse release path=%s had_dirty=%d created=%d rc=%d %.2fms",
                  h->path, was_dirty, h->created, rc, fuse_mono_ms() - t0);
    free(h->path);
    free(h);
    return rc;
}

static int op_rename(const char *from, const char *to)
{
    return mtp_rename_with_xattr(from, to);
}

static int op_unlink(const char *path)
{
    int rc = mtp_unlink(path);
    if (rc == 0)
        xa_purge_path(path, 0);
    return rc;
}

static int op_mkdir(const char *path, mode_t m)
{
    (void)m;
    return mtp_mkdir(path);
}

static int op_rmdir(const char *path)
{
    int rc = mtp_rmdir(path);
    if (rc == 0)
        xa_purge_path(path, 1);
    return rc;
}

static int op_access(const char *path, int mask)
{
    /* Mounted with defer_permissions: the kernel checks access in the calling
     * context. Our attrs are synthetic (regular files are 0666 with no +x), so
     * enforcing mask here makes X_OK fail and Finder shows "no permission". */
    (void)path;
    (void)mask;
    return 0;
}

static int op_chmod (const char *p, mode_t m) { (void)p;(void)m; return 0; }
static int op_chown (const char *p, uid_t u, gid_t g) { (void)p;(void)u;(void)g; return 0; }
static int op_utimens(const char *p, const struct timespec t[2]) { (void)p;(void)t; return 0; }

static void mtp_fill_statvfs_from_bytes(uint64_t tot, uint64_t fr, struct statvfs *st)
{
    memset(st, 0, sizeof(*st));
    unsigned long bs = 4096UL;
    st->f_bsize = (unsigned long)bs;
    st->f_frsize = (unsigned long)bs;
    st->f_namemax = 255;
    if (tot == 0u) {
        st->f_blocks = (fsblkcnt_t)(1024UL * 1024UL * 64UL);
        st->f_bfree = st->f_bavail = (fsblkcnt_t)(1024UL * 1024UL * 32UL);
        return;
    }
    if (fr > tot)
        fr = tot;
    st->f_blocks = (fsblkcnt_t)(tot / bs);
    st->f_bfree = st->f_bavail = (fsblkcnt_t)(fr / bs);
}

#if defined(__APPLE__)
static unsigned int statfs_x_seq;

/* Finder copy/metadata uses setattr_x / fsetattr_x; missing handlers → EPERM. */
static int op_setattr_x(const char *path, struct setattr_x *attr)
{
    (void)path;
    (void)attr;
    return 0;
}

static int op_fsetattr_x(const char *path, struct setattr_x *attr,
                         struct fuse_file_info *fi)
{
    (void)path;
    (void)attr;
    (void)fi;
    return 0;
}

static int op_chflags(const char *path, uint32_t flags)
{
    (void)path;
    (void)flags;
    return 0;
}

static void op_monitor(const char *path, uint32_t ev)
{
    (void)path;
    (void)ev;
}

static int op_renamex(const char *from, const char *to, unsigned int flags)
{
    /* Finder uses renamex_np with RENAME_EXCL for atomic moves; rejecting all
     * non-zero flags surfaced as “no permission” in some macOS versions. */
    if (flags & (unsigned)RENAME_SWAP)
        return -EINVAL;
    if (flags & (unsigned)RENAME_NOFOLLOW_ANY)
        return -EINVAL;
    if (flags & (unsigned)RENAME_RESOLVE_BENEATH)
        return -EINVAL;
    if (flags & (unsigned)RENAME_EXCL) {
        mtp_stat_t sx;
        if (mtp_stat(to, &sx) == 0)
            return -EEXIST;
    }
    return mtp_rename_with_xattr(from, to);
}

static int op_statfs_x(const char *path, struct statfs *st)
{
    if ((++statfs_x_seq % 10U) == 0U)
        mtp_debug_log("fuse statfs_x #%u path=%s", statfs_x_seq, path);
    uint64_t tot = 0, fr = 0;
    int sr = mtp_storage_space_for_path(path, &tot, &fr);
    memset(st, 0, sizeof(*st));
    st->f_bsize = 4096;
    st->f_iosize = 1048576;
    if (sr != 0 || tot == 0u) {
        st->f_blocks = (int64_t)1024 * 1024 * 64;
        st->f_bfree = (int64_t)1024 * 1024 * 32;
        st->f_bavail = (int64_t)1024 * 1024 * 32;
    } else {
        uint64_t bs = 4096u;
        st->f_blocks = (int64_t)(tot / bs);
        st->f_bfree = (int64_t)(fr / bs);
        st->f_bavail = st->f_bfree;
    }
    st->f_files = 1000000;
    st->f_ffree = 500000;
    strncpy(st->f_fstypename, "mtp", sizeof(st->f_fstypename) - 1);
    st->f_fstypename[sizeof(st->f_fstypename) - 1] = '\0';
    return 0;
}

static int op_exchange(const char *p1, const char *p2, unsigned long opts)
{
    (void)p1;
    (void)p2;
    (void)opts;
    /* EXDEV steers Finder away from treating this like a local HFS exchange;
     * -ENOTSUP was sometimes mapped to a generic “permission” alert after copy. */
    return -EXDEV;
}

static int op_getxtimes(const char *path, struct timespec *bkuptime,
                        struct timespec *crtime)
{
    if (!bkuptime || !crtime)
        return -EINVAL;
    mtp_stat_t s;
    int rc = mtp_stat(path, &s);
    if (rc != 0) {
        struct stat st;
        int pr = pending_getattr(path, &st);
        if (pr != 0)
            return pr < 0 ? pr : rc;
        time_t mt = st.st_mtime;
        bkuptime->tv_sec = mt;
        bkuptime->tv_nsec = 0;
        crtime->tv_sec = mt;
        crtime->tv_nsec = 0;
        return 0;
    }
    time_t mt = (time_t)s.mtime;
    if (mt == 0 && s.is_dir)
        mt = time(NULL);
    bkuptime->tv_sec = mt;
    bkuptime->tv_nsec = 0;
    crtime->tv_sec = mt;
    crtime->tv_nsec = 0;
    return 0;
}

static int op_setbkuptime(const char *path, const struct timespec *tv)
{
    (void)path;
    (void)tv;
    return 0;
}

static int op_setchgtime(const char *path, const struct timespec *tv)
{
    (void)path;
    (void)tv;
    return 0;
}

static int op_setcrtime(const char *path, const struct timespec *tv)
{
    (void)path;
    (void)tv;
    return 0;
}
#endif

static int op_statfs(const char *path, struct statvfs *st)
{
    static unsigned long statfs_seq;
    if ((++statfs_seq % 10UL) == 0UL)
        mtp_debug_log("fuse statfs #%lu path=%s", statfs_seq, path);
    uint64_t tot = 0, fr = 0;
    int sr = mtp_storage_space_for_path(path, &tot, &fr);
    if (sr != 0 || tot == 0u) {
        memset(st, 0, sizeof(*st));
        st->f_bsize = 4096;
        st->f_frsize = 4096;
        st->f_blocks = 1024UL * 1024UL * 64UL;
        st->f_bfree = 1024UL * 1024UL * 32UL;
        st->f_bavail = 1024UL * 1024UL * 32UL;
        st->f_namemax = 255;
        return 0;
    }
    mtp_fill_statvfs_from_bytes(tot, fr, st);
    return 0;
}

#ifdef __APPLE__
static int op_getxattr(const char *path, const char *name, char *value, size_t size,
                       uint32_t position)
{
    (void)position;
    return xa_get(path, name, value, size);
}
#else
static int op_getxattr(const char *path, const char *name, char *value, size_t size)
{
    static unsigned long xattr_seq;
    mtp_debug_log("fuse getxattr #%lu path=%s name=%s", ++xattr_seq, path, name ? name : "?");
    return xa_get(path, name, value, size);
}
#endif

static int op_listxattr(const char *path, char *list, size_t size)
{
    return xa_list(path, list, size);
}

#ifdef __APPLE__
static int op_setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags, uint32_t position)
{
    (void)position;
    return xa_set(path, name, value, size, flags);
}
#else
static int op_setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags)
{
    return xa_set(path, name, value, size, flags);
}
#endif

static int op_removexattr(const char *path, const char *name)
{
    return xa_remove_one(path, name);
}

static int op_fallocate(const char *path, int mode, off_t offset, off_t length,
                        struct fuse_file_info *fi)
{
    (void)path;
    (void)mode;
    (void)offset;
    (void)length;
    (void)fi;
    return 0;
}

/* macFUSE fuse_conn_info has max_write / max_readahead (no max_read field). */
static void *op_init(struct fuse_conn_info *conn)
{
#if defined(__APPLE__)
    if (conn->max_write < (unsigned)1048576)
        conn->max_write = (unsigned)1048576;
    if (conn->max_readahead < (unsigned)2097152)
        conn->max_readahead = (unsigned)2097152;
#endif
    return NULL;
}

struct fuse_operations mtpfuse_ops = {
    .init        = op_init,
    .getattr     = op_getattr,
    .fgetattr    = op_fgetattr,
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
    .access      = op_access,
    .statfs      = op_statfs,
    .fallocate   = op_fallocate,
    .getxattr    = op_getxattr,
    .listxattr   = op_listxattr,
    .setxattr    = op_setxattr,
    .removexattr = op_removexattr,
#if defined(__APPLE__)
    .monitor     = op_monitor,
    .renamex     = op_renamex,
    .statfs_x    = op_statfs_x,
    .exchange    = op_exchange,
    .getxtimes   = op_getxtimes,
    .setbkuptime = op_setbkuptime,
    .setchgtime  = op_setchgtime,
    .setcrtime   = op_setcrtime,
    .setattr_x   = op_setattr_x,
    .fsetattr_x  = op_fsetattr_x,
    .chflags     = op_chflags,
#endif
};
