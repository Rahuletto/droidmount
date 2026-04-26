/*
 * mtp_bridge.c — thin wrapper around libmtp for the FUSE layer.
 *
 * Caches an in-memory tree of (path -> object id) so that FUSE can do
 * path-based operations on top of libmtp's id-based world. Folders are
 * populated lazily the first time they are listed.
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
#include <sys/stat.h>
#include <unistd.h>

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

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void load_children(mtp_node_t *dir)
{
    if (!dir->is_dir || dir->children_loaded) return;
    dir->children_loaded = 1;

    double t0 = now_sec();

    /* Storage roots have object_id == 0 and a real storage_id.
     * For nested folders parent_id == dir->object_id. */
    uint32_t parent_id = (dir == g_root) ? 0 : dir->object_id;

    if (dir == g_root) {
        /* children of root == storages */
        for (LIBMTP_devicestorage_t *s = g_device->storage; s; s = s->next) {
            const char *nm = s->StorageDescription
                                ? s->StorageDescription : "Storage";
            if (node_find_child(dir, nm)) continue;
            mtp_node_t *n = node_new(nm, 1, 0, s->id);
            if (n) node_add_child(dir, n);
        }
        return;
    }

    LIBMTP_file_t *files = LIBMTP_Get_Files_And_Folders(
        g_device, dir->storage_id, parent_id);
    while (files) {
        LIBMTP_file_t *nx = files->next;
        int is_dir = (files->filetype == LIBMTP_FILETYPE_FOLDER);
        if (files->filename && !node_find_child(dir, files->filename)) {
            mtp_node_t *n = node_new(files->filename, is_dir,
                                     files->item_id, files->storage_id);
            if (n) {
                n->size  = files->filesize;
                n->mtime = files->modificationdate;
                node_add_child(dir, n);
            }
        }
        LIBMTP_destroy_file_t(files);
        files = nx;
    }
}

/* Resolve "/a/b/c" → node, lazy-loading directories along the way.
 * Returns NULL if any component does not exist. */
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

/* ---------- public api ---------- */

int mtp_open(void)
{
    setvbuf(stderr, NULL, _IOLBF, 0);
    fprintf(stderr, "mtp_open: LIBMTP_Init...\n"); fflush(stderr);
    LIBMTP_Init();
    fprintf(stderr, "mtp_open: detecting raw devices...\n"); fflush(stderr);

    /* Use the more explicit detection API so we can give better
     * diagnostics than Get_First_Device() (which silently swallows
     * everything and returns NULL on any error). */
    LIBMTP_raw_device_t *raw = NULL;
    int n_raw = 0;
    LIBMTP_error_number_t err = LIBMTP_Detect_Raw_Devices(&raw, &n_raw);
    fprintf(stderr, "mtp_open: detect returned err=%d, n=%d\n", err, n_raw); fflush(stderr);
    if (err != LIBMTP_ERROR_NONE || n_raw == 0) {
        fprintf(stderr, "mtp_open: no MTP device found "
                        "(is the phone unlocked and in File Transfer mode?)\n");
        free(raw);
        return -1;
    }
    fprintf(stderr, "mtp_open: opening raw device 0 (VID=%04x PID=%04x)...\n",
            raw[0].device_entry.vendor_id, raw[0].device_entry.product_id);
    fflush(stderr);
    g_device = LIBMTP_Open_Raw_Device_Uncached(&raw[0]);
    free(raw);
    if (!g_device) {
        fprintf(stderr, "mtp_open: LIBMTP_Open_Raw_Device failed\n");
        return -1;
    }
    char *name  = LIBMTP_Get_Friendlyname(g_device);
    char *model = LIBMTP_Get_Modelname(g_device);
    fprintf(stderr, "mtp_open: connected to %s (%s)\n",
            name  ? name  : "?",
            model ? model : "?");
    fflush(stderr);
    free(name); free(model);

    /* Force-update storage list so we can enumerate volumes. */
    fprintf(stderr, "mtp_open: enumerating storage...\n"); fflush(stderr);
    LIBMTP_Get_Storage(g_device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
    fprintf(stderr, "mtp_open: ready\n"); fflush(stderr);

    g_root = node_new("", 1, 0, 0);
    return 0;
}

void mtp_close(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_root)   { node_free(g_root); g_root = NULL; }
    if (g_device) { LIBMTP_Release_Device(g_device); g_device = NULL; }
    pthread_mutex_unlock(&g_lock);
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
    return rc;
}

int mtp_readdir(const char *path, mtp_dir_cb cb, void *ctx)
{
    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(path);
    if (!n)        { pthread_mutex_unlock(&g_lock); return -ENOENT; }
    if (!n->is_dir){ pthread_mutex_unlock(&g_lock); return -ENOTDIR; }
    load_children(n);
    for (mtp_node_t *c = n->first_child; c; c = c->next_sibling) {
        cb(c->name, c->is_dir, ctx);
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* ---- file IO ---- */

int mtp_read(const char *path, char *buf, size_t size, off_t offset)
{
    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(path);
    if (!n)        { pthread_mutex_unlock(&g_lock); return -ENOENT; }
    if (n->is_dir) { pthread_mutex_unlock(&g_lock); return -EISDIR; }
    uint32_t oid = n->object_id;
    uint64_t total = n->size;
    pthread_mutex_unlock(&g_lock);

    if ((uint64_t)offset >= total) return 0;
    if (offset + size > total) size = (size_t)(total - offset);

    /* libmtp lacks a robust ranged read across all stacks; pipe the file
     * into a temp fd and pread() from it. For typical Finder previews
     * this is fine; for huge files macOS will mostly ask for sequential
     * chunks anyway. */
    char tmpl[] = "/tmp/mtpfuse_readXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -errno;
    unlink(tmpl);

    pthread_mutex_lock(&g_lock);
    int rc = LIBMTP_Get_File_To_File_Descriptor(
        g_device, oid, fd, NULL, NULL);
    if (rc != 0) log_mtp_errors();
    pthread_mutex_unlock(&g_lock);

    if (rc != 0) { close(fd); return -EIO; }

    ssize_t got = pread(fd, buf, size, offset);
    int err = (got < 0) ? -errno : (int)got;
    close(fd);
    return err;
}

int mtp_write_full(const char *path, const char *buf, size_t size)
{
    /* Split path into parent dir + basename */
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
    /* Storage root container required */
    uint32_t storage_id = p->storage_id;
    uint32_t parent_id  = (p == g_root) ? 0 : p->object_id;
    if (p == g_root) { /* writing into the root listing is not allowed */
        pthread_mutex_unlock(&g_lock); free(dup); return -EACCES;
    }

    /* If a node already exists, delete it first (MTP overwrite). */
    mtp_node_t *existing = node_find_child(p, name);
    if (existing && !existing->is_dir) {
        LIBMTP_Delete_Object(g_device, existing->object_id);
    }
    pthread_mutex_unlock(&g_lock);

    /* Stage the bytes in a temp file (libmtp wants an fd). */
    char tmpl[] = "/tmp/mtpfuse_wrXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) { free(dup); return -errno; }
    unlink(tmpl);
    ssize_t off = 0;
    while ((size_t)off < size) {
        ssize_t w = write(fd, buf + off, size - off);
        if (w <= 0) { close(fd); free(dup); return -EIO; }
        off += w;
    }
    lseek(fd, 0, SEEK_SET);

    LIBMTP_file_t *meta = LIBMTP_new_file_t();
    meta->filename       = strdup(name);
    meta->parent_id      = parent_id;
    meta->storage_id     = storage_id;
    meta->filesize       = size;
    meta->filetype       = LIBMTP_FILETYPE_UNKNOWN;

    pthread_mutex_lock(&g_lock);
    int rc = LIBMTP_Send_File_From_File_Descriptor(
        g_device, fd, meta, NULL, NULL);
    if (rc != 0) log_mtp_errors();
    /* invalidate parent cache so the new file shows up */
    if (rc == 0 && p->children_loaded) {
        mtp_node_t *c = p->first_child; p->first_child = NULL;
        while (c) { mtp_node_t *nx = c->next_sibling; node_free(c); c = nx; }
        p->children_loaded = 0;
    }
    pthread_mutex_unlock(&g_lock);

    LIBMTP_destroy_file_t(meta);
    close(fd);
    free(dup);
    return (rc == 0) ? (int)size : -EIO;
}

int mtp_unlink(const char *path)
{
    pthread_mutex_lock(&g_lock);
    mtp_node_t *n = resolve(path);
    if (!n)        { pthread_mutex_unlock(&g_lock); return -ENOENT; }
    if (n->is_dir) { pthread_mutex_unlock(&g_lock); return -EISDIR; }
    uint32_t oid = n->object_id;
    int rc = LIBMTP_Delete_Object(g_device, oid);
    if (rc == 0 && n->parent) {
        /* unlink from parent */
        mtp_node_t **pp = &n->parent->first_child;
        while (*pp && *pp != n) pp = &(*pp)->next_sibling;
        if (*pp) { *pp = n->next_sibling; n->next_sibling = NULL; node_free(n); }
    } else if (rc != 0) {
        log_mtp_errors();
    }
    pthread_mutex_unlock(&g_lock);
    return rc == 0 ? 0 : -EIO;
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
    uint32_t parent_id  = p->object_id;
    uint32_t new_id = LIBMTP_Create_Folder(g_device, name, parent_id, storage_id);
    if (new_id == 0) {
        log_mtp_errors();
        pthread_mutex_unlock(&g_lock); free(dup); return -EIO;
    }
    mtp_node_t *n = node_new(name, 1, new_id, storage_id);
    if (n) node_add_child(p, n);
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
    int rc = LIBMTP_Delete_Object(g_device, n->object_id);
    if (rc == 0 && n->parent) {
        mtp_node_t **pp = &n->parent->first_child;
        while (*pp && *pp != n) pp = &(*pp)->next_sibling;
        if (*pp) { *pp = n->next_sibling; n->next_sibling = NULL; node_free(n); }
    } else if (rc != 0) {
        log_mtp_errors();
    }
    pthread_mutex_unlock(&g_lock);
    return rc == 0 ? 0 : -EIO;
}
