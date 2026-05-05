#define _DARWIN_C_SOURCE 1
#include "mtp_log.h"

#include <strings.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static FILE            *g_mtp_log_f;
static pthread_mutex_t  g_mtp_log_mu = PTHREAD_MUTEX_INITIALIZER;
static char             g_mtp_log_path[512];
static unsigned         g_mtp_log_line;
static int              g_mtp_log_full_buf;
static int              g_mtp_log_sync;
static char            *g_mtp_log_buf;
static const size_t     g_mtp_log_buf_size = 256 * 1024u;

int mtp_log_is_enabled(void)
{
    return g_mtp_log_f != NULL;
}

void mtp_log_flush(void)
{
    if (!g_mtp_log_f)
        return;
    pthread_mutex_lock(&g_mtp_log_mu);
    fflush(g_mtp_log_f);
    pthread_mutex_unlock(&g_mtp_log_mu);
}

static int log_env_on(const char *k)
{
    const char *v = getenv(k);
    if (!v || !v[0])
        return 0;
    if (v[0] == '0' && v[1] == '\0')
        return 0;
    if (strcasecmp(v, "no") == 0 || strcasecmp(v, "off") == 0 || strcasecmp(v, "false") == 0)
        return 0;
    return 1;
}

void mtp_log_init(int argc, char **argv)
{
    g_mtp_log_f = NULL;
    g_mtp_log_line = 0;
    g_mtp_log_full_buf = 0;
    g_mtp_log_sync = 0;
    g_mtp_log_buf = NULL;

    if (getenv("MTPFUSE_LOG") && getenv("MTPFUSE_LOG")[0] == '0' && getenv("MTPFUSE_LOG")[1] == '\0') {
        fprintf(stderr, "mtpfuse: file logging off (MTPFUSE_LOG=0)\n");
        fflush(stderr);
        return;
    }

    int want = 0;
    if (log_env_on("MTPFUSE_LOG"))
        want = 1;
    if (log_env_on("MTPFUSE_DEBUG"))
        want = 1;
    const char *p1 = getenv("MTPFUSE_LOG_PATH");
    const char *p2 = getenv("MTPFUSE_LOG_FILE");
    const char *p3 = getenv("MTPFUSE_DEBUG_LOG");
    if ((p1 && p1[0]) || (p2 && p2[0]) || (p3 && p3[0]))
        want = 1;

    if (!want)
        return;

    const char *src = p1 && p1[0] ? p1 : (p2 && p2[0] ? p2 : (p3 && p3[0] ? p3 : NULL));
    if (src) {
        if (src[0] == '~' && src[1] == '/' && src[2]) {
            const char *h = getenv("HOME");
            if (h && h[0])
                snprintf(g_mtp_log_path, sizeof g_mtp_log_path, "%s/%s", h, src + 2);
            else
                snprintf(g_mtp_log_path, sizeof g_mtp_log_path, "/tmp/mtpfuse-%d.log", (int)getpid());
        } else
            snprintf(g_mtp_log_path, sizeof g_mtp_log_path, "%s", src);
    } else {
        const char *h = getenv("HOME");
        if (h && h[0]) {
            if (snprintf(g_mtp_log_path, sizeof g_mtp_log_path, "%s/.AndroidMount/mtpfuse.log", h)
                >= (int)sizeof g_mtp_log_path)
                g_mtp_log_path[0] = '\0';
        }
        if (g_mtp_log_path[0] == '\0')
            snprintf(g_mtp_log_path, sizeof g_mtp_log_path, "/tmp/mtpfuse-%d.log", (int)getpid());
    }
    {
        const char *h = getenv("HOME");
        if (h && h[0] && strstr(g_mtp_log_path, "/.AndroidMount/")) {
            char d[512];
            if (snprintf(d, sizeof d, "%s/.AndroidMount", h) < (int)sizeof d)
                (void)mkdir(d, 0755);
        }
    }

    if (log_env_on("MTPFUSE_LOG_SYNC"))
        g_mtp_log_sync = 1;
    g_mtp_log_full_buf = !g_mtp_log_sync;

    pthread_mutex_lock(&g_mtp_log_mu);
    g_mtp_log_f = fopen(g_mtp_log_path, "a");
    if (!g_mtp_log_f) {
        pthread_mutex_unlock(&g_mtp_log_mu);
        fprintf(stderr, "mtpfuse: mtp log fopen %s: %s\n", g_mtp_log_path, strerror(errno));
        fflush(stderr);
        return;
    }
    if (g_mtp_log_full_buf) {
        g_mtp_log_buf = malloc(g_mtp_log_buf_size);
        if (g_mtp_log_buf)
            setvbuf(g_mtp_log_f, g_mtp_log_buf, _IOFBF, g_mtp_log_buf_size);
        else
            setvbuf(g_mtp_log_f, NULL, _IOLBF, 0);
    } else
        setvbuf(g_mtp_log_f, NULL, _IOLBF, 0);

    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    fprintf(g_mtp_log_f, "\n--- mtp log session start pid=%d wall=%lld.%.03ld ---\n", (int)getpid(),
            (long long)rt.tv_sec, rt.tv_nsec / 1000000L);
    for (int i = 0; i < argc; i++)
        fprintf(g_mtp_log_f, "  argv[%d] %s\n", i, argv[i]);
    const char *mp = "?";
    for (int i = argc - 1; i >= 1; i--) {
        if (argv[i][0] != '-') {
            mp = argv[i];
            break;
        }
    }
    fprintf(g_mtp_log_f, "  mount: %s\n", mp);
    fprintf(g_mtp_log_f, "  log file: %s (MTPFUSE_LOG_SYNC=1 for line I/O; this file is append+buffered by default)\n", g_mtp_log_path);
    fflush(g_mtp_log_f);
    pthread_mutex_unlock(&g_mtp_log_mu);

    if (access("/tmp", W_OK) == 0) {
        unlink("/tmp/mtpfuse-debug-latest.log");
        (void)symlink(g_mtp_log_path, "/tmp/mtpfuse-debug-latest.log");
    }
    fprintf(stderr, "mtpfuse: mtp log -> %s (tail -F this path; symlink /tmp/mtpfuse-debug-latest.log)\n",
            g_mtp_log_path);
    fflush(stderr);
}

static void mtp_log_prefix_line(FILE *f)
{
    struct timespec mono, rt;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &rt);
    fprintf(f, "mono=%lld.%03ld wall=%lld.%03ld thr=%lx | ",
            (long long)mono.tv_sec, mono.tv_nsec / 1000000L, (long long)rt.tv_sec, rt.tv_nsec / 1000000L,
            (unsigned long)pthread_self());
}

void mtp_log(const char *fmt, ...)
{
    if (!g_mtp_log_f)
        return;
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_mtp_log_mu);
    mtp_log_prefix_line(g_mtp_log_f);
    vfprintf(g_mtp_log_f, fmt, ap);
    fprintf(g_mtp_log_f, "\n");
    g_mtp_log_line++;
    if (!g_mtp_log_sync && (g_mtp_log_line % 64u) == 0u)
        fflush(g_mtp_log_f);
    else if (g_mtp_log_sync)
        fflush(g_mtp_log_f);
    pthread_mutex_unlock(&g_mtp_log_mu);
    va_end(ap);
}

void mtp_log_shutdown(void)
{
    if (!g_mtp_log_f)
        return;
    pthread_mutex_lock(&g_mtp_log_mu);
    mtp_log_prefix_line(g_mtp_log_f);
    fprintf(g_mtp_log_f, "=== mtp log session end ===\n");
    fflush(g_mtp_log_f);
    fclose(g_mtp_log_f);
    g_mtp_log_f = NULL;
    if (g_mtp_log_buf) {
        free(g_mtp_log_buf);
        g_mtp_log_buf = NULL;
    }
    pthread_mutex_unlock(&g_mtp_log_mu);
}
