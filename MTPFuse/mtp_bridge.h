#ifndef MTP_BRIDGE_H
#define MTP_BRIDGE_H

#include "mtp_log.h"

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int  mtp_open(void);
void mtp_close(void);
void mtp_set_fuse_mount_point(const char *mountpoint);
int mtp_anon_tempfile_fd(const char *stem);
int  mtp_refresh_tree(void);

typedef struct {
    int      is_dir;
    int      is_shell;
    uint64_t size;
    uint64_t mtime;
    uint32_t object_id;
    uint32_t storage_id;
} mtp_stat_t;

int  mtp_stat(const char *path, mtp_stat_t *out);
int  mtp_stat_refresh(const char *path, mtp_stat_t *out);

typedef struct {
    char    *name;
    int      is_dir;
    int      is_shell;
    uint64_t size;
    uint64_t mtime;
    uint32_t object_id;
    uint32_t storage_id;
} mtp_dirent_t;

int  mtp_readdir_snapshot(const char *path, mtp_dirent_t **out, size_t *n_out);
void mtp_readdir_snapshot_free(mtp_dirent_t *entries, size_t n);
int  mtp_read(const char *path, char *buf, size_t size, off_t offset);
int mtp_long_transfer_active(void);
int mtp_supports_partial_read(void);
int mtp_read_partial(uint32_t oid, uint64_t file_size, char *buf, size_t size,
                     off_t offset);
int  mtp_download_to_fd(const char *path, int fd);
int  mtp_write_full(const char *path, const char *buf, size_t size);
int  mtp_write_full_fd(const char *path, int fd, size_t size);
int  mtp_rename(const char *from, const char *to);
int  mtp_unlink(const char *path);
int  mtp_mkdir(const char *path);
int  mtp_rmdir(const char *path);
int  mtp_shell_file_register(const char *path);
void mtp_shell_file_unregister(const char *path);
int mtp_storage_space_for_path(const char *path, uint64_t *total_bytes, uint64_t *free_bytes);
void mtp_for_each_volume_directory_path(void (*cb)(const char *path, void *ctx), void *ctx);
void mtp_invalidate_fuse_dir_cache(const char *fuse_path);
void mtp_invalidate_fuse_path(const char *path);
int mtp_root_volume_icon_active(void);

#ifdef __cplusplus
}
#endif

#endif
