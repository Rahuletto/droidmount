#ifndef MTP_BRIDGE_H
#define MTP_BRIDGE_H

#include "mtp_debug.h"

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
    uint32_t object_id;   /* 0 = synthetic storage volume node */
    uint32_t storage_id;
} mtp_stat_t;

int  mtp_stat(const char *path, mtp_stat_t *out);

/* One directory level from cache (after MTP fetch for that folder only).
 * Snapshot is malloc'd; free with mtp_readdir_snapshot_free. */
typedef struct {
    char    *name;
    int      is_dir;
    uint64_t size;
    uint64_t mtime;
    uint32_t object_id;
    uint32_t storage_id;
} mtp_dirent_t;

int  mtp_readdir_snapshot(const char *path, mtp_dirent_t **out, size_t *n_out);
void mtp_readdir_snapshot_free(mtp_dirent_t *entries, size_t n);

/* Read [size] bytes at [offset] from the file at [path] into [buf].
 * Returns bytes read or -errno. */
int  mtp_read(const char *path, char *buf, size_t size, off_t offset);

/* 1 if the device reports LIBMTP_DEVICECAP_GetPartialObject (cached). */
int mtp_supports_partial_read(void);

/* Range read via GetPartialObject (no full-file pull). [oid] and [file_size]
 * must match the object opened from the tree. Returns bytes read or -errno. */
int mtp_read_partial(uint32_t oid, uint64_t file_size, char *buf, size_t size,
                     off_t offset);

/* Pull the full object from the device into [fd] (truncated, seek 0). For
 * large files when op_open skips prefetch. Returns 0 or -errno. */
int  mtp_download_to_fd(const char *path, int fd);

/* Replace the file at [path] with [size] bytes from [buf]. Creates the
 * file if it does not exist. Returns bytes written or -errno. */
int  mtp_write_full(const char *path, const char *buf, size_t size);

/* Same as mtp_write_full but reads payload from [fd] at offset 0 (after
 * optional internal lseek). Avoids malloc(filesize) on release. */
int  mtp_write_full_fd(const char *path, int fd, size_t size);

/* Rename or move within the device (MTP Set_Object_Filename / Move_Object). */
int  mtp_rename(const char *from, const char *to);

/* Unlink (delete) a file. */
int  mtp_unlink(const char *path);

/* mkdir / rmdir. */
int  mtp_mkdir(const char *path);
int  mtp_rmdir(const char *path);

/* Free/total bytes for the storage volume containing `path` (from libmtp).
 * If `path` is `/` or cannot be resolved, sums all storages. Returns 0 or -errno. */
int mtp_storage_space_for_path(const char *path, uint64_t *total_bytes, uint64_t *free_bytes);

#ifdef __cplusplus
}
#endif

#endif
