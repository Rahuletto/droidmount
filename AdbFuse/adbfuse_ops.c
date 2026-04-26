/*
 * adbfuse_ops.c — wired ADB-backed read-mostly FUSE (macFUSE).
 * Virtual root maps to ADB_DEVICE_PREFIX (default /storage).
 */

#define FUSE_USE_VERSION 26
#define _DARWIN_C_SOURCE 1

#include "adb_subprocess.h"
#include "adbfuse_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/mount.h>
#endif

static char *g_dev_prefix = NULL; /* e.g. /storage */

/* Short getattr cache (Finder hammers stat); TTL in seconds. */
#define GATTR_N 96
#define GATTR_TTL 0.35
static struct {
    char     path[512];
    struct stat st;
    double   exp_mono;
    int      valid; /* 0 empty 1 hit */
} g_gattr[GATTR_N];
static pthread_mutex_t g_gattr_mu = PTHREAD_MUTEX_INITIALIZER;
static int             g_gattr_rr;

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void gattr_invalidate_all(void)
{
    pthread_mutex_lock(&g_gattr_mu);
    for (int i = 0; i < GATTR_N; i++)
        g_gattr[i].valid = 0;
    pthread_mutex_unlock(&g_gattr_mu);
}

static int adb_readlink_dev(const char *devpath, char *buf, size_t cap)
{
    const char *argv[] = { "shell", "readlink", "-n", "--", devpath, NULL };
    size_t len = 0;
    int r = adb_run_capture(argv, buf, cap, &len, 8);
    if (r != 0 || len == 0 || len >= cap)
        return -1;
    buf[len] = '\0';
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    return 0;
}

static int adb_truncate_remote(const char *devpath, off_t sz)
{
    char szbuf[32];
    snprintf(szbuf, sizeof szbuf, "%lld", (long long)sz);
    const char *argv[] = { "shell", "truncate", "-s", szbuf, "--", devpath, NULL };
    char out[64];
    size_t zn = 0;
    return adb_run_capture(argv, out, sizeof out, &zn, 30) == 0 ? 0 : -EIO;
}

/* Parse `df -k` first data line for [path] or prefix; fills statvfs-ish sizes in 1K units. */
static int adb_df_kb(const char *path, uint64_t *total_kb, uint64_t *avail_kb)
{
    char out[4096];
    size_t len = 0;
    const char *argv[] = { "shell", "df", "-k", path, NULL };
    if (adb_run_capture(argv, out, sizeof out, &len, 12) != 0 || len == 0)
        return -1;
    char *save = NULL;
    char *dup = strdup(out);
    if (!dup)
        return -1;
    int line_no = 0;
    unsigned long kb1 = 0, kb2 = 0, kb3 = 0;
    for (char *line = strtok_r(dup, "\n\r", &save); line; line = strtok_r(NULL, "\n\r", &save)) {
        if (line[0] == '\0')
            continue;
        line_no++;
        if (line_no == 1)
            continue; /* header */
        /* Filesystem 1024-blocks Used Available Capacity Mounted */
        if (sscanf(line, "%*s %lu %lu %lu", &kb1, &kb2, &kb3) >= 3) {
            *total_kb = (uint64_t)kb1;
            *avail_kb = (uint64_t)kb3;
            free(dup);
            return 0;
        }
    }
    free(dup);
    return -1;
}

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 14695981039346656037ULL;
    for (; *s; ++s) {
        h ^= (uint64_t)(unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

static int dev_path_from_fuse(const char *fuse_path, char *out, size_t cap)
{
    if (!fuse_path || fuse_path[0] != '/')
        return -ENOENT;
    if (strstr(fuse_path, "/..") != NULL)
        return -ENOENT;
    const char *pfx = g_dev_prefix ? g_dev_prefix : "/storage";
    if (fuse_path[1] == '\0') {
        if (snprintf(out, cap, "%s", pfx) >= (int)cap)
            return -ENAMETOOLONG;
        return 0;
    }
    if (snprintf(out, cap, "%s%s", pfx, fuse_path) >= (int)cap)
        return -ENAMETOOLONG;
    if (strstr(out, "/../") != NULL || strncmp(out, pfx, strlen(pfx)) != 0)
        return -ENOENT;
    return 0;
}

static int adb_shell_test(const char *flag, const char *devpath)
{
    const char *argv[] = { "shell", "test", flag, "--", devpath, NULL };
    char tmp[16];
    size_t zn = 0;
    int r = adb_run_capture(argv, tmp, sizeof tmp, &zn, 8);
    (void)zn;
    return r == 0 ? 0 : -ENOENT;
}

static int adb_stat_file(const char *devpath, uint64_t *size_out, time_t *mtime_out)
{
    char out[512];
    size_t len = 0;
    /* toybox/coreutils stat */
    const char *argv1[] = { "shell", "stat", "-c", "%s %Y", "--", devpath, NULL };
    int r = adb_run_capture(argv1, out, sizeof out, &len, 8);
    if (r == 0 && len > 0) {
        unsigned long long sz = 0;
        long long mt = 0;
        if (sscanf(out, "%llu %lld", &sz, &mt) >= 1) {
            if (size_out)
                *size_out = (uint64_t)sz;
            if (mtime_out)
                *mtime_out = (time_t)mt;
            return 0;
        }
    }
    /* ls -l fallback: one line */
    const char *argv2[] = { "shell", "ls", "-l", "--", devpath, NULL };
    r = adb_run_capture(argv2, out, sizeof out, &len, 8);
    if (r != 0 || len == 0)
        return -ENOENT;
    /* skip mode bits, find size (column before date) — fragile; best-effort */
    char *p = out;
    while (*p && *p != '\n' && *p != '\r')
        p++;
    *p = '\0';
    /* last token before month often size — skip */
    unsigned long long sz = 0;
    if (sscanf(out, "%*s %llu", &sz) == 1 || sscanf(out, "%*s %*s %llu", &sz) == 1) {
        if (size_out)
            *size_out = sz;
        if (mtime_out)
            *mtime_out = time(NULL);
        return 0;
    }
    return -ENOENT;
}

typedef struct {
    int         kind; /* 1 read stream 2 write staging */
    int         fd;
    char       *devpath;
    int         dirty;
    /* read stream */
    pthread_t   pump;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint64_t    written;
    int         pump_st; /* 0 run 1 ok -1 err */
} adbfuse_fh_t;

static void *pump_main(void *arg)
{
    adbfuse_fh_t *h = (adbfuse_fh_t *)arg;
    int rc = adb_exec_out_cat_to_fd(h->devpath, h->fd, 0);
    struct stat st;
    uint64_t sz = 0;
    if (rc == 0 && fstat(h->fd, &st) == 0)
        sz = (uint64_t)st.st_size;
    pthread_mutex_lock(&h->mu);
    h->written = sz;
    h->pump_st = (rc == 0) ? 1 : -1;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mu);
    return NULL;
}

static int op_getattr(const char *path, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    char dev[4096];
    int dr = dev_path_from_fuse(path, dev, sizeof dev);
    if (dr != 0)
        return dr;

    double now = mono_now();
    pthread_mutex_lock(&g_gattr_mu);
    for (int i = 0; i < GATTR_N; i++) {
        if (g_gattr[i].valid && strcmp(g_gattr[i].path, path) == 0 && g_gattr[i].exp_mono > now) {
            *st = g_gattr[i].st;
            pthread_mutex_unlock(&g_gattr_mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_gattr_mu);

    if (adb_shell_test("-h", dev) == 0) {
        char tgt[8192];
        if (adb_readlink_dev(dev, tgt, sizeof tgt) == 0) {
            memset(st, 0, sizeof(*st));
            st->st_mode = S_IFLNK | 0777;
            st->st_nlink = 1;
            st->st_ino = (ino_t)fnv1a64(dev);
            st->st_uid = getuid();
            st->st_gid = getgid();
            size_t tl = strnlen(tgt, sizeof tgt);
            st->st_size = (off_t)((tl > (size_t)OFF_MAX) ? OFF_MAX : (off_t)tl);
            st->st_mtime = time(NULL);
            goto cache_store;
        }
    }

    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0777;
        st->st_nlink = 2;
        st->st_ino = 1;
        st->st_uid = getuid();
        st->st_gid = getgid();
        st->st_mtime = time(NULL);
        goto cache_store;
    }

    int is_dir = (adb_shell_test("-d", dev) == 0);
    int is_file = 0;
    if (!is_dir)
        is_file = (adb_shell_test("-f", dev) == 0);

    if (is_dir) {
        st->st_mode = S_IFDIR | 0777;
        st->st_nlink = 2;
        st->st_ino = (ino_t)fnv1a64(dev);
        st->st_uid = getuid();
        st->st_gid = getgid();
        st->st_mtime = time(NULL);
        goto cache_store;
    }
    if (!is_file)
        return -ENOENT;

    uint64_t sz = 0;
    time_t mt = 0;
    if (adb_stat_file(dev, &sz, &mt) != 0) {
        sz = 0;
        mt = time(NULL);
    }
    st->st_mode = S_IFREG | 0666;
    st->st_nlink = 1;
    st->st_ino = (ino_t)fnv1a64(dev);
    st->st_size = (off_t)((sz > (uint64_t)OFF_MAX) ? OFF_MAX : (off_t)sz);
    st->st_blksize = 1048576;
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_mtime = mt;
    if (sz)
        st->st_blocks = (blkcnt_t)((sz + 511) / 512);

cache_store:;
    pthread_mutex_lock(&g_gattr_mu);
    double nmono = mono_now();
    int slot = -1;
    for (int i = 0; i < GATTR_N; i++) {
        if (!g_gattr[i].valid || g_gattr[i].exp_mono <= nmono) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = g_gattr_rr++ % GATTR_N;
    }
    if (strlen(path) < sizeof(g_gattr[slot].path)) {
        strcpy(g_gattr[slot].path, path);
        g_gattr[slot].st = *st;
        g_gattr[slot].exp_mono = mono_now() + GATTR_TTL;
        g_gattr[slot].valid = 1;
    }
    pthread_mutex_unlock(&g_gattr_mu);
    return 0;
}

static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                      struct fuse_file_info *fi)
{
    (void)offset;
    (void)fi;
    char dev[4096];
    if (dev_path_from_fuse(path, dev, sizeof dev) != 0)
        return -ENOENT;

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    char out[256 * 1024];
    size_t len = 0;
    const char *argv[] = { "shell", "ls", "-1A", "--", dev, NULL };
    int r = adb_run_capture(argv, out, sizeof out, &len, 20);
    if (r != 0)
        return 0;

    char *save = NULL;
    char *dup = strdup(out);
    if (!dup)
        return -ENOMEM;
    for (char *line = strtok_r(dup, "\n\r", &save); line; line = strtok_r(NULL, "\n\r", &save)) {
        if (line[0] == '\0')
            continue;
        if (strcmp(line, ".") == 0 || strcmp(line, "..") == 0)
            continue;
        filler(buf, line, NULL, 0);
    }
    free(dup);
    return 0;
}

static int op_open(const char *path, struct fuse_file_info *fi)
{
    char dev[4096];
    if (dev_path_from_fuse(path, dev, sizeof dev) != 0)
        return -ENOENT;

    int acc = fi->flags & O_ACCMODE;
    if (acc == O_RDONLY) {
        int okf = (adb_shell_test("-f", dev) == 0);
        if (!okf)
            return -ENOENT;

        adbfuse_fh_t *h = calloc(1, sizeof(*h));
        if (!h)
            return -ENOMEM;
        h->kind = 1;
        h->devpath = strdup(dev);
        if (!h->devpath) {
            free(h);
            return -ENOMEM;
        }
        pthread_mutex_init(&h->mu, NULL);
        pthread_cond_init(&h->cv, NULL);
        char tmpl[] = "/tmp/adbfuseXXXXXX";
        int tfd = mkstemp(tmpl);
        if (tfd < 0) {
            free(h->devpath);
            pthread_mutex_destroy(&h->mu);
            pthread_cond_destroy(&h->cv);
            free(h);
            return -errno;
        }
        unlink(tmpl);
        h->fd = tfd;
        if (pthread_create(&h->pump, NULL, pump_main, h) != 0) {
            close(h->fd);
            free(h->devpath);
            pthread_mutex_destroy(&h->mu);
            pthread_cond_destroy(&h->cv);
            free(h);
            return -EIO;
        }
        fi->fh = (uint64_t)(uintptr_t)h;
        return 0;
    }

    if (acc == O_WRONLY || acc == O_RDWR) {
        int exists = (adb_shell_test("-e", dev) == 0);
        int isdir = exists && (adb_shell_test("-d", dev) == 0);
        if (isdir)
            return -EISDIR;
        if ((fi->flags & O_CREAT) && (fi->flags & O_EXCL) && exists)
            return -EEXIST;

        int need_pull = 0;
        if (exists) {
            if (fi->flags & O_TRUNC)
                need_pull = 1;
            else if (acc == O_RDWR)
                need_pull = 1;
            else if (!(fi->flags & O_CREAT))
                need_pull = 1;
        }

        adbfuse_fh_t *h = calloc(1, sizeof(*h));
        if (!h)
            return -ENOMEM;
        h->kind = 2;
        h->devpath = strdup(dev);
        if (!h->devpath) {
            free(h);
            return -ENOMEM;
        }

        if (need_pull) {
            char pl[PATH_MAX];
            snprintf(pl, sizeof pl, "/tmp/adbpf_%ld_%.0f", (long)getpid(), mono_now() * 1e6);
            if (adb_pull_to_file(dev, pl, 7200) != 0) {
                free(h->devpath);
                free(h);
                return -EIO;
            }
            h->fd = open(pl, O_RDWR | O_CLOEXEC);
            (void)unlink(pl);
            if (h->fd < 0) {
                int e = errno;
                free(h->devpath);
                free(h);
                return -e;
            }
        } else {
            char tmpl[] = "/tmp/adbfusewXXXXXX";
            int tfd = mkostemp(tmpl, O_CLOEXEC);
            if (tfd < 0) {
                free(h->devpath);
                free(h);
                return -errno;
            }
            unlink(tmpl);
            h->fd = tfd;
        }
        fi->fh = (uint64_t)(uintptr_t)h;
        return 0;
    }
    return -EINVAL;
}

static int op_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    (void)path;
    adbfuse_fh_t *h = (adbfuse_fh_t *)(uintptr_t)fi->fh;
    if (!h || h->kind != 1)
        return -EIO;

    for (int spin = 0; spin < 2400; spin++) {
        pthread_mutex_lock(&h->mu);
        if (h->pump_st != 0) {
            int pst = h->pump_st;
            pthread_mutex_unlock(&h->mu);
            if (pst < 0)
                return -EIO;
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 250000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_nsec -= 1000000000;
            ts.tv_sec++;
        }
        (void)pthread_cond_timedwait(&h->cv, &h->mu, &ts);
        pthread_mutex_unlock(&h->mu);
    }
    pthread_mutex_lock(&h->mu);
    int pst_final = h->pump_st;
    pthread_mutex_unlock(&h->mu);
    if (pst_final == 0)
        return -EIO;
    if (pst_final < 0)
        return -EIO;
    ssize_t r = pread(h->fd, buf, size, offset);
    if (r < 0)
        return -errno;
    return (int)r;
}

static int op_write(const char *path, const char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
    (void)path;
    adbfuse_fh_t *h = (adbfuse_fh_t *)(uintptr_t)fi->fh;
    if (!h || h->kind != 2)
        return -EIO;
    ssize_t w = pwrite(h->fd, buf, size, offset);
    if (w < 0)
        return -errno;
    h->dirty = 1;
    return (int)w;
}

static int adb_push(int local_fd, const char *remote)
{
    char tmpl[] = "/tmp/adbfusepushXXXXXX";
    int tfd = mkstemp(tmpl);
    if (tfd < 0)
        return -errno;
    if (lseek(local_fd, 0, SEEK_SET) < 0) {
        close(tfd);
        unlink(tmpl);
        return -errno;
    }
    char ibuf[65536];
    for (;;) {
        ssize_t r = read(local_fd, ibuf, sizeof ibuf);
        if (r < 0) {
            close(tfd);
            unlink(tmpl);
            return -errno;
        }
        if (r == 0)
            break;
        size_t o = 0;
        while (o < (size_t)r) {
            ssize_t w = write(tfd, ibuf + o, (size_t)r - o);
            if (w < 0) {
                close(tfd);
                unlink(tmpl);
                return -errno;
            }
            o += (size_t)w;
        }
    }
    fsync(tfd);
    close(tfd);

    const char *argv[] = { "push", tmpl, remote, NULL };
    char err[512];
    size_t el = 0;
    int rc = adb_run_capture(argv, err, sizeof err, &el, 3600);
    unlink(tmpl);
    (void)err;
    return rc == 0 ? 0 : -EIO;
}

static int op_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    adbfuse_fh_t *h = (adbfuse_fh_t *)(uintptr_t)fi->fh;
    if (!h)
        return 0;
    if (h->kind == 1) {
        pthread_join(h->pump, NULL);
        close(h->fd);
        free(h->devpath);
        pthread_mutex_destroy(&h->mu);
        pthread_cond_destroy(&h->cv);
        free(h);
        return 0;
    }
    if (h->kind == 2) {
        if (h->dirty) {
            if (adb_push(h->fd, h->devpath) == 0)
                gattr_invalidate_all();
        }
        close(h->fd);
        free(h->devpath);
        free(h);
        return 0;
    }
    free(h);
    return 0;
}

static int op_unlink(const char *path)
{
    char dev[4096];
    if (dev_path_from_fuse(path, dev, sizeof dev) != 0)
        return -ENOENT;
    int isdir = (adb_shell_test("-d", dev) == 0);
    char out[64];
    size_t zn = 0;
    int r;
    if (isdir) {
        const char *avd[] = { "shell", "rm", "-rf", "--", dev, NULL };
        r = adb_run_capture(avd, out, sizeof out, &zn, 120);
    } else {
        const char *avf[] = { "shell", "rm", "-f", "--", dev, NULL };
        r = adb_run_capture(avf, out, sizeof out, &zn, 60);
    }
    if (r == 0)
        gattr_invalidate_all();
    return r == 0 ? 0 : -EIO;
}

static int op_mkdir(const char *path, mode_t mode)
{
    (void)mode;
    char dev[4096];
    if (dev_path_from_fuse(path, dev, sizeof dev) != 0)
        return -ENOENT;
    const char *argv[] = { "shell", "mkdir", "-p", "--", dev, NULL };
    char out[64];
    size_t zn = 0;
    int r = adb_run_capture(argv, out, sizeof out, &zn, 30);
    if (r == 0)
        gattr_invalidate_all();
    return r == 0 ? 0 : -EIO;
}

static int op_rmdir(const char *path)
{
    char dev[4096];
    if (dev_path_from_fuse(path, dev, sizeof dev) != 0)
        return -ENOENT;
    char out[256];
    size_t zn = 0;
    const char *argv1[] = { "shell", "rmdir", "--", dev, NULL };
    int r = adb_run_capture(argv1, out, sizeof out, &zn, 30);
    if (r == 0) {
        gattr_invalidate_all();
        return 0;
    }
    const char *argv2[] = { "shell", "rm", "-rf", "--", dev, NULL };
    r = adb_run_capture(argv2, out, sizeof out, &zn, 120);
    if (r == 0)
        gattr_invalidate_all();
    return r == 0 ? 0 : -EIO;
}

static int op_rename(const char *from, const char *to)
{
    char devf[4096], devt[4096];
    if (dev_path_from_fuse(from, devf, sizeof devf) != 0 || dev_path_from_fuse(to, devt, sizeof devt) != 0)
        return -EXDEV;
    const char *argv2[] = { "shell", "mv", "--", devf, devt, NULL };
    char out[256];
    size_t zn = 0;
    int r = adb_run_capture(argv2, out, sizeof out, &zn, 60);
    if (r == 0)
        gattr_invalidate_all();
    return r == 0 ? 0 : -EXDEV;
}

#if defined(__APPLE__)
#ifndef RENAME_SWAP
#define RENAME_SWAP 0x2u
#endif
#ifndef RENAME_EXCL
#define RENAME_EXCL 0x4u
#endif
#ifndef RENAME_NOFOLLOW_ANY
#define RENAME_NOFOLLOW_ANY 0x10u
#endif
#ifndef RENAME_RESOLVE_BENEATH
#define RENAME_RESOLVE_BENEATH 0x40u
#endif

static int op_renamex(const char *from, const char *to, unsigned int flags)
{
    if (flags & (unsigned)RENAME_SWAP)
        return -EINVAL;
    if (flags & (unsigned)RENAME_NOFOLLOW_ANY)
        return -EINVAL;
    if (flags & (unsigned)RENAME_RESOLVE_BENEATH)
        return -EINVAL;
    if (flags & (unsigned)RENAME_EXCL) {
        char devt[4096];
        if (dev_path_from_fuse(to, devt, sizeof devt) == 0 && adb_shell_test("-e", devt) == 0)
            return -EEXIST;
    }
    return op_rename(from, to);
}
#endif

static int op_readlink(const char *path, char *buf, size_t size)
{
    if (size == 0)
        return -EINVAL;
    char dev[4096];
    if (dev_path_from_fuse(path, dev, sizeof dev) != 0)
        return -ENOENT;
    char tmp[8192];
    if (adb_readlink_dev(dev, tmp, sizeof tmp) != 0)
        return -EINVAL;
    size_t tl = strlen(tmp);
    if (tl + 1 > size)
        return -ENAMETOOLONG;
    memcpy(buf, tmp, tl + 1);
    return 0;
}

static int op_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)mode;
    return op_open(path, fi);
}

static void *op_init(struct fuse_conn_info *conn)
{
#if defined(__APPLE__)
    if (conn->max_write < (unsigned)1048576)
        conn->max_write = (unsigned)1048576;
#endif
    return NULL;
}

static int op_access(const char *path, int mask)
{
    char dev[4096];
    if (dev_path_from_fuse(path, dev, sizeof dev) != 0)
        return -ENOENT;
    if ((mask & (R_OK | W_OK | X_OK)) == 0)
        return adb_shell_test("-e", dev) == 0 ? 0 : -ENOENT;
    if (mask & R_OK) {
        if (adb_shell_test("-r", dev) != 0)
            return -EACCES;
    }
    if (mask & W_OK) {
        if (adb_shell_test("-w", dev) != 0)
            return -EACCES;
    }
    if (mask & X_OK) {
        if (adb_shell_test("-x", dev) != 0)
            return -EACCES;
    }
    return 0;
}

static void fill_statvfs_from_df(struct statvfs *st, uint64_t tot_kb, uint64_t avail_kb)
{
    memset(st, 0, sizeof(*st));
    st->f_bsize = 1024;
    st->f_frsize = 1024;
    st->f_blocks = tot_kb;
    st->f_bfree = avail_kb;
    st->f_bavail = avail_kb;
    st->f_namemax = 255;
}

static int op_statfs(const char *path, struct statvfs *st)
{
    uint64_t tot = 1024UL * 1024UL * 256UL, av = 1024UL * 1024UL * 128UL;
    char probe[4096];
    if (dev_path_from_fuse(path, probe, sizeof probe) == 0) {
        uint64_t tk = 0, ak = 0;
        if (adb_df_kb(probe, &tk, &ak) == 0 && tk > 0) {
            tot = tk;
            av = ak;
        }
    }
    fill_statvfs_from_df(st, tot, av);
    (void)path;
    return 0;
}

#if defined(__APPLE__)
static int op_statfs_x(const char *path, struct statfs *st)
{
    uint64_t tot = 1024UL * 1024UL * 256UL, av = 1024UL * 1024UL * 128UL;
    char probe[4096];
    if (dev_path_from_fuse(path, probe, sizeof probe) == 0) {
        uint64_t tk = 0, ak = 0;
        if (adb_df_kb(probe, &tk, &ak) == 0 && tk > 0) {
            tot = tk;
            av = ak;
        }
    }
    memset(st, 0, sizeof(*st));
    st->f_bsize = 1024;
    st->f_iosize = 1048576;
    st->f_blocks = (int64_t)tot;
    st->f_bfree = (int64_t)av;
    st->f_bavail = st->f_bfree;
    st->f_files = 1000000;
    st->f_ffree = 500000;
    strncpy(st->f_fstypename, "adb", sizeof(st->f_fstypename) - 1);
    st->f_fstypename[sizeof(st->f_fstypename) - 1] = '\0';
    (void)path;
    return 0;
}
#endif

static int op_chmod(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    return 0;
}
static int op_chown(const char *path, uid_t u, gid_t g)
{
    (void)path;
    (void)u;
    (void)g;
    return 0;
}
static int op_utimens(const char *path, const struct timespec tv[2])
{
    (void)path;
    (void)tv;
    return 0;
}
static int op_truncate(const char *path, off_t size)
{
    char dev[4096];
    if (dev_path_from_fuse(path, dev, sizeof dev) != 0)
        return -ENOENT;
    int r = adb_truncate_remote(dev, size);
    if (r == 0)
        gattr_invalidate_all();
    return r;
}

static int op_ftruncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void)path;
    adbfuse_fh_t *h = (adbfuse_fh_t *)(uintptr_t)fi->fh;
    if (!h || h->kind != 2)
        return -EINVAL;
    if (ftruncate(h->fd, size) < 0)
        return -errno;
    h->dirty = 1;
    gattr_invalidate_all();
    return 0;
}
static int op_flush(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    return 0;
}
static int op_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)path;
    (void)datasync;
    (void)fi;
    return 0;
}

static int op_fgetattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    adbfuse_fh_t *h = (adbfuse_fh_t *)(uintptr_t)fi->fh;
    if (h && h->kind == 2) {
        struct stat fst;
        memset(st, 0, sizeof(*st));
        if (fstat(h->fd, &fst) < 0)
            return -errno;
        st->st_mode = S_IFREG | 0666;
        st->st_nlink = 1;
        st->st_ino = (ino_t)fnv1a64(h->devpath);
        st->st_size = fst.st_size;
        st->st_blksize = 1048576;
        st->st_uid = getuid();
        st->st_gid = getgid();
        st->st_mtime = fst.st_mtime;
        if ((uint64_t)fst.st_size)
            st->st_blocks = (blkcnt_t)(((uint64_t)fst.st_size + 511) / 512);
        return 0;
    }
    if (h && h->kind == 1) {
        pthread_mutex_lock(&h->mu);
        int pst = h->pump_st;
        pthread_mutex_unlock(&h->mu);
        if (pst == 0)
            return op_getattr(path, st);
        struct stat fst;
        memset(st, 0, sizeof(*st));
        if (fstat(h->fd, &fst) < 0)
            return -errno;
        st->st_mode = S_IFREG | 0666;
        st->st_nlink = 1;
        st->st_ino = (ino_t)fnv1a64(h->devpath);
        st->st_size = fst.st_size;
        st->st_blksize = 1048576;
        st->st_uid = getuid();
        st->st_gid = getgid();
        st->st_mtime = fst.st_mtime;
        if ((uint64_t)fst.st_size)
            st->st_blocks = (blkcnt_t)(((uint64_t)fst.st_size + 511) / 512);
        return 0;
    }
    return op_getattr(path, st);
}

struct fuse_operations adbfuse_ops = {
    .init = op_init,
    .getattr = op_getattr,
    .fgetattr = op_fgetattr,
    .readlink = op_readlink,
    .readdir = op_readdir,
    .open = op_open,
    .create = op_create,
    .read = op_read,
    .write = op_write,
    .release = op_release,
    .unlink = op_unlink,
    .mkdir = op_mkdir,
    .rmdir = op_rmdir,
    .rename = op_rename,
    .chmod = op_chmod,
    .chown = op_chown,
    .utimens = op_utimens,
    .truncate = op_truncate,
    .ftruncate = op_ftruncate,
    .access = op_access,
    .statfs = op_statfs,
    .flush = op_flush,
    .fsync = op_fsync,
#if defined(__APPLE__)
    .renamex = op_renamex,
    .statfs_x = op_statfs_x,
#endif
};

int adbfuse_main(int argc, char **argv)
{
    adb_set_serial_from_env();
    if (!adb_serial()) {
        fprintf(stderr, "adbfuse: set ADB_SERIAL to the device serial (adb devices).\n");
        return 2;
    }
    const char *pfx = getenv("ADB_DEVICE_PREFIX");
    g_dev_prefix = strdup((pfx && pfx[0]) ? pfx : "/storage");
    if (!g_dev_prefix) {
        fprintf(stderr, "adbfuse: out of memory\n");
        return 1;
    }
    fprintf(stderr, "adbfuse: serial=%s prefix=%s\n", adb_serial(), g_dev_prefix);
    fflush(stderr);
    int rc = fuse_main(argc, argv, &adbfuse_ops, NULL);
    free(g_dev_prefix);
    g_dev_prefix = NULL;
    return rc;
}
