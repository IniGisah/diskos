/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "modelock.h"
#include "device_names.h"
#include "screens.h"
#include "anim.h"
#include "ipc.h"
#include "config.h"
#include "theme.h"
#include "theme_kit.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

/* State of mode lockdown */
static int g_lock_mode = 0;
static lv_obj_t *g_lock_root = NULL;

/* Header widgets */
static lv_obj_t *g_hdr_title;

/* Mode containers */
static lv_obj_t *g_cont_bt;
static lv_obj_t *g_cont_dac;
static lv_obj_t *g_cont_storage;

/* Bluetooth Receiver view widgets */
static lv_obj_t *g_bt_disc;
static lv_obj_t *g_bt_spindle;
static lv_obj_t *g_bt_track_title;
static lv_obj_t *g_bt_track_sub;
static lv_obj_t *g_bt_codec_badge;
static lv_obj_t *g_bt_codec_lbl;
static lv_obj_t *g_btn_prev;
static lv_obj_t *g_btn_pp;
static lv_obj_t *g_btn_next;
static lv_obj_t *g_lbl_pp;

/* Exit confirmation modal */
static lv_obj_t *g_exit_modal = NULL;

/* USB DAC view widgets */
static lv_obj_t *g_dac_circle;
static lv_obj_t *g_dac_icon;
static lv_obj_t *g_dac_title;
static lv_obj_t *g_dac_sub;
static lv_obj_t *g_dac_badge;
static lv_obj_t *g_dac_badge_lbl;

/* USB Storage view widgets */
static lv_obj_t *g_stor_circle;
static lv_obj_t *g_stor_icon;
static lv_obj_t *g_stor_title;
static lv_obj_t *g_stor_badge;
static lv_obj_t *g_stor_badge_lbl;
static lv_obj_t *g_stor_sub;

static bt_rx_state_t g_bt_state;
static pthread_mutex_t g_bt_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_worker_th;
static _Atomic int g_worker_run = 0;
static lv_timer_t *g_poll_timer = NULL;
static void bt_disc_spin(int want){
    (void)want;
}

void bt_rx_log(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    char buf[128];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    diskos_trace("%s", buf);
}

/* Helper to execute a quick bounded command without hanging */
static int exec_cmd_capture(const char *cmd, char *out, int maxlen, int timeout_ms){
    return ui_run_cap_bounded(cmd, out, maxlen, timeout_ms);
}

static int dbus_extract_tag(const char *buf, const char *tag, char *dst, size_t dstsz){
    char pat[64];
    snprintf(pat, sizeof pat, "string \"%s\"", tag);
    char *p = strstr(buf, pat);
    if(!p) return 0;
    char *v = strstr(p, "variant");
    if(!v) return 0;
    char *next_entry = strstr(p + strlen(pat), "dict entry(");
    if(next_entry && v > next_entry) return 0;
    char *s = strstr(v, "string \"");
    if(!s) return 0;
    if(next_entry && s > next_entry) return 0;
    char *q1 = s + 8;
    char *eol = strpbrk(q1, "\r\n");
    char *q2 = eol ? (eol - 1) : (q1 + strlen(q1));
    while(q2 >= q1 && *q2 != '"') q2--;
    if(q2 < q1) return 0;
    int len = q2 - q1;
    if(len < 0) return 0;
    if(len >= (int)dstsz) len = dstsz - 1;
    memcpy(dst, q1, len);
    dst[len] = 0;
    return 1;
}

static int dbus_extract_u32(const char *buf, const char *tag, uint32_t *out){
    if(!buf || !tag || !out) return 0;
    char pat[64];
    snprintf(pat, sizeof pat, "string \"%s\"", tag);
    const char *p = strstr(buf, pat);
    if(!p) return 0;
    const char *v = strstr(p, "variant");
    if(!v) return 0;
    const char *next_entry = strstr(p + strlen(pat), "dict entry(");
    if(next_entry && v > next_entry) return 0;
    v += 7; /* skip "variant" */
    while(*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
    /* skip type name (uint32, int64, uint64, int32) */
    while(*v && *v != ' ' && *v != '\t' && *v != '\n' && *v != '\r') v++;
    while(*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
    if(!*v || (*v < '0' || *v > '9')) return 0;
    if(next_entry && v > next_entry) return 0;
    char *end = NULL;
    unsigned long val = strtoul(v, &end, 10);
    if(end == v) return 0;
    *out = (uint32_t)val;
    return 1;
}

static int dbus_parse_variant_u32(const char *buf, uint32_t *out){
    if(!buf || !out) return 0;
    const char *v = strstr(buf, "variant");
    if(!v) return 0;
    v += 7; /* skip "variant" */
    while(*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
    /* skip type name */
    while(*v && *v != ' ' && *v != '\t' && *v != '\n' && *v != '\r') v++;
    while(*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
    if(!*v || (*v < '0' || *v > '9')) return 0;
    char *end = NULL;
    unsigned long val = strtoul(v, &end, 10);
    if(end == v) return 0;
    *out = (uint32_t)val;
    return 1;
}

static void *bt_rx_worker(void *arg);

void bt_rx_worker_ensure(void){
    if(!g_bt_state.local_name[0]){
        pthread_mutex_lock(&g_bt_mu);
        dn_bt_name_copy(g_bt_state.local_name, sizeof g_bt_state.local_name);
        pthread_mutex_unlock(&g_bt_mu);
    }
    if(!g_worker_run){
        g_worker_run = 1;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        pthread_create(&g_worker_th, &attr, bt_rx_worker, NULL);
        pthread_attr_destroy(&attr);
    }
}

void bt_rx_get_state(bt_rx_state_t *out){
    if(!out) return;
    pthread_mutex_lock(&g_bt_mu);
    *out = g_bt_state;
    pthread_mutex_unlock(&g_bt_mu);
}

void bt_rx_get_progress(uint32_t *out_elapsed_ms, uint32_t *out_dur_ms, int *out_fraction){
    bt_rx_state_t st;
    pthread_mutex_lock(&g_bt_mu);
    st = g_bt_state;
    pthread_mutex_unlock(&g_bt_mu);

    uint32_t dur = st.duration_ms;
    uint32_t elapsed = st.position_ms;
    if(st.connected && st.is_playing && st.pos_anchor_ms > 0){
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
        if(now_ms > st.pos_anchor_ms){
            uint64_t delta = now_ms - st.pos_anchor_ms;
            elapsed += (uint32_t)delta;
            if(dur > 0 && elapsed > dur) elapsed = dur;
        }
    }
    int frac = 0;
    if(dur > 0 && elapsed > 0){
        frac = (int)(((uint64_t)elapsed * 1000) / dur);
        if(frac > 1000) frac = 1000;
    }
    if(out_elapsed_ms) *out_elapsed_ms = elapsed;
    if(out_dur_ms) *out_dur_ms = dur;
    if(out_fraction) *out_fraction = frac;
}

/* Background worker thread: polls Bluetooth connection and AVRCP track info */
static void *bt_rx_worker(void *arg){
    (void)arg;
    char buf[2048];
    char mac[20] = {0};
    char dbus_mac[32] = {0};
    char name[64] = {0};
    char pcm_path[128] = {0};
    char local_name[64] = {0};
    int found = 0;

    dn_bt_name_copy(local_name, sizeof local_name);

    diskos_trace("bt_worker: thread started");

    while(g_worker_run){
        if(g_lock_mode != 2 && ui_get_source_mode() != 2){
            usleep(500000);
            continue;
        }

        /* 1. Fast connection check: verify bt_state */
        if(found){
            FILE *f = fopen("/usr/data/fiio/bt_state", "r");
            if(f){
                char ch = (char)fgetc(f);
                fclose(f);
                if(ch != '1') found = 0;
            } else {
                found = 0;
            }
        }

        /* 2. Discover connected device only when not already cached */
        if(!found){
            mac[0] = 0;
            dbus_mac[0] = 0;
            name[0] = 0;

            /* Check bluealsa-cli list-pcms for an active A2DP sink PCM */
            int n = exec_cmd_capture("bluealsa-cli list-pcms 2>/dev/null", buf, sizeof buf, 300);
            if(n > 0){
                char *p = strstr(buf, "/org/bluealsa/hci0/dev_");
                if(p){
                    char *pend = p; while(*pend && *pend != '\r' && *pend != '\n' && *pend != ' ') pend++;
                    int plen = (int)(pend - p);
                    if(plen > 0 && plen < (int)sizeof(pcm_path)){
                        memcpy(pcm_path, p, plen);
                        pcm_path[plen] = 0;
                    }
                    p += 23;
                    if(strlen(p) >= 17){
                        memcpy(dbus_mac, p, 17);
                        dbus_mac[17] = 0;
                        for(int i = 0; i < 17; i++)
                            mac[i] = (dbus_mac[i] == '_') ? ':' : dbus_mac[i];
                        mac[17] = 0;
                        found = 1;
                    }
                }
            }

            /* Fallback: check /usr/data/fiio/bt_state + hcitool con */
            if(!found){
                FILE *f = fopen("/usr/data/fiio/bt_state", "r");
                if(f){
                    char ch = (char)fgetc(f);
                    fclose(f);
                    if(ch == '1'){
                        if(exec_cmd_capture("hcitool con 2>/dev/null", buf, sizeof buf, 300) > 0){
                            char *line = strstr(buf, "ACL ");
                            if(line && strlen(line) >= 21){
                                line += 4;
                                if(line[2] == ':' && line[5] == ':' && line[8] == ':' && line[11] == ':' && line[14] == ':'){
                                    memcpy(mac, line, 17);
                                    mac[17] = 0;
                                    for(int i = 0; i < 17; i++)
                                        dbus_mac[i] = (mac[i] == ':') ? '_' : mac[i];
                                    dbus_mac[17] = 0;
                                    found = 1;
                                }
                            }
                        }
                    }
                }
            }

            if(!found){
                pcm_path[0] = 0;
                dn_bt_name_copy(local_name, sizeof local_name);
                static int s_logged_not_found = 0;
                if(!s_logged_not_found){
                    diskos_trace("bt_worker: device not found");
                    s_logged_not_found = 1;
                }
                pthread_mutex_lock(&g_bt_mu);
                g_bt_state.connected = 0;
                g_bt_state.is_playing = 0;
                snprintf(g_bt_state.local_name, sizeof g_bt_state.local_name, "%s", local_name);
                g_bt_state.mac[0] = 0;
                g_bt_state.dbus_mac[0] = 0;
                g_bt_state.title[0] = 0;
                g_bt_state.artist[0] = 0;
                g_bt_state.album[0] = 0;
                g_bt_state.codec[0] = 0;
                g_bt_state.duration_ms = 0;
                g_bt_state.position_ms = 0;
                g_bt_state.pos_anchor_ms = 0;
                pthread_mutex_unlock(&g_bt_mu);
                usleep(1500000);
                continue;
            }

            /* Connected! Query device name once */
            snprintf(name, sizeof name, "Connected Device");
            char ncmd[256];
            snprintf(ncmd, sizeof ncmd, "bluetoothctl info %s 2>/dev/null | grep -E '^[ \t]*Name:' | cut -d: -f2-", mac);
            if(exec_cmd_capture(ncmd, buf, sizeof buf, 400) > 0){
                char *p = buf; while(*p == ' ' || *p == '\t') p++;
                char *e = p + strlen(p); while(e > p && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ')) *--e = 0;
                if(*p) snprintf(name, sizeof name, "%s", p);
            }
            diskos_trace("bt_worker: connected mac=%s name='%s'", mac, name);
        }

        /* 3. Query player status & metadata */
        char cmd[256];
        int is_play = 0;
        char title[128] = {0}, artist[128] = {0}, album[128] = {0};
        uint32_t dur_ms = 0;
        uint32_t pos_ms = 0;
        int has_pos = 0;

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t sample_time = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;

        /* Query player status */
        snprintf(cmd, sizeof cmd, "dbus-send --system --print-reply --dest=org.bluez /org/bluez/hci0/dev_%s/player0 org.freedesktop.DBus.Properties.Get string:org.bluez.MediaPlayer1 string:Status 2>/dev/null", dbus_mac);
        if(exec_cmd_capture(cmd, buf, sizeof buf, 400) > 0){
            if(strstr(buf, "\"playing\"")) is_play = 1;
        }

        /* Query track metadata and duration */
        snprintf(cmd, sizeof cmd, "dbus-send --system --print-reply --dest=org.bluez /org/bluez/hci0/dev_%s/player0 org.freedesktop.DBus.Properties.Get string:org.bluez.MediaPlayer1 string:Track 2>/dev/null", dbus_mac);
        if(exec_cmd_capture(cmd, buf, sizeof buf, 500) > 0){
            dbus_extract_tag(buf, "Title", title, sizeof title);
            dbus_extract_tag(buf, "Artist", artist, sizeof artist);
            dbus_extract_tag(buf, "Album", album, sizeof album);
            dbus_extract_u32(buf, "Duration", &dur_ms);
        }

        /* Query playback position */
        snprintf(cmd, sizeof cmd, "dbus-send --system --print-reply --dest=org.bluez /org/bluez/hci0/dev_%s/player0 org.freedesktop.DBus.Properties.Get string:org.bluez.MediaPlayer1 string:Position 2>/dev/null", dbus_mac);
        if(exec_cmd_capture(cmd, buf, sizeof buf, 400) > 0){
            if(dbus_parse_variant_u32(buf, &pos_ms)) has_pos = 1;
        }

        /* 4. Query BlueALSA for real-time dynamic Codec, Sample Rate, and Bit depth */
        char codec_str[64] = {0};
        char ccmd[256];
        int info_ok = 0;
        if(pcm_path[0]){
            snprintf(ccmd, sizeof ccmd, "bluealsa-cli info %s 2>/dev/null", pcm_path);
            if(exec_cmd_capture(ccmd, buf, sizeof buf, 400) > 0) info_ok = 1;
        }
        if(!info_ok && dbus_mac[0]){
            snprintf(ccmd, sizeof ccmd, "bluealsa-cli info /org/bluealsa/hci0/dev_%s/a2dpsnk/source 2>/dev/null || bluealsa-cli info /org/bluealsa/hci0/dev_%s/a2dp-sink/source 2>/dev/null", dbus_mac, dbus_mac);
            if(exec_cmd_capture(ccmd, buf, sizeof buf, 400) > 0) info_ok = 1;
        }
        if(info_ok){
            char cname[20] = {0};
            char rate[20] = {0};
            char bits[16] = {0};
            char *c = strstr(buf, "Selected codec:");
            if(c){
                c += 15; while(*c == ' ' || *c == '\t') c++;
                char *end = c; while(*end && *end != '\r' && *end != '\n') end++;
                int len = end - c;
                if(len > 0 && len < (int)sizeof(cname)){
                    memcpy(cname, c, len);
                    cname[len] = 0;
                }
            }
            char *s = strstr(buf, "Sampling:");
            if(s){
                s += 9; while(*s == ' ' || *s == '\t') s++;
                int hz = atoi(s);
                if(hz >= 1000){
                    if(hz % 1000 == 0) snprintf(rate, sizeof rate, "%dkHz", hz / 1000);
                    else snprintf(rate, sizeof rate, "%d.%dkHz", hz / 1000, (hz % 1000) / 100);
                }
            }
            char *f = strstr(buf, "Format:");
            if(f){
                char fline[32] = {0};
                f += 7; while(*f == ' ' || *f == '\t') f++;
                char *fend = f; while(*fend && *fend != '\r' && *fend != '\n') fend++;
                int flen = fend - f;
                if(flen > 0 && flen < (int)sizeof(fline)){
                    memcpy(fline, f, flen);
                    fline[flen] = 0;
                    if(strstr(fline, "S32")) snprintf(bits, sizeof bits, "24/32bit");
                    else if(strstr(fline, "S24")) snprintf(bits, sizeof bits, "24bit");
                    else if(strstr(fline, "S16")) snprintf(bits, sizeof bits, "16bit");
                }
            }
            if(cname[0]){
                if(rate[0] && bits[0])
                    snprintf(codec_str, sizeof codec_str, "%s • %s • %s", cname, rate, bits);
                else if(rate[0])
                    snprintf(codec_str, sizeof codec_str, "%s • %s", cname, rate);
                else
                    snprintf(codec_str, sizeof codec_str, "%s", cname);
            }
        }
        /* Fallback: if BlueALSA returned nothing, check /usr/data/bt_codec */
        if(codec_str[0] == 0){
            FILE *fc = fopen("/usr/data/bt_codec", "r");
            if(fc){
                char cbuf[32] = {0};
                if(fgets(cbuf, sizeof cbuf, fc)){
                    char *comma = strchr(cbuf, ',');
                    if(comma) *comma = 0;
                    char *nl = strpbrk(cbuf, "\r\n");
                    if(nl) *nl = 0;
                    if(cbuf[0]){
                        if(!strcmp(cbuf, "LDAC")) snprintf(codec_str, sizeof codec_str, "LDAC • 96kHz • 24/32bit");
                        else if(!strcmp(cbuf, "AAC")) snprintf(codec_str, sizeof codec_str, "AAC • 44.1kHz • 16bit");
                        else if(!strcmp(cbuf, "SBC")) snprintf(codec_str, sizeof codec_str, "SBC • 44.1kHz • 16bit");
                        else if(!strcmp(cbuf, "aptX")) snprintf(codec_str, sizeof codec_str, "aptX • 44.1kHz • 16bit");
                        else if(!strcmp(cbuf, "aptX-HD")) snprintf(codec_str, sizeof codec_str, "aptX HD • 48kHz • 24bit");
                        else snprintf(codec_str, sizeof codec_str, "%s", cbuf);
                    }
                }
                fclose(fc);
            }
        }

        /* 5. Update shared state under mutex */
        pthread_mutex_lock(&g_bt_mu);
        g_bt_state.connected = 1;
        int was_playing = g_bt_state.is_playing;
        g_bt_state.is_playing = is_play;
        snprintf(g_bt_state.local_name, sizeof g_bt_state.local_name, "%s", local_name);
        snprintf(g_bt_state.mac, sizeof g_bt_state.mac, "%s", mac);
        snprintf(g_bt_state.dbus_mac, sizeof g_bt_state.dbus_mac, "%s", dbus_mac);
        snprintf(g_bt_state.dev_name, sizeof g_bt_state.dev_name, "%s", name);

        int track_changed = 0;
        if(title[0]){
            if(strcmp(g_bt_state.title, title) != 0) track_changed = 1;
            snprintf(g_bt_state.title, sizeof g_bt_state.title, "%s", title);
            snprintf(g_bt_state.artist, sizeof g_bt_state.artist, "%s", artist);
            snprintf(g_bt_state.album, sizeof g_bt_state.album, "%s", album);
        } else if(!is_play && !was_playing){
            /* Playback stopped and no track reported */
            g_bt_state.title[0] = 0;
            g_bt_state.artist[0] = 0;
            g_bt_state.album[0] = 0;
            track_changed = 1;
        }

        if(dur_ms > 0) g_bt_state.duration_ms = dur_ms;
        else if(track_changed) g_bt_state.duration_ms = 0;

        /* Dead-reckoning position tracking */
        static uint32_t last_raw_pos = 0;
        if(track_changed){
            g_bt_state.position_ms = has_pos ? pos_ms : 0;
            g_bt_state.pos_anchor_ms = is_play ? sample_time : 0;
            last_raw_pos = has_pos ? pos_ms : 0;
        } else if(was_playing != is_play){
            if(!is_play){
                /* Paused: freeze progress at current dead-reckoned offset */
                if(g_bt_state.pos_anchor_ms > 0 && sample_time > g_bt_state.pos_anchor_ms){
                    g_bt_state.position_ms += (uint32_t)(sample_time - g_bt_state.pos_anchor_ms);
                    if(g_bt_state.duration_ms > 0 && g_bt_state.position_ms > g_bt_state.duration_ms)
                        g_bt_state.position_ms = g_bt_state.duration_ms;
                }
                g_bt_state.pos_anchor_ms = 0;
            } else {
                /* Resumed: start anchor from current sample_time */
                g_bt_state.pos_anchor_ms = sample_time;
                if(has_pos){ g_bt_state.position_ms = pos_ms; last_raw_pos = pos_ms; }
            }
        } else if(is_play){
            if(has_pos && pos_ms != last_raw_pos){
                g_bt_state.position_ms = pos_ms;
                g_bt_state.pos_anchor_ms = sample_time;
                last_raw_pos = pos_ms;
            } else if(g_bt_state.pos_anchor_ms == 0){
                g_bt_state.pos_anchor_ms = sample_time;
                if(has_pos){ g_bt_state.position_ms = pos_ms; last_raw_pos = pos_ms; }
            }
        }

        int codec_changed = 0;
        if(codec_str[0] && strcmp(g_bt_state.codec, codec_str) != 0){
            codec_changed = 1;
            snprintf(g_bt_state.codec, sizeof g_bt_state.codec, "%s", codec_str);
        }
        if(track_changed || was_playing != is_play || codec_changed){
            diskos_trace("bt_worker: play=%d (was=%d) title='%s' dur=%u pos=%u codec='%s'",
                         is_play, was_playing, g_bt_state.title, g_bt_state.duration_ms, g_bt_state.position_ms, g_bt_state.codec);
        }
        pthread_mutex_unlock(&g_bt_mu);

        usleep(1500000);
    }
    return NULL;
}

/* AVRCP commands for Bluetooth receiving mode:
 * Stock mq_ui sends 0201 frames directly to /player. mq_player translates them
 * to AVRCP commands (0201000C0000=play/pause, 0201000C0001=next, 0201000C0002=prev). */
void bt_rx_play_pause(void){
    diskos_trace("bt_rx_play_pause: send");
    ipc_send_cmd("0201000C0000");
    char dbus_mac[32] = {0};
    int is_playing = 0;
    pthread_mutex_lock(&g_bt_mu);
    if(g_bt_state.connected && g_bt_state.dbus_mac[0]){
        snprintf(dbus_mac, sizeof dbus_mac, "%s", g_bt_state.dbus_mac);
        is_playing = g_bt_state.is_playing;
    }
    pthread_mutex_unlock(&g_bt_mu);
    if(dbus_mac[0]){
        char cmd[256];
        snprintf(cmd, sizeof cmd, "dbus-send --system --dest=org.bluez /org/bluez/hci0/dev_%s org.bluez.MediaControl1.%s >/dev/null 2>&1 &",
                 dbus_mac, is_playing ? "Pause" : "Play");
        system(cmd);
    }
}

void bt_rx_next(void){
    ipc_send_cmd("0201000C0001");
    char dbus_mac[32] = {0};
    pthread_mutex_lock(&g_bt_mu);
    if(g_bt_state.connected && g_bt_state.dbus_mac[0]){
        snprintf(dbus_mac, sizeof dbus_mac, "%s", g_bt_state.dbus_mac);
    }
    pthread_mutex_unlock(&g_bt_mu);
    if(dbus_mac[0]){
        char cmd[256];
        snprintf(cmd, sizeof cmd, "dbus-send --system --dest=org.bluez /org/bluez/hci0/dev_%s org.bluez.MediaControl1.Next >/dev/null 2>&1 &", dbus_mac);
        system(cmd);
    }
}

void bt_rx_prev(void){
    ipc_send_cmd("0201000C0002");
    char dbus_mac[32] = {0};
    pthread_mutex_lock(&g_bt_mu);
    if(g_bt_state.connected && g_bt_state.dbus_mac[0]){
        snprintf(dbus_mac, sizeof dbus_mac, "%s", g_bt_state.dbus_mac);
    }
    pthread_mutex_unlock(&g_bt_mu);
    if(dbus_mac[0]){
        char cmd[256];
        snprintf(cmd, sizeof cmd, "dbus-send --system --dest=org.bluez /org/bluez/hci0/dev_%s org.bluez.MediaControl1.Previous >/dev/null 2>&1 &", dbus_mac);
        system(cmd);
    }
}

static void uac_query_status(int *out_connected, char *out_fmt, size_t fmt_sz){
    if(out_connected) *out_connected = 0;
    if(out_fmt && fmt_sz > 0) out_fmt[0] = 0;

    /* Check UDC state */
    FILE *f = fopen("/sys/class/udc/13500000.otg_new/state", "r");
    if(f){
        char state[32] = {0};
        if(fgets(state, sizeof state, f)){
            if(strstr(state, "configured")) {
                if(out_connected) *out_connected = 1;
            }
        }
        fclose(f);
    }

    char rate[24] = {0};
    char bits[16] = {0};

    f = fopen("/proc/asound/card1/pcm0c/sub0/hw_params", "r");
    if(!f) f = fopen("/proc/asound/card0/pcm3p/sub0/hw_params", "r");
    if(f){
        char line[128];
        while(fgets(line, sizeof line, f)){
            if(strstr(line, "closed")) break;
            char *r = strstr(line, "rate: ");
            if(r){
                int hz = atoi(r + 6);
                if(hz >= 1000){
                    if(hz % 1000 == 0) snprintf(rate, sizeof rate, "%dkHz", hz / 1000);
                    else snprintf(rate, sizeof rate, "%d.%dkHz", hz / 1000, (hz % 1000) / 100);
                }
            }
            char *fmt = strstr(line, "format: ");
            if(fmt){
                if(strstr(fmt, "S32")) snprintf(bits, sizeof bits, "32bit");
                else if(strstr(fmt, "S24")) snprintf(bits, sizeof bits, "24bit");
                else if(strstr(fmt, "S16")) snprintf(bits, sizeof bits, "16bit");
                else if(strstr(fmt, "DSD")) snprintf(bits, sizeof bits, "DSD");
            }
        }
        fclose(f);
    }

    if(out_fmt && fmt_sz > 0){
        if(rate[0]){
            if(bits[0]) snprintf(out_fmt, fmt_sz, "PCM • %s • %s", rate, bits);
            else snprintf(out_fmt, fmt_sz, "PCM • %s", rate);
        } else {
            snprintf(out_fmt, fmt_sz, "PCM • Idle");
        }
    }
}

static int g_bt_cached_vol = 40;

void bt_rx_sync_volume(int vol){
    if(vol < 0){
        track_state_t tst;
        memset(&tst, 0, sizeof tst);
        ipc_get_state(&tst);
        vol = tst.volume;
    }
    if(vol < 0) vol = 0;
    if(vol > 120) vol = 120;
    g_bt_cached_vol = vol;
    /* Internal CS43131 uses hardware MCU analog volume via ui_set_volume */
}

/* UI poll timer: updates LVGL labels and button icons on main thread */
static void modelock_poll_cb(lv_timer_t *t){
    (void)t;
    if(g_lock_mode == 1){
        /* USB DAC Mode poll */
        int connected = 0;
        char fmt[64] = {0};
        uac_query_status(&connected, fmt, sizeof fmt);

        if(g_dac_title){
            lv_label_set_text(g_dac_title, connected ? "PC / Mac Connected" : "USB DAC Idle");
        }

        if(g_dac_sub){
            if(connected){
                lv_label_set_text(g_dac_sub, "Playing audio from PC / Mac.\nHardware DAC volume active.");
            } else {
                lv_label_set_text(g_dac_sub, "Connect to computer with USB-C cable.\nSelect 'SNOWSKY DISC' as output.");
            }
        }

        if(g_dac_badge && g_dac_badge_lbl){
            if(connected && fmt[0]){
                lv_label_set_text(g_dac_badge_lbl, fmt);
                lv_obj_remove_flag(g_dac_badge, LV_OBJ_FLAG_HIDDEN);
                lv_obj_align(g_dac_badge, LV_ALIGN_TOP_MID, 0, 246);
            } else {
                lv_obj_add_flag(g_dac_badge, LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }
    if(g_lock_mode != 2) return;

    bt_rx_state_t st;
    pthread_mutex_lock(&g_bt_mu);
    st = g_bt_state;
    pthread_mutex_unlock(&g_bt_mu);

    if(g_bt_track_title){
        if(st.connected && st.title[0]){
            lv_label_set_text(g_bt_track_title, st.title);
        } else if(st.connected && st.is_playing){
            lv_label_set_text(g_bt_track_title, "Bluetooth Audio Playing");
        } else if(st.connected){
            lv_label_set_text(g_bt_track_title, st.dev_name[0] ? st.dev_name : "Connected Device");
        } else {
            lv_label_set_text(g_bt_track_title, "Ready to Connect");
        }
    }

    if(g_bt_track_sub){
        if(st.connected && (st.artist[0] || st.album[0])){
            char sub[256];
            if(st.artist[0] && st.album[0]) snprintf(sub, sizeof sub, "%s • %s", st.artist, st.album);
            else snprintf(sub, sizeof sub, "%s", st.artist[0] ? st.artist : st.album);
            lv_label_set_text(g_bt_track_sub, sub);
        } else if(st.connected && (st.is_playing || st.title[0])){
            lv_label_set_text(g_bt_track_sub, st.dev_name[0] ? st.dev_name : "Bluetooth Audio");
        } else if(st.connected){
            lv_label_set_text(g_bt_track_sub, "Play audio from your phone");
        } else {
            char sub[128];
            const char *bname = st.local_name[0] ? st.local_name : "SNOWSKY DISC";
            snprintf(sub, sizeof sub, "Select '%s' on phone", bname);
            lv_label_set_text(g_bt_track_sub, sub);
        }
    }

    if(g_bt_codec_lbl){
        if(st.connected && st.codec[0]){
            lv_label_set_text(g_bt_codec_lbl, st.codec);
            if(g_bt_codec_badge){
                lv_obj_remove_flag(g_bt_codec_badge, LV_OBJ_FLAG_HIDDEN);
                lv_obj_align(g_bt_codec_badge, LV_ALIGN_TOP_MID, 0, 222);
            }
        } else if(st.connected){
            lv_label_set_text(g_bt_codec_lbl, "BT Audio");
            if(g_bt_codec_badge){
                lv_obj_remove_flag(g_bt_codec_badge, LV_OBJ_FLAG_HIDDEN);
                lv_obj_align(g_bt_codec_badge, LV_ALIGN_TOP_MID, 0, 222);
            }
        } else {
            if(g_bt_codec_badge) lv_obj_add_flag(g_bt_codec_badge, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if(g_lbl_pp){
        lv_label_set_text(g_lbl_pp, st.is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        if(theme_trait(THEME_TRAIT_NP_BIG_PLAY)){
            lv_obj_set_style_text_color(g_lbl_pp, TC(ON_ACCENT), 0);
        } else {
            lv_obj_set_style_text_color(g_lbl_pp, ui_current_accent(), 0);
        }
    }

    bt_disc_spin(st.connected && st.is_playing);
}

/* Event handler for Transport buttons */
static void transport_btn_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char *action = (const char *)lv_event_get_user_data(e);
    if(!action) return;
    if(!strcmp(action, "pp"))   bt_rx_play_pause();
    else if(!strcmp(action, "next")) bt_rx_next();
    else if(!strcmp(action, "prev")) bt_rx_prev();
}

static void exit_modal_close(void){
    if(g_exit_modal){
        lv_obj_delete(g_exit_modal);
        g_exit_modal = NULL;
    }
}

static void exit_modal_cancel_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    exit_modal_close();
}

static void exit_modal_confirm_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    exit_modal_close();
    if(ui_set_source_mode(0) == 0){
        modelock_close();
        screen_show(SCR_HOME);
    } else if(ui_sd_is_held()){
        modelock_close();
        screen_show(SCR_HOME);
    }
}

void modelock_prompt_exit(void){
    if(g_exit_modal) return; /* Already showing */

    g_exit_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_exit_modal);
    lv_obj_set_size(g_exit_modal, 360, 360);
    lv_obj_center(g_exit_modal);
    lv_obj_set_style_bg_color(g_exit_modal, TC(SCRIM), 0);
    lv_obj_set_style_bg_opa(g_exit_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(g_exit_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_exit_modal, LV_OBJ_FLAG_CLICKABLE);
    ui_on(g_exit_modal, exit_modal_cancel_cb, LV_EVENT_CLICKED, NULL, "modelock.modal_scrim", UI_CORE);

    lv_obj_t *card = lv_obj_create(g_exit_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 280, 164);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_color(card, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, TC(BORDER), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    const char *title_txt = "Exit Mode?";
    if(g_lock_mode == 1) title_txt = "Exit USB DAC?";
    else if(g_lock_mode == 2) title_txt = "Exit Bluetooth Receiver?";
    else if(g_lock_mode == 3) title_txt = "Exit USB Storage?";
    lv_obj_set_style_text_font(t, TF(UI_16), 0);
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);
    theme_case_text(t, title_txt);

    lv_obj_t *sub = lv_label_create(card);
    lv_label_set_text(sub, "Return to local music playback?");
    lv_obj_set_style_text_font(sub, TF(USER_14), 0);
    lv_obj_set_style_text_color(sub, TC(TEXT_MUTED), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 52);

    /* Cancel Pill */
    lv_obj_t *btn_cancel = lv_button_create(card);
    lv_obj_remove_style_all(btn_cancel);
    lv_obj_set_size(btn_cancel, 108, 42);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT, 20, -18);
    lv_obj_set_ext_click_area(btn_cancel, 4);
    lv_obj_set_style_radius(btn_cancel, 12, 0);
    lv_obj_set_style_bg_color(btn_cancel, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(btn_cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn_cancel, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(btn_cancel, exit_modal_cancel_cb, LV_EVENT_CLICKED, NULL, "modelock.cancel", UI_CORE);

    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_center(lbl_cancel);
    lv_obj_set_style_text_font(lbl_cancel, TF(UI_16), 0);
    lv_obj_set_style_text_color(lbl_cancel, TC(TEXT_SECONDARY), 0);

    /* Exit Pill */
    lv_obj_t *btn_exit = lv_button_create(card);
    lv_obj_remove_style_all(btn_exit);
    lv_obj_set_size(btn_exit, 108, 42);
    lv_obj_align(btn_exit, LV_ALIGN_BOTTOM_RIGHT, -20, -18);
    lv_obj_set_ext_click_area(btn_exit, 4);
    lv_obj_set_style_radius(btn_exit, 12, 0);
    lv_obj_set_style_bg_color(btn_exit, ui_current_accent(), 0);
    lv_obj_set_style_bg_opa(btn_exit, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn_exit, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(btn_exit, exit_modal_confirm_cb, LV_EVENT_CLICKED, NULL, "modelock.exit", UI_CORE);

    lv_obj_t *lbl_exit = lv_label_create(btn_exit);
    lv_label_set_text(lbl_exit, "Exit");
    lv_obj_center(lbl_exit);
    lv_obj_set_style_text_font(lbl_exit, TF(UI_16), 0);
    lv_obj_set_style_text_color(lbl_exit, TC(ON_ACCENT), 0);
}

static void modelock_back_btn_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    modelock_prompt_exit();
}

void modelock_create(lv_obj_t *root){
    g_lock_root = root;
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* 1. Header (Standard diskOS theme kit header + back navigation) */
    g_hdr_title = ui_header_cb(root, "Bluetooth", modelock_back_btn_cb);

    /* 2. Container for Bluetooth Receiving (Mode 2) */
    g_cont_bt = lv_obj_create(root);
    lv_obj_remove_style_all(g_cont_bt);
    lv_obj_set_pos(g_cont_bt, 0, 0);
    lv_obj_set_size(g_cont_bt, 360, 360);
    lv_obj_clear_flag(g_cont_bt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_cont_bt, LV_OBJ_FLAG_SCROLLABLE);

    /* Vinyl disc visual */
    g_bt_disc = lv_obj_create(g_cont_bt);
    kit_keep(g_bt_disc);
    lv_obj_remove_style_all(g_bt_disc);
    lv_obj_set_size(g_bt_disc, 84, 84);
    lv_obj_set_pos(g_bt_disc, 138, 70);
    lv_obj_set_style_radius(g_bt_disc, 42, 0);
    lv_obj_set_style_bg_color(g_bt_disc, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(g_bt_disc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_bt_disc, 2, 0);
    lv_obj_set_style_border_color(g_bt_disc, TC(BORDER), 0);
    lv_obj_set_style_transform_pivot_x(g_bt_disc, 42, 0);
    lv_obj_set_style_transform_pivot_y(g_bt_disc, 42, 0);

    /* Grooves inside vinyl disc */
    lv_obj_t *ring1 = lv_obj_create(g_bt_disc);
    kit_keep(ring1);
    lv_obj_remove_style_all(ring1);
    lv_obj_set_size(ring1, 64, 64);
    lv_obj_center(ring1);
    lv_obj_set_style_radius(ring1, 32, 0);
    lv_obj_set_style_border_width(ring1, 1, 0);
    lv_obj_set_style_border_color(ring1, TC(BORDER_STRONG), 0);

    lv_obj_t *ring2 = lv_obj_create(g_bt_disc);
    kit_keep(ring2);
    lv_obj_remove_style_all(ring2);
    lv_obj_set_size(ring2, 46, 46);
    lv_obj_center(ring2);
    lv_obj_set_style_radius(ring2, 23, 0);
    lv_obj_set_style_border_width(ring2, 1, 0);
    lv_obj_set_style_border_color(ring2, TC(BORDER), 0);

    /* Horizontal lines across the vinyl disc */
    static const struct { int y; int w; } m_hlines[] = {
        { -24, 46 },
        { -12, 64 },
        {   0, 76 },
        {  12, 64 },
        {  24, 46 }
    };
    for(size_t i = 0; i < sizeof(m_hlines)/sizeof(m_hlines[0]); i++){
        lv_obj_t *hl = lv_obj_create(g_bt_disc);
        kit_keep(hl);
        lv_obj_remove_style_all(hl);
        lv_obj_set_size(hl, m_hlines[i].w, (m_hlines[i].y == 0) ? 2 : 1);
        lv_obj_align(hl, LV_ALIGN_CENTER, 0, m_hlines[i].y);
        lv_obj_set_style_bg_color(hl, (m_hlines[i].y == 0) ? TC(BORDER_STRONG) : TC(BORDER), 0);
        lv_obj_set_style_bg_opa(hl, LV_OPA_COVER, 0);
        lv_obj_clear_flag(hl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Center spindle label */
    g_bt_spindle = lv_obj_create(g_bt_disc);
    kit_keep(g_bt_spindle);
    lv_obj_remove_style_all(g_bt_spindle);
    lv_obj_set_size(g_bt_spindle, 28, 28);
    lv_obj_center(g_bt_spindle);
    lv_obj_set_style_radius(g_bt_spindle, 14, 0);
    lv_obj_set_style_bg_color(g_bt_spindle, ui_current_accent(), 0);
    lv_obj_set_style_bg_opa(g_bt_spindle, LV_OPA_COVER, 0);

    lv_obj_t *bt_icon = lv_label_create(g_bt_spindle);
    lv_label_set_text(bt_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_center(bt_icon);
    lv_obj_set_style_text_color(bt_icon, TC(ON_ACCENT), 0);
    lv_obj_set_style_text_font(bt_icon, TF(UI_14), 0);

    /* Track Title */
    g_bt_track_title = lv_label_create(g_cont_bt);
    lv_obj_set_pos(g_bt_track_title, 20, 166);
    lv_obj_set_width(g_bt_track_title, 320);
    lv_obj_set_height(g_bt_track_title, LV_SIZE_CONTENT);
    lv_obj_clear_flag(g_bt_track_title, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(g_bt_track_title, "Ready to Connect");
    lv_label_set_long_mode(g_bt_track_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(g_bt_track_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_bt_track_title, TF(USER_20), 0);
    lv_obj_set_style_text_color(g_bt_track_title, TC(TEXT_PRIMARY), 0);

    /* Track Sub (Artist / Album) */
    g_bt_track_sub = lv_label_create(g_cont_bt);
    lv_obj_set_pos(g_bt_track_sub, 25, 194);
    lv_obj_set_width(g_bt_track_sub, 310);
    lv_obj_set_height(g_bt_track_sub, LV_SIZE_CONTENT);
    lv_obj_clear_flag(g_bt_track_sub, LV_OBJ_FLAG_SCROLLABLE);
    char init_bname[64];
    dn_bt_name_copy(init_bname, sizeof init_bname);
    char init_btsub[128];
    snprintf(init_btsub, sizeof init_btsub, "Select '%s' on phone", init_bname);
    lv_label_set_text(g_bt_track_sub, init_btsub);
    lv_label_set_long_mode(g_bt_track_sub, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(g_bt_track_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_bt_track_sub, TF(USER_14), 0);
    lv_obj_set_style_text_color(g_bt_track_sub, TC(TEXT_MUTED), 0);

    /* Codec pill badge */
    g_bt_codec_badge = lv_obj_create(g_cont_bt);
    lv_obj_remove_style_all(g_bt_codec_badge);
    lv_obj_set_size(g_bt_codec_badge, LV_SIZE_CONTENT, 22);
    lv_obj_set_style_min_width(g_bt_codec_badge, 80, 0);
    lv_obj_set_style_pad_hor(g_bt_codec_badge, 12, 0);
    lv_obj_set_style_pad_ver(g_bt_codec_badge, 2, 0);
    lv_obj_align(g_bt_codec_badge, LV_ALIGN_TOP_MID, 0, 222);
    lv_obj_set_style_radius(g_bt_codec_badge, 11, 0);
    lv_obj_set_style_bg_color(g_bt_codec_badge, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(g_bt_codec_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_bt_codec_badge, 1, 0);
    lv_obj_set_style_border_color(g_bt_codec_badge, ui_current_accent(), 0);
    lv_obj_clear_flag(g_bt_codec_badge, LV_OBJ_FLAG_SCROLLABLE);

    g_bt_codec_lbl = lv_label_create(g_bt_codec_badge);
    lv_label_set_text(g_bt_codec_lbl, "Bluetooth Audio");
    lv_obj_center(g_bt_codec_lbl);
    lv_obj_set_style_text_font(g_bt_codec_lbl, TF(USER_14), 0);
    lv_obj_set_style_text_color(g_bt_codec_lbl, ui_current_accent(), 0);

    /* Transport Controls Row */
    lv_obj_t *ctrl_row = lv_obj_create(g_cont_bt);
    lv_obj_remove_style_all(ctrl_row);
    lv_obj_set_size(ctrl_row, 240, 60);
    lv_obj_align(ctrl_row, LV_ALIGN_TOP_MID, 0, 256);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl_row, 22, 0);

    /* Prev button */
    g_btn_prev = lv_button_create(ctrl_row);
    lv_obj_remove_style_all(g_btn_prev);
    lv_obj_set_size(g_btn_prev, 46, 46);
    lv_obj_set_ext_click_area(g_btn_prev, 4);
    lv_obj_set_style_radius(g_btn_prev, 23, 0);
    lv_obj_set_style_bg_color(g_btn_prev, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(g_btn_prev, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g_btn_prev, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(g_btn_prev, transport_btn_cb, LV_EVENT_CLICKED, (void*)"prev", "modelock.prev", UI_CORE);
    lv_obj_t *lbl_prev = lv_label_create(g_btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
    lv_obj_center(lbl_prev);
    lv_obj_set_style_text_color(lbl_prev, TC(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(lbl_prev, TF(UI_20), 0);

    /* Play/Pause button */
    g_btn_pp = lv_button_create(ctrl_row);
    lv_obj_remove_style_all(g_btn_pp);
    lv_obj_set_size(g_btn_pp, 56, 56);
    lv_obj_set_ext_click_area(g_btn_pp, 4);
    lv_obj_set_style_radius(g_btn_pp, 28, 0);
    lv_obj_set_style_bg_opa(g_btn_pp, LV_OPA_COVER, 0);
    if(theme_trait(THEME_TRAIT_NP_BIG_PLAY)){
        lv_obj_set_style_bg_color(g_btn_pp, ui_current_accent(), 0);
        lv_obj_set_style_border_width(g_btn_pp, 0, 0);
    } else {
        lv_obj_set_style_bg_color(g_btn_pp, TC(SURFACE_RAISED), 0);
        lv_obj_set_style_border_width(g_btn_pp, 2, 0);
        lv_obj_set_style_border_color(g_btn_pp, ui_current_accent(), 0);
    }
    lv_obj_set_style_bg_color(g_btn_pp, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(g_btn_pp, transport_btn_cb, LV_EVENT_CLICKED, (void*)"pp", "modelock.pp", UI_CORE);
    g_lbl_pp = lv_label_create(g_btn_pp);
    lv_label_set_text(g_lbl_pp, LV_SYMBOL_PLAY);
    lv_obj_center(g_lbl_pp);
    lv_obj_set_style_text_color(g_lbl_pp, theme_trait(THEME_TRAIT_NP_BIG_PLAY) ? TC(ON_ACCENT) : ui_current_accent(), 0);
    lv_obj_set_style_text_font(g_lbl_pp, TF(UI_24), 0);

    /* Next button */
    g_btn_next = lv_button_create(ctrl_row);
    lv_obj_remove_style_all(g_btn_next);
    lv_obj_set_size(g_btn_next, 46, 46);
    lv_obj_set_ext_click_area(g_btn_next, 4);
    lv_obj_set_style_radius(g_btn_next, 23, 0);
    lv_obj_set_style_bg_color(g_btn_next, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(g_btn_next, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g_btn_next, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(g_btn_next, transport_btn_cb, LV_EVENT_CLICKED, (void*)"next", "modelock.next", UI_CORE);
    lv_obj_t *lbl_next = lv_label_create(g_btn_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_NEXT);
    lv_obj_center(lbl_next);
    lv_obj_set_style_text_color(lbl_next, TC(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(lbl_next, TF(UI_20), 0);

    /* 3. Container for USB DAC (Mode 1) */
    g_cont_dac = lv_obj_create(root);
    lv_obj_remove_style_all(g_cont_dac);
    lv_obj_set_pos(g_cont_dac, 0, 0);
    lv_obj_set_size(g_cont_dac, 360, 360);
    lv_obj_clear_flag(g_cont_dac, LV_OBJ_FLAG_SCROLLABLE);

    g_dac_circle = lv_obj_create(g_cont_dac);
    kit_keep(g_dac_circle);
    lv_obj_remove_style_all(g_dac_circle);
    lv_obj_set_size(g_dac_circle, 84, 84);
    lv_obj_set_pos(g_dac_circle, 138, 70);
    lv_obj_set_style_radius(g_dac_circle, 42, 0);
    lv_obj_set_style_bg_color(g_dac_circle, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(g_dac_circle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_dac_circle, 2, 0);
    lv_obj_set_style_border_color(g_dac_circle, ui_current_accent(), 0);

    g_dac_icon = lv_label_create(g_dac_circle);
    lv_label_set_text(g_dac_icon, LV_SYMBOL_AUDIO);
    lv_obj_center(g_dac_icon);
    lv_obj_set_style_text_font(g_dac_icon, TF(UI_28), 0);
    lv_obj_set_style_text_color(g_dac_icon, ui_current_accent(), 0);

    g_dac_title = lv_label_create(g_cont_dac);
    lv_obj_set_pos(g_dac_title, 20, 166);
    lv_obj_set_width(g_dac_title, 320);
    lv_obj_set_height(g_dac_title, LV_SIZE_CONTENT);
    lv_label_set_text(g_dac_title, "Waiting for USB");
    lv_obj_set_style_text_align(g_dac_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_dac_title, TF(USER_18), 0);
    lv_obj_set_style_text_color(g_dac_title, TC(TEXT_PRIMARY), 0);

    g_dac_sub = lv_label_create(g_cont_dac);
    lv_obj_set_pos(g_dac_sub, 20, 196);
    lv_obj_set_size(g_dac_sub, 320, 44);
    lv_label_set_text(g_dac_sub, "Connect to computer with USB-C cable\nSelect 'SNOWSKY DISC' as output");
    lv_obj_set_style_text_align(g_dac_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_dac_sub, TF(USER_14), 0);
    lv_obj_set_style_text_color(g_dac_sub, TC(TEXT_MUTED), 0);

    /* Sample Rate / Codec badge */
    g_dac_badge = lv_obj_create(g_cont_dac);
    lv_obj_remove_style_all(g_dac_badge);
    lv_obj_set_size(g_dac_badge, LV_SIZE_CONTENT, 22);
    lv_obj_align(g_dac_badge, LV_ALIGN_TOP_MID, 0, 246);
    lv_obj_set_style_radius(g_dac_badge, 11, 0);
    lv_obj_set_style_bg_color(g_dac_badge, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(g_dac_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_dac_badge, 1, 0);
    lv_obj_set_style_border_color(g_dac_badge, ui_current_accent(), 0);
    lv_obj_set_style_pad_hor(g_dac_badge, 12, 0);
    lv_obj_clear_flag(g_dac_badge, LV_OBJ_FLAG_SCROLLABLE);

    g_dac_badge_lbl = lv_label_create(g_dac_badge);
    lv_label_set_text(g_dac_badge_lbl, "PCM • Idle");
    lv_obj_center(g_dac_badge_lbl);
    lv_obj_set_style_text_font(g_dac_badge_lbl, TF(USER_14), 0);
    lv_obj_set_style_text_color(g_dac_badge_lbl, ui_current_accent(), 0);

    /* 4. Container for USB Storage (Mode 3) */
    g_cont_storage = lv_obj_create(root);
    lv_obj_remove_style_all(g_cont_storage);
    lv_obj_set_pos(g_cont_storage, 0, 0);
    lv_obj_set_size(g_cont_storage, 360, 360);
    lv_obj_clear_flag(g_cont_storage, LV_OBJ_FLAG_SCROLLABLE);

    g_stor_circle = lv_obj_create(g_cont_storage);
    kit_keep(g_stor_circle);
    lv_obj_remove_style_all(g_stor_circle);
    lv_obj_set_size(g_stor_circle, 84, 84);
    lv_obj_set_pos(g_stor_circle, 138, 70);
    lv_obj_set_style_radius(g_stor_circle, 42, 0);
    lv_obj_set_style_bg_color(g_stor_circle, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(g_stor_circle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_stor_circle, 2, 0);
    lv_obj_set_style_border_color(g_stor_circle, ui_current_accent(), 0);

    g_stor_icon = lv_label_create(g_stor_circle);
    lv_label_set_text(g_stor_icon, LV_SYMBOL_DRIVE);
    lv_obj_center(g_stor_icon);
    lv_obj_set_style_text_font(g_stor_icon, TF(UI_28), 0);
    lv_obj_set_style_text_color(g_stor_icon, ui_current_accent(), 0);

    g_stor_title = lv_label_create(g_cont_storage);
    lv_obj_set_pos(g_stor_title, 20, 166);
    lv_obj_set_width(g_stor_title, 320);
    lv_obj_set_height(g_stor_title, LV_SIZE_CONTENT);
    lv_label_set_text(g_stor_title, "MicroSD Card Exported");
    lv_obj_set_style_text_align(g_stor_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_stor_title, TF(USER_18), 0);
    lv_obj_set_style_text_color(g_stor_title, TC(TEXT_PRIMARY), 0);

    g_stor_sub = lv_label_create(g_cont_storage);
    lv_obj_set_pos(g_stor_sub, 20, 196);
    lv_obj_set_size(g_stor_sub, 320, 44);
    lv_label_set_text(g_stor_sub, "Card is mounted by computer\nPlease safely eject on PC before exit");
    lv_obj_set_style_text_align(g_stor_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_stor_sub, TF(USER_14), 0);
    lv_obj_set_style_text_color(g_stor_sub, TC(TEXT_MUTED), 0);

    /* Storage badge */
    g_stor_badge = lv_obj_create(g_cont_storage);
    lv_obj_remove_style_all(g_stor_badge);
    lv_obj_set_size(g_stor_badge, LV_SIZE_CONTENT, 22);
    lv_obj_align(g_stor_badge, LV_ALIGN_TOP_MID, 0, 246);
    lv_obj_set_style_radius(g_stor_badge, 11, 0);
    lv_obj_set_style_bg_color(g_stor_badge, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(g_stor_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_stor_badge, 1, 0);
    lv_obj_set_style_border_color(g_stor_badge, ui_current_accent(), 0);
    lv_obj_set_style_pad_hor(g_stor_badge, 12, 0);
    lv_obj_clear_flag(g_stor_badge, LV_OBJ_FLAG_SCROLLABLE);

    g_stor_badge_lbl = lv_label_create(g_stor_badge);
    lv_label_set_text(g_stor_badge_lbl, "USB Mass Storage");
    lv_obj_center(g_stor_badge_lbl);
    lv_obj_set_style_text_font(g_stor_badge_lbl, TF(USER_14), 0);
    lv_obj_set_style_text_color(g_stor_badge_lbl, ui_current_accent(), 0);

    /* Subtle exit hint at bottom */
    lv_obj_t *hint = lv_label_create(root);
    lv_label_set_text(hint, LV_SYMBOL_LEFT " Swipe right to exit");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_text_font(hint, TF(UI_12), 0);
    lv_obj_set_style_text_color(hint, TC(TEXT_HINT), 0);
}

void modelock_open(int mode){
    if(mode != 1 && mode != 2 && mode != 3) return;
    if(g_lock_mode != 0 && g_lock_mode != mode){
        modelock_close();
    }
    g_lock_mode = mode;

    /* Hide all sub-containers first */
    if(g_cont_bt)      lv_obj_add_flag(g_cont_bt, LV_OBJ_FLAG_HIDDEN);
    if(g_cont_dac)     lv_obj_add_flag(g_cont_dac, LV_OBJ_FLAG_HIDDEN);
    if(g_cont_storage) lv_obj_add_flag(g_cont_storage, LV_OBJ_FLAG_HIDDEN);

    if(mode == 1){
        /* USB DAC */
        if(g_hdr_title)  theme_title_text(g_hdr_title, "USB DAC");
        if(g_cont_dac)   lv_obj_remove_flag(g_cont_dac, LV_OBJ_FLAG_HIDDEN);
        bt_disc_spin(0);
        if(!g_poll_timer){
            g_poll_timer = lv_timer_create(modelock_poll_cb, 300, NULL);
        }
    } else if(mode == 2){
        /* Bluetooth Receiving */
        if(g_hdr_title)  theme_title_text(g_hdr_title, "Bluetooth");
        if(g_cont_bt)    lv_obj_remove_flag(g_cont_bt, LV_OBJ_FLAG_HIDDEN);

        /* Start background worker thread if not running */
        if(!g_worker_run){
            g_worker_run = 1;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 256 * 1024);
            pthread_create(&g_worker_th, &attr, bt_rx_worker, NULL);
            pthread_attr_destroy(&attr);
        }
        if(!g_poll_timer){
            g_poll_timer = lv_timer_create(modelock_poll_cb, 300, NULL);
        }
    } else if(mode == 3){
        /* USB Storage */
        if(g_hdr_title)  theme_title_text(g_hdr_title, "USB Storage");
        if(g_cont_storage) lv_obj_remove_flag(g_cont_storage, LV_OBJ_FLAG_HIDDEN);
        bt_disc_spin(0);
    }

    /* Update colors to current accent */
    lv_color_t acc = ui_current_accent();
    if(g_bt_spindle)     lv_obj_set_style_bg_color(g_bt_spindle, acc, 0);
    if(g_bt_codec_badge) lv_obj_set_style_border_color(g_bt_codec_badge, acc, 0);
    if(g_bt_codec_lbl)   lv_obj_set_style_text_color(g_bt_codec_lbl, acc, 0);
    if(g_btn_pp){
        if(theme_trait(THEME_TRAIT_NP_BIG_PLAY)){
            lv_obj_set_style_bg_color(g_btn_pp, acc, 0);
            lv_obj_set_style_border_width(g_btn_pp, 0, 0);
            if(g_lbl_pp) lv_obj_set_style_text_color(g_lbl_pp, TC(ON_ACCENT), 0);
        } else {
            lv_obj_set_style_bg_color(g_btn_pp, TC(SURFACE_RAISED), 0);
            lv_obj_set_style_border_width(g_btn_pp, 2, 0);
            lv_obj_set_style_border_color(g_btn_pp, acc, 0);
            if(g_lbl_pp) lv_obj_set_style_text_color(g_lbl_pp, acc, 0);
        }
    }
    if(g_dac_circle)     lv_obj_set_style_border_color(g_dac_circle, acc, 0);
    if(g_dac_icon)       lv_obj_set_style_text_color(g_dac_icon, acc, 0);
    if(g_dac_badge)      lv_obj_set_style_border_color(g_dac_badge, acc, 0);
    if(g_dac_badge_lbl)  lv_obj_set_style_text_color(g_dac_badge_lbl, acc, 0);
    if(g_stor_circle)    lv_obj_set_style_border_color(g_stor_circle, acc, 0);
    if(g_stor_icon)      lv_obj_set_style_text_color(g_stor_icon, acc, 0);
    if(g_stor_badge)     lv_obj_set_style_border_color(g_stor_badge, acc, 0);
    if(g_stor_badge_lbl) lv_obj_set_style_text_color(g_stor_badge_lbl, acc, 0);

    screen_show(SCR_MODELOCK);
}

void modelock_close(void){
    int prev_mode = g_lock_mode;
    g_lock_mode = 0;
    exit_modal_close();
    bt_disc_spin(0);

    if(g_poll_timer){
        lv_timer_del(g_poll_timer);
        g_poll_timer = NULL;
    }

    if(prev_mode == 2){
        char mac[20] = {0};
        char dbus_mac[32] = {0};
        pthread_mutex_lock(&g_bt_mu);
        if(g_bt_state.connected && g_bt_state.mac[0]){
            snprintf(mac, sizeof mac, "%s", g_bt_state.mac);
            snprintf(dbus_mac, sizeof dbus_mac, "%s", g_bt_state.dbus_mac);
        }
        memset(&g_bt_state, 0, sizeof(g_bt_state));
        pthread_mutex_unlock(&g_bt_mu);

        char tear_down_cmd[512];
        if(mac[0]){
            snprintf(tear_down_cmd, sizeof tear_down_cmd,
                "( dbus-send --system --dest=org.bluez /org/bluez/hci0/dev_%s org.bluez.Device1.Disconnect 2>/dev/null; "
                "  bluetoothctl disconnect %s 2>/dev/null; "
                "  bluetoothctl discoverable off 2>/dev/null; "
                "  bluetoothctl pairable off 2>/dev/null; "
                "  %s ) >/dev/null 2>&1 &",
                dbus_mac, mac,
                (cfg_get_int("bt_on", 0) == 1) ? "true" : "hciconfig hci0 down 2>/dev/null; rfkill block bluetooth 2>/dev/null");
        } else {
            snprintf(tear_down_cmd, sizeof tear_down_cmd,
                "( bluetoothctl disconnect 2>/dev/null; "
                "  bluetoothctl discoverable off 2>/dev/null; "
                "  bluetoothctl pairable off 2>/dev/null; "
                "  %s ) >/dev/null 2>&1 &",
                (cfg_get_int("bt_on", 0) == 1) ? "true" : "hciconfig hci0 down 2>/dev/null; rfkill block bluetooth 2>/dev/null");
        }
        system(tear_down_cmd);

        if(cfg_get_int("bt_on", 0) == 1){
            bt_boot_restore();
        }
    }
}

int modelock_is_active(void){
    return (g_lock_mode != 0);
}

int modelock_get_mode(void){
    return g_lock_mode;
}
