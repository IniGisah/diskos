/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bt_codec_observe.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
static int mac_copy(const char *s, char out[18]){
    if(!s || strlen(s) != 17) return 0;
    for(unsigned i = 0; i < 17; i++){
        unsigned char c = (unsigned char)s[i];
        if(i % 3 == 2){ if(c != ':' && c != '_') return 0; out[i] = ':'; }
        else { if(c >= 'a' && c <= 'f') c -= 'a' - 'A';
               if(!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return 0;
               out[i] = (char)c; }
    }
    out[17] = 0; return 1;
}
static int number(const char *s, unsigned *out, const char *tail){
    unsigned n = 0; const char *p = s;
    while(*p >= '0' && *p <= '9'){
        unsigned digit = (unsigned)(*p++ - '0');
        if(n > (UINT_MAX - digit) / 10) return 0;
        n = n * 10 + digit;
    }
    if(p == s || strcmp(p, tail)) return 0;
    *out = n; return 1;
}
static int endpoint(const char *path, const char *prefix, const char *suffix,
                    unsigned *adapter, char mac[18]){
    size_t pl = strlen(prefix);
    if(strncmp(path, prefix, pl)) return 0;
    const char *p = path + pl, *start = p;
    while(*p >= '0' && *p <= '9') p++;
    char digits[12], raw[18]; size_t n = (size_t)(p - start);
    if(!n || n >= sizeof digits || strncmp(p, "/dev_", 5)) return 0;
    memcpy(digits, start, n); digits[n] = 0;
    if(!number(digits, adapter, "") || strlen(p + 5) != 17 + strlen(suffix)) return 0;
    memcpy(raw, p + 5, 17); raw[17] = 0;
    return !strcmp(p + 22, suffix) && mac_copy(raw, mac);
}
int bc_parse(const char *text, size_t len, const char *mac, bc_snapshot *out){
    if(!text || !out || !len || len > 4096 || memchr(text, 0, len)) return BC_ARGUMENT;
    bc_snapshot s = {0}; char expected[18], device[128] = {0}; unsigned seen = 0;
    if(!mac_copy(mac, expected)) return BC_ARGUMENT;
    const char *keys[] = {"PCM=", "Device: ", "Transport: ", "Mode: ", "Format: ",
                          "Channels: ", "Sampling: ", "Sequence: ", "Selected codec: "};
    for(size_t at = 0; at < len;){
        const char *end = memchr(text + at, '\n', len - at);
        if(!end) return BC_MISSING;
        size_t n = (size_t)(end - text - at); if(n && text[at + n - 1] == '\r') n--;
        if(n >= 256) return BC_ARGUMENT;
        char line[256]; memcpy(line, text + at, n); line[n] = 0; at = (size_t)(end - text) + 1;
        for(unsigned k = 0; k < sizeof keys / sizeof *keys; k++){
            size_t kl = strlen(keys[k]); if(strncmp(line, keys[k], kl)) continue;
            if(seen & (1u << k)) return BC_ARGUMENT;
            seen |= 1u << k; const char *value = line + kl; size_t vl = strlen(value);
            char *target = NULL; size_t cap = 0;
            switch(k){
            case 0: target = s.pcm; cap = sizeof s.pcm; break;
            case 1: target = device; cap = sizeof device; break;
            case 2: if(strcmp(value, "A2DP-source")) return BC_MISMATCH; break;
            case 3: if(strcmp(value, "sink")) return BC_MISMATCH; break;
            case 4: target = s.format; cap = sizeof s.format; break;
            case 5: if(!number(value, &s.channels, "") || !s.channels || s.channels > 8) return BC_ARGUMENT; break;
            case 6: if(!number(value, &s.rate, " Hz") || !s.rate) return BC_ARGUMENT; break;
            case 7: if(!number(value, &s.sequence, "")) return BC_ARGUMENT; break;
            case 8: target = s.codec_name; cap = sizeof s.codec_name; break;
            }
            if(target){ if(!vl || vl >= cap) return BC_ARGUMENT; memcpy(target, value, vl + 1); }
        }
    }
    if(seen != (1u << 9) - 1) return BC_MISSING;
    unsigned adapter; char other[18];
    if(!endpoint(s.pcm, "/org/bluealsa/hci", "/a2dpsrc/sink", &s.adapter, s.mac) ||
       !endpoint(device, "/org/bluez/hci", "", &adapter, other) || adapter != s.adapter ||
       strcmp(s.mac, other) || strcmp(s.mac, expected)) return BC_MISMATCH;
    s.codec = !strcmp(s.codec_name, "SBC") ? BC_CODEC_SBC : !strcmp(s.codec_name, "AAC") ? BC_CODEC_AAC :
              !strcmp(s.codec_name, "LDAC") ? BC_CODEC_LDAC : BC_CODEC_OTHER;
    *out = s; return BC_OK;
}
int bc_begin(bc_observer *o, uint64_t route, uint64_t player, const char *mac,
             uint32_t now, uint32_t wait_ms, bc_token *out){
    char normalized[18];
    if(!o || !out || !route || !player || !wait_ms || wait_ms > 30000 ||
       !mac_copy(mac, normalized) || o->token.attempt == UINT64_MAX) return BC_ARGUMENT;
    o->token = (bc_token){o->token.attempt + 1, route, player};
    memcpy(o->mac, normalized, sizeof o->mac); o->deadline = now + wait_ms;
    o->pending = 1; o->have = 0; *out = o->token; return BC_OK;
}
int bc_complete(bc_observer *o, const bc_token *t, uint64_t route, uint64_t player,
                uint32_t now, const char *text, size_t len){
    if(!o || !t) return BC_ARGUMENT;
    if(!o->pending || t->attempt != o->token.attempt || t->route != o->token.route ||
       t->player != o->token.player || route != t->route || player != t->player) return BC_STALE;
    o->pending = 0;
    if((int32_t)(now - o->deadline) >= 0) return BC_TIMEOUT;
    bc_snapshot s; int rc = bc_parse(text, len, o->mac, &s);
    if(rc == BC_OK){ s.observed_ms = now; o->snapshot = s; o->have = 1; }
    return rc;
}
void bc_cancel(bc_observer *o){ if(o){ o->pending = 0; o->have = 0; } }
int bc_get(const bc_observer *o, uint64_t route, uint64_t player, bc_snapshot *out){
    if(!o || !out) return BC_ARGUMENT;
    if(!o->have || route != o->token.route || player != o->token.player) return BC_STALE;
    *out = o->snapshot; return BC_OK;
}
