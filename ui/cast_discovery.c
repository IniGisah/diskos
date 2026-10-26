/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cast_discovery.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(CD_TEST_PATH) && (defined(__mips__) || defined(__mips))
#error "CD_TEST_PATH is restricted to host tests"
#endif
#ifdef CD_TEST_PATH /* host tests only; never defined by the device build */
#define BROWSER (getenv("CD_TEST_BROWSER") ? getenv("CD_TEST_BROWSER") : "/nonexistent")
#else
#define BROWSER "/usr/bin/avahi-browse"
#endif
void cast_disc_init(cast_disc *d) { memset(d, 0, sizeof *d); }
void cast_disc_cancel(cast_disc_job *j) { atomic_store(&j->cancel, 1); }

#include "avahi_text.h"
static void line_done(cast_disc *d)
{
    char *f[10], *p = d->line; cast_receiver r = {0};
    char svc[64], id[64], fn[300], md[300]; struct in_addr a;
    for (int i = 0; i < 9; i++) { /* the TXT list is the remainder: it may hold ';' */
        f[i] = p; if (!(p = strchr(p, ';'))) return;
        *p++ = 0;
    }
    f[9] = p;
    const char *s = f[3];
    if (strcmp(f[0], "=") || strcmp(f[1], "wlan0") || strcmp(f[2], "IPv4") || strcmp(f[4], "_googlecast._tcp")) return;
    if (unesc(&s, svc, sizeof svc, 0) < 1 || *s) return;
    int port = port_of(f[8]);
    if (!port || inet_pton(AF_INET, f[7], &a) != 1 || strlen(f[7]) > 15) return;
    uint32_t h = ntohl(a.s_addr);
    if (h >> 24 == 0 || h >> 24 == 127 || h >> 24 >= 224 ||
        (h & UINT32_C(0xffff0000)) == UINT32_C(0xa9fe0000)) return;
    if (txt_get(f[9], "id", id, sizeof id) < 0 || txt_get(f[9], "fn", fn, sizeof fn) < 0 ||
        txt_get(f[9], "md", md, sizeof md) < 0) return;
    if (put(r.id, sizeof r.id, *id ? id : svc, 0) || put(r.name, sizeof r.name, *fn ? fn : svc, 1) ||
        put(r.model, sizeof r.model, md, 1)) return;
    strcpy(r.addr, f[7]); r.port = (uint16_t)port;
    for (size_t i = 0; i < d->n; i++) {
        if (!strcmp(d->r[i].id, r.id)) {
            d->r[i] = r;
            return;
        }
    }
    if (d->n < CD_MAX) d->r[d->n++] = r;
}
static void flush(cast_disc *d)
{
    if (!d->bad) { d->line[d->used] = 0; if (strlen(d->line) == d->used) line_done(d); }
    d->used = 0; d->bad = 0;
}
void cast_disc_feed(cast_disc *d, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (b[i] == '\n') flush(d);
        else if (d->used + 1 >= sizeof d->line) d->bad = 1;
        else d->line[d->used++] = b[i];
    }
}
void cast_disc_end(cast_disc *d) { if (d->used || d->bad) flush(d); }

static int64_t now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
#ifdef CD_TEST_PATH
/* Host tests only: the deadline starts once the fake browser has created the
 * CD_TEST_READY file (bounded: at most 10 s of waiting), so a slow start can
 * not eat a short deadline. */
static void test_ready(const char *f, int64_t t0, unsigned ms, int *ready, int64_t *end)
{
    if (!f || *ready) return;
    int64_t t = now_ms();
    if (!access(f, F_OK)) { *ready = 1; *end = t + ms; }
    else if (t < t0 + 10000) *end = t + ms;
}
#define TEST_READY() test_ready(rdy, t0, ms, &ready, &end)
#else
#define TEST_READY() ((void)0)
#endif
/* One past the highest descriptor the child must close: the soft limit, or
 * more when an open descriptor sits above it. Parent side (getrlimit is not
 * async-signal-safe). The process must not raise RLIMIT_NOFILE concurrently
 * with a spawn. Returns -1 on failure. */
static long fd_top(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0 ||
        rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > INT_MAX)
        return -1;

    long top = (long)rl.rlim_cur;
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) return -1;

    struct dirent *de;
    for (;;) {
        errno = 0; /* only readdir's own errno counts; strtol may set EINVAL */
        if (!(de = readdir(dir))) break;
        char *end;
        long fd = strtol(de->d_name, &end, 10);
        if (end != de->d_name && !*end && fd >= top) {
            if (fd >= INT_MAX) {
                closedir(dir);
                return -1;
            }
            top = fd + 1;
        }
    }
    int failed = errno;
    closedir(dir);
    return failed ? -1 : top;
}
/* Fork/exec with a private stdout pipe and an error-report pipe (CLOEXEC:
 * EOF means exec succeeded). Returns pid or -1. Process contract: SIGCHLD
 * stays at its default (not ignored, no SA_NOCLDWAIT) and no other thread
 * reaps this child; otherwise the child's identity cannot be guaranteed, so
 * nothing is started (EINVAL). If the Disc inherits an ignored SIGCHLD,
 * normalize it once in single-threaded startup, never inside this worker. */
static pid_t spawn(int out[2], int err[2], const char *type)
{
    char *const argv[] = { "avahi-browse", "-rpt", (char *)type, NULL };
    static char *const env[] = { "LC_ALL=C", "PATH=/usr/bin:/bin", NULL };
    sigset_t none; sigemptyset(&none);
    struct sigaction chld;
    if (sigaction(SIGCHLD, NULL, &chld) < 0 ||
        chld.sa_handler != SIG_DFL ||
        (chld.sa_flags & SA_NOCLDWAIT)) {
        errno = EINVAL;
        return -1;
    }
    long top = fd_top();
    if (top < 0) return -1;
#ifdef CD_TEST_PATH
    const char *barrier = getenv("CD_TEST_BARRIER"); /* host tests: hold the child before exec */
#endif
    pid_t pid = fork();
    if (pid) return pid;
    int e, nul = open("/dev/null", O_RDWR);
    sigprocmask(SIG_SETMASK, &none, NULL);
    if (nul < 0 || dup2(nul, 0) < 0 || dup2(nul, 2) < 0 || dup2(out[1], 1) < 0) { e = errno ? errno : EIO; goto fail; }
    for (long fd = 3; fd < top; fd++) if (fd != err[1]) close((int)fd);
#ifdef CD_TEST_PATH
    if (barrier) { int bf = open(barrier, O_RDONLY); if (bf >= 0) close(bf); }
#endif
    execve(BROWSER, argv, env);
    e = errno;
fail:
    if (write(err[1], &e, sizeof e) < 0) { }
    _exit(127);
}
static void reap(pid_t pid)
{
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
}
static int run_lines(cast_disc_job *j, cast_disc *d, unsigned ms, const char *type,
                     void (*feed)(void *, const char *, size_t), void *ctx)
{
    int out[2], err[2], eof = 0, st = 0, rc = -1, e = 0, reaped = 0;
    cast_disc_init(d); atomic_store(&j->pid, 0);
    if (!ms || ms > 30000) return CD_ERROR;
    int64_t end = now_ms() + ms;
#ifdef CD_TEST_PATH
    const char *rdy = getenv("CD_TEST_READY"); int ready = 0; int64_t t0 = now_ms();
#endif
    if (atomic_load(&j->cancel)) return CD_CANCEL;
    if (pipe2(out, O_CLOEXEC) < 0) return CD_ERROR;
    if (pipe2(err, O_CLOEXEC) < 0) { close(out[0]); close(out[1]); return CD_ERROR; }
    pid_t pid = spawn(out, err, type);
    close(out[1]); close(err[1]);
    if (pid < 0) { close(out[0]); close(err[0]); return CD_ERROR; }
    atomic_store(&j->pid, pid);
    /* EOF on the error pipe = exec happened; an int = why it did not. */
    struct pollfd ep = { err[0], POLLIN, 0 };
    int n;
    for (;;) {
        if (atomic_load(&j->cancel)) {
            rc = CD_CANCEL;
            break;
        }
        TEST_READY();
        int64_t left = end - now_ms();
        if (left <= 0) {
            rc = CD_TIMEOUT;
            break;
        }
        n = poll(&ep, 1, left > 20 ? 20 : (int)left);
        if (n < 0) {
            if (errno == EINTR) continue;
            rc = CD_ERROR;
            break;
        }
        if (!n) continue;

        ssize_t k = read(err[0], &e, sizeof e);
        if (k < 0 && errno == EINTR) continue;
        if (!k) break; /* CLOEXEC: exec succeeded */
        if (k == (ssize_t)sizeof e) {
            close(err[0]);
            close(out[0]);
            reap(pid);
            return e == ENOENT || e == ENOTDIR ? CD_MISSING : CD_EXEC;
        }
        rc = CD_ERROR;
        break;
    }
    close(err[0]);
    while (rc < 0) {
        TEST_READY();
        int64_t left = end - now_ms();
        if (atomic_load(&j->cancel)) { rc = CD_CANCEL; break; }
        if (left <= 0) { rc = CD_TIMEOUT; break; }
        if (!eof) {
            struct pollfd q = { out[0], POLLIN, 0 };
            n = poll(&q, 1, left > 20 ? 20 : (int)left);
            if (n < 0 && errno != EINTR) { rc = CD_ERROR; break; }
            if (n > 0) {
                char b[512]; ssize_t g = read(out[0], b, sizeof b);
                if (g > 0) { if(feed) feed(ctx, b, (size_t)g); else cast_disc_feed(d, b, (size_t)g); }
                else if (!g) eof = 1;
                else if (errno != EINTR && errno != EAGAIN) rc = CD_ERROR;
            }
        } else {
            pid_t w = waitpid(pid, &st, WNOHANG);
            if (w == pid || (w < 0 && errno != EINTR)) { reaped = 1; if (w < 0) rc = CD_ERROR; else break; }
            else nanosleep(&(struct timespec){0, 5000000}, NULL);
        }
    }
    close(out[0]);
    if (!reaped) { kill(pid, SIGKILL); reap(pid); } /* exactly this child, never a group */
    if (rc < 0) { /* browser exited by itself */
        if (!WIFEXITED(st) || WEXITSTATUS(st)) rc = CD_DAEMON;
        else { cast_disc_end(d); rc = d->n ? CD_FOUND : CD_NONE; }
    } else if (rc == CD_TIMEOUT && d->n) rc = CD_FOUND; /* a cut-off last line is never parsed */
    if (rc != CD_FOUND) cast_disc_init(d);
    return rc;
}

int cast_disc_run(cast_disc_job *j, cast_disc *d, unsigned ms)
{
    return run_lines(j, d, ms, "_googlecast._tcp", NULL, NULL);
}
int cast_disc_run_lines(cast_disc_job *j, const char *type, unsigned ms,
                        void (*feed)(void *, const char *, size_t), void *ctx)
{
    if(!feed || (strcmp(type, "_googlecast._tcp") && strcmp(type, "_airplay._tcp") && strcmp(type, "_raop._tcp"))) return CD_ERROR;
    cast_disc ignored;
    return run_lines(j, &ignored, ms, type, feed, ctx);
}
