/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "procrun.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* One permanent worker owns all children, pipes and deferred reaping. Never replace it.
 * Keep the leader unreaped until group cleanup, including when descendants hold stdout. */
static struct job {
    unsigned id; int done, consumed, cancel;
    char cmd[512], *out; int cap, n, fd, started;
    pid_t pid; int64_t deadline;
} jobs[PROC_SLOTS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static unsigned serial;
static int running;
static int64_t now_ms(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void finish(struct job *j, int result){
    if(j->pid > 0){ kill(-j->pid, SIGKILL); kill(j->pid, SIGKILL); }
    if(j->fd >= 0){ close(j->fd); j->fd = -1; }
    j->out[j->n] = 0;
    pthread_mutex_lock(&mu); j->done = result; pthread_mutex_unlock(&mu);
}
static void launch(struct job *j){
    int f[2]; j->started = 1;
    long fd_limit = sysconf(_SC_OPEN_MAX);
    if(fd_limit < 3 || fd_limit > INT_MAX){ finish(j, PROC_FAILED); return; }
    if(pipe2(f, O_CLOEXEC) != 0){ finish(j, PROC_FAILED); return; }
    j->pid = fork();
    if(j->pid == 0){
        if(setpgid(0, 0) != 0) _exit(126);
        if(dup2(f[1], STDOUT_FILENO) < 0) _exit(126);
        if(fcntl(STDOUT_FILENO, F_SETFD, 0) < 0) _exit(126);
        if(f[0] != STDOUT_FILENO) close(f[0]);
        if(f[1] != STDOUT_FILENO) close(f[1]);
        for(int fd = 3; fd < fd_limit; fd++) close(fd);
        execl("/bin/sh", "sh", "-c", j->cmd, (char *)0); _exit(127);
    }
    close(f[1]);
    if(j->pid < 0){ close(f[0]); finish(j, PROC_FAILED); return; }
    setpgid(j->pid, j->pid);
    j->fd = f[0];
    if(fcntl(j->fd, F_SETFL, O_NONBLOCK) < 0) finish(j, PROC_FAILED);
}
static void *worker(void *unused){
    (void)unused;
    for(;;){
        int active = 0;
        for(int i=0; i<PROC_SLOTS; i++){
            struct job *j = &jobs[i];
            pthread_mutex_lock(&mu);
            unsigned id = j->id; int cancel = j->cancel, done = j->done;
            pthread_mutex_unlock(&mu);
            if(!id) continue;
            active = 1;
            if(!done){
                if(cancel) finish(j, PROC_CANCELLED);
                else if(now_ms() >= j->deadline) finish(j, PROC_TIMED_OUT);
                else {
                    if(!j->started) launch(j);
                    if(j->fd >= 0){
                        char b[1024]; ssize_t n = read(j->fd, b, sizeof b);
                        if(now_ms() >= j->deadline) finish(j, PROC_TIMED_OUT);
                        else if(n > 0){
                            int room = j->cap - 1 - j->n;
                            int take = n > room ? room : (int)n;
                            memcpy(j->out+j->n, b, take); j->n += take;
                            if(n > room) finish(j, PROC_TRUNCATED);
                        } else if(n == 0){
                            siginfo_t si = {0};
                            if(waitid(P_PID, j->pid, &si, WEXITED|WNOHANG|WNOWAIT) == 0 && si.si_pid)
                                finish(j, si.si_code == CLD_EXITED && !si.si_status ? PROC_COMPLETE : PROC_FAILED);
                        } else if(errno != EAGAIN && errno != EINTR) finish(j, PROC_FAILED);
                    }
                }
            }
            if(j->done && j->pid > 0){
                pid_t p = waitpid(j->pid, NULL, WNOHANG);
                if(p == j->pid || (p < 0 && errno == ECHILD)) j->pid = 0;
            }
            pthread_mutex_lock(&mu);
            if(j->done && j->consumed && j->pid <= 0){ free(j->out); j->id = 0; }
            pthread_mutex_unlock(&mu);
        }
        pthread_mutex_lock(&mu);
        if(!active){
            int queued = 0;
            for(int i=0;i<PROC_SLOTS;i++) queued |= jobs[i].id != 0;
            if(!queued) pthread_cond_wait(&wake, &mu);
        }
        pthread_mutex_unlock(&mu);
        usleep(1000);
    }
    return NULL;
}
unsigned proc_start(const char *cmd, int timeout_ms, int cap){
    if(!cmd || strlen(cmd) >= sizeof jobs[0].cmd || timeout_ms <= 0 || cap < 1 || cap > PROC_OUTPUT) return 0;
    pthread_mutex_lock(&mu);
    if(!running){
        pthread_t t;
        if(pthread_create(&t, NULL, worker, NULL)){ pthread_mutex_unlock(&mu); return 0; }
        pthread_detach(t); running = 1;
    }
    unsigned id = 0;
    for(int i=0;i<PROC_SLOTS;i++) if(!jobs[i].id){
        struct job *j = &jobs[i];
        char *out = malloc(cap); if(!out) break;
        memset(j, 0, sizeof *j); j->out = out;
        if(++serial == 0) ++serial;
        id = j->id = serial; j->fd = -1; j->cap = cap;
        j->deadline = now_ms() + timeout_ms; strcpy(j->cmd, cmd); break;
    }
    pthread_cond_signal(&wake); pthread_mutex_unlock(&mu); return id;
}
int proc_poll(unsigned id, char *out, int cap){
    int r = 0; if(cap < 1) return 0;
    out[0] = 0; pthread_mutex_lock(&mu);
    for(int i=0;i<PROC_SLOTS;i++) if(id && jobs[i].id == id && !jobs[i].consumed && jobs[i].done){
        struct job *j = &jobs[i]; int n = j->n < cap-1 ? j->n : cap-1;
        memcpy(out, j->out, n); out[n] = 0;
        r = n < j->n ? PROC_TRUNCATED : j->done; j->consumed = 1; break;
    }
    pthread_mutex_unlock(&mu); return r;
}
void proc_cancel(unsigned id){
    pthread_mutex_lock(&mu);
    for(int i=0;i<PROC_SLOTS;i++) if(id && jobs[i].id == id && !jobs[i].done) jobs[i].cancel = 1;
    pthread_mutex_unlock(&mu);
}
