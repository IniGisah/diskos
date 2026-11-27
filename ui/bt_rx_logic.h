#ifndef DISKOS_BT_RX_LOGIC_H
#define DISKOS_BT_RX_LOGIC_H

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int bt_rx_mac_valid(const char *mac){
    if(!mac || strlen(mac) != 17) return 0;
    for(int i = 0; i < 17; i++){
        if((i % 3) == 2){
            if(mac[i] != ':') return 0;
        } else {
            if(!isxdigit((unsigned char)mac[i])) return 0;
        }
    }
    return 1;
}

static inline int bt_rx_mac_eq_ci(const char *a, const char *b){
    if(!a || !b || !a[0] || !b[0]) return 0;
    for(int i = 0;; i++){
        int ca = toupper((unsigned char)a[i]);
        int cb = toupper((unsigned char)b[i]);
        if(ca != cb) return 0;
        if(ca == 0) return 1;
    }
}

static inline void bt_rx_colon_to_dbus(const char *colon, char *dbus, int dbus_sz){
    if(!dbus || dbus_sz <= 0) return;
    dbus[0] = '\0';
    if(!colon) return;
    int j = 0;
    for(int i = 0; colon[i] && j < dbus_sz - 1; i++){
        dbus[j++] = (colon[i] == ':') ? '_' : colon[i];
    }
    dbus[j] = '\0';
}

static inline void bt_rx_dbus_to_colon(const char *dbus, char *colon, int colon_sz){
    if(!colon || colon_sz <= 0) return;
    colon[0] = '\0';
    if(!dbus) return;
    int j = 0;
    for(int i = 0; dbus[i] && j < colon_sz - 1; i++){
        colon[j++] = (dbus[i] == '_') ? ':' : (char)toupper((unsigned char)dbus[i]);
    }
    colon[j] = '\0';
}

/* Lexical helpers for dbus-send --print-reply output.
 * Handles escaped quotes (\"), backslashes (\\), multi-line quoted strings,
 * and arbitrary whitespace/newlines between key ("Tag") and variant value. */
static inline const char *bt_rx_skip_ws(const char *p){
    while(p && *p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static inline const char *bt_rx_skip_quoted(const char *p, char *out, int out_sz, int *out_len){
    if(!p || *p != '"') return NULL;
    p++; /* consume opening quote */
    int w = 0;
    while(*p && *p != '"'){
        unsigned char c = (unsigned char)*p++;
        if(c == '\\' && *p){
            unsigned char esc = (unsigned char)*p++;
            switch(esc){
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case '\\': c = '\\'; break;
                case '"': c = '"'; break;
                default: c = esc; break;
            }
        }
        if(out && w < out_sz - 1){
            out[w++] = (char)c;
        }
    }
    if(out && out_sz > 0) out[w] = '\0';
    if(out_len) *out_len = w;
    if(*p == '"') p++;
    return p;
}

/* Advance through dbus-send output within [start, end) looking for
 * `string "<tag>"` followed by `variant` at structural token boundaries
 * (not inside another string). */
static inline const char *bt_rx_dbus_find_tag_value_range(const char *start, const char *end, const char *tag){
    if(!start || !tag || !tag[0]) return NULL;
    if(!end) end = start + strlen(start);
    if(end <= start) return NULL;
    size_t tlen = strlen(tag);
    const char *p = start;
    char qbuf[128];
    while(p < end && *p){
        if(*p == '"'){
            int qlen = 0;
            const char *after = bt_rx_skip_quoted(p, qbuf, (int)sizeof qbuf, &qlen);
            if(!after || after > end) break;
            if((size_t)qlen == tlen && strcmp(qbuf, tag) == 0){
                /* Check backwards: preceded by "string" keyword? */
                const char *s = p;
                while(s > start && (s[-1] == ' ' || s[-1] == '\t' || s[-1] == '\r' || s[-1] == '\n')) s--;
                if(s - start >= 6 && strncmp(s - 6, "string", 6) == 0){
                    int ok_before = (s - 6 == start) || !isalnum((unsigned char)s[-7]);
                    if(ok_before){
                        const char *v = bt_rx_skip_ws(after);
                        if(v < end && strncmp(v, "variant", 7) == 0 &&
                           (!v[7] || !isalnum((unsigned char)v[7]))){
                            const char *val = bt_rx_skip_ws(v + 7);
                            return (val < end) ? val : NULL;
                        }
                    }
                }
            }
            p = after;
            continue;
        }
        p++;
    }
    return NULL;
}

static inline const char *bt_rx_dbus_find_tag_value(const char *reply, const char *tag){
    return reply ? bt_rx_dbus_find_tag_value_range(reply, NULL, tag) : NULL;
}

static inline int bt_rx_dbus_extract_tag_range(const char *start, const char *end, const char *tag,
                                               char *out, int out_sz){
    if(!out || out_sz <= 0) return 0;
    out[0] = '\0';
    const char *v = bt_rx_dbus_find_tag_value_range(start, end, tag);
    if(!v) return 0;
    if(strncmp(v, "string", 6) != 0 || (v[6] && isalnum((unsigned char)v[6]))) return 0;
    v = bt_rx_skip_ws(v + 6);
    if((end && v >= end) || *v != '"') return 0;
    int len = 0;
    const char *after = bt_rx_skip_quoted(v, out, out_sz, &len);
    if(!after || (end && after > end)) { out[0] = '\0'; return 0; }
    /* Normalize embedded newlines/tabs to spaces so UI labels stay single-line */
    for(int i = 0; out[i]; i++){
        if(out[i] == '\r' || out[i] == '\n' || out[i] == '\t') out[i] = ' ';
    }
    return out[0] != '\0';
}

static inline int bt_rx_dbus_extract_tag(const char *reply, const char *tag, char *out, int out_sz){
    return bt_rx_dbus_extract_tag_range(reply, NULL, tag, out, out_sz);
}

static inline int bt_rx_dbus_parse_int_after_type(const char *v, uint32_t *out){
    if(!v || !out) return 0;
    const char *types[] = {"uint32", "int32", "uint64", "int64", "uint16", "int16", "byte"};
    for(size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++){
        size_t tl = strlen(types[i]);
        if(strncmp(v, types[i], tl) == 0 && (!v[tl] || isspace((unsigned char)v[tl]))){
            const char *num = bt_rx_skip_ws(v + tl);
            if(!*num) return 0;
            if(*num == '-'){
                char *end = NULL;
                long long sv = strtoll(num, &end, 10);
                if(end == num) return 0;
                *out = (sv < 0) ? 0u : (sv > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)sv);
                return 1;
            }
            if(!isdigit((unsigned char)*num)) return 0;
            char *end = NULL;
            unsigned long long uv = strtoull(num, &end, 10);
            if(end == num) return 0;
            *out = (uv > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)uv;
            return 1;
        }
    }
    return 0;
}

static inline int bt_rx_dbus_extract_u32(const char *reply, const char *tag, uint32_t *out){
    if(!out) return 0;
    const char *v = bt_rx_dbus_find_tag_value(reply, tag);
    if(!v) return 0;
    return bt_rx_dbus_parse_int_after_type(v, out);
}

static inline int bt_rx_dbus_parse_variant_u32(const char *reply, uint32_t *out){
    if(!reply || !out) return 0;
    const char *p = reply;
    while(*p){
        if(*p == '"'){
            const char *after = bt_rx_skip_quoted(p, NULL, 0, NULL);
            if(!after) break;
            p = after;
            continue;
        }
        if(strncmp(p, "variant", 7) == 0 &&
           (p == reply || !isalnum((unsigned char)p[-1])) &&
           (!p[7] || !isalnum((unsigned char)p[7]))){
            const char *v = bt_rx_skip_ws(p + 7);
            if(bt_rx_dbus_parse_int_after_type(v, out)) return 1;
        }
        if(bt_rx_dbus_parse_int_after_type(p, out)){
            int ok_before = (p == reply) || !isalnum((unsigned char)p[-1]);
            if(ok_before) return 1;
        }
        p++;
    }
    return 0;
}

typedef struct {
    char title[128];
    char artist[128];
    char album[128];
    char status[32];
    uint32_t position_ms;
    uint32_t duration_ms;
    int has_title;
    int has_artist;
    int has_album;
    int has_status;
    int has_position;
    int has_duration;
} bt_rx_mediaplayer_props_t;

static inline void bt_rx_dbus_parse_mediaplayer_props(const char *reply, bt_rx_mediaplayer_props_t *out){
    if(!out) return;
    memset(out, 0, sizeof(*out));
    if(!reply || !reply[0]) return;
    out->has_title = bt_rx_dbus_extract_tag(reply, "Title", out->title, (int)sizeof out->title);
    out->has_artist = bt_rx_dbus_extract_tag(reply, "Artist", out->artist, (int)sizeof out->artist);
    out->has_album = bt_rx_dbus_extract_tag(reply, "Album", out->album, (int)sizeof out->album);
    out->has_status = bt_rx_dbus_extract_tag(reply, "Status", out->status, (int)sizeof out->status);
    out->has_position = bt_rx_dbus_extract_u32(reply, "Position", &out->position_ms);
    out->has_duration = bt_rx_dbus_extract_u32(reply, "Duration", &out->duration_ms);
}

typedef int (*bt_rx_audio_src_fn)(const char *mac, void *ctx);

static inline int bt_rx_dbus_extract_bool(const char *start, const char *end, const char *tag){
    if(!start || !end || end <= start || !tag) return -1;
    const char *v = bt_rx_dbus_find_tag_value_range(start, end, tag);
    if(!v || v >= end) return -1;
    if(strncmp(v, "boolean", 7) != 0 || (v[7] && isalnum((unsigned char)v[7]))) return -1;
    v = bt_rx_skip_ws(v + 7);
    if(v >= end) return -1;
    if(strncmp(v, "true", 4) == 0 && (!v[4] || !isalnum((unsigned char)v[4]))) return 1;
    if(strncmp(v, "false", 5) == 0 && (!v[5] || !isalnum((unsigned char)v[5]))) return 0;
    return -1;
}

static inline int bt_rx_dbus_is_line_obj_hdr(const char *base, const char *pos){
    if(!base || !pos || pos < base) return 0;
    const char *line = pos;
    while(line > base && line[-1] != '\n' && line[-1] != '\r') line--;
    for(const char *c = line; c < pos; c++){
        if(*c != ' ' && *c != '\t') return 0;
    }
    return 1;
}

static inline const char *bt_rx_dbus_find_next_obj(const char *cur, const char *base){
    const char *p = cur;
    while(p && (p = strstr(p, "object path \"")) != NULL){
        if(bt_rx_dbus_is_line_obj_hdr(base, p)) return p;
        p += 13;
    }
    return NULL;
}

/* Parse `dbus-send ... ObjectManager.GetManagedObjects` reply for a connected
 * org.bluez.Device1 object (skipping sub-objects like /player0). */
static inline int bt_rx_extract_bluez_connected(const char *objs, bt_rx_audio_src_fn is_src, void *ctx,
                                                char *mac_out, int mac_sz, char *name_out, int name_sz){
    if(mac_out && mac_sz > 0) mac_out[0] = '\0';
    if(name_out && name_sz > 0) name_out[0] = '\0';
    if(!objs || !objs[0]) return 0;

    const char *p = objs;
    while((p = strstr(p, "object path \"/org/bluez/hci0/dev_")) != NULL){
        if(!bt_rx_dbus_is_line_obj_hdr(objs, p)){ p += 13; continue; }
        const char *path = p + 13;
        const char *q = strchr(path, '"');
        if(!q) break;
        size_t plen = (size_t)(q - path);
        p = q + 1;
        /* Exact device object path ends after /dev_XX_XX_XX_XX_XX_XX (len == 37) */
        if(plen != 37) continue;

        const char *next_obj = bt_rx_dbus_find_next_obj(p, objs);
        const char *end = next_obj ? next_obj : (p + strlen(p));

        if(bt_rx_dbus_extract_bool(p, end, "Connected") != 1) continue;

        char dmac[18];
        memcpy(dmac, path + 20, 17);
        dmac[17] = '\0';
        char mac[18];
        bt_rx_dbus_to_colon(dmac, mac, (int)sizeof mac);
        if(!bt_rx_mac_valid(mac)) continue;
        if(is_src && !is_src(mac, ctx)) continue;

        if(mac_out && mac_sz > 0) snprintf(mac_out, (size_t)mac_sz, "%s", mac);
        if(name_out && name_sz > 0){
            if(!bt_rx_dbus_extract_tag_range(p, end, "Alias", name_out, name_sz)){
                bt_rx_dbus_extract_tag_range(p, end, "Name", name_out, name_sz);
            }
        }
        return 1;
    }
    return 0;
}

typedef struct {
    int present;
    int powered;
    int discoverable;
    int pairable;
    char alias[64];
} bt_rx_adapter_state_t;

static inline int bt_rx_extract_adapter_state(const char *objs, bt_rx_adapter_state_t *out){
    if(!out) return 0;
    memset(out, 0, sizeof(*out));
    if(!objs || !objs[0]) return 0;

    const char *p = objs;
    while((p = strstr(p, "object path \"/org/bluez/hci0\"")) != NULL){
        if(!bt_rx_dbus_is_line_obj_hdr(objs, p)){ p += 13; continue; }
        const char *q = strchr(p + 13, '"');
        if(!q) break;
        p = q + 1;
        const char *next_obj = bt_rx_dbus_find_next_obj(p, objs);
        const char *end = next_obj ? next_obj : (p + strlen(p));
        out->present = 1;
        out->powered = (bt_rx_dbus_extract_bool(p, end, "Powered") == 1);
        out->discoverable = (bt_rx_dbus_extract_bool(p, end, "Discoverable") == 1);
        out->pairable = (bt_rx_dbus_extract_bool(p, end, "Pairable") == 1);
        bt_rx_dbus_extract_tag_range(p, end, "Alias", out->alias, (int)sizeof out->alias);
        return 1;
    }
    return 0;
}

static inline int bt_rx_path_safe(const char *path){
    if(!path || path[0] != '/') return 0;
    for(const char *p = path; *p; p++){
        unsigned char c = (unsigned char)*p;
        if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') || c == '/' || c == '_')){
            return 0;
        }
    }
    return 1;
}

/* Discover active MediaPlayer1 object path for `mac` from GetManagedObjects
 * (handles /player0, /player1, etc., or MediaControl1.Player). */
static inline int bt_rx_extract_player_path(const char *objs, const char *mac, char *path_out, int path_sz){
    if(!path_out || path_sz <= 0) return 0;
    path_out[0] = '\0';
    if(!objs || !objs[0] || !bt_rx_mac_valid(mac)) return 0;

    char dmac[18];
    bt_rx_colon_to_dbus(mac, dmac, (int)sizeof dmac);
    for(int i = 0; dmac[i]; i++) dmac[i] = (char)toupper((unsigned char)dmac[i]);

    char prefix[48];
    snprintf(prefix, sizeof prefix, "/org/bluez/hci0/dev_%s/", dmac);
    size_t pre_len = strlen(prefix);

    /* 1. Check MediaControl1.Player on the exact device object first */
    char dev_hdr[64];
    snprintf(dev_hdr, sizeof dev_hdr, "object path \"/org/bluez/hci0/dev_%s\"", dmac);
    const char *dev = objs;
    while((dev = strstr(dev, dev_hdr)) != NULL){
        if(bt_rx_dbus_is_line_obj_hdr(objs, dev)){
            const char *ds = dev + strlen(dev_hdr);
            const char *next_obj = bt_rx_dbus_find_next_obj(ds, objs);
            const char *dend = next_obj ? next_obj : (ds + strlen(ds));
            const char *pv = bt_rx_dbus_find_tag_value_range(ds, dend, "Player");
            if(pv && strncmp(pv, "object path", 11) == 0 && (!pv[11] || !isalnum((unsigned char)pv[11]))){
                pv = bt_rx_skip_ws(pv + 11);
                if(pv < dend && *pv == '"'){
                    char cand[96] = {0};
                    int clen = 0;
                    const char *after = bt_rx_skip_quoted(pv, cand, (int)sizeof cand, &clen);
                    if(after && after <= dend && strncasecmp(cand, prefix, pre_len) == 0 && bt_rx_path_safe(cand)){
                        snprintf(path_out, (size_t)path_sz, "%s", cand);
                        return 1;
                    }
                }
            }
            break;
        }
        dev += 13;
    }

    /* 2. Scan for any /org/bluez/hci0/dev_<MAC>/player... object path */
    const char *p = objs;
    while((p = strstr(p, "object path \"/org/bluez/hci0/dev_")) != NULL){
        if(!bt_rx_dbus_is_line_obj_hdr(objs, p)){ p += 13; continue; }
        const char *path = p + 13;
        const char *q = strchr(path, '"');
        if(!q) break;
        size_t plen = (size_t)(q - path);
        p = q + 1;
        if(plen > pre_len && plen < (size_t)path_sz &&
           strncasecmp(path, prefix, pre_len) == 0 &&
           strncmp(path + pre_len, "player", 6) == 0){
            char cand[96];
            memcpy(cand, path, plen);
            cand[plen] = '\0';
            if(bt_rx_path_safe(cand)){
                snprintf(path_out, (size_t)path_sz, "%s", cand);
                return 1;
            }
        }
    }
    return 0;
}

static inline int bt_rx_extract_mediaplayer_from_objs(const char *objs, const char *player_path,
                                                      bt_rx_mediaplayer_props_t *out){
    if(!out) return 0;
    memset(out, 0, sizeof(*out));
    if(!objs || !objs[0] || !player_path || !player_path[0]) return 0;
    char hdr[128];
    snprintf(hdr, sizeof hdr, "object path \"%s\"", player_path);
    const char *p = objs;
    while((p = strstr(p, hdr)) != NULL){
        if(bt_rx_dbus_is_line_obj_hdr(objs, p)) break;
        p += 13;
    }
    if(!p) return 0;
    p += strlen(hdr);
    const char *next_obj = bt_rx_dbus_find_next_obj(p, objs);
    const char *end = next_obj ? next_obj : (p + strlen(p));
    out->has_title = bt_rx_dbus_extract_tag_range(p, end, "Title", out->title, (int)sizeof out->title);
    out->has_artist = bt_rx_dbus_extract_tag_range(p, end, "Artist", out->artist, (int)sizeof out->artist);
    out->has_album = bt_rx_dbus_extract_tag_range(p, end, "Album", out->album, (int)sizeof out->album);
    out->has_status = bt_rx_dbus_extract_tag_range(p, end, "Status", out->status, (int)sizeof out->status);
    const char *v = bt_rx_dbus_find_tag_value_range(p, end, "Position");
    if(v) out->has_position = bt_rx_dbus_parse_int_after_type(v, &out->position_ms);
    v = bt_rx_dbus_find_tag_value_range(p, end, "Duration");
    if(v) out->has_duration = bt_rx_dbus_parse_int_after_type(v, &out->duration_ms);
    return out->has_status || out->has_title || out->has_artist || out->has_album;
}

typedef struct {
    int connected;
    char mac[18];
    char name[64];
    int has_sink_pcm;
    int acl_active;
    int bluez_connected;
    int ignored_stale_bt_state;
} bt_rx_conn_info_t;

static inline int bt_rx_extract_sink_pcm_mac(const char *pcm_list, char *mac_out, int mac_sz){
    if(mac_out && mac_sz > 0) mac_out[0] = '\0';
    if(!pcm_list || !pcm_list[0]) return 0;
    const char *p = pcm_list;
    while(*p){
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        if(len >= sizeof line) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        if(strstr(line, "/a2dpsnk/") || strstr(line, "/a2dp-sink/")){
            const char *dev = strstr(line, "dev_");
            if(dev && strlen(dev) >= 21){
                char raw[18];
                memcpy(raw, dev + 4, 17);
                raw[17] = '\0';
                char colon[18];
                bt_rx_dbus_to_colon(raw, colon, (int)sizeof colon);
                if(bt_rx_mac_valid(colon)){
                    if(mac_out && mac_sz > 0) snprintf(mac_out, (size_t)mac_sz, "%s", colon);
                    return 1;
                }
            }
        }
        p = eol ? eol + 1 : p + strlen(p);
    }
    return 0;
}

static inline int bt_rx_extract_acl_mac(const char *hcitool_con, bt_rx_audio_src_fn is_audio_src, void *ctx,
                                        char *mac_out, int mac_sz){
    if(mac_out && mac_sz > 0) mac_out[0] = '\0';
    if(!hcitool_con || !hcitool_con[0]) return 0;
    const char *p = hcitool_con;
    while((p = strstr(p, "ACL ")) != NULL){
        p += 4;
        while(*p == ' ') p++;
        if(strlen(p) >= 17){
            char cand[18];
            memcpy(cand, p, 17);
            cand[17] = '\0';
            if(bt_rx_mac_valid(cand)){
                if(!is_audio_src || is_audio_src(cand, ctx)){
                    if(mac_out && mac_sz > 0){
                        for(int i = 0; i < 18; i++) mac_out[i] = (char)toupper((unsigned char)cand[i]);
                    }
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* Reconcile BlueALSA sink PCM, live ACL handle, and BlueZ Device1.Connected.
 * Stale /usr/data/fiio/bt_state == '1' NEVER marks connected without live evidence. */
static inline void bt_rx_reconcile_conn(const char *pcm_list,
                                        const char *hcitool_con,
                                        const char *bluez_connected_mac,
                                        const char *bluez_connected_name,
                                        int stale_bt_state,
                                        int force_drop_active,
                                        bt_rx_audio_src_fn is_audio_src,
                                        void *ctx,
                                        bt_rx_conn_info_t *out){
    if(!out) return;
    memset(out, 0, sizeof(*out));
    char pcm_mac[18] = {0};
    char acl_mac[18] = {0};
    out->has_sink_pcm = bt_rx_extract_sink_pcm_mac(pcm_list, pcm_mac, (int)sizeof pcm_mac);
    out->acl_active = bt_rx_extract_acl_mac(hcitool_con, is_audio_src, ctx, acl_mac, (int)sizeof acl_mac);
    out->bluez_connected = bt_rx_mac_valid(bluez_connected_mac);

    if(out->has_sink_pcm){
        out->connected = 1;
        snprintf(out->mac, sizeof out->mac, "%s", pcm_mac);
        if(out->bluez_connected && bt_rx_mac_eq_ci(pcm_mac, bluez_connected_mac) &&
           bluez_connected_name && bluez_connected_name[0]){
            snprintf(out->name, sizeof out->name, "%s", bluez_connected_name);
        }
        return;
    }

    /* No sink PCM: only consider ACL + BlueZ Connected if user did not just drop */
    if(!force_drop_active && out->acl_active && out->bluez_connected &&
       bt_rx_mac_eq_ci(acl_mac, bluez_connected_mac)){
        out->connected = 1;
        for(int i = 0; i < 18; i++){
            out->mac[i] = (char)toupper((unsigned char)bluez_connected_mac[i]);
        }
        if(bluez_connected_name && bluez_connected_name[0]){
            snprintf(out->name, sizeof out->name, "%s", bluez_connected_name);
        }
        return;
    }

    if(stale_bt_state){
        out->ignored_stale_bt_state = 1;
    }
}

typedef enum {
    BT_RX_AVRCP_ACT_PLAY_PAUSE = 0,
    BT_RX_AVRCP_ACT_NEXT,
    BT_RX_AVRCP_ACT_PREV
} bt_rx_avrcp_act_t;

typedef struct {
    int use_dbus;
    const char *ctrl_method;
    const char *player_method;
    const char *ipc_cmd;
} bt_rx_avrcp_plan_t;

static inline bt_rx_avrcp_plan_t bt_rx_plan_avrcp(bt_rx_avrcp_act_t act, int connected,
                                                  const char *dbus_mac, int is_playing){
    bt_rx_avrcp_plan_t p;
    memset(&p, 0, sizeof(p));
    switch(act){
        case BT_RX_AVRCP_ACT_NEXT:
            p.ctrl_method = "Next";
            p.player_method = "Next";
            p.ipc_cmd = "0201000C0001";
            break;
        case BT_RX_AVRCP_ACT_PREV:
            p.ctrl_method = "Previous";
            p.player_method = "Previous";
            p.ipc_cmd = "0201000C0002";
            break;
        case BT_RX_AVRCP_ACT_PLAY_PAUSE:
        default:
            p.ctrl_method = is_playing ? "Pause" : "Play";
            p.player_method = is_playing ? "Pause" : "Play";
            p.ipc_cmd = "0201000C0000";
            break;
    }
    p.use_dbus = (connected && dbus_mac && dbus_mac[0] != '\0') ? 1 : 0;
    return p;
}

static inline int bt_rx_should_heal(uint64_t now_ms, uint64_t enter_ms, uint64_t last_heal_ms,
                                    uint64_t grace_ms, uint64_t cooldown_ms,
                                    int connected, int bt_enabling){
    if(connected || bt_enabling) return 0;
    if(now_ms < enter_ms || (now_ms - enter_ms) < grace_ms) return 0;
    if(last_heal_ms != 0 && (now_ms < last_heal_ms || (now_ms - last_heal_ms) < cooldown_ms)) return 0;
    return 1;
}

static inline int bt_rx_clean_codec_label(const char *raw, char *out, int out_sz){
    if(!out || out_sz <= 0) return 0;
    out[0] = '\0';
    if(!raw) return 0;
    while(*raw == ' ' || *raw == '\t' || *raw == '\r' || *raw == '\n') raw++;
    size_t len = strlen(raw);
    while(len > 0 && (raw[len - 1] == ' ' || raw[len - 1] == '\t' ||
                      raw[len - 1] == '\r' || raw[len - 1] == '\n')){
        len--;
    }
    if(len == 0) return 0;
    if(len >= (size_t)out_sz) len = (size_t)out_sz - 1;
    memcpy(out, raw, len);
    out[len] = '\0';
    if(strcasecmp(out, "AUTO") == 0){
        out[0] = '\0';
        return 0;
    }
    return 1;
}

#endif
