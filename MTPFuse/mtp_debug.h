#ifndef MTP_DEBUG_H
#define MTP_DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Trace file: /tmp/mtpfuse-<pid>.log and symlink /tmp/mtpfuse-debug-latest.log
 * Disable with MTPFUSE_DEBUG=0. Override path with MTPFUSE_DEBUG_LOG=/path. */
void mtp_debug_boot(int argc, char **argv);
#if defined(__GNUC__)
void mtp_debug_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#else
void mtp_debug_log(const char *fmt, ...);
#endif
void mtp_debug_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
