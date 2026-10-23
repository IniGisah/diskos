/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_plist.h"
#include <stdlib.h>
#include <string.h>
static uint64_t number(const uint8_t *p, unsigned n){
    uint64_t v = 0; for(unsigned i = 0; i < n; i++) v = v << 8 | p[i]; return v;
}
typedef struct { const uint8_t *b; uint64_t count, table; unsigned off, ref, quota; } guard;
static int object(guard *g, uint64_t id, unsigned depth){
    if(id >= g->count || depth > 32 || !g->quota--) return -1;
    uint64_t at = number(g->b + g->table + id * g->off, g->off);
    if(at < 8 || at >= g->table) return -1;
    unsigned marker = g->b[at++], type = marker >> 4; uint64_t n = marker & 15;
    if(type == 0) return marker == 0 || marker == 8 || marker == 9 ? 0 : -1;
    /* libplist write_uint emits a 16-byte zero-extended uint64. Reject values
     * beyond uint64 rather than letting the library truncate the upper half. */
    if(type == 1 && n == 4) return g->table-at >= 16 && !number(g->b+at, 8) ? 0 : -1;
    if(type == 1 || type == 2 || type == 3){
        if(n > 3 || (type == 3 && marker != 0x33)) return -1;
        return (UINT64_C(1) << n) <= g->table - at ? 0 : -1;
    }
    if(type == 8) return n + 1 <= g->table - at ? 0 : -1;
    if(type != 4 && type != 5 && type != 6 && type != 10 && type != 13) return -1;
    if(n == 15){
        if(at >= g->table || g->b[at] >> 4 != 1 || (g->b[at] & 15) > 3) return -1;
        unsigned width = 1u << (g->b[at++] & 15);
        if(width > g->table - at) return -1;
        n = number(g->b + at, width); at += width;
    }
    uint64_t width = type == 6 ? 2 : type == 10 ? g->ref : type == 13 ? 2 * g->ref : 1;
    if(n > (g->table - at) / width) return -1;
    if(type == 10 || type == 13){
        uint64_t children = n * (type == 13 ? 2 : 1);
        if(children > g->quota) return -1;
        for(uint64_t i = 0; i < children; i++)
            if(object(g, number(g->b + at + i * g->ref, g->ref), depth + 1)) return -1;
    }
    return 0;
}
plist_t aps_plist_read(const void *data, size_t n){
    const uint8_t *b = data; plist_t p = NULL;
    if(n < 40 || n > APS_BODY_MAX || memcmp(b, "bplist00", 8)) return NULL;
    const uint8_t *t = b + n - 32;
    guard g = { b, number(t + 8, 8), number(t + 24, 8), t[6], t[7], 4096 };
    if(!g.count || g.count > 4096 || !g.off || g.off > 8 || !g.ref || g.ref > 8 ||
       g.table < 8 || g.table > n - 32 || g.count > (n - 32 - g.table) / g.off ||
       object(&g, number(t + 16, 8), 0)) return NULL;
    if(plist_from_bin(data, (uint32_t)n, &p) != PLIST_ERR_SUCCESS || !p) return NULL;
    if(plist_get_node_type(p) != PLIST_DICT){ plist_free(p); return NULL; }
    return p;
}
int aps_plist_request_path(aps_rtsp *r, const char *method, const char *path, plist_t p, aps_reply *reply, unsigned ms){
    char *b = NULL; uint32_t n = 0;
    if(plist_to_bin(p, &b, &n) != PLIST_ERR_SUCCESS || !b || n > APS_BODY_MAX){ free(b); return -1; }
    int rc = aps_rtsp_request(r, method, path, "Content-Type: application/x-apple-binary-plist\r\n", b, n, reply, ms);
    free(b); return rc;
}
int aps_plist_request(aps_rtsp *r, const char *method, plist_t p, aps_reply *reply, unsigned ms){
    return aps_plist_request_path(r, method, r->uri, p, reply, ms);
}
int aps_plist_uint(plist_t d, const char *key, uint64_t *out){
    if(!d || plist_get_node_type(d) != PLIST_DICT) return -1;
    plist_t n = plist_dict_get_item(d, key);
    if(!n || plist_get_node_type(n) != PLIST_UINT) return -1;
    plist_get_uint_val(n, out); return 0;
}
int aps_plist_port(plist_t d, const char *key, uint16_t *out){
    uint64_t v; if(aps_plist_uint(d, key, &v) || !v || v > 65535) return -1;
    *out = (uint16_t)v; return 0;
}
void aps_plist_int(plist_t d, const char *key, uint64_t v){ plist_dict_set_item(d, key, plist_new_uint(v)); }
void aps_plist_text(plist_t d, const char *key, const char *s){ plist_dict_set_item(d, key, plist_new_string(s)); }
