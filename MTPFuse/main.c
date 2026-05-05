#define FUSE_USE_VERSION 26
#include "fs_ops.h"
#include "mtp_log.h"
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

    mtp_log_init(argc, argv);

    if (mtp_open() != 0) {
        mtp_log("mtp_open failed");
        fprintf(stderr, "mtpfuse: failed to open MTP device\n");
        mtp_log_shutdown();
        return 1;
    }

    mtp_set_fuse_mount_point(argv[argc - 1]);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);
    signal(SIGPIPE, SIG_IGN);

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

    mtp_log("entering fuse_main (FUSE session start)");
    int rc = fuse_main(n, fuse_argv, &mtpfuse_ops, NULL);
    mtp_log("fuse_main returned rc=%d", rc);
    mtp_log_flush();
    free(fuse_argv);
    mtp_close();
    return rc;
}
