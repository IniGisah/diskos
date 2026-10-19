/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "webui.h"
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define WEBSCRIPT "/usr/data/bin/diskos-web.sh"

static lv_obj_t *g_url_lbl, *g_status_lbl, *g_pw_lbl, *g_btn_lbl, *g_info_lbl, *g_main_btn;
static int g_web_running = 0;


static const char *get_webscript_path(void) {
    if(access("/usr/data/bin/diskos-web.sh", F_OK) == 0) return "/usr/data/bin/diskos-web.sh";
    if(access("/tmp/sdcard/diskos/diskos-web.sh", F_OK) == 0) return "/tmp/sdcard/diskos/diskos-web.sh";
    if(access("/tmp/sdcard/diskos-web/diskos-web.sh", F_OK) == 0) return "/tmp/sdcard/diskos-web/diskos-web.sh";
    if(access("/tmp/sdcard/.diskos/diskos-web.sh", F_OK) == 0) return "/tmp/sdcard/.diskos/diskos-web.sh";
    return NULL;
}

static int is_device_installed(void) {
    return (access("/usr/data/bin/diskos-webd", F_OK) == 0 &&
            access("/usr/data/web/index.html", F_OK) == 0);
}

static int is_sd_package_present(void) {
    if(access("/tmp/sdcard/diskos/diskos-webd", F_OK) == 0) return 1;
    if(access("/tmp/sdcard/diskos-web/diskos-webd", F_OK) == 0) return 1;
    if(access("/tmp/sdcard/.diskos/diskos-webd", F_OK) == 0) return 1;
    return 0;
}

static void escape_json_str(char *dst, const char *src, size_t max) {
    if(!src) { if(max > 0) dst[0] = 0; return; }
    size_t d = 0;
    while(*src && d + 2 < max) {
        if(*src == '"' || *src == '\\') {
            dst[d++] = '\\';
            dst[d++] = *src++;
        } else if((unsigned char)*src < 32) {
            dst[d++] = ' ';
            src++;
        } else {
            dst[d++] = *src++;
        }
    }
    dst[d] = 0;
}

static void webui_status_sync_tick(lv_timer_t *t) {
    (void)t;
    track_state_t st;
    ipc_get_state(&st);

    char esc_title[320], esc_artist[200], esc_album[200], esc_path[320];
    escape_json_str(esc_title, st.title, sizeof(esc_title));
    escape_json_str(esc_artist, st.artist, sizeof(esc_artist));
    escape_json_str(esc_album, st.album, sizeof(esc_album));
    escape_json_str(esc_path, st.path, sizeof(esc_path));

    int playing = ui_is_playing();

    const char *tmp_path = "/tmp/diskos_player.json.tmp";
    const char *final_path = "/tmp/diskos_player.json";

    FILE *f = fopen(tmp_path, "w");
    if(!f) return;

    fprintf(f, "{\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\",\"path\":\"%s\","
               "\"duration\":%ld,\"duration_ms\":%ld,\"position\":%ld,\"position_ms\":%ld,"
               "\"volume\":%d,\"playing\":%s,\"state\":%d,"
               "\"sample_rate\":%d,\"is_dsd\":%s}\n",
               esc_title, esc_artist, esc_album, esc_path,
               st.duration_ms, st.duration_ms, st.position_ms, st.position_ms,
               st.volume, playing ? "true" : "false", playing ? 2 : 0,
               st.sample_rate, st.is_dsd ? "true" : "false");
    fclose(f);
    rename(tmp_path, final_path);
}

static int wlan_ip_str(char *out, int n){
    int s = socket(AF_INET, SOCK_DGRAM, 0); if(s < 0) return 0;
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ-1);
    int ok = (ioctl(s, SIOCGIFADDR, &ifr) == 0);
    if(ok){
        struct sockaddr_in *a = (struct sockaddr_in *)&ifr.ifr_addr;
        snprintf(out, n, "%s", inet_ntoa(a->sin_addr));
    }
    close(s);
    return ok;
}

static int check_daemon_running(void){
    DIR *d = opendir("/proc"); if(!d) return 0;
    struct dirent *e; int found = 0;
    while((e = readdir(d))){
        if(e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[64]; snprintf(p, sizeof p, "/proc/%s/cmdline", e->d_name);
        FILE *f = fopen(p, "r"); if(!f) continue;
        char c[64] = "";
        if(fgets(c, sizeof c, f) && strstr(c, "diskos-webd")) found = 1;
        fclose(f);
        if(found) break;
    }
    closedir(d);
    return found;
}

void webui_refresh(void){
    g_web_running = check_daemon_running();

    char ip[40] = "";
    int has_ip = wlan_ip_str(ip, sizeof ip);

    char url_buf[128];
    lv_color_t acc = ui_current_accent();

    if(g_web_running){
        lv_label_set_text(g_status_lbl, "Status: RUNNING");
        lv_obj_set_style_text_color(g_status_lbl, acc, 0);
        if(has_ip){
            snprintf(url_buf, sizeof url_buf, "http://%s:8080/", ip);
        } else {
            snprintf(url_buf, sizeof url_buf, "http://<device-ip>:8080/");
        }
        if(g_main_btn){
            lv_obj_clear_state(g_main_btn, LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(g_main_btn, acc, 0);
        }
        lv_label_set_text(g_btn_lbl, "Stop Server");
        lv_obj_set_style_text_color(g_btn_lbl, theme_on_color(acc), 0);
        lv_label_set_text(g_info_lbl, "Control player & files from your browser.");
    } else {
        int installed = is_device_installed();
        int sd_ready = is_sd_package_present();

        if(installed){
            lv_label_set_text(g_status_lbl, "Status: STOPPED");
            lv_obj_set_style_text_color(g_status_lbl, TC(TEXT_MUTED), 0);
            if(has_ip){
                snprintf(url_buf, sizeof url_buf, "http://%s:8080/", ip);
            } else {
                snprintf(url_buf, sizeof url_buf, "Connect to Wi-Fi first");
            }
            if(g_main_btn){
                lv_obj_clear_state(g_main_btn, LV_STATE_DISABLED);
                lv_obj_set_style_bg_color(g_main_btn, acc, 0);
            }
            lv_label_set_text(g_btn_lbl, "Start Server");
            lv_obj_set_style_text_color(g_btn_lbl, theme_on_color(acc), 0);
            lv_label_set_text(g_info_lbl, "Tap Start to launch web control over Wi-Fi.");
        } else if(sd_ready){
            lv_label_set_text(g_status_lbl, "Status: SD READY");
            lv_obj_set_style_text_color(g_status_lbl, acc, 0);
            snprintf(url_buf, sizeof url_buf, has_ip ? "http://%s:8080/" : "Ready to install");
            if(g_main_btn){
                lv_obj_clear_state(g_main_btn, LV_STATE_DISABLED);
                lv_obj_set_style_bg_color(g_main_btn, acc, 0);
            }
            lv_label_set_text(g_btn_lbl, "Install & Start");
            lv_obj_set_style_text_color(g_btn_lbl, theme_on_color(acc), 0);
            lv_label_set_text(g_info_lbl, "Found Web UI package on MicroSD.\nTap to install & start.");
        } else {
            lv_label_set_text(g_status_lbl, "Status: NOT INSTALLED");
            lv_obj_set_style_text_color(g_status_lbl, TC(TEXT_MUTED), 0);
            snprintf(url_buf, sizeof url_buf, "MicroSD package needed");
            if(g_main_btn){
                lv_obj_add_state(g_main_btn, LV_STATE_DISABLED);
                lv_obj_set_style_bg_color(g_main_btn, TC(SURFACE), 0);
            }
            lv_label_set_text(g_btn_lbl, "Not Installed");
            lv_obj_set_style_text_color(g_btn_lbl, TC(TEXT_MUTED), 0);
            lv_label_set_text(g_info_lbl, "Copy diskos/ folder to MicroSD\nor reflash firmware.");
        }
    }
    lv_label_set_text(g_url_lbl, url_buf);

    const char *cur_pw = cfg_get_str("web_pw", "1234");
    char pw_buf[96];
    if(cur_pw[0]){
        snprintf(pw_buf, sizeof pw_buf, "PIN / Password: %s", cur_pw);
    } else {
        snprintf(pw_buf, sizeof pw_buf, "PIN / Password: (None)");
    }
    lv_label_set_text(g_pw_lbl, pw_buf);
}

static void toggle_server_cb(lv_event_t *e){
    (void)e;
    const char *script = get_webscript_path();
    if(!script) {
        webui_refresh();
        return;
    }
    if(g_web_running){
        char *cmd[] = { (char *)"/bin/sh", (char *)script, (char *)"stop", NULL };
        ui_run_bounded(cmd, 3000);
    } else {
        char *cmd[] = { (char *)"/bin/sh", (char *)script, (char *)"start", NULL };
        ui_run_bounded(cmd, 5000);
    }
    webui_refresh();
}

static void on_pin_done(const char *text){
    if(text){
        cfg_set_str("web_pw", text);
        if(g_web_running){
            /* Restart daemon with new password */
            const char *script = get_webscript_path();
            if(script){
                char *cmd_stop[] = { (char *)"/bin/sh", (char *)script, (char *)"stop", NULL };
                ui_run_bounded(cmd_stop, 3000);
                char *cmd_start[] = { (char *)"/bin/sh", (char *)script, (char *)"start", NULL };
                ui_run_bounded(cmd_start, 3000);
            }
        }
        webui_refresh();
    }
}

static void change_pin_cb(lv_event_t *e){
    (void)e;
    const char *cur_pw = cfg_get_str("web_pw", "1234");
    kbinput_open("Web PIN / Password", cur_pw, on_pin_done);
}

void webui_open(void){
    webui_refresh();
    screen_show(SCR_WEBUI);
}

void webui_create(lv_obj_t *root){
    ui_header(root, "Web Server");

    /* Status Label (RUNNING / STOPPED) */
    g_status_lbl = lv_label_create(root);
    lv_obj_set_width(g_status_lbl, 320);
    lv_obj_set_style_text_align(g_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_status_lbl, TF(UI_16), 0);
    lv_obj_align(g_status_lbl, LV_ALIGN_TOP_MID, 0, 78);

    /* Access URL */
    g_url_lbl = lv_label_create(root);
    lv_obj_set_width(g_url_lbl, 320);
    lv_obj_set_style_text_align(g_url_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_url_lbl, TF(UI_18), 0);
    lv_obj_set_style_text_color(g_url_lbl, TC(TEXT_PRIMARY), 0);
    lv_obj_align(g_url_lbl, LV_ALIGN_TOP_MID, 0, 106);

    /* Current PIN Label */
    g_pw_lbl = lv_label_create(root);
    lv_obj_set_width(g_pw_lbl, 320);
    lv_obj_set_style_text_align(g_pw_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_pw_lbl, TF(UI_14), 0);
    lv_obj_set_style_text_color(g_pw_lbl, TC(TEXT_SECONDARY), 0);
    lv_obj_align(g_pw_lbl, LV_ALIGN_TOP_MID, 0, 140);

    /* Change PIN button */
    lv_obj_t *pin_btn = lv_button_create(root);
    lv_obj_set_size(pin_btn, 130, 36);
    lv_obj_set_style_radius(pin_btn, 18, 0);
    lv_obj_set_style_bg_color(pin_btn, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(pin_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(pin_btn, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_align(pin_btn, LV_ALIGN_TOP_MID, 0, 168);
    ui_on(pin_btn, change_pin_cb, LV_EVENT_CLICKED, NULL, "webui.pin_btn", UI_CORE);

    lv_obj_t *pin_btn_lbl = lv_label_create(pin_btn);
    lv_label_set_text(pin_btn_lbl, "Change PIN");
    lv_obj_set_style_text_font(pin_btn_lbl, TF(UI_14), 0);
    lv_obj_set_style_text_color(pin_btn_lbl, TC(TEXT_PRIMARY), 0);
    lv_obj_center(pin_btn_lbl);

    /* Info / Guidance Label */
    g_info_lbl = lv_label_create(root);
    lv_obj_set_width(g_info_lbl, 280);
    lv_label_set_long_mode(g_info_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(g_info_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_info_lbl, TF(UI_14), 0);
    lv_obj_set_style_text_color(g_info_lbl, TC(TEXT_MUTED), 0);
    lv_obj_align(g_info_lbl, LV_ALIGN_TOP_MID, 0, 218);

    /* Start / Stop Action Button */
    g_main_btn = lv_button_create(root);
    lv_obj_set_size(g_main_btn, 190, 50);
    lv_obj_set_style_shadow_width(g_main_btn, 0, 0);
    lv_obj_align(g_main_btn, LV_ALIGN_BOTTOM_MID, 0, -44);
    ui_on(g_main_btn, toggle_server_cb, LV_EVENT_CLICKED, NULL, "webui.btn", UI_CORE);

    lv_color_t acc = ui_current_accent();
    lv_obj_set_style_bg_color(g_main_btn, acc, 0);
    lv_obj_set_style_bg_opa(g_main_btn, LV_OPA_COVER, 0);

    g_btn_lbl = lv_label_create(g_main_btn);
    lv_obj_center(g_btn_lbl);
    lv_obj_set_style_text_font(g_btn_lbl, TF(UI_16), 0);
    lv_obj_set_style_text_color(g_btn_lbl, theme_on_color(acc), 0);

    lv_timer_create(webui_status_sync_tick, 500, NULL);
    webui_refresh();
}

