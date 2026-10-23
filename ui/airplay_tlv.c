/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_tlv.h"
#include <string.h>
int aps_tlv_get(const void *p, size_t n, unsigned tag, void *out, size_t cap){
    const uint8_t *b = p; size_t at = 0, used = 0;
    uint8_t seen[256] = {0}; unsigned prior = 256, last = 0;
    if(!p || n > APS_TLV_MAX || tag > 255) return -1;
    while(at < n){
        if(n - at < 2) return -1;
        unsigned t = b[at++], k = b[at++];
        if(k > n - at || (seen[t] && (prior != t || last != 255))) return -1;
        seen[t] = 1;
        if(t == tag){
            if(k > cap - used) return -1;
            if(out) memcpy((uint8_t *)out + used, b + at, k);
            used += k;
        }
        prior = t; last = k; at += k;
    }
    return (int)used;
}
int aps_tlv_put(aps_tlv *t, unsigned tag, const void *p, size_t n){
    if(!t || tag > 255 || !p || !n || n > APS_TLV_MAX || t->n > APS_TLV_MAX ||
       n + ((n + 254) / 255) * 2 > APS_TLV_MAX - t->n) return -1;
    const uint8_t *b = p;
    while(n){
        size_t k = n > 255 ? 255 : n;
        t->b[t->n++] = (uint8_t)tag; t->b[t->n++] = (uint8_t)k;
        memcpy(t->b + t->n, b, k); t->n += k; b += k; n -= k;
    }
    return 0;
}
int aps_tlv_byte(aps_tlv *t, unsigned tag, unsigned v){
    uint8_t b = (uint8_t)v; return v > 255 ? -1 : aps_tlv_put(t, tag, &b, 1);
}
int aps_tlv_state(const aps_tlv *t, unsigned state){
    uint8_t b, e;
    return aps_tlv_get(t->b, t->n, 6, &b, 1) == 1 && b == state &&
           aps_tlv_get(t->b, t->n, 7, &e, 1) == 0 ? 0 : -1;
}
