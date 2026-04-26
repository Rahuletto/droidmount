/*
 * mtpfuse — mount the first attached MTP/Android device as a FUSE
 * filesystem on the given mount point.
 *
 * Usage:  mtpfuse [-f] [-o opts] /Volumes/AndroidDevice
 *
 * The -f flag (foreground) is normally passed by the launcher so that
 * killing this process is enough to tear the mount down.
 */

#define FUSE_USE_VERSION 26
#include "fs_ops.h"
#include "mtp_bridge.h"

#include <fuse.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Best-effort teardown: mtp_close is not fully async-signal-safe, but avoids
 * leaving the MTP stack up after SIGTERM. Prefer exiting via unmount when possible. */
static void on_signal(int sig)
{
    fprintf(stderr, "mtpfuse: signal %d, shutting down\n", sig);
    mtp_close();
    _exit(0);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s [-f] [-o opts] <mountpoint>\n", argv[0]);
        return 2;
    }

    mtp_debug_boot(argc, argv);

    if (mtp_open() != 0) {
        mtp_debug_log("mtp_open failed");
        fprintf(stderr, "mtpfuse: failed to open MTP device\n");
        mtp_debug_shutdown();
        return 1;
    }

    /* macOS statfs can pass the full host path; bridge needs it for per-storage totals. */
    mtp_set_fuse_mount_point(argv[argc - 1]);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Inject some sensible defaults if the caller didn't override them.
     * "local"     → Finder shows it as a local volume (sidebar entry)
     * "noappledouble" → no AppleDouble/._ files (omit noapplexattr so Finder
     * can read com.apple.FinderInfo on "/" for /.VolumeIcon.icns volume icons)
     * "iosize=2097152" → 2 MiB IO size (matches GetPartialObject chunking)
     * The volname/volicon can be passed in by the launcher via -o. */
    char **fuse_argv = calloc((size_t)argc + 10, sizeof(char *));
    if (!fuse_argv) {
        fprintf(stderr, "mtpfuse: calloc fuse argv failed\n");
        mtp_close();
        return 1;
    }
    int n = 0;
    fuse_argv[n++] = argv[0];
    int saw_o = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) saw_o = 1;
        fuse_argv[n++] = argv[i];
    }
    if (!saw_o) {
        fuse_argv[n++] = (char *)"-o";
        fuse_argv[n++] = (char *)"local,noappledouble,"
                                 "iosize=2097152,volname=AndroidDevice";
    }
    fuse_argv[n] = NULL;

    mtp_debug_log("entering fuse_main (FUSE session start)");
    int rc = fuse_main(n, fuse_argv, &mtpfuse_ops, NULL);
    mtp_debug_log("fuse_main returned rc=%d", rc);
    free(fuse_argv);
    mtp_close();
    return rc;
}
