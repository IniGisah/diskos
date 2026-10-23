/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "remote_discovery.h"
#include <arpa/inet.h>
#include <string.h>
#include "avahi_text.h"

static int hex_digit(unsigned char c){
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int hex_number(const char *s, size_t n, size_t max, uint64_t *out){
    if(n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')){ s += 2; n -= 2; }
    if(!n || n > max) return -1;
    uint64_t value = 0;
    for(size_t i = 0; i < n; i++){
        int digit = hex_digit((unsigned char)s[i]);
        if(digit < 0) return -1;
        value = value << 4 | (unsigned)digit;
    }
    *out = value; return 0;
}
static int feature_number(const char *s, uint64_t *value){
    const char *comma = strchr(s, ',');
    if(!comma) return hex_number(s, strlen(s), 16, value);
    uint64_t low, high;
    if(hex_number(s, (size_t)(comma - s), 8, &low) ||
       hex_number(comma + 1, strlen(comma + 1), 8, &high)) return -1;
    *value = low | high << 32; return 0;
}
static int advertised_number(const char *txt, const char *key, const char *alias, int features,
                             int *present, uint64_t *value){
    char a[64], b[64]; uint64_t other;
    int pa = txt_get(txt, key, a, sizeof a), pb = txt_get(txt, alias, b, sizeof b);
    if(pa < 0 || pb < 0) return -1;
    *present = pa || pb;
    if(pa && (features ? feature_number(a, value) : hex_number(a, strlen(a), 16, value))) return -1;
    if(pb){
        if(features ? feature_number(b, &other) : hex_number(b, strlen(b), 16, &other)) return -1;
        if(pa && other != *value) return -1;
        *value = other;
    }
    return 0;
}
static void normalize_mac(char *id){
    size_t n = strlen(id); char canonical[18];
    if(n != 12 && n != 17) return;
    for(size_t i = 0; i < 6; i++){
        size_t at = i * (n == 17 ? 3 : 2);
        int a = hex_digit((unsigned char)id[at]), b = hex_digit((unsigned char)id[at + 1]);
        if(a < 0 || b < 0 || (n == 17 && i < 5 && id[at + 2] != ':')) return;
        canonical[i * 3] = "0123456789ABCDEF"[a]; canonical[i * 3 + 1] = "0123456789ABCDEF"[b];
        canonical[i * 3 + 2] = i == 5 ? 0 : ':';
    }
    memcpy(id, canonical, sizeof canonical);
}
static void resolved(remote_disc *d){
    char *f[10], *p = d->line, service[256], id[128], name[300], model[300]; rd_receiver r = {0};
    for(int i = 0; i < 9; i++){ f[i] = p; if(!(p = strchr(p, ';'))) return; *p++ = 0; }
    f[9] = p;
    if(strcmp(f[0], "=") || strcmp(f[1], "wlan0") || strcmp(f[2], "IPv4") || strcmp(f[5], "local")) return;
    if(!strcmp(f[4], "_googlecast._tcp")) r.protocol = RD_CAST;
    else if(!strcmp(f[4], "_airplay._tcp")) r.protocol = RD_AIRPLAY;
    else if(!strcmp(f[4], "_raop._tcp")) r.protocol = RD_RAOP;
    else return;
    const char *s = f[3];
    if(unesc(&s, service, sizeof service, 0) < 1 || *s) return;
    struct in_addr address; int port = port_of(f[8]);
    if(!port || strlen(f[7]) > 15 || inet_pton(AF_INET, f[7], &address) != 1) return;
    uint32_t host = ntohl(address.s_addr);
    if(host >> 24 == 0 || host >> 24 == 127 || host >> 24 >= 224 ||
       (host & UINT32_C(0xffff0000)) == UINT32_C(0xa9fe0000)) return;
    if(txt_get(f[9], r.protocol == RD_CAST ? "id" : "deviceid", id, sizeof id) < 0 ||
       txt_get(f[9], "fn", name, sizeof name) < 0 ||
       txt_get(f[9], r.protocol == RD_CAST ? "md" : r.protocol == RD_RAOP ? "am" : "model", model, sizeof model) < 0) return;
    const char *display = service;
    if(r.protocol == RD_RAOP){
        if(strlen(service) < 14 || service[12] != '@') return;
        for(int i = 0; i < 12; i++) if(hex_digit((unsigned char)service[i]) < 0) return;
        memcpy(id, service, 12); id[12] = 0; display = service + 13;
    }
    r.stable_id = *id != 0;
    if(put(r.id, sizeof r.id, *id ? id : service, 0) ||
       put(r.name, sizeof r.name, r.protocol == RD_CAST && *name ? name : display, 1) ||
       put(r.model, sizeof r.model, model, 1)) return;
    if(r.protocol != RD_CAST){
        if(r.stable_id) normalize_mac(r.id);
        if(advertised_number(f[9], "features", "ft", 1, &r.have_features, &r.features) ||
           advertised_number(f[9], "flags", "sf", 0, &r.have_flags, &r.flags)) return;
    }
    if(r.protocol == RD_RAOP){
        char et[128], pw[16];
        if(txt_get(f[9], "et", et, sizeof et) < 0 || txt_get(f[9], "pw", pw, sizeof pw) < 0) return;
        if(*et){
            r.have_encryption = 1;
            for(const char *p = et; *p;){
                unsigned value = 0; int digits = 0;
                while(*p >= '0' && *p <= '9'){ value = value * 10 + (unsigned)(*p++ - '0'); if(value > 31) return; digits++; }
                if(!digits || (*p && *p != ',')) return;
                r.encryption |= UINT32_C(1) << value;
                if(*p++ == 0) break;
                if(!*p) return;
            }
        }
        if(*pw){
            if(!strcmp(pw, "true") || !strcmp(pw, "1")) r.password = 1;
            else if(strcmp(pw, "false") && strcmp(pw, "0")) return;
        }
    }
    strcpy(r.addr, f[7]); r.port = (uint16_t)port;
    for(size_t i = 0; i < d->n; i++){
        if(d->r[i].protocol == r.protocol && d->r[i].stable_id == r.stable_id && !strcmp(d->r[i].id, r.id)){
            d->r[i] = r; return;
        }
    }
    if(d->n < RD_MAX) d->r[d->n++] = r;
}
static void flush(remote_disc *d){
    if(!d->bad){ d->line[d->used] = 0; if(strlen(d->line) == d->used) resolved(d); }
    d->used = 0; d->bad = 0;
}
static int current(remote_disc *d, uint64_t token, uint32_t now){
    if(!d || !token || !d->token) return RD_ARGUMENT;
    if(d->token != token) return RD_STALE;
    if(d->state == RD_OPEN && (uint32_t)(now - d->start_ms) >= d->budget_ms){
        d->state = RD_EXPIRED; d->used = 0; d->bad = 0;
    }
    return d->state;
}
int remote_disc_begin(remote_disc *d, uint64_t token, uint32_t now, uint32_t budget){
    if(!d || !token || !budget || budget > 30000) return RD_ARGUMENT;
    memset(d, 0, sizeof *d); d->token = token; d->start_ms = now; d->budget_ms = budget;
    return RD_OPEN;
}
int remote_disc_feed(remote_disc *d, uint64_t token, uint32_t now, const char *bytes, size_t n){
    if(n && !bytes) return RD_ARGUMENT;
    int state = current(d, token, now);
    if(state != RD_OPEN) return state;
    for(size_t i = 0; i < n; i++){
        if(bytes[i] == '\n') flush(d);
        else if(d->used + 1 >= sizeof d->line) d->bad = 1;
        else d->line[d->used++] = bytes[i];
    }
    return RD_OPEN;
}
int remote_disc_finish(remote_disc *d, uint64_t token, uint32_t now){
    int state = current(d, token, now);
    if(state != RD_OPEN) return state;
    if(d->used || d->bad) flush(d);
    return d->state = RD_FINISHED;
}
int remote_disc_cancel(remote_disc *d, uint64_t token){
    if(!d || !token || !d->token) return RD_ARGUMENT;
    if(d->token != token) return RD_STALE;
    memset(d->r, 0, sizeof d->r); d->n = d->used = 0; d->bad = 0;
    return d->state = RD_CANCELLED;
}
