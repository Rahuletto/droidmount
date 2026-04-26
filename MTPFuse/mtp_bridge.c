/*
 * mtp_bridge.c — thin wrapper around libmtp for the FUSE layer.
 *
 * Caches an in-memory tree of (path -> object id) so that FUSE can do
 * path-based operations on top of libmtp's id-based world.
 *
 * Loading is lazy and one MTP level at a time (BFS-by-navigation):
 * resolve() walks path components and calls load_children() only on
 * each ancestor — never recursively prefetches deeper folders.
 */

#define _DARWIN_C_SOURCE 1
#include "mtp_bridge.h"

#include <libmtp.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ----- debug trace (/tmp/mtpfuse-<pid>.log) ----- */

static FILE            *g_dbg      = NULL;
static pthread_mutex_t  g_dbg_mu  = PTHREAD_MUTEX_INITIALIZER;
static char             g_dbg_path[512];

static void dbg_prefix(FILE *f)
{
    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    fprintf(f, "[%lld.%03ld thr=%lx] ",
            (long long)mono.tv_sec, mono.tv_nsec / 1000000L,
            (unsigned long)pthread_self());
}

void mtp_debug_boot(int argc, char **argv)
{
    const char *dis = getenv("MTPFUSE_DEBUG");
    if (dis && dis[0] == '0' && dis[1] == '\0')
        return;

    const char *custom = getenv("MTPFUSE_DEBUG_LOG");
    if (custom && custom[0])
        snprintf(g_dbg_path, sizeof g_dbg_path, "%s", custom);
    else
        snprintf(g_dbg_path, sizeof g_dbg_path, "/tmp/mtpfuse-%d.log",
                 (int)getpid());

    pthread_mutex_lock(&g_dbg_mu);
    g_dbg = fopen(g_dbg_path, "w");
    if (!g_dbg) {
        pthread_mutex_unlock(&g_dbg_mu);
        return;
    }
    setvbuf(g_dbg, NULL, _IOLBF, 0);
    dbg_prefix(g_dbg);
    fprintf(g_dbg, "=== mtpfuse boot pid=%d ===\n", (int)getpid());
    for (int i = 0; i < argc; i++)
        fprintf(g_dbg, "  argv[%d] %s\n", i, argv[i]);
    const char *mp = "?";
    for (int i = argc - 1; i >= 1; i--) {
        if (argv[i][0] != '-') {
            mp = argv[i];
            break;
        }
    }
    fprintf(g_dbg, "  inferred mountpoint: %s\n", mp);
    fprintf(g_dbg, "  (disable file log: MTPFUSE_DEBUG=0)\n");
    fflush(g_dbg);
    pthread_mutex_unlock(&g_dbg_mu);

    unlink("/tmp/mtpfuse-debug-latest.log");
    if (symlink(g_dbg_path, "/tmp/mtpfuse-debug-latest.log") != 0) { /* ignore */ }

    fprintf(stderr, "mtpfuse: debug log %s  (stable path: /tmp/mtpfuse-debug-latest.log)\n",
            g_dbg_path);
    fflush(stderr);
}

void mtp_debug_log(const char *fmt, ...)
{
    if (!g_dbg)
        return;
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_dbg_mu);
    dbg_prefix(g_dbg);
    vfprintf(g_dbg, fmt, ap);
    fprintf(g_dbg, "\n");
    fflush(g_dbg);
    pthread_mutex_unlock(&g_dbg_mu);
    va_end(ap);
}

void mtp_debug_shutdown(void)
{
    if (!g_dbg)
        return;
    pthread_mutex_lock(&g_dbg_mu);
    dbg_prefix(g_dbg);
    fprintf(g_dbg, "=== mtpfuse session end ===\n");
    fflush(g_dbg);
    fclose(g_dbg);
    g_dbg = NULL;
    pthread_mutex_unlock(&g_dbg_mu);
}

/* ---------------- node tree ---------------- */

typedef struct mtp_node {
    char            *name;          /* basename, never NULL */
    uint32_t         object_id;     /* 0 for storage root nodes */
    uint32_t         storage_id;
    int              is_dir;
    uint64_t         size;
    uint64_t         mtime;
    int              children_loaded;
    struct mtp_node *parent;
    struct mtp_node *first_child;
    struct mtp_node *next_sibling;
} mtp_node_t;

static LIBMTP_mtpdevice_t *g_device = NULL;
static mtp_node_t         *g_root   = NULL;
static pthread_mutex_t     g_lock   = PTHREAD_MUTEX_INITIALIZER;
/* Serialize all libmtp USB I/O (library is not thread-safe; also avoids
 * holding g_lock across slow transfers — Finder can getattr other paths
 * while a large folder lists). */
static pthread_mutex_t     g_mtp    = PTHREAD_MUTEX_INITIALIZER;

static void log_mtp_errors(void);

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
        for (LIBMTP_devicestorage_t *s = g_device->storage; s; s = s->next) {
            const char *nm = s->StorageDescription
                                ? s->StorageDescription : "Storage";
            if (node_find_child(dir, nm)) continue;
            mtp_node_t *n = node_new(nm, 1, 0, s->id);
            if (n) node_add_child(dir, n);
        }
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

    pthread_mutex_lock(&g_mtp);
    uint32_t *ids = NULL;
    int nkids = LIBMTP_Get_Children(g_device, sid, pid, &ids);
    pthread_mutex_unlock(&g_mtp);

    mtp_debug_log("[LOAD] \"%s\" Get_Children → n=%d elapsed %.3fs",
                  dbg, nkids, now_sec() - t0);

    if (nkids < 0) {
        pthread_mutex_lock(&g_mtp);
        log_mtp_errors();
        pthread_mutex_unlock(&g_mtp);
        if (!dir->children_loaded)
            dir->children_loaded = 1;
        fprintf(stderr, "[LOAD] \"%s\" Get_Children failed\n", dbg);
        fflush(stderr);
        return;
    }

    if (nkids == 0) {
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

    if (!dir->children_loaded)
        dir->children_loaded = 1;

    mtp_debug_log("[LOAD] \"%s\" done: %d entries in %.3fs", dbg, count, now_sec() - t0);
    fprintf(stderr, "[LOAD] \"%s\" done: %d entries in %.3fs\n",
            dbg, count, now_sec() - t0);
    fflush(stderr);
}

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
        if (!child) { free(dup); return NULL; }
        cur = child;
    }
    free(dup);
    return cur;
}

static void log_mtp_errors(void)
{
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
    mtp_debug_log("mtp_open: success g_root=%p", (void *)g_root);
    return 0;
}

void mtp_close(void)
{
    mtp_debug_log("mtp_close: begin (release tree + device)");
    pthread_mutex_lock(&g_mtp);
    pthread_mutex_lock(&g_lock);
    if (g_root)   { node_free(g_root); g_root = NULL; }
    if (g_device) { LIBMTP_Release_Device(g_device); g_device = NULL; }
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_mtp);
    mtp_debug_shutdown();
}

int mtp_refresh_tree(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_root) { node_free(g_root); g_root = NULL; }
    g_root = node_new("", 1, 0, 0);
    pthread_mutex_unlock(&g_lock);
    return g_root ? 0 : -1;
}

int mtp_stat(const char *path, mtp_stat_t *out)
{
    if (!out) return -EINVAL;
    double t0 = now_sec();
    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(path);
    int rc = -ENOENT;
    if (n) {
        out->is_dir = n->is_dir;
        out->size   = n->size;
        out->mtime  = n->mtime;
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
    mtp_debug_log("readdir_snapshot pre-load_children path=%s children_loaded=%d",
                  path, n->children_loaded);
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
        arr[i].name = strdup(c->name);
        arr[i].is_dir = c->is_dir;
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

int mtp_read(const char *path, char *buf, size_t size, off_t offset)
{
    double t0 = now_sec();
    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(path);
    if (!n)        { pthread_mutex_unlock(&g_lock); return -ENOENT; }
    if (n->is_dir) { pthread_mutex_unlock(&g_lock); return -EISDIR; }
    uint32_t oid = n->object_id;
    uint64_t total = n->size;
    pthread_mutex_unlock(&g_lock);

    if ((uint64_t)offset >= total) return 0;
    if (offset + size > total) size = (size_t)(total - offset);

    mtp_debug_log("mtp_read begin path=%s oid=%u off=%lld sz=%zu file_sz=%llu",
                  path, oid, (long long)offset, size, (unsigned long long)total);

    /* libmtp lacks a robust ranged read across all stacks; pipe the file
     * into a temp fd and pread() from it. For typical Finder previews
     * this is fine; for huge files macOS will mostly ask for sequential
     * chunks anyway. */
    char tmpl[] = "/tmp/mtpfuse_readXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -errno;
    unlink(tmpl);

    pthread_mutex_lock(&g_mtp);
    int rc = LIBMTP_Get_File_To_File_Descriptor(
        g_device, oid, fd, NULL, NULL);
    if (rc != 0) log_mtp_errors();
    pthread_mutex_unlock(&g_mtp);

    if (rc != 0) {
        mtp_debug_log("mtp_read Get_File_To_FD FAIL path=%s rc=%d dt=%.3fs",
                      path, rc, now_sec() - t0);
        close(fd);
        return -EIO;
    }

    ssize_t got = pread(fd, buf, size, offset);
    int err = (got < 0) ? -errno : (int)got;
    close(fd);
    mtp_debug_log("mtp_read end path=%s got=%d dt=%.3fs (full file pulled from device each call)",
                  path, err, now_sec() - t0);
    return err;
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
    uint32_t oid = n->object_id;
    pthread_mutex_unlock(&g_lock);

    if (ftruncate(fd, 0) < 0)
        return -errno;
    if (lseek(fd, 0, SEEK_SET) < 0)
        return -errno;

    mtp_debug_log("mtp_download_to_fd path=%s oid=%u", path, oid);
    pthread_mutex_lock(&g_mtp);
    int rc = LIBMTP_Get_File_To_File_Descriptor(
        g_device, oid, fd, NULL, NULL);
    if (rc != 0)
        log_mtp_errors();
    pthread_mutex_unlock(&g_mtp);
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

/* Send file bytes from fd (offset 0, length size) to MTP at path. */
static int mtp_write_send_fd(const char *path, int fd, size_t size)
{
    char *dup = strdup(path);
    if (!dup) return -ENOMEM;
    char *slash = strrchr(dup, '/');
    if (!slash) { free(dup); return -EINVAL; }
    *slash = '\0';
    const char *parent = (dup[0] == '\0') ? "/" : dup;
    const char *name   = slash + 1;
    if (!*name) { free(dup); return -EINVAL; }

    pthread_mutex_lock(&g_lock);
    mtp_node_t *p = resolve(parent);
    if (!p || !p->is_dir) {
        pthread_mutex_unlock(&g_lock); free(dup); return -ENOENT;
    }
    uint32_t storage_id = p->storage_id;
    uint32_t parent_id  = mtp_parent_handle(p);
    if (p == g_root) {
        pthread_mutex_unlock(&g_lock); free(dup); return -EACCES;
    }

    uint32_t replace_oid = 0;
    mtp_node_t *existing = node_find_child(p, name);
    if (existing && !existing->is_dir)
        replace_oid = existing->object_id;
    pthread_mutex_unlock(&g_lock);

    if (replace_oid) {
        pthread_mutex_lock(&g_mtp);
        LIBMTP_Delete_Object(g_device, replace_oid);
        pthread_mutex_unlock(&g_mtp);
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        free(dup);
        return -errno;
    }

    LIBMTP_file_t *meta = LIBMTP_new_file_t();
    meta->filename   = strdup(name);
    meta->parent_id  = parent_id;
    meta->storage_id = storage_id;
    meta->filesize   = size;
    meta->filetype   = LIBMTP_FILETYPE_UNKNOWN;

    pthread_mutex_lock(&g_mtp);
    int rc = LIBMTP_Send_File_From_File_Descriptor(
        g_device, fd, meta, NULL, NULL);
    if (rc != 0) log_mtp_errors();
    pthread_mutex_unlock(&g_mtp);

    pthread_mutex_lock(&g_lock);
    p = resolve(parent);
    if (p && rc == 0 && p->children_loaded) {
        mtp_node_t *c = p->first_child; p->first_child = NULL;
        while (c) { mtp_node_t *nx = c->next_sibling; node_free(c); c = nx; }
        p->children_loaded = 0;
    }
    pthread_mutex_unlock(&g_lock);

    LIBMTP_destroy_file_t(meta);
    free(dup);
    return (rc == 0) ? 0 : -EIO;
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
        return -EACCES;
    }

    mtp_node_t *dst_parent = resolve(tparent_s);
    if (!dst_parent || !dst_parent->is_dir || dst_parent == g_root) {
        pthread_mutex_unlock(&g_lock);
        free(fpath);
        free(tpath);
        return dst_parent ? -EACCES : -ENOENT;
    }

    mtp_node_t *to_existing = resolve(to);
    if (to_existing) {
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
    int same_parent = (src_parent == dst_parent);
    int cross_dir_move = (!same_parent && src->is_dir);

    pthread_mutex_unlock(&g_lock);

    if (orphan_target) {
        pthread_mutex_lock(&g_mtp);
        int dr = LIBMTP_Delete_Object(g_device, orphan_target->object_id);
        if (dr != 0) log_mtp_errors();
        pthread_mutex_unlock(&g_mtp);
        pthread_mutex_lock(&g_lock);
        node_free(orphan_target);
        pthread_mutex_unlock(&g_lock);
        if (dr != 0) {
            free(fpath);
            free(tpath);
            return -EIO;
        }
    }

    pthread_mutex_lock(&g_mtp);
    int rc;
    if (same_parent) {
        rc = LIBMTP_Set_Object_Filename(g_device, src_oid, (char *)tname);
    } else {
        rc = LIBMTP_Move_Object(g_device, src_oid, new_parent_id, new_storage_id);
        if (rc == 0)
            rc = LIBMTP_Set_Object_Filename(g_device, src_oid, (char *)tname);
    }
    if (rc != 0) log_mtp_errors();
    pthread_mutex_unlock(&g_mtp);

    if (rc != 0) {
        free(fpath);
        free(tpath);
        return -EIO;
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
    uint32_t oid = n->object_id;
    pthread_mutex_unlock(&g_lock);

    pthread_mutex_lock(&g_mtp);
    int rc = LIBMTP_Delete_Object(g_device, oid);
    if (rc != 0) log_mtp_errors();
    pthread_mutex_unlock(&g_mtp);

    if (rc != 0) return -EIO;

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
    if (!p || !p->is_dir || p == g_root) {
        pthread_mutex_unlock(&g_lock); free(dup); return -EACCES;
    }
    uint32_t storage_id = p->storage_id;
    uint32_t parent_id  = mtp_parent_handle(p);
    pthread_mutex_unlock(&g_lock);

    pthread_mutex_lock(&g_mtp);
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
    if (n == g_root){ pthread_mutex_unlock(&g_lock); return -EACCES; }
    uint32_t oid = n->object_id;
    pthread_mutex_unlock(&g_lock);

    pthread_mutex_lock(&g_mtp);
    int rc = LIBMTP_Delete_Object(g_device, oid);
    if (rc != 0) log_mtp_errors();
    pthread_mutex_unlock(&g_mtp);

    if (rc != 0) return -EIO;

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
