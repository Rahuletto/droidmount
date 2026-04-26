/*
 * adb_subprocess.c — spawn adb with no shell; timeouts on blocked I/O.
 *
 * Locking (audit):
 *   • adb_short_mu wraps only **short** `adb shell …` captures (adb_run_capture).
 *   • adb push/pull **release the mutex immediately after posix_spawn succeeds** and
 *     wait for the child **without** holding the mutex so (a) other stat/ls/shell
 *     probes can run during a multi‑GB transfer, and (b) multiple push/pull
 *     transfers can overlap (Finder parallel copies) — the adb server / USB
 *     stack may still serialize on the device, but the host is no longer
 *     artificially single‑flight for long I/O.
 *   • adb_exec_out_* helpers never take adb_short_mu (streaming / large pipes).
 *
 * Long I/O also shares a small pool (ANDROIDMOUNT_ADB_MAX_PARALLEL, default 4):
 * cat-to-fd, large exec-out script reads, push, and pull each take a slot so
 * several files can transfer in parallel without spawning unbounded adb children.
 *
 * Streaming: `adb push` already streams from disk over the sync connection; we
 * do not re‑implement chunking here unless we need stdin‑based uploads later.
 */

#define _DARWIN_C_SOURCE 1
#include "adb_subprocess.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static const char *g_serial;

/* Serialize short `adb shell` captures only. Long push/pull wait **outside** this. */
static pthread_mutex_t adb_short_mu = PTHREAD_MUTEX_INITIALIZER;

/* Cap concurrent long adb transfers (push/pull/exec-out bulk). */
static pthread_once_t adb_lim_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t adb_lim_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t adb_lim_cv = PTHREAD_COND_INITIALIZER;
static int adb_lim_max;
static int adb_lim_cur;

static void adb_lim_init_impl(void)
{
    const char *e = getenv("ANDROIDMOUNT_ADB_MAX_PARALLEL");
    int m = 4;
    if (e && e[0]) {
        int v = atoi(e);
        if (v == 0)
            m = 999;
        else if (v > 0 && v < 1000)
            m = v;
    }
    adb_lim_max = m;
    adb_lim_cur = 0;
}

static void adb_long_slot_wait(void)
{
    pthread_once(&adb_lim_once, adb_lim_init_impl);
    pthread_mutex_lock(&adb_lim_mu);
    while (adb_lim_cur >= adb_lim_max)
        pthread_cond_wait(&adb_lim_cv, &adb_lim_mu);
    adb_lim_cur++;
    pthread_mutex_unlock(&adb_lim_mu);
}

static void adb_long_slot_post(void)
{
    pthread_mutex_lock(&adb_lim_mu);
    if (adb_lim_cur > 0)
        adb_lim_cur--;
    pthread_cond_signal(&adb_lim_cv);
    pthread_mutex_unlock(&adb_lim_mu);
}

const char *adb_serial(void)
{
    return g_serial;
}

void adb_set_serial_from_env(void)
{
    const char *e = getenv("ADB_SERIAL");
    g_serial = (e && e[0]) ? e : NULL;
}

static int reap_pid(pid_t pid, int sec)
{
    for (int i = 0; i < sec * 20; i++) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (r < 0)
            return -1;
        usleep(50000);
    }
    kill(pid, SIGTERM);
    usleep(150000);
    kill(pid, SIGKILL);
    (void)waitpid(pid, NULL, 0);
    return -1;
}

static int adb_run_capture_impl(const char *const *argv_tail, char *out, size_t cap, size_t *out_len,
                                int timeout_sec)
{
    if (out_len)
        *out_len = 0;
    if (!out || cap == 0)
        return -EINVAL;
    out[0] = '\0';
    if (!g_serial || !g_serial[0])
        return -ENODEV;

    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -errno;

    size_t ntail = 0;
    while (argv_tail[ntail])
        ntail++;

    char **argv = calloc(ntail + 4, sizeof(char *));
    if (!argv) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -ENOMEM;
    }
    size_t k = 0;
    argv[k++] = (char *)"adb";
    argv[k++] = (char *)"-s";
    argv[k++] = (char *)g_serial;
    for (size_t i = 0; i < ntail; i++)
        argv[k++] = (char *)argv_tail[i];
    argv[k] = NULL;

    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        free(argv);
        close(pipefd[0]);
        close(pipefd[1]);
        return -errno;
    }
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid;
    int pe = posix_spawnp(&pid, "adb", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    free(argv);
    close(pipefd[1]);
    if (pe != 0) {
        close(pipefd[0]);
        return -pe;
    }

    size_t total = 0;
    int tlim = timeout_sec > 0 ? timeout_sec * 1000 : 120000;
    int spent = 0;

    while (total + 1 < cap) {
        struct pollfd p = { .fd = pipefd[0], .events = POLLIN, .revents = 0 };
        int pr = poll(&p, 1, 250);
        if (pr < 0) {
            int e = errno;
            kill(pid, SIGKILL);
            (void)waitpid(pid, NULL, 0);
            close(pipefd[0]);
            return -e;
        }
        spent += 250;
        if (spent > tlim) {
            kill(pid, SIGTERM);
            usleep(150000);
            kill(pid, SIGKILL);
            (void)waitpid(pid, NULL, 0);
            close(pipefd[0]);
            return -ETIMEDOUT;
        }
        if (pr == 0) {
            if (waitpid(pid, NULL, WNOHANG) == pid) {
                close(pipefd[0]);
                return -EIO;
            }
            continue;
        }
        ssize_t r = read(pipefd[0], out + total, cap - 1 - total);
        if (r < 0) {
            int e = errno;
            kill(pid, SIGKILL);
            (void)waitpid(pid, NULL, 0);
            close(pipefd[0]);
            return -e;
        }
        if (r == 0)
            break;
        total += (size_t)r;
    }
    close(pipefd[0]);

    int code = reap_pid(pid, 10);
    if (code != 0)
        return -EIO;
    out[total] = '\0';
    if (out_len)
        *out_len = total;
    return 0;
}

int adb_run_capture(const char *const *argv_tail, char *out, size_t cap, size_t *out_len,
                    int timeout_sec)
{
    if (out_len)
        *out_len = 0;
    if (!out || cap == 0)
        return -EINVAL;
    if (!g_serial || !g_serial[0])
        return -ENODEV;
    pthread_mutex_lock(&adb_short_mu);
    int r = adb_run_capture_impl(argv_tail, out, cap, out_len, timeout_sec);
    pthread_mutex_unlock(&adb_short_mu);
    return r;
}

int adb_exec_out_cat_to_fd(const char *remote_path, int write_fd, int timeout_sec)
{
    if (!remote_path || !g_serial || write_fd < 0)
        return -EINVAL;

    adb_long_slot_wait();

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        adb_long_slot_post();
        return -errno;
    }

    char *spawn_argv[] = {
        (char *)"adb", (char *)"-s", (char *)g_serial, (char *)"exec-out", (char *)"cat", (char *)"--",
        (char *)remote_path, NULL,
    };

    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        adb_long_slot_post();
        return -errno;
    }
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid;
    int pe = posix_spawnp(&pid, "adb", &fa, NULL, spawn_argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    if (pe != 0) {
        close(pipefd[0]);
        adb_long_slot_post();
        return -pe;
    }

    enum { cat_buf_sz = 1 << 20 };
    char *buf = malloc(cat_buf_sz);
    if (!buf) {
        kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        close(pipefd[0]);
        adb_long_slot_post();
        return -ENOMEM;
    }

    int tlim = timeout_sec > 0 ? timeout_sec * 1000 : 6 * 3600000;
    int spent = 0;
    int ret = 0;

    for (;;) {
        struct pollfd p = { .fd = pipefd[0], .events = POLLIN, .revents = 0 };
        int pr = poll(&p, 1, 250);
        if (pr < 0) {
            int e = errno;
            kill(pid, SIGKILL);
            (void)waitpid(pid, NULL, 0);
            close(pipefd[0]);
            ret = -e;
            goto cat_done;
        }
        spent += 250;
        if (spent > tlim) {
            kill(pid, SIGTERM);
            usleep(150000);
            kill(pid, SIGKILL);
            (void)waitpid(pid, NULL, 0);
            close(pipefd[0]);
            ret = -ETIMEDOUT;
            goto cat_done;
        }
        if (pr == 0)
            continue;

        ssize_t r = read(pipefd[0], buf, cat_buf_sz);
        if (r < 0) {
            int e = errno;
            kill(pid, SIGKILL);
            (void)waitpid(pid, NULL, 0);
            close(pipefd[0]);
            ret = -e;
            goto cat_done;
        }
        if (r == 0)
            break;
        size_t off = 0;
        while (off < (size_t)r) {
            ssize_t w = write(write_fd, buf + off, (size_t)r - off);
            if (w < 0) {
                int e = errno;
                kill(pid, SIGKILL);
                (void)waitpid(pid, NULL, 0);
                close(pipefd[0]);
                ret = -e;
                goto cat_done;
            }
            off += (size_t)w;
        }
    }
    close(pipefd[0]);
    int code = reap_pid(pid, 60);
    ret = (code != 0) ? -EIO : 0;
cat_done:
    free(buf);
    adb_long_slot_post();
    return ret;
}

int adb_exec_out_script_read(const char *script, unsigned char *buf, size_t want, int timeout_sec,
                             size_t *got_out)
{
    if (!script || !buf || want == 0)
        return -EINVAL;
    if (got_out)
        *got_out = 0;
    if (!g_serial || !g_serial[0])
        return -ENODEV;

    int slot_held = 0;
    if (want >= 262144) {
        adb_long_slot_wait();
        slot_held = 1;
    }

    int ret = 0;
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        ret = -errno;
        goto script_out;
    }

    char *spawn_argv[] = {
        (char *)"adb", (char *)"-s", (char *)g_serial, (char *)"exec-out", (char *)"sh", (char *)"-c",
        (char *)script, NULL,
    };

    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        ret = -errno;
        goto script_out;
    }
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid;
    int pe = posix_spawnp(&pid, "adb", &fa, NULL, spawn_argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    if (pe != 0) {
        close(pipefd[0]);
        ret = -pe;
        goto script_out;
    }

    size_t total = 0;
    int tlim = timeout_sec > 0 ? timeout_sec * 1000 : 3600000;
    int spent = 0;

    while (total < want) {
        struct pollfd p = { .fd = pipefd[0], .events = POLLIN, .revents = 0 };
        int pr = poll(&p, 1, 250);
        if (pr < 0) {
            int e = errno;
            kill(pid, SIGKILL);
            (void)waitpid(pid, NULL, 0);
            close(pipefd[0]);
            ret = -e;
            goto script_out;
        }
        spent += 250;
        if (spent > tlim) {
            kill(pid, SIGTERM);
            usleep(150000);
            kill(pid, SIGKILL);
            (void)waitpid(pid, NULL, 0);
            close(pipefd[0]);
            ret = -ETIMEDOUT;
            goto script_out;
        }
        if (pr == 0) {
            if (waitpid(pid, NULL, WNOHANG) == pid)
                break;
            continue;
        }

        ssize_t r = read(pipefd[0], buf + total, want - total);
        if (r < 0) {
            int e = errno;
            kill(pid, SIGKILL);
            (void)waitpid(pid, NULL, 0);
            close(pipefd[0]);
            ret = -e;
            goto script_out;
        }
        if (r == 0)
            break;
        total += (size_t)r;
    }
    close(pipefd[0]);
    (void)reap_pid(pid, 5);
    if (got_out)
        *got_out = total;
    ret = 0;
script_out:
    if (slot_held)
        adb_long_slot_post();
    return ret;
}

int adb_push_from_file(const char *local_path, const char *remote_path, int timeout_sec)
{
    if (!local_path || !remote_path || !g_serial || !g_serial[0])
        return -EINVAL;

    adb_long_slot_wait();

    pthread_mutex_lock(&adb_short_mu);

    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        pthread_mutex_unlock(&adb_short_mu);
        adb_long_slot_post();
        return -errno;
    }
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    char *argv[] = {
        (char *)"adb", (char *)"-s", (char *)g_serial, (char *)"push", (char *)local_path, (char *)remote_path, NULL,
    };

    pid_t pid;
    int pe = posix_spawnp(&pid, "adb", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (pe != 0) {
        pthread_mutex_unlock(&adb_short_mu);
        adb_long_slot_post();
        return -pe;
    }
    pthread_mutex_unlock(&adb_short_mu);

    int lim_sec = timeout_sec > 0 ? timeout_sec : 7200;
    for (int i = 0; i < lim_sec * 20 + 400; i++) {
        int st = 0;
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            int rc = (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -EIO;
            adb_long_slot_post();
            return rc;
        }
        if (w < 0) {
            int e = -errno;
            adb_long_slot_post();
            return e;
        }
        usleep(50000);
    }
    kill(pid, SIGTERM);
    usleep(200000);
    kill(pid, SIGKILL);
    (void)waitpid(pid, NULL, 0);
    adb_long_slot_post();
    return -ETIMEDOUT;
}

int adb_pull_to_file(const char *remote_path, const char *local_path, int timeout_sec)
{
    if (!remote_path || !local_path || !g_serial || !g_serial[0])
        return -EINVAL;

    (void)unlink(local_path);

    adb_long_slot_wait();

    pthread_mutex_lock(&adb_short_mu);

    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        pthread_mutex_unlock(&adb_short_mu);
        adb_long_slot_post();
        return -errno;
    }
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    char *argv[] = {
        (char *)"adb", (char *)"-s", (char *)g_serial, (char *)"pull", (char *)remote_path, (char *)local_path, NULL,
    };

    pid_t pid;
    int pe = posix_spawnp(&pid, "adb", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (pe != 0) {
        pthread_mutex_unlock(&adb_short_mu);
        adb_long_slot_post();
        return -pe;
    }
    pthread_mutex_unlock(&adb_short_mu);

    int lim_sec = timeout_sec > 0 ? timeout_sec : 7200;
    for (int i = 0; i < lim_sec * 20 + 400; i++) {
        int st = 0;
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            int rc = (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -EIO;
            adb_long_slot_post();
            return rc;
        }
        if (w < 0) {
            int e = -errno;
            adb_long_slot_post();
            return e;
        }
        usleep(50000);
    }
    kill(pid, SIGTERM);
    usleep(200000);
    kill(pid, SIGKILL);
    (void)waitpid(pid, NULL, 0);
    adb_long_slot_post();
    return -ETIMEDOUT;
}
