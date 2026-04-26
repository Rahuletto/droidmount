#ifndef ADB_SUBPROCESS_H
#define ADB_SUBPROCESS_H

#include <stddef.h>

const char *adb_serial(void);
void        adb_set_serial_from_env(void);

/* argv_tail points at first arg after `adb -s SERIAL` (e.g. "shell", "echo", "ok", NULL). */
int adb_run_capture(const char *const *argv_tail, char *out, size_t cap, size_t *out_len,
                    int timeout_sec);

int adb_exec_out_cat_to_fd(const char *remote_path, int write_fd, int timeout_sec);

/* `adb pull remote local` (spawn serialized; transfer runs under ANDROIDMOUNT_ADB_MAX_PARALLEL). */
int adb_pull_to_file(const char *remote_path, const char *local_path, int timeout_sec);

/* `adb push local remote` (same parallelism as pull). */
int adb_push_from_file(const char *local_path, const char *remote_path, int timeout_sec);

/* `adb exec-out sh -c SCRIPT` → read up to [want] bytes into buf (binary-safe). */
int adb_exec_out_script_read(const char *script, unsigned char *buf, size_t want, int timeout_sec,
                             size_t *got_out);

#endif
