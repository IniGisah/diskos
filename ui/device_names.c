/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "device_names.h"
#include "names.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <unistd.h>

/* Host fixtures use their own root even for openat, which the renderer does
 * not interpose. Production has no environment-controlled persistence paths. */
static int parent(const char *path, char leaf[256]){
    char full[4096];
#ifdef DISKOS_HOST
    const char *root = getenv("DISKOS_HOST_ROOT");
#else
    const char *root = NULL;
#endif
    if(snprintf(full, sizeof full, "%s%s", root ? root : "", path) >= (int)sizeof full) return -1;
    char *slash = strrchr(full, '/');
    if(!slash || !slash[1] || strlen(slash+1) >= 256) return -1;
    strcpy(leaf, slash+1); *slash = 0;
    /* Walk components with NOFOLLOW; a replaced ancestor cannot redirect I/O. */
    int fd = open("/", O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    char *save = NULL;
    for(char *p = strtok_r(full, "/", &save); fd >= 0 && p; p = strtok_r(NULL, "/", &save)){
        int next = openat(fd, p, O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        close(fd); fd = next;
    }
    return fd;
}
static int regular(const struct stat *s){ return S_ISREG(s->st_mode) && s->st_nlink == 1; }
int dn_read_file(const char *path, char *out, size_t cap, size_t *length){
    char leaf[256]; int d = parent(path, leaf);
    if(d < 0) return errno == ENOENT ? 1 : -1;
    int fd = openat(d, leaf, O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC); int saved = errno; close(d);
    if(fd < 0) return saved == ENOENT ? 1 : -1;
    struct stat before, after; size_t n = 0; int rc = -1;
    if(fstat(fd, &before) || !regular(&before) || before.st_size < 0 || (size_t)before.st_size >= cap) goto done;
    for(;;){
        ssize_t r = read(fd, out+n, cap-1-n);
        if(r < 0){ if(errno == EINTR) continue; goto done; }
        if(!r) break;
        n += (size_t)r;
        if(n == cap-1){ char extra; if(read(fd, &extra, 1) != 0) goto done; break; }
    }
    if(fstat(fd, &after) || n != (size_t)before.st_size || before.st_size != after.st_size ||
       before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
       memchr(out, 0, n)) goto done;
    out[n] = 0; if(length) *length = n; rc = 0;
done:
    if(close(fd)) rc = -1;
    return rc;
}
int dn_replace(const char *path, const char *text, size_t n){
    char leaf[256], temp[96]; int d = parent(path, leaf), fd = -1, rc = DN_FAILED;
    if(d < 0 || n > 1024 || memchr(text, 0, n)) goto done;
    struct stat st;
    if(fstatat(d, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0){ if(!regular(&st)) goto done; }
    else if(errno != ENOENT) goto done;
    /* O_EXCL chooses a unique same-directory file without trusting old debris. */
    for(unsigned i = 0; i < 100; i++){
        snprintf(temp, sizeof temp, ".diskos-name-%ld-%u", (long)getpid(), i);
        fd = openat(d, temp, O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC, 0600);
        if(fd >= 0 || errno != EEXIST) break;
    }
    if(fd < 0) goto done;
    size_t at = 0;
    while(at < n){
        ssize_t w = write(fd, text+at, n-at);
        if(w < 0 && errno == EINTR) continue;
        if(w <= 0) goto clean;
        at += (size_t)w;
    }
    if(fsync(fd)) goto clean;
    if(close(fd)){ fd = -1; goto clean; } fd = -1;
    if(renameat(d, temp, d, leaf)) goto clean;
    rc = fsync(d) ? DN_UNCERTAIN : DN_OK;
    goto done;
clean:
    if(fd >= 0) close(fd);
    unlinkat(d, temp, 0);
done:
    if(d >= 0) close(d);
    return rc;
}
int dn_remove(const char *path){
    char leaf[256]; int d = parent(path, leaf); struct stat st; int rc = DN_FAILED;
    if(d < 0) return rc;
    if(fstatat(d, leaf, &st, AT_SYMLINK_NOFOLLOW)){
        if(errno == ENOENT) rc = fsync(d) ? DN_UNCERTAIN : DN_OK;
    } else if(regular(&st) && !unlinkat(d, leaf, 0)) rc = fsync(d) ? DN_UNCERTAIN : DN_OK;
    close(d); return rc;
}
static int network(const char *s){ return !*s || names_check(NAMES_NETWORK, s, NULL) == NAMES_OK; }
int dn_save_network(const char *s){
    if(!s || !network(s)) return DN_FAILED;
    char b[42]; int n = snprintf(b, sizeof b, "v1\nname=%s\n", s);
    return dn_replace(DN_SAVED, b, (size_t)n);
}
int dn_saved_bt_valid(const char *s){ return names_saved_bt_valid(s); }
int dn_save_bt(const char *s){
    if(names_check(NAMES_BT, s, NULL) != NAMES_OK) return DN_FAILED;
    char b[34]; int n = snprintf(b, sizeof b, "%s\n", s);
    char old[256]; size_t old_n;
    if(!dn_read_file(DN_BT,old,sizeof old,&old_n) &&
       ((old_n==(size_t)n && !memcmp(old,b,old_n)) ||
        (old_n==(size_t)n-1 && !memcmp(old,b,old_n)))){
        /* An equal file may be an earlier uncertain replacement. Confirm both
         * file and directory durability without replacing the stock encoding. */
        char leaf[256]; int d=parent(DN_BT,leaf);
        if(d<0) return DN_FAILED;
        int fd=openat(d,leaf,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        int ok=fd>=0;
        if(fd>=0){
            struct stat st;
            ok=!fstat(fd,&st) && regular(&st) && !fsync(fd);
            if(close(fd)) ok=0;
        }
        if(ok && fsync(d)) ok=0;
        close(d); return ok ? DN_OK : DN_FAILED;
    }
    return dn_replace(DN_BT, b, (size_t)n);
}
static int metadata(const char *path, mode_t mode, int directory){
    char leaf[256]; int d = parent(path, leaf); struct stat s; int rc = 0;
    if(d < 0) return 0;
#ifdef DISKOS_HOST
    uid_t required_uid=geteuid(); /* unprivileged offline fixture owner */
#else
    uid_t required_uid=0;
#endif
    if(!fstatat(d, leaf, &s, AT_SYMLINK_NOFOLLOW)) rc = s.st_uid == required_uid && (s.st_mode&07777) == mode &&
        (directory ? S_ISDIR(s.st_mode) : regular(&s));
    close(d); return rc;
}
static int activation(char out[33], int *capability){
    static const char declaration[] = "v1\nfeature=network-naming\nsaved=/usr/data/diskos-network-name\nactive=/tmp/diskos-names/active\nenvironment=DISKOS_NETWORK_NAMES=v1\nreceiver-naming=no\n";
    char b[256], leaf[256]; size_t n;
    *capability = !dn_read_file("/opt/diskos/network-names-v1", b, sizeof b, &n) &&
        !strcmp(b, declaration) && metadata("/opt/diskos/network-names-v1", 0644, 0);
#ifndef DISKOS_HOST
    int d = parent("/opt/diskos/network-names-v1", leaf); struct statvfs fs; struct statfs type;
    if(d < 0 || fstatvfs(d, &fs) || !(fs.f_flag & ST_RDONLY) || fstatfs(d,&type) || type.f_type!=0x73717368) *capability = 0;
    if(d >= 0) close(d);
#else
    (void)leaf;
#endif
    const char *token = getenv("DISKOS_NETWORK_NAMES");
    if(!*capability || !token || strcmp(token, "v1") || !metadata("/tmp/diskos-names", 0755, 1) ||
       !metadata("/tmp/diskos-names/active", 0444, 0) || dn_read_file("/tmp/diskos-names/active", b, sizeof b, &n)) return 0;
#ifndef DISKOS_HOST
    d=parent("/tmp/diskos-names/active",leaf);
    if(d<0) return 0;
    int tmpfs=!fstatfs(d,&type) && type.f_type==0x01021994; close(d);
    if(!tmpfs) return 0;
#endif
    char boot[38] = {0};
    char proc[4096];
#ifdef DISKOS_HOST
    snprintf(proc, sizeof proc, "%s/proc/sys/kernel/random/boot_id", getenv("DISKOS_HOST_ROOT") ? getenv("DISKOS_HOST_ROOT") : "");
#else
    strcpy(proc, "/proc/sys/kernel/random/boot_id");
#endif
    int fd = open(proc, O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd < 0) return 0;
    ssize_t k; do { k=read(fd, boot, sizeof boot); } while(k < 0 && errno == EINTR);
    int closed=close(fd);
    if(k != 37 || boot[36] != '\n' || closed) return 0;
    boot[36]=0;
    for(int i=0;i<36;i++){
        int dash=i==8||i==13||i==18||i==23;
        if(dash ? boot[i]!='-' : !((boot[i]>='0'&&boot[i]<='9')||(boot[i]>='a'&&boot[i]<='f'))) return 0;
    }
    if(n < 51 || n > 83 || strncmp(b,"v1\nboot=",8) || memcmp(b+8,boot,36) || strncmp(b+44,"\nname=",6) || b[n-1]!='\n') return 0;
    b[n-1]=0; if(!network(b+50)) return 0;
    strcpy(out,b+50); return 1;
}
void dn_read(dn_snapshot *s){
    memset(s,0,sizeof *s); char b[256]; size_t n;
    int r=dn_read_file(DN_SAVED,b,sizeof b,&n);
    if(r==1) s->saved_valid=1;
    else if(!r && n>=9 && n<=41 && !strncmp(b,"v1\nname=",8) && b[n-1]=='\n'){
        b[n-1]=0; if(network(b+8)){ strcpy(s->saved,b+8); s->saved_valid=1; }
    }
    strcpy(s->bluetooth,DN_DEFAULT_BT);
    r=dn_read_file(DN_BT,b,sizeof b,&n);
    if(r==1) s->bluetooth_valid=1;
    else if(!r){
        if(n && b[n-1]=='\n') b[--n]=0;
        if(dn_saved_bt_valid(b)){ strcpy(s->bluetooth,b); s->bluetooth_valid=1; }
    }
    s->activated=activation(s->active,&s->capability);
    s->pending=s->saved_valid && (!s->activated || strcmp(s->saved,s->active));
}
void dn_sender_copy(char out[33]){
    int capability; char active[33]="";
    activation(active,&capability);
    strcpy(out,*active ? active : DN_DEFAULT_SENDER);
}
