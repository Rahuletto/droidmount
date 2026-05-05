#ifndef MTP_LOG_H
#define MTP_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

void  mtp_log_init(int argc, char **argv);
int   mtp_log_is_enabled(void);
void  mtp_log_flush(void);
#if defined(__GNUC__)
void  mtp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#else
void  mtp_log(const char *fmt, ...);
#endif
void  mtp_log_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
