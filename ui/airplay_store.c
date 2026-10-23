/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_store.h"
#include "airplay_crypto.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
/* Explicit byte record, no native struct padding or host endian fields. */
#define RECORD 238
static int valid(const aps_identity *k){
    if(strnlen(k->client_id, sizeof k->client_id) != 36 || !k->peer_id[0] ||
       strnlen(k->peer_id, sizeof k->peer_id) >= sizeof k->peer_id) return 0;
    for(int i = 0; i < 36; i++){
        char c = k->client_id[i];
        if(i == 8 || i == 13 || i == 18 || i == 23){ if(c != '-') return 0; }
        else if(!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}
static int name_for(const char *id, char out[80]){
    uint8_t h[64]; static const char hex[] = "0123456789abcdef";
    size_t n = id ? strnlen(id, 129) : 0;
    if(!n || n > 128 || aps_hash(id, n, h)) return -1;
    memcpy(out, "airplay-", 8);
    for(int i = 0; i < 32; i++){ out[8 + 2*i] = hex[h[i] >> 4]; out[9 + 2*i] = hex[h[i] & 15]; }
    memcpy(out + 72, ".key", 5); return 0;
}
static int directory(int fd){
    struct stat st;
    return fstat(fd, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022) ? -1 : 0;
}
int aps_identity_new(aps_identity *k){
    uint8_t b[16]; static const char hex[] = "0123456789ABCDEF";
    memset(k, 0, sizeof *k);
    if(aps_random(k->seed, 32) || aps_random(b, sizeof b)){ aps_wipe(k, sizeof *k); return -1; }
    b[6] = (b[6] & 15) | 0x40; b[8] = (b[8] & 63) | 0x80;
    int j = 0;
    for(int i = 0; i < 16; i++){
        if(i == 4 || i == 6 || i == 8 || i == 10) k->client_id[j++] = '-';
        k->client_id[j++] = hex[b[i] >> 4]; k->client_id[j++] = hex[b[i] & 15];
    }
    aps_wipe(b, sizeof b); return 0;
}
int aps_store_load(int d, const char *id, aps_identity *k){
    uint8_t b[RECORD], h[64]; char name[80]; struct stat st; int rc = -1;
    aps_wipe(k, sizeof *k);
    if(directory(d) || name_for(id, name)) return -1;
    int fd = openat(d, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if(fd < 0) return errno == ENOENT ? 1 : -1;
    if(fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
       (st.st_mode & 0777) != 0600 || st.st_nlink != 1 || st.st_size != RECORD) goto end;
    size_t at = 0;
    while(at < sizeof b){
        ssize_t n = read(fd, b + at, sizeof b - at);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) goto end;
        at += (size_t)n;
    }
    if(memcmp(b, "APS1", 4) || aps_hash(b, RECORD - 64, h) || !aps_equal(h, b + RECORD - 64, 64)) goto end;
    memcpy(k->client_id, b + 4, 37); memcpy(k->peer_id, b + 41, 65);
    memcpy(k->seed, b + 106, 32); memcpy(k->peer_key, b + 138, 32);
    /* Reserved record bytes must be zero. */
    if(b[170] || b[171] || b[172] || b[173] || !valid(k)) goto end;
    rc = 0;
end:
    close(fd); aps_wipe(b, sizeof b); aps_wipe(h, sizeof h);
    if(rc) aps_wipe(k, sizeof *k);
    return rc;
}
int aps_store_save(int d, const char *id, const aps_identity *k){
    uint8_t b[RECORD] = {0}, random[8]; char name[80], temp[112]; int rc = -1;
    if(directory(d) || name_for(id, name) || !valid(k) || aps_random(random, sizeof random)) return -1;
    snprintf(temp, sizeof temp, "%s.%02x%02x%02x%02x%02x%02x%02x%02x.new", name,
             random[0],random[1],random[2],random[3],random[4],random[5],random[6],random[7]);
    int fd = openat(d, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if(fd < 0) return -1;
    memcpy(b, "APS1", 4); memcpy(b + 4, k->client_id, 37); memcpy(b + 41, k->peer_id, 65);
    memcpy(b + 106, k->seed, 32); memcpy(b + 138, k->peer_key, 32);
    if(aps_hash(b, RECORD - 64, b + RECORD - 64) || fchmod(fd, 0600)) goto end;
    size_t at = 0;
    while(at < sizeof b){
        ssize_t n = write(fd, b + at, sizeof b - at);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) goto end;
        at += (size_t)n;
    }
    if(fsync(fd) || renameat(d, temp, d, name) || fsync(d)) goto end;
    rc = 0;
end:
    close(fd); if(rc) unlinkat(d, temp, 0);
    aps_wipe(b, sizeof b); aps_wipe(random, sizeof random); return rc;
}
int aps_store_forget(int d, const char *id){
    char name[80];
    if(directory(d) || name_for(id, name)) return -1;
    if(unlinkat(d, name, 0) && errno != ENOENT) return -1;
    return fsync(d) ? -1 : 0;
}
