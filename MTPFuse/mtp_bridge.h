#ifndef MTP_BRIDGE_H
#define MTP_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise libmtp and pick the first attached MTP device.
 * Returns 0 on success, -1 on failure. */
int  mtp_open(void);
void mtp_close(void);

/* Walk the entire device tree once and populate the id<->path cache.
 * Safe to call multiple times — refreshes the cache. */
int  mtp_refresh_tree(void);

/* Path-oriented helpers used by the FUSE layer.
 * All paths are absolute and start with "/". The first component is the
 * storage name (e.g. "/Internal storage/DCIM/IMG_0001.jpg"). */

typedef struct {
    int      is_dir;
    uint64_t size;
    uint64_t mtime;
} mtp_stat_t;

int  mtp_stat(const char *path, mtp_stat_t *out);

/* Directory listing. Calls fill(name, is_dir, ctx) for every child. */
typedef void (*mtp_dir_cb)(const char *name, int is_dir, void *ctx);
int  mtp_readdir(const char *path, mtp_dir_cb cb, void *ctx);

/* Read [size] bytes at [offset] from the file at [path] into [buf].
 * Returns bytes read or -errno. */
int  mtp_read(const char *path, char *buf, size_t size, off_t offset);

/* Replace the file at [path] with [size] bytes from [buf]. Creates the
 * file if it does not exist. Returns bytes written or -errno. */
int  mtp_write_full(const char *path, const char *buf, size_t size);

/* Unlink (delete) a file. */
int  mtp_unlink(const char *path);

/* mkdir / rmdir. */
int  mtp_mkdir(const char *path);
int  mtp_rmdir(const char *path);

#ifdef __cplusplus
}
#endif

#endif
