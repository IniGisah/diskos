/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include "airplay_probe.h"
#include "sdio.h"

static int due(uint32_t now, uint32_t t){ return (int32_t)(now - t) >= 0; }
static int def_reap(pid_t p){ int st; pid_t r = waitpid(p, &st, WNOHANG); return r == p || (r < 0 && errno != EINTR); }
static int send_frame(ap_probe *a, char type, const char *pl, int fd){
    unsigned char b[16] = { (unsigned char)type, (unsigned char)a->serial, (unsigned char)(a->serial >> 8), (unsigned char)(a->serial >> 16), (unsigned char)(a->serial >> 24) };
    size_t n = 5, k = pl ? strlen(pl) : 0;
    if(a->ctl < 0 || k > 8) return -1;
    if(k){ memcpy(b + 5, pl, k); n += k; }
    struct iovec iv = { b, n };
    union { struct cmsghdr h; char c[CMSG_SPACE(sizeof(int))]; } u;
    struct msghdr m; memset(&m, 0, sizeof m); m.msg_iov = &iv; m.msg_iovlen = 1;
    if(fd >= 0){
        memset(&u, 0, sizeof u); m.msg_control = u.c; m.msg_controllen = sizeof u.c;
        struct cmsghdr *c = CMSG_FIRSTHDR(&m); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof fd); memcpy(CMSG_DATA(c), &fd, sizeof fd);
    }
    return sendmsg(a->ctl, &m, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)n ? 0 : -1;
}
struct ap_dirent64 {
    uint64_t ino; int64_t off;
    unsigned short reclen; unsigned char type; char name[];
};
static int child_close_extra(void){
    int dfd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(dfd < 0) return -1;
    char buf[2048];
    for(;;){
        long n = syscall(SYS_getdents64, dfd, buf, sizeof buf);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0){ close(dfd); return n == 0 ? 0 : -1; }
        for(long off = 0; off < n;){
            struct ap_dirent64 *d = (struct ap_dirent64 *)(buf + off);
            if(!d->reclen || d->reclen > n - off){ close(dfd); return -1; }
            int v = 0, ok = d->name[0] != 0;
            for(const char *c = d->name; *c; c++){
                if(*c < '0' || *c > '9'){ ok = 0; break; }
                v = v * 10 + (*c - '0');
            }
            if(ok && v >= 4 && v != dfd) close(v);
            off += d->reclen;
        }
    }
}
static int spawn(ap_probe *a){
    int sp[2]; char att[16]; const char *av[16]; int n = 0;
    if(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sp) < 0) return -1;
    snprintf(att, sizeof att, "%u", a->serial);
    av[n++] = a->cfg.helper; av[n++] = "--ctl"; av[n++] = "3"; av[n++] = "--attempt"; av[n++] = att;
    for(const char *const *x = a->cfg.extra; x && *x && n < 15; x++) av[n++] = *x;
    av[n] = NULL;
    pid_t p = fork();
    if(p < 0){ close(sp[0]); close(sp[1]); return -1; }
    if(p == 0){   /* async-signal-safe calls only until exec */
        sigset_t e; sigemptyset(&e); sigprocmask(SIG_SETMASK, &e, NULL);
        setpgid(0, 0);
        int c = fcntl(sp[1], F_DUPFD, 10);
        if(c < 0 || dup2(c, 3) < 0) _exit(126);
        if(child_close_extra() < 0) _exit(126);
        int z = open("/dev/null", O_RDWR);
        if(z < 0 || dup2(z, 0) < 0 || dup2(z, 1) < 0 || dup2(z, 2) < 0) _exit(126);
        if(z > 2) close(z);
        execv(av[0], (char *const *)av); _exit(127);
    }
    setpgid(p, p); close(sp[1]);
    a->ctl = sp[0]; a->pid = p; a->termed = a->faulted = 0;
    return 0;
}
/* Graceful first (cancel frame), then SIGTERM, then SIGKILL, all to the child's process group; the lease stays until the reap. */
static void term_begin(ap_probe *a){
    if(a->pid <= 0 || a->termed) return;
    a->termed = 1; a->term_at = a->now + (a->cfg.term_ms ? a->cfg.term_ms : 1500);
    (void)send_frame(a, 'C', NULL, -1);
}
static ho_sink sink(ap_probe *a);
static void feed(ap_probe *a, ho_evtype t, const ho_local *l, int owned){
    ho_sink k = sink(a); ho_event ev = { t, ho_attempt(a->h), l, "AirPlay", owned };
    ho_step(a->h, a->now, &ev, &k);
    if(a->phase == AP_RUN && ho_get(a->h) == HO_LOCAL) a->phase = AP_IDLE;
}
static void prep_end(ap_probe *a, ho_msg m){ a->phase = AP_IDLE; a->pin_wanted = 0; term_begin(a); if(m && a->cfg.say) a->cfg.say(m); }
static void go_run(ap_probe *a, const ho_local *l){   /* 'R': a FRESH snapshot starts the handoff; no cast status is ever published */
    ho_sink k = sink(a); ho_event s = { HO_EV_SELECT, 0, l, "AirPlay", 0 };
    a->pin_wanted = 0; a->phase = AP_RUN;
    ho_step(a->h, a->now, &s, &k);
    if(ho_get(a->h) != HO_PREPARING){ a->phase = AP_IDLE; term_begin(a); return; }
    feed(a, HO_EV_REMOTE_READY, l, 0);
}
static void on_frame(ap_probe *a, char t, const ho_local *l){
    int prep = a->phase == AP_PREP, run = a->phase == AP_RUN;
    if(t == 'P' && prep) a->pin_wanted = 1;
    else if(t == 'R' && prep) go_run(a, l);   /* a repeated or late READY finds the phase moved on */
    else if(t == 'S' && run){ feed(a, HO_EV_LOAD_OK, l, 0); feed(a, HO_EV_MEDIA, l, 1); }
    else if(t == 'D' && run) feed(a, HO_EV_STOP, l, 1);
    else if(t == 'F'){ if(prep) prep_end(a, HO_MSG_FAILED); else if(run) feed(a, HO_EV_REMOTE_LOST, l, 0); }
}
static void drain(ap_probe *a, const ho_local *l){
    unsigned char b[32];
    while(a->ctl >= 0){
        ssize_t n = recv(a->ctl, b, sizeof b, MSG_DONTWAIT);
        if(n == 0){ close(a->ctl); a->ctl = -1; break; }   /* the child closed its end: the reap follows */
        if(n < 0){ if(errno == EINTR) continue; break; }
        uint32_t at = n >= 5 ? (uint32_t)b[1] | (uint32_t)b[2] << 8 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 24 : 0;
        if(n >= 5 && at == a->serial) on_frame(a, (char)b[0], l);
    }
}
static void reap_step(ap_probe *a, const ho_local *l){
    if(a->pid <= 0) return;
    if((a->cfg.reap ? a->cfg.reap : def_reap)(a->pid)){
        int asked = a->termed;
        if(a->ctl >= 0) close(a->ctl);
        a->ctl = -1; a->pid = 0; a->termed = 0;
        if(a->lease){ a->lease = 0; sd_io_end(); }   /* only now: the child can hold no descriptor */
        if(a->phase == AP_PREP){ a->phase = AP_IDLE; a->pin_wanted = 0; if(!asked && a->cfg.say) a->cfg.say(HO_MSG_FAILED); }
        else if(a->phase == AP_RUN) feed(a, ho_get(a->h) == HO_STOPPING ? HO_EV_REMOTE_STOPPED : HO_EV_REMOTE_LOST, l, 0);
        return;
    }
    if(a->termed == 1 && due(a->now, a->term_at)){ kill(-a->pid, SIGTERM); kill(a->pid, SIGTERM); a->termed = 2; a->kill_at = a->now + (a->cfg.kill_ms ? a->cfg.kill_ms : 1500); }
    else if(a->termed == 2 && due(a->now, a->kill_at)){ kill(-a->pid, SIGKILL); kill(a->pid, SIGKILL); a->termed = 3; a->fault_at = a->now + (a->cfg.fault_ms ? a->cfg.fault_ms : 2000); }
    else if(a->termed == 3 && !a->faulted && due(a->now, a->fault_at)){ a->faulted = 1; sd_io_fault(); }   /* unaccounted: SD admission stays closed, the lease stays held */
}
static int s_pause(void *c){ ap_probe *a = c; return a->cfg.pause ? a->cfg.pause() : -1; }
static int s_load(void *c, uint32_t at, const char *path, long ms){   /* START: lease, then the file, then the descriptor to the child */
    ap_probe *a = c; struct stat st; char position[9]; (void)at;
    if(ms < 0 || (uint64_t)ms > UINT32_MAX || a->pid <= 0 || a->lease || !sd_io_begin()) return -1;
    snprintf(position, sizeof position, "%08X", (unsigned)ms);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if(fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || send_frame(a, 'G', position, fd) < 0){ if(fd >= 0) close(fd); sd_io_end(); return -1; }
    close(fd); a->lease = 1;
    return 0;
}
static int s_play(void *c){ (void)c; return 0; }
static int s_stop(void *c){ ap_probe *a = c; if(a->pid <= 0) return -1; (void)send_frame(a, 'C', NULL, -1); return 0; }   /* confirmed by the reap (or ho's own STOP deadline) */
static void s_revoke(void *c){ (void)c; }
static void s_release(void *c){ term_begin((ap_probe *)c); }
static void s_cast(void *c, const pb_cast_state *s){ (void)c; (void)s; }
static void s_msg(void *c, ho_msg m){ ap_probe *a = c; if(a->cfg.say) a->cfg.say(m); }
static int s_teardown(void *c){ ap_probe *a = c; return a->cfg.teardown ? a->cfg.teardown(a->h->gen) : -1; }   /* the saved generation: a restarted player is refused */
static ho_sink sink(ap_probe *a){ ho_sink k = { a, s_pause, s_load, s_play, s_stop, s_revoke, s_release, s_cast, s_msg, s_teardown }; return k; }

void ap_init(ap_probe *a, ho *h, const ap_cfg *c){ memset(a, 0, sizeof *a); a->h = h; a->cfg = *c; a->ctl = -1; }
int ap_active(const ap_probe *a){ return a->phase != AP_IDLE || a->pid > 0; }
int ap_pin_wanted(const ap_probe *a){ return a->phase == AP_PREP && a->pin_wanted; }
int ap_start(ap_probe *a, uint32_t now, const ho_local *l){
    if(ap_active(a) || ho_get(a->h) != HO_LOCAL || (a->cfg.cast_busy && a->cfg.cast_busy())) return -1;   /* the Cast ownership guard */
    if(!l || !l->have_track || !l->sd_ok) return -1;
    a->now = now; a->serial++;
    if(spawn(a) < 0) return -1;
    a->phase = AP_PREP; a->pin_wanted = 0; a->hgen = l->generation; a->hsrc = l->source_mode;
    a->deadline = now + (a->cfg.prep_ms ? a->cfg.prep_ms : AP_PREP_MS);
    return 0;
}
int ap_pin(ap_probe *a, const char *d){
    size_t n = d ? strlen(d) : 0;
    if(!ap_pin_wanted(a) || n < 4 || n > 8) return -1;
    for(size_t i = 0; i < n; i++) if(d[i] < '0' || d[i] > '9') return -1;
    if(send_frame(a, 'N', d, -1) < 0) return -1;
    a->pin_wanted = 0;
    return 0;
}
void ap_tick(ap_probe *a, uint32_t now, const ho_local *l){
    if(!ap_active(a)) return;
    a->now = now;
    if(a->phase == AP_PREP){   /* the Disc keeps playing; only a change of storage, player or source ends the preparation */
        ho_msg m = HO_MSG_NONE;
        if(due(now, a->deadline)) m = HO_MSG_FAILED;
        else if(l && !l->sd_ok) m = HO_MSG_SD;
        else if(l && l->generation != a->hgen) m = HO_MSG_RESTARTED;
        else if(l && l->source_mode != a->hsrc) m = HO_MSG_SOURCE;
        if(m) prep_end(a, m);
    }
    if(a->phase == AP_RUN && ho_get(a->h) != HO_STOPPING &&
       l && l->source_mode != a->hsrc){
        if(a->cfg.say) a->cfg.say(HO_MSG_SOURCE);
        ap_cancel(a, now);
    }
    drain(a, l);
    reap_step(a, l);
    if(a->phase == AP_RUN) feed(a, HO_EV_LOCAL, l, 0);
}
void ap_cancel(ap_probe *a, uint32_t now){   /* user Cancel, sleep, shutdown, reload */
    a->now = now;
    if(a->phase == AP_PREP) prep_end(a, HO_MSG_NONE);
    else if(a->phase == AP_RUN) feed(a, HO_EV_STOP, NULL, 1);
    if(a->phase == AP_IDLE) term_begin(a);
}
int ap_shutdown(ap_probe *a, uint32_t now, uint32_t budget_ms){
    ap_cancel(a, now);
    for(uint32_t t = 0; ap_active(a) && t < budget_ms; t += 10){ usleep(10000); now += 10; ap_tick(a, now, NULL); }
    return !ap_active(a);
}
