/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent DACP subset documented in airplay-spec/audio/remote_control. */
#include <netinet/in.h>
#include "airplay_remote.h"
#include "airplay_crypto.h"
#include "airplay_rtsp.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <poll.h>
#include <unistd.h>
#ifndef APS_AVAHI_PUBLISH
#define APS_AVAHI_PUBLISH "/usr/bin/avahi-publish-service"
#endif
void aps_remote_init(aps_remote *r){
    memset(r, 0, sizeof *r); r->listener = -1;
    for(unsigned i = 0; i < 2; i++) r->clients[i].fd = -1;
}
static int value(const char *p, int64_t *out){
    uint64_t v = 0; if(!*p) return -1;
    for(; *p; p++){
        if(*p < '0' || *p > '9' || v > (INT32_MAX - (unsigned)(*p - '0')) / 10) return -1;
        v = v * 10 + (unsigned)(*p - '0');
    }
    *out = (int64_t)v; return 0;
}
static int percent(const char *p, int64_t *out){
    unsigned whole = 0, fraction = 0, scale = 1, digits = 0;
    if(*p < '0' || *p > '9') return -1;
    while(*p >= '0' && *p <= '9'){ whole = whole * 10 + (unsigned)(*p++ - '0'); if(whole > 100) return -1; }
    if(*p == '.'){
        p++;
        while(*p >= '0' && *p <= '9'){
            if(++digits > 9) return -1;
            fraction = fraction * 10 + (unsigned)(*p++ - '0'); scale *= 10;
        }
        if(!digits) return -1;
    }
    if(*p || (whole == 100 && fraction)) return -1;
    *out = whole + (fraction >= (scale + 1) / 2); return 0;
}
int aps_remote_parse(const char *request, size_t n, const char *token, aps_message *m){
    if(!request || !n || n >= 2048 || memchr(request, 0, n)) return -1;
    char bounded[2048]; memcpy(bounded, request, n); bounded[n] = 0; request = bounded;
    const char *head = strstr(request, "\r\n"), *tail = strstr(request, "\r\n\r\n");
    if(!head || !tail || tail + 4 != request + n) return -1;
    if(strncmp(request, "GET /ctrl-int/1/", 16)) return -1;
    const char *space = strchr(request + 16, ' ');
    if(!space || space >= head || (strncmp(space, " HTTP/1.1\r\n", 11) && strncmp(space, " HTTP/1.0\r\n", 11))) return -1;
    char command[256]; size_t cn = (size_t)(space - request - 16);
    if(!cn || cn >= sizeof command) return -1;
    memcpy(command, request + 16, cn); command[cn] = 0;
    int authorized = 0;
    for(const char *p = head + 2; p < tail;){
        const char *end = strstr(p, "\r\n"), *colon = strchr(p, ':');
        if(!end || !colon || colon >= end) return -1;
        size_t kn = (size_t)(colon - p); const char *v = colon + 1;
        while(v < end && (*v == ' ' || *v == '\t')) v++;
        size_t vn = (size_t)(end - v);
        if(kn == 13 && !strncasecmp(p, "Active-Remote", 13)){
            if(authorized || vn != strlen(token) || !aps_equal(v, token, vn)) return -1;
            authorized = 1;
        } else if((kn == 14 && !strncasecmp(p, "Content-Length", 14)) || (kn == 17 && !strncasecmp(p, "Transfer-Encoding", 17))) return -1;
        p = end + 2;
    }
    if(!authorized) return -1;
    memset(m, 0, sizeof *m); m->type = APS_COMMAND; m->play = -1;
    if(!strcmp(command, "playpause")) m->command = PB_CMD_TOGGLE;
    else if(!strcmp(command, "play") || !strcmp(command, "playresume")){ m->command = PB_CMD_TOGGLE; m->play = 1; }
    else if(!strcmp(command, "pause")){ m->command = PB_CMD_TOGGLE; m->play = 0; }
    else if(!strcmp(command, "mutetoggle")) m->command = APS_MUTE;
    else if(!strcmp(command, "stop")) m->command = APS_REMOTE_STOP;
    else if(!strcmp(command, "nextitem")) m->command = PB_CMD_NEXT;
    else if(!strcmp(command, "previtem")) m->command = PB_CMD_PREV;
    else if(!strcmp(command, "volumeup") || !strcmp(command, "volumedown")){
        m->command = PB_CMD_VOLUME; m->position_ms = !strcmp(command, "volumeup") ? -1 : -2;
    } else if(!strncmp(command, "setproperty?dacp.playingtime=", strlen("setproperty?dacp.playingtime="))){
        m->command = PB_CMD_SEEK; if(value(command + strlen("setproperty?dacp.playingtime="), &m->position_ms)) return -1;
    } else if(!strncmp(command, "setproperty?dmcp.volume=", strlen("setproperty?dmcp.volume="))){
        m->command = PB_CMD_VOLUME; if(percent(command + strlen("setproperty?dmcp.volume="), &m->position_ms)) return -1;
    } else return -1;
    return 0;
}
int aps_remote_start(aps_remote *r, const char *local, const char *remote){
    struct sockaddr_in bind_addr = { .sin_family = AF_INET }; socklen_t len = sizeof bind_addr;
    uint8_t random[12]; static const char hex[] = "0123456789ABCDEF";
    if(r->listener >= 0 || inet_pton(AF_INET, local, &bind_addr.sin_addr) != 1 ||
       inet_pton(AF_INET, remote, &r->peer) != 1 || aps_random(random, sizeof random)) return -1;
    for(unsigned i = 0; i < 8; i++){ r->id[2*i] = hex[random[i] >> 4]; r->id[2*i+1] = hex[random[i] & 15]; }
    uint32_t token; memcpy(&token, random + 8, 4); if(!token) token = 1;
    snprintf(r->token, sizeof r->token, "%u", token);
    r->listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if(r->listener < 0 || bind(r->listener, (struct sockaddr *)&bind_addr, sizeof bind_addr) ||
       listen(r->listener, 2) || getsockname(r->listener, (struct sockaddr *)&bind_addr, &len)) goto fail;
    char service[64], port[8], db[32];
    snprintf(service, sizeof service, "iTunes_Ctrl_%s", r->id); snprintf(port, sizeof port, "%u", ntohs(bind_addr.sin_port));
    snprintf(db, sizeof db, "DbId=%s", r->id);
    char *const argv[] = { "avahi-publish-service", service, "_dacp._tcp", port, "txtvers=1", "Ver=131075", db, NULL };
    int ready[2]; if(pipe2(ready, O_CLOEXEC | O_NONBLOCK)) goto fail;
    pid_t owner = getpid(), pid = fork();
    if(pid < 0){ close(ready[0]); close(ready[1]); goto fail; }
    if(!pid){
        close(ready[0]);
        if(prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != owner){
            int error = errno ? errno : ECHILD; (void)write(ready[1], &error, sizeof error); _exit(126);
        }
        /* Keep only the exec-error pipe; CLOEXEC closes it on success. */
        for(int fd = 3; fd < 1024; fd++) if(fd != ready[1]) close(fd);
        execv(APS_AVAHI_PUBLISH, argv);
        int error = errno; (void)write(ready[1], &error, sizeof error); _exit(127);
    }
    close(ready[1]);
    struct pollfd status = { ready[0], POLLIN | POLLHUP, 0 };
    int polled; do { polled = poll(&status, 1, 1000); } while(polled < 0 && errno == EINTR);
    int error; ssize_t result = read(ready[0], &error, sizeof error); close(ready[0]);
    if(polled <= 0 || result != 0){
        kill(pid, SIGKILL); int code; while(waitpid(pid, &code, 0) < 0 && errno == EINTR){}
        goto fail;
    }
    r->publisher = pid; aps_wipe(random, sizeof random); return 0;
fail:
    aps_remote_close(r); return -1;
}
int aps_remote_service(aps_remote *r){
    if(r->listener < 0) return 0;
    if(r->publisher > 0){ int status; pid_t pid = waitpid(r->publisher, &status, WNOHANG); if(pid == r->publisher){ r->publisher = 0; return -1; } if(pid < 0 && errno != EINTR) return -1; }
    for(unsigned i = 0; i < 2; i++){
        aps_remote_client *c = &r->clients[i];
        if(c->fd < 0){
            struct sockaddr_in from; socklen_t len = sizeof from;
            int fd = accept4(r->listener, (struct sockaddr *)&from, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if(fd < 0) continue;
            if(from.sin_family != AF_INET || from.sin_addr.s_addr != r->peer.s_addr){ close(fd); continue; }
            c->fd = fd; c->used = c->reply_at = 0; c->answered = 0; c->request[0] = 0; c->deadline = aps_now_ms() + 1000;
        }
        int closed = 0;
        if(!c->answered){
            ssize_t n = recv(c->fd, c->request + c->used, sizeof c->request - c->used - 1, MSG_DONTWAIT);
            if(n > 0){ c->used += (size_t)n; c->request[c->used] = 0; }
            closed = n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK);
            if(strstr(c->request, "\r\n\r\n")){
                aps_message m; int valid = !aps_remote_parse(c->request, c->used, r->token, &m) && r->n < 8;
                if(valid) r->commands[r->n++] = m;
                c->answered = valid ? 1 : 2; c->reply_at = 0;
            }
        }
        if(c->answered){
            const char *response = c->answered == 1 ? "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" :
                                                     "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            size_t length = strlen(response);
            ssize_t n = send(c->fd, response + c->reply_at, length - c->reply_at, MSG_DONTWAIT | MSG_NOSIGNAL);
            if(n > 0) c->reply_at += (size_t)n;
            else if(!n || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) closed = 1;
            if(c->reply_at == length) closed = 1;
        }
        if(closed || (!c->answered && c->used + 1 >= sizeof c->request) || aps_now_ms() >= c->deadline){ close(c->fd); c->fd = -1; aps_wipe(c->request, sizeof c->request); }
    }
    return 0;
}
int aps_remote_take(aps_remote *r, aps_message *m){
    if(!r->n) return 0;
    *m = r->commands[0]; --r->n; memmove(r->commands, r->commands + 1, r->n * sizeof *m); return 1;
}
void aps_remote_close(aps_remote *r){
    if(r->listener >= 0) close(r->listener);
    r->listener = -1;
    for(unsigned i = 0; i < 2; i++) if(r->clients[i].fd >= 0){ close(r->clients[i].fd); r->clients[i].fd = -1; }
    if(r->publisher > 0){
        kill(r->publisher, SIGKILL); int status;
        while(waitpid(r->publisher, &status, 0) < 0 && errno == EINTR){}
        r->publisher = 0;
    }
    aps_wipe(r->token, sizeof r->token); r->n = 0;
}
