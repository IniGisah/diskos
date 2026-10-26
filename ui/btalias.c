/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "btalias.h"
#include "names.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
/* Reserve before spawning; never forget a child stuck in kernel I/O. A later
 * call collects it. The mutex protects both reservations and deferred reaps. */
static pthread_mutex_t reap_mu = PTHREAD_MUTEX_INITIALIZER;
static pid_t pending[8];
static int reserve(void){
    int slot = -1;
    pthread_mutex_lock(&reap_mu);
    for(int i = 0; i < 8; i++){
        if(pending[i] > 0){
            pid_t w = waitpid(pending[i], NULL, WNOHANG);
            if(w > 0 || (w < 0 && errno == ECHILD)) pending[i] = 0;
        }
        if(!pending[i] && slot < 0) slot = i;
    }
    if(slot >= 0) pending[slot] = -1;
    pthread_mutex_unlock(&reap_mu);
    return slot;
}
static int64_t now_ms(void){
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int object(const char *mac, char path[64]){
    strcpy(path, "/org/bluez/hci0");
    if(!mac) return 1;
    if(strlen(mac) != 17) return 0;
    strcat(path, "/dev_");
    for(int i = 0; i < 17; i++){
        char c = mac[i];
        if(i % 3 == 2){ if(c != ':') return 0; c = '_'; }
        else {
            if(c >= 'a' && c <= 'f') c -= 'a' - 'A';
            if(!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return 0;
        }
        path[20+i] = c;
    }
    path[37] = 0;
    return 1;
}
static btalias_result_t capture(char *const argv[], char *out, size_t capacity, int64_t end){
    int slot = reserve(), fd[2], status = 0, reaped = 0, eof = 0;
    pid_t pid = 0;
    size_t n = 0;
    btalias_result_t result = BTALIAS_ERROR;
    if(slot < 0) return result;
    if(pipe2(fd, O_CLOEXEC) != 0) goto release;
    const char *search = getenv("PATH");
    if(!search) search = "/usr/local/bin:/bin:/usr/bin";
    pid_t parent_pid=getpid();
    pid = fork();
    if(pid == 0){
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent_pid) _exit(126);
        close(fd[0]);
        if(setpgid(0, 0) != 0) _exit(126);
        if(dup2(fd[1], 1) < 0 || dup2(fd[1], 2) < 0) _exit(126);
        close(fd[1]);
        int nul = open("/dev/null", O_RDONLY);
        if(nul < 0 || dup2(nul, 0) < 0) _exit(126);
        if(nul > 2) close(nul);
        /* Search PATH without execvp's possible shell fallback on ENOEXEC. */
        for(const char *p = search;;){
            const char *q = strchr(p, ':');
            size_t n = q ? (size_t)(q-p) : strlen(p);
            char exe[4096];
            if(n + 1 + strlen(argv[0]) < sizeof exe){
                memcpy(exe, p, n);
                if(n) exe[n++] = '/';
                strcpy(exe+n, argv[0]);
                execve(exe, argv, environ);
                if(errno == ENOEXEC) _exit(127);
                if(errno != ENOENT && errno != ENOTDIR && errno != EACCES) _exit(126);
            }
            if(!q) break;
            p = q+1;
        }
        _exit(127);
    }
    close(fd[1]);
    if(pid < 0){
        pid = 0;
        close(fd[0]);
        goto release;
    }
    /* Also establish the group from the parent; timeout never waits for exec. */
    setpgid(pid, pid);
    if(fcntl(fd[0], F_SETFL, O_NONBLOCK) < 0) goto finish;
    for(;;){
        int64_t left = end - now_ms();
        if(left <= 0){ result = BTALIAS_TIMEOUT; break; }
        ssize_t r = read(fd[0], out+n, capacity-1-n);
        if(r > 0){ n += (size_t)r; if(n == capacity-1) break; continue; }
        if(r == 0) eof = 1;
        else if(errno != EAGAIN && errno != EINTR) break;
        if(eof){
            pid_t w = waitpid(pid, &status, WNOHANG);
            if(w == pid){ reaped = 1; result = BTALIAS_OK; break; }
            if(w < 0 && errno != EINTR){ reaped = errno == ECHILD; break; }
        }
        /* EOF can precede process exit. Never let a closed pipe bypass deadline. */
        struct pollfd p = {fd[0], POLLIN, 0};
        int ms = left > 5 ? 5 : (int)left;
        if(eof) poll(NULL, 0, ms); else poll(&p, 1, ms);
    }
finish:
    close(fd[0]);
    if(!reaped){
        /* Still unreaped: pid/group cannot have been reused. */
        kill(-pid, SIGKILL); kill(pid, SIGKILL);
        int64_t grace = now_ms() + 100;
        do {
            pid_t w = waitpid(pid, NULL, WNOHANG);
            if(w == pid || (w < 0 && errno == ECHILD)){ reaped = 1; break; }
            poll(NULL, 0, 1);
        } while(now_ms() < grace);
    }
    out[n] = 0;
    if(result == BTALIAS_OK && strncmp(out, "method return ", 14)){
        if(strstr(out, "org.freedesktop.DBus.Error.NoReply") ||
           strstr(out, "org.freedesktop.DBus.Error.Timeout")) result = BTALIAS_TIMEOUT;
        else if(strstr(out, "Error org.freedesktop.DBus.Error.ServiceUnknown:") ||
                strstr(out, "Error org.freedesktop.DBus.Error.NameHasNoOwner:") ||
                !strncmp(out, "Failed to open connection", 25) || !n) result = BTALIAS_ABSENT;
        else result = BTALIAS_ERROR;
    }
    if(!n && WIFEXITED(status) && WEXITSTATUS(status) == 126) result = BTALIAS_ERROR;
    if(result == BTALIAS_OK && (!WIFEXITED(status) || WEXITSTATUS(status))) result = BTALIAS_ERROR;
release:
    pthread_mutex_lock(&reap_mu);
    pending[slot] = pid > 0 && !reaped ? pid : 0;
    pthread_mutex_unlock(&reap_mu);
    return result;
}
static btalias_result_t owner_until(char out[64],int ms,int64_t end);
static btalias_result_t call(const char *owner, const char *path, int device, const char *alias,
                            const char *property, char out[4096], int timeout, int64_t end){
    if(now_ms() >= end) return BTALIAS_TIMEOUT;
    char limit[48], value[NAMES_STORED_BYTES+16], dest[80];
    snprintf(dest,sizeof dest,"--dest=%s",owner ? owner : "org.bluez");
    snprintf(limit, sizeof limit, "--reply-timeout=%d", timeout);
    if(alias) snprintf(value, sizeof value, "variant:string:%s", alias);
    char *argv[] = {"dbus-send", "--system", "--print-reply", limit, dest,
        (char *)path, alias ? "org.freedesktop.DBus.Properties.Set" : "org.freedesktop.DBus.Properties.Get",
        device ? "string:org.bluez.Device1" : "string:org.bluez.Adapter1",
        (char *)property, alias ? value : NULL, NULL};
    return capture(argv, out, 4096, end);
}
static btalias_result_t get(const char *owner, const char *path, int device, const char *prop,
                           char out[4096], int timeout, int64_t end){
    char reply[4096] = "";
    btalias_result_t r = call(owner, path, device, NULL, prop, reply, timeout, end);
    const char missing[] = "Error org.freedesktop.DBus.Error.UnknownProperty:";
    if(r == BTALIAS_ERROR && !strcmp(prop, "string:Name") &&
       !strncmp(reply, missing, sizeof missing - 1)){
        out[0] = 0;
        return BTALIAS_OK;
    }
    if(r != BTALIAS_OK) return r;
    char *p = strchr(reply, '\n');
    if(!p) return BTALIAS_ERROR;
    p++; while(*p == ' ') p++;
    if(strncmp(p, "variant", 7)) return BTALIAS_ERROR;
    p += 7; while(*p == ' ') p++;
    if(strncmp(p, "string \"", 8)) return BTALIAS_ERROR;
    p += 8;
    /* Shipped dbus-send prints raw bytes, without quote/backslash escaping
     * (v257:0x401f6c-0x401fb0). Only strip the final quote and newline. */
    size_t n = strlen(p);
    if(n < 2 || strcmp(p+n-2, "\"\n")) return BTALIAS_ERROR;
    p[n-2] = 0;
    strcpy(out, p);
    return BTALIAS_OK;
}
btalias_result_t btalias_get(const char *mac, char *out, size_t cap, int timeout_ms){
    char path[64], value[4096];
    if(!out || !cap || timeout_ms <= 0 || !object(mac, path)) return BTALIAS_INVALID;
    btalias_result_t r = get(NULL, path, mac != NULL, "string:Alias", value, timeout_ms, now_ms()+timeout_ms);
    if(r == BTALIAS_OK){
        if(strlen(value) >= cap) return BTALIAS_ERROR;
        strcpy(out, value);
    }
    return r;
}
static btalias_result_t set(const char *mac, const char *alias, int timeout_ms, int saved){
    char path[64], reply[4096], actual[4096], remote[4096];
    if(timeout_ms <= 0 || !object(mac, path) || !alias ||
       (!(mac && !*alias) && (saved ? !names_saved_bt_valid(alias) : names_check(NAMES_BT, alias, NULL) != NAMES_OK))) return BTALIAS_INVALID;
    int64_t end = now_ms()+timeout_ms;
    char owner[64], current[64];
    btalias_result_t r=owner_until(owner,timeout_ms,end);
    if(r!=BTALIAS_OK) return r;
    if(mac){
        r=call(owner,path,1,NULL,"string:Paired",reply,timeout_ms,end);
        if(r!=BTALIAS_OK) return r;
        char *p=strchr(reply,'\n');
        if(!p) return BTALIAS_ERROR;
        p++; while(*p==' ') p++;
        if(strncmp(p,"variant ",8)) return BTALIAS_ERROR;
        p+=8; while(*p==' ') p++;
        if(strcmp(p,"boolean true\n")) return BTALIAS_ERROR;
    }
    /* A replaced daemon cannot inherit the checked object's authorization.
     * Pin Set and every readback to this connection, within the same budget. */
    r=owner_until(current,timeout_ms,end);
    if(r!=BTALIAS_OK) return r;
    if(strcmp(owner,current)) return BTALIAS_ERROR;
    r = call(owner,path, mac != NULL, alias, "string:Alias", reply, timeout_ms, end);
    if(r != BTALIAS_OK) return r;
    r = get(owner, path, mac != NULL, "string:Alias", actual, timeout_ms, end);
    if(r != BTALIAS_OK) return r;
    if(mac && !*alias){
        r = get(owner, path, 1, "string:Name", remote, timeout_ms, end);
        if(r != BTALIAS_OK) return r;
        if(!*remote){
            strcpy(remote, path + 20);
            for(char *p = remote; *p; p++) if(*p == '_') *p = '-';
        }
        alias = remote;
    }
    return strcmp(actual, alias) ? BTALIAS_MISMATCH : BTALIAS_OK;
}
btalias_result_t btalias_set(const char *mac, const char *alias, int ms){ return set(mac,alias,ms,0); }
btalias_result_t btalias_replay(const char *alias, int ms){ return set(NULL,alias,ms,1); }

/* Introspection supplies child paths without remote Name/Alias strings. Parsing
 * ObjectManager's human-readable dump would let a remote newline imitate its
 * structure, because the stock dbus-send prints string contents without escapes. */
static btalias_result_t introspect(char *out, size_t cap, int timeout, int64_t end){
    char limit[48]; snprintf(limit,sizeof limit,"--reply-timeout=%d",timeout);
    char *argv[]={"dbus-send","--system","--print-reply",limit,"--dest=org.bluez","/org/bluez/hci0",
        "org.freedesktop.DBus.Introspectable.Introspect",NULL};
    return capture(argv,out,cap,end);
}
static btalias_result_t owner_until(char out[64], int ms,int64_t end){
    if(now_ms()>=end) return BTALIAS_TIMEOUT;
    char reply[4096],limit[48]; snprintf(limit,sizeof limit,"--reply-timeout=%d",ms);
    char *argv[]={"dbus-send","--system","--print-reply",limit,"--dest=org.freedesktop.DBus",
        "/org/freedesktop/DBus","org.freedesktop.DBus.GetNameOwner","string:org.bluez",NULL};
    btalias_result_t r=capture(argv,reply,sizeof reply,end);
    if(r!=BTALIAS_OK) return r;
    char *p=strchr(reply,'\n'); if(!p) return BTALIAS_ERROR;
    p++; while(*p==' ') p++;
    if(strncmp(p,"string \":",9)) return BTALIAS_ERROR;
    p+=8; size_t n=strlen(p);
    if(n<4 || n>=65 || strcmp(p+n-2,"\"\n")) return BTALIAS_ERROR;
    p[n-2]=0;
    for(char *q=p+1;*q;q++) if(!((*q>='0'&&*q<='9')||*q=='.')) return BTALIAS_ERROR;
    strcpy(out,p); return BTALIAS_OK;
}
btalias_result_t btalias_owner(char out[64],int ms){
    if(ms<=0) return BTALIAS_INVALID;
    return owner_until(out,ms,now_ms()+ms);
}
btalias_result_t btalias_paired(char (*macs)[18], size_t capacity, size_t *count, int ms){
    if(!macs || !count || !capacity || ms<=0) return BTALIAS_INVALID;
    const size_t cap=1024*1024;
    char *reply=malloc(cap); if(!reply) return BTALIAS_ERROR;
    int64_t end=now_ms()+ms;
    btalias_result_t result=introspect(reply,cap,ms,end);
    size_t n=0;
    if(result!=BTALIAS_OK) goto done;
    char *p=strchr(reply,'\n');
    char *tail=p ? strstr(p,"</node>") : NULL;
    if(!tail || (strcmp(tail,"</node>\"\n") && strcmp(tail,"</node>\n\"\n"))) goto malformed;
    /* These fixed node tags are generated by BlueZ, not remote properties. */
    for(; (p=strstr(p,"<node name=")); p++){
        p+=11; char quote=*p++;
        if(quote!='\"' && quote!='\'') goto malformed;
        char *q=strchr(p,quote); if(!q) goto malformed;
        if((size_t)(q-p)!=21 || strncmp(p,"dev_",4)){
            /* The root may carry the adapter's absolute name. Other direct
             * child types do not represent Device1 and are ignored. */
            if(!strncmp(p,"dev_",4)) goto malformed;
            continue;
        }
        if(strncmp(q+1,"/>",2) && strncmp(q+1," />",3)) goto malformed;
        char mac[18],path[64],value[4096];
        for(int i=0;i<17;i++) mac[i]=p[4+i]=='_' ? ':' : p[4+i];
        mac[17]=0;
        if(!object(mac,path) || memcmp(path+20,p+4,17)) goto malformed;
        result=call(NULL,path,1,NULL,"string:Paired",value,ms,end);
        if(result!=BTALIAS_OK) goto done;
        char *v=strchr(value,'\n'); if(!v) goto malformed;
        v++; while(*v==' ') v++;
        if(strncmp(v,"variant",7)) goto malformed;
        v+=7; while(*v==' ') v++;
        if(!strcmp(v,"boolean true\n")){
            if(n>=capacity) goto malformed;
            for(size_t i=0;i<n;i++) if(!strcmp(macs[i],mac)) goto malformed;
            strcpy(macs[n++],mac);
        } else if(strcmp(v,"boolean false\n")) goto malformed;
    }
    *count=n; result=BTALIAS_OK; goto done;
malformed: result=BTALIAS_ERROR;
done: free(reply); return result;
}
