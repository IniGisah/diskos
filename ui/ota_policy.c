#define _POSIX_C_SOURCE 200809L
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ota_policy.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int boot_valid(const char b[37]){
    for(int i = 0; i < 36; i++){
        if(i == 8 || i == 13 || i == 18 || i == 23){
            if(b[i] != '-') return 0;
        } else if(!((b[i] >= '0' && b[i] <= '9') ||
                    (b[i] >= 'a' && b[i] <= 'f'))) return 0;
    }
    return b[36] == 0;
}
static int record_valid(const ota_policy_record_t *r, uint64_t *seconds){
    if(r->state[0] != '1' || (r->state[1] != 'F' && r->state[1] != 'R') ||
       r->state[2] || !boot_valid(r->boot)) return 0;
    uint64_t n = 0;
    for(size_t i = 0; i < sizeof r->elapsed; i++){
        unsigned char c = r->elapsed[i];
        if(!c){ if(!i) return 0; *seconds = n; return 1; }
        if(c < '0' || c > '9' || (i && r->elapsed[0] == '0') ||
           n > (UINT64_MAX - (c - '0')) / 10) return 0;
        n = n * 10 + (c - '0');
    }
    return 0;
}
static int usable(ota_policy_t *p){
    return p && !p->blocked && p->io.load && p->io.save && p->io.clock;
}
static int snapshot(ota_policy_t *p, ota_policy_record_t *r, uint64_t *now){
    memset(r, 0, sizeof *r);
    return p->io.clock(p->io.ctx, r->boot, now) == 0 && boot_valid(r->boot);
}
static int persist(ota_policy_t *p, ota_policy_record_t *r, uint64_t now, char state){
    r->state[0] = '1'; r->state[1] = state; r->state[2] = 0;
    snprintf(r->elapsed, sizeof r->elapsed, "%" PRIu64, now);
    if(p->io.save(p->io.ctx, r) != 0){ p->blocked = 1; return 0; }
    return 1;
}
void ota_policy_init(ota_policy_t *p, const ota_policy_io_t *io){
    memset(p, 0, sizeof *p);
    if(io) p->io = *io;
}
int ota_policy_enable(ota_policy_t *p){
    if(!usable(p)) return 0;
    ota_policy_record_t old = {0}, next;
    uint64_t now, since;
    int rc = p->io.load(p->io.ctx, &old);
    if(rc == 1) return record_valid(&old, &since);
    if(rc != 0 || !snapshot(p, &next, &now)) return 0;
    return persist(p, &next, now, 'F');
}
static int reserve(ota_policy_t *p, int enabled, int manual){
    if(!enabled || !usable(p)) return 0;
    ota_policy_record_t old = {0}, next;
    uint64_t now, since;
    if(p->io.load(p->io.ctx, &old) != 1 || !record_valid(&old, &since) ||
       !snapshot(p, &next, &now)) return 0;
    if(!manual){
        if(!strcmp(old.boot, next.boot)){
            if(now < since || (old.state[1] == 'R' && now - since < 86400)) return 0;
        } else if(now < 86400) return 0;
    }
    return persist(p, &next, now, 'R');
}
int ota_policy_reserve(ota_policy_t *p, int enabled){ return reserve(p, enabled, 0); }
int ota_policy_note_manual(ota_policy_t *p, int enabled){ return reserve(p, enabled, 1); }

/* Separate from diskos.conf: reset/opt-out must never erase a reservation.
 * Fixed-width char fields, no native integers or struct padding on disk. */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#ifndef OTA_POLICY_DIR
#define OTA_POLICY_DIR "/usr/data"
#endif
#ifndef OTA_POLICY_BOOT_ID
#define OTA_POLICY_BOOT_ID "/proc/sys/kernel/random/boot_id"
#endif
#define POLICY_PATH OTA_POLICY_DIR "/diskos_ota_policy"
static int device_load(void *ctx, ota_policy_record_t *out){
    (void)ctx;
    FILE *f = fopen(POLICY_PATH, "rb");
    if(!f) return errno == ENOENT ? 0 : -1;
    ota_policy_record_t r;
    size_t n = fread(&r, 1, sizeof r, f);
    int extra = fgetc(f), bad = ferror(f);
    if(fclose(f) != 0) bad = 1;
    uint64_t seconds;
    if(bad || n != sizeof r || extra != EOF || !record_valid(&r, &seconds)) return -1;
    *out = r;
    return 1;
}
static int device_save(void *ctx, const ota_policy_record_t *r){
    (void)ctx;
    uint64_t seconds;
    if(!record_valid(r, &seconds)) return -1;
    FILE *f = fopen(POLICY_PATH ".tmp", "wb");
    if(!f) return -1;
    int ok = fwrite(r, 1, sizeof *r, f) == sizeof *r;
    if(ok && (fflush(f) != 0 || fsync(fileno(f)) != 0)) ok = 0;
    if(fclose(f) != 0) ok = 0;
    if(!ok || rename(POLICY_PATH ".tmp", POLICY_PATH) != 0){
        unlink(POLICY_PATH ".tmp"); return -1;
    }
    int fd = open(OTA_POLICY_DIR, O_RDONLY | O_DIRECTORY);
    if(fd < 0) return -1;
    int rc = fsync(fd);
    if(close(fd) != 0) rc = -1;
    return rc == 0 ? 0 : -1;
}
static int device_clock(void *ctx, char boot[37], uint64_t *seconds){
    (void)ctx;
    FILE *f = fopen(OTA_POLICY_BOOT_ID, "rb");
    if(!f) return -1;
    char b[38] = {0};
    size_t n = fread(b, 1, sizeof b, f);
    int bad = ferror(f);
    if(fclose(f) != 0) bad = 1;
    if(bad || n != 37 || b[36] != '\n') return -1;
    b[36] = 0;
    struct timespec t;
    /* Existing UI clock uses CLOCK_MONOTONIC too (main.c). It excludes
     * suspend time: conservative delay is preferable to calendar-clock trust.
     * Kernel boot_id availability still requires a Disc check. */
    if(!boot_valid(b) || clock_gettime(CLOCK_MONOTONIC, &t) != 0 || t.tv_sec < 0) return -1;
    memcpy(boot, b, 37); *seconds = (uint64_t)t.tv_sec;
    return 0;
}
ota_policy_io_t ota_policy_device_io(void){
    return (ota_policy_io_t){NULL, device_load, device_save, device_clock};
}
