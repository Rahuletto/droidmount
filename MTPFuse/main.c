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

    if (mtp_open() != 0) {
        fprintf(stderr, "mtpfuse: failed to open MTP device\n");
        return 1;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Inject some sensible defaults if the caller didn't override them.
     * "local"     → Finder shows it as a local volume (sidebar entry)
     * "noappledouble" / "noapplexattr" → no AppleDouble/._ files
     * "iosize=1048576" → 1 MiB IO size so Finder copies don't crawl
     * The volname/volicon can be passed in by the launcher via -o. */
    char **fuse_argv = calloc(argc + 8, sizeof(char *));
    int n = 0;
    fuse_argv[n++] = argv[0];
    int saw_o = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) saw_o = 1;
        fuse_argv[n++] = argv[i];
    }
    if (!saw_o) {
        fuse_argv[n++] = (char *)"-o";
        fuse_argv[n++] = (char *)"local,noappledouble,noapplexattr,"
                                 "iosize=1048576,volname=AndroidDevice";
    }

    int rc = fuse_main(n, fuse_argv, &mtpfuse_ops, NULL);
    free(fuse_argv);
    mtp_close();
    return rc;
}
