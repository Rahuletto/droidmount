#define _DARWIN_C_SOURCE 1
#include "fs_ops.h"
#include "mtp_log.h"
#include "mtp_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

#if defined(__APPLE__)
/* See file comment: libmtp’s LIBMTP_Read_Event blocks on USB and races the stack. */
static struct fuse           *g_mtpfuse_handle;
static volatile int           g_mtpfuse_remote_inval_stop;

typedef struct {
    struct fuse *f;
} mtp_inval_vol_ctx_t;

static void mtp_inval_one_volume_path(const char *path, void *ctx)
{
    mtp_inval_vol_ctx_t *c = (mtp_inval_vol_ctx_t *)ctx;
    /* Phone-side changes: no reliable MTP push; we poll. Default: FUSE
     * fuse_invalidate_path on each storage root every MTP_DEVICE_SYNC_INTERVAL_SEC (60s).
     * Set MTP_AGGRESSIVE_TREE_INVALIDATE=1 to also drop the in-memory dir cache (stronger
     * sync, more USB/CPU; competes with Finder on deep folders). */
    const char *ag = getenv("MTP_AGGRESSIVE_TREE_INVALIDATE");
    if (ag && ag[0] && strcmp(ag, "0") != 0)
        mtp_invalidate_fuse_dir_cache(path);
    int ir = fuse_invalidate_path(c->f, path);
    if (ir < 0 && ir != -ENOENT)
        mtp_log("fuse_invalidate_path(%s) rc=%d", path, ir);
}

/* Sleep granularity 100ms; return ~iterations = seconds * 10. Default 60s. */
static int mtp_remote_inval_wait_iters(void)
{
    const char *e = getenv("MTP_DEVICE_SYNC_INTERVAL_SEC");
    if (e && e[0]) {
        char *end = NULL;
        double s = strtod(e, &end);
        if (end != e && s >= 3.0 && s <= 600.0)
            return (int)(s * 10.0 + 0.5);
    }
    return 600;
}

static void *mtp_remote_inval_loop(void *arg)
{
    (void)arg;
    while (!g_mtpfuse_remote_inval_stop) {
        int w = mtp_remote_inval_wait_iters();
        for (int n = 0; n < w && !g_mtpfuse_remote_inval_stop; n++)
            usleep(100000);
        struct fuse *f = g_mtpfuse_handle;
        if (!f || g_mtpfuse_remote_inval_stop)
            continue;
        mtp_inval_vol_ctx_t ic = { f };
        mtp_for_each_volume_directory_path(mtp_inval_one_volume_path, &ic);
    }
    return NULL;
}
#endif

/* Keep open-path cheap: avoid any full-object pull in op_open.
 * Reads can still hydrate lazily on demand (or via partial reads). */
#define MTP_OP_OPEN_PREFETCH_MAX ((uint64_t)0)
#define MTP_OP_OPEN_MEDIA_PREFETCH_MAX ((uint64_t)0)
/* Match default mount iosize (2MiB); st_blksize=0 confuses some media stacks (QuickTime). */
#define MTP_ST_BLKSIZE (2097152)

/* Serializes lazy mtp_download_to_fd for one handle; never nest with g_lock. */
static pthread_mutex_t g_stage_mu = PTHREAD_MUTEX_INITIALIZER;

static int path_use_full_mtp_fetch(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base)
        return 0;
    dot++;
    static const char *const exts[] = {
        "mp4", "mov", "m4v", "m4a", "mkv", "avi", "3gp", "webm",
        "heic", "jpeg", "jpg", "png", NULL,
    };
    for (int i = 0; exts[i]; i++) {
        if (strcasecmp(dot, exts[i]) == 0)
            return 1;
    }
    return 0;
}

/* Container formats where GetPartialObject during Finder *duplicate* can yield bytes
 * QuickTime rejects; full Get_File to temp (use_partial=0) matches move/rename (no re-read). */
static int path_mtp_video_container(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base)
        return 0;
    dot++;
    static const char *const exts[] = {
        "mp4", "mov", "m4v", "m4a", "mkv", "avi", "3gp", "webm", NULL,
    };
    for (int i = 0; exts[i]; i++) {
        if (strcasecmp(dot, exts[i]) == 0)
            return 1;
    }
    return 0;
}

static double fuse_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

typedef struct {
    const char *op;
    const char *a;
    const char *b;
    double        t0;
} fuse_trc_t;

#define FUSE_T0(s, o, p)  fuse_trc_t s = { (o), (p), NULL, fuse_mono_ms() }
#define FUSE_T0_2(s, o, p, q) fuse_trc_t s = { (o), (p), (q), fuse_mono_ms() }

static int fuse_trc_r(fuse_trc_t *z, int rc)
{
    if (mtp_log_is_enabled()) {
        double dt = fuse_mono_ms() - z->t0;
        if (z->b && z->b[0])
            mtp_log("fuse op=%s path1=%.400s path2=%.400s rc=%d dt=%.2fms", z->op, z->a, z->b, rc, dt);
        else
            mtp_log("fuse op=%.48s path=%.400s rc=%d dt=%.2fms", z->op, z->a, rc, dt);
    }
    return rc;
}

static int fuse_trc_rw(const char *op, const char *path, size_t sz, off_t off, int ret, double t0)
{
    if (mtp_log_is_enabled()) {
        double dt = fuse_mono_ms() - t0;
        mtp_log("fuse op=%.24s path=%.400s req=%zu off=%lld ret=%d dt=%.2fms", op, path, sz, (long long)off, ret, dt);
    }
    return ret;
}

/* Loop pwrite until full buffer — avoids silent truncation if a single pwrite short‑returns. */
static int pwrite_full(int fd, const char *buf, size_t size, off_t offset)
{
    size_t done = 0;
    while (done < size) {
        ssize_t w = pwrite(fd, buf + done, size - done, offset + (off_t)done);
        if (w < 0)
            return -errno;
        if (w == 0)
            return -EIO;
        done += (size_t)w;
    }
    return (int)size;
}

/* Loop pread until buffer full or EOF (returns byte count, possibly < size). */
static int pread_loop(int fd, char *buf, size_t size, off_t offset)
{
    size_t done = 0;
    while (done < size) {
        ssize_t r = pread(fd, buf + done, size - done, offset + (off_t)done);
        if (r < 0)
            return -errno;
        if (r == 0)
            break;
        done += (size_t)r;
    }
    return (int)done;
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
        st->st_blksize = 4096;
    } else {
        st->st_mode  = S_IFREG | 0666;
        st->st_nlink = 1;
        st->st_size  = (off_t)s->size;
        st->st_blksize = MTP_ST_BLKSIZE;
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

/* 0 = ok, -1 = out of memory (caller must destroy handle). */
static int pending_add(const char *path, handle_t *h)
{
    if (!path || !h)
        return -1;
    pending_create_t *e = malloc(sizeof(*e));
    if (!e) return -1;
    e->path = strdup(path);
    if (!e->path) { free(e); return -1; }
    e->h = h;
    pthread_mutex_lock(&g_pending_mu);
    e->next = g_pending_creates;
    g_pending_creates = e;
    pthread_mutex_unlock(&g_pending_mu);
    return 0;
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
        if (!ph || !ph->created) continue;
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
        st->st_blksize = MTP_ST_BLKSIZE;
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
    uint32_t       path_hash;  /* hash of path for faster comparison */
} xa_ent_t;

/* Hash table with 64 buckets (power of 2 for fast modulo) */
#define XA_BUCKET_COUNT 64
static xa_ent_t       *g_xa_buckets[XA_BUCKET_COUNT];
static pthread_mutex_t g_xa_mu = PTHREAD_MUTEX_INITIALIZER;

enum { XA_MAX_NODES = 450, XA_MAX_VALUE = 65536 };

/* Simple hash function for (path,name) combination */
static uint32_t xa_hash(const char *path, const char *name)
{
    uint32_t h = 5381;
    int c;
    /* Hash path */
    while ((c = (unsigned char)*path++))
        h = ((h << 5) + h) + c;
    /* Hash name */
    while ((c = (unsigned char)*name++))
        h = ((h << 5) + h) + c;
    return h;
}

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
    for (int i = 0; i < XA_BUCKET_COUNT; i++) {
        for (xa_ent_t *e = g_xa_buckets[i]; e; e = e->next)
            n++;
    }
    return n;
}

static void xa_evict_tail_unlocked(void)
{
    /* Find the last entry across all buckets */
    xa_ent_t *last = NULL;
    xa_ent_t *last_prev = NULL;
    int last_bucket = -1;
    
    for (int i = 0; i < XA_BUCKET_COUNT; i++) {
        xa_ent_t **pp = &g_xa_buckets[i];
        while (*pp) {
            last_prev = *pp;
            pp = &(*pp)->next;
        }
        if (last_prev) {
            last = last_prev;
            last_bucket = i;
        }
    }
    
    if (last) {
        xa_ent_t **pp = &g_xa_buckets[last_bucket];
        while (*pp && *pp != last)
            pp = &(*pp)->next;
        if (*pp) {
            *pp = last->next;
            xa_free_ent(last);
        }
    }
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
    for (int i = 0; i < XA_BUCKET_COUNT; i++) {
        xa_ent_t **pp = &g_xa_buckets[i];
        while (*pp) {
            xa_ent_t *e = *pp;
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
                /* Rehash since path changed */
                e->path_hash = xa_hash(e->path, e->name);
            } else {
                pp = &(*pp)->next;
            }
        }
    }
    pthread_mutex_unlock(&g_xa_mu);
}

static void xa_purge_path(const char *path, int is_dir)
{
    size_t L = strlen(path);
    pthread_mutex_lock(&g_xa_mu);
    for (int i = 0; i < XA_BUCKET_COUNT; i++) {
        xa_ent_t **pp = &g_xa_buckets[i];
        while (*pp) {
            xa_ent_t *e = *pp;
            int drop = 0;
            if (strcmp(e->path, path) == 0)
                drop = 1;
            else if (is_dir && L > 0 && strncmp(e->path, path, L) == 0 &&
                     e->path[L] == '/')
                drop = 1;
            if (drop) {
                *pp = e->next;
                xa_free_ent(e);
            } else {
                pp = &(*pp)->next;
            }
        }
    }
    pthread_mutex_unlock(&g_xa_mu);
}

/* Darwin xattrs may be read/written with a byte [position] (resource-fork style). */
static int xa_get(const char *path, const char *name, char *value, size_t size,
                  uint32_t position)
{
    if (!name)
        return -EINVAL;
    pthread_mutex_lock(&g_xa_mu);
    uint32_t h = xa_hash(path, name);
    xa_ent_t *found = NULL;
    for (xa_ent_t *e = g_xa_buckets[h & (XA_BUCKET_COUNT - 1)]; e; e = e->next) {
        if (e->path_hash == h && strcmp(e->path, path) == 0 && strcmp(e->name, name) == 0) {
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
    size_t tail = 0;
    if (found->vlen > (size_t)position)
        tail = found->vlen - (size_t)position;
    if (size == 0) {
        pthread_mutex_unlock(&g_xa_mu);
        return (int)tail;
    }
    if (size < tail) {
        pthread_mutex_unlock(&g_xa_mu);
        return -ERANGE;
    }
    if (tail && value && found->val)
        memcpy(value, found->val + (size_t)position, tail);
    pthread_mutex_unlock(&g_xa_mu);
    return (int)tail;
}

static int xa_set(const char *path, const char *name, const char *value,
                  size_t size, int flags, uint32_t position)
{
    if (!path || !name || !*name)
        return -EINVAL;
    if ((uint64_t)position + (uint64_t)size > (uint64_t)XA_MAX_VALUE)
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
    uint32_t h = xa_hash(path, name);
    xa_ent_t **slot = &g_xa_buckets[h & (XA_BUCKET_COUNT - 1)];
    xa_ent_t  *found = NULL;
    while (*slot) {
        xa_ent_t *e = *slot;
        if (e->path_hash == h && strcmp(e->path, path) == 0 && strcmp(e->name, name) == 0) {
            found = e;
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
    const size_t need = (size_t)position + size;

    if (found) {
        size_t newlen = found->vlen > need ? found->vlen : need;
        if (newlen > (size_t)XA_MAX_VALUE) {
            pthread_mutex_unlock(&g_xa_mu);
            return -E2BIG;
        }
        unsigned char *nb = found->val;
        if (newlen == 0) {
            free(found->val);
            found->val = NULL;
            found->vlen = 0;
        } else {
            nb = realloc(found->val, newlen);
            if (!nb) {
                pthread_mutex_unlock(&g_xa_mu);
                return -ENOMEM;
            }
            if (newlen > found->vlen)
                memset(nb + found->vlen, 0, newlen - found->vlen);
            if (size && value)
                memcpy(nb + (size_t)position, value, size);
            found->val = nb;
            found->vlen = newlen;
        }
        pthread_mutex_unlock(&g_xa_mu);
        return 0;
    }

    while (xa_count_unlocked() >= (size_t)XA_MAX_NODES)
        xa_evict_tail_unlocked();
    xa_ent_t *ne = calloc(1, sizeof(*ne));
    if (!ne) {
        pthread_mutex_unlock(&g_xa_mu);
        return -ENOMEM;
    }
    ne->path = strdup(path);
    ne->name = strdup(name);
    if (!ne->path || !ne->name) {
        xa_free_ent(ne);
        pthread_mutex_unlock(&g_xa_mu);
        return -ENOMEM;
    }
    ne->path_hash = xa_hash(path, name);
    if (need > 0) {
        ne->val = calloc(1, need);
        if (!ne->val) {
            xa_free_ent(ne);
            pthread_mutex_unlock(&g_xa_mu);
            return -ENOMEM;
        }
        if (size && value)
            memcpy(ne->val + (size_t)position, value, size);
        ne->vlen = need;
    }
    /* Add to hash bucket */
    uint32_t bucket = ne->path_hash & (XA_BUCKET_COUNT - 1);
    ne->next = g_xa_buckets[bucket];
    g_xa_buckets[bucket] = ne;
    pthread_mutex_unlock(&g_xa_mu);
    return 0;
}

static int xa_list(const char *path, char *list, size_t size)
{
    pthread_mutex_lock(&g_xa_mu);
    size_t need = 0;
    for (int i = 0; i < XA_BUCKET_COUNT; i++) {
        for (xa_ent_t *e = g_xa_buckets[i]; e; e = e->next) {
            if (strcmp(e->path, path) != 0)
                continue;
            need += strlen(e->name) + 1;
        }
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
    for (int i = 0; i < XA_BUCKET_COUNT; i++) {
        for (xa_ent_t *e = g_xa_buckets[i]; e; e = e->next) {
            if (strcmp(e->path, path) != 0)
                continue;
            size_t nl = strlen(e->name) + 1;
            memcpy(p, e->name, nl);
            p += nl;
        }
    }
    pthread_mutex_unlock(&g_xa_mu);
    return (int)need;
}

static int xa_has_xattr(const char *path, const char *name)
{
    if (!path || !name)
        return 0;
    pthread_mutex_lock(&g_xa_mu);
    uint32_t h = xa_hash(path, name);
    for (xa_ent_t *e = g_xa_buckets[h & (XA_BUCKET_COUNT - 1)]; e; e = e->next) {
        if (e->path_hash == h && strcmp(e->path, path) == 0 && strcmp(e->name, name) == 0) {
            pthread_mutex_unlock(&g_xa_mu);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_xa_mu);
    return 0;
}

static int xa_remove_one(const char *path, const char *name)
{
    pthread_mutex_lock(&g_xa_mu);
    uint32_t h = xa_hash(path, name);
    xa_ent_t **pp = &g_xa_buckets[h & (XA_BUCKET_COUNT - 1)];
    while (*pp) {
        xa_ent_t *e = *pp;
        if (e->path_hash == h && strcmp(e->path, path) == 0 && strcmp(e->name, name) == 0) {
            *pp = e->next;
            xa_free_ent(e);
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

static int mtp_fuse_getattr_core(const char *path, struct stat *st)
{
    mtp_stat_t s;
    int rc = mtp_stat(path, &s);
    if (rc == 0) {
        mtp_fill_stat(&s, st);
        return 0;
    }
    if (rc == -ENOENT) {
        int pr = pending_getattr(path, st);
        if (pr == 0)
            return 0;
        if (pr < 0)
            return pr;
    }
    return rc;
}

static int op_getattr(const char *path, struct stat *st)
{
    FUSE_T0(tr, "getattr", path);
    return fuse_trc_r(&tr, mtp_fuse_getattr_core(path, st));
}

static int op_fgetattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    FUSE_T0(tr, "fgetattr", path);
    int rc = mtp_fuse_getattr_core(path, st);
    if (rc != 0)
        return fuse_trc_r(&tr, rc);
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (!h || !h->path || strcmp(h->path, path) != 0)
        return fuse_trc_r(&tr, 0);
    if (!S_ISREG(st->st_mode))
        return fuse_trc_r(&tr, 0);
    if (h->dirty || h->created) {
        struct stat fst;
        if (fstat(h->fd, &fst) == 0)
            st->st_size = fst.st_size;
        return fuse_trc_r(&tr, 0);
    }
    if (st->st_size >= 0 && (uint64_t)st->st_size <= (uint64_t)OFF_MAX)
        h->remote_size = (uint64_t)st->st_size;
    return fuse_trc_r(&tr, 0);
}

/* libfuse: filler's last arg is the dirent offset for seekdir; must be
 * unique and monotonic. Incoming `off` is the continuation cookie. */
static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t off, struct fuse_file_info *fi)
{
    (void)fi;
    FUSE_T0(tr, "readdir", path);
    double t0 = tr.t0;

    mtp_dirent_t *entries = NULL;
    size_t nent = 0;
    int rc = mtp_readdir_snapshot(path, &entries, &nent);
    if (rc != 0)
        return fuse_trc_r(&tr, rc);

    int xfer_busy = mtp_long_transfer_active();
    mtp_stat_t s_here;
    struct stat st_dot;
    if (xfer_busy || mtp_stat(path, &s_here) != 0) {
        memset(&s_here, 0, sizeof(s_here));
        s_here.is_dir = 1;
    }
    mtp_fill_stat(&s_here, &st_dot);

    char parent[PATH_MAX];
    parent_path_for_readdir(path, parent, sizeof(parent));
    mtp_stat_t s_up;
    struct stat st_dotdot;
    if (!xfer_busy && parent[0] && mtp_stat(parent, &s_up) == 0)
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
        se.is_shell   = entries[i].is_shell;
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
    if (mtp_log_is_enabled())
        mtp_log("fuse op=readdir path=%.400s off=%lld nent=%zu buf_full=%d rc=0 dt=%.2fms", path, (long long)off, nent,
                buf_full, fuse_mono_ms() - t0);
    return 0;
}

static handle_t *make_handle(const char *path)
{
    if (!path)
        return NULL;
    handle_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->path = strdup(path);
    if (!h->path) {
        free(h);
        return NULL;
    }
    h->fd = mtp_anon_tempfile_fd("mtpfuse_h");
    if (h->fd < 0) {
        free(h->path);
        free(h);
        return NULL;
    }
    return h;
}

/* RDWR large-file opens may read via GetPartialObject until the first write. */
static int ensure_staging_from_partial(const char *path, handle_t *h)
{
    if (!h->use_partial)
        return 0;
    pthread_mutex_lock(&g_stage_mu);
    if (!h->cache_ready) {
        int st = mtp_download_to_fd(h->path, h->fd);
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
    FUSE_T0(tr, "open", path);
    mtp_stat_t s;
    int rc = mtp_stat(path, &s);
    if (rc != 0) {
        int acc = fi->flags & O_ACCMODE;
        if (rc == -ENOENT && acc != O_RDONLY) {
            handle_t *h = make_handle(path);
            if (!h)
                return fuse_trc_r(&tr, -ENOMEM);
            h->created = 1;
            h->cache_ready = 1;
            if (pending_add(path, h) != 0) {
                close(h->fd);
                free(h->path);
                free(h);
                return fuse_trc_r(&tr, -ENOMEM);
            }
            fi->fh = (uint64_t)(uintptr_t)h;
            if (mtp_log_is_enabled())
                mtp_log("fuse op=open-impl create path=%.400s flags=0x%x", path, (unsigned)fi->flags);
            return fuse_trc_r(&tr, 0);
        }
        return fuse_trc_r(&tr, rc);
    }
    if (s.is_dir)
        return fuse_trc_r(&tr, -EISDIR);

    if (s.is_shell) {
        handle_t *h = make_handle(path);
        if (!h)
            return fuse_trc_r(&tr, -ENOMEM);
        h->object_id = 0;
        h->remote_size = 0;
        h->use_partial = 0;
        h->cache_ready = 1;
        if (ftruncate(h->fd, 0) < 0) {
            int e = errno;
            close(h->fd);
            free(h->path);
            free(h);
            return fuse_trc_r(&tr, -e);
        }
        if (fi->flags & O_TRUNC)
            h->dirty = 1;
        fi->fh = (uint64_t)(uintptr_t)h;
        if (mtp_log_is_enabled())
            mtp_log("fuse op=open-shell path=%.400s flags=0x%x", path, (unsigned)fi->flags);
        return fuse_trc_r(&tr, 0);
    }

    handle_t *h = make_handle(path);
    if (!h)
        return fuse_trc_r(&tr, -ENOMEM);

    h->object_id = s.object_id;
    h->remote_size = s.size;
    h->use_partial = 0;
    h->cache_ready = 0;

    if (fi->flags & O_TRUNC) {
        ftruncate(h->fd, 0);
        h->dirty = 1;
        h->cache_ready = 1;
        h->use_partial = 0;
    } else {
        const char *sr = getenv("MTP_STREAM_READ");
        int stream     = !sr || sr[0] == '\0' || strcmp(sr, "1") == 0;
        const char *sv = getenv("MTP_STREAM_VIDEO");
        int vpart      = (sv && strcmp(sv, "1") == 0);
        if (stream && mtp_supports_partial_read()) {
            if (!path_mtp_video_container(path) || vpart)
                h->use_partial = 1;
        }
    }
    fi->fh = (uint64_t)(uintptr_t)h;
    if (mtp_log_is_enabled()) {
        const char *sv = getenv("MTP_STREAM_VIDEO");
        mtp_log("fuse op=open reg path=%.400s flags=0x%x size=%llu use_partial=%d video=%d mtp_stream_video=%s use_full_mtp=%d prefetch_max=%llu media_max=%llu",
                path, (unsigned)fi->flags, (unsigned long long)s.size, h->use_partial,
                path_mtp_video_container(path), (sv && sv[0]) ? sv : "default",
                path_use_full_mtp_fetch(path), (unsigned long long)MTP_OP_OPEN_PREFETCH_MAX,
                (unsigned long long)MTP_OP_OPEN_MEDIA_PREFETCH_MAX);
    }
    return fuse_trc_r(&tr, 0);
}

static int op_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)mode;
    FUSE_T0(tr, "create", path);
    handle_t *h = make_handle(path);
    if (!h)
        return fuse_trc_r(&tr, -ENOMEM);
    h->created = 1;
    h->cache_ready = 1;
    if (pending_add(path, h) != 0) {
        close(h->fd);
        free(h->path);
        free(h);
        return fuse_trc_r(&tr, -ENOMEM);
    }
    fi->fh = (uint64_t)(uintptr_t)h;
    return fuse_trc_r(&tr, 0);
}

static int op_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi)
{
    double t0 = fuse_mono_ms();
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (!h)
        return fuse_trc_rw("read", path, size, offset, -EBADF, t0);
    if (!h->path)
        return fuse_trc_rw("read", path, size, offset, -EIO, t0);
    if (h->use_partial && !h->created) {
        int pr = mtp_read(h->path, buf, size, offset);
        return fuse_trc_rw("read", path, size, offset, pr, t0);
    }
    if (!h->cache_ready) {
        if (h->created) {
            h->cache_ready = 1;
        } else {
            pthread_mutex_lock(&g_stage_mu);
            if (!h->cache_ready) {
                int st = mtp_download_to_fd(h->path, h->fd);
                if (st != 0) {
                    pthread_mutex_unlock(&g_stage_mu);
                    return fuse_trc_rw("read", path, size, offset, st, t0);
                }
                h->cache_ready = 1;
            }
            pthread_mutex_unlock(&g_stage_mu);
        }
    }
    int n = pread_loop(h->fd, buf, size, offset);
    return fuse_trc_rw("read", path, size, offset, n, t0);
}

static int op_write(const char *path, const char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
    double t0 = fuse_mono_ms();
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (!h)
        return fuse_trc_rw("write", path, size, offset, -EBADF, t0);
    if (!h->path)
        return fuse_trc_rw("write", path, size, offset, -EIO, t0);
    if (h->use_partial) {
        int st = ensure_staging_from_partial(path, h);
        if (st != 0)
            return fuse_trc_rw("write", path, size, offset, st, t0);
    }
    int w = pwrite_full(h->fd, buf, size, offset);
    if (w < 0)
        return fuse_trc_rw("write", path, size, offset, w, t0);
    h->dirty = 1;
    return fuse_trc_rw("write", path, size, offset, w, t0);
}

static int op_truncate(const char *path, off_t size)
{
    FUSE_T0(tr, "truncate", path);
    if (size < 0)
        return fuse_trc_r(&tr, -EINVAL);
    /* Finder sometimes truncate(2)s the path during create/copy before our fd is visible
     * as ftruncate; pending creates must resize staging or the object ships wrong length. */
    pthread_mutex_lock(&g_pending_mu);
    for (pending_create_t *e = g_pending_creates; e; e = e->next) {
        if (strcmp(e->path, path) != 0)
            continue;
        handle_t *ph = e->h;
        if (!ph || ph->fd < 0)
            continue;
        pthread_mutex_unlock(&g_pending_mu);
        if (ftruncate(ph->fd, size) < 0)
            return fuse_trc_r(&tr, -errno);
        ph->dirty = 1;
        return fuse_trc_r(&tr, 0);
    }
    pthread_mutex_unlock(&g_pending_mu);
    (void)path;
    return fuse_trc_r(&tr, 0);
}

static int op_ftruncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    FUSE_T0(tr, "ftruncate", path);
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (!h)
        return fuse_trc_r(&tr, -EBADF);
    if (!h->path)
        return fuse_trc_r(&tr, -EIO);
    if (h->use_partial) {
        int st = ensure_staging_from_partial(path, h);
        if (st != 0)
            return fuse_trc_r(&tr, st);
    }
    if (ftruncate(h->fd, size) < 0)
        return fuse_trc_r(&tr, -errno);
    h->dirty = 1;
    return fuse_trc_r(&tr, 0);
}

static int op_flush(const char *path, struct fuse_file_info *fi)
{
    FUSE_T0(tr, "flush", path);
    (void)path;
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (!h || !h->path)
        return fuse_trc_r(&tr, 0);
    if (h->dirty || h->created) {
        if (fsync(h->fd) != 0)
            return fuse_trc_r(&tr, -errno);
    }
    return fuse_trc_r(&tr, 0);
}

static int op_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)datasync;
    (void)fi;
    FUSE_T0(tr, "fsync", path);
    return fuse_trc_r(&tr, 0);
}

static int op_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (!h) return 0;
    if (!h->path) {
        close(h->fd);
        free(h);
        return -EIO;
    }
    double t0 = fuse_mono_ms();
    int was_dirty = h->dirty;
    int rc = 0;
    int skip_empty_create_upload = 0;
    /* Finder can issue placeholder create+close on duplicate/move flows before
     * the real writer handle sends bytes; uploading that zero-length placeholder
     * corrupts the destination lifecycle. Keep untouched zero-byte creates local. */
    if (h->created && !h->dirty) {
        struct stat fst0;
        if (fstat(h->fd, &fst0) == 0 && fst0.st_size == 0)
            skip_empty_create_upload = 1;
    }
    /* Upload new or modified files unless this is an untouched empty placeholder create. */
    if ((h->dirty || h->created) && !skip_empty_create_upload) {
        /* Flush host cache before measuring size / streaming to USB (multi‑GB safe). */
        if (fsync(h->fd) != 0) {
            rc = -errno;
        }
        struct stat fst;
        if (rc == 0 && fstat(h->fd, &fst) < 0) {
            rc = -errno;
        } else if (rc == 0 && fst.st_size < 0) {
            rc = -EIO;
        } else if (rc == 0) {
            /* fstat st_size is authoritative (handles sparse/hole edge cases vs SEEK_END). */
            uint64_t sz64 = (uint64_t)fst.st_size;
            if (sz64 > (uint64_t)SIZE_MAX) {
                rc = -EFBIG;
            } else if (lseek(h->fd, 0, SEEK_SET) < 0) {
                rc = -errno;
            } else {
                int wr = mtp_write_full_fd(h->path, h->fd, (size_t)sz64);
                if (wr < 0)
                    rc = wr;
            }
        }
    }
    if (h->created && skip_empty_create_upload) {
        if (mtp_shell_file_register(h->path) != 0)
            mtp_log("mtp_shell_file_register failed path=%s", h->path);
    }
    if (h->created)
        pending_remove(h);
    close(h->fd);
    if (mtp_log_is_enabled())
        mtp_log("fuse op=release path=%.400s created=%d dirty=%d skip_empty=%d mtp_rc=%d dt=%.2fms", h->path, h->created, was_dirty,
                skip_empty_create_upload, rc, fuse_mono_ms() - t0);
    free(h->path);
    free(h);
    return rc;
}

static int op_rename(const char *from, const char *to)
{
    FUSE_T0_2(tr, "rename", from, to);
    if (mtp_long_transfer_active())
        return fuse_trc_r(&tr, -EBUSY);
    return fuse_trc_r(&tr, mtp_rename_with_xattr(from, to));
}

static int op_unlink(const char *path)
{
    FUSE_T0(tr, "unlink", path);
    if (mtp_long_transfer_active())
        return fuse_trc_r(&tr, -EBUSY);
    int rc = mtp_unlink(path);
    if (rc == 0)
        xa_purge_path(path, 0);
    return fuse_trc_r(&tr, rc);
}

static int op_mkdir(const char *path, mode_t m)
{
    (void)m;
    FUSE_T0(tr, "mkdir", path);
    if (mtp_long_transfer_active())
        return fuse_trc_r(&tr, -EBUSY);
    return fuse_trc_r(&tr, mtp_mkdir(path));
}

static int op_rmdir(const char *path)
{
    FUSE_T0(tr, "rmdir", path);
    if (mtp_long_transfer_active())
        return fuse_trc_r(&tr, -EBUSY);
    int rc = mtp_rmdir(path);
    if (rc == 0)
        xa_purge_path(path, 1);
    return fuse_trc_r(&tr, rc);
}

static int op_access(const char *path, int mask)
{
    (void)mask;
    FUSE_T0(tr, "access", path);
    return fuse_trc_r(&tr, 0);
}

static int op_chmod(const char *p, mode_t m)
{
    (void)m;
    FUSE_T0(tr, "chmod", p);
    return fuse_trc_r(&tr, 0);
}
static int op_chown(const char *p, uid_t u, gid_t g)
{
    (void)u;
    (void)g;
    FUSE_T0(tr, "chown", p);
    return fuse_trc_r(&tr, 0);
}
static int op_utimens(const char *p, const struct timespec t[2])
{
    (void)t;
    FUSE_T0(tr, "utimens", p);
    return fuse_trc_r(&tr, 0);
}

static void mtp_bytes_to_blk(uint64_t tot, uint64_t fr, uint64_t *bs_out, uint64_t *blk_out,
                             uint64_t *bfree_out)
{
    uint64_t bs = 4096;
    if (tot == 0u) {
        *bs_out = bs;
        *blk_out = 0;
        *bfree_out = 0;
        return;
    }
    if (fr > tot)
        fr = tot;
    while (tot / bs > (uint64_t)UINT32_MAX && bs < (1ULL << 40))
        bs *= 2u;
    *bs_out = bs;
    *blk_out = tot / bs;
    *bfree_out = fr / bs;
    if (*bfree_out > *blk_out)
        *bfree_out = *blk_out;
    if (*blk_out > (uint64_t)UINT32_MAX) {
        *blk_out = UINT32_MAX;
        if (*bfree_out > *blk_out)
            *bfree_out = *blk_out;
    }
}

static void mtp_fill_statvfs_from_bytes(uint64_t tot, uint64_t fr, struct statvfs *st)
{
    memset(st, 0, sizeof(*st));
    uint64_t bs, nblk, nbf;
    mtp_bytes_to_blk(tot, fr, &bs, &nblk, &nbf);
    st->f_bsize = (unsigned long)bs;
    st->f_frsize = (unsigned long)bs;
    st->f_namemax = 255;
    if (tot == 0u) {
        st->f_blocks = (fsblkcnt_t)(1024UL * 1024UL * 64UL);
        st->f_bfree = st->f_bavail = (fsblkcnt_t)(1024UL * 1024UL * 32UL);
        return;
    }
    st->f_blocks = (fsblkcnt_t)nblk;
    st->f_bfree = st->f_bavail = (fsblkcnt_t)nbf;
}

#if defined(__APPLE__)
static int op_setattr_x(const char *path, struct setattr_x *attr)
{
    (void)attr;
    FUSE_T0(tr, "setattr_x", path);
    return fuse_trc_r(&tr, 0);
}

static int op_fsetattr_x(const char *path, struct setattr_x *attr,
                         struct fuse_file_info *fi)
{
    (void)attr;
    (void)fi;
    FUSE_T0(tr, "fsetattr_x", path);
    return fuse_trc_r(&tr, 0);
}

static int op_chflags(const char *path, uint32_t flags)
{
    (void)flags;
    FUSE_T0(tr, "chflags", path);
    return fuse_trc_r(&tr, 0);
}

static void op_monitor(const char *path, uint32_t ev)
{
    if (mtp_log_is_enabled())
        mtp_log("fuse op=monitor path=%.400s ev=%u", path, (unsigned)ev);
}

static int op_renamex(const char *from, const char *to, unsigned int flags)
{
    FUSE_T0_2(tr, "renamex", from, to);
    if (mtp_long_transfer_active())
        return fuse_trc_r(&tr, -EBUSY);
    if (flags & (unsigned)RENAME_SWAP)
        return fuse_trc_r(&tr, -EINVAL);
    if (flags & (unsigned)RENAME_EXCL) {
        mtp_stat_t sx;
        int sr = mtp_stat_refresh(to, &sx);
        if (sr == 0)
            return fuse_trc_r(&tr, -EEXIST);
        if (sr != -ENOENT)
            return fuse_trc_r(&tr, sr);
    }
    return fuse_trc_r(&tr, mtp_rename_with_xattr(from, to));
}

static int op_statfs_x(const char *path, struct statfs *st)
{
    FUSE_T0(tr, "statfs_x", path);
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
        const uint64_t bs = 4096;
        uint64_t nblk = tot / bs, nbf = fr / bs;
        if (nblk > (uint64_t)UINT32_MAX) {
            nbf = (uint64_t)((long double)fr * (uint64_t)UINT32_MAX / (long double)tot);
            if (nbf > (uint64_t)UINT32_MAX)
                nbf = UINT32_MAX;
            nblk = UINT32_MAX;
        }
        st->f_blocks = (int64_t)nblk;
        st->f_bfree = (int64_t)nbf;
        st->f_bavail = (int64_t)nbf;
    }
    st->f_files = 1000000;
    st->f_ffree = 500000;
    strncpy(st->f_fstypename, "mtp", sizeof(st->f_fstypename) - 1);
    st->f_fstypename[sizeof(st->f_fstypename) - 1] = '\0';
    return fuse_trc_r(&tr, 0);
}

static int op_exchange(const char *p1, const char *p2, unsigned long opts)
{
    (void)opts;
    FUSE_T0_2(tr, "exchange", p1, p2);
    return fuse_trc_r(&tr, -EXDEV);
}

static int op_getxtimes(const char *path, struct timespec *bkuptime,
                        struct timespec *crtime)
{
    FUSE_T0(tr, "getxtimes", path);
    if (!bkuptime || !crtime)
        return fuse_trc_r(&tr, -EINVAL);
    mtp_stat_t s;
    int rc = mtp_stat(path, &s);
    if (rc != 0) {
        struct stat st;
        int pr = pending_getattr(path, &st);
        if (pr != 0) {
            int r = pr < 0 ? pr : rc;
            return fuse_trc_r(&tr, r);
        }
        time_t mt = st.st_mtime;
        bkuptime->tv_sec = mt;
        bkuptime->tv_nsec = 0;
        crtime->tv_sec = mt;
        crtime->tv_nsec = 0;
        return fuse_trc_r(&tr, 0);
    }
    time_t mt = (time_t)s.mtime;
    if (mt == 0 && s.is_dir)
        mt = time(NULL);
    bkuptime->tv_sec = mt;
    bkuptime->tv_nsec = 0;
    crtime->tv_sec = mt;
    crtime->tv_nsec = 0;
    return fuse_trc_r(&tr, 0);
}

static int op_setbkuptime(const char *path, const struct timespec *tv)
{
    (void)tv;
    FUSE_T0(tr, "setbkuptime", path);
    return fuse_trc_r(&tr, 0);
}

static int op_setchgtime(const char *path, const struct timespec *tv)
{
    (void)tv;
    FUSE_T0(tr, "setchgtime", path);
    return fuse_trc_r(&tr, 0);
}

static int op_setcrtime(const char *path, const struct timespec *tv)
{
    (void)tv;
    FUSE_T0(tr, "setcrtime", path);
    return fuse_trc_r(&tr, 0);
}
#endif

static int op_statfs(const char *path, struct statvfs *st)
{
    FUSE_T0(tr, "statfs", path);
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
        return fuse_trc_r(&tr, 0);
    }
    mtp_fill_statvfs_from_bytes(tot, fr, st);
    return fuse_trc_r(&tr, 0);
}

#ifdef __APPLE__
static int op_getxattr(const char *path, const char *name, char *value, size_t size,
                       uint32_t position)
{
    FUSE_T0_2(tr, "getxattr", path, name ? name : "");
    if (path && path[0] == '/' && path[1] == '\0' && name &&
        strcmp(name, "com.apple.FinderInfo") == 0 && mtp_root_volume_icon_active()) {
        int xr = xa_get(path, name, value, size, position);
        if (xr != -ENOATTR)
            return fuse_trc_r(&tr, xr);
        static const unsigned char kRootFinderInfo[32] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x04, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        };
        const size_t full = sizeof kRootFinderInfo;
        if ((size_t)position >= full)
            return fuse_trc_r(&tr, 0);
        size_t tail = full - (size_t)position;
        if (size == 0)
            return fuse_trc_r(&tr, (int)tail);
        if (size < tail)
            return fuse_trc_r(&tr, -ERANGE);
        if (value)
            memcpy(value, kRootFinderInfo + position, tail);
        return fuse_trc_r(&tr, (int)tail);
    }
    return fuse_trc_r(&tr, xa_get(path, name, value, size, position));
}
#else
static int op_getxattr(const char *path, const char *name, char *value, size_t size)
{
    FUSE_T0_2(tr, "getxattr", path, name ? name : "");
    return fuse_trc_r(&tr, xa_get(path, name, value, size, 0u));
}
#endif

static int op_listxattr(const char *path, char *list, size_t size)
{
    FUSE_T0(tr, "listxattr", path);
#if defined(__APPLE__)
    static const char kFi[] = "com.apple.FinderInfo";
    const size_t fi_len = sizeof kFi; /* includes NUL */
    int add_fi = (path && path[0] == '/' && path[1] == '\0' && mtp_root_volume_icon_active() &&
                  !xa_has_xattr(path, kFi));
#else
    int add_fi = 0;
    const size_t fi_len = 0;
#endif
    int need = xa_list(path, NULL, 0);
    if (need < 0)
        return fuse_trc_r(&tr, need);
#if defined(__APPLE__)
    size_t total = (size_t)need + (add_fi ? fi_len : 0);
#else
    size_t total = (size_t)need;
#endif
    if (size == 0)
        return fuse_trc_r(&tr, (int)total);
    if (size < total)
        return fuse_trc_r(&tr, -ERANGE);
    if (need > 0) {
        int w = xa_list(path, list, size);
        if (w != need)
            return fuse_trc_r(&tr, w);
    }
#if defined(__APPLE__)
    if (add_fi)
        memcpy(list + (size_t)need, kFi, fi_len);
#endif
    return fuse_trc_r(&tr, (int)total);
}

#ifdef __APPLE__
static int op_setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags, uint32_t position)
{
    FUSE_T0_2(tr, "setxattr", path, name ? name : "");
    return fuse_trc_r(&tr, xa_set(path, name, value, size, flags, position));
}
#else
static int op_setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags)
{
    FUSE_T0_2(tr, "setxattr", path, name ? name : "");
    return fuse_trc_r(&tr, xa_set(path, name, value, size, flags, 0u));
}
#endif

static int op_removexattr(const char *path, const char *name)
{
    FUSE_T0_2(tr, "removexattr", path, name ? name : "");
    return fuse_trc_r(&tr, xa_remove_one(path, name));
}

static int op_fallocate(const char *path, int mode, off_t offset, off_t length,
                        struct fuse_file_info *fi)
{
    (void)mode;
    (void)offset;
    (void)length;
    (void)fi;
    FUSE_T0(tr, "fallocate", path);
    return fuse_trc_r(&tr, 0);
}

/* macFUSE fuse_conn_info has max_write / max_readahead (no max_read field). */
static void *op_init(struct fuse_conn_info *conn)
{
#if defined(__APPLE__)
    if (conn->max_write < (unsigned)1048576)
        conn->max_write = (unsigned)1048576;
    if (conn->max_readahead < (unsigned)2097152)
        conn->max_readahead = (unsigned)2097152;
    if (mtp_log_is_enabled())
        mtp_log("fuse op=init max_write=%u max_readahead=%u", (unsigned)conn->max_write,
                (unsigned)conn->max_readahead);
    struct fuse_context *fc = fuse_get_context();
    if (fc && fc->fuse) {
        g_mtpfuse_handle = fc->fuse;
        g_mtpfuse_remote_inval_stop = 0;
        pthread_t tid;
        if (pthread_create(&tid, NULL, mtp_remote_inval_loop, NULL) != 0)
            mtp_log("pthread_create(mtp_remote_inval_loop) failed");
        else
            pthread_detach(tid);
    }
#endif
    return NULL;
}

static void op_destroy(void *userdata)
{
    (void)userdata;
    if (mtp_log_is_enabled())
        mtp_log("fuse op=destroy");
#if defined(__APPLE__)
    g_mtpfuse_remote_inval_stop = 1;
    g_mtpfuse_handle = NULL;
#endif
}

/* Send an immediate invalidation pulse to Finder for the given path.
 * This is called after a transfer completes to refresh any folders that
 * might have been blocked during the transfer. */
void mtp_invalidate_fuse_path(const char *path)
{
#if defined(__APPLE__)
    if (!path || path[0] != '/')
        return;
    struct fuse *f = g_mtpfuse_handle;
    if (!f)
        return;
    int ir = fuse_invalidate_path(f, path);
    if (ir < 0 && ir != -ENOENT)
        mtp_log("fuse_invalidate_path(%s) rc=%d", path, ir);
#endif
}

struct fuse_operations mtpfuse_ops = {
    .init        = op_init,
    .destroy     = op_destroy,
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
