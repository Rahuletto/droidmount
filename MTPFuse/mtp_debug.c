#define _DARWIN_C_SOURCE 1
#include "mtp_debug.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE            *g_dbg     = NULL;
static pthread_mutex_t  g_dbg_mu = PTHREAD_MUTEX_INITIALIZER;
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
