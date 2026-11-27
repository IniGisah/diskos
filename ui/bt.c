/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "fwcaps.h"
#include "device_names_ui.h"
#include "i18n.h"
#include "procrun.h"
#include "ui_wake.h"
#include "radio_monitor.h"
#include "screens.h"
extern int ui_screen_backlight_on(void);
#include "theme.h"
#include "theme_kit.h"
#include "config.h"       /* cfg_get_int/cfg_set_int: persist the BT on/off intent */
#include "bt_codec_observe.h"
#include "bt_prepare.h"
#include "bt_tx_logic.h"
#include "musicdb.h"
#include "modes.h"
#include "bt_avrcp.h"
#include "modelock.h"
#include "radio_bt.h"
#include "radio_cfg.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>

/* Bluetooth settings (SCR_BT) + a details screen (SCR_BT_INFO).
 * The BT chip (BCM43438 / AP6212, 2.4GHz-only) sits on UART /dev/ttyS0. RE of the
 * stock mq_player (NOT BSA - bsa_server is dead code for the wrong chip) shows the
 * stock production stack is ALSO bluez, but it FIRST downloads the chip firmware
 * patch with `brcm_patchram_plus ... --patchram <BCM4343A1...hcd> /dev/ttyS0` @3Mbaud
 * to create a working hci0 - nothing attaches hci0 at boot (bcmdhd.ko only loads the
 * driver/GPIOs). Without that patchram step inquiry/scan is broken, which is why our
 * earlier bluez-only bring-up scanned poorly. bt_enable() now mirrors stock exactly:
 * patchram -> /usr/project/bluetoothd (a2dp,avrcp,source) -> hciconfig up/piscan/class
 * -> bluetoothctl agent. `bluetoothctl` drives scan/pair/connect; `bluealsa` is the
 * a2dp-source audio engine (SBC/LDAC).  Layout matches the Wi-Fi page (control row in the
 * centre band; the list carries on/scanning/empty state; connected checkmark row taps
 * through to details).  NOTE: routing the player's audio onto a connected BT
 * sink is the stock mq_player's job (it has bluealsa support) - building the
 * profile here is stage 1; confirm real headphone playback with the user. */

static lv_obj_t *g_sw, *g_list, *g_info_list;
static lv_obj_t *g_rescan_icon, *g_bt_note;
static lv_obj_t *g_row_obj, *g_row_val, *g_inf_name, *g_inf_conn, *g_inf_paired, *g_inf_type, *g_inf_codec, *g_inf_audio;
static int g_rescan_spinning;
static int g_scan_n;
static lv_timer_t *g_scan_timer;
static lv_timer_t *g_scan_live_timer;
static lv_timer_t *g_scanwait_timer;   /* bt_open's non-destructive "wait for adapter then scan" poll */
static uint32_t    g_scanwait_start;
static lv_timer_t *g_radio_timer;
static lv_timer_t *g_bootrestore_timer;
static lv_timer_t *g_rx_resume_timer;
static lv_timer_t *g_bt_autoroute_timer;
static lv_timer_t *g_info_live_timer;
static int g_autoroute_wait;
static char g_sel_mac[20];     /* device selected for the details screen */
static char g_bt_autorouted[20];
static char g_autoroute_fail_mac[20];
static int  g_autoroute_fails;
static uint32_t g_autoroute_backoff_until;
static int  g_autoroute_dirty;
static int  g_bt_probe_dirty;
static int  g_bt_live_codec = -1;
static int  g_bt_live_rate_hz = 0;
static char g_bt_live_mac[20];
static int  g_bt_on_last = 0;
static unsigned g_bt_on_seen = ~0u;
static uint32_t g_bt_on_retry = 0;
static void bt_status_note_sync(void);
static void scan_render(void);
static int bt_on(void);
static void bt_codec_async_cancel(void);
static void bt_scan_mark_connected(const char *mac, int set_bits, int clear_bits);
/* ---- routing ownership ---- */
/* Automatic routing obeys the connection coordinator. Before any explicit connection choice (startup) a connected
 * sink may be routed; afterwards only the authorized MAC may. A pending, failed, cancelled or superseded attempt
 * authorizes nothing, and a revocation is never undone by a PCM disappearing, a timeout or a player restart. */
static int  g_route_owned;
static char g_route_auth[20];
static int bt_route_allowed(const char *mac){ return !g_route_owned || !strcmp(mac, g_route_auth); }
static void bt_route_own(const char *mac){ g_route_owned = 1; snprintf(g_route_auth, sizeof g_route_auth, "%s", mac ? mac : ""); }
static void bt_route_revoke(const char *mac){
    if(!g_route_owned) bt_route_own(NULL);
    else if(!strcmp(mac, g_route_auth)) g_route_auth[0] = 0;
}
/* ---- end routing ownership ---- */

static void start_scan(void);             /* fwd */
static void scan_timer_cb(lv_timer_t *t);  /* fwd */
static void scan_abort(void);             /* fwd */
static void scan_kick(void);              /* fwd - off-thread re-enumerate (no discovery window) */
static void bt_autoroute_start(void);      /* fwd */
static void bt_autoroute_stop(void);       /* fwd */
static void bootrestore_cancel(void);      /* fwd - every explicit enable/disable supersedes boot observation */
static void rx_resume_cancel(void);        /* fwd - exit-from-RX restoration timer */
static char s_rx_resume_out_mac[20];       /* BT output MAC routed prior to entering RX mode */

/* The refresh glyph INSIDE the "Scanning" message spins while a scan runs. It lives
 * in g_list, so it must be stopped before g_list is cleaned (else the anim references
 * a freed object). scan_stop() is called at the top of every g_list-clearing path. */
static lv_obj_t *g_scan_icon;
static void spin_anim_cb(void *o, int32_t v){ lv_obj_set_style_transform_rotation((lv_obj_t*)o, v, 0); }
static void scan_stop(void){
    if(g_scan_icon){ lv_anim_delete(g_scan_icon, spin_anim_cb); g_scan_icon = NULL; }
}

/* ---- header helpers (local copies) -------------------------------------- */

/* UI polling never waits for a child. Separate keys preserve the existing timer callers. */
enum { JOB_DRAIN = 1, JOB_FORGET };
static void bt_job_add(unsigned h, int kind, const char *mac);
static struct { char cmd[512]; unsigned handle; } g_queries[4];
static void bt_capture_drop_except(const char *keep){
    for(int i = 0; i < 4; i++){
        if(!g_queries[i].handle || (keep && !strcmp(g_queries[i].cmd, keep))) continue;
        proc_cancel(g_queries[i].handle);
        bt_job_add(g_queries[i].handle, JOB_DRAIN, NULL);
        g_queries[i].handle = 0; g_queries[i].cmd[0] = 0;
    }
}
static int run_cap_bounded(const char *cmd, char *out, int cap, int timeout_ms){
    out[0] = 0;
    if(strlen(cmd) >= sizeof g_queries[0].cmd) return PROC_FAILED;
    int i = -1;
    for(int k = 0; k < 4; k++)
        if(!strcmp(g_queries[k].cmd, cmd)){ i = k; break; }
    if(i < 0) for(int k = 0; k < 4; k++)
        if(!g_queries[k].cmd[0]){ i = k; break; }
    if(i >= 0){
        if(!g_queries[i].handle){
            strcpy(g_queries[i].cmd, cmd);
            g_queries[i].handle = proc_start(cmd, timeout_ms, cap);
            return g_queries[i].handle ? 0 : PROC_FAILED;
        }
        int r = proc_poll(g_queries[i].handle, out, cap);
        if(!r) return 0;
        g_queries[i].handle = 0; g_queries[i].cmd[0] = 0;
        if(r != PROC_COMPLETE) out[0] = 0; /* incomplete query is unknown */
        return r;
    }
    return PROC_FAILED;
}
static void scan_spin_start(void){
    if(!g_scan_icon)return;
    lv_anim_delete(g_scan_icon,spin_anim_cb);
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, g_scan_icon);
    lv_anim_set_exec_cb(&a, spin_anim_cb);
    lv_anim_set_values(&a, 0, 3600); lv_anim_set_time(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}
static void scan_spinner_sync(int visible){
    static int was_visible=1;
    if(visible==was_visible)return;
    was_visible=visible;
    if(g_scan_icon){
        if(visible)scan_spin_start();else lv_anim_delete(g_scan_icon,spin_anim_cb);
    }
    if(g_rescan_icon){
        if(!visible && g_rescan_spinning){
            lv_anim_delete(g_rescan_icon,spin_anim_cb);
            lv_obj_set_style_transform_rotation(g_rescan_icon,0,0);
            g_rescan_spinning=0;
        }
    }
}
static void rescan_spin_set(int active){
    if(!g_rescan_icon){ g_rescan_spinning = 0; return; }
    if(active && !g_rescan_spinning){
        g_rescan_spinning = 1;
        lv_obj_set_style_transform_pivot_x(g_rescan_icon, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(g_rescan_icon, lv_pct(50), 0);
        lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, g_rescan_icon);
        lv_anim_set_exec_cb(&a, spin_anim_cb);
        lv_anim_set_values(&a, 0, 3600); lv_anim_set_time(&a, 900);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    } else if(!active && g_rescan_spinning){
        lv_anim_delete(g_rescan_icon, spin_anim_cb);
        lv_obj_set_style_transform_rotation(g_rescan_icon, 0, 0);
        g_rescan_spinning = 0;
    }
}
/* Legacy workflows and scan/codec workers: bounded wait; their state migration is separate. */
static int bt_capture_wait(const char *cmd, char *out, int cap, int timeout_ms){
    unsigned h = proc_start(cmd, timeout_ms, cap);
    out[0] = 0;
    if(!h) return 0;
    int r;
    r = proc_wait(h, out, cap);
    if(r != PROC_COMPLETE) out[0] = 0;
    return (int)strlen(out);
}
int ui_run_cap_bounded(const char *cmd, char *out, int cap, int timeout_ms){ return bt_capture_wait(cmd, out, cap, timeout_ms); }
static int run_cap(const char *cmd, char *out, int cap){ return bt_capture_wait(cmd, out, cap, 1000); }

/* a single status message (Scanning / off / empty) - centered in the list area */
static void list_msg(const char *m){
    if(!g_list) return;
    scan_stop();
    lv_obj_clean(g_list);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *e = lv_label_create(g_list);
    lv_label_set_text(e, m);
    lv_obj_set_style_text_color(e, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_font(e, TF(UI_14), 0);
}
/* "Scanning" + a spinning refresh glyph, centered in the list area */
static void list_msg_scanning(void){
    if(!g_list) return;
    scan_stop();
    lv_obj_clean(g_list);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *row = lv_obj_create(g_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, tr("Scanning"));
    lv_obj_set_style_text_color(t, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_font(t, TF(UI_14), 0);
    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, LV_SYMBOL_REFRESH);
    lv_obj_set_size(ic, 24, 24);
    lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ic, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_font(ic, TF(UI_14), 0);
    g_scan_icon = ic;
    lv_obj_set_style_transform_pivot_x(ic, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(ic, lv_pct(50), 0);
    if(ui_screen_backlight_on() && screen_current()==SCR_BT)scan_spin_start();
}

/* powered = bluetoothd up AND adapter Powered: yes */
static int g_bt_probe_pending;
static int bt_radio_native(void);
static int bt_on(void){
    if(bt_radio_native() == 0){
        g_bt_on_last = 0;
        g_bt_on_seen = radio_monitor_revision();
        return 0;
    }
    unsigned revision = radio_monitor_revision();
    uint32_t now = lv_tick_get();
    int need_fast = (!g_bt_on_last && bt_control_on() && (int32_t)(now - g_bt_on_retry) >= 0);
    if(!g_bt_probe_pending && !g_bt_probe_dirty && !need_fast &&
       g_bt_on_seen == revision && (radio_monitor_ready() || (int32_t)(now - g_bt_on_retry) < 0))
        return g_bt_on_last;
    char s[512]; int result = run_cap_bounded(
        "dbus-send --system --print-reply --reply-timeout=350 --dest=org.bluez /org/bluez/hci0 "
        "org.freedesktop.DBus.Properties.Get string:org.bluez.Adapter1 string:Powered 2>/dev/null",
        s, sizeof s, 500);
    g_bt_probe_pending = (result == 0);
    if(result){
        g_bt_on_retry = lv_tick_get() + 1000;
        if(s[0]) g_bt_on_last = (strstr(s, "boolean true") != NULL);
        else     g_bt_on_last = 0;
        if(!g_bt_probe_dirty) g_bt_on_seen = revision;
        else g_bt_probe_dirty = 0;
    }
    return g_bt_on_last;
}

/* Cheap "is the BT radio enabled?" for the status icon + QS tile: reads the bluetooth rfkill soft-block
 * from /sys (no process spawn). Reflects reality regardless of who set it - diskOS bt_disable() rfkill-
 * blocks, and the stock boot can bring BT up per its own SYSCONFIG - unlike the persisted cfg intent,
 * which goes stale when the stock firmware enables BT out from under us. Falls back to the intent if no
 * bluetooth rfkill node exists. */
static atomic_int g_bt_radio_observed;
static unsigned s_bt_op_gen = 0;
static uint64_t s_bt_disable_until_ms = 0;
static atomic_int g_bt_syncoff_inflight;
static lv_timer_t *g_bt_syncoff_timer = NULL;
static int g_bt_syncoff_tries = 0;

static uint64_t bt_mono_ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static unsigned bt_op_next_gen(void){
    unsigned gen = ++s_bt_op_gen;
    if(gen == 0) gen = ++s_bt_op_gen;
    FILE *f = fopen("/tmp/.diskos_bt_op.tmp", "w");
    if(f){
        fprintf(f, "%u\n", gen);
        fclose(f);
        rename("/tmp/.diskos_bt_op.tmp", "/tmp/.diskos_bt_op");
    }
    return gen;
}

static int bt_radio_native(void){
    int found = 0;
    for(int i = 0; i < 12; i++){
        char p[64]; snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/type", i);
        FILE *f = fopen(p, "r"); if(!f) continue;
        char t[16] = {0}; char *r = fgets(t, sizeof t, f); fclose(f);
        if(!r || strncmp(t, "bluetooth", 9) != 0) continue;
        found = 1;
        snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/soft", i);
        f = fopen(p, "r"); if(!f) continue;
        int soft = 1; if(fscanf(f, "%d", &soft) != 1) soft = 1; fclose(f);
        if(soft != 0) return 0;   /* any bluetooth rfkill soft-blocked -> BT off */
    }
    return found ? 1 : -1;
}
int bt_radio_on(void){
    int on = bt_radio_native();
    if(on > 0 && cfg_get_int("bt_on", 0) == 0 && bt_mono_ms() < s_bt_disable_until_ms)
        on = 0;
    if(on < 0) on = cfg_get_int("bt_on", 0);
    atomic_store(&g_bt_radio_observed, on);
    return on;
}
int bt_radio_cached(void){return atomic_load(&g_bt_radio_observed);}
int bt_radio_probe(void){int on=bt_radio_native();return on<0 ? bt_radio_cached() : on;}
/* What the page's switch and the Settings row's switch show: the saved intent OR the live radio (see bt_open). */
int bt_control_on(void){ return cfg_get_int("bt_on", 0) || bt_radio_on(); }

/* ---- enable / disable --------------------------------------------------- */
/* idempotent: ensure the pairing agent + a2dp-source audio endpoint are up.
 * Must run whenever the radio is on - without bt-agent pairing fails, and
 * without bluealsa there is no audio sink for the player to route to. */
static void bg_system(const char *cmd){
    pid_t pid = fork();
    if(pid == 0){
        if(fork() == 0){
            for(int i = 3; i < 256; i++) close(i);
            int fd = open("/dev/null", O_RDWR);
            if(fd >= 0){ dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if(fd > 2) close(fd); }
            setpgid(0, 0);
            execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if(pid > 0) waitpid(pid, NULL, 0);
}

static void bt_signal_procs_by_names(const char *const *names, size_t nnames, int sig){
    DIR *dir = opendir("/proc");
    if(!dir) return;
    struct dirent *de;
    while((de = readdir(dir)) != NULL){
        if(!isdigit((unsigned char)de->d_name[0])) continue;
        char path[64], comm[64];
        snprintf(path, sizeof path, "/proc/%s/comm", de->d_name);
        FILE *f = fopen(path, "r");
        if(!f) continue;
        int match = 0;
        if(fgets(comm, sizeof comm, f)){
            char *nl = strpbrk(comm, "\r\n");
            if(nl) *nl = 0;
            for(size_t i = 0; i < nnames; i++){
                size_t nlen = strlen(names[i]);
                if((nlen >= 15 && !strncmp(comm, names[i], 15)) || !strcmp(comm, names[i])){
                    match = 1;
                    break;
                }
            }
        }
        fclose(f);
        if(match){
            long p = strtol(de->d_name, NULL, 10);
            if(p > 1) kill((pid_t)p, sig);
        }
    }
    closedir(dir);
}

static void kill_procs_by_names(const char *const *names, size_t nnames){
    bt_signal_procs_by_names(names, nnames, SIGKILL);
}

static int bt_proc_has_comm(const char *target){
    DIR *dir = opendir("/proc");
    if(!dir) return 0;
    struct dirent *de;
    size_t tlen = strlen(target);
    int found = 0;
    while((de = readdir(dir)) != NULL){
        if(!isdigit((unsigned char)de->d_name[0])) continue;
        char path[64], comm[64];
        snprintf(path, sizeof path, "/proc/%s/comm", de->d_name);
        FILE *f = fopen(path, "r");
        if(!f) continue;
        if(fgets(comm, sizeof comm, f)){
            char *nl = strpbrk(comm, "\r\n");
            if(nl) *nl = 0;
            if((tlen >= 15 && !strncmp(comm, target, 15)) || !strcmp(comm, target)){
                found = 1;
                fclose(f);
                break;
            }
        }
        fclose(f);
    }
    closedir(dir);
    return found;
}

/* a2dp-source audio engine. --sbc-quality=medium (bit-pool ~33) keeps SBC encode load
 * under this X2000 CPU's ceiling: stock's default-quality stereo SBC stutters, but medium
 * quality plays clean STEREO with headroom (on-device: ~86% idle). Persistent bt-agent
 * handles just-works pairing whenever the radio is up. */
static void bt_ensure_services(void){
    if(ui_get_source_mode() == 2) return;
    if(access("/tmp/bt_enabling", F_OK) == 0) return;
    bg_system(
        "[ ! -e /tmp/bt_enabling ] || exit 0; "
        "pidof bluetoothd >/dev/null 2>&1 || exit 0; "
        "hciconfig hci0 2>/dev/null | grep -q RUNNING || exit 0; "
        "if ! pidof bluealsa bluealsa-custom >/dev/null 2>&1; then "
        "  BA=bluealsa; [ -x /opt/diskos/bin/bluealsa ] && BA=/opt/diskos/bin/bluealsa; "
        "  \"$BA\" -S --device=hci0 --profile=a2dp-source --sbc-quality=medium "
        "    --ldac-abr --ldac-quality=standard --codec=sbc --initial-volume=48 >/tmp/bluealsa.log 2>&1 & "
        "fi; "
        "if command -v bt-agent >/dev/null 2>&1; then "
        "  pidof bt-agent fiio_bluetoothctl >/dev/null 2>&1 || bt-agent -c NoInputNoOutput -d >/tmp/bt-agent.log 2>&1 & "
        "fi");
}
/* Stock V2.57 codec menu: mq_ui 0x455bd0, payloadless VALUE1 0..4.
 * diskOS extends this with aptX (5) and aptX-HD (6). */
const char *const BT_CODEC_LABEL[BT_CODEC_N] = {
    "SBC",
    "AAC",
    "LDAC Mobile",
    "LDAC Standard",
    "LDAC High",
    "aptX",
    "aptX-HD"
};
static int bt_mac_valid(const char *mac);
static int bt_codec_frame_value(const char *mac, int v, char *out, int cap){
    return bt_tx_codec_frame_value(mac, v, out, cap);
}
int bt_codec_pick(int choice, unsigned avail, int avail_known){
    return bt_tx_codec_pick(choice, avail, avail_known);
}
unsigned bt_codec_parse_avail(const char *info, int *known){
    return bt_tx_codec_parse_avail(info, known);
}
static int bt_fw_supports_06b3(void){
    int v = fw_os_ver();
    return v == 209 || v == 228 || v == 240 || v == 257;
}
static void bt_write_ldac_quality(int value){
    if(value < 2 || value > 4) return;
    FILE *fq = fopen("/usr/data/bt_ldac_quality", "w");
    if(fq){
        fprintf(fq, "%s\n", (value == 2) ? "mobile" : (value == 4) ? "high" : "standard");
        fclose(fq);
    }
}
/* Shell command that prints "PCM=<path>" then the A2DP source PCM's info for a device (MAC already validated);
 * it prints nothing while BlueALSA has no such PCM. */
static int bt_codec_query_cmd(const char *mac, char *out, int cap){
    if(!bt_mac_valid(mac)) return -1;
    char d[24]; int n = 0;
    for(int i = 0; i < 17; i++) d[n++] = mac[i] == ':' ? '_' : mac[i];
    d[n] = 0;
    int r = snprintf(out, cap, "p=$(bluealsa-cli list-pcms 2>/dev/null) || exit 1; "
                               "p=$(printf '%%s\\n' \"$p\" | grep -iE 'dev_%s/(a2dpsrc|a2dp-source)' | head -n 1); "
                               "[ -n \"$p\" ] && { echo \"PCM=$p\"; bluealsa-cli info \"$p\" 2>/dev/null; }", d);
    return (r > 0 && r < cap) ? 0 : -1;
}
/* Non-blocking codec frame builder: never stalls the LVGL thread.
 * When choice > 0 and bt_prepare is running, bt_prepare probes and applies the best codec
 * asynchronously; this fast path seeds SBC (06b3001D0000<MAC>) without blocking. */
int ui_bt_codec_frame(const char *mac, char *out, int cap){
    int choice = cfg_get_int("bt_codec", -1);
    if(choice < 0) choice = mdb_sysconfig_bt_codec();
    if(choice < 0 || choice >= BT_CODEC_N) choice = 0;
    bt_write_ldac_quality(choice);
    return bt_codec_frame_value(mac, 0, out, cap);
}
static int bt_pref_codec_choice(void){
    int c = cfg_get_int("bt_codec", -1);
    if(c < 0) c = mdb_sysconfig_bt_codec();
    if(c < 0 || c >= BT_CODEC_N) c = 0;
    return c;
}
static int bt_display_codec_choice(const char *routed){
    if(routed && routed[0] && g_bt_live_codec >= 0 && g_bt_live_codec < BT_CODEC_N &&
       !strcasecmp(routed, g_bt_live_mac)){
        if(g_bt_live_codec >= 2 && g_bt_live_codec <= 4){
            int pref = bt_pref_codec_choice();
            if(pref >= 2 && pref <= 4) return pref;
        }
        return g_bt_live_codec;
    }
    return bt_pref_codec_choice();
}
void ui_bt_live_codec_observe(const char *mac, int codec_family, int rate_hz){
    if(!mac || !bt_mac_valid(mac)){
        g_bt_live_codec = -1;
        g_bt_live_rate_hz = 0;
        g_bt_live_mac[0] = 0;
    } else {
        int pref = bt_pref_codec_choice();
        int mapped = -1;
        switch(codec_family){
        case BC_CODEC_SBC:     mapped = 0; break;
        case BC_CODEC_AAC:     mapped = 1; break;
        case BC_CODEC_LDAC:    mapped = (pref >= 2 && pref <= 4) ? pref : 3; break;
        case BC_CODEC_APTX:    mapped = 5; break;
        case BC_CODEC_APTX_HD: mapped = 6; break;
        default: break;
        }
        if(mapped >= 0){
            g_bt_live_codec = mapped;
            g_bt_live_rate_hz = rate_hz;
            snprintf(g_bt_live_mac, sizeof g_bt_live_mac, "%s", mac);
        }
    }
    if(screen_current() == SCR_BT){
        if(g_scan_n > 0) scan_render();
        bt_status_note_sync();
    }
}
static void bt_codec_sync_start(const char *mac, int choice, int announce_fb);
void ui_bt_codec_sync_after_route(const char *mac, int choice){
    if(ui_get_source_mode() == 2 || !bt_mac_valid(mac)) return;
    bt_codec_sync_start(mac, choice, 1);
}
int ui_bt_codec_set(int value){
    if(value < 0 || value >= BT_CODEC_N) return -1;
    cfg_set_int("bt_codec", value);

    const char *codec_arg = bt_tx_codec_cli_name(value);
    bt_write_ldac_quality(value);

    if(ui_get_source_mode() == 2){
        /* BLUETOOTH RECEIVING MODE (Mode 2 / A2DP Sink):
         * NEVER send 06b3 to mq_player here: 06b3 is transmitter-only and invokes
         * set_out_dev_sample_array(2, ...) inside mq_player, which crashes mq_player
         * and kills the bluetooth daemon. */
        FILE *f = fopen("/usr/data/bt_rx_codec", "w");
        if(f){
            fprintf(f, "%s\n", codec_arg);
            fclose(f);
        }
        char cmd[768];
        snprintf(cmd, sizeof cmd,
                 "( pidof bluetoothd >/dev/null 2>&1 || exit 0; "
                 "  active=0; "
                 "  for p in $(bluealsa-cli list-pcms 2>/dev/null); do "
                 "    case \"$p\" in "
                 "      *a2dpsnk*|*a2dp-sink*) active=1; bluealsa-cli codec \"$p\" %s 2>/dev/null || true ;; "
                 "    esac; "
                 "  done; "
                 "  if [ \"$active\" -eq 0 ] && hciconfig hci0 2>/dev/null | grep -q RUNNING; then "
                 "    killall -9 bluealsa bluealsa-custom 2>/dev/null; "
                 "    sleep 0.2; "
                 "    /opt/diskos/bin/bluealsa -S --device=hci0 --profile=a2dp-sink -p hfp-ag >/tmp/bluealsa.log 2>&1 & "
                 "  fi ) >/dev/null 2>&1 &",
                 codec_arg);
        bg_system(cmd);
        return 0;
    }

    /* BLUETOOTH TRANSMITTING / LOCAL MODE: non-blocking reconfiguration when routed. */
    const char *mac = ui_route_mac();
    if(mac && bt_mac_valid(mac)){
        if(ui_bt_reconfigure_codec(value) != 0){
            bt_codec_sync_start(mac, value, 1);
        }
        if(screen_current() == SCR_BT){
            if(g_scan_n > 0) scan_render();
            bt_status_note_sync();
        } else if(screen_current() == SCR_BT_INFO && !strcasecmp(g_sel_mac, mac)){
            bt_info_refresh();
        }
    }
    return 0;
}

static atomic_int s_bt_rx_ll = ATOMIC_VAR_INIT(-1);

int bt_rx_low_latency_enabled(void){
    int v = atomic_load(&s_bt_rx_ll);
    if(v < 0){
        v = cfg_get_int("bt_rx_ll", 0);
        atomic_store(&s_bt_rx_ll, v ? 1 : 0);
    }
    return v;
}

void bt_rx_low_latency_apply(int enable){
    atomic_store(&s_bt_rx_ll, enable ? 1 : 0);
    cfg_set_int("bt_rx_ll", enable ? 1 : 0);
    FILE *f = fopen("/usr/data/bt_rx_ll", "w");
    if(f){
        fprintf(f, "%d\n", enable ? 1 : 0);
        fclose(f);
    }
    /* Low Latency mode is RX-only (a2dp-sink); never interrupt TX audio or start bluealsa when off. */
    if(ui_get_source_mode() != 2) return;
    if(enable){
        bg_system("( pidof bluetoothd >/dev/null 2>&1 || exit 0; "
                  "  active=0; "
                  "  for p in $(bluealsa-cli list-pcms 2>/dev/null); do "
                  "    case \"$p\" in "
                  "      *a2dpsnk*|*a2dp-sink*) active=1; bluealsa-cli codec \"$p\" SBC >/dev/null 2>&1 || true ;; "
                  "    esac; "
                  "  done; "
                  "  if [ \"$active\" -eq 0 ] && hciconfig hci0 2>/dev/null | grep -q RUNNING && pidof bluealsa bluealsa-custom >/dev/null 2>&1; then "
                  "    killall -9 bluealsa bluealsa-custom 2>/dev/null; "
                  "    sleep 0.2; "
                  "    /opt/diskos/bin/bluealsa -S --device=hci0 --profile=a2dp-sink -p hfp-ag >/tmp/bluealsa.log 2>&1 & "
                  "  fi ) >/dev/null 2>&1 &");
    } else {
        bg_system("( pidof bluetoothd >/dev/null 2>&1 || exit 0; "
                  "  active=0; "
                  "  for p in $(bluealsa-cli list-pcms 2>/dev/null); do "
                  "    case \"$p\" in *a2dpsnk*|*a2dp-sink*) active=1 ;; esac; "
                  "  done; "
                  "  if [ \"$active\" -eq 0 ] && hciconfig hci0 2>/dev/null | grep -q RUNNING && pidof bluealsa bluealsa-custom >/dev/null 2>&1; then "
                  "    killall -9 bluealsa bluealsa-custom 2>/dev/null; "
                  "    sleep 0.2; "
                  "    /opt/diskos/bin/bluealsa -S --device=hci0 --profile=a2dp-sink -p hfp-ag >/tmp/bluealsa.log 2>&1 & "
                  "  fi ) >/dev/null 2>&1 &");
    }
}

static int64_t bt_probe_now(void){
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC, &t) != 0) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
/* Thread-safe capture for worker threads (never touches PROC_SLOTS). Only complete
 * zero-exit output is evidence. One deadline covers output AND child exit. */
static int bt_probe_run(const char *cmd, char *out, int cap, int timeout_ms){
    if(!out || cap < 1) return 0;
    out[0] = 0;
    if(!cmd || cap < 2 || timeout_ms <= 0 || timeout_ms > 30000) return 0;
    struct sigaction chld;
    if(sigaction(SIGCHLD, NULL, &chld) != 0 || chld.sa_handler != SIG_DFL ||
       (chld.sa_flags & SA_NOCLDWAIT)) return 0;
    int64_t start = bt_probe_now();
    if(start < 0) return 0;
    int64_t deadline = start + timeout_ms;
    int fds[2];
    if(pipe(fds) != 0) return 0;
    pid_t pid = fork();
    if(pid < 0){ close(fds[0]); close(fds[1]); return 0; }
    if(pid == 0){
        close(fds[0]);
        if(setpgid(0, 0) != 0 || dup2(fds[1], 1) < 0) _exit(127);
        close(fds[1]);
        for(int fd = 3; fd < 256; fd++) close(fd);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL); _exit(127);
    }
    setpgid(pid, pid);
    close(fds[1]);
    int n = 0, eof = 0, reaped = 0, success = 0, status;
    for(;;){
        int64_t now = bt_probe_now();
        if(now < 0 || now >= deadline) break;
        int left = (int)(deadline - now);
        if(eof){
            pid_t r = waitpid(pid, &status, WNOHANG);
            if(r == pid){ reaped = 1; success = WIFEXITED(status) && WEXITSTATUS(status) == 0; break; }
            if(r < 0 && errno != EINTR) break;
            if(poll(NULL, 0, left < 10 ? left : 10) < 0 && errno != EINTR) break;
            continue;
        }
        struct pollfd pf = { fds[0], POLLIN, 0 };
        int ready = poll(&pf, 1, left);
        if(ready < 0){ if(errno == EINTR) continue; break; }
        if(!ready || (pf.revents & (POLLERR | POLLNVAL))) break;
        char chunk[256]; ssize_t r = read(fds[0], chunk, sizeof chunk);
        if(r < 0){ if(errno == EINTR) continue; break; }
        if(!r){ eof = 1; continue; }
        if(r > cap - 1 - n) break;
        memcpy(out + n, chunk, (size_t)r);
        n += r;
    }
    close(fds[0]);
    if(!reaped){
        kill(-pid, SIGKILL); kill(pid, SIGKILL);
        while(waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    out[success ? n : 0] = 0;
    return success ? n : 0;
}
/* Separate read-only job for bt_prepare. A cancelled worker keeps the single busy
 * latch until it exits, so route churn cannot accumulate jobs. */
static struct {
    pthread_mutex_t mu;
    uint64_t serial, id;
    unsigned generation;
    int busy, done, good, known;
    unsigned available;
    bc_snapshot selected;
} g_bp = {.mu = PTHREAD_MUTEX_INITIALIZER};
typedef struct { uint64_t id; char mac[18]; } bp_probe_arg;
static void *bt_prepare_probe_worker(void *arg){
    bp_probe_arg *a = arg;
    char cmd[384], info[4097]; bc_snapshot s = {0}; int n = 0, known = 0; unsigned avail = 0;
    if(bt_codec_query_cmd(a->mac, cmd, sizeof cmd) == 0) n = bt_probe_run(cmd, info, sizeof info, 700);
    int good = n > 0 && bc_parse(info, (size_t)n, a->mac, &s) == BC_OK && s.adapter == 0;
    if(good) avail = bt_codec_parse_avail(info, &known);
    pthread_mutex_lock(&g_bp.mu);
    if(g_bp.id == a->id){
        g_bp.selected = s; g_bp.known = known; g_bp.available = avail;
        g_bp.good = good; g_bp.done = 1;
    }
    g_bp.busy = 0;
    pthread_mutex_unlock(&g_bp.mu);
    ui_notify(UI_WAKE_BT);
    free(a); return NULL;
}
int bt_prepare_probe_start(const char *mac, unsigned generation, uint64_t *id){
    if(!id || !bt_mac_valid(mac) || generation != ipc_generation()) return -1;
    bp_probe_arg *a = malloc(sizeof *a); if(!a) return -1;
    pthread_mutex_lock(&g_bp.mu);
    if(g_bp.busy || g_bp.serial == UINT64_MAX){
        int rc = g_bp.busy ? 1 : -1; pthread_mutex_unlock(&g_bp.mu); free(a); return rc;
    }
    a->id = ++g_bp.serial; snprintf(a->mac, sizeof a->mac, "%s", mac);
    g_bp.id = a->id; g_bp.generation = generation; g_bp.busy = 1; g_bp.done = 0;
    pthread_t thread;
    if(pthread_create(&thread, NULL, bt_prepare_probe_worker, a) != 0){
        g_bp.busy = 0; g_bp.id = 0; pthread_mutex_unlock(&g_bp.mu); free(a); return -1;
    }
    *id = a->id; pthread_detach(thread); pthread_mutex_unlock(&g_bp.mu); return 0;
}
int bt_prepare_probe_read(uint64_t id, unsigned generation, bc_snapshot *s, int *known, unsigned *avail){
    if(!s || !known || !avail) return -1;
    pthread_mutex_lock(&g_bp.mu);
    int rc = -1;
    if(id && id == g_bp.id && generation == g_bp.generation && generation == ipc_generation()){
        rc = g_bp.done ? (g_bp.good ? 1 : -1) : 0;
        if(rc == 1){ *s = g_bp.selected; *known = g_bp.known; *avail = g_bp.available; }
    }
    pthread_mutex_unlock(&g_bp.mu); return rc;
}
void bt_prepare_probe_cancel(uint64_t id){
    pthread_mutex_lock(&g_bp.mu);
    if(g_bp.id == id){ g_bp.id = 0; g_bp.done = 0; }
    pthread_mutex_unlock(&g_bp.mu);
}

/* Separate off-thread `bluealsa-cli codec` applier for bt_prepare. */
static struct {
    pthread_mutex_t mu;
    uint64_t serial, id;
    unsigned generation;
    int busy, done, ok;
} g_bc_apply = {.mu = PTHREAD_MUTEX_INITIALIZER};
typedef struct { uint64_t id; char pcm[128]; char codec[24]; } bp_codec_arg;
static void *bt_prepare_codec_apply_worker(void *arg){
    bp_codec_arg *a = arg;
    char cmd[256], out[64];
    snprintf(cmd, sizeof cmd, "bluealsa-cli codec '%s' %s >/dev/null 2>&1 && echo OK", a->pcm, a->codec);
    int n = bt_probe_run(cmd, out, sizeof out, 1500);
    int ok = (n > 0 && strstr(out, "OK") != NULL);
    pthread_mutex_lock(&g_bc_apply.mu);
    if(g_bc_apply.id == a->id){
        g_bc_apply.ok = ok;
        g_bc_apply.done = 1;
    }
    g_bc_apply.busy = 0;
    pthread_mutex_unlock(&g_bc_apply.mu);
    ui_notify(UI_WAKE_BT);
    free(a);
    return NULL;
}
int bt_prepare_codec_apply_start(const char *pcm, const char *codec_name, unsigned generation, uint64_t *id){
    if(!id || !pcm || !pcm[0] || !codec_name || !codec_name[0] || generation != ipc_generation()) return -1;
    /* Validate pcm path has no shell metacharacters */
    for(const char *p = pcm; *p; p++){
        char c = *p;
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='/'||c=='_'||c=='-')) return -1;
    }
    bp_codec_arg *a = malloc(sizeof *a); if(!a) return -1;
    pthread_mutex_lock(&g_bc_apply.mu);
    if(g_bc_apply.busy || g_bc_apply.serial == UINT64_MAX){
        int rc = g_bc_apply.busy ? 1 : -1;
        pthread_mutex_unlock(&g_bc_apply.mu); free(a); return rc;
    }
    a->id = ++g_bc_apply.serial;
    snprintf(a->pcm, sizeof a->pcm, "%s", pcm);
    snprintf(a->codec, sizeof a->codec, "%s", codec_name);
    g_bc_apply.id = a->id; g_bc_apply.generation = generation;
    g_bc_apply.busy = 1; g_bc_apply.done = 0; g_bc_apply.ok = 0;
    pthread_t th;
    if(pthread_create(&th, NULL, bt_prepare_codec_apply_worker, a) != 0){
        g_bc_apply.busy = 0; g_bc_apply.id = 0;
        pthread_mutex_unlock(&g_bc_apply.mu); free(a); return -1;
    }
    *id = a->id;
    pthread_detach(th);
    pthread_mutex_unlock(&g_bc_apply.mu);
    return 0;
}
int bt_prepare_codec_apply_read(uint64_t id, unsigned generation){
    pthread_mutex_lock(&g_bc_apply.mu);
    int rc = -1;
    if(id && id == g_bc_apply.id && generation == g_bc_apply.generation && generation == ipc_generation()){
        rc = g_bc_apply.done ? (g_bc_apply.ok ? 1 : -1) : 0;
    }
    pthread_mutex_unlock(&g_bc_apply.mu);
    return rc;
}
void bt_prepare_codec_apply_cancel(uint64_t id){
    pthread_mutex_lock(&g_bc_apply.mu);
    if(g_bc_apply.id == id){ g_bc_apply.id = 0; g_bc_apply.done = 0; }
    pthread_mutex_unlock(&g_bc_apply.mu);
}
int bt_prepare_codec_frame(const char *mac, int choice, char *out, int capacity){
    return choice < 0 || choice >= BT_CODEC_N ? -1 : bt_codec_frame_value(mac, choice, out, capacity);
}

/* Full stock bring-up: download the chip firmware patch over /dev/ttyS0 (creates a
 * working hci0), poll org.bluez D-Bus readiness, power on, start bluealsa + persistent bt-agent + avrcpd. */
static int bt_syncoff_send_cb(const char *frame, void *ctx){
    (void)frame;
    return ctx ? (int)(intptr_t)ctx : 0;
}

typedef struct {
    const radio_fw_caps_t *caps;
    int on;
} bt_syncoff_job_t;

static void *bt_syncoff_worker(void *arg){
    bt_syncoff_job_t *job = (bt_syncoff_job_t *)arg;
    radio_bt_ops_t ops = {
        bt_syncoff_send_cb,
        NULL,
        "/usr/data/fiio/db/sysconfig.db",
        job->caps
    };
    radio_bt_set_with(&ops, job->on ? 1 : 0);
    free(job);
    atomic_store(&g_bt_syncoff_inflight, 0);
    return NULL;
}

static int s_bt_player_may_init = 1;
static uint64_t s_bt_off_ms = 0;
static int s_bt_want_06c1 = 0;
static lv_timer_t *g_bt_stage2_timer = NULL;
static uint32_t s_bt_stage2_start = 0;

static void bt_stage2_cancel(void){
    s_bt_want_06c1 = 0;
    if(g_bt_stage2_timer){
        lv_timer_del(g_bt_stage2_timer);
        g_bt_stage2_timer = NULL;
    }
}

static void bt_syncoff_cancel(void){
    if(g_bt_syncoff_timer){
        lv_timer_del(g_bt_syncoff_timer);
        g_bt_syncoff_timer = NULL;
    }
    g_bt_syncoff_tries = 0;
}

static void bt_player_sync_off(int automatic);

static void bt_syncoff_timer_cb(lv_timer_t *t){
    (void)t;
    if(cfg_get_int("bt_on", 0) != 0 || ui_get_source_mode() == 2){
        bt_syncoff_cancel();
        return;
    }
    if(++g_bt_syncoff_tries > 20){
        bt_syncoff_cancel();
        return;
    }
    bt_player_sync_off(1);
}

static void bt_player_spawn_dbsync(const radio_fw_caps_t *caps, int on){
    if(atomic_exchange(&g_bt_syncoff_inflight, 1) != 0) return;
    bt_syncoff_job_t *job = (bt_syncoff_job_t *)malloc(sizeof *job);
    if(!job){
        atomic_store(&g_bt_syncoff_inflight, 0);
        return;
    }
    job->caps = caps;
    job->on = on ? 1 : 0;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if(pthread_create(&th, &attr, bt_syncoff_worker, job) != 0){
        free(job);
        atomic_store(&g_bt_syncoff_inflight, 0);
    }
    pthread_attr_destroy(&attr);
}

/* Tell stock mq_player that BT is off (06c0) and mirror SYSCONFIG.BT_STATUS=0 off the UI thread
 * so mq_player does not save BT_STATUS=1 on shutdown or re-enable BT on next boot/restart. */
static void bt_player_sync_off(int automatic){
    if(ui_get_source_mode() == 2) return;
    const radio_fw_caps_t *caps = radio_fw_caps_running();
    if(!caps || !(caps->cmds & RADIO_CAP_CMD_BT)) return;

    int send_res;
    if(automatic){
        send_res = ipc_send_automatic("06c0000C0000", ipc_generation());
    } else if(ipc_player_ready()){
        ipc_set_quiet(1);
        send_res = ipc_send_cmd("06c0000C0000");
        ipc_set_quiet(0);
    } else {
        send_res = IPC_SEND_DEFERRED;
    }

    if(send_res == IPC_SEND_DEFERRED){
        if(!g_bt_syncoff_timer){
            g_bt_syncoff_tries = 0;
            g_bt_syncoff_timer = lv_timer_create(bt_syncoff_timer_cb, 1000, NULL);
        }
        return;
    }
    if(send_res != 0) return;

    s_bt_off_ms = bt_mono_ms();
    s_bt_player_may_init = 1;
    bt_syncoff_cancel();
    bt_player_spawn_dbsync(caps, 0);
}

/* Pre-initialize stock mq_player's BT flag (06c1) once hci0 is already up cleanly,
 * so mq_player never hits `hci0 up failed` or traps itself in a 12s firmware re-burn
 * loop, and later route_bt/BP_ROUTE 06c1 calls are safe no-ops. */
static int bt_player_sync_on(void){
    if(ui_get_source_mode() == 2) return 0;
    const radio_fw_caps_t *caps = radio_fw_caps_running();
    if(!caps || !(caps->cmds & RADIO_CAP_CMD_BT)) return 0;
    if(!ipc_player_ready()) return 0;

    ipc_set_quiet(1);
    int send_res = ipc_send_cmd("06c1000C0000");
    ipc_set_quiet(0);
    if(send_res != 0) return 0;

    bt_player_spawn_dbsync(caps, 1);
    return 1;
}

static void bt_rfkill_set_sync(int unblock){
    int fd = open("/dev/rfkill", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if(fd >= 0){
        struct { uint32_t idx; uint8_t type, op, soft, hard; } ev = {
            0, 2, 3, (uint8_t)(unblock ? 0 : 1), 0
        };
        ssize_t wr = write(fd, &ev, sizeof ev);
        (void)wr;
        close(fd);
    }
    for(int i = 0; i < 12; i++){
        char p[64]; snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/type", i);
        FILE *f = fopen(p, "r"); if(!f) continue;
        char t[16] = {0}; char *r = fgets(t, sizeof t, f); fclose(f);
        if(!r || strncmp(t, "bluetooth", 9) != 0) continue;
        snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/soft", i);
        int softfd = open(p, O_WRONLY | O_CLOEXEC);
        if(softfd >= 0){ ssize_t wr = write(softfd, unblock ? "0\n" : "1\n", 2); (void)wr; close(softfd); }
        snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/state", i);
        int sfd = open(p, O_WRONLY | O_CLOEXEC);
        if(sfd >= 0){ ssize_t wr = write(sfd, unblock ? "1\n" : "0\n", 2); (void)wr; close(sfd); }
    }
}

static void bt_stage2_poke(void){
    if(!s_bt_want_06c1){
        if(g_bt_stage2_timer){ lv_timer_del(g_bt_stage2_timer); g_bt_stage2_timer = NULL; }
        return;
    }
    if(ui_get_source_mode() == 2 || cfg_get_int("bt_on", 0) != 1){
        bt_stage2_cancel();
        return;
    }
    if(access("/tmp/.diskos_bt_hci_up", F_OK) == 0){
        s_bt_want_06c1 = 0;
        if(bt_player_sync_on()) s_bt_player_may_init = 0;
        bt_stage2_cancel();
        return;
    }
    if(lv_tick_elaps(s_bt_stage2_start) > 25000){
        bt_stage2_cancel();
    }
}

static void bt_stage2_timer_cb(lv_timer_t *t){
    if(t != g_bt_stage2_timer) return;
    bt_stage2_poke();
}

static void bt_enable(void){
    bootrestore_cancel();   /* an explicit enable has its own radio poll */
    rx_resume_cancel();
    bt_syncoff_cancel();
    bt_stage2_cancel();
    s_bt_disable_until_ms = 0;
    g_bt_probe_dirty = 1;
    g_route_owned = 0;
    g_route_auth[0] = 0;
    unlink("/tmp/.diskos_bt_rx");
    if(access("/tmp/bt_enabling", F_OK) == 0){
        struct stat st;
        int busy = (g_radio_timer != NULL || g_bootrestore_timer != NULL ||
                    g_scanwait_timer != NULL || g_rx_resume_timer != NULL);
        if(busy && stat("/tmp/bt_enabling", &st) == 0 && (time(NULL) - st.st_mtime) < 35)
            return;
        unlink("/tmp/bt_enabling");
    }
    unlink("/tmp/.diskos_bt_hci_up");
    /* Unblock rfkill synchronously before spawning bring-up, then attach brcm_patchram_plus +
     * hci0 UP FIRST before sending 06c1 to mq_player so mq_player never hits `hci0 up failed`. */
    bt_rfkill_set_sync(1);
    const radio_fw_caps_t *caps = radio_fw_caps_running();
    int want_06c1 = s_bt_player_may_init && ui_get_source_mode() != 2 &&
                    caps && (caps->cmds & RADIO_CAP_CMD_BT) && ipc_player_ready();
    int need_off_wait = (s_bt_off_ms != 0 && (bt_mono_ms() - s_bt_off_ms) < 900ULL);
    s_bt_want_06c1 = want_06c1 ? 1 : 0;
    if(s_bt_want_06c1){
        s_bt_stage2_start = lv_tick_get();
        g_bt_stage2_timer = lv_timer_create(bt_stage2_timer_cb, 400, NULL);
    }
    unsigned gen = bt_op_next_gen();
    int fd = open("/tmp/bt_enabling", O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    if(fd >= 0) close(fd);
    char cmd[4096];
    int n = snprintf(cmd, sizeof cmd,
        "( ok(){ [ -e /tmp/bt_enabling ] && [ \"$(cat /tmp/.diskos_bt_op 2>/dev/null)\" = \"%u\" ]; }; "
        "  ok || exit; "
        "  [ %d -eq 1 ] && { sleep 1; ok || exit; }; "
        "  killall -9 diskos-avrcpd fiio_bluetoothctl brcm_patchram_plus bluetoothd bluealsa bluealsa-custom bt-agent 2>/dev/null; "
        "  sleep 0.2; ok || exit; "
        "  hciconfig hci0 down 2>/dev/null; "
        "  rfkill block bluetooth; sleep 0.3; ok || exit; "
        "  rfkill unblock bluetooth; sleep 0.3; ok || exit; "
        "  brcm_patchram_plus --enable_lpm --enable_hci --no2bytes --tosleep 200000 --baudrate 3000000 "
        "    --patchram /lib/firmware/bt_bcm/BCM4343A1_001.002.009.1026.1055.hcd /dev/ttyS0 >/tmp/patchram.log 2>&1 & "
        "  HCI_OK=0; i=0; while [ \"$i\" -lt 24 ]; do ok || exit; "
        "    hciconfig hci0 up 2>/dev/null; "
        "    if hciconfig hci0 2>/dev/null | grep -q RUNNING; then HCI_OK=1; break; fi; "
        "    sleep 0.5; i=$((i+1)); done; "
        "  ok || exit; "
        "  [ \"$HCI_OK\" -eq 1 ] || exit; "
        "  : > /tmp/.diskos_bt_hci_up; "
        "  READY=0; "
        "  if [ %d -eq 1 ]; then "
        "    w=0; while [ \"$w\" -lt 30 ]; do ok || exit; "
        "      if pidof bt-agent >/dev/null 2>&1 && pidof bluetoothd >/dev/null 2>&1 && "
        "         pidof bluealsa bluealsa-custom >/dev/null 2>&1 && "
        "         hciconfig hci0 2>/dev/null | grep -q RUNNING && "
        "         dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus "
        "           org.freedesktop.DBus.NameHasOwner string:org.bluez 2>/dev/null | grep -q 'boolean true'; then "
        "        READY=1; break; "
        "      fi; "
        "      sleep 0.2; w=$((w+1)); done; "
        "    ok || exit; "
        "  fi; "
        "  if [ \"$READY\" -eq 0 ]; then "
        "    pidof bluetoothd >/dev/null 2>&1 || /usr/project/bluetoothd --noplugin=sap --plugin=a2dp,avrcp --mode=source >/tmp/btd.log 2>&1 & "
        "    j=0; while [ \"$j\" -lt 40 ]; do ok || exit; "
        "      dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus "
        "        org.freedesktop.DBus.NameHasOwner string:org.bluez 2>/dev/null | grep -q 'boolean true' && "
        "      dbus-send --system --print-reply --dest=org.bluez /org/bluez/hci0 "
        "        org.freedesktop.DBus.Properties.Get string:org.bluez.Adapter1 string:Address >/dev/null 2>&1 && break; "
        "      sleep 0.2; j=$((j+1)); done; "
        "    ok || exit; "
        "    dbus-send --system --dest=org.bluez /org/bluez/hci0 org.freedesktop.DBus.Properties.Set "
        "      string:org.bluez.Adapter1 string:Powered variant:boolean:true >/dev/null 2>&1 || bluetoothctl power on >/dev/null 2>&1; "
        "    hciconfig hci0 piscan 2>/dev/null; hciconfig hci0 class 0x200414 2>/dev/null; "
        "    ok || exit; "
        "    if ! pidof bluealsa bluealsa-custom >/dev/null 2>&1; then "
        "      BA=bluealsa; [ -x /opt/diskos/bin/bluealsa ] && BA=/opt/diskos/bin/bluealsa; "
        "      \"$BA\" -S --device=hci0 --profile=a2dp-source --sbc-quality=medium "
        "        --ldac-abr --ldac-quality=standard --codec=sbc --initial-volume=48 >/tmp/bluealsa.log 2>&1 & "
        "    fi; "
        "    if ! pidof bt-agent fiio_bluetoothctl >/dev/null 2>&1; then "
        "      if command -v bt-agent >/dev/null 2>&1; then "
        "        bt-agent -c NoInputNoOutput -d >/tmp/bt-agent.log 2>&1 & "
        "      else "
        "        bluetoothctl agent on >/dev/null 2>&1; bluetoothctl default-agent >/dev/null 2>&1; "
        "      fi; "
        "    fi; "
        "  fi; "
        "  bluetoothctl pairable on >/dev/null 2>&1; "
        "  pidof diskos-avrcpd >/dev/null 2>&1 || ("
        "    if [ -x /opt/diskos/bin/diskos-avrcpd ]; then /opt/diskos/bin/diskos-avrcpd >/tmp/avrcpd.log 2>&1 & "
        "    elif [ -x /usr/data/diskos-avrcpd ]; then /usr/data/diskos-avrcpd >/tmp/avrcpd.log 2>&1 & fi"
        "  ); "
        "  [ \"$(cat /tmp/.diskos_bt_op 2>/dev/null)\" = \"%u\" ] && rm -f /tmp/bt_enabling /tmp/.diskos_bt_hci_up "
        ") >/dev/null 2>&1 &",
        gen, need_off_wait ? 1 : 0, want_06c1 ? 1 : 0, gen);
    if(n <= 0 || n >= (int)sizeof cmd){
        bt_stage2_cancel();
        unlink("/tmp/bt_enabling");
        unlink("/tmp/.diskos_bt_hci_up");
        return;
    }
    bg_system(cmd);
}
static void bt_disable(void){
    bt_stage2_cancel();
    bootrestore_cancel();   /* Off must also cancel the independent boot observer */
    rx_resume_cancel();
    s_rx_resume_out_mac[0] = 0;
    ui_bt_route_prepare_cancel();
    bt_codec_async_cancel();
    ui_bt_live_codec_observe("", 0, 0);
    scan_abort();
    bt_autoroute_stop();
    g_route_owned = 0;
    g_route_auth[0] = 0;
    unlink("/tmp/bt_enabling");
    unlink("/tmp/.diskos_bt_hci_up");
    s_bt_disable_until_ms = bt_mono_ms() + 3000ULL;
    g_bt_on_last = 0;
    g_bt_on_seen = ~0u;
    g_bt_probe_dirty = 1;
    bt_op_next_gen();
    static const char *const tx_daemons[] = {
        "diskos-avrcpd", "bt-agent", "bluealsa", "bluealsa-custom",
        "bluetoothd", "brcm_patchram_plus", "fiio_bluetoothctl"
    };
    kill_procs_by_names(tx_daemons, sizeof tx_daemons / sizeof tx_daemons[0]);
    /* Synchronous rfkill block in C: zero background shell that could race a fast re-enable. */
    bt_rfkill_set_sync(0);
}

/* A Bluetooth MAC must be exactly AA:BB:CC:DD:EE:FF (hex + colons) before it is ever
 * interpolated into a bluetoothctl shell command. bluez scan output is already this
 * format, so this is defensive: a malformed/hostile address can carry no shell
 * metacharacters past this gate. */
static int bt_mac_valid(const char *mac){
    return bt_tx_mac_valid(mac);
}

static int bt_autoroute_allowed_cb(const char *mac, void *ctx){
    (void)ctx;
    return bt_route_allowed(mac);
}
static int bt_info_prop(const char *buf, const char *key, char *out, int cap);
/* A missing PCM is not a missing radio link (codec rebuilds remove PCMs).
 * Return 1 for confirmed down, 0 for connected, -1 for unknown, -2 for pending. */
static int bt_link_disconnected(const char *mac, int timeout_ms){
    if(timeout_ms <= 0 || !bt_mac_valid(mac)) return -1;
    char cmd[224], info[4097];
    snprintf(cmd, sizeof cmd,
        "bluetoothctl info %s 2>/dev/null; printf '\\nDISKOS_INFO_DONE\\n'; "
        "hcitool con 2>/dev/null; printf '\\nDISKOS_HCI_DONE\\n'", mac);
    bt_capture_drop_except(cmd);
    if(run_cap_bounded(cmd, info, sizeof info, timeout_ms) == 0) return -2;
    return bt_tx_parse_link_check(info, mac);
}

/* Route once per connected bluealsa A2DP sink. Keeping the MAC latched while the
 * PCM exists preserves a manual switch back to analog until the sink reconnects. */
static void bt_autoroute_poll_cb(lv_timer_t *t){
    static char missing[20];
    static unsigned misses, absent_polls;
    static int recovery_started;
    static uint32_t loss_sample_due;static int loss_sample_armed;
    static int link_pending; /* 0=none, 1=checking routed, 2=checking g_bt_autorouted */
    static char link_target[20];
    if(t)lv_timer_pause(t);
    g_autoroute_wait=0;
    if(ui_receiver_active()){
        link_pending = 0; link_target[0] = 0;
        bt_capture_drop_except(NULL);
        return;
    }
    const char *routed = ui_route_mac();
    int loss_accepted = ui_route_bt_loss_accepted();
    if(link_pending == 1){
        if(!routed[0] || strcmp(routed, link_target)){
            link_pending = 0; link_target[0] = 0;
        } else if(!loss_accepted){
            int disconnected = bt_link_disconnected(routed, 600);
            if(disconnected == -2){ g_autoroute_wait = 1; return; }
            link_pending = 0; link_target[0] = 0;
            loss_sample_armed = 1; loss_sample_due = lv_tick_get() + 3000;
            if(disconnected == 0){
                misses = absent_polls = 0; recovery_started = 0;
                if(t) lv_timer_resume(t);
                return;
            }
            if(absent_polls < 10) absent_polls++;
            if(disconnected < 0){
                fprintf(stderr, "bt autoroute: link_pending=1 disconnected=%d absent=%u\n", disconnected, absent_polls);
                misses = 0;
                if(absent_polls < 10){
                    if(t) lv_timer_resume(t);
                    return;
                }
                misses = 2;
            } else {
                if(misses < 2) misses++;
                if(absent_polls >= 10) misses = 2;
            }
            if(misses == 2){
                ui_bt_route_prepare_cancel();
                int rc = ui_route_bt_lost(!recovery_started && !loss_accepted);
                fprintf(stderr, "bt autoroute: ui_route_bt_lost -> rc=%d (routed='%s')\n", rc, ui_route_mac());
                recovery_started = 1;
                if(rc == 0){
                    g_bt_autorouted[0] = 0; misses = absent_polls = 0; missing[0] = 0;
                    if(screen_current() == SCR_BT){ if(g_scan_n > 0) scan_render(); bt_status_note_sync(); scan_kick(); }
                }
            }
            if(t) lv_timer_resume(t);
            return;
        } else {
            link_pending = 0; link_target[0] = 0;
        }
    } else if(link_pending == 2){
        if(!g_bt_autorouted[0] || strcmp(g_bt_autorouted, link_target)){
            link_pending = 0; link_target[0] = 0;
        } else {
            int disconnected = bt_link_disconnected(g_bt_autorouted, 600);
            if(disconnected == -2){ g_autoroute_wait = 1; return; }
            link_pending = 0; link_target[0] = 0;
            if(disconnected != 1){
                if(t) lv_timer_resume(t);
                return;
            }
            g_bt_autorouted[0] = 0;
            g_autoroute_fail_mac[0] = 0;
            g_autoroute_fails = 0;
            ui_bt_route_prepare_cancel();
            bt_status_note_sync();
            if(t) lv_timer_resume(t);
            return;
        }
    }

    char path[2048] = {0}, mac[20] = "";
    int found = 0, routed_found = 0;
    int result = run_cap_bounded(
        "bluealsa-cli list-pcms 2>/dev/null && printf '\nDISKOS_LIST_DONE\n'",
        path, sizeof path, 600);
    if(!result){g_autoroute_wait=1;return;}
    g_autoroute_dirty = 0;
    char *done = strstr(path, "\nDISKOS_LIST_DONE\n");
    if(!done || done[strlen("\nDISKOS_LIST_DONE\n")]){
        static uint32_t bad_log;
        if(!bad_log || lv_tick_elaps(bad_log) >= 10000){
            bad_log = lv_tick_get();
            fprintf(stderr, "bt autoroute: list-pcms incomplete result=%d len=%zu\n", result, strlen(path));
        }
        if(misses < 2) misses = 0;
        absent_polls = 0;
        if(t)lv_timer_resume(t);
        return;
    }
    *done = 0;
    bt_tx_parse_source_pcms(path, routed, bt_autoroute_allowed_cb, NULL,
                            mac, sizeof mac, &found, &routed_found);
    if(routed_found || loss_accepted ||
       (!routed[0] && (found || !g_bt_autorouted[0])))
        bt_capture_drop_except(NULL);
    if(routed[0] && (!routed_found || loss_accepted)){
        if(strcmp(missing, routed)){
            snprintf(missing, sizeof missing, "%s", routed);
            misses = absent_polls = 0; recovery_started = 0; loss_sample_armed = 0;
        }
        if(loss_sample_armed && (int32_t)(lv_tick_get()-loss_sample_due)<0){
            if(t) lv_timer_resume(t);
            return;
        }
        if(!loss_accepted){
            int disconnected = bt_link_disconnected(routed, 600);
            if(disconnected == -2){
                link_pending = 1;
                snprintf(link_target, sizeof link_target, "%s", routed);
                g_autoroute_wait = 1;
                return;
            }
            loss_sample_armed = 1; loss_sample_due = lv_tick_get() + 3000;
            if(disconnected == 0){
                misses = absent_polls = 0; recovery_started = 0;
                if(t) lv_timer_resume(t);
                return;
            }
            if(absent_polls < 10) absent_polls++;
            if(disconnected < 0){
                misses = 0;
                if(absent_polls < 10){
                    if(t) lv_timer_resume(t);
                    return;
                }
                misses = 2;
            } else {
                if(misses < 2) misses++;
                if(absent_polls >= 10) misses = 2;
            }
        } else {
            loss_sample_armed = 1; loss_sample_due = lv_tick_get() + 3000;
            if(misses < 2) misses++;
        }
        if(misses == 2){
            ui_bt_route_prepare_cancel();
            int rc = ui_route_bt_lost(!recovery_started && !loss_accepted);
            recovery_started = 1;
            if(rc == 0){
                g_bt_autorouted[0] = 0; misses = absent_polls = 0; missing[0] = 0;
                if(screen_current() == SCR_BT){ if(g_scan_n > 0) scan_render(); bt_status_note_sync(); scan_kick(); }
            }
        }
        if(t) lv_timer_resume(t);
        return;
    }
    misses = absent_polls = 0; missing[0] = 0; recovery_started = 0; loss_sample_armed = 0;
    if(routed_found && bt_route_allowed(routed)) snprintf(mac, sizeof mac, "%s", routed);
    if(!found){
        if(g_bt_autorouted[0]){
            int disconnected = bt_link_disconnected(g_bt_autorouted, 600);
            if(disconnected == -2){
                link_pending = 2;
                snprintf(link_target, sizeof link_target, "%s", g_bt_autorouted);
                g_autoroute_wait = 1;
                return;
            }
            if(disconnected != 1){ if(t) lv_timer_resume(t); return; }
        }
        g_bt_autorouted[0] = 0;
        g_autoroute_fail_mac[0] = 0;
        g_autoroute_fails = 0;
        ui_bt_route_prepare_cancel();
        bt_status_note_sync();
        if(t) lv_timer_resume(t);
        return;
    }
    if(mac[0] && strcmp(mac, g_bt_autorouted)){
        if(g_autoroute_fail_mac[0] && !strcasecmp(mac, g_autoroute_fail_mac) &&
           g_autoroute_fails >= 3 && (int32_t)(lv_tick_get() - g_autoroute_backoff_until) < 0){
            if(t) lv_timer_resume(t);
            return;
        }
        int rrc = ui_route_bt_automatic(mac);
        fprintf(stderr, "bt autoroute: auto(%s) -> %d (routed='%s')\n", mac, rrc, ui_route_mac());
        if(rrc == 0){
            if(!strcasecmp(ui_route_mac(), mac)){
                bt_note_routed(mac);
            }
        }
    }
    if(t) lv_timer_resume(t);
}

static void bt_autoroute_start(void){
    if(g_bt_autoroute_timer) return;
    fprintf(stderr, "bt autoroute: start (routed='%s')\n", ui_route_mac());
    g_bt_autoroute_timer = lv_timer_create(bt_autoroute_poll_cb, 2000, NULL);
    if(g_bt_autoroute_timer) lv_timer_ready(g_bt_autoroute_timer);
}
static void bt_autoroute_stop(void){
    bt_capture_drop_except(NULL);
    if(g_bt_autoroute_timer){ lv_timer_del(g_bt_autoroute_timer); g_bt_autoroute_timer = NULL; }
    g_bt_autorouted[0] = 0;
    g_autoroute_fail_mac[0] = 0;
    g_autoroute_fails = 0;
    g_autoroute_backoff_until = 0;
    g_route_owned = 0;
    g_route_auth[0] = 0;
}
void bt_note_routed(const char *mac){
    if(mac && bt_mac_valid(mac)){
        snprintf(g_bt_autorouted, sizeof g_bt_autorouted, "%s", mac);
        g_autoroute_fail_mac[0] = 0;
        g_autoroute_fails = 0;
        g_autoroute_backoff_until = 0;
        bt_scan_mark_connected(mac, 3, 0);
        if(screen_current() == SCR_BT){
            if(g_scan_n > 0) scan_render();
            bt_status_note_sync();
            scan_kick();
        } else if(screen_current() == SCR_BT_INFO && !strcasecmp(g_sel_mac, mac)){
            bt_info_refresh();
        }
    }
}
int bt_note_route_failed(const char *mac){
    int suppress = 0;
    if(mac && bt_mac_valid(mac)){
        if(!strcasecmp(g_autoroute_fail_mac, mac)){
            g_autoroute_fails++;
        } else {
            snprintf(g_autoroute_fail_mac, sizeof g_autoroute_fail_mac, "%s", mac);
            g_autoroute_fails = 1;
        }
        suppress = (g_autoroute_fails > 1);
        if(g_autoroute_fails >= 3){
            /* Bounded cooldown instead of permanently latching g_bt_autorouted = mac when !routed */
            g_autoroute_backoff_until = lv_tick_get() + 15000;
        }
        if(!strcasecmp(g_bt_autorouted, mac) && !ui_route_mac()[0]){
            g_bt_autorouted[0] = 0;
        }
    }
    if(screen_current() == SCR_BT){
        if(g_scan_n > 0) scan_render();
        bt_status_note_sync();
        scan_kick();
    }
    return suppress;
}
void bt_notify_player_restart(void){
    ui_bt_route_prepare_cancel();
    g_bt_autorouted[0] = 0;
    g_autoroute_fail_mac[0] = 0;
    g_autoroute_fails = 0;
    if(ui_get_source_mode() == 2){ bt_autoroute_stop(); return; }
    if(cfg_get_int("bt_on", 0) != 1){
        if(!g_rx_resume_timer && !g_radio_timer && !g_bootrestore_timer && bt_radio_native() == 1){
            bt_disable();
            atomic_store(&g_bt_radio_observed, 0);
            ui_status_refresh();
        }
        bt_player_sync_off(1);
        return;
    }
    if(g_bt_autoroute_timer){lv_timer_resume(g_bt_autoroute_timer);lv_timer_ready(g_bt_autoroute_timer);}
    else if(bt_on())bt_autoroute_start();
}

/* ---- pair + connect ----------------------------------------------------- */
static int bt_info_prop(const char *buf, const char *key, char *out, int cap){
    return bt_tx_info_prop(buf, key, out, cap);
}
/* ---- operations: the details query and drained children ----------------------------------------------
 * Nothing here waits: a command goes to the bounded child facility (procrun.c) and one 200 ms UI timer polls the
 * handles. A handle is single-use and belongs to exactly one owner, so a cancelled query is only drained (its
 * result is read and thrown away) and can never change what the UI reports. */
#define BT_PAIR_MS    20000
#define BT_TRUST_MS    4000
#define BT_CONNECT_MS 12000
#define BT_INFO_MS     1500
#define BT_JOB_MS      6000
enum { AT_IDLE, AT_PAIR, AT_TRUST, AT_CONNECT, AT_ROUTE };   /* AT_ROUTE: error naming only */
static struct { unsigned h; int stage; char mac[20]; uint32_t begin; } g_at;   /* the one current attempt */
static struct { unsigned h; int kind; char mac[20]; } g_jobs[PROC_SLOTS];                    /* each holds a facility slot */
static struct { unsigned h; int on; char cmd[512]; uint32_t begin; } g_inf;    /* the details query */
static void info_row(const char *key, const char *val);   /* fwd */
static lv_timer_t *g_ops_timer;
static void bt_ops_ensure(void);

static void bt_job_add(unsigned h, int kind, const char *mac){
    if(!h) return;
    for(int i = 0; i < PROC_SLOTS; i++) if(!g_jobs[i].h){
        g_jobs[i].h = h;
        g_jobs[i].kind = kind;
        snprintf(g_jobs[i].mac, sizeof g_jobs[i].mac, "%s", mac ? mac : "");
        bt_ops_ensure();
        return;
    }
    proc_cancel(h);   /* unreachable: each entry holds one of the facility's PROC_SLOTS slots */
}
static int bt_job_pending(const char *mac){
    for(int i = 0; i < PROC_SLOTS; i++)
        if(g_jobs[i].h && !strcmp(g_jobs[i].mac, mac)) return 1;
    return 0;
}
static void bt_jobs_step(void){
    for(int i = 0; i < PROC_SLOTS; i++){
        char out[16];
        if(!g_jobs[i].h) continue;
        int r = proc_poll(g_jobs[i].h, out, sizeof out);
        if(!r) continue;
        int kind = g_jobs[i].kind;
        g_jobs[i].h = 0;
        if(kind == JOB_FORGET){ ui_toast(r == PROC_COMPLETE ? tr("Device forgotten") : tr("Couldn't forget device")); scan_kick(); }
    }
}
static pthread_mutex_t g_codec_sync_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_codec_sync_gen = 0;

typedef struct {
    unsigned gen;
    char mac[20];
    int choice;
    int announce_fb;
} bt_codec_sync_arg_t;

typedef struct {
    unsigned gen;
    char mac[20];
    int requested;
    int selected;
    int live_family;
    int rate_hz;
    int announce_fb;
} bt_codec_sync_res_t;

static void bt_codec_async_cancel(void){
    pthread_mutex_lock(&g_codec_sync_mu);
    g_codec_sync_gen++;
    pthread_mutex_unlock(&g_codec_sync_mu);
}

static void bt_codec_sync_done_async(void *ptr){
    bt_codec_sync_res_t *res = (bt_codec_sync_res_t *)ptr;
    if(!res) return;
    pthread_mutex_lock(&g_codec_sync_mu);
    int current = (res->gen == g_codec_sync_gen);
    pthread_mutex_unlock(&g_codec_sync_mu);
    if(!current || ui_get_source_mode() == 2){
        free(res);
        return;
    }
    if(res->announce_fb && res->selected >= 0 && res->selected < BT_CODEC_N &&
       res->selected != res->requested){
        char msg[64];
        snprintf(msg, sizeof msg, "Using %s (device limit)", BT_CODEC_LABEL[res->selected]);
        ui_toast(msg);
    }
    if(bt_fw_supports_06b3() && bt_mac_valid(res->mac) &&
       !strcasecmp(ui_route_mac(), res->mac) && res->selected >= 0){
        char frame[48];
        if(bt_codec_frame_value(res->mac, res->selected, frame, sizeof frame) == 0){
            ipc_send_cmd(frame);
        }
    }
    if(res->live_family > 0){
        ui_bt_live_codec_observe(res->mac, res->live_family, res->rate_hz);
    }
    if(screen_current() == SCR_BT_INFO && !strcasecmp(g_sel_mac, res->mac)){
        bt_info_refresh();
    }
    free(res);
}

static void *bt_codec_sync_worker(void *arg){
    bt_codec_sync_arg_t *a = (bt_codec_sync_arg_t *)arg;
    char cmd[384], info[4097];
    bc_snapshot snap = {0};
    int have_pcm = 0;

    if(bt_codec_query_cmd(a->mac, cmd, sizeof cmd) == 0){
        for(int attempt = 0; attempt < 8; attempt++){
            pthread_mutex_lock(&g_codec_sync_mu);
            int alive = (a->gen == g_codec_sync_gen);
            pthread_mutex_unlock(&g_codec_sync_mu);
            if(!alive || ui_get_source_mode() == 2){ free(a); return NULL; }
            int n = bt_probe_run(cmd, info, sizeof info, 700);
            if(n > 0 && bc_parse(info, (size_t)n, a->mac, &snap) == BC_OK && snap.adapter == 0){
                have_pcm = 1;
                break;
            }
            struct timespec ts = { 0, 250000000L };
            nanosleep(&ts, NULL);
        }
    }

    if(!have_pcm){
        free(a);
        return NULL;
    }

    int known = 0;
    unsigned avail = bt_codec_parse_avail(info, &known);
    int best = known ? bt_codec_pick(a->choice, avail, 1) : a->choice;
    const char *best_arg = bt_tx_codec_cli_name(best);

    /* Switch BlueALSA SEP first if it does not match target codec. */
    if(snap.pcm[0] && strcasecmp(snap.codec_name, best_arg) != 0){
        char apply_cmd[320], apply_out[64];
        snprintf(apply_cmd, sizeof apply_cmd,
                 "bluealsa-cli codec '%s' %s >/dev/null 2>&1 && echo OK",
                 snap.pcm, best_arg);
        if(bt_probe_run(apply_cmd, apply_out, sizeof apply_out, 1800) > 0){
            int n2 = bt_probe_run(cmd, info, sizeof info, 700);
            if(n2 > 0){
                bc_snapshot s2 = {0};
                if(bc_parse(info, (size_t)n2, a->mac, &s2) == BC_OK){
                    snap = s2;
                }
            }
        }
    }

    pthread_mutex_lock(&g_codec_sync_mu);
    int still_alive = (a->gen == g_codec_sync_gen);
    pthread_mutex_unlock(&g_codec_sync_mu);
    if(!still_alive || ui_get_source_mode() == 2){
        free(a);
        return NULL;
    }

    bt_codec_sync_res_t *res = (bt_codec_sync_res_t *)calloc(1, sizeof *res);
    if(res){
        res->gen = a->gen;
        snprintf(res->mac, sizeof res->mac, "%s", a->mac);
        res->requested = a->choice;
        res->selected = (snap.codec != 0 && snap.codec != BC_CODEC_OTHER)
                            ? bt_tx_choice_from_codec_name(snap.codec_name, best)
                            : best;
        if(res->selected < 0) res->selected = best;
        res->live_family = (int)snap.codec;
        res->rate_hz = (int)snap.rate;
        res->announce_fb = a->announce_fb;
        if(lv_async_call(bt_codec_sync_done_async, res) != LV_RES_OK){
            free(res);
        }
    }
    free(a);
    return NULL;
}

static void bt_codec_sync_start(const char *mac, int choice, int announce_fb){
    if(!bt_mac_valid(mac) || choice < 0 || choice >= BT_CODEC_N) return;
    bt_codec_sync_arg_t *a = (bt_codec_sync_arg_t *)calloc(1, sizeof *a);
    if(!a) return;
    pthread_mutex_lock(&g_codec_sync_mu);
    a->gen = ++g_codec_sync_gen;
    pthread_mutex_unlock(&g_codec_sync_mu);
    snprintf(a->mac, sizeof a->mac, "%s", mac);
    a->choice = choice;
    a->announce_fb = announce_fb ? 1 : 0;

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if(pthread_create(&th, &attr, bt_codec_sync_worker, a) != 0){
        free(a);
    }
    pthread_attr_destroy(&attr);
}

static int bt_disconnect_job(const char *mac){      /* 0: not submitted (every facility slot is in use) */
    char cmd[128];
    snprintf(cmd, sizeof cmd, "bluetoothctl disconnect %s >/dev/null 2>&1", mac);
    unsigned h = proc_start(cmd, BT_JOB_MS, 16);
    if(!h) return 0;
    bt_job_add(h, JOB_DRAIN, mac);
    return 1;
}
/* Cancel the current attempt: its child group is killed and its result drained. cleanup: also ask the daemon to drop
 * a connect that may still complete late (the killed client cannot take a Connect back). Routing permission is
 * revoked either way: a late Connected or PCM for this MAC must not route. */
static void bt_attempt_cancel(int cleanup){
    if(g_at.stage == AT_IDLE) return;
    if(g_at.h){ proc_cancel(g_at.h); bt_job_add(g_at.h, JOB_DRAIN, NULL); }
    bt_route_revoke(g_at.mac);
    if(cleanup && g_at.stage == AT_CONNECT) bt_disconnect_job(g_at.mac);
    g_at.stage = AT_IDLE; g_at.h = 0;
}
static const char *bt_stage_error(int stage){
    switch(stage){
    case AT_PAIR:    return tr("Pairing failed");
    case AT_TRUST:   return tr("Trust failed");
    case AT_CONNECT: return tr("Connection failed");
    default:         return tr("Audio route failed");
    }
}
static const char *bt_stage_pending_text(int stage){
    switch(stage){
    case 1:  return tr("Pairing...");
    case 2:  return tr("Trusting...");
    default: return tr("Connecting...");
    }
}
static void bt_attempt_fail(void){
    int stage = g_at.stage;
    g_at.stage = AT_IDLE; g_at.h = 0;
    if(!ui_route_mac()[0] && !g_route_auth[0]) g_route_owned = 0;
    ui_toast(bt_stage_error(stage));
    if(screen_current() == SCR_BT && g_list && g_scan_n > 0) scan_render();
    bt_status_note_sync();
    scan_kick();                        /* re-list the current devices, no fresh discovery */
}
/* Each stage is "<verb> then bluetoothctl info" and passes only when info shows the property is "yes": a command's
 * exit status alone is never trusted. Only the verified Connected of the CURRENT attempt authorizes routing. */
static void bt_attempt_step(void){
    static const struct { const char *verb, *prop; int ms; } S[] = {
        {0, 0, 0}, {"pair", "Paired:", BT_PAIR_MS}, {"trust", "Trusted:", BT_TRUST_MS}, {"connect", "Connected:", BT_CONNECT_MS} };
    if(g_at.stage == AT_IDLE) return;
    if(g_at.stage == AT_PAIR && !g_at.h && bt_job_pending(g_at.mac)){
        g_at.begin = lv_tick_get();
        return;
    }
    int left = S[g_at.stage].ms - (int)lv_tick_elaps(g_at.begin);
    if(!g_at.h){                        /* not submitted yet (or no facility slot free): retry until the stage budget ends */
        char cmd[256];
        if(left <= 0){ bt_attempt_fail(); return; }
        if(screen_current() == SCR_BT && g_list){
            if(g_scan_n > 0){
                scan_render();
            } else if(!g_scan_icon){
                const char *msg = g_at.stage == AT_PAIR ? tr("Pairing...") :
                                  g_at.stage == AT_TRUST ? tr("Trusting...") : tr("Connecting...");
                list_msg(msg);
            }
        }
        bt_status_note_sync();
        snprintf(cmd, sizeof cmd, "bluetoothctl %s %s >/dev/null 2>&1; bluetoothctl info %s 2>/dev/null",
                 S[g_at.stage].verb, g_at.mac, g_at.mac);
        g_at.h = proc_start(cmd, left, 2048);
        return;
    }
    char out[2048], v[32];
    int r = proc_poll(g_at.h, out, sizeof out);
    if(!r) return;
    g_at.h = 0;
    if(r != PROC_COMPLETE || !bt_info_prop(out, S[g_at.stage].prop, v, sizeof v) || strcmp(v, "yes")){ bt_attempt_fail(); return; }
    if(g_at.stage < AT_CONNECT){ g_at.stage++; g_at.begin = lv_tick_get(); return; }
    char mac[20];
    snprintf(mac, sizeof mac, "%s", g_at.mac);
    g_at.stage = AT_IDLE;
    bt_scan_mark_connected(mac, 3, 0);
    bt_route_own(mac);
    if(ui_route_bt(mac) == 0){
        snprintf(g_bt_autorouted, sizeof g_bt_autorouted, "%s", mac);
        ui_toast(tr("Connected"));
    } else {
        ui_toast(bt_stage_error(AT_ROUTE));
    }
    if(screen_current() == SCR_BT && g_list && g_scan_n > 0) scan_render();
    bt_status_note_sync();
    scan_kick();
}
/* Details: a pending query shows "Checking..."; any incomplete query (timeout, failure, no slot) shows Unknown. */
static lv_obj_t *g_rename, *g_audio, *g_audio_lbl, *g_info_button, *g_info_button_label;
static int g_info_connected;
static char g_info_name[249];
static void bt_info_apply(const char *raw_buf){
    const char *sep = raw_buf ? strstr(raw_buf, "\n===DISKOS_BA_INFO===\n") : NULL;
    char bt_buf[2048];
    const char *ba_buf = NULL;
    if(sep){
        size_t blen = (size_t)(sep - raw_buf);
        if(blen >= sizeof bt_buf) blen = sizeof bt_buf - 1;
        memcpy(bt_buf, raw_buf, blen);
        bt_buf[blen] = '\0';
        ba_buf = sep + strlen("\n===DISKOS_BA_INFO===\n");
    } else {
        snprintf(bt_buf, sizeof bt_buf, "%s", raw_buf ? raw_buf : "");
    }
    const char *buf = bt_buf;
    char v[249];
    const char *display=
        ((bt_info_prop(buf,"Alias:",v,sizeof v) && v[0]) ||
         (bt_info_prop(buf,"Name:",v,sizeof v) && v[0])) ? v : buf && *buf ? g_sel_mac : tr("Unknown");
    snprintf(g_info_name,sizeof g_info_name,"%s",display);
    if(g_inf_name) lv_label_set_text(g_inf_name,g_info_name);
    int paired=bt_info_prop(buf,"Paired:",v,sizeof v) && !strcmp(v,"yes");
    if(g_rename) { if(paired) lv_obj_remove_state(g_rename,LV_STATE_DISABLED); else lv_obj_add_state(g_rename,LV_STATE_DISABLED); }
    g_info_connected=bt_info_prop(buf,"Connected:",v,sizeof v) && !strcmp(v,"yes");
    int is_routed = !strcasecmp(g_sel_mac, ui_route_mac());
    if(g_audio){
        if(g_info_connected && !is_routed) lv_obj_remove_state(g_audio, LV_STATE_DISABLED);
        else lv_obj_add_state(g_audio, LV_STATE_DISABLED);
    }
    if(g_audio_lbl){
        lv_label_set_text(g_audio_lbl, tr(is_routed ? "Audio active" : "Use for audio"));
        lv_obj_set_style_text_color(g_audio_lbl, is_routed ? TC(STATUS_SUCCESS) : TC(TEXT_PRIMARY), 0);
    }
    if(g_info_button_label){
        lv_label_set_text(g_info_button_label, tr(g_info_connected ? "Disconnect" : "Connect"));
        lv_obj_set_style_text_color(g_info_button_label, g_info_connected ? TC(TEXT_PRIMARY) : TC(STATUS_INFO), 0);
    }
    if(g_info_button){
        lv_obj_set_style_bg_color(g_info_button, g_info_connected ? TC(SURFACE) : TC(CONNECTED_SURFACE), 0);
        if(bt_info_prop(buf,"Connected:",v,sizeof v)) lv_obj_remove_state(g_info_button,LV_STATE_DISABLED);
        else lv_obj_add_state(g_info_button,LV_STATE_DISABLED);
    }
    if(g_inf_conn){
        int has_c = bt_info_prop(buf, "Connected:", v, sizeof v);
        lv_label_set_text(g_inf_conn, has_c ? (!strcmp(v, "yes") ? tr("Yes") : tr("No")) : tr("Unknown"));
        lv_obj_set_style_text_color(g_inf_conn, (has_c && !strcmp(v, "yes")) ? TC(STATUS_SUCCESS) : TC(TEXT_PRIMARY), 0);
    }
    if(g_inf_paired) lv_label_set_text(g_inf_paired, bt_info_prop(buf, "Paired:", v, sizeof v) ? (!strcmp(v, "yes") ? tr("Yes") : tr("No")) : tr("Unknown"));
    if(g_inf_type){
        const char *tlbl = (bt_info_prop(buf, "Icon:", v, sizeof v) && v[0]) ? bt_tx_device_type_label(v) : "-";
        lv_label_set_text(g_inf_type, tr(tlbl));
    }
    if(g_inf_audio){
        const char *albl = is_routed ? tr("Active output") : (g_info_connected ? tr("Connected (idle)") : tr("Disconnected"));
        lv_label_set_text(g_inf_audio, albl);
        lv_obj_set_style_text_color(g_inf_audio,
            is_routed ? TC(STATUS_SUCCESS) : (g_info_connected ? TC(STATUS_INFO) : TC(TEXT_MUTED)), 0);
    }
    char codec_val[64] = "-";
    if(ba_buf && ba_buf[0] && (g_info_connected || is_routed)){
        char *sc = strstr(ba_buf, "Selected codec:");
        if(sc){
            sc += 15;
            while(*sc == ' ' || *sc == '\t') sc++;
            char *eol = strpbrk(sc, "\r\n");
            int clen = eol ? (int)(eol - sc) : (int)strlen(sc);
            if(clen > 0 && clen < 24){
                char cname[32];
                memcpy(cname, sc, (size_t)clen); cname[clen] = '\0';
                char *smp = strstr(ba_buf, "Sampling:");
                int hz = 0;
                if(smp && sscanf(smp + 9, "%d", &hz) == 1 && hz > 0){
                    if(hz % 1000)
                        snprintf(codec_val, sizeof codec_val, "%s (%.1f kHz)", cname, (double)hz / 1000.0);
                    else
                        snprintf(codec_val, sizeof codec_val, "%s (%d kHz)", cname, hz / 1000);
                } else {
                    snprintf(codec_val, sizeof codec_val, "%s", cname);
                }
                if(is_routed){
                    int fam = !strcasecmp(cname, "SBC") ? BC_CODEC_SBC :
                              !strcasecmp(cname, "AAC") ? BC_CODEC_AAC :
                              !strcasecmp(cname, "LDAC") ? BC_CODEC_LDAC :
                              !strcasecmp(cname, "aptX") ? BC_CODEC_APTX :
                              (!strcasecmp(cname, "aptX-HD") || !strcasecmp(cname, "aptX HD")) ? BC_CODEC_APTX_HD : 0;
                    if(fam > 0) ui_bt_live_codec_observe(g_sel_mac, fam, hz);
                }
            }
        }
    }
    if(g_inf_codec) lv_label_set_text(g_inf_codec, codec_val);
}
static void bt_info_cancel(void){
    if(g_info_live_timer){ lv_timer_del(g_info_live_timer); g_info_live_timer = NULL; }
    if(g_inf.h){ proc_cancel(g_inf.h); bt_job_add(g_inf.h, JOB_DRAIN, NULL); }
    g_inf.h = 0; g_inf.on = 0;
}
void bt_info_close(void){ bt_info_cancel(); }
static void bt_info_begin(const char *mac){
    bt_info_cancel();
    char d[24]; int n = 0;
    for(int i = 0; i < 17; i++) d[n++] = mac[i] == ':' ? '_' : mac[i];
    d[n] = '\0';
    snprintf(g_inf.cmd, sizeof g_inf.cmd,
        "bluetoothctl info %s 2>/dev/null; printf '\\n===DISKOS_BA_INFO===\\n'; "
        "p=$(bluealsa-cli list-pcms 2>/dev/null | grep -iE 'dev_%s/(a2dpsrc|a2dp-source)' | head -n 1); "
        "[ -n \"$p\" ] && bluealsa-cli info \"$p\" 2>/dev/null", mac, d);
    g_inf.on = 1; g_inf.begin = lv_tick_get();
    g_inf.h = proc_start(g_inf.cmd, BT_INFO_MS, 4096);
    bt_ops_ensure();
}
static void bt_info_step(void){
    char out[4096];
    if(!g_inf.on) return;
    int left = BT_INFO_MS - (int)lv_tick_elaps(g_inf.begin);
    if(!g_inf.h){                       /* no facility slot yet: retry until the budget ends, then Unknown */
        if(left > 0){ g_inf.h = proc_start(g_inf.cmd, left, sizeof out); return; }
        out[0] = 0;
    } else {
        int r = proc_poll(g_inf.h, out, sizeof out);
        if(!r) return;
        if(r != PROC_COMPLETE) out[0] = 0;
    }
    g_inf.h = 0; g_inf.on = 0;
    bt_info_apply(out);
}
static int bt_ops_busy(void){
    for(int i = 0; i < PROC_SLOTS; i++) if(g_jobs[i].h) return 1;
    return g_at.stage != AT_IDLE || g_inf.on;
}
static void bt_ops_cb(lv_timer_t *t){
    for(int i=0;i<3;i++){
        int before=g_at.stage;bt_attempt_step();
        if(g_at.stage==before || g_at.h || g_at.stage==AT_IDLE)break;
    }
    bt_jobs_step();bt_info_step();
    if(!bt_ops_busy()){if(t)lv_timer_del(t);g_ops_timer=NULL;}
    else if(t){
        if((g_at.stage!=AT_IDLE && !g_at.h) || (g_inf.on && !g_inf.h))lv_timer_resume(t);
        else lv_timer_pause(t);
    }
}
static void bt_ops_ensure(void){ if(!g_ops_timer) g_ops_timer = lv_timer_create(bt_ops_cb, 200, NULL); }
/* ---- end operations ---- */

/* A new attempt (or a cancel) supersedes the old one; pairing can be slow, so each stage has its own budget. */
static void bt_connect(const char *mac){
    if(ui_get_source_mode() == 2) return;
    if(!bt_mac_valid(mac)){ ui_toast(tr("Bad device address")); return; }
    bt_attempt_cancel(strcmp(g_at.mac, mac) != 0);   /* a re-tap of the same device needs no disconnect */
    bt_route_own(NULL);                               /* before submission: nothing is authorized while this is pending */
    snprintf(g_at.mac, sizeof g_at.mac, "%s", mac);
    g_at.stage = AT_PAIR; g_at.h = 0; g_at.begin = lv_tick_get();
    ui_toast(tr("Connecting..."));
    if(screen_current() == SCR_BT && g_list){
        if(g_scan_n > 0) scan_render();
        else list_msg(tr("Connecting..."));
    }
    bt_status_note_sync();
    bt_ops_ensure();
    bt_attempt_step();
}
static void bt_disconnect(const char *mac){
    if(!bt_mac_valid(mac)) return;
    if(!strcmp(mac, g_bt_autorouted)) g_bt_autorouted[0] = 0;
    bt_route_revoke(mac);
    if(g_at.stage != AT_IDLE && !strcmp(g_at.mac, mac)) bt_attempt_cancel(0);
    if(!strcmp(mac, ui_route_mac())) ui_route_analog_pause();
    bt_scan_mark_connected(mac, 0, 1);
    ui_toast(bt_disconnect_job(mac) ? tr("Disconnecting...") : tr("Connection failed"));
    if(screen_current() == SCR_BT && g_list && g_scan_n > 0) scan_render();
    bt_status_note_sync();
}
/* Forget = remove (BlueZ drops the link first). The result toast arrives when the job finishes. */
static void bt_forget(const char *mac){
    if(!bt_mac_valid(mac)) return;
    if(!strcmp(mac, g_bt_autorouted)) g_bt_autorouted[0] = 0;
    bt_route_revoke(mac);
    if(g_at.stage != AT_IDLE && !strcmp(g_at.mac, mac)) bt_attempt_cancel(0);
    if(!strcmp(mac, ui_route_mac())) ui_route_analog_pause();
    char cmd[128];
    snprintf(cmd, sizeof cmd, "bluetoothctl remove %s >/dev/null 2>&1", mac);
    unsigned h = proc_start(cmd, BT_JOB_MS, 16);
    if(!h){ ui_toast(tr("Couldn't forget device")); return; }
    bt_job_add(h, JOB_FORGET, mac);
    bt_status_note_sync();
}
/* Tapping a connected sink is an explicit choice: it replaces any earlier ownership, ends an older attempt and
 * authorizes this MAC. */
static void bt_route_select(const char *mac){
    if(ui_get_source_mode() == 2) return;
    if(bt_job_pending(mac)){
        bt_connect(mac);
        return;
    }
    bt_attempt_cancel(strcmp(g_at.mac, mac) != 0);
    bt_route_own(mac);
    if(ui_route_bt(mac) == 0){
        snprintf(g_bt_autorouted, sizeof g_bt_autorouted, "%s", mac);
        ui_toast(tr("Connected"));
        if(screen_current() == SCR_BT && g_list && g_scan_n > 0) scan_render();
        if(screen_current() == SCR_BT_INFO && !strcasecmp(g_sel_mac, mac)) bt_info_refresh();
    } else {
        ui_toast(bt_stage_error(AT_ROUTE));
    }
    bt_status_note_sync();
}

/* ---- details screen (SCR_BT_INFO) --------------------------------------- */
static void info_row(const char *key, const char *val){
    lv_obj_t *r = lv_obj_create(g_info_list);
    g_row_obj = r;
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 250, 40);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_50, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *k = lv_label_create(r);
    lv_label_set_text(k, key);
    lv_obj_set_pos(k, 12, 11);
    lv_obj_set_style_text_font(k, TF(UI_14), 0);
    lv_obj_set_style_text_color(k, TC(TEXT_MUTED), 0);
    lv_obj_t *v = lv_label_create(r);
    g_row_val = v;
    lv_label_set_text(v, val && val[0] ? val : "-");
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(v, 96, 11); lv_obj_set_size(v, 142, 18);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(v, TF(USER_14), 0);   /* "Name" value = device name: Cyrillic/CJK-capable (issue #3) */
    lv_obj_set_style_text_color(v, TC(TEXT_PRIMARY), 0);
}
/* Forget (unpair + untrust) the selected device, then return to the list + rescan (C13). */
static void info_forget_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    bt_info_cancel();
    bt_forget(g_sel_mac);       /* nonblocking: the result toast follows when the job ends */
    screen_back();
    if(g_scan_timer) lv_timer_del(g_scan_timer);
    g_scan_timer = lv_timer_create(scan_timer_cb, 3000, NULL);
    lv_timer_set_repeat_count(g_scan_timer, 1);
}
static void info_action_row(const char *label, lv_event_cb_t cb){
    lv_obj_t *r = lv_button_create(g_info_list);
    g_row_obj = r;
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 250, 44);
    lv_obj_set_style_radius(r, 8, 0);
    int danger=!strcmp(label,"Forget This Device");
    lv_obj_set_style_bg_color(r, danger ? TC(DANGER_SURFACE) : TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, danger ? TC(DANGER_SURFACE_PRESSED) : TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(r, cb, LV_EVENT_CLICKED, NULL, "bt.cb", UI_CORE);
    lv_obj_t *t = lv_label_create(r);
    g_row_val = t;
    lv_label_set_text(t, label); lv_obj_center(t);
    lv_obj_set_style_text_font(t, TF(UI_14), 0);
    lv_obj_set_style_text_color(t, danger ? TC(STATUS_DANGER) : TC(TEXT_PRIMARY), 0);
}
static void info_codec_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bt_show_codec_dialog();
}
static void info_audio_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED || !g_info_connected) return;
    if(ui_receiver_active()) ui_toast("Return to Local first");
    else if(ui_cast_session_active()) ui_toast("Stop beaming to change this");
    else bt_route_select(g_sel_mac);
}
static void info_rename_cb(lv_event_t *e){
    if(lv_event_get_code(e)==LV_EVENT_CLICKED && g_inf_name)
        dnui_paired(g_sel_mac,g_info_name);
}
static void info_disc_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    bt_info_cancel();
    if(g_info_connected) bt_disconnect(g_sel_mac); else bt_connect(g_sel_mac);
    screen_back();
    /* re-list shortly so the now-disconnected device loses its checkmark / styling
     * (mirrors the connect path; disconnect is async via bluetoothctl). */
    if(g_scan_timer) lv_timer_del(g_scan_timer);
    g_scan_timer = lv_timer_create(scan_timer_cb, 4000, NULL);
    lv_timer_set_repeat_count(g_scan_timer, 1);
}
static void info_back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED){ bt_info_cancel(); screen_back(); } }

void bt_info_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header_cb(root, "Device", info_back_cb);   /* shared header */
    g_info_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_info_list);
    lv_obj_set_pos(g_info_list, 55, 78); lv_obj_set_size(g_info_list, 250, 190);
    lv_obj_set_style_bg_opa(g_info_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(g_info_list, 8, 0);
    lv_obj_set_flex_flow(g_info_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_info_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(g_info_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_info_list, LV_SCROLLBAR_MODE_OFF);

    /* Disconnect / Connect button under the info rows */
    lv_obj_t *db = lv_button_create(root);
    g_info_button=db;
    lv_obj_remove_style_all(db);
    lv_obj_set_pos(db, 188, 278); lv_obj_set_size(db, 108, 38);
    lv_obj_set_ext_click_area(db,6);
    lv_obj_set_style_radius(db, 12, 0);
    lv_obj_set_style_bg_color(db, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(db, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(db, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(db, info_disc_cb, LV_EVENT_CLICKED, NULL, "bt.info_disc", UI_CORE);
    lv_obj_t *dl = lv_label_create(db);
    g_info_button_label=dl;
    lv_label_set_text(dl, "Disconnect");
    lv_obj_center(dl);
    lv_obj_set_style_text_font(dl, TF(UI_14), 0);
    lv_obj_set_style_text_color(dl, TC(TEXT_PRIMARY), 0);
    g_rename=lv_button_create(root); lv_obj_remove_style_all(g_rename);
    lv_obj_set_pos(g_rename,64,278); lv_obj_set_size(g_rename,108,38);
    lv_obj_set_ext_click_area(g_rename,6);
    lv_obj_set_style_radius(g_rename,12,0);
    lv_obj_set_style_bg_color(g_rename,TC(SURFACE),0);
    lv_obj_set_style_bg_opa(g_rename,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(g_rename, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(g_rename,info_rename_cb,LV_EVENT_CLICKED,NULL,"bt.rename",UI_CORE);
    lv_obj_t *rename_label=lv_label_create(g_rename); lv_label_set_text(rename_label,tr("Rename"));
    lv_obj_set_style_text_font(rename_label,TF(UI_14),0);
    lv_obj_set_style_text_color(rename_label,TC(TEXT_PRIMARY),0); lv_obj_center(rename_label);

}

static void bt_info_build(void){
    bt_info_cancel();
    lv_obj_clean(g_info_list);
    g_inf_name = g_inf_conn = g_inf_paired = g_inf_type = g_inf_codec = g_inf_audio = NULL;
    g_audio = g_audio_lbl = NULL;
    g_info_name[0]=0;
    if(g_info_button) lv_obj_add_state(g_info_button,LV_STATE_DISABLED);
    const char *wait = tr("Checking...");   /* the query runs in the background: opening never waits for it */
    info_row("Name", wait);      g_inf_name = g_row_val;
    int lh=lv_font_get_line_height(lv_obj_get_style_text_font(g_inf_name,LV_PART_MAIN));
    int ls=lv_obj_get_style_text_line_space(g_inf_name,LV_PART_MAIN);
    lv_obj_set_height(g_inf_name,2*lh+ls);
    lv_obj_set_height(g_row_obj,2*lh+ls+22);
    info_row("Address", g_sel_mac);
    info_row("Connected", wait); g_inf_conn = g_row_val;
    info_row("Paired", wait);    g_inf_paired = g_row_val;
    info_row("Type", "-");       g_inf_type = g_row_val;
    info_row("Codec", "-");      g_inf_codec = g_row_val;
    info_row("Audio", "-");      g_inf_audio = g_row_val;
    info_action_row(tr("Use for audio"), info_audio_cb);
    g_audio = g_row_obj; g_audio_lbl = g_row_val;
    lv_obj_add_state(g_audio, LV_STATE_DISABLED);
    info_action_row(tr("Output codec"), info_codec_cb);
    if(g_rename) lv_obj_add_state(g_rename,LV_STATE_DISABLED);
    info_action_row("Forget This Device", info_forget_cb);   /* C13: unpair + untrust */
}
static void info_live_timer_cb(lv_timer_t *t){
    (void)t;
    if(screen_current() != SCR_BT_INFO || !ui_screen_backlight_on()){
        return;
    }
    if(!g_inf.on && bt_mac_valid(g_sel_mac)){
        bt_info_begin(g_sel_mac);
    }
}
void bt_info_open(void){
    if(!g_info_list || !bt_mac_valid(g_sel_mac)) return;
    bt_info_build(); screen_show(SCR_BT_INFO); bt_info_begin(g_sel_mac);
    if(!g_info_live_timer) g_info_live_timer = lv_timer_create(info_live_timer_cb, 2000, NULL);
}
#ifdef DISKOS_HOST
void bt_info_fixture(const char *alias,int connected){
    strcpy(g_sel_mac,"AA:BB:CC:DD:EE:FF"); bt_info_build();
    char info[768]; snprintf(info,sizeof info,"Device %s (public)\n\tAlias: %s\n\tName: Original headphones\n\tConnected: %s\n\tPaired: yes\n\tIcon: audio-headphones\n",g_sel_mac,alias,connected?"yes":"no");
    bt_info_apply(info); screen_show(SCR_BT_INFO);
}
#endif

void bt_info_refresh(void){
    if(g_inf_name) lv_label_set_text(g_inf_name,tr("Checking..."));
    bt_info_begin(g_sel_mac);
}

/* ---- device list -------------------------------------------------------- */
static void row_free_cb(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static void dev_cb(lv_event_t *e){
    lv_event_code_t code = lv_event_get_code(e);
    if(code != LV_EVENT_CLICKED && code != LV_EVENT_LONG_PRESSED) return;
    lv_obj_t *row = lv_event_get_target(e);
    const char *mac = (const char*)lv_obj_get_user_data(row);
    int connected = (intptr_t)lv_event_get_user_data(e);
    if(!mac || !mac[0]) return;
    if(code == LV_EVENT_LONG_PRESSED){
        if(connected & 3){
            snprintf(g_sel_mac, sizeof g_sel_mac, "%s", mac);
            bt_info_open();
        }
        return;
    }
    if(connected & 1){
        const char *routed = ui_route_mac();
        if(routed && routed[0] && !strcasecmp(routed, mac)){
            snprintf(g_sel_mac, sizeof g_sel_mac, "%s", mac);
            bt_info_open();
        } else {
            bt_route_select(mac);
        }
    } else if(g_at.stage != AT_IDLE && !strcasecmp(g_at.mac, mac)){
        ui_toast(tr("Connecting..."));
    } else {
        bt_connect(mac);
        if(g_scan_timer){ lv_timer_del(g_scan_timer); g_scan_timer = NULL; }
    }
}

static void add_dev_row(const char *mac, const char *name, int connected){
    int rstate = bt_tx_classify_row(mac, connected, g_at.mac, g_at.stage != AT_IDLE, ui_route_mac());
    int two_line = (rstate != BT_ROW_AVAIL);
    int h = (rstate >= BT_ROW_CONNECTING) ? 54 : (rstate == BT_ROW_PAIRED ? 52 : 46);

    lv_obj_t *r = lv_button_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 268, h);
    lv_obj_set_style_radius(r, 8, 0);
    int hi = (rstate >= BT_ROW_CONNECTING);
    lv_obj_set_style_bg_color(r, hi ? TC(CONNECTED_SURFACE) : TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(r, hi ? LV_OPA_COVER : ((rstate == BT_ROW_PAIRED) ? LV_OPA_70 : LV_OPA_50), 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    char *dup = strdup(mac);
    lv_obj_set_user_data(r, dup);
    ui_on(r, dev_cb, LV_EVENT_CLICKED, (void*)(intptr_t)connected, "bt.dev", UI_CORE);
    if(connected & 3){
        ui_on(r, dev_cb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)connected, "bt.dev.hold", UI_CORE);
    }
    lv_obj_add_event_cb(r, row_free_cb, LV_EVENT_DELETE, NULL);

    int tx = (rstate >= BT_ROW_CONNECTING) ? 34 : 12;
    if(rstate == BT_ROW_ROUTED || rstate == BT_ROW_CONNECTED){
        lv_obj_t *ck = lv_label_create(r);
        lv_label_set_text(ck, LV_SYMBOL_OK);
        lv_obj_set_pos(ck, 11, 18);
        lv_obj_set_style_text_font(ck, TF(UI_14), 0);
        lv_obj_set_style_text_color(ck, (rstate == BT_ROW_ROUTED) ? TC(STATUS_SUCCESS) : TC(STATUS_INFO), 0);
    } else if(rstate == BT_ROW_CONNECTING){
        lv_obj_t *ck = lv_label_create(r);
        lv_label_set_text(ck, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_pos(ck, 12, 18);
        lv_obj_set_style_text_font(ck, TF(UI_14), 0);
        lv_obj_set_style_text_color(ck, TC(STATUS_INFO), 0);
    }

    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, name);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(t, tx, two_line ? 8 : 13);
    lv_obj_set_size(t, 232 - tx, 20);
    lv_obj_set_style_text_font(t, TF(USER_16), 0);   /* BT device names are user data: Cyrillic/CJK-capable (issue #3) */
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);

    if(two_line){
        char sub[64];
        lv_color_t sub_clr = TC(TEXT_MUTED);
        if(rstate == BT_ROW_ROUTED){
            int c = bt_display_codec_choice(ui_route_mac());
            snprintf(sub, sizeof sub, "%s - %s", tr("Audio active"), bt_tx_codec_short_name(c));
            sub_clr = TC(STATUS_SUCCESS);
        } else if(rstate == BT_ROW_CONNECTED){
            snprintf(sub, sizeof sub, "%s", tr("Connected - Tap to use for audio"));
            sub_clr = TC(STATUS_INFO);
        } else if(rstate == BT_ROW_CONNECTING){
            snprintf(sub, sizeof sub, "%s", bt_stage_pending_text(g_at.stage));
            sub_clr = TC(STATUS_INFO);
        } else {
            snprintf(sub, sizeof sub, "%s", tr("Paired - Hold for options"));
            sub_clr = TC(TEXT_MUTED);
        }
        lv_obj_t *s = lv_label_create(r);
        lv_label_set_text(s, sub);
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s, tx, 29);
        lv_obj_set_size(s, 232 - tx, 16);
        lv_obj_set_style_text_font(s, TF(UI_12), 0);
        lv_obj_set_style_text_color(s, sub_clr, 0);
    }

    lv_obj_t *ic = lv_label_create(r);
    lv_label_set_text(ic, (rstate == BT_ROW_ROUTED) ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_BLUETOOTH);
    lv_obj_set_pos(ic, 240, two_line ? 18 : 14);
    lv_obj_set_style_text_font(ic, TF(UI_14), 0);
    lv_obj_set_style_text_color(ic,
        (rstate == BT_ROW_ROUTED) ? TC(STATUS_SUCCESS) :
        ((rstate == BT_ROW_CONNECTED || rstate == BT_ROW_CONNECTING || rstate == BT_ROW_PAIRED) ? TC(STATUS_INFO) : TC(TEXT_MUTED)), 0);
}

/* Show only real AUDIO devices (bluez Icon = audio-card / audio-headset / ...),
 * so BLE gadgets and the "LE-" advertisement entries (which carry no A2DP) are
 * hidden and the user can only tap a connectable sink.  Connected checkmark comes from
 * bluez's own "Connected: yes" (not a raw ACL link, which over-reports). */
/* Enumeration runs OFF the main thread: `bluetoothctl devices` plus a per-device
 * `bluetoothctl info` is up to ~60 blocking popen() spawns - running that inline froze
 * the LVGL loop for seconds (H5) and risked the fiio_init hardware watchdog. The worker
 * builds ONLY plain data; the main thread renders rows from it (LVGL is main-thread-only).
 * A generation counter drops a superseded worker's result if a newer scan started. */
typedef bt_tx_scan_dev_t bt_scan_dev_t;
static pthread_mutex_t g_scan_mu = PTHREAD_MUTEX_INITIALIZER;
static bt_scan_dev_t   g_scan_res[40];
static int             g_scan_done = 0;      /* [g_scan_mu] worker finished -> main renders */
static int             g_scan_running = 0;   /* [g_scan_mu] a worker is in flight */
static unsigned        g_scan_gen = 0;       /* [g_scan_mu] bumped per scan; worker tags its result */
static lv_timer_t     *g_scanpoll_timer;
static int             g_scanpoll_n = 0;
static int             g_scan_pending = 0;   /* main-thread: a re-enumerate was requested while a worker ran */

static void bt_scan_mark_connected(const char *mac, int set_bits, int clear_bits){
    if(!mac || !mac[0]) return;
    pthread_mutex_lock(&g_scan_mu);
    for(int i = 0; i < g_scan_n; i++){
        if(!strcasecmp(g_scan_res[i].mac, mac)){
            g_scan_res[i].connected = (g_scan_res[i].connected & ~clear_bits) | set_bits;
            break;
        }
    }
    pthread_mutex_unlock(&g_scan_mu);
}

static void scan_append_from_list(const char *list_buf, bt_scan_dev_t *res, int *n, int64_t deadline){
    if(!list_buf || !res || !n) return;
    const char *l = list_buf;
    while(l && *l && *n < 40){
        if(bt_probe_now() >= deadline) break;
        const char *nl = strchr(l, '\n');
        size_t llen = nl ? (size_t)(nl - l) : strlen(l);
        if(llen > 7 + 17 && !strncmp(l, "Device ", 7)){
            char mac[20]; memcpy(mac, l + 7, 17); mac[17] = 0;
            if(bt_mac_valid(mac)){
                int dup = 0;
                for(int i = 0; i < *n; i++) if(!strcasecmp(res[i].mac, mac)){ dup = 1; break; }
                if(!dup){
                    size_t nlen = llen - (7 + 17 + 1);
                    char name[249];
                    if(nlen >= sizeof name) nlen = sizeof name - 1;
                    memcpy(name, l + 7 + 17 + 1, nlen);
                    name[nlen] = 0;
                    if(name[0]){
                        char cmd[128], info[2048];
                        snprintf(cmd, sizeof cmd, "bluetoothctl info %s 2>/dev/null", mac);
                        int64_t left = deadline - bt_probe_now();
                        int tmo = left > 450 ? 450 : (int)left;
                        if(tmo <= 0) break;
                        if(bt_probe_run(cmd, info, sizeof info, tmo) > 0){
                            char cv[16], alias[249];
                            if(bt_tx_info_is_audio(info)){
                                snprintf(res[*n].mac, sizeof res[*n].mac, "%s", mac);
                                const char *display = (bt_info_prop(info, "Alias:", alias, sizeof alias) && alias[0]) ||
                                    (bt_info_prop(info, "Name:", alias, sizeof alias) && alias[0]) ? alias : name;
                                snprintf(res[*n].name, sizeof res[*n].name, "%s", display);
                                res[*n].connected = (bt_info_prop(info, "Connected:", cv, sizeof cv) && !strcmp(cv, "yes"));
                                if(bt_info_prop(info, "Paired:", cv, sizeof cv) && !strcmp(cv, "yes")) res[*n].connected |= 2;
                                (*n)++;
                            }
                        }
                    }
                }
            }
        }
        if(!nl) break;
        l = nl + 1;
    }
}

static void *scan_worker(void *arg){
    unsigned my_gen = (unsigned)(intptr_t)arg;
    bt_scan_dev_t res[40]; int n = 0;

    /* Fast path: single D-Bus GetManagedObjects query (typically <100ms, zero PROC_SLOTS usage) */
    char *mbuf = malloc(262144);
    if(mbuf){
        if(bt_probe_run("dbus-send --system --print-reply --dest=org.bluez / "
                        "org.freedesktop.DBus.ObjectManager.GetManagedObjects 2>/dev/null",
                        mbuf, 262144, 2500) > 0){
            n = bt_tx_parse_managed_objects(mbuf, res, 40);
        }
        free(mbuf);
    }

    /* Fallback: bounded bluetoothctl enumeration (paired first, capped at 18s < 30s abandon) */
    if(n == 0){
        int64_t deadline = bt_probe_now() + 18000;
        char paired_buf[4096], dev_buf[8192];
        if(bt_probe_run("bluetoothctl paired-devices 2>/dev/null", paired_buf, sizeof paired_buf, 1000) > 0){
            scan_append_from_list(paired_buf, res, &n, deadline);
        }
        if(n < 40 && bt_probe_now() < deadline &&
           bt_probe_run("bluetoothctl devices 2>/dev/null", dev_buf, sizeof dev_buf, 1000) > 0){
            scan_append_from_list(dev_buf, res, &n, deadline);
        }
    }

    pthread_mutex_lock(&g_scan_mu);
    if(my_gen == g_scan_gen){
        memcpy(g_scan_res, res, (size_t)n * sizeof(bt_scan_dev_t));
        g_scan_n = n; g_scan_done = 1;
        g_scan_running = 0;
    }
    pthread_mutex_unlock(&g_scan_mu);
    ui_notify(UI_WAKE_BT);
    return NULL;
}

static void bt_status_note_sync(void){
    if(!g_bt_note) return;
    if(!bt_control_on() && !bt_on()){
        lv_label_set_text(g_bt_note, tr("Turn on Bluetooth to connect headphones or speakers"));
        lv_obj_set_style_text_color(g_bt_note, TC(TEXT_FOOTNOTE), 0);
        return;
    }
    if(g_radio_timer || g_scanwait_timer){
        lv_label_set_text(g_bt_note, tr("Starting Bluetooth..."));
        lv_obj_set_style_text_color(g_bt_note, TC(STATUS_INFO), 0);
        return;
    }
    if(g_at.stage != AT_IDLE){
        lv_label_set_text(g_bt_note, bt_stage_pending_text(g_at.stage));
        lv_obj_set_style_text_color(g_bt_note, TC(STATUS_INFO), 0);
        return;
    }
    const char *routed = ui_route_mac();
    if(routed && routed[0]){
        int c = bt_display_codec_choice(routed);
        char note_buf[80];
        snprintf(note_buf, sizeof note_buf, "%s (%s)", tr("Audio active"), bt_tx_codec_short_name(c));
        lv_label_set_text(g_bt_note, note_buf);
        lv_obj_set_style_text_color(g_bt_note, TC(STATUS_SUCCESS), 0);
        return;
    }
    pthread_mutex_lock(&g_scan_mu);
    int scanning = g_scan_running || (g_scan_timer != NULL);
    pthread_mutex_unlock(&g_scan_mu);
    if(scanning){
        lv_label_set_text(g_bt_note, tr("Searching for nearby audio devices..."));
        lv_obj_set_style_text_color(g_bt_note, TC(TEXT_MUTED), 0);
    } else {
        lv_label_set_text(g_bt_note, tr("Tap to connect (hold paired for options)"));
        lv_obj_set_style_text_color(g_bt_note, TC(TEXT_FOOTNOTE), 0);
    }
}

/* main thread only: render the worker's results into g_list (LVGL touched only here) */
static void scan_render(void){
    if(!g_list) return;
    scan_stop();
    lv_obj_clean(g_list);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);  /* rows top-aligned */
    static bt_scan_dev_t local[40]; int n;
    pthread_mutex_lock(&g_scan_mu);
    n = g_scan_n; if(n > 40) n = 40;
    memcpy(local, g_scan_res, (size_t)n * sizeof(bt_scan_dev_t));
    int still_scanning = g_scan_running || (g_scan_timer != NULL);
    pthread_mutex_unlock(&g_scan_mu);
    static const int order[] = {
        BT_ROW_ROUTED, BT_ROW_CONNECTED, BT_ROW_CONNECTING, BT_ROW_PAIRED, BT_ROW_AVAIL
    };
    const char *routed = ui_route_mac();
    int wake_autoroute = 0;
    for(size_t o = 0; o < sizeof(order)/sizeof(order[0]); o++){
        for(int i = 0; i < n; i++){
            int cls = bt_tx_classify_row(local[i].mac, local[i].connected,
                                         g_at.mac, g_at.stage != AT_IDLE, routed);
            if(cls == order[o]) add_dev_row(local[i].mac, local[i].name, local[i].connected);
            if(routed && routed[0] && !strcasecmp(local[i].mac, routed) && !(local[i].connected & 1))
                wake_autoroute = 1;
            else if((!routed || !routed[0]) && (local[i].connected & 1) && bt_route_allowed(local[i].mac))
                wake_autoroute = 1;
        }
    }
    if(wake_autoroute && g_bt_autoroute_timer) lv_timer_ready(g_bt_autoroute_timer);
    if(n == 0){
        if(still_scanning) list_msg_scanning();
        else list_msg(tr("No audio devices found"));
    }
    rescan_spin_set(ui_screen_backlight_on() && screen_current() == SCR_BT && still_scanning);
    bt_status_note_sync();
}

static void scanpoll_cb(lv_timer_t *t){
    (void)t;
    int done;
    pthread_mutex_lock(&g_scan_mu); done = g_scan_done; pthread_mutex_unlock(&g_scan_mu);
    if(!done){
        pthread_mutex_lock(&g_scan_mu); g_scan_gen++; g_scan_running = 0; pthread_mutex_unlock(&g_scan_mu);
    }
    if(g_scanpoll_timer){ lv_timer_del(g_scanpoll_timer); g_scanpoll_timer = NULL; }
    if(done) scan_render();
    if(g_scan_pending){
        if(g_at.stage == AT_IDLE){ g_scan_pending = 0; scan_kick(); }
        return;
    }
    if(!done) scan_render();
}

static void scan_ensure_poll(void){
    g_scanpoll_n = 0;
    if(g_scanpoll_timer) lv_timer_del(g_scanpoll_timer);
    g_scanpoll_timer = lv_timer_create(scanpoll_cb, 30000, NULL);
}

static void scan_kick(void){
    if(g_at.stage != AT_IDLE){
        g_scan_pending = 1;
        return;
    }
    pthread_mutex_lock(&g_scan_mu);
    if(g_scan_running){
        pthread_mutex_unlock(&g_scan_mu); g_scan_pending = 1; scan_ensure_poll(); return;
    }
    g_scan_running = 1; g_scan_done = 0; g_scan_gen++;
    unsigned gen = g_scan_gen;
    pthread_mutex_unlock(&g_scan_mu);
    g_scan_pending = 0;
    pthread_t th;
    if(pthread_create(&th, NULL, scan_worker, (void*)(intptr_t)gen) == 0){
        pthread_detach(th);
        scan_ensure_poll();
    } else {
        pthread_mutex_lock(&g_scan_mu); g_scan_running = 0; pthread_mutex_unlock(&g_scan_mu);
        if(g_scanpoll_timer){ lv_timer_del(g_scanpoll_timer); g_scanpoll_timer = NULL; }
        scan_render();
    }
}

static void scan_live_cb(lv_timer_t *t){
    (void)t;
    if(screen_current() != SCR_BT || !ui_screen_backlight_on() || ui_get_source_mode() == 2) return;
    if(g_at.stage == AT_IDLE) scan_kick();
}
static void scan_timer_cb(lv_timer_t *t){
    (void)t;
    if(g_scan_timer){ lv_timer_del(g_scan_timer); g_scan_timer = NULL; }
    if(g_scan_live_timer){ lv_timer_del(g_scan_live_timer); g_scan_live_timer = NULL; }
    rescan_spin_set(0);
    scan_kick();
    bt_status_note_sync();
}
static void scan_abort(void){
    if(g_scan_timer){ lv_timer_del(g_scan_timer); g_scan_timer = NULL; }
    if(g_scan_live_timer){ lv_timer_del(g_scan_live_timer); g_scan_live_timer = NULL; }
    if(g_scanpoll_timer){ lv_timer_del(g_scanpoll_timer); g_scanpoll_timer = NULL; }
    if(g_scanwait_timer){ lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL; }
    g_scan_pending = 0;
    rescan_spin_set(0);
    pthread_mutex_lock(&g_scan_mu);
    g_scan_gen++; g_scan_running = 0; g_scan_n = 0; g_scan_done = 0;
    pthread_mutex_unlock(&g_scan_mu);
}
static void start_scan(void){
    if(!bt_on()){ rescan_spin_set(0); list_msg(tr("Bluetooth is off")); bt_status_note_sync(); return; }
    if(g_at.stage != AT_IDLE) return;
    if(g_scan_n > 0){
        scan_render();
        rescan_spin_set(ui_screen_backlight_on() && screen_current() == SCR_BT);
    } else {
        list_msg_scanning();
    }
    bg_system("bluetoothctl --timeout 13 scan on >/dev/null 2>&1 &");
    if(g_scan_timer) lv_timer_del(g_scan_timer);
    g_scan_timer = lv_timer_create(scan_timer_cb, 13500, NULL);
    lv_timer_set_repeat_count(g_scan_timer, 1);
    if(g_scan_live_timer) lv_timer_del(g_scan_live_timer);
    g_scan_live_timer = lv_timer_create(scan_live_cb, 2000, NULL);
    scan_kick();
    bt_status_note_sync();
}

/* ---- radio toggle ------------------------------------------------------- */
/* bt_enable() is slow (rfkill cycle + patchram firmware download + hci0 bring-up,
 * several seconds). A fixed settle timer would either fire too early ("No audio
 * devices found" before the radio is up) or never actively scan. Poll bt_on()
 * until the adapter is Powered, then run a real scan. */
static uint32_t    g_radio_start;
static void radio_on_poll_cb(lv_timer_t *t){
    if(t != g_radio_timer) return;
    if(ui_get_source_mode() == 2 || cfg_get_int("bt_on", 0) != 1){ lv_timer_del(t); g_radio_timer = NULL; return; }
    bt_stage2_poke();
    if(bt_on()){
        lv_timer_del(g_radio_timer); g_radio_timer = NULL;
        bt_ensure_services();
        bt_autoroute_start();
        start_scan();
        return;
    }
    if(lv_tick_elaps(g_radio_start) > 32000){   /* rfkill cycle + brcm_patchram_plus + daemon/power settle */
        lv_timer_del(g_radio_timer); g_radio_timer = NULL;
        /* bring-up failed: drop the persisted intent so the QS tile + boot-restore don't keep
         * showing/enforcing "on" for a radio that never came up. Tear the stack down so the cleared
         * intent matches a real off state (a late/half-started enable can't linger powered), and
         * sync the screen switch. */
        cfg_set_int("bt_on", 0);
        atomic_store(&g_bt_radio_observed, 0);
        if(g_sw) lv_obj_clear_state(g_sw, LV_STATE_CHECKED);
        bt_disable();
        bt_player_sync_off(1);
        rescan_spin_set(0);
        list_msg(tr("Couldn't turn on Bluetooth"));
        bt_status_note_sync();
    }
}
/* The one Bluetooth power path: the page's switch and the Settings > Network row both call it. An explicit enable
 * replaces the boot-restore observer with its own radio poll (bt_enable); Off cancels both (bt_disable). */
int bt_power_pending(void){ return g_radio_timer != NULL; }   /* a start made here is still waiting for the adapter */
static int bt_power_off_ready(void){
    if(!ui_route_mac()[0] || ui_route_analog_pause() == 0) return 1;
    if(g_sw) lv_obj_add_state(g_sw, LV_STATE_CHECKED);
    ui_toast(tr("Couldn't switch output - try again"));
    return 0;
}
void bt_power_set(int on){
    if(ui_get_source_mode() == 2){
        ui_toast(tr("Exit Bluetooth Receiver first"));
        if(g_sw){
            if(bt_control_on()) lv_obj_add_state(g_sw, LV_STATE_CHECKED);
            else                lv_obj_clear_state(g_sw, LV_STATE_CHECKED);
        }
        return;
    }
    if(!on && !bt_power_off_ready()) return;
    if(on){
        cfg_set_int("bt_on", 1);atomic_store(&g_bt_radio_observed,1);   /* persist intent so boot restores it (mirror WiFi) */
        bt_enable();
        list_msg("Turning on " LV_SYMBOL_BLUETOOTH);
        g_radio_start = lv_tick_get();
        if(g_scanwait_timer){ lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL; }  /* radio poll owns bring-up now - keep the two mutually exclusive */
        if(g_radio_timer) lv_timer_del(g_radio_timer);
        g_radio_timer = lv_timer_create(radio_on_poll_cb, 500, NULL);
        bt_status_note_sync();
    } else {
        cfg_set_int("bt_on", 0);atomic_store(&g_bt_radio_observed,0);   /* OFF before teardown so boot-restore won't resurrect it */
        if(g_radio_timer){ lv_timer_del(g_radio_timer); g_radio_timer = NULL; }
        bt_attempt_cancel(0);  /* a pending attempt must not scan_kick() or route while BT is off */
        scan_abort();          /* cancel any pending/active scan so it can't render over "off" */
        bt_disable();
        bt_player_sync_off(0);
        list_msg(tr("Bluetooth is off"));
        bt_status_note_sync();
    }
    /* a choice made on the Settings row also shows on the page's switch */
    if(g_sw){ if(on) lv_obj_add_state(g_sw, LV_STATE_CHECKED); else lv_obj_clear_state(g_sw, LV_STATE_CHECKED); }
}
static void sw_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_VALUE_CHANGED) return;
    bt_power_set(lv_obj_has_state(g_sw, LV_STATE_CHECKED));
}

/* Quick Settings tile short-press: flip BT + persist intent, no screen-specific UI.
 * Mirrors sw_cb's actions (bt_boot_restore/keepalive enforce the intent). Returns new state, -1 if refused. */
int bt_toggle(void){
    if(ui_get_source_mode() == 2){
        ui_toast(tr("Exit Bluetooth Receiver first"));
        return -1;
    }
    int on = !(bt_power_pending() || bt_control_on());   /* flip the active/pending control state cleanly */
    if(!on && !bt_power_off_ready()) return -1;
    cfg_set_int("bt_on", on);atomic_store(&g_bt_radio_observed,!!on);
    if(on){
        bt_enable();
        g_radio_start = lv_tick_get();
        if(g_scanwait_timer){ lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL; }
        if(g_radio_timer) lv_timer_del(g_radio_timer);
        g_radio_timer = lv_timer_create(radio_on_poll_cb, 500, NULL);
    } else {
        if(g_radio_timer){ lv_timer_del(g_radio_timer); g_radio_timer = NULL; }        /* cancel the bring-up poll too */
        bt_attempt_cancel(0);  /* a pending attempt must not scan_kick() or route while BT is off */
        scan_abort();          /* cancel any pending/active scan */
        bt_disable();
        bt_player_sync_off(0);
    }
    /* keep the BT screen's switch in sync so it reflects reality when opened later */
    if(g_sw){ if(on) lv_obj_add_state(g_sw, LV_STATE_CHECKED); else lv_obj_clear_state(g_sw, LV_STATE_CHECKED); }
    ui_status_refresh();   /* update the home BT icon immediately (don't wait for the next status poll) */
    return on;
}

/* ---- Bluetooth persistence: restore the radio at boot if it was on (mirrors WiFi) -------
 * diskOS owns a "bt_on" intent (cfg), which starts OFF on a fresh install (bt_init_intent below - it is NOT
 * seeded from stock SYSCONFIG.BT_STATUS); the toggle above persists every change. bt_boot_restore() (called at startup) brings the stack up if
 * the intent is on - bluez then auto-reconnects trusted speakers and bt_autoroute picks them
 * up, so BT audio survives reboots with no BT-screen visit. Uses its OWN poll timer (not
 * g_radio_timer) and never scans (no BT screen at boot). */
void bt_init_intent(void){
    if(cfg_get_int("bt_on", -1) >= 0) return;             /* already owned by diskOS - keep the user's choice */
    /* Default OFF on a fresh diskOS: do NOT inherit stock's SYSCONFIG.BT_STATUS. BT on-by-default put
     * users one tap from the freeze bug (now fixed), and most listening here is wired - so a new install
     * starts with Bluetooth off and the user enables it when they want it. */
    cfg_set_int("bt_on", 0);
}
static uint32_t    g_bootrestore_start;
static int         g_bootrestore_observe;
static int         g_rx_resume_allow;
static int         g_rx_reconnect_sent;
static void bootrestore_cancel(void){
    if(g_bootrestore_timer){ lv_timer_del(g_bootrestore_timer); g_bootrestore_timer = NULL; }
}
static void rx_resume_cancel(void){
    if(g_rx_resume_timer){ lv_timer_del(g_rx_resume_timer); g_rx_resume_timer = NULL; }
}
static void bootrestore_poll_cb(lv_timer_t *t){
    if(t != g_bootrestore_timer) return;
    if(ui_get_source_mode() == 2 || cfg_get_int("bt_on", 0) != 1){ bootrestore_cancel(); return; }
    bt_stage2_poke();
    if(bt_on()){
        bootrestore_cancel();
        fprintf(stderr, "bt bootrestore: ready -> ensure services & autoroute\n");
        bt_ensure_services();
        bt_autoroute_start();          /* speaker auto-reconnects -> auto-route routes it */
        ui_status_refresh();
        if(s_rx_resume_out_mac[0] && !g_rx_reconnect_sent && bt_mac_valid(s_rx_resume_out_mac)){
            char target[20];
            snprintf(target, sizeof target, "%s", s_rx_resume_out_mac);
            s_rx_resume_out_mac[0] = 0;
            g_rx_reconnect_sent = 1;
            char cmd[384];
            snprintf(cmd, sizeof cmd,
                "( sleep 1; "
                "  dm=$(echo \"%s\" | tr ':' '_'); "
                "  dbus-send --system --dest=org.bluez \"/org/bluez/hci0/dev_${dm}\" org.bluez.Device1.Connect 2>/dev/null; "
                "  sleep 1; "
                "  bluetoothctl connect %s >/dev/null 2>&1 ) >/dev/null 2>&1 &",
                target, target);
            bg_system(cmd);
        }
        return;
    }
    if(g_bootrestore_observe && !g_bt_probe_pending){
        g_bootrestore_observe=0;
        fprintf(stderr, "bt bootrestore: bt_on=0 -> calling bt_enable()\n");
        bt_enable(); /* cancels this observer before starting bring-up */
        g_bootrestore_start=lv_tick_get();
        g_bootrestore_timer=lv_timer_create(bootrestore_poll_cb,500,NULL);
        return;
    }
    if(lv_tick_elaps(g_bootrestore_start) > 32000){       /* patchram + power-on settle */
        fprintf(stderr, "bt bootrestore: timed out waiting for bt_on()\n");
        bootrestore_cancel();
    }
}
void bt_boot_restore(void){
    bootrestore_cancel();   /* replacement/off/already-up paths must not leave an old observer */
    bt_init_intent();
    if(ui_get_source_mode() == 2) return;                 /* receiver owns the BT controller in mode 2 */
    if(cfg_get_int("bt_on", 0) != 1){
        if(bt_radio_native() == 1){
            bt_disable();
            atomic_store(&g_bt_radio_observed, 0);
            ui_status_refresh();
        }
        bt_player_sync_off(1);
        return;              /* was off -> stay off */
    }
    if(bt_on()){ bt_ensure_services(); bt_autoroute_start(); return; }   /* already up (UI restart) */
    g_bootrestore_observe=g_bt_probe_pending;
    fprintf(stderr, "bt bootrestore: init observe=%d\n", g_bootrestore_observe);
    if(!g_bootrestore_observe)bt_enable();
    g_bootrestore_start = lv_tick_get();
    if(g_bootrestore_timer) lv_timer_del(g_bootrestore_timer);
    g_bootrestore_timer = lv_timer_create(bootrestore_poll_cb, 500, NULL);
}

void bt_output_suspend_for_rx(const char *active_route_mac){
    if(active_route_mac && bt_mac_valid(active_route_mac))
        snprintf(s_rx_resume_out_mac, sizeof s_rx_resume_out_mac, "%s", active_route_mac);
    else
        s_rx_resume_out_mac[0] = 0;
    g_rx_reconnect_sent = 0;

    bt_stage2_cancel();
    rx_resume_cancel();
    bootrestore_cancel();
    if(g_radio_timer){ lv_timer_del(g_radio_timer); g_radio_timer = NULL; }
    ui_bt_route_prepare_cancel();
    bt_codec_async_cancel();
    bt_attempt_cancel(0);
    scan_abort();
    bt_autoroute_stop();
    g_route_owned = 0;
    g_route_auth[0] = 0;
    g_bt_autorouted[0] = 0;
    /* Non-blocking: signal RX mode active, abort any pending bt_enable(), and kill existing
     * TX daemons directly via /proc scan (never sync_system, and never races mq_player's 0657 6). */
    bt_op_next_gen();
    int rfd = open("/tmp/.diskos_bt_rx", O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    if(rfd >= 0) close(rfd);
    unlink("/tmp/bt_enabling");
    bt_avrcp_suspend();
    static const char *const tx_userspace[] = {
        "diskos-avrcpd", "bt-agent", "bluealsa", "bluealsa-custom",
        "bluetoothd", "fiio_bluetoothctl"
    };
    /* Gracefully terminate bluetoothd first so BlueZ closes active TX ACLs cleanly,
     * then SIGKILL stragglers. Keep brcm_patchram_plus + hci0 alive when already attached
     * so mq_player 0657 6 skips firmware re-upload when its chip-init flag is set. */
    bt_signal_procs_by_names(tx_userspace, sizeof tx_userspace / sizeof tx_userspace[0], SIGTERM);
    usleep(80000);
    kill_procs_by_names(tx_userspace, sizeof tx_userspace / sizeof tx_userspace[0]);

    int hci_warm = (access("/sys/class/bluetooth/hci0", F_OK) == 0) && bt_proc_has_comm("brcm_patchram_plus");
    if(hci_warm){
        bt_rfkill_set_sync(1);
    } else {
        static const char *const patchram_proc[] = { "brcm_patchram_plus" };
        kill_procs_by_names(patchram_proc, 1);
        bt_rfkill_set_sync(0);
        usleep(250000);
        bt_rfkill_set_sync(1);
        usleep(100000);
    }
    s_bt_player_may_init = 1;
}

static void rx_resume_timer_cb(lv_timer_t *t){
    if(t == g_rx_resume_timer) g_rx_resume_timer = NULL;
    lv_timer_del(t);
    if(ui_get_source_mode() == 2) return;

    if(g_rx_resume_allow && cfg_get_int("bt_on", 0) == 1){
        g_bt_autorouted[0] = 0;
        g_route_owned = 0;
        g_route_auth[0] = 0;
        g_rx_reconnect_sent = 0;
        if(s_rx_resume_out_mac[0]) ui_toast(tr("Restoring Bluetooth output..."));
        bt_enable();
        g_bootrestore_observe = 0;
        g_bootrestore_start = lv_tick_get();
        bootrestore_cancel();
        g_bootrestore_timer = lv_timer_create(bootrestore_poll_cb, 1000, NULL);
    } else if(!g_rx_resume_allow && cfg_get_int("bt_on", 0) == 1){
        /* User manually turned Bluetooth back on during RX teardown; keep it up. */
        return;
    } else {
        s_rx_resume_out_mac[0] = 0;
        g_rx_reconnect_sent = 0;
        bt_disable();
        bt_player_sync_off(1);
        atomic_store(&g_bt_radio_observed, 0);
        bt_status_note_sync();
        ui_status_refresh();
    }
}

void bt_output_resume_after_rx(int allow_bt_output){
    rx_resume_cancel();
    g_rx_resume_allow = allow_bt_output ? 1 : 0;
    if(!g_rx_resume_allow){
        cfg_set_int("bt_on", 0);
        atomic_store(&g_bt_radio_observed, 0);
        s_rx_resume_out_mac[0] = 0;
        g_rx_reconnect_sent = 0;
        if(g_sw) lv_obj_clear_state(g_sw, LV_STATE_CHECKED);
        bt_status_note_sync();
    }
    g_rx_resume_timer = lv_timer_create(rx_resume_timer_cb, g_rx_resume_allow ? 1200 : 700, NULL);
}

static void rescan_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) start_scan(); }
static void back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

void bt_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header_cb(root, "Bluetooth", back_cb);   /* shared header */

    /* control row: pill with [label + switch]; scan button OUTSIDE, to the
     * right of the switch (still inside the round screen). */
    lv_obj_t *trow = lv_obj_create(root);
    lv_obj_remove_style_all(trow);
    lv_obj_set_pos(trow, 50, 64); lv_obj_set_size(trow, 208, 48);
    lv_obj_set_style_radius(trow, 12, 0);
    lv_obj_set_style_bg_color(trow, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(trow, LV_OPA_70, 0);
    lv_obj_clear_flag(trow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(trow);
    lv_label_set_text(tl, "Bluetooth");
    lv_obj_set_pos(tl, 16, 14);
    lv_obj_set_style_text_font(tl, TF(UI_16), 0);
    lv_obj_set_style_text_color(tl, TC(TEXT_PRIMARY), 0);

    g_sw = lv_switch_create(trow);
    lv_obj_set_size(g_sw, 46, 24);
    lv_obj_set_ext_click_area(g_sw, 10);
    lv_obj_align(g_sw, LV_ALIGN_RIGHT_MID, -14, 0);
    ui_on(g_sw, sw_cb, LV_EVENT_VALUE_CHANGED, NULL, "bt.sw.value", UI_CORE);

    lv_obj_t *rb = lv_button_create(root);
    lv_obj_remove_style_all(rb);
    lv_obj_set_pos(rb, 266, 72); lv_obj_set_size(rb, 36, 32);
    lv_obj_set_ext_click_area(rb, 8);
    lv_obj_set_style_radius(rb, 10, 0);
    lv_obj_set_style_bg_color(rb, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(rb, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(rb, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(rb, rescan_cb, LV_EVENT_CLICKED, NULL, "bt.rescan", UI_CORE);
    lv_obj_t *rl = lv_label_create(rb);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_set_size(rl, 24, 24);
    lv_obj_set_style_text_align(rl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(rl, TF(UI_16), 0);
    lv_obj_set_style_text_color(rl, TC(TEXT_SECONDARY), 0);
    lv_obj_center(rl);
    g_rescan_icon = rl;

    g_bt_note = lv_label_create(root);
    lv_label_set_text(g_bt_note, tr("Tap to connect (hold paired for options)"));
    lv_label_set_long_mode(g_bt_note, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(g_bt_note, 30, 120); lv_obj_set_size(g_bt_note, 300, 16);
    lv_obj_set_style_text_align(g_bt_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_bt_note, TF(UI_12), 0);
    lv_obj_set_style_text_color(g_bt_note, TC(TEXT_FOOTNOTE), 0);

    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 40, 138); lv_obj_set_size(g_list, 280, 180);
    lv_obj_set_style_pad_bottom(g_list, 44, 0);   /* last row scrolls clear of the round bottom bezel */
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(g_list, 6, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    ui_setup_scrollbar(g_list);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

/* Non-destructive "wait for the adapter, then scan" poll used when the BT screen is opened while the
 * radio is on/coming up (from a QS toggle, or brought up externally). Unlike radio_on_poll_cb it NEVER
 * powers BT off on a timeout - it just stops trying, so a flaky bt_on() probe can't kill a live radio. */
static void scanwait_poll_cb(lv_timer_t *t){
    (void)t;
    if(ui_get_source_mode() == 2 || screen_current() != SCR_BT){ /* left BT screen or entered RX -> stop waiting */
        lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL; return;
    }
    bt_stage2_poke();
    if(bt_on()){
        lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL;
        bt_ensure_services(); bt_autoroute_start(); start_scan();
        return;
    }
    if(lv_tick_elaps(g_scanwait_start) > 32000){    /* gave up waiting - do NOT tear the radio down */
        lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL;
        list_msg(bt_radio_on() ? tr("Bluetooth is starting up") : tr("Bluetooth is off"));
    }
}

void bt_open(void){
    if(ui_get_source_mode() == 2){
        bt_show_paired_devices_dialog();
        return;
    }
    /* "should BT be on" = the intent OR the live radio. Intent is the reliable signal right after a QS
     * toggle, because bt_enable() rfkill-blocks-then-unblocks, so bt_radio_on() reads 0 for a moment. */
    int want = bt_control_on();
    if(g_sw){
        if(want) lv_obj_add_state(g_sw, LV_STATE_CHECKED);
        else     lv_obj_clear_state(g_sw, LV_STATE_CHECKED);
    }
    screen_show(SCR_BT);
    /* bt_open is authoritative for this screen entry: cancel any in-flight bring-up observer (sw_cb's radio
     * poll or a prior scanwait) BEFORE deciding a path, so none of the branches below can race a pending
     * timer into a second start_scan() (e.g. reopen after the radio powered up but before g_radio_timer
     * fired would otherwise scan here AND on the next poll tick). */
    if(g_radio_timer){ lv_timer_del(g_radio_timer); g_radio_timer = NULL; }
    if(g_scanwait_timer){ lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL; }
    g_bt_probe_dirty = 1;
    if(bt_on()){                                   /* fully up -> scan immediately */
        const char *routed = ui_route_mac();
        if(routed && routed[0] && !ui_bt_route_prepare_busy() &&
           (g_bt_live_codec < 0 || strcasecmp(routed, g_bt_live_mac) != 0)){
            bt_codec_sync_start(routed, bt_pref_codec_choice(), 0);
        }
        bt_ensure_services(); bt_autoroute_start(); start_scan();
    } else if(want){                                /* on/coming up (e.g. just toggled from Quick Settings) */
        list_msg_scanning();
        g_scanwait_start = lv_tick_get();
        if(g_scanwait_timer) lv_timer_del(g_scanwait_timer);
        g_scanwait_timer = lv_timer_create(scanwait_poll_cb, 500, NULL);
        bt_status_note_sync();
    } else {
        rescan_spin_set(0);
        list_msg(tr("Bluetooth is off"));
        bt_status_note_sync();
    }
}

static int bt_monitor_wanted(void){
    if(ui_get_source_mode() == 2) return 0;
    return cfg_get_int("bt_on",0) || bt_radio_cached() || g_radio_timer ||
        g_bootrestore_timer || g_scanwait_timer || g_bt_autoroute_timer;
}
/* Main-thread delivery. Raw monitor text never drives routing directly. */
void bt_event_deliver(unsigned topics){
    scan_spinner_sync(ui_screen_backlight_on() && screen_current()==SCR_BT);
    static unsigned seen,player;static int receiver=-1;
    if(radio_monitor_quiescing())return;
    int watch=bt_monitor_wanted();radio_monitor_set_active(watch);
    if(watch && radio_monitor_start()<0 && g_bt_autoroute_timer)
        lv_timer_resume(g_bt_autoroute_timer); /* no worker: retain the original observation deadline */
    unsigned revision=radio_monitor_revision(),generation=ipc_generation();
    int rx=ui_receiver_active();
    int changed=revision!=seen || generation!=player || rx!=receiver;
    int rx_or_player_changed = generation!=player || rx!=receiver;
    seen=revision;player=generation;receiver=rx;
    if(rx_or_player_changed){
        bt_capture_drop_except(NULL);
        g_autoroute_wait=0;
        g_bt_probe_dirty=0;
        g_autoroute_dirty=0;
    } else if(changed){
        if(g_bt_probe_pending) g_bt_probe_dirty=1;
        if(g_autoroute_wait) g_autoroute_dirty=1;
    }
    if(g_ops_timer && (changed || (topics&UI_WAKE_BT)))bt_ops_cb(g_ops_timer);
    int done;pthread_mutex_lock(&g_scan_mu);done=g_scan_done;pthread_mutex_unlock(&g_scan_mu);
    if(done && g_scanpoll_timer && ui_screen_backlight_on())scanpoll_cb(g_scanpoll_timer);
    if(watch && (changed || (topics&UI_WAKE_BT)))(void)bt_on();
    if(changed){
        dnui_radio();
        if(g_scanwait_timer)lv_timer_ready(g_scanwait_timer);
        if(screen_current()==SCR_BT && g_at.stage==AT_IDLE){
            bt_status_note_sync();
            static uint32_t s_last_disc_kick;
            if(bt_on() && (!g_scan_timer || g_scan_n == 0 || lv_tick_elaps(s_last_disc_kick) >= 1500)){
                s_last_disc_kick = lv_tick_get();
                scan_kick();
            }
        }
    }
    if(g_bt_autoroute_timer && (changed || g_autoroute_dirty || (g_autoroute_wait && (topics&UI_WAKE_BT))))bt_autoroute_poll_cb(g_bt_autoroute_timer);
}

lv_obj_t *bt_scroller(void){ return g_list; }
