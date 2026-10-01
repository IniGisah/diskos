/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "modelock.h"
#include "screens.h"
#include "anim.h"
#include "ipc.h"
#include "config.h"
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

/* State of mode lockdown */
static int g_lock_mode = 0;
static lv_obj_t *g_lock_root = NULL;

/* Header widgets */
static lv_obj_t *g_hdr_title;
static lv_obj_t *g_hdr_status;

/* Mode containers */
static lv_obj_t *g_cont_bt;
static lv_obj_t *g_cont_dac;
static lv_obj_t *g_cont_storage;

/* Bluetooth Receiver view widgets */
static lv_obj_t *g_bt_disc;
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

/* Bluetooth AVRCP state cache (thread-safe) */
typedef struct {
    int connected;
    int is_playing;
    char dev_name[64];
    char mac[20];
    char dbus_mac[32];   /* MAC with colons replaced by underscores */
    char title[128];
    char artist[128];
    char album[128];
    char codec[64];
} bt_rx_state_t;

static bt_rx_state_t g_bt_state;
static pthread_mutex_t g_bt_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_worker_th;
static _Atomic int g_worker_run = 0;
static lv_timer_t *g_poll_timer = NULL;
static int g_disc_spinning = 0;

static void disc_spin_cb(void *var, int32_t val){
    lv_image_set_rotation((lv_obj_t*)var, val % 3600);
}

static void bt_disc_spin(int want){
    if(!g_bt_disc) return;
    if(want == g_disc_spinning) return;
    g_disc_spinning = want;
    if(want){
        int32_t cur = lv_image_get_rotation(g_bt_disc);
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, g_bt_disc);
        lv_anim_set_exec_cb(&a, disc_spin_cb);
        lv_anim_set_values(&a, cur, cur + 3600);
        lv_anim_set_time(&a, 7000);   /* smooth rotating vinyl disc */
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    } else {
        lv_anim_delete(g_bt_disc, disc_spin_cb);
    }
}

/* Helper to execute a quick bounded command without hanging */
static int exec_cmd_capture(const char *cmd, char *out, int maxlen, int timeout_ms){
    out[0] = 0;
    int fds[2];
    if(pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if(pid < 0){ close(fds[0]); close(fds[1]); return -1; }
    if(pid == 0){
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[1]);
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    close(fds[1]);

    int n = 0;
    struct pollfd pf = { fds[0], POLLIN, 0 };
    int r = poll(&pf, 1, timeout_ms);
    if(r > 0){
        n = read(fds[0], out, maxlen - 1);
        if(n < 0) n = 0;
    }
    out[n] = 0;
    close(fds[0]);
    kill(-pid, SIGKILL);
    /* Block wait for child process to be reaped - prevents zombies */
    waitpid(pid, NULL, 0);
    return n;
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

/* Background worker thread: polls Bluetooth connection and AVRCP track info */
static void *bt_rx_worker(void *arg){
    (void)arg;
    char buf[2048];
    while(g_worker_run){
        if(g_lock_mode != 2){
            usleep(500000);
            continue;
        }

        /* 1. Check connected device via hcitool con */
        int n = exec_cmd_capture("hcitool con 2>/dev/null", buf, sizeof buf, 250);
        char mac[20] = {0};
        int found = 0;
        if(n > 0){
            char *line = strstr(buf, "ACL ");
            if(line && strlen(line) >= 21){
                line += 4;
                if(line[2] == ':' && line[5] == ':' && line[8] == ':' && line[11] == ':' && line[14] == ':'){
                    memcpy(mac, line, 17);
                    mac[17] = 0;
                    found = 1;
                }
            }
        }

        if(!found){
            pthread_mutex_lock(&g_bt_mu);
            g_bt_state.connected = 0;
            g_bt_state.is_playing = 0;
            g_bt_state.mac[0] = 0;
            g_bt_state.dbus_mac[0] = 0;
            g_bt_state.title[0] = 0;
            g_bt_state.artist[0] = 0;
            g_bt_state.album[0] = 0;
            g_bt_state.codec[0] = 0;
            pthread_mutex_unlock(&g_bt_mu);
            usleep(1200000);
            continue;
        }

        /* Found connected device */
        char dbus_mac[32];
        for(int i = 0; i < 17; i++) dbus_mac[i] = (mac[i] == ':') ? '_' : mac[i];
        dbus_mac[17] = 0;

        char name[64] = "Connected Device";
        char cmd[256];
        snprintf(cmd, sizeof cmd, "bluetoothctl info %s 2>/dev/null | grep -E '^[ \t]*Name:' | cut -d: -f2-", mac);
        if(exec_cmd_capture(cmd, buf, sizeof buf, 300) > 0){
            char *p = buf; while(*p == ' ' || *p == '\t') p++;
            char *e = p + strlen(p); while(e > p && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ')) *--e = 0;
            if(*p) snprintf(name, sizeof name, "%s", p);
        }

        /* Query player status */
        int is_play = 0;
        snprintf(cmd, sizeof cmd, "dbus-send --system --print-reply --dest=org.bluez /org/bluez/hci0/dev_%s/player0 org.freedesktop.DBus.Properties.Get string:org.bluez.MediaPlayer1 string:Status 2>/dev/null", dbus_mac);
        if(exec_cmd_capture(cmd, buf, sizeof buf, 300) > 0){
            if(strstr(buf, "\"playing\"")) is_play = 1;
        }

        /* Query track metadata */
        char title[128] = {0}, artist[128] = {0}, album[128] = {0};
        snprintf(cmd, sizeof cmd, "dbus-send --system --print-reply --dest=org.bluez /org/bluez/hci0/dev_%s/player0 org.freedesktop.DBus.Properties.Get string:org.bluez.MediaPlayer1 string:Track 2>/dev/null", dbus_mac);
        if(exec_cmd_capture(cmd, buf, sizeof buf, 600) > 0){
            dbus_extract_tag(buf, "Title", title, sizeof title);
            dbus_extract_tag(buf, "Artist", artist, sizeof artist);
            dbus_extract_tag(buf, "Album", album, sizeof album);
        }

        if(found){
            /* Maintain audio router daemon whenever connected */
            if(exec_cmd_capture("pgrep bluealsa-aplay 2>/dev/null", buf, sizeof buf, 150) <= 0){
                system("killall -9 bluealsa-aplay 2>/dev/null; ( sleep 0.4; bluealsa-aplay -D plughw:0,3 >/dev/null 2>&1 ) &");
            }
        }

        /* Query BlueALSA for Codec, Sample Rate, and Bit depth */
        char codec_str[64] = {0};
        snprintf(cmd, sizeof cmd,
            "bluealsa-cli info /org/bluealsa/hci0/dev_%s/a2dpsnk/source 2>/dev/null || "
            "bluealsa-cli info /org/bluealsa/hci0/dev_%s/a2dp-sink/source 2>/dev/null",
            dbus_mac, dbus_mac);
        if(exec_cmd_capture(cmd, buf, sizeof buf, 300) > 0){
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
                    else snprintf(rate, sizeof rate, "%.1fkHz", (double)hz / 1000.0);
                }
            }
            char *f = strstr(buf, "Format:");
            if(f){
                if(strstr(f, "S32")) snprintf(bits, sizeof bits, "24/32bit");
                else if(strstr(f, "S24")) snprintf(bits, sizeof bits, "24bit");
                else if(strstr(f, "S16")) snprintf(bits, sizeof bits, "16bit");
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

        pthread_mutex_lock(&g_bt_mu);
        g_bt_state.connected = 1;
        g_bt_state.is_playing = is_play;
        snprintf(g_bt_state.mac, sizeof g_bt_state.mac, "%s", mac);
        snprintf(g_bt_state.dbus_mac, sizeof g_bt_state.dbus_mac, "%s", dbus_mac);
        snprintf(g_bt_state.dev_name, sizeof g_bt_state.dev_name, "%s", name);
        if(title[0]){
            snprintf(g_bt_state.title, sizeof g_bt_state.title, "%s", title);
            snprintf(g_bt_state.artist, sizeof g_bt_state.artist, "%s", artist);
            snprintf(g_bt_state.album, sizeof g_bt_state.album, "%s", album);
        } else if(!is_play && !g_bt_state.is_playing){
            /* Playback stopped and no track reported */
            g_bt_state.title[0] = 0;
            g_bt_state.artist[0] = 0;
            g_bt_state.album[0] = 0;
        }
        if(codec_str[0]) snprintf(g_bt_state.codec, sizeof g_bt_state.codec, "%s", codec_str);
        pthread_mutex_unlock(&g_bt_mu);

        usleep(1000000);
    }
    return NULL;
}

/* AVRCP commands for Bluetooth receiving mode */
void bt_rx_play_pause(void){
    char dbus_mac[32] = {0};
    int is_playing = 0;
    pthread_mutex_lock(&g_bt_mu);
    if(g_bt_state.connected && g_bt_state.dbus_mac[0]){
        snprintf(dbus_mac, sizeof dbus_mac, "%s", g_bt_state.dbus_mac);
        is_playing = g_bt_state.is_playing;
    }
    pthread_mutex_unlock(&g_bt_mu);
    if(!dbus_mac[0]) return;
    char cmd[256];
    snprintf(cmd, sizeof cmd, "dbus-send --system --print-reply --dest=org.bluez /org/bluez/hci0/dev_%s org.bluez.MediaControl1.%s >/dev/null 2>&1 &",
             dbus_mac, is_playing ? "Pause" : "Play");
    system(cmd);
}

void bt_rx_next(void){
    char dbus_mac[32] = {0};
    pthread_mutex_lock(&g_bt_mu);
    if(g_bt_state.connected && g_bt_state.dbus_mac[0]){
        snprintf(dbus_mac, sizeof dbus_mac, "%s", g_bt_state.dbus_mac);
    }
    pthread_mutex_unlock(&g_bt_mu);
    if(!dbus_mac[0]) return;
    char cmd[256];
    snprintf(cmd, sizeof cmd, "dbus-send --system --print-reply --dest=org.bluez /org/bluez/hci0/dev_%s org.bluez.MediaControl1.Next >/dev/null 2>&1 &", dbus_mac);
    system(cmd);
}

void bt_rx_prev(void){
    char dbus_mac[32] = {0};
    pthread_mutex_lock(&g_bt_mu);
    if(g_bt_state.connected && g_bt_state.dbus_mac[0]){
        snprintf(dbus_mac, sizeof dbus_mac, "%s", g_bt_state.dbus_mac);
    }
    pthread_mutex_unlock(&g_bt_mu);
    if(!dbus_mac[0]) return;
    char cmd[256];
    snprintf(cmd, sizeof cmd, "dbus-send --system --print-reply --dest=org.bluez /org/bluez/hci0/dev_%s org.bluez.MediaControl1.Previous >/dev/null 2>&1 &", dbus_mac);
    system(cmd);
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
    if(g_lock_mode != 2) return;

    bt_rx_state_t st;
    pthread_mutex_lock(&g_bt_mu);
    st = g_bt_state;
    pthread_mutex_unlock(&g_bt_mu);

    if(g_hdr_status){
        if(st.connected && st.dev_name[0]){
            char s[128]; snprintf(s, sizeof s, "Connected: %s", st.dev_name);
            lv_label_set_text(g_hdr_status, s);
            lv_obj_set_style_text_color(g_hdr_status, ui_current_accent(), 0);
        } else {
            lv_label_set_text(g_hdr_status, "Waiting for device connection...");
            lv_obj_set_style_text_color(g_hdr_status, lv_color_hex(0x8E8E93), 0);
        }
    }

    if(g_bt_track_title){
        if(st.connected && st.title[0]){
            lv_label_set_text(g_bt_track_title, st.title);
        } else if(st.connected && st.is_playing){
            lv_label_set_text(g_bt_track_title, "Bluetooth Audio Playing");
        } else if(st.connected){
            lv_label_set_text(g_bt_track_title, "No Media Playing");
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
            lv_label_set_text(g_bt_track_sub, "Select 'SNOWSKY DISC' on phone");
        }
    }

    if(g_bt_codec_lbl){
        if(st.connected && st.codec[0]){
            lv_label_set_text(g_bt_codec_lbl, st.codec);
            if(g_bt_codec_badge) lv_obj_remove_flag(g_bt_codec_badge, LV_OBJ_FLAG_HIDDEN);
        } else if(st.connected){
            lv_label_set_text(g_bt_codec_lbl, "BT Audio");
            if(g_bt_codec_badge) lv_obj_remove_flag(g_bt_codec_badge, LV_OBJ_FLAG_HIDDEN);
        } else {
            if(g_bt_codec_badge) lv_obj_add_flag(g_bt_codec_badge, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if(g_lbl_pp){
        lv_label_set_text(g_lbl_pp, st.is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
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
    ui_set_source_mode(0);
    modelock_close();
    screen_show(SCR_HOME);
}

void modelock_prompt_exit(void){
    if(g_exit_modal) return; /* Already showing */

    g_exit_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_exit_modal);
    lv_obj_set_size(g_exit_modal, 360, 360);
    lv_obj_center(g_exit_modal);
    lv_obj_set_style_bg_color(g_exit_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_exit_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(g_exit_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_exit_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_exit_modal, exit_modal_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(g_exit_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 280, 160);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2C2C2E), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    const char *title_txt = "Exit Mode?";
    if(g_lock_mode == 1) title_txt = "Exit USB DAC?";
    else if(g_lock_mode == 2) title_txt = "Exit Bluetooth Receiver?";
    else if(g_lock_mode == 3) title_txt = "Exit USB Storage?";
    lv_label_set_text(t, title_txt);
    lv_obj_set_style_text_font(t, ui_font_cjk(16), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *sub = lv_label_create(card);
    lv_label_set_text(sub, "Return to local music playback?");
    lv_obj_set_style_text_font(sub, ui_font_cjk(14), 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x8E8E93), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 52);

    /* Cancel Pill */
    lv_obj_t *btn_cancel = lv_button_create(card);
    lv_obj_remove_style_all(btn_cancel);
    lv_obj_set_size(btn_cancel, 108, 42);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT, 20, -18);
    lv_obj_set_style_radius(btn_cancel, 12, 0);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(btn_cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x3A3A3C), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn_cancel, exit_modal_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_center(lbl_cancel);
    lv_obj_set_style_text_font(lbl_cancel, ui_font_cjk(15), 0);
    lv_obj_set_style_text_color(lbl_cancel, lv_color_hex(0xFFFFFF), 0);

    /* Exit Pill */
    lv_obj_t *btn_exit = lv_button_create(card);
    lv_obj_remove_style_all(btn_exit);
    lv_obj_set_size(btn_exit, 108, 42);
    lv_obj_align(btn_exit, LV_ALIGN_BOTTOM_RIGHT, -20, -18);
    lv_obj_set_style_radius(btn_exit, 12, 0);
    lv_obj_set_style_bg_color(btn_exit, ui_current_accent(), 0);
    lv_obj_set_style_bg_opa(btn_exit, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(btn_exit, exit_modal_confirm_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_exit = lv_label_create(btn_exit);
    lv_label_set_text(lbl_exit, "Exit");
    lv_obj_center(lbl_exit);
    lv_obj_set_style_text_font(lbl_exit, ui_font_cjk(15), 0);
    lv_obj_set_style_text_color(lbl_exit, lv_color_hex(0x000000), 0);
}

void modelock_create(lv_obj_t *root){
    g_lock_root = root;
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* 1. Header (Mode title + Status badge) */
    g_hdr_title = lv_label_create(root);
    lv_obj_set_pos(g_hdr_title, 20, 20);
    lv_obj_set_size(g_hdr_title, 320, 28);
    lv_label_set_text(g_hdr_title, "Mode Lockdown");
    lv_obj_set_style_text_align(g_hdr_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_hdr_title, ui_font_cjk(18), 0);
    lv_obj_set_style_text_color(g_hdr_title, lv_color_hex(0xFFFFFF), 0);

    g_hdr_status = lv_label_create(root);
    lv_obj_set_pos(g_hdr_status, 20, 48);
    lv_obj_set_size(g_hdr_status, 320, 22);
    lv_label_set_text(g_hdr_status, "Active");
    lv_obj_set_style_text_align(g_hdr_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_hdr_status, ui_font_cjk(14), 0);
    lv_obj_set_style_text_color(g_hdr_status, lv_color_hex(0x8E8E93), 0);

    /* 2. Container for Bluetooth Receiving (Mode 2) */
    g_cont_bt = lv_obj_create(root);
    lv_obj_remove_style_all(g_cont_bt);
    lv_obj_set_pos(g_cont_bt, 0, 72);
    lv_obj_set_size(g_cont_bt, 360, 224);
    lv_obj_clear_flag(g_cont_bt, LV_OBJ_FLAG_SCROLLABLE);



    /* Vinyl disc visual */
    g_bt_disc = lv_obj_create(g_cont_bt);
    lv_obj_remove_style_all(g_bt_disc);
    lv_obj_set_size(g_bt_disc, 84, 84);
    lv_obj_set_pos(g_bt_disc, 138, 2);
    lv_obj_set_style_radius(g_bt_disc, 42, 0);
    lv_obj_set_style_bg_color(g_bt_disc, lv_color_hex(0x18181A), 0);
    lv_obj_set_style_bg_opa(g_bt_disc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_bt_disc, 2, 0);
    lv_obj_set_style_border_color(g_bt_disc, lv_color_hex(0x2C2C2E), 0);
    lv_image_set_pivot(g_bt_disc, 42, 42);

    /* Grooves inside vinyl disc */
    lv_obj_t *ring1 = lv_obj_create(g_bt_disc);
    lv_obj_remove_style_all(ring1);
    lv_obj_set_size(ring1, 64, 64);
    lv_obj_center(ring1);
    lv_obj_set_style_radius(ring1, 32, 0);
    lv_obj_set_style_border_width(ring1, 1, 0);
    lv_obj_set_style_border_color(ring1, lv_color_hex(0x28282A), 0);

    /* Center spindle label */
    lv_obj_t *spindle = lv_obj_create(g_bt_disc);
    lv_obj_remove_style_all(spindle);
    lv_obj_set_size(spindle, 28, 28);
    lv_obj_center(spindle);
    lv_obj_set_style_radius(spindle, 14, 0);
    lv_obj_set_style_bg_color(spindle, ui_current_accent(), 0);
    lv_obj_set_style_bg_opa(spindle, LV_OPA_COVER, 0);

    lv_obj_t *bt_icon = lv_label_create(spindle);
    lv_label_set_text(bt_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_center(bt_icon);
    lv_obj_set_style_text_color(bt_icon, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(bt_icon, &lv_font_montserrat_14, 0);

    /* Track Title */
    g_bt_track_title = lv_label_create(g_cont_bt);
    lv_obj_set_pos(g_bt_track_title, 20, 88);
    lv_obj_set_width(g_bt_track_title, 320);
    lv_obj_set_height(g_bt_track_title, LV_SIZE_CONTENT);
    lv_obj_clear_flag(g_bt_track_title, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(g_bt_track_title, "Ready to Connect");
    lv_label_set_long_mode(g_bt_track_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(g_bt_track_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_bt_track_title, ui_font_cjk(16), 0);
    lv_obj_set_style_text_color(g_bt_track_title, lv_color_hex(0xFFFFFF), 0);

    /* Track Sub (Artist / Album) */
    g_bt_track_sub = lv_label_create(g_cont_bt);
    lv_obj_set_pos(g_bt_track_sub, 20, 114);
    lv_obj_set_width(g_bt_track_sub, 320);
    lv_obj_set_height(g_bt_track_sub, LV_SIZE_CONTENT);
    lv_obj_clear_flag(g_bt_track_sub, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(g_bt_track_sub, "Select 'SNOWSKY DISC' on phone");
    lv_label_set_long_mode(g_bt_track_sub, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(g_bt_track_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_bt_track_sub, ui_font_cjk(14), 0);
    lv_obj_set_style_text_color(g_bt_track_sub, lv_color_hex(0x8E8E93), 0);

    /* Codec pill badge */
    g_bt_codec_badge = lv_obj_create(g_cont_bt);
    lv_obj_remove_style_all(g_bt_codec_badge);
    lv_obj_set_size(g_bt_codec_badge, 220, 22);
    lv_obj_set_pos(g_bt_codec_badge, 70, 138);
    lv_obj_set_style_radius(g_bt_codec_badge, 11, 0);
    lv_obj_set_style_bg_color(g_bt_codec_badge, lv_color_hex(0x18181A), 0);
    lv_obj_set_style_bg_opa(g_bt_codec_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_bt_codec_badge, 1, 0);
    lv_obj_set_style_border_color(g_bt_codec_badge, ui_current_accent(), 0);
    lv_obj_clear_flag(g_bt_codec_badge, LV_OBJ_FLAG_SCROLLABLE);

    g_bt_codec_lbl = lv_label_create(g_bt_codec_badge);
    lv_label_set_text(g_bt_codec_lbl, "Bluetooth Audio");
    lv_obj_center(g_bt_codec_lbl);
    lv_obj_set_style_text_font(g_bt_codec_lbl, ui_font_cjk(12), 0);
    lv_obj_set_style_text_color(g_bt_codec_lbl, ui_current_accent(), 0);

    /* Transport Controls Row */
    lv_obj_t *ctrl_row = lv_obj_create(g_cont_bt);
    lv_obj_remove_style_all(ctrl_row);
    lv_obj_set_pos(ctrl_row, 60, 164);
    lv_obj_set_size(ctrl_row, 240, 56);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl_row, 20, 0);

    /* Prev button */
    g_btn_prev = lv_button_create(ctrl_row);
    lv_obj_remove_style_all(g_btn_prev);
    lv_obj_set_size(g_btn_prev, 44, 44);
    lv_obj_set_style_radius(g_btn_prev, 22, 0);
    lv_obj_set_style_bg_color(g_btn_prev, lv_color_hex(0x242426), 0);
    lv_obj_set_style_bg_opa(g_btn_prev, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g_btn_prev, lv_color_hex(0x3A3A3C), LV_STATE_PRESSED);
    lv_obj_add_event_cb(g_btn_prev, transport_btn_cb, LV_EVENT_CLICKED, (void*)"prev");
    lv_obj_t *lbl_prev = lv_label_create(g_btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
    lv_obj_center(lbl_prev);
    lv_obj_set_style_text_color(lbl_prev, lv_color_hex(0xFFFFFF), 0);

    /* Play/Pause button */
    g_btn_pp = lv_button_create(ctrl_row);
    lv_obj_remove_style_all(g_btn_pp);
    lv_obj_set_size(g_btn_pp, 52, 52);
    lv_obj_set_style_radius(g_btn_pp, 26, 0);
    lv_obj_set_style_bg_color(g_btn_pp, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(g_btn_pp, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_btn_pp, 2, 0);
    lv_obj_set_style_border_color(g_btn_pp, ui_current_accent(), 0);
    lv_obj_set_style_bg_color(g_btn_pp, lv_color_hex(0x48484A), LV_STATE_PRESSED);
    lv_obj_add_event_cb(g_btn_pp, transport_btn_cb, LV_EVENT_CLICKED, (void*)"pp");
    g_lbl_pp = lv_label_create(g_btn_pp);
    lv_label_set_text(g_lbl_pp, LV_SYMBOL_PLAY);
    lv_obj_center(g_lbl_pp);
    lv_obj_set_style_text_color(g_lbl_pp, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(g_lbl_pp, &lv_font_montserrat_20, 0);

    /* Next button */
    g_btn_next = lv_button_create(ctrl_row);
    lv_obj_remove_style_all(g_btn_next);
    lv_obj_set_size(g_btn_next, 44, 44);
    lv_obj_set_style_radius(g_btn_next, 22, 0);
    lv_obj_set_style_bg_color(g_btn_next, lv_color_hex(0x242426), 0);
    lv_obj_set_style_bg_opa(g_btn_next, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g_btn_next, lv_color_hex(0x3A3A3C), LV_STATE_PRESSED);
    lv_obj_add_event_cb(g_btn_next, transport_btn_cb, LV_EVENT_CLICKED, (void*)"next");
    lv_obj_t *lbl_next = lv_label_create(g_btn_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_NEXT);
    lv_obj_center(lbl_next);
    lv_obj_set_style_text_color(lbl_next, lv_color_hex(0xFFFFFF), 0);

    /* 3. Container for USB DAC (Mode 1) */
    g_cont_dac = lv_obj_create(root);
    lv_obj_remove_style_all(g_cont_dac);
    lv_obj_set_pos(g_cont_dac, 0, 72);
    lv_obj_set_size(g_cont_dac, 360, 220);
    lv_obj_clear_flag(g_cont_dac, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dac_circle = lv_obj_create(g_cont_dac);
    lv_obj_remove_style_all(dac_circle);
    lv_obj_set_size(dac_circle, 84, 84);
    lv_obj_set_pos(dac_circle, 138, 14);
    lv_obj_set_style_radius(dac_circle, 42, 0);
    lv_obj_set_style_bg_color(dac_circle, lv_color_hex(0x18181A), 0);
    lv_obj_set_style_bg_opa(dac_circle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dac_circle, 2, 0);
    lv_obj_set_style_border_color(dac_circle, ui_current_accent(), 0);

    lv_obj_t *dac_icon = lv_label_create(dac_circle);
    lv_label_set_text(dac_icon, LV_SYMBOL_AUDIO);
    lv_obj_center(dac_icon);
    lv_obj_set_style_text_font(dac_icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(dac_icon, ui_current_accent(), 0);

    lv_obj_t *dac_msg1 = lv_label_create(g_cont_dac);
    lv_obj_set_pos(dac_msg1, 20, 116);
    lv_obj_set_size(dac_msg1, 320, 26);
    lv_label_set_text(dac_msg1, "USB Audio Class Active");
    lv_obj_set_style_text_align(dac_msg1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(dac_msg1, ui_font_cjk(16), 0);
    lv_obj_set_style_text_color(dac_msg1, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t *dac_msg2 = lv_label_create(g_cont_dac);
    lv_obj_set_pos(dac_msg2, 20, 148);
    lv_obj_set_size(dac_msg2, 320, 44);
    lv_label_set_text(dac_msg2, "Playing audio from PC / Mac.\nHardware DAC volume active.");
    lv_obj_set_style_text_align(dac_msg2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(dac_msg2, ui_font_cjk(14), 0);
    lv_obj_set_style_text_color(dac_msg2, lv_color_hex(0x8E8E93), 0);

    /* 4. Container for USB Storage (Mode 3) */
    g_cont_storage = lv_obj_create(root);
    lv_obj_remove_style_all(g_cont_storage);
    lv_obj_set_pos(g_cont_storage, 0, 72);
    lv_obj_set_size(g_cont_storage, 360, 220);
    lv_obj_clear_flag(g_cont_storage, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *stor_circle = lv_obj_create(g_cont_storage);
    lv_obj_remove_style_all(stor_circle);
    lv_obj_set_size(stor_circle, 84, 84);
    lv_obj_set_pos(stor_circle, 138, 14);
    lv_obj_set_style_radius(stor_circle, 42, 0);
    lv_obj_set_style_bg_color(stor_circle, lv_color_hex(0x18181A), 0);
    lv_obj_set_style_bg_opa(stor_circle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(stor_circle, 2, 0);
    lv_obj_set_style_border_color(stor_circle, ui_current_accent(), 0);

    lv_obj_t *stor_icon = lv_label_create(stor_circle);
    lv_label_set_text(stor_icon, LV_SYMBOL_DRIVE);
    lv_obj_center(stor_icon);
    lv_obj_set_style_text_font(stor_icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(stor_icon, ui_current_accent(), 0);

    lv_obj_t *stor_msg1 = lv_label_create(g_cont_storage);
    lv_obj_set_pos(stor_msg1, 20, 116);
    lv_obj_set_size(stor_msg1, 320, 26);
    lv_label_set_text(stor_msg1, "MicroSD Card Exported");
    lv_obj_set_style_text_align(stor_msg1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(stor_msg1, ui_font_cjk(16), 0);
    lv_obj_set_style_text_color(stor_msg1, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t *stor_msg2 = lv_label_create(g_cont_storage);
    lv_obj_set_pos(stor_msg2, 20, 148);
    lv_obj_set_size(stor_msg2, 320, 44);
    lv_label_set_text(stor_msg2, "Card is mounted by computer.\nPlease safely eject on PC before exit.");
    lv_obj_set_style_text_align(stor_msg2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(stor_msg2, ui_font_cjk(14), 0);
    lv_obj_set_style_text_color(stor_msg2, lv_color_hex(0x8E8E93), 0);

    /* Subtle exit hint at bottom */
    lv_obj_t *hint = lv_label_create(root);
    lv_label_set_text(hint, LV_SYMBOL_LEFT " Swipe right to exit");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_text_font(hint, ui_font_cjk(12), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555558), 0);
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
        if(g_hdr_title)  lv_label_set_text(g_hdr_title, LV_SYMBOL_AUDIO " USB DAC Mode");
        if(g_hdr_status) lv_label_set_text(g_hdr_status, "Connected to computer");
        if(g_cont_dac)   lv_obj_remove_flag(g_cont_dac, LV_OBJ_FLAG_HIDDEN);
        bt_disc_spin(0);
    } else if(mode == 2){
        /* Bluetooth Receiving */
        if(g_hdr_title)  lv_label_set_text(g_hdr_title, LV_SYMBOL_BLUETOOTH " Bluetooth Receiver");
        if(g_hdr_status) lv_label_set_text(g_hdr_status, "Ready to connect");
        if(g_cont_bt)    lv_obj_remove_flag(g_cont_bt, LV_OBJ_FLAG_HIDDEN);

        /* Ensure Bluetooth hardware is attached, bluetoothd is in sink mode, and bluealsa is in a2dp-sink */
        system(
            "if ! hciconfig hci0 2>/dev/null | grep -q RUNNING; then "
            "  rfkill unblock bluetooth 2>/dev/null; "
            "  brcm_patchram_plus --enable_lpm --enable_hci --no2bytes --tosleep 200000 --baudrate 3000000 "
            "    --patchram /lib/firmware/bt_bcm/BCM4343A1_001.002.009.1026.1055.hcd /dev/ttyS0 >/tmp/patchram.log 2>&1 & "
            "  i=0; while [ \"$i\" -lt 30 ]; do "
            "    hciconfig hci0 up 2>/dev/null; "
            "    hciconfig hci0 2>/dev/null | grep -q RUNNING && break; "
            "    sleep 0.5; i=$((i+1)); "
            "  done; "
            "fi; "
            "if ! ps aux | grep -v grep | grep -q 'bluetoothd.*--mode=sink'; then "
            "  killall -9 bluealsa bluetoothd bt-agent 2>/dev/null; sleep 0.3; "
            "  /usr/project/bluetoothd --noplugin=sap --plugin=a2dp,avrcp --mode=sink >/tmp/btd.log 2>&1 & "
            "  sleep 0.4; "
            "  bluealsa -S --device=hci0 -p a2dp-sink --codec=sbc --codec=aac --codec=ldac --ldac-abr --ldac-quality=standard --initial-volume=100 >/tmp/bluealsa.log 2>&1 & "
            "  sleep 0.4; "
            "fi; "
            "hciconfig hci0 up 2>/dev/null; "
            "hciconfig hci0 piscan 2>/dev/null; "
            "hciconfig hci0 class 0x200414 2>/dev/null; "
            "bluetoothctl power on 2>/dev/null; "
            "bluetoothctl pairable on 2>/dev/null; "
            "bluetoothctl discoverable on 2>/dev/null; "
            "killall -9 bt-agent 2>/dev/null; "
            "bt-agent -c NoInputNoOutput -d 2>/dev/null &"
        );

        /* Start audio routing to internal 3.5mm DAC */
        system("killall -9 bluealsa-aplay 2>/dev/null; ( sleep 0.4; bluealsa-aplay -D plughw:0,3 >/dev/null 2>&1 ) &");

        /* Start background worker thread if not running */
        if(!g_worker_run){
            g_worker_run = 1;
            pthread_create(&g_worker_th, NULL, bt_rx_worker, NULL);
        }
        if(!g_poll_timer){
            g_poll_timer = lv_timer_create(modelock_poll_cb, 300, NULL);
        }
    } else if(mode == 3){
        /* USB Storage */
        if(g_hdr_title)  lv_label_set_text(g_hdr_title, LV_SYMBOL_DRIVE " USB Storage Mode");
        if(g_hdr_status) lv_label_set_text(g_hdr_status, "Card exported to host");
        if(g_cont_storage) lv_obj_remove_flag(g_cont_storage, LV_OBJ_FLAG_HIDDEN);
        bt_disc_spin(0);
    }

    /* Update colors to current accent */
    if(g_btn_pp)   lv_obj_set_style_border_color(g_btn_pp, ui_current_accent(), 0);
    if(g_bt_codec_badge) lv_obj_set_style_border_color(g_bt_codec_badge, ui_current_accent(), 0);
    if(g_bt_codec_lbl)   lv_obj_set_style_text_color(g_bt_codec_lbl, ui_current_accent(), 0);

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
                "  killall -9 bluealsa-aplay bt-agent 2>/dev/null; "
                "  killall -9 bluealsa bluetoothd 2>/dev/null; "
                "  %s ) >/dev/null 2>&1 &",
                dbus_mac, mac,
                (cfg_get_int("bt_on", 0) == 1) ? "true" : "hciconfig hci0 down 2>/dev/null; rfkill block bluetooth 2>/dev/null");
        } else {
            snprintf(tear_down_cmd, sizeof tear_down_cmd,
                "( bluetoothctl disconnect 2>/dev/null; "
                "  bluetoothctl discoverable off 2>/dev/null; "
                "  bluetoothctl pairable off 2>/dev/null; "
                "  killall -9 bluealsa-aplay bt-agent 2>/dev/null; "
                "  killall -9 bluealsa bluetoothd 2>/dev/null; "
                "  %s ) >/dev/null 2>&1 &",
                (cfg_get_int("bt_on", 0) == 1) ? "true" : "hciconfig hci0 down 2>/dev/null; rfkill block bluetooth 2>/dev/null");
        }
        system(tear_down_cmd);

        if(cfg_get_int("bt_on", 0) == 1){
            bt_boot_restore();
        }
    } else {
        system("killall -9 bluealsa-aplay 2>/dev/null &");
    }
}

int modelock_is_active(void){
    return (g_lock_mode != 0);
}

int modelock_get_mode(void){
    return g_lock_mode;
}
