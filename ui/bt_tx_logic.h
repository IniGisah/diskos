/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef DISKOS_BT_TX_LOGIC_H
#define DISKOS_BT_TX_LOGIC_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef BT_CODEC_N
#define BT_CODEC_N 7
#endif

#define BTC_AAC     2u
#define BTC_LDAC    4u
#define BTC_APTX    8u
#define BTC_APTX_HD 16u

typedef struct {
    char mac[20];
    char name[249];
    int connected;
} bt_tx_scan_dev_t;

static inline int bt_tx_mac_valid(const char *mac) {
    if (!mac || strlen(mac) != 17) return 0;
    for (int i = 0; i < 17; i++) {
        char c = mac[i];
        if ((i % 3) == 2) {
            if (c != ':') return 0;
        } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return mac[17] == '\0';
}

/* Extract a `bluetoothctl info` property value: line-anchored so Name:/Alias: cannot spoof it. */
static inline int bt_tx_info_prop(const char *buf, const char *key, char *out, int cap) {
    if (cap <= 0) return 0;
    out[0] = '\0';
    if (!buf || !key) return 0;
    size_t klen = strlen(key);
    for (const char *l = buf; l && *l; ) {
        const char *p = l;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, key, klen)) {
            p += klen;
            while (*p == ' ' || *p == '\t') p++;
            int i = 0;
            while (p[i] && p[i] != '\n' && p[i] != '\r' && i < cap - 1) {
                out[i] = p[i];
                i++;
            }
            while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t')) i--;
            out[i] = '\0';
            return 1;
        }
        const char *nl = strchr(l, '\n');
        l = nl ? nl + 1 : NULL;
    }
    return 0;
}

static inline int bt_tx_codec_frame_value(const char *mac, int v, char *out, int cap) {
    if (!bt_tx_mac_valid(mac) || !out || cap <= 0) return -1;
    if (v < 0 || v > 4) v = 0; /* stock 06b3 only supports 0..4 (SBC..LDAC High) */
    int r = snprintf(out, (size_t)cap, "06b3%04X%04X%s", (unsigned)(12 + 17), (unsigned)v, mac);
    return (r > 0 && r < cap) ? 0 : -1;
}

static inline const char *bt_tx_codec_cli_name(int choice) {
    switch (choice) {
    case 1: return "AAC";
    case 2:
    case 3:
    case 4: return "LDAC";
    case 5: return "aptX";
    case 6: return "aptX-HD";
    default: return "SBC";
    }
}

static inline int bt_tx_codec_pick(int choice, unsigned avail, int avail_known) {
    if (choice < 0 || choice >= BT_CODEC_N) return 0;
    if (!avail_known) return 0;

    if (choice == 6) {
        if (avail & BTC_APTX_HD) return 6;
        if (avail & BTC_APTX)    return 5;
        if (avail & BTC_AAC)     return 1;
        return 0;
    }
    if (choice == 5) {
        if (avail & BTC_APTX)    return 5;
        if (avail & BTC_AAC)     return 1;
        return 0;
    }
    if (choice >= 2 && choice <= 4) {
        if (avail & BTC_LDAC)    return choice;
        if (avail & BTC_AAC)     return 1;
        return 0;
    }
    if (choice == 1) {
        if (avail & BTC_AAC)     return 1;
        return 0;
    }
    return 0;
}

static inline unsigned bt_tx_codec_parse_avail(const char *info, int *known) {
    unsigned m = 0;
    if (known) *known = 0;
    const char *h = info ? strstr(info, "Available codecs:") : NULL;
    if (!h) return 0;
    if (known) *known = 1;
    const char *p = h + 17;
    for (int first = 1; *p; ) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (!first && memchr(p, ':', n)) break;
        first = 0;
        for (size_t i = 0; i < n; ) {
            size_t j = i;
            while (j < n && ((p[j] >= 'A' && p[j] <= 'Z') || (p[j] >= 'a' && p[j] <= 'z') ||
                             (p[j] >= '0' && p[j] <= '9') || p[j] == '-' || p[j] == '_')) {
                j++;
            }
            if (j - i == 3 && !strncasecmp(p + i, "AAC", 3)) m |= BTC_AAC;
            else if (j - i == 4 && !strncasecmp(p + i, "LDAC", 4)) m |= BTC_LDAC;
            else if (j - i == 4 && !strncasecmp(p + i, "aptX", 4)) m |= BTC_APTX;
            else if (j - i == 7 && (!strncasecmp(p + i, "aptX-HD", 7) || !strncasecmp(p + i, "aptX_HD", 7))) m |= BTC_APTX_HD;
            i = j > i ? j : i + 1;
        }
        if (!e) break;
        p = e + 1;
    }
    return m;
}

/* Parse `bluealsa-cli list-pcms` output (already verified complete via marker) for A2DP source PCMs.
 * Matches both /a2dpsrc/ and /a2dp-source/. */
static inline void bt_tx_parse_source_pcms(const char *list_text, const char *routed,
                                           int (*allowed_fn)(const char *mac, void *ctx), void *ctx,
                                           char *first_mac, size_t first_cap,
                                           int *out_found, int *out_routed_found) {
    if (first_mac && first_cap > 0) first_mac[0] = '\0';
    if (out_found) *out_found = 0;
    if (out_routed_found) *out_routed_found = 0;
    if (!list_text) return;

    const char *line = list_text;
    while (line && *line) {
        const char *nl = strpbrk(line, "\r\n");
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        /* Scan line for dev_XX_XX_XX_XX_XX_XX/(a2dpsrc|a2dp-source)/ */
        const char *end = line + llen;
        const char *cur = line;
        while (cur < end) {
            const char *dev = NULL;
            for (const char *s = cur; s + 26 <= end; s++) {
                if (!strncmp(s, "dev_", 4)) { dev = s; break; }
            }
            if (!dev) break;
            if ((size_t)(end - (dev + 4)) >= 25 &&
                (!strncmp(dev + 21, "/a2dpsrc/", 9) ||
                 ((size_t)(end - (dev + 4)) >= 29 && !strncmp(dev + 21, "/a2dp-source/", 13)))) {
                char candidate[20];
                memcpy(candidate, dev + 4, 17);
                candidate[17] = '\0';
                for (int i = 0; i < 17; i++) {
                    if (candidate[i] == '_') candidate[i] = ':';
                    if (candidate[i] >= 'a' && candidate[i] <= 'f') candidate[i] -= 32;
                }
                if (bt_tx_mac_valid(candidate)) {
                    if (out_found) *out_found = 1;
                    if (first_mac && first_cap > 0 && !first_mac[0] && (!allowed_fn || allowed_fn(candidate, ctx))) {
                        snprintf(first_mac, first_cap, "%s", candidate);
                    }
                    if (routed && routed[0] && !strcasecmp(candidate, routed)) {
                        if (out_routed_found) *out_routed_found = 1;
                    }
                }
                break;
            }
            cur = dev + 4;
        }
        if (!nl) break;
        line = nl + 1;
        while (*line == '\r' || *line == '\n') line++;
    }
}

/* Parse combined `bluetoothctl info <MAC> ; echo DISKOS_INFO_DONE ; hcitool con ; echo DISKOS_HCI_DONE`
 * Return: 1 = confirmed disconnected, 0 = connected, -1 = unknown. */
static inline int bt_tx_parse_link_check(const char *buf, const char *mac) {
    if (!buf || !bt_tx_mac_valid(mac)) return -1;
    const char *info_marker = "\nDISKOS_INFO_DONE\n";
    const char *hci_marker  = "\nDISKOS_HCI_DONE\n";
    const char *done = strstr(buf, info_marker);
    if (done) {
        size_t ilen = (size_t)(done - buf);
        if (ilen < 4096) {
            char info[4096];
            memcpy(info, buf, ilen);
            info[ilen] = '\0';
            char device[32], value[16];
            snprintf(device, sizeof(device), "Device %s", mac);
            size_t dn = strlen(device);
            if (!strncasecmp(info, device, dn) && (info[dn] == ' ' || info[dn] == '\r' || info[dn] == '\n')) {
                if (bt_tx_info_prop(info, "Connected:", value, sizeof(value))) {
                    if (!strcmp(value, "no")) return 1;
                    if (!strcmp(value, "yes")) return 0;
                }
            }
        }
    }

    const char *hci_start = done ? (done + strlen(info_marker)) : buf;
    const char *hci_end = strstr(hci_start, hci_marker);
    if (hci_end && !hci_end[strlen(hci_marker)]) {
        char upper_mac[20];
        for (int i = 0; i < 17; i++) {
            char c = mac[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            upper_mac[i] = c;
        }
        upper_mac[17] = '\0';
        size_t hlen = (size_t)(hci_end - hci_start);
        if (hlen < 2048) {
            char hbuf[2048];
            memcpy(hbuf, hci_start, hlen);
            hbuf[hlen] = '\0';
            if (strstr(hbuf, upper_mac)) return 0;
        }
    }
    return -1;
}

/* Map BlueALSA "Selected codec:" string back to 0..6 UI index (preserving LDAC quality 2/3/4). */
static inline int bt_tx_choice_from_codec_name(const char *name, int pref_choice) {
    if (!name || !name[0]) return -1;
    if (!strcasecmp(name, "SBC")) return 0;
    if (!strcasecmp(name, "AAC")) return 1;
    if (!strcasecmp(name, "LDAC")) return (pref_choice >= 2 && pref_choice <= 4) ? pref_choice : 3;
    if (!strcasecmp(name, "aptX")) return 5;
    if (!strcasecmp(name, "aptX-HD") || !strcasecmp(name, "aptX HD")) return 6;
    return -1;
}

static inline int bt_tx_cod_is_audio(unsigned cod) {
    /* Bluetooth CoD Major Device Class bits 12..8 == 0x04 (Audio/Video). */
    return ((cod >> 8) & 0x1Fu) == 0x04u;
}

/* Parse `bluetoothctl info` output to decide if an un-iconed / pairing device is an audio sink. */
static inline int bt_tx_info_is_audio(const char *info) {
    if (!info || !info[0]) return 0;
    char prop[256];
    if (bt_tx_info_prop(info, "Icon:", prop, (int)sizeof(prop)) && prop[0]) {
        return !strncmp(prop, "audio", 5);
    }
    if (bt_tx_info_prop(info, "Class:", prop, (int)sizeof(prop)) && prop[0]) {
        unsigned long cod = strtoul(prop, NULL, 0);
        if (bt_tx_cod_is_audio((unsigned)cod)) return 1;
    }
    if (strstr(info, "0000110b-") || strstr(info, "0000110b") || strstr(info, "Audio Sink")) return 1;
    if (bt_tx_info_prop(info, "Connected:", prop, (int)sizeof(prop)) && !strcmp(prop, "yes")) return 1;
    if (bt_tx_info_prop(info, "Paired:", prop, (int)sizeof(prop)) && !strcmp(prop, "yes")) return 1;
    return 0;
}

static inline int bt_tx_is_line_obj_hdr(const char *base, const char *pos) {
    const char *line = pos;
    while (line > base && line[-1] != '\n' && line[-1] != '\r') line--;
    for (const char *c = line; c < pos; c++) {
        if (*c != ' ' && *c != '\t') return 0;
    }
    return 1;
}

static inline const char *bt_tx_find_next_obj(const char *cur, const char *base) {
    const char *p = cur;
    while (p && (p = strstr(p, "object path \"/org/bluez/")) != NULL) {
        if (bt_tx_is_line_obj_hdr(base, p)) return p;
        p += 13;
    }
    return NULL;
}

/* Parse `dbus-send --system --print-reply --dest=org.bluez / org.freedesktop.DBus.ObjectManager.GetManagedObjects`
 * output for audio devices (Icon starting with "audio", or pairing devices with Audio CoD / A2DP Sink UUID). */
static inline int bt_tx_parse_managed_objects(const char *buf, bt_tx_scan_dev_t *res, int max_devs) {
    if (!buf || !res || max_devs <= 0) return 0;
    int count = 0;
    const char *p = buf;
    while (count < max_devs) {
        const char *dev = strstr(p, "string \"org.bluez.Device1\"");
        if (!dev) break;
        const char *next_dev = bt_tx_find_next_obj(dev + 26, buf);
        size_t blk_len = next_dev ? (size_t)(next_dev - dev) : strlen(dev);
        char mac[20] = {0}, icon[64] = {0}, alias[249] = {0}, name[249] = {0};
        int conn = 0, paired = 0, has_cod_audio = 0, has_a2dp_uuid = 0;
        const char *cur = dev;
        const char *end = dev + blk_len;
        while (cur < end) {
            const char *de = strstr(cur, "dict entry(");
            if (!de || de >= end) break;
            const char *q1 = strstr(de, "string \"");
            if (!q1 || q1 >= end) break;
            q1 += 8;
            const char *q2 = memchr(q1, '"', (size_t)(end - q1));
            if (!q2) break;
            size_t klen = (size_t)(q2 - q1);
            const char *var = strstr(q2, "variant");
            if (var && var < end) {
                var += 7;
                while (var < end && (*var == ' ' || *var == '\t' || *var == '\r' || *var == '\n')) var++;
                if (var + 8 <= end && !strncmp(var, "string \"", 8)) {
                    const char *vs = var + 8;
                    const char *ve = memchr(vs, '\n', (size_t)(end - vs));
                    const char *vq = ve ? ve : end;
                    while (vq > vs && vq[-1] != '"') vq--;
                    if (vq > vs && vq[-1] == '"') vq--;
                    size_t vlen = (size_t)(vq - vs);
                    if (klen == 7 && !strncmp(q1, "Address", 7) && vlen == 17) {
                        memcpy(mac, vs, 17);
                        mac[17] = '\0';
                    } else if (klen == 4 && !strncmp(q1, "Icon", 4) && vlen < sizeof(icon)) {
                        memcpy(icon, vs, vlen);
                        icon[vlen] = '\0';
                    } else if (klen == 5 && !strncmp(q1, "Alias", 5)) {
                        size_t take = vlen < sizeof(alias) - 1 ? vlen : sizeof(alias) - 1;
                        memcpy(alias, vs, take);
                        alias[take] = '\0';
                    } else if (klen == 4 && !strncmp(q1, "Name", 4)) {
                        size_t take = vlen < sizeof(name) - 1 ? vlen : sizeof(name) - 1;
                        memcpy(name, vs, take);
                        name[take] = '\0';
                    }
                } else if (var + 8 <= end && !strncmp(var, "boolean ", 8)) {
                    int bval = !strncmp(var + 8, "true", 4);
                    if (klen == 9 && !strncmp(q1, "Connected", 9)) conn = bval;
                    else if (klen == 6 && !strncmp(q1, "Paired", 6)) paired = bval;
                } else if (klen == 5 && !strncmp(q1, "Class", 5) && var + 7 <= end && !strncmp(var, "uint32 ", 7)) {
                    unsigned long cod = strtoul(var + 7, NULL, 0);
                    if (bt_tx_cod_is_audio((unsigned)cod)) has_cod_audio = 1;
                } else if (klen == 5 && !strncmp(q1, "UUIDs", 5)) {
                    const char *arr_end = strstr(var, "]");
                    if (!arr_end || arr_end > end) arr_end = end;
                    for (const char *u = var; u + 8 <= arr_end; u++) {
                        if (!strncasecmp(u, "0000110b", 8)) { has_a2dp_uuid = 1; break; }
                    }
                }
            }
            cur = q2 + 1;
        }
        int is_audio = icon[0] ? (!strncmp(icon, "audio", 5)) :
                       (has_cod_audio || has_a2dp_uuid ||
                        ((paired || conn) && (alias[0] || name[0])));
        if (bt_tx_mac_valid(mac) && is_audio) {
            int dup = 0;
            for (int i = 0; i < count; i++) {
                if (!strcasecmp(res[i].mac, mac)) { dup = 1; break; }
            }
            if (!dup) {
                snprintf(res[count].mac, sizeof(res[count].mac), "%s", mac);
                const char *disp = alias[0] ? alias : (name[0] ? name : mac);
                snprintf(res[count].name, sizeof(res[count].name), "%s", disp);
                res[count].connected = (conn ? 1 : 0) | (paired ? 2 : 0);
                count++;
            }
        }
        p = next_dev ? next_dev : end;
    }
    return count;
}

enum {
    BT_ROW_AVAIL = 0,
    BT_ROW_PAIRED,
    BT_ROW_CONNECTING,
    BT_ROW_CONNECTED,
    BT_ROW_ROUTED
};

static inline int bt_tx_classify_row(const char *mac, int conn_flags, const char *attempt_mac,
                                       int attempt_active, const char *routed_mac) {
    if (!mac || !mac[0]) return BT_ROW_AVAIL;
    if (attempt_active && attempt_mac && attempt_mac[0] && !strcasecmp(mac, attempt_mac)) {
        return BT_ROW_CONNECTING;
    }
    if (conn_flags & 1) {
        if (routed_mac && routed_mac[0] && !strcasecmp(mac, routed_mac)) {
            return BT_ROW_ROUTED;
        }
        return BT_ROW_CONNECTED;
    }
    if (conn_flags & 2) {
        return BT_ROW_PAIRED;
    }
    return BT_ROW_AVAIL;
}

static inline const char *bt_tx_device_type_label(const char *icon) {
    if (!icon || !icon[0]) return "Audio";
    if (!strcmp(icon, "audio-headphones")) return "Headphones";
    if (!strcmp(icon, "audio-headset"))    return "Headset";
    if (!strcmp(icon, "audio-handsfree"))  return "Handsfree";
    if (!strcmp(icon, "audio-speakers"))   return "Speaker";
    if (!strcmp(icon, "audio-card"))       return "Audio DAC";
    return icon;
}

static inline const char *bt_tx_codec_short_name(int choice) {
    switch (choice) {
    case 1: return "AAC";
    case 2: return "LDAC (Eco)";
    case 3: return "LDAC (Balanced)";
    case 4: return "LDAC (High)";
    case 5: return "aptX";
    case 6: return "aptX HD";
    default: return "SBC";
    }
}

#endif /* DISKOS_BT_TX_LOGIC_H */
