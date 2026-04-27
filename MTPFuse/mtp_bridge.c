/*
 * mtp_bridge.c — thin wrapper around libmtp for the FUSE layer.
 *
 * Caches an in-memory tree of (path -> object id) so that FUSE can do
 * path-based operations on top of libmtp's id-based world.
 *
 * Loading is lazy and one MTP level at a time (BFS-by-navigation):
 * resolve() walks path components and calls load_children() only on
 * each ancestor — never recursively prefetches deeper folders.
 *
 * Phone apps / other PCs change MTP objects without notifying us — listings are
 * cached and invalidated periodically / after local writes (see mtp_invalidate_fuse_dir_cache).
 *
 * Uploads: FUSE writes to one unlinked staging file; on close we call
 * LIBMTP_Send_File_From_File_Descriptor, which streams from that fd to USB — no
 * second full-file copy in userspace (only mtp_write_full’s rare buf path buffers once).
 *
 * Locking (deadlock avoidance):
 *   • g_lock  — in-memory tree; callers hold it across resolve/load_children.
 *   • g_mtp   — all libmtp USB I/O; take only while NOT holding g_mtp then
 *               waiting for g_lock. load_children: g_lock → g_mtp (release
 *               g_mtp before returning; never block on g_lock while holding g_mtp).
 *   • mtp_close: g_lock then g_mtp (same as other “full session” teardown).
 * FUSE fs_ops.c uses g_stage_mu only around lazy download; order there is
 * g_stage_mu → (bridge takes g_lock → g_mtp inside mtp_download_to_fd).
 */

#define _DARWIN_C_SOURCE 1
#include "mtp_bridge.h"

#include <libmtp.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <sys/stat.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

/* ---------------- node tree ---------------- */

typedef struct mtp_node {
    char            *name;          /* basename, never NULL */
    uint32_t         object_id;     /* 0 for storage root nodes */
    uint32_t         storage_id;
    int              is_dir;
    int              is_synth;     /* 1: macOS Finder metadata path, not on device */
    uint64_t         size;
    uint64_t         mtime;
    double           meta_cached_at; /* now_sec() after last Get_Filemetadata sync (files) */
    int              load_busy;      /* directory listing in progress (other ops wait) */
    uint32_t         load_gen;       /* bumped when cache dropped; stale loaders discard */
    int              children_loaded;
    struct mtp_node *parent;
    struct mtp_node *first_child;
    struct mtp_node *next_sibling;
} mtp_node_t;

static LIBMTP_mtpdevice_t *g_device = NULL;
static mtp_node_t         *g_root   = NULL;
/* Canonical host path of the FUSE mount (for statfs path normalization). */
static char               *g_fuse_mount_host_abs = NULL;
static pthread_mutex_t     g_lock   = PTHREAD_MUTEX_INITIALIZER;
/* Serialize libmtp USB I/O (library is not thread-safe). Lock order: always
 * take g_lock before g_mtp when both are needed (see mtp_close). */
static pthread_mutex_t     g_mtp    = PTHREAD_MUTEX_INITIALIZER;

/* Non-zero while a multi-GB Send_File / Get_File holds g_mtp. Cheap UI ops
 * (getattr metadata refresh, statfs storage refresh) check this and fall back
 * to cached values instead of queuing behind the transfer — that queueing is
 * what froze Finder for the entire copy. The flag is purely advisory; libmtp
 * is still serialized by g_mtp. */
static atomic_int g_mtp_long_xfer;

static inline void mtp_long_xfer_begin(void)
{
    atomic_fetch_add_explicit(&g_mtp_long_xfer, 1, memory_order_acq_rel);
}

static inline void mtp_long_xfer_end(void)
{
    atomic_fetch_sub_explicit(&g_mtp_long_xfer, 1, memory_order_acq_rel);
}

static inline int mtp_long_xfer_active(void)
{
    return atomic_load_explicit(&g_mtp_long_xfer, memory_order_acquire) > 0;
}

/* -1 = unknown, 0 = no, 1 = yes (LIBMTP_DEVICECAP_GetPartialObject). */
static int g_cap_partial_get = -1;

/* LIBMTP_Get_Storage is slow; statfs + pre-copy checks poll it often. */
static const double k_mtp_storage_cache_ttl_sec = 4.0;
/* Finder issues many getattrs; full Get_Filemetadata each time serializes on g_mtp and freezes UI.
 * Reads/downloads always refresh separately. Override with MTP_STAT_META_TTL_SEC (0 = every stat). */
static const double k_mtp_stat_meta_ttl_sec_default = 60.0;

static pthread_cond_t  g_dir_load_cv = PTHREAD_COND_INITIALIZER;
/* Async folder listing runs Get_Children + metadata without g_lock so Finder can
 * getattr/readdir other paths. mtp_close / mtp_refresh_tree wait for inflight==0. */
static pthread_mutex_t g_tree_async_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_tree_async_cv = PTHREAD_COND_INITIALIZER;
static int             g_tree_async_inflight;
static int             g_tree_async_pause;
static int             g_tree_async_closing;

typedef struct {
    uint32_t id;
    uint64_t max_cap;
    uint64_t free_bytes;
} mtp_stor_snap_t;

static mtp_stor_snap_t g_st_cache[64];
static size_t          g_st_cache_n;
static double          g_st_cache_mono = -1e9;
/* Lightweight mutex held only while reading/writing the storage snapshot
 * itself. Cache writers also hold g_mtp (libmtp serialization); cache
 * readers take only this so statfs never queues behind a long transfer. */
static pthread_mutex_t g_st_cache_mu = PTHREAD_MUTEX_INITIALIZER;

static void log_mtp_errors(void);
static double now_sec(void);

int mtp_anon_tempfile_fd(const char *stem)
{
    if (!stem || !stem[0])
        return -EINVAL;
    char tmpl[PATH_MAX];
    const char *d = getenv("TMPDIR");
    if (!d || !d[0])
        d = "/tmp";
    size_t dl = strlen(d);
    int n = snprintf(tmpl, sizeof tmpl, "%s%s%sXXXXXX", d,
                     (dl > 0 && d[dl - 1] == '/') ? "" : "/", stem);
    if (n < 0 || (size_t)n >= sizeof tmpl)
        return -ENAMETOOLONG;
    int fd = mkstemp(tmpl);
    if (fd < 0)
        return -errno;
    unlink(tmpl);
    return fd;
}

/* USB MTP stacks often flake with a single NAK; Finder maps hard I/O failure
 * to “The device disappeared.” Multi-level retries cover long full-file pulls.
 *
 * Two retry budgets:
 *   • Short ops (Get_Children, Get_Filemetadata, Delete_Object, Set_Filename,
 *     Move_Object): can afford up to MTP_USB_RETRIES because each attempt is
 *     fast (sub-second).
 *   • Big transfers (Get_File_To_FD, Send_File_From_FD): ONE attempt may take
 *     minutes and holds g_mtp the whole time. Anything queued behind it is
 *     blocked. So we cap inner retries hard and lean on outer-wave restarts
 *     instead — that returns control between waves so other UI ops (statfs,
 *     stat, readdir of new folders) can squeeze in. */
enum {
    /* Inner attempts per short libmtp call (NAK / brief stall). */
    MTP_USB_RETRIES = 16,
    /* Per-call retries for full-file Get_File / Send_File.
     * Reduced from 3 to 2 to prevent retry storms. */
    MTP_USB_TRANSFER_RETRIES = 2,
    /* Full download restarts (large files often fail near EOF).
     * Reduced from 3 to 2 to prevent retry storms. */
    MTP_DOWNLOAD_OUTER_WAVES = 2,
    /* Full upload restarts after send or post-send verify fails.
     * Reduced from 3 to 2 to prevent retry storms. */
    MTP_SEND_OUTER_WAVES = 2,
};

static void mtp_usb_backoff(unsigned attempt_1based)
{
    if (attempt_1based == 0)
        return;
    /* 100ms … cap at 10s for very long transfers / deep USB sleep. */
    unsigned ms = 100u * (attempt_1based > 24u ? 24u : attempt_1based);
    if (attempt_1based > 8u)
        ms += 50u * (attempt_1based - 8u);
    if (attempt_1based > 16u)
        ms += 100u * (attempt_1based - 16u);
    if (ms > 10000u)
        ms = 10000u;
    usleep(ms * 1000u);
}

/* Bounded backoff for transfer retries — caps at 1s so a single bad transfer
 * cannot hold g_mtp for tens of seconds between attempts. */
static void mtp_usb_transfer_backoff(unsigned attempt_1based)
{
    if (attempt_1based == 0)
        return;
    unsigned ms = 125u * (attempt_1based > 8u ? 8u : attempt_1based);
    if (ms > 1000u)
        ms = 1000u;
    usleep(ms * 1000u);
}

/* Returns 0, -ENODEV, or -1 (caller maps -1 → -EIO). Resets [fd] to empty + offset 0 between tries. */
static int mtp_get_file_to_fd_retry(uint32_t oid, int fd)
{
    int last = -1;
    for (unsigned k = 0; k < (unsigned)MTP_USB_TRANSFER_RETRIES; k++) {
        if (k > 0) {
            mtp_usb_transfer_backoff((unsigned)k);
            if (ftruncate(fd, 0) < 0)
                return -errno;
            if (lseek(fd, 0, SEEK_SET) < 0)
                return -errno;
        }
        mtp_long_xfer_begin();
        pthread_mutex_lock(&g_mtp);
        if (!g_device) {
            pthread_mutex_unlock(&g_mtp);
            mtp_long_xfer_end();
            return -ENODEV;
        }
        last = LIBMTP_Get_File_To_File_Descriptor(g_device, oid, fd, NULL, NULL);
        pthread_mutex_unlock(&g_mtp);
        mtp_long_xfer_end();
        if (last == 0)
            return 0;
        log_mtp_errors();
    }
    (void)last;
    return -1;
}

/* Full-file Get_File with outer restarts. If libmtp reports success but the fd is
 * shorter than the listed size, or Finder fsync/close does post-copy I/O, we
 * would otherwise return OK then fail later; verify length and trim if larger. */
static int mtp_get_oid_to_fd_full_retry(uint32_t oid, int fd, uint64_t expect_sz)
{
    for (unsigned w = 0; w < (unsigned)MTP_DOWNLOAD_OUTER_WAVES; w++) {
        if (w > 0) {
            mtp_debug_log("mtp_get_oid_to_fd full restart wave %u oid=%u", w, oid);
            mtp_usb_backoff(12u + w);
            if (ftruncate(fd, 0) < 0)
                return -errno;
            if (lseek(fd, 0, SEEK_SET) < 0)
                return -errno;
        }
        int rc = mtp_get_file_to_fd_retry(oid, fd);
        if (rc == -ENODEV)
            return -ENODEV;
        if (rc == 0) {
            if (expect_sz == 0u)
                return 0;
            struct stat st;
            if (fstat(fd, &st) < 0) {
                mtp_debug_log("mtp_get_oid_to_fd fstat after Get_File oid=%u (retry wave)", oid);
                continue;
            }
            if ((uint64_t)st.st_size < expect_sz) {
                mtp_debug_log("mtp_get_oid_to_fd short file oid=%u need=%llu got=%llu",
                    oid, (unsigned long long)expect_sz,
                    (unsigned long long)st.st_size);
                continue;
            }
            /* Metadata often under-reports media size; never trim a longer object. */
            if ((uint64_t)st.st_size > expect_sz) {
                mtp_debug_log("mtp_get_oid_to_fd oid=%u metadata=%llu on_disk=%llu (keep full)",
                    oid, (unsigned long long)expect_sz,
                    (unsigned long long)st.st_size);
            }
            return 0;
        }
    }
    return -1;
}

/* Returns 0, -ENODEV, or -1 (EIO). Seeks fd to 0 before each attempt. */
static int mtp_send_file_from_fd_retry(LIBMTP_file_t *meta, int fd)
{
    int last = -1;
    for (unsigned k = 0; k < (unsigned)MTP_USB_TRANSFER_RETRIES; k++) {
        if (k > 0)
            mtp_usb_transfer_backoff((unsigned)k);
        if (lseek(fd, 0, SEEK_SET) < 0)
            return -errno;
        mtp_long_xfer_begin();
        pthread_mutex_lock(&g_mtp);
        if (!g_device) {
            pthread_mutex_unlock(&g_mtp);
            mtp_long_xfer_end();
            return -ENODEV;
        }
        last = LIBMTP_Send_File_From_File_Descriptor(g_device, fd, meta, NULL, NULL);
        pthread_mutex_unlock(&g_mtp);
        mtp_long_xfer_end();
        if (last == 0)
            return 0;
        log_mtp_errors();
    }
    (void)last;
    return -1;
}

static int mtp_delete_object_retry(uint32_t oid)
{
    int last = -1;
    for (unsigned k = 0; k < (unsigned)MTP_USB_RETRIES; k++) {
        if (k > 0)
            mtp_usb_backoff((unsigned)k);
        pthread_mutex_lock(&g_mtp);
        if (!g_device) {
            pthread_mutex_unlock(&g_mtp);
            return -ENODEV;
        }
        last = LIBMTP_Delete_Object(g_device, oid);
        pthread_mutex_unlock(&g_mtp);
        if (last == 0)
            return 0;
        log_mtp_errors();
    }
    (void)last;
    return -1;
}

/* File metadata refresh must not run mtp_usb_backoff() while holding g_lock:
 * Finder would freeze for the whole mount until USB retries finish. */
static mtp_node_t *resolve(const char *path);
static mtp_node_t *resolve_cached(const char *path, int load_children_flag);

static double stat_meta_ttl_sec(void)
{
    static double cache = -1.0;
    if (cache >= 0.0)
        return cache;
    const char *e = getenv("MTP_STAT_META_TTL_SEC");
    if (e && e[0]) {
        char *end = NULL;
        double v = strtod(e, &end);
        if (end != e && v >= 0.0 && v <= 600.0) {
            cache = v;
            return cache;
        }
    }
    cache = k_mtp_stat_meta_ttl_sec_default;
    return cache;
}

static mtp_node_t *node_find_by_object_id(mtp_node_t *r, uint32_t oid)
{
    if (!r || oid == 0u)
        return NULL;
    if (!r->is_dir && r->object_id == oid)
        return r;
    for (mtp_node_t *c = r->first_child; c; c = c->next_sibling) {
        mtp_node_t *h = node_find_by_object_id(c, oid);
        if (h)
            return h;
    }
    return NULL;
}

/* Returns 0 if skipped (dir/synth/no oid) or updated; -1 if metadata missing. */
static int refresh_file_meta_for_path(const char *path_use)
{
    if (!path_use)
        return -1;
    for (unsigned k = 0; k < (unsigned)MTP_USB_RETRIES; k++) {
        if (k > 0)
            mtp_usb_backoff((unsigned)k);
        pthread_mutex_lock(&g_lock);
        mtp_node_t *n = resolve(path_use);
        if (!n) {
            pthread_mutex_unlock(&g_lock);
            return -1;
        }
        if (n->is_synth || n->is_dir || n->object_id == 0u) {
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        uint32_t oid = n->object_id;
        pthread_mutex_unlock(&g_lock);

        pthread_mutex_lock(&g_mtp);
        LIBMTP_file_t *f = NULL;
        if (g_device)
            f = LIBMTP_Get_Filemetadata(g_device, oid);
        pthread_mutex_unlock(&g_mtp);
        if (f) {
            uint64_t sz = f->filesize;
            uint64_t mt = (uint64_t)f->modificationdate;
            LIBMTP_destroy_file_t(f);
            pthread_mutex_lock(&g_lock);
            n = resolve(path_use);
            if (n && !n->is_synth && !n->is_dir && n->object_id == oid) {
                n->size  = sz;
                n->mtime = mt;
                n->meta_cached_at = now_sec();
            }
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        log_mtp_errors();
    }
    return -1;
}

static void refresh_meta_by_oid_for_tree(uint32_t oid)
{
    if (oid == 0u)
        return;
    for (unsigned k = 0; k < (unsigned)MTP_USB_RETRIES; k++) {
        if (k > 0)
            mtp_usb_backoff((unsigned)k);
        pthread_mutex_lock(&g_mtp);
        LIBMTP_file_t *f = NULL;
        if (g_device)
            f = LIBMTP_Get_Filemetadata(g_device, oid);
        pthread_mutex_unlock(&g_mtp);
        if (f) {
            uint64_t sz = f->filesize;
            uint64_t mt = (uint64_t)f->modificationdate;
            LIBMTP_destroy_file_t(f);
            pthread_mutex_lock(&g_lock);
            mtp_node_t *hit = node_find_by_object_id(g_root, oid);
            if (hit && !hit->is_synth && !hit->is_dir) {
                hit->size  = sz;
                hit->mtime = mt;
                hit->meta_cached_at = now_sec();
            }
            pthread_mutex_unlock(&g_lock);
            return;
        }
        log_mtp_errors();
    }
}

/* ---------- helpers ---------- */

static mtp_node_t *node_new(const char *name, int is_dir,
                            uint32_t oid, uint32_t sid)
{
    mtp_node_t *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->name = strdup(name ? name : "");
    if (!n->name) {
        free(n);
        return NULL;
    }
    n->object_id = oid;
    n->storage_id = sid;
    n->is_dir = is_dir;
    return n;
}

static void node_free(mtp_node_t *n)
{
    if (!n) return;
    /* Unlink from parent when still in-tree (caller may have cleared parent->first_child first). */
    if (n->parent) {
        mtp_node_t **slot = &n->parent->first_child;
        while (*slot && *slot != n)
            slot = &(*slot)->next_sibling;
        if (*slot)
            *slot = n->next_sibling;
        n->next_sibling = NULL;
        n->parent = NULL;
    }
    mtp_node_t *c = n->first_child;
    while (c) {
        mtp_node_t *nx = c->next_sibling;
        node_free(c);
        c = nx;
    }
    free(n->name);
    free(n);
}

static void node_add_child(mtp_node_t *parent, mtp_node_t *child)
{
    if (!parent || !child)
        return;
    child->parent = parent;
    child->next_sibling = parent->first_child;
    parent->first_child = child;
}

static mtp_node_t *node_find_child(mtp_node_t *parent, const char *name)
{
    if (!parent || !name)
        return NULL;
    for (mtp_node_t *c = parent->first_child; c; c = c->next_sibling) {
        if (c->name && strcmp(c->name, name) == 0) return c;
    }
    return NULL;
}

/* Volume nodes are synthetic: object_id 0 + real storage_id (see load_children).
 * libmtp expects LIBMTP_FILES_AND_FOLDERS_ROOT for “this storage’s top level”,
 * not parent handle 0 — using 0 breaks many phones (flat junk listings, wrong
 * objects). g_root uses storage_id 0 so it is not confused with a volume. */
static uint32_t mtp_parent_handle(const mtp_node_t *folder)
{
    if (!folder || folder == g_root)
        return LIBMTP_FILES_AND_FOLDERS_ROOT;
    if (folder->object_id == 0 && folder->storage_id != 0)
        return LIBMTP_FILES_AND_FOLDERS_ROOT;
    return folder->object_id;
}

/* Nested folder nodes often have storage_id 0; Move_Object needs the real volume id. */
static uint32_t node_effective_storage_id(const mtp_node_t *n)
{
    if (!n)
        return 0u;
    if (n->storage_id != 0u)
        return n->storage_id;
    for (mtp_node_t *w = n->parent; w; w = w->parent) {
        if (w->storage_id != 0u)
            return w->storage_id;
    }
    return 0u;
}

/* Re-find a directory after async load (pointer [dir] may not be validated across g_lock). */
static mtp_node_t *node_find_dir_by_key(mtp_node_t *r, uint32_t sid, uint32_t oid)
{
    if (!r)
        return NULL;
    if (r->is_dir && !r->is_synth && node_effective_storage_id(r) == sid && r->object_id == oid)
        return r;
    for (mtp_node_t *c = r->first_child; c; c = c->next_sibling) {
        mtp_node_t *h = node_find_dir_by_key(c, sid, oid);
        if (h)
            return h;
    }
    return NULL;
}

static void free_built_child_chain(mtp_node_t *head)
{
    while (head) {
        mtp_node_t *nx = head->next_sibling;
        head->next_sibling = NULL;
        node_free(head);
        head = nx;
    }
}

static int tree_async_begin(void)
{
    pthread_mutex_lock(&g_tree_async_mu);
    if (g_tree_async_pause || g_tree_async_closing) {
        pthread_mutex_unlock(&g_tree_async_mu);
        return -1;
    }
    g_tree_async_inflight++;
    pthread_mutex_unlock(&g_tree_async_mu);
    return 0;
}

static void tree_async_end(void)
{
    pthread_mutex_lock(&g_tree_async_mu);
    g_tree_async_inflight--;
    pthread_cond_broadcast(&g_tree_async_cv);
    pthread_mutex_unlock(&g_tree_async_mu);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Caller holds g_mtp. Also takes g_st_cache_mu for the read-side. */
static void storage_cache_invalidate_unlocked(void)
{
    pthread_mutex_lock(&g_st_cache_mu);
    g_st_cache_n = 0;
    g_st_cache_mono = -1e9;
    pthread_mutex_unlock(&g_st_cache_mu);
}

/* Precondition: g_mtp held. */
static int storage_cache_ensure_fresh_unlocked(void)
{
    if (!g_device)
        return -ENODEV;
    double now = now_sec();
    if (g_st_cache_n > 0 && (now - g_st_cache_mono) < k_mtp_storage_cache_ttl_sec)
        return 0;
    LIBMTP_Get_Storage(g_device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
    mtp_stor_snap_t tmp[sizeof(g_st_cache) / sizeof(g_st_cache[0])];
    size_t tmp_n = 0;
    for (LIBMTP_devicestorage_t *s = g_device->storage;
         s && tmp_n < sizeof(tmp) / sizeof(tmp[0]); s = s->next) {
        tmp[tmp_n].id = s->id;
        tmp[tmp_n].max_cap = s->MaxCapacity;
        tmp[tmp_n].free_bytes = s->FreeSpaceInBytes;
        tmp_n++;
    }
    pthread_mutex_lock(&g_st_cache_mu);
    if (tmp_n)
        memcpy(g_st_cache, tmp, tmp_n * sizeof(tmp[0]));
    g_st_cache_n = tmp_n;
    g_st_cache_mono = now;
    pthread_mutex_unlock(&g_st_cache_mu);
    return 0;
}

static void mtp_storage_cache_bust(void)
{
    pthread_mutex_lock(&g_mtp);
    storage_cache_invalidate_unlocked();
    pthread_mutex_unlock(&g_mtp);
}

#if defined(__APPLE__)
static void mtp_synth_attach_volume_meta(mtp_node_t *vol);

/* Finder volume art: host .icns path from MTP_VOLUME_ICON_PATH; exposed as /.VolumeIcon.icns. */
static char g_volicon_src[PATH_MAX];

static void mtp_cache_volume_icon_path_from_env(void)
{
    g_volicon_src[0] = '\0';
    const char *vip = getenv("MTP_VOLUME_ICON_PATH");
    if (!vip || !vip[0])
        return;
    struct stat st;
    if (stat(vip, &st) != 0 || !S_ISREG(st.st_mode))
        return;
    if (snprintf(g_volicon_src, sizeof g_volicon_src, "%s", vip) >= (int)sizeof g_volicon_src)
        g_volicon_src[0] = '\0';
}

static int mtp_volicon_pread(char *buf, size_t size, off_t offset)
{
    if (!g_volicon_src[0])
        return -EIO;
    int fd = open(g_volicon_src, O_RDONLY);
    if (fd < 0)
        return -errno;
    ssize_t r = pread(fd, buf, size, offset);
    int e = errno;
    close(fd);
    if (r < 0)
        return -e;
    return (int)r;
}

static int mtp_volicon_copy_to_fd(int outfd)
{
    if (!g_volicon_src[0])
        return -EIO;
    int in = open(g_volicon_src, O_RDONLY);
    if (in < 0)
        return -errno;
    unsigned char chunk[64 * 1024];
    ssize_t nread;
    while ((nread = read(in, chunk, sizeof chunk)) > 0) {
        ssize_t w = write(outfd, chunk, (size_t)nread);
        if (w != nread) {
            close(in);
            return -EIO;
        }
    }
    int saved = errno;
    close(in);
    if (nread < 0)
        return -saved;
    return 0;
}

static void mtp_add_root_volume_icon_locked(mtp_node_t *dir)
{
    if (dir != g_root)
        return;
    if (!g_volicon_src[0])
        return;
    if (node_find_child(dir, ".VolumeIcon.icns"))
        return;
    struct stat st;
    if (stat(g_volicon_src, &st) != 0 || !S_ISREG(st.st_mode))
        return;
    mtp_node_t *vn = node_new(".VolumeIcon.icns", 0, 0, 0);
    if (!vn)
        return;
    vn->is_synth = 1;
    vn->size = (uint64_t)st.st_size;
    vn->mtime = st.st_mtime > 0 ? (uint64_t)st.st_mtime : (uint64_t)time(NULL);
    node_add_child(dir, vn);
}

int mtp_root_volume_icon_active(void)
{
    if (!g_volicon_src[0])
        return 0;
    struct stat st;
    return (stat(g_volicon_src, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
}
#endif

#if !defined(__APPLE__)
int mtp_root_volume_icon_active(void)
{
    return 0;
}
#endif

/* Caller must hold g_lock. Frees all cached children (recursive); clears loaded flag. */
static void dir_drop_children_locked(mtp_node_t *dir)
{
    if (!dir || !dir->is_dir)
        return;
    dir->load_gen++;
    mtp_node_t *c = dir->first_child;
    dir->first_child = NULL;
    dir->children_loaded = 0;
    while (c) {
        mtp_node_t *nx = c->next_sibling;
        c->next_sibling = NULL;
        c->parent = NULL;
        node_free(c);
        c = nx;
    }
}

/* SwiftMTP FAQ: exclusive MTP session — same idea as Kalam/OpenMTP “close other holders”. */
static void mtp_print_mtp_session_hint(void)
{
    fprintf(stderr,
            "mtp_open: If the device is connected, another app may be holding the MTP USB session.\n"
            "  Quit Preview, Image Capture, and Android File Transfer (also its background agent in\n"
            "  Activity Monitor), then unplug/replug or retry.\n");
    fflush(stderr);
}

static int mtp_skip_hidden_dot_filenames(void)
{
    static int cached = -1;
    if (cached >= 0)
        return cached;
    const char *a = getenv("MTP_SKIP_HIDDEN");
    const char *b = getenv("MTP_HIDE_DOTFILES");
    int on = (a && a[0] && strcmp(a, "0") != 0) || (b && b[0] && strcmp(b, "0") != 0);
    cached = on ? 1 : 0;
    return cached;
}

static int mtp_load_trace_stderr(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("MTPFUSE_VERBOSE_LOAD");
        v = (e && e[0] && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return v;
}

/* Caller must hold g_lock. Serializes per-directory listing so two FUSE threads
 * do not duplicate Get_Children storms. Non-root listings drop g_lock during
 * USB (see g_tree_async_*); root stays under g_lock (few storages). */
static void load_children(mtp_node_t *dir)
{
    if (!dir->is_dir || dir->children_loaded)
        return;
    if (dir->load_busy) {
        while (dir->load_busy)
            pthread_cond_wait(&g_dir_load_cv, &g_lock);
        return;
    }
    dir->load_busy = 1;

    double t0 = now_sec();
    int trace = mtp_load_trace_stderr();

    if (dir == g_root) {
        mtp_debug_log("[LOAD] root start");
        if (trace) {
            fprintf(stderr, "[LOAD] root start\n");
            fflush(stderr);
        }
        if (!g_device) {
            dir->children_loaded = 1;
            goto load_done;
        }
        /* Some phones report NULL/duplicate StorageDescription for multiple
         * volumes; skipping the second hid entire storages (e.g. only “SD card”). */
        for (int attempt = 0; attempt < 3; attempt++) {
            pthread_mutex_lock(&g_mtp);
            if (g_device)
                LIBMTP_Get_Storage(g_device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
            pthread_mutex_unlock(&g_mtp);
            if (!g_device)
                break;
            for (LIBMTP_devicestorage_t *s = g_device->storage; s; s = s->next) {
                const char *base = (s->StorageDescription && s->StorageDescription[0])
                                       ? s->StorageDescription
                                       : "Storage";
                char vlabel[384];
                snprintf(vlabel, sizeof(vlabel), "%.300s", base);
                int tag = 0;
                while (node_find_child(dir, vlabel)) {
                    if (tag == 0)
                        snprintf(vlabel, sizeof(vlabel), "%.280s [%u]", base, s->id);
                    else
                        snprintf(vlabel, sizeof(vlabel), "%.250s [%u]#%d", base, s->id, tag);
                    tag++;
                    if (tag > 64) {
                        mtp_debug_log("[LOAD] root storage sid=%u: could not uniquify label", s->id);
                        break;
                    }
                }
                if (node_find_child(dir, vlabel))
                    continue;
                if (strcmp(vlabel, base) != 0)
                    mtp_debug_log("[LOAD] root storage label sid=%u: \"%s\" → \"%s\"", s->id,
                                  base, vlabel);
                mtp_node_t *n = node_new(vlabel, 1, 0, s->id);
                if (n)
                    node_add_child(dir, n);
            }
            if (dir->first_child)
                break;
            /* Do not drop g_lock during backoff: mtp_close may tear down the tree. */
            mtp_usb_backoff((unsigned)(attempt + 1u));
        }
#if defined(__APPLE__)
        mtp_add_root_volume_icon_locked(dir);
#endif
        dir->children_loaded = 1;
        mtp_debug_log("[LOAD] root done %.3fs", now_sec() - t0);
        if (trace) {
            fprintf(stderr, "[LOAD] root done %.3fs\n", now_sec() - t0);
            fflush(stderr);
        }
        goto load_done;
    }

    uint32_t sid = dir->storage_id;
    uint32_t pid = mtp_parent_handle(dir);
    uint32_t dir_key_sid = node_effective_storage_id(dir);
    uint32_t dir_key_oid = dir->object_id;
    uint32_t gen_start = dir->load_gen;
    const char *dbg = dir->name ? dir->name : "?";

    mtp_debug_log("[LOAD] \"%s\" sid=%u parent_id=0x%x → Get_Files_And_Folders (batch metadata)",
                  dbg, sid, pid);
    if (trace) {
        fprintf(stderr, "[LOAD] \"%s\" sid=%u parent_id=0x%x (MTP listing)…\n",
                dbg, sid, pid);
        fflush(stderr);
    }

    int async_ok = (tree_async_begin() == 0);
    if (async_ok)
        pthread_mutex_unlock(&g_lock);

    LIBMTP_file_t *files = NULL;
    int nkids = -1;
    for (unsigned k = 0; k < (unsigned)MTP_USB_RETRIES; k++) {
        if (k > 0) {
            mtp_usb_backoff((unsigned)k);
            if (files) {
                LIBMTP_destroy_file_t(files);
                files = NULL;
            }
        }
        pthread_mutex_lock(&g_mtp);
        if (!g_device) {
            pthread_mutex_unlock(&g_mtp);
            if (files) {
                LIBMTP_destroy_file_t(files);
                files = NULL;
            }
            nkids = -1;
            break;
        }
        files = LIBMTP_Get_Files_And_Folders(g_device, sid, pid);
        pthread_mutex_unlock(&g_mtp);
        if (files) {
            /* Count entries by traversing the linked list */
            nkids = 0;
            LIBMTP_file_t *tmp = files;
            while (tmp) {
                nkids++;
                tmp = tmp->next;
            }
            break;
        }
        log_mtp_errors();
    }

    mtp_debug_log("[LOAD] \"%s\" Get_Files_And_Folders → n=%d elapsed %.3fs",
                  dbg, nkids, now_sec() - t0);

    mtp_node_t *built = NULL;
    int count = 0;
    int meta_aborted = 0;

    if (nkids > 0 && files) {
        LIBMTP_file_t *file = files;
        while (file) {
            int is_dir = (file->filetype == LIBMTP_FILETYPE_FOLDER);
            if (file->filename && file->filename[0] &&
                !(mtp_skip_hidden_dot_filenames() && file->filename[0] == '.')) {
                mtp_node_t *n = node_new(file->filename, is_dir,
                                         file->item_id, file->storage_id);
                if (n) {
                    n->size  = file->filesize;
                    n->mtime = file->modificationdate;
                    if (!is_dir)
                        n->meta_cached_at = now_sec();
                    n->next_sibling = built;
                    built = n;
                    count++;
                    if (trace && (count % 200) == 0) {
                        mtp_debug_log("[LOAD] \"%s\" … %d entries (%.3fs)",
                                      dbg, count, now_sec() - t0);
                        fprintf(stderr, "[LOAD] \"%s\" … %d entries so far\n", dbg, count);
                        fflush(stderr);
                    } else if ((count % 200) == 0) {
                        mtp_debug_log("[LOAD] \"%s\" … %d entries (%.3fs)",
                                      dbg, count, now_sec() - t0);
                    }
                }
            }
            file = file->next;
        }
    }

    if (files) {
        LIBMTP_destroy_file_t(files);
        files = NULL;
    }

    mtp_node_t *dir_target = dir;

    if (async_ok) {
        pthread_mutex_lock(&g_lock);
        tree_async_end();
        if (g_tree_async_closing) {
            free_built_child_chain(built);
            goto load_done;
        }
        dir_target = node_find_dir_by_key(g_root, dir_key_sid, dir_key_oid);
        if (!dir_target || dir_target->load_gen != gen_start || dir_target->children_loaded) {
            free_built_child_chain(built);
            goto load_done;
        }
    }

    if (nkids < 0) {
        if (!dir_target->children_loaded)
            dir_target->children_loaded = 1;
        free_built_child_chain(built);
        if (trace) {
            fprintf(stderr, "[LOAD] \"%s\" Get_Children failed\n", dbg);
            fflush(stderr);
        }
        goto load_done;
    }

    if (nkids == 0) {
#if defined(__APPLE__)
        if (dir_target->parent == g_root)
            mtp_synth_attach_volume_meta(dir_target);
#endif
        if (!dir_target->children_loaded)
            dir_target->children_loaded = 1;
        free_built_child_chain(built);
        mtp_debug_log("[LOAD] \"%s\" empty folder (0 handles)", dbg);
        if (trace) {
            fprintf(stderr, "[LOAD] \"%s\" done: 0 entries\n", dbg);
            fflush(stderr);
        }
        goto load_done;
    }

    if (meta_aborted) {
        free_built_child_chain(built);
        if (!dir_target->children_loaded)
            dir_target->children_loaded = 1;
        mtp_debug_log("[LOAD] \"%s\" aborted mid-list (device gone)", dbg);
        goto load_done;
    }

    while (built) {
        mtp_node_t *n = built;
        built = n->next_sibling;
        n->next_sibling = NULL;
        if (n->name && !node_find_child(dir_target, n->name))
            node_add_child(dir_target, n);
        else
            node_free(n);
    }

#if defined(__APPLE__)
    if (dir_target->parent == g_root)
        mtp_synth_attach_volume_meta(dir_target);
#endif
    if (!dir_target->children_loaded)
        dir_target->children_loaded = 1;

    mtp_debug_log("[LOAD] \"%s\" done: %d entries in %.3fs", dbg, count, now_sec() - t0);
    if (trace) {
        fprintf(stderr, "[LOAD] \"%s\" done: %d entries in %.3fs\n",
                dbg, count, now_sec() - t0);
        fflush(stderr);
    }

load_done:
    dir->load_busy = 0;
    pthread_cond_broadcast(&g_dir_load_cv);
}

#if defined(__APPLE__)
/* Finder probes paths like `/.Trashes` that are not on MTP; ENOENT is noisy and
 * can contribute to spurious "device disappeared" handling. We expose them as
 * read-only empty dirs without listing them under `/` (not children of g_root). */
static char g_synthn_trashes[]  = ".Trashes";
static char g_synthn_fsevents[] = ".fseventsd";
static char g_synthn_temp[]     = ".TemporaryItems";
static mtp_node_t g_synth_trashes = { .name = g_synthn_trashes,  .is_dir = 1, .is_synth = 1, .children_loaded = 1 };
static mtp_node_t g_synth_fsev   = { .name = g_synthn_fsevents, .is_dir = 1, .is_synth = 1, .children_loaded = 1 };
static mtp_node_t g_synth_temp   = { .name = g_synthn_temp,     .is_dir = 1, .is_synth = 1, .children_loaded = 1 };

static mtp_node_t *mtp_synth_root_child(const char *tok)
{
    if (!tok) return NULL;
    if (strcmp(tok, g_synthn_trashes) == 0)  return &g_synth_trashes;
    if (strcmp(tok, g_synthn_fsevents) == 0) return &g_synth_fsev;
    if (strcmp(tok, g_synthn_temp) == 0)     return &g_synth_temp;
    return NULL;
}

/* Three root-level synth nodes only; in-memory children are Finder-created. */
static int mtp_synth_is_static(const mtp_node_t *n)
{
    return n == (const mtp_node_t *)&g_synth_trashes
        || n == (const mtp_node_t *)&g_synth_fsev
        || n == (const mtp_node_t *)&g_synth_temp;
}

/* Drop subdirs (e.g. .Trashes/501) before refresh/close; not on device. */
static void mtp_synth_free_user_branches(void)
{
    mtp_node_t *roots[] = { &g_synth_trashes, &g_synth_fsev, &g_synth_temp };
    for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); r++) {
        mtp_node_t *d = roots[r];
        if (!d->first_child)
            continue;
        mtp_node_t *c = d->first_child;
        d->first_child = NULL;
        while (c) {
            mtp_node_t *nx = c->next_sibling;
            c->next_sibling = NULL;
            c->parent = NULL;
            node_free(c);
            c = nx;
        }
    }
}

/* Finder probes `/StorageName/.Trashes` on every volume; only `/.Trashes`
 * was synthesised at the mount root, so ENOENT on storage volumes confused
 * the metadata phase after a successful byte copy (“no permission”). */
static void mtp_synth_attach_volume_meta(mtp_node_t *vol)
{
    static const char *names[] = { ".Trashes", ".fseventsd", ".TemporaryItems" };
    if (!vol || !vol->is_dir || vol->parent != g_root)
        return;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (node_find_child(vol, names[i]))
            continue;
        mtp_node_t *sn = node_new(names[i], 1, 0u, vol->storage_id);
        if (!sn)
            return;
        sn->is_synth = 1;
        sn->children_loaded = 1;
        node_add_child(vol, sn);
    }
}
#endif

/* Resolve "/a/b/c" → node, lazy-loading directories along the way.
 * Call only while holding g_lock (load_children keeps the lock). */
static mtp_node_t *resolve(const char *path)
{
    return resolve_cached(path, 1);
}

/* resolve_cached: if load_children == 1, load as needed (slow path).
 * If load_children == 0, return cached node or NULL (fast getattr path). */
static mtp_node_t *resolve_cached(const char *path, int load_children_flag)
{
    if (!path || path[0] != '/') return NULL;
    if (strlen(path) >= PATH_MAX) return NULL;
    if (path[1] == '\0') return g_root;

    mtp_node_t *cur = g_root;
    if (load_children_flag)
        load_children(cur);

    char *dup = strdup(path + 1);
    if (!dup) return NULL;

    char *save = NULL;
    for (char *tok = strtok_r(dup, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (load_children_flag)
            load_children(cur);
        mtp_node_t *child = node_find_child(cur, tok);
        if (!child) {
#if defined(__APPLE__)
            if (cur == g_root) {
                mtp_node_t *syn = mtp_synth_root_child(tok);
                if (syn) {
                    cur = syn;
                    continue;
                }
            }
#endif
            free(dup);
            return NULL;
        }
        cur = child;
    }
    free(dup);
    return cur;
}

static void log_mtp_errors(void)
{
    if (!g_device)
        return;
    LIBMTP_Dump_Errorstack(g_device);
    LIBMTP_Clear_Errorstack(g_device);
}

/* Pick raw[] index: env MTP_USB_BUS_LOCATION (decimal or 0x hex) should match
 * libmtp bus_location; else MTP_RAW_INDEX; else 0. */
static int pick_raw_device_index(LIBMTP_raw_device_t *raw, int n_raw)
{
    const char *want_env = getenv("MTP_USB_BUS_LOCATION");
    if (want_env && want_env[0]) {
        char *end = NULL;
        unsigned long v = strtoul(want_env, &end, 0);
        if (end != want_env && *end == '\0' && v <= UINT32_MAX) {
            uint32_t want = (uint32_t)v;
            for (int i = 0; i < n_raw; i++) {
                if (raw[i].bus_location == want) {
                    mtp_debug_log("mtp_open: raw[%d] bus_location=0x%x matches "
                                  "MTP_USB_BUS_LOCATION",
                                  i, want);
                    fprintf(stderr, "mtp_open: using raw[%d] bus_location=0x%x\n",
                            i, want);
                    fflush(stderr);
                    return i;
                }
            }
            mtp_debug_log("mtp_open: no raw[] with bus_location=0x%x (wanted "
                          "MTP_USB_BUS_LOCATION=%s)",
                          want, want_env);
            fprintf(stderr, "mtp_open: no device with bus_location=0x%x; candidates:\n",
                    want);
            for (int i = 0; i < n_raw; i++) {
                fprintf(stderr, "  [%d] bus_location=0x%x VID=%04x PID=%04x\n",
                        i, raw[i].bus_location,
                        raw[i].device_entry.vendor_id,
                        raw[i].device_entry.product_id);
            }
            fflush(stderr);
        }
    }
    const char *ix_env = getenv("MTP_RAW_INDEX");
    if (ix_env && ix_env[0]) {
        int j = atoi(ix_env);
        if (j >= 0 && j < n_raw) {
            mtp_debug_log("mtp_open: using MTP_RAW_INDEX=%d", j);
            fprintf(stderr, "mtp_open: using MTP_RAW_INDEX=%d\n", j);
            fflush(stderr);
            return j;
        }
    }
    return 0;
}

/* ---------- public api ---------- */

int mtp_open(void)
{
    enum { MTP_OPEN_ATTEMPTS = 4 };

    setvbuf(stderr, NULL, _IOLBF, 0);
    mtp_debug_log("mtp_open: LIBMTP_Init");
    fprintf(stderr, "mtp_open: LIBMTP_Init...\n"); fflush(stderr);
    LIBMTP_Init();

    for (int attempt = 0; attempt < MTP_OPEN_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            fprintf(stderr, "mtp_open: retry %d/%d (USB settle / release MTP session)...\n",
                    attempt, MTP_OPEN_ATTEMPTS - 1);
            fflush(stderr);
            usleep(1500000);
        }

        mtp_debug_log("mtp_open: Detect_Raw_Devices (attempt %d)", attempt);
        fprintf(stderr, "mtp_open: detecting raw devices...\n"); fflush(stderr);

        LIBMTP_raw_device_t *raw = NULL;
        int n_raw = 0;
        LIBMTP_error_number_t err = LIBMTP_Detect_Raw_Devices(&raw, &n_raw);
        mtp_debug_log("mtp_open: detect err=%d n_raw=%d", (int)err, n_raw);
        fprintf(stderr, "mtp_open: detect returned err=%d, n=%d\n", err, n_raw); fflush(stderr);

        if (err != LIBMTP_ERROR_NONE || n_raw == 0) {
            free(raw);
            if (attempt == MTP_OPEN_ATTEMPTS - 1) {
                fprintf(stderr, "mtp_open: no MTP device found "
                                "(unlock phone, enable File Transfer / MTP)\n");
                mtp_print_mtp_session_hint();
            }
            continue;
        }

        int ri = pick_raw_device_index(raw, n_raw);
        mtp_debug_log("mtp_open: Open_Raw_Device_Uncached raw[%d] VID=%04x PID=%04x "
                      "bus_location=0x%x",
                      ri, raw[ri].device_entry.vendor_id, raw[ri].device_entry.product_id,
                      raw[ri].bus_location);
        fprintf(stderr, "mtp_open: opening raw device %d (VID=%04x PID=%04x bus=0x%x)...\n",
                ri, raw[ri].device_entry.vendor_id, raw[ri].device_entry.product_id,
                raw[ri].bus_location);
        fflush(stderr);
        g_device = LIBMTP_Open_Raw_Device_Uncached(&raw[ri]);
        free(raw);
        if (!g_device) {
            mtp_debug_log("mtp_open: Open_Raw_Device failed (attempt %d)", attempt);
            fprintf(stderr, "mtp_open: LIBMTP_Open_Raw_Device_Uncached failed\n");
            if (attempt == MTP_OPEN_ATTEMPTS - 1)
                mtp_print_mtp_session_hint();
            continue;
        }

        char *name  = LIBMTP_Get_Friendlyname(g_device);
        char *model = LIBMTP_Get_Modelname(g_device);
        mtp_debug_log("mtp_open: connected name=%s model=%s",
                      name ? name : "?", model ? model : "?");
        fprintf(stderr, "mtp_open: connected to %s (%s)\n",
                name  ? name  : "?",
                model ? model : "?");
        fflush(stderr);
        free(name); free(model);

        mtp_debug_log("mtp_open: Get_Storage");
        fprintf(stderr, "mtp_open: enumerating storage...\n"); fflush(stderr);
        LIBMTP_Get_Storage(g_device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
        fprintf(stderr, "mtp_open: ready\n"); fflush(stderr);

        g_root = node_new("", 1, 0, 0);
        if (!g_root) {
            mtp_debug_log("mtp_open: node_new(root) failed");
            fprintf(stderr, "mtp_open: out of memory building root node\n");
            LIBMTP_Release_Device(g_device);
            g_device = NULL;
            return -1;
        }
        mtp_debug_log("mtp_open: success g_root=%p", (void *)g_root);
#if defined(__APPLE__)
        mtp_cache_volume_icon_path_from_env();
#endif
        pthread_mutex_lock(&g_tree_async_mu);
        g_tree_async_closing = 0;
        g_tree_async_pause = 0;
        pthread_mutex_unlock(&g_tree_async_mu);
        return 0;
    }
    return -1;
}

void mtp_set_fuse_mount_point(const char *mountpoint)
{
    free(g_fuse_mount_host_abs);
    g_fuse_mount_host_abs = NULL;
    if (!mountpoint || !mountpoint[0])
        return;
    char canon[PATH_MAX];
    if (realpath(mountpoint, canon))
        g_fuse_mount_host_abs = strdup(canon);
    else
        g_fuse_mount_host_abs = strdup(mountpoint);
}

/* If the kernel passes a full host path under our mount, map to "/…" in the
 * MTP namespace so resolve() finds the right storage_id. */
static const char *mtp_normalize_path_for_tree(const char *path, char *out, size_t outsz)
{
    if (!path || !out || outsz < 2u)
        return path;
    if (!g_fuse_mount_host_abs || !g_fuse_mount_host_abs[0])
        return path;
    size_t pl = strlen(g_fuse_mount_host_abs);
    if (strncmp(path, g_fuse_mount_host_abs, pl) != 0)
        return path;
    if (path[pl] != '\0' && path[pl] != '/')
        return path; /* e.g. …/Phone vs …/Phone2 — not our mount */
    const char *tail = path + pl;
    if (*tail == '\0') {
        out[0] = '/';
        out[1] = '\0';
        return out;
    }
    if (*tail == '/') {
        size_t tl = strlen(tail) + 1u;
        if (tl > outsz)
            return path;
        memcpy(out, tail, tl);
        return out;
    }
    if ((size_t)snprintf(out, outsz, "/%s", tail) >= outsz)
        return path;
    return out;
}

/* macFUSE sometimes passes a full host path; map to in-mount "/Volume/…" for resolve(). */
static const char *mtp_path_for_tree(const char *path, char *norm_buf, size_t norm_sz)
{
    if (!path || path[0] != '/')
        return path;
    return mtp_normalize_path_for_tree(path, norm_buf, norm_sz);
}

void mtp_invalidate_fuse_dir_cache(const char *fuse_path)
{
    if (!fuse_path || fuse_path[0] != '/')
        return;
    char norm[PATH_MAX];
    const char *p = mtp_path_for_tree(fuse_path, norm, sizeof norm);
    if (!p || p[0] != '/')
        return;
    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(p);
    if (n && n->is_dir)
        dir_drop_children_locked(n);
    pthread_mutex_unlock(&g_lock);
}

void mtp_close(void)
{
    mtp_debug_log("mtp_close: begin (release tree + device)");
    pthread_mutex_lock(&g_tree_async_mu);
    g_tree_async_closing = 1;
    g_tree_async_pause = 1;
    while (g_tree_async_inflight > 0)
        pthread_cond_wait(&g_tree_async_cv, &g_tree_async_mu);
    pthread_mutex_unlock(&g_tree_async_mu);

    pthread_mutex_lock(&g_lock);
#if defined(__APPLE__)
    g_volicon_src[0] = '\0';
    mtp_synth_free_user_branches();
#endif
    pthread_mutex_lock(&g_mtp);
    storage_cache_invalidate_unlocked();
    if (g_root)   { node_free(g_root); g_root = NULL; }
    if (g_device) { LIBMTP_Release_Device(g_device); g_device = NULL; }
    g_cap_partial_get = -1;
    pthread_mutex_unlock(&g_mtp);
    pthread_mutex_unlock(&g_lock);
    free(g_fuse_mount_host_abs);
    g_fuse_mount_host_abs = NULL;
    pthread_mutex_lock(&g_tree_async_mu);
    g_tree_async_closing = 0;
    g_tree_async_pause = 0;
    pthread_mutex_unlock(&g_tree_async_mu);
    mtp_debug_shutdown();
}

int mtp_refresh_tree(void)
{
    pthread_mutex_lock(&g_tree_async_mu);
    g_tree_async_pause = 1;
    while (g_tree_async_inflight > 0)
        pthread_cond_wait(&g_tree_async_cv, &g_tree_async_mu);
    pthread_mutex_unlock(&g_tree_async_mu);

    pthread_mutex_lock(&g_lock);
#if defined(__APPLE__)
    mtp_synth_free_user_branches();
#endif
    if (g_root) { node_free(g_root); g_root = NULL; }
    g_root = node_new("", 1, 0, 0);
    int ok = g_root != NULL;
    pthread_mutex_unlock(&g_lock);

    pthread_mutex_lock(&g_tree_async_mu);
    g_tree_async_pause = 0;
    pthread_cond_broadcast(&g_tree_async_cv);
    pthread_mutex_unlock(&g_tree_async_mu);

    mtp_storage_cache_bust();
    if (!ok)
        mtp_debug_log("mtp_refresh_tree: node_new failed");
    return ok ? 0 : -ENOMEM;
}

int mtp_storage_space_for_path(const char *path, uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!total_bytes || !free_bytes)
        return -EINVAL;
    *total_bytes = 0;
    *free_bytes = 0;

    char norm_buf[PATH_MAX];
    const char *path_use = path;
    if (path && path[0] == '/') {
        const char *nrm = mtp_normalize_path_for_tree(path, norm_buf, sizeof norm_buf);
        if (nrm)
            path_use = nrm;
    }

    uint32_t want_sid = 0;
    int have_node = 0;

    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = (path_use && path_use[0] == '/') ? resolve(path_use) : NULL;
    if (n) {
        have_node = 1;
        for (mtp_node_t *w = n; w; w = w->parent) {
            if (w->storage_id != 0u) {
                want_sid = w->storage_id;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);

    /* During a multi-GB Send_File / Get_File g_mtp is pinned for the whole
     * transfer; blocking statfs there is what froze Finder. Read the snapshot
     * under its own short-held mutex (g_st_cache_mu), only reaching for
     * g_mtp to actually refresh the cache, and skip the refresh entirely
     * while a transfer is active. */
    int refreshed = 0;
    if (!mtp_long_xfer_active()) {
        if (pthread_mutex_trylock(&g_mtp) == 0) {
            if (!g_device) {
                pthread_mutex_unlock(&g_mtp);
                return have_node ? -ENODEV : -ENOENT;
            }
            (void)storage_cache_ensure_fresh_unlocked();
            pthread_mutex_unlock(&g_mtp);
            refreshed = 1;
        }
    }

    mtp_stor_snap_t snap[64];
    size_t snap_n = 0;
    pthread_mutex_lock(&g_st_cache_mu);
    snap_n = g_st_cache_n;
    if (snap_n > sizeof(snap) / sizeof(snap[0]))
        snap_n = sizeof(snap) / sizeof(snap[0]);
    if (snap_n)
        memcpy(snap, g_st_cache, snap_n * sizeof(snap[0]));
    pthread_mutex_unlock(&g_st_cache_mu);
    (void)refreshed;

    if (snap_n == 0)
        return have_node ? -ENODEV : -ENOENT;

    uint64_t tot = 0, fr = 0;
    if (want_sid == 0u) {
        for (size_t i = 0; i < snap_n; i++) {
            tot += snap[i].max_cap;
            fr += snap[i].free_bytes;
        }
    } else {
        int found = 0;
        for (size_t i = 0; i < snap_n; i++) {
            if (snap[i].id == want_sid) {
                tot = snap[i].max_cap;
                fr = snap[i].free_bytes;
                found = 1;
                break;
            }
        }
        if (!found)
            return -ENOENT;
    }

    if (fr > tot)
        fr = tot;
    *total_bytes = tot;
    *free_bytes = fr;
    return 0;
}

/* [storage_ref_path] must resolve under the target volume (e.g. parent dir of the file).
 * [bytes_reclaimed_if_replace] is the current on-device size of an object we will delete first. */
static int mtp_precheck_upload_space(const char *storage_ref_path,
    uint64_t upload_bytes, uint64_t bytes_reclaimed_if_replace)
{
    uint64_t total = 0, freeb = 0;
    int rc = mtp_storage_space_for_path(storage_ref_path, &total, &freeb);
    if (rc != 0)
        return rc;
    uint64_t need = upload_bytes + 65536u; /* MTP / allocator slack */
    if (need < upload_bytes)
        return -EFBIG;
    uint64_t avail = freeb + bytes_reclaimed_if_replace;
    if (need > avail) {
        fprintf(stderr,
            "mtpfuse: No disk storage on your Android device (need %llu bytes, %llu available).\n",
            (unsigned long long)need, (unsigned long long)avail);
        mtp_debug_log("mtp_precheck_upload_space ENOSPC ref=%s need=%llu avail=%llu reclaim=%llu",
            storage_ref_path, (unsigned long long)need, (unsigned long long)avail,
            (unsigned long long)bytes_reclaimed_if_replace);
        return -ENOSPC;
    }
    return 0;
}

void mtp_for_each_volume_directory_path(void (*cb)(const char *path, void *ctx), void *ctx)
{
    if (!cb)
        return;
    pthread_mutex_lock(&g_lock);
    if (g_root) {
        load_children(g_root);
        for (mtp_node_t *c = g_root->first_child; c; c = c->next_sibling) {
            if (!c->is_dir || !c->name || !c->name[0] || c->is_synth)
                continue;
            char buf[PATH_MAX];
            if (snprintf(buf, sizeof buf, "/%s", c->name) >= (int)sizeof buf)
                continue;
            cb(buf, ctx);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

static int mtp_stat_impl(const char *path, mtp_stat_t *out, int force_meta_refresh)
{
    if (!out) return -EINVAL;
    double t0 = now_sec();
    char norm_buf[PATH_MAX];
    const char *path_use = mtp_path_for_tree(path, norm_buf, sizeof norm_buf);
    pthread_mutex_lock(&g_lock);
    /* Finder getattr hammers us; use fast cached-only resolve to avoid blocking on load_children. */
    mtp_node_t *n = resolve_cached(path_use, 0);
    int rc = -ENOENT;
    if (n) {
        int is_real_file = (!n->is_synth && !n->is_dir && n->object_id != 0u);
        int need_meta = 0;
        if (is_real_file) {
            if (force_meta_refresh)
                need_meta = 1;
            else {
                double ttl = stat_meta_ttl_sec();
                need_meta = (ttl <= 0.0) || (n->meta_cached_at <= 0.0) ||
                            ((now_sec() - n->meta_cached_at) >= ttl);
            }
            /* Don't queue a Get_Filemetadata behind an in-flight multi-GB
             * Send_File / Get_File: that's the Finder freeze. Cached values
             * stay accurate for the duration of the transfer. force_meta_refresh
             * (mtp_stat_refresh) still wins — it's only used by op_open and
             * download paths that genuinely need the on-device size. */
            if (need_meta && !force_meta_refresh && n->meta_cached_at > 0.0 &&
                mtp_long_xfer_active())
                need_meta = 0;
        }
        if (need_meta) {
            pthread_mutex_unlock(&g_lock);
            /* For force_meta_refresh during a long transfer, trylock first to avoid blocking.
             * If the transfer holds g_mtp, fall back to cached values instead of waiting. */
            int long_xfer_active = mtp_long_xfer_active();
            if (force_meta_refresh && long_xfer_active && n->meta_cached_at > 0.0) {
                /* Use cached values instead of blocking on the transfer. */
                need_meta = 0;
            } else if (refresh_file_meta_for_path(path_use) != 0) {
                return -EIO;
            }
            pthread_mutex_lock(&g_lock);
            n = resolve(path_use);
            if (!n) {
                pthread_mutex_unlock(&g_lock);
                return -ENOENT;
            }
        }
        out->is_dir     = n->is_dir;
        out->size       = n->size;
        out->mtime      = n->mtime;
        out->storage_id = n->storage_id;
        if (n->is_synth) {
#if defined(__APPLE__)
            /* Distinct inodes: real nodes use 0+0 → 1 in FUSE, same as root. */
            if (n == &g_synth_trashes)     out->object_id = 0xE0000001u;
            else if (n == &g_synth_fsev)  out->object_id = 0xE0000002u;
            else if (n == &g_synth_temp)  out->object_id = 0xE0000003u;
            else if (!n->is_dir && n->name && strcmp(n->name, ".VolumeIcon.icns") == 0)
                out->object_id = 0xE0000004u;
            else {
                /* User mkdir under synth (e.g. .Trashes/501) — must not use 0+0. */
                uint32_t h = (uint32_t)(uintptr_t)(void *)n;
                out->object_id = 0xE2000000u | (h & 0x00FFFFFFu);
                if (out->object_id < 0xE2000001u)
                    out->object_id = 0xE2000001u;
            }
#else
            out->object_id = 0xE0000F00u;
#endif
        } else {
            out->object_id = n->object_id;
        }
        rc = 0;
    }
    pthread_mutex_unlock(&g_lock);
    double dt = now_sec() - t0;
    if (dt > 0.25)
        mtp_debug_log("mtp_stat SLOW path=%s rc=%d dt=%.3fs (often loading dir from device)",
                      path, rc, dt);
    return rc;
}

int mtp_stat(const char *path, mtp_stat_t *out)
{
    return mtp_stat_impl(path, out, 0);
}

int mtp_stat_refresh(const char *path, mtp_stat_t *out)
{
    return mtp_stat_impl(path, out, 1);
}

void mtp_readdir_snapshot_free(mtp_dirent_t *entries, size_t n)
{
    if (!entries) return;
    for (size_t i = 0; i < n; i++)
        free(entries[i].name);
    free(entries);
}

int mtp_readdir_snapshot(const char *path, mtp_dirent_t **out, size_t *n_out)
{
    *out = NULL;
    *n_out = 0;
    double t_snap = now_sec();
    mtp_debug_log("readdir_snapshot ENTER path=%s", path);

    char norm_buf[PATH_MAX];
    const char *path_use = mtp_path_for_tree(path, norm_buf, sizeof norm_buf);

    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(path_use);
    if (!n) {
        pthread_mutex_unlock(&g_lock);
        mtp_debug_log("readdir_snapshot ENOENT path=%s (%.3fs)", path, now_sec() - t_snap);
        return -ENOENT;
    }
    if (!n->is_dir) {
        pthread_mutex_unlock(&g_lock);
        mtp_debug_log("readdir_snapshot ENOTDIR path=%s (%.3fs)", path, now_sec() - t_snap);
        return -ENOTDIR;
    }
    /* Use cached tree; refetch only if never loaded or after mtp_invalidate_fuse_dir_cache /
     * upload invalidation / periodic volume drop (fs_ops). */
    mtp_debug_log("readdir_snapshot load_children path=%s", path);
    load_children(n);

    size_t count = 0;
    for (mtp_node_t *c = n->first_child; c; c = c->next_sibling)
        count++;

    mtp_dirent_t *arr = NULL;
    if (count) {
        arr = calloc(count, sizeof(mtp_dirent_t));
        if (!arr) {
            pthread_mutex_unlock(&g_lock);
            mtp_debug_log("readdir_snapshot ENOMEM path=%s count=%zu", path, count);
            return -ENOMEM;
        }
    }

    size_t i = 0;
    for (mtp_node_t *c = n->first_child; c; c = c->next_sibling) {
        arr[i].name    = strdup(c->name);
        arr[i].is_dir  = c->is_dir;
        arr[i].size    = c->size;
        arr[i].mtime   = c->mtime;
        arr[i].object_id  = c->object_id;
        arr[i].storage_id = c->storage_id;
        if (!arr[i].name) {
            mtp_readdir_snapshot_free(arr, i);
            pthread_mutex_unlock(&g_lock);
            mtp_debug_log("readdir_snapshot ENOMEM strdup path=%s i=%zu", path, i);
            return -ENOMEM;
        }
        i++;
    }
    pthread_mutex_unlock(&g_lock);

    *out = arr;
    *n_out = count;
    mtp_debug_log("readdir_snapshot OK path=%s n=%zu total %.3fs", path, count,
                  now_sec() - t_snap);
    return 0;
}

/* ---- file IO ---- */

int mtp_supports_partial_read(void)
{
    if (g_cap_partial_get >= 0)
        return g_cap_partial_get;
    pthread_mutex_lock(&g_mtp);
    if (!g_device) {
        pthread_mutex_unlock(&g_mtp);
        return 0;
    }
    int ok = LIBMTP_Check_Capability(g_device, LIBMTP_DEVICECAP_GetPartialObject);
    g_cap_partial_get = ok ? 1 : 0;
    pthread_mutex_unlock(&g_mtp);
    mtp_debug_log("mtp_supports_partial_read: %d", g_cap_partial_get);
    return g_cap_partial_get;
}

int mtp_read_partial(uint32_t oid, uint64_t file_size, char *buf, size_t size,
                     off_t offset)
{
    if (offset < 0)
        return -EINVAL;
    if (oid == 0u)
        return -EINVAL;
    if ((uint64_t)offset >= file_size)
        return 0;
    uint64_t remain = file_size - (uint64_t)offset;
    if (size > remain)
        size = (size_t)remain;
    if (size == 0)
        return 0;

    /* PTP chunk size: 2MiB caps round-trips on long reads while staying below typical firmware limits. */
    const uint32_t chunk_max = 2097152u;
    uint32_t req = (size > (size_t)chunk_max) ? chunk_max : (uint32_t)size;

    for (unsigned k = 0; k < (unsigned)MTP_USB_RETRIES; k++) {
        if (k > 0)
            mtp_usb_backoff((unsigned)k);
        unsigned char *data = NULL;
        unsigned int got = 0;
        pthread_mutex_lock(&g_mtp);
        if (!g_device) {
            pthread_mutex_unlock(&g_mtp);
            return -ENODEV;
        }
        int last = LIBMTP_GetPartialObject(g_device, oid, (uint64_t)offset, req,
                                           &data, &got);
        pthread_mutex_unlock(&g_mtp);
        if (last == 0 && data && got > 0u) {
            if (got > req)
                got = req;
            if (got > size)
                got = (unsigned int)size;
            /* Short reads mid-file are almost always USB flake — retry instead of returning
             * a truncated buffer (corrupts copies at TB scale). EOF tail may be short. */
            uint64_t endpos = (uint64_t)offset + (uint64_t)got;
            int at_eof = endpos >= file_size;
            if (!at_eof && got < req) {
                free(data);
                mtp_debug_log("mtp_read_partial: short mid-file oid=%u off=%llu got=%u want=%u → retry",
                              oid, (unsigned long long)offset, got, req);
                log_mtp_errors();
                continue;
            }
            memcpy(buf, data, got);
            free(data);
            return (int)got;
        }
        if (data) {
            free(data);
            data = NULL;
        }
        log_mtp_errors();
    }
    return -EIO;
}

int mtp_read(const char *path, char *buf, size_t size, off_t offset)
{
    double t0 = now_sec();
    char norm_buf[PATH_MAX];
    const char *path_use = mtp_path_for_tree(path, norm_buf, sizeof norm_buf);
    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(path_use);
    if (!n)        { pthread_mutex_unlock(&g_lock); return -ENOENT; }
    if (n->is_dir) { pthread_mutex_unlock(&g_lock); return -EISDIR; }
#if defined(__APPLE__)
    if (n->is_synth && !n->is_dir && n->name && strcmp(n->name, ".VolumeIcon.icns") == 0) {
        uint64_t total = n->size;
        pthread_mutex_unlock(&g_lock);
        if ((uint64_t)offset >= total)
            return 0;
        if (offset + size > total)
            size = (size_t)(total - (uint64_t)offset);
        return mtp_volicon_pread(buf, size, offset);
    }
#endif
    pthread_mutex_unlock(&g_lock);
    if (refresh_file_meta_for_path(path_use) != 0)
        return -EIO;
    pthread_mutex_lock(&g_lock);
    n = resolve(path_use);
    if (!n)        { pthread_mutex_unlock(&g_lock); return -ENOENT; }
    if (n->is_dir) { pthread_mutex_unlock(&g_lock); return -EISDIR; }
    uint32_t oid = n->object_id;
    uint64_t total = n->size;
    pthread_mutex_unlock(&g_lock);

    /* Bound reads by refreshed metadata. Media opens should use full-object prefetch in
     * fs_ops (Get_File) so length matches bytes on wire; probing past metadata via
     * GetPartialObject can append garbage on some devices and corrupt duplicates. */
    if ((uint64_t)offset >= total)
        return 0;
    if (offset + size > total) size = (size_t)(total - offset);

    mtp_debug_log("mtp_read begin path=%s oid=%u off=%lld sz=%zu file_sz=%llu",
                  path, oid, (long long)offset, size, (unsigned long long)total);

    if (mtp_supports_partial_read()) {
        int pr = mtp_read_partial(oid, total, buf, size, offset);
        mtp_debug_log("mtp_read end path=%s partial_rc=%d dt=%.3fs",
                      path, pr, now_sec() - t0);
        return pr;
    }

    /* No GetPartialObject: pull full file into a temp fd and pread(). */
    int fd = mtp_anon_tempfile_fd("mtpfuse_read");
    if (fd < 0)
        return fd;

    int rc = mtp_get_oid_to_fd_full_retry(oid, fd, total);
    if (rc == -ENODEV) {
        close(fd);
        return -ENODEV;
    }
    if (rc != 0) {
        mtp_debug_log("mtp_read Get_File_To_FD FAIL path=%s dt=%.3fs",
                      path, now_sec() - t0);
        close(fd);
        return -EIO;
    }

    size_t got = 0;
    while (got < size) {
        ssize_t r = pread(fd, buf + got, size - got, offset + (off_t)got);
        if (r < 0) {
            int e = errno;
            close(fd);
            return -e;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    mtp_debug_log("mtp_read end path=%s got=%zu dt=%.3fs (full file pulled from device each call)",
                  path, got, now_sec() - t0);
    return (int)got;
}

int mtp_download_to_fd(const char *path, int fd)
{
    char norm_buf[PATH_MAX];
    const char *path_use = mtp_path_for_tree(path, norm_buf, sizeof norm_buf);
    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(path_use);
    if (!n) {
        pthread_mutex_unlock(&g_lock);
        return -ENOENT;
    }
    if (n->is_dir) {
        pthread_mutex_unlock(&g_lock);
        return -EISDIR;
    }
#if defined(__APPLE__)
    if (n->is_synth && !n->is_dir && n->name && strcmp(n->name, ".VolumeIcon.icns") == 0) {
        pthread_mutex_unlock(&g_lock);
        if (ftruncate(fd, 0) < 0)
            return -errno;
        if (lseek(fd, 0, SEEK_SET) < 0)
            return -errno;
        int cr = mtp_volicon_copy_to_fd(fd);
        return (cr == 0) ? 0 : cr;
    }
#endif
    pthread_mutex_unlock(&g_lock);
    if (refresh_file_meta_for_path(path_use) != 0)
        return -EIO;
    pthread_mutex_lock(&g_lock);
    n = resolve(path_use);
    if (!n) {
        pthread_mutex_unlock(&g_lock);
        return -ENOENT;
    }
    if (n->is_dir) {
        pthread_mutex_unlock(&g_lock);
        return -EISDIR;
    }
    uint32_t oid = n->object_id;
    uint64_t expect = n->size;
    pthread_mutex_unlock(&g_lock);

    if (ftruncate(fd, 0) < 0)
        return -errno;
    if (lseek(fd, 0, SEEK_SET) < 0)
        return -errno;

    mtp_debug_log("mtp_download_to_fd path=%s oid=%u expect=%llu", path, oid,
                  (unsigned long long)expect);
    int rc = mtp_get_oid_to_fd_full_retry(oid, fd, expect);
    if (rc == -ENODEV)
        return -ENODEV;
    return (rc == 0) ? 0 : -EIO;
}

/* path_copy is a writable copy of an absolute path; splits into parent + name. */
static int path_parent_basename(char *path_copy, const char **parent, const char **name)
{
    if (!path_copy || path_copy[0] != '/')
        return -EINVAL;
    if (path_copy[1] == '\0')
        return -EINVAL;
    char *slash = strrchr(path_copy + 1, '/');
    if (!slash) {
        *parent = "/";
        *name = path_copy + 1;
    } else {
        *slash = '\0';
        *parent = path_copy;
        *name = slash + 1;
    }
    if (!**name)
        return -EINVAL;
    return 0;
}

static void tree_detach(mtp_node_t *n)
{
    if (!n || !n->parent)
        return;
    mtp_node_t *p = n->parent;
    mtp_node_t **slot = &p->first_child;
    while (*slot && *slot != n)
        slot = &(*slot)->next_sibling;
    if (*slot) {
        *slot = n->next_sibling;
        n->next_sibling = NULL;
        n->parent = NULL;
    }
}

/* After upload: update one dentry in an already-loaded folder. Avoids nuking every
 * sibling (Finder re-stat storms + FUSE freezes on large DCIM folders). */
static void tree_upsert_uploaded_file_locked(mtp_node_t *parent, const char *basename,
                                               uint32_t new_oid, uint32_t stor_id,
                                               uint64_t file_size)
{
    if (!parent || !parent->is_dir || !basename || !basename[0] || new_oid == 0u)
        return;
    if (!parent->children_loaded)
        return;
    mtp_node_t *old = node_find_child(parent, basename);
    if (old) {
        tree_detach(old);
        node_free(old);
    }
    mtp_node_t *nn = node_new(basename, 0, new_oid, stor_id);
    if (!nn)
        return;
    nn->size = file_size;
    nn->mtime = (uint64_t)time(NULL);
    nn->meta_cached_at = now_sec();
    node_add_child(parent, nn);
}

/* UNKNOWN works on many phones; explicit MP4 type has triggered bad objects on some stacks. */
static LIBMTP_filetype_t guess_filetype_from_basename(const char *base)
{
    const char *dot = strrchr(base, '.');
    if (!dot || dot[1] == '\0')
        return LIBMTP_FILETYPE_UNKNOWN;
    const char *e = dot + 1;
    if (strcasecmp(e, "mp4") == 0 || strcasecmp(e, "m4v") == 0)
        return LIBMTP_FILETYPE_UNKNOWN;
    if (strcasecmp(e, "mov") == 0)
        return LIBMTP_FILETYPE_QT;
    if (strcasecmp(e, "m4a") == 0)
        return LIBMTP_FILETYPE_M4A;
    if (strcasecmp(e, "3gp") == 0 || strcasecmp(e, "3g2") == 0)
        return LIBMTP_FILETYPE_MPEG;
    if (strcasecmp(e, "webm") == 0 || strcasecmp(e, "mkv") == 0)
        return LIBMTP_FILETYPE_UNDEF_VIDEO;
    if (strcasecmp(e, "jpg") == 0 || strcasecmp(e, "jpeg") == 0)
        return LIBMTP_FILETYPE_JPEG;
    if (strcasecmp(e, "png") == 0)
        return LIBMTP_FILETYPE_PNG;
    if (strcasecmp(e, "mp3") == 0)
        return LIBMTP_FILETYPE_MP3;
    return LIBMTP_FILETYPE_UNKNOWN;
}

/* Compare first and last ~256KiB on device vs host fd. Catches same-size corruption that
 * still breaks QuickTime / on-device players. Requires GetPartialObject. */
static int mtp_verify_upload_head_tail_vs_fd(uint32_t oid, int fd, size_t size)
{
    if (size == 0 || oid == 0u)
        return 0;
    const uint32_t chk = 262144u;
    size_t n = (size < (size_t)chk) ? size : (size_t)chk;
    unsigned char *host = (unsigned char *)malloc(n);
    if (!host)
        return -1;
    if (pread(fd, host, n, 0) != (ssize_t)n) {
        free(host);
        return -1;
    }
    int ok = -1;
    for (unsigned attempt = 0; attempt < 8u; attempt++) {
        if (attempt > 0)
            mtp_usb_backoff(attempt);
        unsigned char *data = NULL;
        unsigned int got = 0;
        pthread_mutex_lock(&g_mtp);
        if (!g_device) {
            pthread_mutex_unlock(&g_mtp);
            free(host);
            return -1;
        }
        int lr = LIBMTP_GetPartialObject(g_device, oid, 0, (uint32_t)n, &data, &got);
        pthread_mutex_unlock(&g_mtp);
        if (lr == 0 && data && got == n && memcmp(host, data, n) == 0) {
            free(data);
            ok = 0;
            break;
        }
        if (data)
            free(data);
    }
    if (ok != 0) {
        mtp_debug_log("mtp_verify_upload: head mismatch oid=%u n=%zu", oid, n);
        free(host);
        return -1;
    }
    if (size > (size_t)chk) {
        uint64_t off = (uint64_t)size - (uint64_t)chk;
        if (pread(fd, host, (size_t)chk, (off_t)off) != (ssize_t)chk) {
            free(host);
            return -1;
        }
        ok = -1;
        for (unsigned attempt = 0; attempt < 8u; attempt++) {
            if (attempt > 0)
                mtp_usb_backoff(attempt);
            unsigned char *data = NULL;
            unsigned int got = 0;
            pthread_mutex_lock(&g_mtp);
            if (!g_device) {
                pthread_mutex_unlock(&g_mtp);
                free(host);
                return -1;
            }
            int lr = LIBMTP_GetPartialObject(g_device, oid, off, chk, &data, &got);
            pthread_mutex_unlock(&g_mtp);
            if (lr == 0 && data && got == chk && memcmp(host, data, chk) == 0) {
                free(data);
                ok = 0;
                break;
            }
            if (data)
                free(data);
        }
        if (ok != 0) {
            mtp_debug_log("mtp_verify_upload: tail mismatch oid=%u off=%llu", oid,
                          (unsigned long long)off);
            free(host);
            return -1;
        }
    }
    free(host);
    mtp_debug_log("mtp_verify_upload: OK oid=%u size=%zu", oid, size);
    return 0;
}

/* Send file bytes from fd (at current offset 0, length size) to MTP at path.
 * Returns 0 on success, -errno on error. Streams via LIBMTP_Send_File_From_File_Descriptor
 * (no extra /tmp copy of the payload on upload). */
static int mtp_write_send_fd(const char *path, int fd, size_t size)
{
    char norm_buf[PATH_MAX];
    const char *path_use = mtp_path_for_tree(path, norm_buf, sizeof norm_buf);
    char *path_dup = strdup(path_use);
    if (!path_dup)
        return -ENOMEM;

    /* Split path into parent and basename. */
    char *slash = strrchr(path_dup, '/');
    if (!slash) {
        free(path_dup);
        return -EINVAL;
    }
    *slash = '\0';
    const char *parent_path = (path_dup[0] == '\0') ? "/" : path_dup;
    const char *basename = slash + 1;
    if (!*basename) {
        free(path_dup);
        return -EINVAL;
    }

    mtp_debug_log("mtp_write_send_fd: path=%s size=%zu basename='%s'", path, size, basename);

    /* Resolve parent directory and check validity. */
    pthread_mutex_lock(&g_lock);
    mtp_node_t *parent_node = resolve(parent_path);
    if (!parent_node || !parent_node->is_dir || parent_node == g_root || parent_node->is_synth) {
        pthread_mutex_unlock(&g_lock);
        free(path_dup);
        int ec = (!parent_node || !parent_node->is_dir) ? ENOENT :
                 (parent_node == g_root) ? EXDEV : EXDEV;
        return -ec;
    }

    uint32_t storage_id = parent_node->storage_id;
    uint32_t parent_id = mtp_parent_handle(parent_node);

    /* Check for existing file to replace. */
    uint32_t replace_oid = 0;
    mtp_node_t *existing = node_find_child(parent_node, basename);
    uint64_t reclaim_replace = 0;
    if (existing && !existing->is_dir) {
        replace_oid = existing->object_id;
        reclaim_replace = existing->size;
        mtp_debug_log("mtp_write_send_fd: replacing existing file object_id=%u", replace_oid);
    }

    pthread_mutex_unlock(&g_lock);

    {
        int pre = mtp_precheck_upload_space(parent_path, (uint64_t)size, reclaim_replace);
        if (pre != 0) {
            free(path_dup);
            return pre;
        }
    }

    /* Delete existing MTP object so the new upload is a clean replace. */
    if (replace_oid) {
        int dr = mtp_delete_object_retry(replace_oid);
        if (dr == -ENODEV) {
            free(path_dup);
            return -ENODEV;
        }
        if (dr != 0) {
            mtp_debug_log("mtp_write_send_fd: failed to delete existing object_id=%u", replace_oid);
            free(path_dup);
            return -EIO;
        }
        /* Short settle after delete; keep minimal to reduce “done copying” lag. */
        usleep(10000);
    }

    /* Ensure fd is at offset 0 with pending writes flushed. */
    if (lseek(fd, 0, SEEK_SET) < 0) {
        free(path_dup);
        return -errno;
    }
    if (fsync(fd) < 0) {
        free(path_dup);
        return -errno;
    }
    {
        struct stat sst;
        if (fstat(fd, &sst) != 0) {
            int e = errno;
            free(path_dup);
            return -e;
        }
        if ((uint64_t)sst.st_size != (uint64_t)size) {
            free(path_dup);
            mtp_debug_log("mtp_write_send_fd: staging size mismatch (got %llu want %zu)",
                          (unsigned long long)sst.st_size, size);
            return -EIO;
        }
    }

    int final_rc = -EIO;

    for (unsigned wave = 0; wave < (unsigned)MTP_SEND_OUTER_WAVES; wave++) {
        if (wave > 0)
            mtp_usb_backoff(10u + wave);
        if (lseek(fd, 0, SEEK_SET) < 0) {
            final_rc = -errno;
            goto mtp_write_send_done;
        }

        LIBMTP_file_t *meta = LIBMTP_new_file_t();
        if (!meta) {
            final_rc = -ENOMEM;
            goto mtp_write_send_done;
        }
        meta->filename = strdup(basename);
        if (!meta->filename) {
            LIBMTP_destroy_file_t(meta);
            final_rc = -ENOMEM;
            goto mtp_write_send_done;
        }
        meta->parent_id = parent_id;
        meta->storage_id = storage_id;
        meta->filesize = (uint64_t)size;
        meta->filetype = guess_filetype_from_basename(basename);
        meta->modificationdate = time(NULL);

        mtp_debug_log("mtp_write_send_fd: wave=%u sending via fd (size=%zu)", wave, size);
        int send_rc = mtp_send_file_from_fd_retry(meta, fd);
        mtp_debug_log("mtp_write_send_fd: wave=%u send_rc=%d", wave, send_rc);

        if (send_rc == -ENODEV) {
            LIBMTP_destroy_file_t(meta);
            final_rc = -ENODEV;
            goto mtp_write_send_done;
        }
        if (send_rc != 0) {
            LIBMTP_destroy_file_t(meta);
            continue;
        }

        int verify_ok = 1;
        uint32_t new_oid = meta->item_id;
        uint64_t post_meta_sz = 0;
        int have_post_meta = 0;
        if (new_oid != 0u) {
            pthread_mutex_lock(&g_mtp);
            LIBMTP_file_t *chk = g_device ? LIBMTP_Get_Filemetadata(g_device, new_oid) : NULL;
            pthread_mutex_unlock(&g_mtp);
            if (!chk) {
                mtp_debug_log("mtp_write_send_fd: post-send verify missing metadata oid=%u wave=%u",
                              new_oid, wave);
                mtp_delete_object_retry(new_oid);
                verify_ok = 0;
            } else {
                post_meta_sz = (uint64_t)chk->filesize;
                have_post_meta = 1;
                LIBMTP_destroy_file_t(chk);
                /* With GetPartialObject we verify bytes; metadata often lies on media. */
                if (mtp_supports_partial_read() && post_meta_sz < (uint64_t)size)
                    mtp_debug_log("mtp_write_send_fd: post-send metadata size %llu < sent %zu "
                                  "(common on media; byte verify next)",
                                  (unsigned long long)post_meta_sz, size);
            }
        } else {
            mtp_debug_log("mtp_write_send_fd: post-send verify skipped (item_id==0)");
        }

        if (verify_ok && new_oid != 0u && size > 0u && mtp_supports_partial_read()) {
            if (mtp_verify_upload_head_tail_vs_fd(new_oid, fd, size) != 0) {
                mtp_delete_object_retry(new_oid);
                verify_ok = 0;
            }
        } else if (verify_ok && new_oid != 0u && size > 0u && !mtp_supports_partial_read()) {
            if (!have_post_meta || post_meta_sz < (uint64_t)size) {
                mtp_debug_log("mtp_write_send_fd: no partial read; metadata %llu vs sent %zu — reject",
                              (unsigned long long)post_meta_sz, size);
                mtp_delete_object_retry(new_oid);
                verify_ok = 0;
            }
        }

        LIBMTP_destroy_file_t(meta);

        if (!verify_ok)
            continue;

        pthread_mutex_lock(&g_lock);
        mtp_node_t *parent_now = resolve(parent_path);
        if (parent_now && new_oid != 0u) {
            uint32_t sid_use = node_effective_storage_id(parent_now);
            if (sid_use == 0u)
                sid_use = storage_id;
            tree_upsert_uploaded_file_locked(parent_now, basename, new_oid, sid_use,
                                             (uint64_t)size);
        }
        pthread_mutex_unlock(&g_lock);

        mtp_storage_cache_bust();
        free(path_dup);
        /* Kick Finder invalidation pulse for parent directory so any blocked
         * folders are refreshed immediately rather than waiting up to 60s. */
        mtp_invalidate_fuse_path(parent_path);
        mtp_debug_log("mtp_write_send_fd: success");
        return 0;
    }

mtp_write_send_done:
    free(path_dup);
    return final_rc;
}

int mtp_write_full_fd(const char *path, int fd, size_t size)
{
    int rc = mtp_write_send_fd(path, fd, size);
    return (rc == 0) ? (int)size : rc;
}

int mtp_write_full(const char *path, const char *buf, size_t size)
{
    int fd = mtp_anon_tempfile_fd("mtpfuse_wr");
    if (fd < 0)
        return fd;
    ssize_t off = 0;
    while ((size_t)off < size) {
        ssize_t w = write(fd, buf + off, size - off);
        if (w <= 0) { close(fd); return -EIO; }
        off += w;
    }
    int rc = mtp_write_send_fd(path, fd, size);
    close(fd);
    return (rc == 0) ? (int)size : rc;
}

int mtp_rename(const char *from, const char *to)
{
    if (!from || !to || from[0] != '/' || to[0] != '/')
        return -EINVAL;
    if (strcmp(from, to) == 0)
        return 0;

    char nfrom[PATH_MAX], nto[PATH_MAX];
    const char *from_use = mtp_path_for_tree(from, nfrom, sizeof nfrom);
    const char *to_use = mtp_path_for_tree(to, nto, sizeof nto);

    char *fpath = strdup(from_use);
    char *tpath = strdup(to_use);
    if (!fpath || !tpath) {
        free(fpath);
        free(tpath);
        return -ENOMEM;
    }

    const char *fparent_s, *fname;
    const char *tparent_s, *tname;
    if (path_parent_basename(fpath, &fparent_s, &fname) != 0 ||
        path_parent_basename(tpath, &tparent_s, &tname) != 0) {
        free(fpath);
        free(tpath);
        return -EINVAL;
    }

    mtp_node_t *orphan_target = NULL;

    pthread_mutex_lock(&g_lock);
    /* Cached directory listings go stale vs the phone; resolve() would not refetch
     * with children_loaded set → ENOENT for a file Finder still shows → error -43. */
    mtp_node_t *src_par = resolve(fparent_s);
    if (src_par && src_par->is_dir)
        dir_drop_children_locked(src_par);
    mtp_node_t *dst_par0 = resolve(tparent_s);
    if (dst_par0 && dst_par0->is_dir)
        dir_drop_children_locked(dst_par0);

    mtp_node_t *src = resolve(from_use);
    if (!src) {
        pthread_mutex_unlock(&g_lock);
        mtp_debug_log("mtp_rename: ENOENT from=%s (after parent cache drop)", from_use);
        free(fpath);
        free(tpath);
        return -ENOENT;
    }
    if (src == g_root) {
        pthread_mutex_unlock(&g_lock);
        free(fpath);
        free(tpath);
        return -EXDEV;
    }
    if (src->is_synth) {
        pthread_mutex_unlock(&g_lock);
        free(fpath);
        free(tpath);
        return -EXDEV;
    }

    mtp_node_t *dst_parent = resolve(tparent_s);
    if (!dst_parent || !dst_parent->is_dir || dst_parent == g_root) {
        pthread_mutex_unlock(&g_lock);
        free(fpath);
        free(tpath);
        if (!dst_parent)
            return -ENOENT;
        return dst_parent->is_dir ? -EXDEV : -ENOTDIR;
    }
    if (dst_parent->is_synth) {
        pthread_mutex_unlock(&g_lock);
        free(fpath);
        free(tpath);
        return -EXDEV;
    }

    mtp_node_t *to_existing = resolve(to_use);
    if (to_existing) {
        if (to_existing->is_synth) {
            pthread_mutex_unlock(&g_lock);
            free(fpath);
            free(tpath);
            return -EXDEV;
        }
        if (to_existing->is_dir || src->is_dir) {
            pthread_mutex_unlock(&g_lock);
            free(fpath);
            free(tpath);
            return (src->is_dir && to_existing->is_dir) ? -EEXIST : -EISDIR;
        }
        tree_detach(to_existing);
        orphan_target = to_existing;
    }

    mtp_node_t *src_parent = src->parent;
    uint32_t new_parent_id = mtp_parent_handle(dst_parent);
    uint32_t new_storage_id = node_effective_storage_id(dst_parent);
    uint32_t dst_oid_for_meta = dst_parent->object_id;
    uint32_t src_oid = src->object_id;
    uint32_t src_sid = node_effective_storage_id(src);
    int same_parent = (src_parent == dst_parent);
    int cross_dir_move = (!same_parent && src->is_dir);

    pthread_mutex_unlock(&g_lock);

    if (orphan_target) {
        int dr = mtp_delete_object_retry(orphan_target->object_id);
        pthread_mutex_lock(&g_lock);
        node_free(orphan_target);
        pthread_mutex_unlock(&g_lock);
        if (dr == -ENODEV) {
            free(fpath);
            free(tpath);
            mtp_refresh_tree();
            return -ENODEV;
        }
        if (dr != 0) {
            free(fpath);
            free(tpath);
            return -EIO;
        }
    }

    if (new_storage_id == 0u && dst_oid_for_meta != 0u) {
        pthread_mutex_lock(&g_mtp);
        if (g_device) {
            LIBMTP_file_t *fm = LIBMTP_Get_Filemetadata(g_device, dst_oid_for_meta);
            if (fm) {
                if (fm->storage_id != 0u)
                    new_storage_id = fm->storage_id;
                LIBMTP_destroy_file_t(fm);
            }
        }
        pthread_mutex_unlock(&g_mtp);
    }
    if (new_storage_id == 0u)
        new_storage_id = src_sid;

    int rc = -1;
    if (same_parent) {
        for (unsigned k = 0; k < (unsigned)MTP_USB_RETRIES; k++) {
            if (k > 0)
                mtp_usb_backoff((unsigned)k);
            pthread_mutex_lock(&g_mtp);
            if (!g_device) {
                pthread_mutex_unlock(&g_mtp);
                free(fpath);
                free(tpath);
                mtp_refresh_tree();
                return -ENODEV;
            }
            rc = LIBMTP_Set_Object_Filename(g_device, src_oid, (char *)tname);
            pthread_mutex_unlock(&g_mtp);
            if (rc == 0)
                break;
            log_mtp_errors();
        }
    } else {
        rc = -1;
        for (unsigned k = 0; k < (unsigned)MTP_USB_RETRIES && rc != 0; k++) {
            if (k > 0)
                mtp_usb_backoff((unsigned)k);
            pthread_mutex_lock(&g_mtp);
            if (!g_device) {
                pthread_mutex_unlock(&g_mtp);
                free(fpath);
                free(tpath);
                mtp_refresh_tree();
                return -ENODEV;
            }
            rc = LIBMTP_Move_Object(g_device, src_oid, new_storage_id, new_parent_id);
            pthread_mutex_unlock(&g_mtp);
            if (rc != 0)
                log_mtp_errors();
        }
        if (rc == 0) {
            rc = -1;
            for (unsigned j = 0; j < (unsigned)MTP_USB_RETRIES && rc != 0; j++) {
                if (j > 0)
                    mtp_usb_backoff((unsigned)j);
                pthread_mutex_lock(&g_mtp);
                if (!g_device) {
                    pthread_mutex_unlock(&g_mtp);
                    free(fpath);
                    free(tpath);
                    mtp_refresh_tree();
                    return -ENODEV;
                }
                rc = LIBMTP_Set_Object_Filename(g_device, src_oid, (char *)tname);
                pthread_mutex_unlock(&g_mtp);
                if (rc != 0)
                    log_mtp_errors();
            }
        }
    }

    if (rc != 0) {
        free(fpath);
        free(tpath);
        /* Do not return EXDEV: Finder would copy via FUSE and re-upload, which has
         * corrupted media for some users. Prefer a hard error over a bad file. */
        fprintf(stderr,
            "mtpfuse: MTP move/rename failed (USB error). Try again, or move the file on the "
            "phone. Avoid Finder copy fallback — it can damage large videos.\n");
        mtp_debug_log("mtp_rename: failed same_parent=%d src_oid=%u dst_storage=%u dst_parent=0x%x",
                      same_parent, src_oid, new_storage_id, new_parent_id);
        return -EIO;
    }

    if (cross_dir_move) {
        free(fpath);
        free(tpath);
        /* Targeted invalidation instead of nuking the entire tree.
         * Source parent: file/dir moved out. Destination parent: new item added. */
        mtp_invalidate_fuse_dir_cache(fparent_s);
        if (strcmp(fparent_s, tparent_s) != 0)
            mtp_invalidate_fuse_dir_cache(tparent_s);
        return 0;
    }

    char *tname_copy = strdup(tname);
    if (!tname_copy) {
        free(fpath);
        free(tpath);
        return -ENOMEM;
    }

    pthread_mutex_lock(&g_lock);
    src = resolve(from_use);
    mtp_node_t *dp = resolve(tparent_s);
    if (!src || !dp) {
        pthread_mutex_unlock(&g_lock);
        free(tname_copy);
        free(fpath);
        free(tpath);
        mtp_refresh_tree();
        return 0;
    }
    if (same_parent) {
        free(src->name);
        src->name = tname_copy;
    } else {
        tree_detach(src);
        free(src->name);
        src->name = tname_copy;
        src->storage_id = new_storage_id;
        node_add_child(dp, src);
    }
    int do_meta = !src->is_dir;
    uint32_t oid_after = src->object_id;
    pthread_mutex_unlock(&g_lock);
    if (do_meta)
        refresh_meta_by_oid_for_tree(oid_after);

    mtp_invalidate_fuse_dir_cache(fparent_s);
    if (strcmp(fparent_s, tparent_s) != 0)
        mtp_invalidate_fuse_dir_cache(tparent_s);

    free(fpath);
    free(tpath);
    return 0;
}

int mtp_unlink(const char *path)
{
    char norm_buf[PATH_MAX];
    const char *path_use = mtp_path_for_tree(path, norm_buf, sizeof norm_buf);
    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(path_use);
    if (!n)        { pthread_mutex_unlock(&g_lock); return -ENOENT; }
    if (n->is_dir) { pthread_mutex_unlock(&g_lock); return -EISDIR; }
    if (n->is_synth) {
        pthread_mutex_unlock(&g_lock);
        return -EXDEV;
    }
    uint32_t oid = n->object_id;
    pthread_mutex_unlock(&g_lock);

    int rc = mtp_delete_object_retry(oid);
    if (rc == -ENODEV)
        return -ENODEV;
    if (rc != 0)
        return -EIO;

    pthread_mutex_lock(&g_lock);
    n = resolve(path_use);
    if (n && n->parent) {
        mtp_node_t **pp = &n->parent->first_child;
        while (*pp && *pp != n) pp = &(*pp)->next_sibling;
        if (*pp) { *pp = n->next_sibling; n->next_sibling = NULL; node_free(n); }
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int mtp_mkdir(const char *path)
{
    char norm_buf[PATH_MAX];
    const char *path_use = mtp_path_for_tree(path, norm_buf, sizeof norm_buf);
    char *dup = strdup(path_use);
    if (!dup) return -ENOMEM;
    char *slash = strrchr(dup, '/');
    if (!slash) { free(dup); return -EINVAL; }
    *slash = '\0';
    const char *parent = (dup[0] == '\0') ? "/" : dup;
    char *name = slash + 1;
    if (!*name) { free(dup); return -EINVAL; }

    pthread_mutex_lock(&g_lock);
    mtp_node_t *p = resolve(parent);
    if (!p) {
        pthread_mutex_unlock(&g_lock); free(dup); return -ENOENT;
    }
    if (!p->is_dir) {
        pthread_mutex_unlock(&g_lock); free(dup); return -ENOTDIR;
    }
    if (p == g_root) {
        pthread_mutex_unlock(&g_lock); free(dup); return -EXDEV;
    }
#if defined(__APPLE__)
    if (p->is_synth) {
        if (node_find_child(p, name)) {
            pthread_mutex_unlock(&g_lock); free(dup); return -EEXIST;
        }
        mtp_node_t *nd = node_new(name, 1, 0, p->storage_id);
        if (!nd) {
            pthread_mutex_unlock(&g_lock); free(dup); return -ENOMEM;
        }
        nd->is_synth     = 1;
        nd->children_loaded = 1;
        node_add_child(p, nd);
        pthread_mutex_unlock(&g_lock);
        free(dup);
        return 0;
    }
#endif
    uint32_t storage_id = p->storage_id;
    uint32_t parent_id  = mtp_parent_handle(p);
    pthread_mutex_unlock(&g_lock);

    pthread_mutex_lock(&g_mtp);
    if (!g_device) {
        pthread_mutex_unlock(&g_mtp);
        free(dup);
        return -ENODEV;
    }
    uint32_t new_id = LIBMTP_Create_Folder(g_device, name, parent_id, storage_id);
    if (new_id == 0) log_mtp_errors();
    pthread_mutex_unlock(&g_mtp);

    if (new_id == 0) { free(dup); return -EIO; }

    pthread_mutex_lock(&g_lock);
    p = resolve(parent);
    if (p && p->is_dir && p != g_root) {
        if (!node_find_child(p, name)) {
            mtp_node_t *n = node_new(name, 1, new_id, storage_id);
            if (n) node_add_child(p, n);
        }
    }
    pthread_mutex_unlock(&g_lock);
    free(dup);
    return 0;
}

int mtp_rmdir(const char *path)
{
    char norm_buf[PATH_MAX];
    const char *path_use = mtp_path_for_tree(path, norm_buf, sizeof norm_buf);
    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(path_use);
    if (!n)         { pthread_mutex_unlock(&g_lock); return -ENOENT; }
    if (!n->is_dir) { pthread_mutex_unlock(&g_lock); return -ENOTDIR; }
    if (n == g_root) { pthread_mutex_unlock(&g_lock); return -EBUSY; }
#if defined(__APPLE__)
    if (n->is_synth) {
        if (mtp_synth_is_static(n)) {
            pthread_mutex_unlock(&g_lock);
            return -EBUSY;
        }
        if (n->first_child) {
            pthread_mutex_unlock(&g_lock);
            return -ENOTEMPTY;
        }
        tree_detach(n);
        node_free(n);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
#endif
    uint32_t oid = n->object_id;
    pthread_mutex_unlock(&g_lock);

    int rc = mtp_delete_object_retry(oid);
    if (rc == -ENODEV)
        return -ENODEV;
    if (rc != 0)
        return -EIO;

    pthread_mutex_lock(&g_lock);
    n = resolve(path_use);
    if (n && n->parent) {
        mtp_node_t **pp = &n->parent->first_child;
        while (*pp && *pp != n) pp = &(*pp)->next_sibling;
        if (*pp) { *pp = n->next_sibling; n->next_sibling = NULL; node_free(n); }
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}
