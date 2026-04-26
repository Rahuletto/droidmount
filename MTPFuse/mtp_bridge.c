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
 * Phone apps / other PCs change MTP objects without notifying us.
 * mtp_readdir_snapshot() therefore drops the in-memory listing for that
 * directory before each readdir so Finder matches the device (MTP has no
 * push sync).
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

/* -1 = unknown, 0 = no, 1 = yes (LIBMTP_DEVICECAP_GetPartialObject). */
static int g_cap_partial_get = -1;

static void log_mtp_errors(void);

/* USB MTP stacks often flake with a single NAK; Finder maps hard I/O failure
 * to “The device disappeared.” Multi-level retries cover long full-file pulls. */
enum {
    MTP_USB_RETRIES = 10,
    /* Full download restarts after inner retries exhaust (big files often fail near EOF). */
    MTP_DOWNLOAD_OUTER_WAVES = 3,
};

static void mtp_usb_backoff(unsigned attempt_1based)
{
    if (attempt_1based == 0)
        return;
    /* 100ms … ~2.5s for late attempts (long USB stall / phone power step). */
    unsigned ms = 100u * (attempt_1based > 16u ? 16u : attempt_1based);
    if (attempt_1based > 8u)
        ms += 50u * (attempt_1based - 8u);
    if (ms > 2500u)
        ms = 2500u;
    usleep(ms * 1000u);
}

/* Returns 0, -ENODEV, or -1 (caller maps -1 → -EIO). Resets [fd] to empty + offset 0 between tries. */
static int mtp_get_file_to_fd_retry(uint32_t oid, int fd)
{
    int last = -1;
    for (unsigned k = 0; k < (unsigned)MTP_USB_RETRIES; k++) {
        if (k > 0) {
            mtp_usb_backoff((unsigned)k);
            if (ftruncate(fd, 0) < 0)
                return -errno;
            if (lseek(fd, 0, SEEK_SET) < 0)
                return -errno;
        }
        pthread_mutex_lock(&g_mtp);
        if (!g_device) {
            pthread_mutex_unlock(&g_mtp);
            return -ENODEV;
        }
        last = LIBMTP_Get_File_To_File_Descriptor(g_device, oid, fd, NULL, NULL);
        pthread_mutex_unlock(&g_mtp);
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
            if ((uint64_t)st.st_size > expect_sz) {
                if (ftruncate(fd, (off_t)expect_sz) < 0)
                    return -errno;
            }
            return 0;
        }
    }
    return -1;
}

#ifndef __APPLE__
/* Returns 0, -ENODEV, or -1 (EIO). Seeks fd to 0 before each attempt. */
static int mtp_send_file_from_fd_retry(LIBMTP_file_t *meta, int fd)
{
    int last = -1;
    for (unsigned k = 0; k < (unsigned)MTP_USB_RETRIES; k++) {
        if (k > 0)
            mtp_usb_backoff((unsigned)k);
        if (lseek(fd, 0, SEEK_SET) < 0)
            return -errno;
        pthread_mutex_lock(&g_mtp);
        if (!g_device) {
            pthread_mutex_unlock(&g_mtp);
            return -ENODEV;
        }
        last = LIBMTP_Send_File_From_File_Descriptor(g_device, fd, meta, NULL, NULL);
        pthread_mutex_unlock(&g_mtp);
        if (last == 0)
            return 0;
        log_mtp_errors();
    }
    (void)last;
    return -1;
}
#endif

#ifdef __APPLE__
static int mtp_send_file_from_named_path_retry(LIBMTP_file_t *meta, const char *path)
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
        last = LIBMTP_Send_File_From_File(g_device, path, meta, NULL, NULL);
        pthread_mutex_unlock(&g_mtp);
        if (last == 0)
            return 0;
        log_mtp_errors();
    }
    return -1;
}

/* Robustly copy exactly nbytes from src to dst at current offsets.
 * Returns 0 on success, -EIO on read/write error, -EINTR on signal. */
static int copy_fd_to_fd_safe(int src, int dst, size_t nbytes)
{
    unsigned char buf[256 * 1024];
    size_t remaining = nbytes;
    size_t total_copied = 0;
    
    while (remaining > 0) {
        size_t to_read = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
        ssize_t nread = read(src, buf, to_read);
        
        if (nread < 0) {
            int e = errno;
            mtp_debug_log("copy_fd_to_fd_safe: read error at offset %zu: %s",
                         total_copied, strerror(e));
            return -e;
        }
        if (nread == 0) {
            /* Unexpected EOF before nbytes; this is corruption. */
            mtp_debug_log("copy_fd_to_fd_safe: unexpected EOF at offset %zu (expected %zu total)",
                         total_copied, nbytes);
            return -EIO;
        }
        
        /* Write all bytes read. */
        size_t to_write = (size_t)nread;
        size_t written = 0;
        while (written < to_write) {
            ssize_t nwrite = write(dst, buf + written, to_write - written);
            if (nwrite < 0) {
                int e = errno;
                mtp_debug_log("copy_fd_to_fd_safe: write error at offset %zu: %s",
                             total_copied + written, strerror(e));
                return -e;
            }
            if (nwrite == 0) {
                /* write() returned 0; unusual but treat as error. */
                mtp_debug_log("copy_fd_to_fd_safe: write returned 0 at offset %zu",
                             total_copied + written);
                return -EIO;
            }
            written += (size_t)nwrite;
        }
        
        remaining -= (size_t)nread;
        total_copied += (size_t)nread;
    }
    
    mtp_debug_log("copy_fd_to_fd_safe: copied %zu bytes successfully", total_copied);
    return 0;
}
#endif

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

/* Refresh size + modification time from the device when the directory
 * listing left them unset (common for some stacks). Caller must hold g_lock;
 * briefly takes g_mtp (same nesting order as load_children). */
static void node_refresh_meta_if_stale_locked(mtp_node_t *n)
{
    /* Folders often have mtime==0 on MTP; Get_Filemetadata per folder during
     * readdir/stat was stalling listings and could leave Finder showing an
     * empty volume. Files still refresh when the listing left size/time unset.
     * Some stacks report size=0 in Get_Children metadata but a non-zero mtime;
     * Quick Look then shows "Zero KB" until we re-query by object id. */
    if (!n || n->is_synth)
        return;
    if (n->object_id == 0 || n->is_dir)
        return;
    if (n->mtime != 0 && n->size != 0u)
        return;
    pthread_mutex_lock(&g_mtp);
    if (!g_device) {
        pthread_mutex_unlock(&g_mtp);
        return;
    }
    LIBMTP_file_t *f = LIBMTP_Get_Filemetadata(g_device, n->object_id);
    pthread_mutex_unlock(&g_mtp);
    if (!f)
        return;
    n->size  = f->filesize;
    n->mtime = (uint64_t)f->modificationdate;
    LIBMTP_destroy_file_t(f);
}

/* ---------- helpers ---------- */

static mtp_node_t *node_new(const char *name, int is_dir,
                            uint32_t oid, uint32_t sid)
{
    mtp_node_t *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->name = strdup(name ? name : "");
    n->object_id = oid;
    n->storage_id = sid;
    n->is_dir = is_dir;
    return n;
}

static void node_free(mtp_node_t *n)
{
    if (!n) return;
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
    child->parent = parent;
    child->next_sibling = parent->first_child;
    parent->first_child = child;
}

static mtp_node_t *node_find_child(mtp_node_t *parent, const char *name)
{
    for (mtp_node_t *c = parent->first_child; c; c = c->next_sibling) {
        if (strcmp(c->name, name) == 0) return c;
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

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
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

/* Caller must hold g_lock for the whole call; do not drop it here.
 * (Dropping g_lock during MTP raced with readdir snapshots → corrupt names.) */
static void load_children(mtp_node_t *dir)
{
    if (!dir->is_dir || dir->children_loaded) return;

    double t0 = now_sec();

    if (dir == g_root) {
        mtp_debug_log("[LOAD] root start");
        fprintf(stderr, "[LOAD] root start\n");
        fflush(stderr);
        if (!g_device) {
            dir->children_loaded = 1;
            return;
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
            mtp_usb_backoff((unsigned)(attempt + 1u));
        }
#if defined(__APPLE__)
        mtp_add_root_volume_icon_locked(dir);
#endif
        dir->children_loaded = 1;
        mtp_debug_log("[LOAD] root done %.3fs", now_sec() - t0);
        fprintf(stderr, "[LOAD] root done %.3fs\n", now_sec() - t0);
        fflush(stderr);
        return;
    }

    uint32_t sid = dir->storage_id;
    uint32_t pid = mtp_parent_handle(dir);
    const char *dbg = dir->name ? dir->name : "?";

    mtp_debug_log("[LOAD] \"%s\" sid=%u parent_id=0x%x → Get_Children + per-object metadata",
                  dbg, sid, pid);
    fprintf(stderr, "[LOAD] \"%s\" sid=%u parent_id=0x%x (MTP listing)…\n",
            dbg, sid, pid);
    fflush(stderr);

    uint32_t *ids = NULL;
    int nkids = -1;
    for (unsigned k = 0; k < (unsigned)MTP_USB_RETRIES; k++) {
        if (k > 0) {
            mtp_usb_backoff((unsigned)k);
            free(ids);
            ids = NULL;
        }
        pthread_mutex_lock(&g_mtp);
        if (!g_device) {
            pthread_mutex_unlock(&g_mtp);
            free(ids);
            if (!dir->children_loaded)
                dir->children_loaded = 1;
            return;
        }
        nkids = LIBMTP_Get_Children(g_device, sid, pid, &ids);
        pthread_mutex_unlock(&g_mtp);
        if (nkids >= 0)
            break;
        log_mtp_errors();
    }

    mtp_debug_log("[LOAD] \"%s\" Get_Children → n=%d elapsed %.3fs",
                  dbg, nkids, now_sec() - t0);

    if (nkids < 0) {
        free(ids);
        if (!dir->children_loaded)
            dir->children_loaded = 1;
        fprintf(stderr, "[LOAD] \"%s\" Get_Children failed\n", dbg);
        fflush(stderr);
        return;
    }

    if (nkids == 0) {
#if defined(__APPLE__)
        if (dir->parent == g_root)
            mtp_synth_attach_volume_meta(dir);
#endif
        if (!dir->children_loaded)
            dir->children_loaded = 1;
        mtp_debug_log("[LOAD] \"%s\" empty folder (0 handles)", dbg);
        fprintf(stderr, "[LOAD] \"%s\" done: 0 entries\n", dbg);
        fflush(stderr);
        return;
    }

    int count = 0;
    for (int i = 0; i < nkids; i++) {
        pthread_mutex_lock(&g_mtp);
        if (!g_device) {
            pthread_mutex_unlock(&g_mtp);
            free(ids);
            if (!dir->children_loaded)
                dir->children_loaded = 1;
            mtp_debug_log("[LOAD] \"%s\" aborted mid-list (device gone)", dbg);
            return;
        }
        LIBMTP_file_t *file = LIBMTP_Get_Filemetadata(g_device, ids[i]);
        pthread_mutex_unlock(&g_mtp);

        if (dir->children_loaded) {
            if (file)
                LIBMTP_destroy_file_t(file);
            free(ids);
            mtp_debug_log("[LOAD] \"%s\" superseded after %d/%d metadata fetches",
                          dbg, i, nkids);
            return;
        }
        if (file) {
            int is_dir = (file->filetype == LIBMTP_FILETYPE_FOLDER);
            if (file->filename && file->filename[0] &&
                !node_find_child(dir, file->filename)) {
                mtp_node_t *n = node_new(file->filename, is_dir,
                                         file->item_id, file->storage_id);
                if (n) {
                    n->size  = file->filesize;
                    n->mtime = file->modificationdate;
                    node_add_child(dir, n);
                    count++;
                    if ((count % 200) == 0) {
                        mtp_debug_log("[LOAD] \"%s\" … %d entries (%.3fs)",
                                      dbg, count, now_sec() - t0);
                        fprintf(stderr, "[LOAD] \"%s\" … %d entries so far\n", dbg, count);
                        fflush(stderr);
                    }
                }
            }
            LIBMTP_destroy_file_t(file);
        }
    }

    free(ids);

#if defined(__APPLE__)
    if (dir->parent == g_root)
        mtp_synth_attach_volume_meta(dir);
#endif
    if (!dir->children_loaded)
        dir->children_loaded = 1;

    mtp_debug_log("[LOAD] \"%s\" done: %d entries in %.3fs", dbg, count, now_sec() - t0);
    fprintf(stderr, "[LOAD] \"%s\" done: %d entries in %.3fs\n",
            dbg, count, now_sec() - t0);
    fflush(stderr);
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
    if (!path || path[0] != '/') return NULL;
    if (path[1] == '\0') return g_root;

    mtp_node_t *cur = g_root;
    load_children(cur);

    char *dup = strdup(path + 1);
    if (!dup) return NULL;

    char *save = NULL;
    for (char *tok = strtok_r(dup, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
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
    setvbuf(stderr, NULL, _IOLBF, 0);
    mtp_debug_log("mtp_open: LIBMTP_Init");
    fprintf(stderr, "mtp_open: LIBMTP_Init...\n"); fflush(stderr);
    LIBMTP_Init();
    mtp_debug_log("mtp_open: Detect_Raw_Devices");
    fprintf(stderr, "mtp_open: detecting raw devices...\n"); fflush(stderr);

    /* Use the more explicit detection API so we can give better
     * diagnostics than Get_First_Device() (which silently swallows
     * everything and returns NULL on any error). */
    LIBMTP_raw_device_t *raw = NULL;
    int n_raw = 0;
    LIBMTP_error_number_t err = LIBMTP_Detect_Raw_Devices(&raw, &n_raw);
    mtp_debug_log("mtp_open: detect err=%d n_raw=%d", (int)err, n_raw);
    fprintf(stderr, "mtp_open: detect returned err=%d, n=%d\n", err, n_raw); fflush(stderr);
    if (err != LIBMTP_ERROR_NONE || n_raw == 0) {
        fprintf(stderr, "mtp_open: no MTP device found "
                        "(is the phone unlocked and in File Transfer mode?)\n");
        free(raw);
        return -1;
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
        mtp_debug_log("mtp_open: Open_Raw_Device failed");
        fprintf(stderr, "mtp_open: LIBMTP_Open_Raw_Device failed\n");
        return -1;
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

    /* Force-update storage list so we can enumerate volumes. */
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
    return 0;
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

void mtp_close(void)
{
    mtp_debug_log("mtp_close: begin (release tree + device)");
    pthread_mutex_lock(&g_lock);
#if defined(__APPLE__)
    g_volicon_src[0] = '\0';
    mtp_synth_free_user_branches();
#endif
    pthread_mutex_lock(&g_mtp);
    if (g_root)   { node_free(g_root); g_root = NULL; }
    if (g_device) { LIBMTP_Release_Device(g_device); g_device = NULL; }
    g_cap_partial_get = -1;
    pthread_mutex_unlock(&g_mtp);
    pthread_mutex_unlock(&g_lock);
    free(g_fuse_mount_host_abs);
    g_fuse_mount_host_abs = NULL;
    mtp_debug_shutdown();
}

int mtp_refresh_tree(void)
{
    pthread_mutex_lock(&g_lock);
#if defined(__APPLE__)
    mtp_synth_free_user_branches();
#endif
    if (g_root) { node_free(g_root); g_root = NULL; }
    g_root = node_new("", 1, 0, 0);
    int ok = g_root != NULL;
    pthread_mutex_unlock(&g_lock);
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

    pthread_mutex_lock(&g_mtp);
    if (!g_device) {
        pthread_mutex_unlock(&g_mtp);
        return have_node ? -ENODEV : -ENOENT;
    }
    LIBMTP_Get_Storage(g_device, LIBMTP_STORAGE_SORTBY_NOTSORTED);

    uint64_t tot = 0, fr = 0;
    if (want_sid == 0u) {
        for (LIBMTP_devicestorage_t *s = g_device->storage; s; s = s->next) {
            tot += s->MaxCapacity;
            fr += s->FreeSpaceInBytes;
        }
    } else {
        int found = 0;
        for (LIBMTP_devicestorage_t *s = g_device->storage; s; s = s->next) {
            if (s->id == want_sid) {
                tot = s->MaxCapacity;
                fr = s->FreeSpaceInBytes;
                found = 1;
                break;
            }
        }
        if (!found) {
            pthread_mutex_unlock(&g_mtp);
            return -ENOENT;
        }
    }
    pthread_mutex_unlock(&g_mtp);

    if (fr > tot)
        fr = tot;
    *total_bytes = tot;
    *free_bytes = fr;
    return 0;
}

int mtp_stat(const char *path, mtp_stat_t *out)
{
    if (!out) return -EINVAL;
    double t0 = now_sec();
    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(path);
    int rc = -ENOENT;
    if (n) {
        node_refresh_meta_if_stale_locked(n);
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

    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(path);
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
    /* Refetch from device: cache was authoritative until the phone changed something. */
    dir_drop_children_locked(n);
    mtp_debug_log("readdir_snapshot load_children path=%s (fresh MTP listing)", path);
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

    /* PTP max chunk is uint32; align with mount iosize=1MiB for fewer round-trips. */
    const uint32_t chunk_max = 1048576u;
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
    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(path);
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
    uint32_t oid = n->object_id;
    uint64_t total = n->size;
    pthread_mutex_unlock(&g_lock);

    if ((uint64_t)offset >= total) return 0;
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
    char tmpl[] = "/tmp/mtpfuse_readXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -errno;
    unlink(tmpl);

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
    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(path);
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
    uint32_t oid = n->object_id;
    node_refresh_meta_if_stale_locked(n);
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

/* UNKNOWN works on many phones; some stacks mis-index video until format is set. */
static LIBMTP_filetype_t guess_filetype_from_basename(const char *base)
{
    const char *dot = strrchr(base, '.');
    if (!dot || dot[1] == '\0')
        return LIBMTP_FILETYPE_UNKNOWN;
    const char *e = dot + 1;
    if (strcasecmp(e, "mp4") == 0 || strcasecmp(e, "m4v") == 0)
        return LIBMTP_FILETYPE_MP4;
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

/* Send file bytes from fd (at current offset 0, length size) to MTP at path.
 * Returns 0 on success, -errno on error. On macOS, creates a temporary snapshot
 * file to work around libmtp fd-based send limitations. Automatically deletes
 * existing files before creating new ones to ensure clean overwrites. */
static int mtp_write_send_fd(const char *path, int fd, size_t size)
{
    char *path_dup = strdup(path);
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
    if (existing && !existing->is_dir) {
        replace_oid = existing->object_id;
        mtp_debug_log("mtp_write_send_fd: replacing existing file object_id=%u", replace_oid);
    }

    pthread_mutex_unlock(&g_lock);

    /* Delete existing file if necessary. */
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
        /* Device stacks need time to process deletion. */
        usleep(500000);
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

    /* Create libmtp file metadata. */
    LIBMTP_file_t *meta = LIBMTP_new_file_t();
    if (!meta) {
        free(path_dup);
        return -ENOMEM;
    }
    meta->filename = strdup(basename);
    if (!meta->filename) {
        LIBMTP_destroy_file_t(meta);
        free(path_dup);
        return -ENOMEM;
    }
    meta->parent_id = parent_id;
    meta->storage_id = storage_id;
    meta->filesize = (uint64_t)size;
    meta->filetype = guess_filetype_from_basename(basename);
    meta->modificationdate = time(NULL);

#ifdef __APPLE__
    /* macOS workaround: libmtp Send_File_From_File_Descriptor on unlinked
     * temp fd often fails mid-transfer ("device disappeared"). Use a named
     * temp file with Send_File_From_File instead. Copy fd to temp snapshot,
     * fsync, and send from that path. */

    char snap_path[] = "/tmp/mtpfuse_upXXXXXX";
    int snapfd = mkstemp(snap_path);
    if (snapfd < 0) {
        int e = errno;
        LIBMTP_destroy_file_t(meta);
        free(path_dup);
        return -e;
    }

    /* Copy staging fd to snapshot file. */
    int copy_rc = copy_fd_to_fd_safe(fd, snapfd, size);
    if (copy_rc != 0) {
        close(snapfd);
        unlink(snap_path);
        LIBMTP_destroy_file_t(meta);
        free(path_dup);
        mtp_debug_log("mtp_write_send_fd: copy_fd_to_fd_safe failed: %d", copy_rc);
        return copy_rc;
    }

    struct stat snapst;
    if (fstat(snapfd, &snapst) != 0) {
        int e = errno;
        close(snapfd);
        unlink(snap_path);
        LIBMTP_destroy_file_t(meta);
        free(path_dup);
        mtp_debug_log("mtp_write_send_fd: fstat(snap) failed: %s", strerror(e));
        return -e;
    }
    if ((uint64_t)snapst.st_size != (uint64_t)size) {
        close(snapfd);
        unlink(snap_path);
        LIBMTP_destroy_file_t(meta);
        free(path_dup);
        mtp_debug_log("mtp_write_send_fd: snapshot size mismatch (got %llu want %zu)",
                      (unsigned long long)snapst.st_size, size);
        return -EIO;
    }

    /* Flush snapshot to disk before sending. */
    if (fsync(snapfd) < 0) {
        int e = errno;
        close(snapfd);
        unlink(snap_path);
        LIBMTP_destroy_file_t(meta);
        free(path_dup);
        mtp_debug_log("mtp_write_send_fd: fsync(snapfd) failed: %s", strerror(e));
        return -e;
    }
    close(snapfd);

    mtp_debug_log("mtp_write_send_fd: sending via named path %s (size=%zu)", snap_path, size);
    int send_rc = mtp_send_file_from_named_path_retry(meta, snap_path);
    mtp_debug_log("mtp_write_send_fd: send_rc=%d", send_rc);
    unlink(snap_path);

#else  /* Linux */
    mtp_debug_log("mtp_write_send_fd: sending via fd (size=%zu)", size);
    int send_rc = mtp_send_file_from_fd_retry(meta, fd);
    mtp_debug_log("mtp_write_send_fd: send_rc=%d", send_rc);
#endif

    /* Check send result. */
    if (send_rc == -ENODEV) {
        LIBMTP_destroy_file_t(meta);
        free(path_dup);
        return -ENODEV;
    }
    if (send_rc != 0) {
        mtp_debug_log("mtp_write_send_fd: send failed with rc=%d", send_rc);
        LIBMTP_destroy_file_t(meta);
        free(path_dup);
        return -EIO;
    }

    /* libmtp fills item_id on success; confirm device-reported size (truncated USB
     * uploads otherwise look like “bad MP4” on Mac + phone). */
    if (meta->item_id != 0u) {
        pthread_mutex_lock(&g_mtp);
        LIBMTP_file_t *chk = g_device ? LIBMTP_Get_Filemetadata(g_device, meta->item_id) : NULL;
        pthread_mutex_unlock(&g_mtp);
        if (!chk) {
            mtp_debug_log("mtp_write_send_fd: post-send verify missing metadata oid=%u",
                          meta->item_id);
            mtp_delete_object_retry(meta->item_id);
            LIBMTP_destroy_file_t(meta);
            free(path_dup);
            return -EIO;
        }
        if ((uint64_t)chk->filesize != (uint64_t)size) {
            mtp_debug_log("mtp_write_send_fd: post-send SIZE oid=%u device=%llu sent=%zu (scrub)",
                          meta->item_id, (unsigned long long)chk->filesize, size);
            LIBMTP_destroy_file_t(chk);
            mtp_delete_object_retry(meta->item_id);
            LIBMTP_destroy_file_t(meta);
            free(path_dup);
            return -EIO;
        }
        LIBMTP_destroy_file_t(chk);
    } else {
        mtp_debug_log("mtp_write_send_fd: post-send verify skipped (item_id==0)");
    }

    /* Invalidate parent's child list to force refresh on next readdir. */
    pthread_mutex_lock(&g_lock);
    mtp_node_t *parent_now = resolve(parent_path);
    if (parent_now && parent_now->children_loaded) {
        mtp_node_t *child = parent_now->first_child;
        parent_now->first_child = NULL;
        while (child) {
            mtp_node_t *next = child->next_sibling;
            node_free(child);
            child = next;
        }
        parent_now->children_loaded = 0;
        mtp_debug_log("mtp_write_send_fd: invalidated parent child cache");
    }
    pthread_mutex_unlock(&g_lock);

    LIBMTP_destroy_file_t(meta);
    free(path_dup);
    mtp_debug_log("mtp_write_send_fd: success");
    return 0;
}

int mtp_write_full_fd(const char *path, int fd, size_t size)
{
    int rc = mtp_write_send_fd(path, fd, size);
    return (rc == 0) ? (int)size : rc;
}

int mtp_write_full(const char *path, const char *buf, size_t size)
{
    char tmpl[] = "/tmp/mtpfuse_wrXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -errno;
    unlink(tmpl);
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

    char *fpath = strdup(from);
    char *tpath = strdup(to);
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
    mtp_node_t *src = resolve(from);
    if (!src) {
        pthread_mutex_unlock(&g_lock);
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

    mtp_node_t *to_existing = resolve(to);
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
    uint32_t new_storage_id = dst_parent->storage_id;
    uint32_t src_oid = src->object_id;
    uint32_t src_sid = src->storage_id;
    if (src_sid == 0u) {
        for (mtp_node_t *w = src->parent; w; w = w->parent) {
            if (w->storage_id != 0u) {
                src_sid = w->storage_id;
                break;
            }
        }
    }
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

    pthread_mutex_lock(&g_mtp);
    if (!g_device) {
        pthread_mutex_unlock(&g_mtp);
        free(fpath);
        free(tpath);
        mtp_refresh_tree();
        return -ENODEV;
    }
    int rc;
    if (same_parent) {
        rc = LIBMTP_Set_Object_Filename(g_device, src_oid, (char *)tname);
    } else {
        /* libmtp: Move_Object(device, object_id, storage_id, parent_id) — not
         * (object_id, parent_id, storage_id). Wrong order produced EIO on mv. */
        rc = LIBMTP_Move_Object(g_device, src_oid, new_storage_id, new_parent_id);
        if (rc == 0)
            rc = LIBMTP_Set_Object_Filename(g_device, src_oid, (char *)tname);
    }
    if (rc != 0) log_mtp_errors();
    pthread_mutex_unlock(&g_mtp);

    if (rc != 0) {
        free(fpath);
        free(tpath);
        /* Move failed - return EXDEV so Finder falls back to copy+delete */
        return -EXDEV;
    }

    if (cross_dir_move) {
        free(fpath);
        free(tpath);
        mtp_refresh_tree();
        return 0;
    }

    char *tname_copy = strdup(tname);
    if (!tname_copy) {
        free(fpath);
        free(tpath);
        return -ENOMEM;
    }

    pthread_mutex_lock(&g_lock);
    src = resolve(from);
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
    pthread_mutex_unlock(&g_lock);

    free(fpath);
    free(tpath);
    return 0;
}

int mtp_unlink(const char *path)
{
    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(path);
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
    n = resolve(path);
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
    char *dup = strdup(path);
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
    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(path);
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
    n = resolve(path);
    if (n && n->parent) {
        mtp_node_t **pp = &n->parent->first_child;
        while (*pp && *pp != n) pp = &(*pp)->next_sibling;
        if (*pp) { *pp = n->next_sibling; n->next_sibling = NULL; node_free(n); }
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}
