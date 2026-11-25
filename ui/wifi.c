/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "wifi.h"
#include "wifi_logic.h"
#include "i18n.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "fwcaps.h"
#include "ipc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include "ui_wake.h"
#include "procrun.h"

/* Wi-Fi settings (SCR_WIFI) + a details screen (SCR_WIFI_INFO).
 * Radio on/off reuses the stock /usr/bin/wifi_up.sh / wifi_down.sh (rfkill +
 * wpa_supplicant + udhcpc). Scan/connect drive the running supplicant via
 * native supplicant control sockets. Connecting writes the network in and `save_config`s it
 * to /usr/data/wpa_supplicant.conf (update_config=1), so it persists.
 * Round-screen-aware: the top corners stay clear (only back chevron + centred
 * title); the on/off switch + scan button share one row in the wide centre
 * band. The list itself carries state, saved profiles, and hidden-network entry. */

#define WCLI "/usr/sbin/wpa_cli -i wlan0 "
struct wifi_proc { int pid; unsigned long long start; };
struct wifi_obs {
    struct wifi_proc wpa, dhcp;
    int id, ip, dhcp_count, idle, sockets, wpa_completed, allowed;
    char ssid[64], bssid[32];
};

#define WIFI_ATTEMPT_MARKER "/tmp/diskos-wifi-unsaved"
#define WIFI_SAVED_CONFIG "/usr/data/wpa_supplicant.conf"

struct wnet {
    char ssid[64];
    int sig;
    int freq;
    int secured;
    int connected;
    int connecting;
    int saved;
    int in_range;
};
#define WNET_MAX 40

/* The worker owns all supplicant, DHCP and transaction state below. The UI owns
 * objects and config; neither side reads the other's mutable state. */
enum { WIFI_POWER=1, WIFI_CONNECT, WIFI_DISCONNECT, WIFI_FORGET, WIFI_RELOAD };
enum { WIFI_SCAN=1, WIFI_DETAILS=2, WIFI_INPUT=4 };
enum { CONN_PHASE_IDLE=0, CONN_PHASE_ASSOC, CONN_PHASE_AUTH, CONN_PHASE_DHCP };
enum { CONN_ERR_NONE=0, CONN_ERR_WRONG_KEY, CONN_ERR_ASSOC_REJECT };

struct wifi_request {
    unsigned epoch;
    int kind, on;
    char ssid[64], key[64];
    int has_key, force_open, hidden;
};
struct wifi_snapshot {
    unsigned epoch, player, toast_seq;
    int ready, online, seed, reloaded, n, scanning;
    int conn_active, conn_phase;
    char ssid[64], ip[32], conn_ssid[64], toast[128];
    char info[8][64];
    char detail_ssid[64];
    int detail_saved, detail_connected, detail_secured, detail_in_range;
    struct wnet nets[WNET_MAX];
};
static pthread_mutex_t wifi_mu=PTHREAD_MUTEX_INITIALIZER;
static struct wifi_request wifi_queued;
static int wifi_scan_requested;
static uint32_t wifi_scan_retry_due;
static int wifi_has_request,wifi_started,wifi_seed=-1,wifi_pipe[2]={-1,-1};
static unsigned wifi_hints,wifi_worker_epoch=1,wifi_worker_player,wifi_capture_handle;
static atomic_int wifi_desired=1,wifi_page,wifi_lit,wifi_reload_freeze;
static unsigned wifi_reload_requested,wifi_reload_player;
static pid_t g_wifi_launch;
static int g_wifi_launch_grouped;
static int wifi_launch_stop(int off);
static atomic_uint wifi_activity;
static struct wifi_snapshot g_work={.seed=-1,.online=-1},g_mail,g_view={.seed=-1,.online=-1};
static int wifi_mail_ready,g_control_fd=-1,g_monitor_fd=-1,g_netlink_fd=-1,g_rolling_back;
static int g_monitor_ping;static uint32_t g_monitor_due;
static uint32_t wifi_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000);}
static uint32_t wifi_elapsed(uint32_t at){return wifi_now()-at;}
static int wifi_want(void){return atomic_load(&wifi_desired);}
static int wifi_current(void);
static int wifi_socket_open(void);
static int wifi_request_on(int *,const char *,char *,int);
static int wifi_capture(const char *,char *,int,int);
static void wifi_session_end(int *);
static void wifi_rollback_end(int *unused){(void)unused;g_rolling_back=0;}
static void wifi_hint(unsigned);
static void wifi_submit(int,const char *,const char *,int);
static void wifi_lease_step(void);
static int wifi_list_ready(void);
static void scan_fill(void);
static void wifi_pm_sync(int radio_up);

static lv_obj_t *g_sw, *g_list;
static int g_scanning;
static uint32_t g_scan_due;
/* 0: clean, 1: awaiting outcome, 2: rollback required. Never save in 1 or 2. */
static int g_conn_dirty;
static int g_save_pending;  /* verified attempt or sanitation changes awaiting SAVE_CONFIG */
static int g_initial_reload = 1;
static int wifi_discard_attempt(void);
static int wifi_reply_ok(const char *out);
static int g_conn_active;      /* non-zero while a user connect is being polled */
static int g_conn_phase;       /* CONN_PHASE_* */
static int g_conn_err;         /* CONN_ERR_* from monitor events */
static uint32_t g_wifi_last_start = 0;   /* last wifi_up.sh launch (shared: toggle + keepalive) */
static int      g_sanitize_pending = 1;  /* sanitize saved nets once the supplicant is up */
static int      g_wifi_theme_reload;
static int      g_wifi_pm_mode = -1;     /* last applied Broadcom wl PM mode (-1 unknown) */
static atomic_uint g_intent_epoch = 1;   /* bumped by every explicit user Wi-Fi choice */
static char g_pending_ssid[64];          /* SSID awaiting a password or hidden-net key */
static char g_cur_ssid[64];              /* currently-connected SSID */
static lv_obj_t *g_info_list;            /* details screen */

static void start_scan(void);
static void wifi_scan_start(void);
static void wifi_submit_connect(const char *,const char *);
static void wifi_submit_connect_opts(const char *,const char *,int force_open,int hidden);
static void wifi_lease_retry_reset(void);
static void wifi_feedback(const char *text);
static void wifi_info_refresh(void);
static void wifi_info_open_ssid(const char *ssid, int saved, int connected, int secured, int in_range);
extern int ui_screen_backlight_on(void);

/* Spinning icons: g_scan_icon lives in g_list (must stop before lv_obj_clean),
 * g_rescan_icon lives inside the header/control-row refresh button. */
static lv_obj_t *g_scan_icon, *g_rescan_icon;
static int g_rescan_spinning;
static void spin_anim_cb(void *o, int32_t v){ lv_obj_set_style_transform_rotation((lv_obj_t*)o, v, 0); }
static void scan_stop(void){
    if(g_scan_icon){ lv_anim_delete(g_scan_icon, spin_anim_cb); g_scan_icon = NULL; }
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

/* ---- shell helpers ------------------------------------------------------ */
/* Native observations share the control protocol; stock scripts use the bounded worker runner. */

static int g_edit_fd=-1;
static int run_cap(const char *cmd,char *out,int cap){
    if(!strncmp(cmd,WCLI,strlen(WCLI))){
        char request[512]; snprintf(request,sizeof request,"%s",cmd+strlen(WCLI));
        char *redirect=strstr(request," 2>"); if(redirect) *redirect=0;
        for(char *c=request;*c && *c!=' ';c++) if(*c>='a' && *c<='z') *c-=32;
        int *fd=g_edit_fd>=0 ? &g_edit_fd : &g_control_fd;
        if(*fd<0) *fd=wifi_socket_open();
        return wifi_request_on(fd,request,out,cap);
    }
    return wifi_capture(cmd,out,cap,1000);
}

/* Write ahead of ADD/SET/SELECT, so a UI crash cannot hide unsaved daemon state.
 * /tmp is tmpfs on V2.57 (etc/fstab:6); a reboot discards daemon RAM as well.
 * No credentials or IDs belong in this marker. Failure must prevent mutation. */
static int wifi_attempt_mark(void){
    int fd = open(WIFI_ATTEMPT_MARKER, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if(fd < 0) return 0;
    return close(fd) == 0;
}
static int wifi_attempt_clear(void){
    return unlink(WIFI_ATTEMPT_MARKER) == 0 || errno == ENOENT;
}
/* Reload only when an earlier UI left an attempt unsaved. */
static int wifi_initial_reload(void){
    if(!g_initial_reload) return 1;
    struct stat st;
    if(lstat(WIFI_ATTEMPT_MARKER, &st) < 0){
        if(errno != ENOENT) return 0;
    }else{
        char out[64]; run_cap(WCLI "reconfigure 2>/dev/null", out, sizeof out);
        if(!wifi_reply_ok(out) || !wifi_attempt_clear()) return 0;
    }
    g_initial_reload = 0;
    return 1;
}

static void wifi_edit_close(void){
    if(g_edit_fd >= 0) close(g_edit_fd);
    g_edit_fd = -1;
}
static int wifi_edit_open(void){
    wifi_edit_close(); g_edit_fd=wifi_socket_open(); return g_edit_fd>=0;
}
static int wifi_edit_request(const char *cmd,char *out,int cap){
    return wifi_request_on(&g_edit_fd,cmd,out,cap);
}

static int wifi_dhcp_absent(void);

/* Radio-up stays asynchronous. Its children belong to stock, not our probe group. */
static int wifi_shell(const char *cmd){
    if(!wifi_current()) return -1;
    if(g_wifi_launch>0){
        pid_t reaped=waitpid(g_wifi_launch,NULL,WNOHANG);
        if(reaped==g_wifi_launch || (reaped<0 && errno==ECHILD))g_wifi_launch=0;
    }
    if(strstr(cmd,"wifi_down.sh") && !wifi_launch_stop(1))return -1;
    if(strstr(cmd, "wifi_up.sh")){
        int absent = wifi_dhcp_absent();
        if(absent < 0) return -1;
        if(!absent) return wifi_shell("rfkill unblock wifi; ifconfig wlan0 up; "
            "wpa_supplicant -B -i wlan0 -c /usr/data/wpa_supplicant.conf");
        if(g_wifi_launch > 0) return -1;
        struct sigaction chld;
        if(sigaction(SIGCHLD,NULL,&chld) || chld.sa_handler!=SIG_DFL ||
           (chld.sa_flags&SA_NOCLDWAIT))return -1;
        pthread_mutex_lock(&wifi_mu);
        if(!wifi_current()){pthread_mutex_unlock(&wifi_mu);return -1;}
        g_wifi_launch = fork();
        if(g_wifi_launch>0)g_wifi_launch_grouped=setpgid(g_wifi_launch,g_wifi_launch)==0 || (errno==EACCES && getpgid(g_wifi_launch)==g_wifi_launch);
        if(g_wifi_launch != 0)pthread_mutex_unlock(&wifi_mu);
        if(g_wifi_launch == 0){ if(setpgid(0,0))_exit(126); execl("/bin/sh", "sh", "-c", ". /usr/bin/wifi_up.sh; wait", (char *)NULL); _exit(127); }
        return g_wifi_launch < 0 ? -1 : 0;
    }
    char bounded[1024], out[64];
    snprintf(bounded, sizeof bounded, "%s; printf '\nRC=%%d\n' \"$?\"", cmd);
    wifi_capture(bounded, out, sizeof out,
                       strstr(cmd, WCLI "scan ") ? 1700 : 2000);
    return strstr(out, "\nRC=0\n") ? 0 : -1;
}
#define system wifi_shell

/* wpa_cli prints SSIDs (scan_results, list_networks, status) printf-encoded.
 * Decode to the real bytes into out[cap]. */
static int wifi_ssid_decode(const char *in, int inlen, char *out, int cap){
    int o = 0;
    for(int i=0;i<inlen;i++){
        unsigned char c = (unsigned char)in[i], v = c;
        if(c == '\\'){
            if(++i >= inlen) return -1;
            char e = in[i];
            if(e == '\\' || e == '"') v = (unsigned char)e;
            else if(e == 'x'){
                int h = 0;
                for(int k=1;k<=2;k++){
                    if(i + k >= inlen) return -1;
                    char d = in[i+k]; int dv;
                    if(d >= '0' && d <= '9') dv = d - '0';
                    else if(d >= 'a' && d <= 'f') dv = d - 'a' + 10;
                    else if(d >= 'A' && d <= 'F') dv = d - 'A' + 10;
                    else return -1;
                    h = h * 16 + dv;
                }
                v = (unsigned char)h; i += 2;
            }
            else if(e == 'n') v = '\n';
            else if(e == 'r') v = '\r';
            else if(e == 't') v = '\t';
            else if(e == 'e') v = 27;
            else return -1;
        }
        if(v < 0x20 || v == 0x7f) return -1;
        if(o >= 32 || o >= cap - 1) return -1;
        out[o++] = (char)v;
    }
    out[o] = 0;
    return o;
}
static int wifi_ssid_field_eq(const char *fld, int flen, const char *ssid){
    char d[64]; int n = wifi_ssid_decode(fld, flen, d, sizeof d);
    return n >= 0 && (size_t)n == strlen(ssid) && memcmp(d, ssid, (size_t)n) == 0;
}

static int wifi_lease_ready(const char *status);
static int wifi_status(char *ssid, int scap, char *ip, int icap){
    (void)scap; (void)icap;
    ssid[0] = 0; ip[0] = 0;
    char buf[2048];
    if(!run_cap(WCLI "status 2>/dev/null", buf, sizeof buf)) return 0;
    int connected = (strstr(buf, "wpa_state=COMPLETED") != NULL);
    char *l = buf;
    while(l && *l){
        if(!strncmp(l, "ssid=", 5)){
            char *e = strchr(l+5, '\n'); int n = e ? (int)(e - (l+5)) : (int)strlen(l+5);
            if(wifi_ssid_decode(l+5, n, ssid, 64) < 0) ssid[0] = 0;
        }
        else if(!strncmp(l, "ip_address=", 11)){ sscanf(l+11, "%31[^\n]", ip); }
        l = strchr(l, '\n'); if(l) l++;
    }
    return connected && wifi_lease_ready(buf);
}

static int wifi_proc_find(const char *,struct wifi_proc *);
static int wifi_radio_on(void){
    struct wifi_proc p={0}; return wifi_proc_find("wpa_supplicant",&p);
}

/* ---- persistence / auto-reconnect hardening ----------------------------- */
static int wpa_ready(void){
    char buf[64]; run_cap(WCLI "ping 2>/dev/null",buf,sizeof buf);
    return strstr(buf,"PONG")!=NULL;
}
static int wifi_config_writable(void){
    FILE *f = fopen(WIFI_SAVED_CONFIG, "r");
    if(!f) return errno == ENOENT ? 0 : -1;
    char line[512]; int value = 0, v;
    while(fgets(line, sizeof line, f))
        if(sscanf(line, " update_config = %d", &v) == 1) value = v;
    int failed = ferror(f); fclose(f);
    return failed ? -1 : value == 1;
}
static int wifi_network_int(int id, const char *field, int *value){
    char cmd[128], out[64], *end;
    snprintf(cmd, sizeof cmd, WCLI "get_network %d %s 2>/dev/null", id, field);
    run_cap(cmd, out, sizeof out);
    errno = 0;
    long v = strtol(out, &end, 10);
    if(end == out || errno || v < 0 || v > 2147483647L) return 0;
    while(*end == '\r' || *end == '\n') end++;
    if(*end) return 0;
    *value = (int)v; return 1;
}
/* Returns 1 when sanitation is complete. Rollback never writes the saved file. */
static int wifi_sanitize_savenets(void){
    if(!wpa_ready() || !wifi_edit_open()) return 0;
    int session __attribute__((cleanup(wifi_session_end))) = 1;
    (void)session;
    if(g_conn_dirty){
        if(g_conn_active) return 0;
        return wifi_discard_attempt();
    }
    if(!wifi_initial_reload()) return 0;
    int writable = wifi_config_writable();
    if(writable < 0) return 0;
    char out[64];
    run_cap(WCLI "set update_config 1 2>/dev/null", out, sizeof out);
    if(!wifi_reply_ok(out)) return 0;
    if(!writable) g_save_pending = 1;
    char buf[4096];
    int n = run_cap(WCLI "list_networks 2>/dev/null", buf, sizeof buf);
    if(n <= 0 || n >= (int)sizeof buf - 1 || strncmp(buf, "network id", 10) || buf[n-1] != '\n') return 0;
    char *l = strchr(buf, '\n'); if(l) l++;
    int enable = 0;
    while(l && *l){
        int nid, disabled = 0, frequency = 0;
        if(sscanf(l, "%d", &nid) != 1 || nid < 0 ||
           !wifi_network_int(nid, "disabled", &disabled)) return 0;
        if(disabled == 1) enable = 1;
        /* frequency is optional per supplicant build: only clear when confirmed non-zero */
        if(wifi_network_int(nid, "frequency", &frequency) && frequency != 0){
            char cmd[128];
            if(!wifi_attempt_mark()) return 0;
            g_save_pending = 1;
            snprintf(cmd, sizeof cmd, WCLI "set_network %d frequency 0 2>/dev/null", nid);
            run_cap(cmd, out, sizeof out);
            if(!wifi_reply_ok(out)) return 0;
        }
        l = strchr(l, '\n'); if(l) l++;
    }
    if(enable){
        if(!wifi_attempt_mark()) return 0;
        g_save_pending = 1;
        run_cap(WCLI "enable_network all 2>/dev/null", out, sizeof out);
        if(!wifi_reply_ok(out)) return 0;
    }
    if(g_save_pending){
        run_cap(WCLI "save_config 2>/dev/null", out, sizeof out);
        if(!wifi_reply_ok(out)) return 0;
        g_save_pending = 0;
    }
    return wifi_attempt_clear();
}

/* ---- V2.57 radio reconciliation ----------------------------------------- */
#define RECO_SETTLE_MS 9000
#define RECO_GIVEUP_MS 30000
#define RECO_WINDOW_MS 30000
#define RECO_PROBE_MS  1000
static int      g_reco_hold = 0;
static unsigned g_reco_gen  = 0xFFFFFFFFu;
static unsigned g_reco_rx0, g_reco_epoch0;
static uint32_t g_reco_armed_at, g_reco_first_rx_at, g_reco_probe_at, g_reco_eval_at;
static int      g_reco_seen_rx;

static int wifi_reco_decide(int db_wifi, int db_bt, int eff_wifi, int eff_bt, int want_wifi, int user_changed){
    return db_wifi == 0 && db_bt == 1 && eff_wifi == 0 && eff_bt == 1 && want_wifi == 1 && !user_changed;
}
static int read_saved_radios(int *wifi, int *bt){
    char buf[32] = {0};
    ui_run_cap_bounded("sqlite3 /usr/data/fiio/db/sysconfig.db \"SELECT WIFI_STATUS||'|'||BT_STATUS FROM SYSCONFIG WHERE ID=1\" 2>/dev/null",
                       buf, sizeof buf, RECO_PROBE_MS);
    if((buf[0] == '0' || buf[0] == '1') && buf[1] == '|' && (buf[2] == '0' || buf[2] == '1') && (buf[3] == '\n' || buf[3] == 0)){
        *wifi = buf[0] - '0'; *bt = buf[2] - '0'; return 1;
    }
    return 0;
}
static int wifi_rfkill_blocked(void){
    for(int i = 0; i < 12; i++){
        char p[64]; snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/type", i);
        FILE *f = fopen(p, "r"); if(!f) continue;
        char t[16] = {0}; char *r = fgets(t, sizeof t, f); fclose(f);
        if(!r || strncmp(t, "wlan", 4) != 0) continue;
        snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/soft", i);
        f = fopen(p, "r"); if(!f) return -1;
        int soft = -1; if(fscanf(f, "%d", &soft) != 1) soft = -1; fclose(f);
        snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/hard", i);
        f = fopen(p, "r"); if(!f) return -1;
        int hard = -1; if(fscanf(f, "%d", &hard) != 1) hard = -1; fclose(f);
        return soft == 1 || hard == 1 ? 1 : (soft == 0 && hard == 0 ? 0 : -1);
    }
    return -1;
}
static int wifi_effective(void){
    int daemon=wifi_radio_on(),blocked=wifi_rfkill_blocked();
    if(daemon || blocked==0) return 1;
    return blocked==1 ? 0 : -1;
}

int wifi_control_on(void){
    return cfg_get_int("wifi_on", 1) != 0;
}

static void wifi_reconcile_poll(void){
    if(!fw_has_radio_boot_arbitration()){ g_reco_hold = 0; return; }
    unsigned gen = ipc_generation(), rx = ipc_rx_frames();
    uint32_t now = wifi_now();
    int first_generation = 0;
    if(gen != g_reco_gen){
        first_generation = (g_reco_gen == 0xFFFFFFFFu);
        g_reco_gen = gen; g_reco_hold = 1; g_reco_rx0 = rx; g_reco_epoch0 = g_intent_epoch;
        g_reco_armed_at = now; g_reco_seen_rx = 0; g_reco_probe_at = now - 1000; g_reco_eval_at = now - RECO_PROBE_MS;
    }
    if(!g_reco_hold) return;
    if(g_intent_epoch != g_reco_epoch0){ g_reco_hold = 0; return; }
    if(wifi_want() == 0){ g_reco_hold = 0; return; }
    if(first_generation){
        if(g_wifi_theme_reload){ g_reco_hold = 0; return; }
        int dbw, dbb;
        if(read_saved_radios(&dbw, &dbb) && dbb == 0){
            g_reco_hold = 0;
            fprintf(stderr, "radio reconcile (gen %u): saved Bluetooth off, released boot hold\n", gen);
            return;
        }
    }
    if(!g_reco_seen_rx){
        if(rx == g_reco_rx0){
            if(now - g_reco_armed_at >= RECO_GIVEUP_MS){ g_reco_hold = 0; return; }
            if(now - g_reco_probe_at >= 1000){ g_reco_probe_at = now; ipc_send_probe("02020008"); }
            return;
        }
        g_reco_seen_rx = 1; g_reco_first_rx_at = now;
    }
    if(now - g_reco_first_rx_at < RECO_SETTLE_MS) return;
    if(now - g_reco_eval_at < 1000) return;
    g_reco_eval_at = now;
    int dbw = -1, dbb = -1;
    if(!read_saved_radios(&dbw, &dbb)) dbw = dbb = -1;
    int effw = wifi_effective(), effb = bt_radio_probe();
    if(wifi_now() - g_reco_first_rx_at >= RECO_WINDOW_MS){ g_reco_hold = 0; return; }
    if(g_intent_epoch != g_reco_epoch0){ g_reco_hold = 0; return; }
    if(wifi_reco_decide(dbw, dbb, effw, effb, wifi_want(), 0)){
        fprintf(stderr, "radio reconcile (gen %u): stock boot settled, retaining wifi_on=1\n", gen);
        g_reco_hold = 0;
        return;
    }
    if(now - g_reco_first_rx_at >= RECO_WINDOW_MS) g_reco_hold = 0;
}

/* ---- DHCP lease management ---------------------------------------------- */
static wifi_lease_sm_t g_lease_sm;
static struct {
    struct wifi_obs net;
    unsigned epoch, intent, player;
    int explicit_connect;
    uint32_t ready_at;
} g_lease;

static uint32_t g_lease_check, g_lease_retry_ms = 100;
static int g_lease_settled;
static uint32_t g_dhcp_last_spawn;

static int wifi_proc_stat(int pid, struct wifi_proc *p, int *parent){
    char path[64], buf[1024]; snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r"); if(!f) return 0;
    errno = 0;
    char *r = fgets(buf, sizeof buf, f); int err = errno; fclose(f); errno = err;
    char *q = r ? strrchr(buf, ')') : NULL; if(!q) return 0;
    char *save, *t = strtok_r(q + 2, " ", &save); unsigned long long start = 0;
    for(int field = 3; t && field <= 22; field++, t = strtok_r(NULL, " ", &save)){
        if(field == 4) *parent = atoi(t);
        if(field == 22) start = strtoull(t, NULL, 10);
    }
    p->pid = pid; p->start = start; return start != 0;
}
static int wifi_proc_match(int pid, const char *name){
    char path[64], buf[2048]; snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    FILE *f = fopen(path, "r"); if(!f) return errno == ENOENT ? 0 : -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f); int err = ferror(f); fclose(f); buf[n] = 0;
    if(err || n == sizeof buf - 1) return -1;
    if(!n) return 0;
    const char *first = strrchr(buf, '/'); first = first ? first + 1 : buf;
    int app = 0, iface = 0;
    for(size_t i = 0; i < n; ){
        const char *arg = buf + i, *base = strrchr(arg, '/'); base = base ? base + 1 : arg;
        if((i == 0 || (i == strlen(buf) + 1 && !strcmp(first, "busybox"))) && !strcmp(base, name)) app = 1;
        if(!strcmp(arg, "-iwlan0")) iface = 1;
        if(!strcmp(arg, "-i") && i + strlen(arg) + 1 < n && !strcmp(arg + strlen(arg) + 1, "wlan0")) iface = 1;
        i += strlen(arg) + 1;
    }
    return app && iface;
}
static int wifi_proc_find_count(const char *name, struct wifi_proc *p){
    DIR *d = opendir("/proc"); if(!d){ if(p) memset(p, 0, sizeof *p); return -1; }
    struct dirent *e; int count = 0, bad = 0, parent; struct wifi_proc found = {0};
    while((e = readdir(d))){
        int pid = atoi(e->d_name); if(pid <= 0) continue;
        int match = wifi_proc_match(pid, name);
        if(match < 0){ bad = 1; continue; }
        if(!match) continue;
        count++;
        if(count == 1){
            if(!wifi_proc_stat(pid, &found, &parent)) bad = 1;
        }
    }
    closedir(d);
    if(bad || count != 1){ if(p) memset(p, 0, sizeof *p); return bad ? -1 : count; }
    if(p) *p = found;
    return 1;
}
static int wifi_proc_find(const char *name, struct wifi_proc *p){
    return wifi_proc_find_count(name, p) == 1;
}
static int wifi_dhcp_absent(void){
    int c = wifi_proc_find_count("udhcpc", NULL);
    return c < 0 ? -1 : (c == 0 ? 1 : 0);
}
static int wifi_proc_eq(struct wifi_proc a, struct wifi_proc b){ return a.pid && a.pid == b.pid && a.start == b.start; }
static int wifi_lease_signal_context(int sig);
static int wifi_dhcp_signal(struct wifi_proc expected, int sig){
    struct wifi_proc current = {0};
    if(!wifi_current() || !wifi_lease_signal_context(sig) || !wifi_proc_find("udhcpc",&current) || !wifi_proc_eq(expected,current)) return 0;
    pthread_mutex_lock(&wifi_mu);
    int ok=wifi_current() && kill(current.pid,sig)==0;
    pthread_mutex_unlock(&wifi_mu);
    return ok;
}
/* Ensure exactly one detached udhcpc daemon is running for wlan0. */
static void wifi_dhcp_ensure(int count){
    if(!wifi_current() || !wifi_want() || count < 0 || count == 1) return;
    if(g_dhcp_last_spawn && wifi_elapsed(g_dhcp_last_spawn) < 5000) return;
    g_dhcp_last_spawn = wifi_now();
    if(count > 1) system("killall udhcpc >/dev/null 2>&1");
    struct sigaction chld;
    if(sigaction(SIGCHLD, NULL, &chld) || chld.sa_handler != SIG_DFL || (chld.sa_flags & SA_NOCLDWAIT)) return;
    pthread_mutex_lock(&wifi_mu);
    int ok = wifi_current() && wifi_want();
    pid_t pid = ok ? fork() : -1;
    pthread_mutex_unlock(&wifi_mu);
    if(pid == 0){
        pid_t gc = fork();
        if(gc == 0){
            setsid();
            long fd_limit = sysconf(_SC_OPEN_MAX);
            if(fd_limit < 3 || fd_limit > 4096) fd_limit = 256;
            int nul = open("/dev/null", O_RDWR);
            if(nul >= 0){ dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); if(nul > 2) close(nul); }
            for(int fd = 3; fd < fd_limit; fd++) close(fd);
            execlp("udhcpc", "udhcpc", "-i", "wlan0", "-b", "-t", "5", "-T", "2", "-A", "5",
                   "-x", "hostname:SNOWSKY_DISC", (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if(pid > 0) waitpid(pid, NULL, 0);
}
static int wifi_dhcp_idle(struct wifi_proc p, int *sockets){
    char path[64], link[128]; int idle = 1; *sockets = 0;
    DIR *d = opendir("/proc"); if(!d) return 0;
    struct dirent *e; struct wifi_proc child; int parent;
    while((e = readdir(d))){
        int pid = atoi(e->d_name);
        if(pid <= 0) continue;
        errno = 0;
        if(!wifi_proc_stat(pid, &child, &parent)){
            if(errno != ENOENT && errno != ESRCH) idle = 0;
        } else if(parent == p.pid) idle = 0;
    }
    closedir(d); snprintf(path, sizeof path, "/proc/%d/fd", p.pid);
    d = opendir(path); if(!d) return 0;
    while((e = readdir(d))){
        if(e->d_name[0] == '.') continue;
        char fd[128]; snprintf(fd, sizeof fd, "%s/%.32s", path, e->d_name);
        ssize_t n = readlink(fd, link, sizeof link - 1); if(n < 0){ idle = 0; continue; }
        link[n] = 0; if(!strncmp(link, "socket:[", 8)) (*sockets)++;
    }
    closedir(d); return idle;
}
static int wifi_ipv4(void){
    int fd = socket(AF_INET, SOCK_DGRAM, 0); if(fd < 0) return -1;
    struct ifreq req; memset(&req, 0, sizeof req); strcpy(req.ifr_name, "wlan0");
    int flags = ioctl(fd, SIOCGIFFLAGS, &req);
    int up = flags == 0 && (req.ifr_flags & IFF_UP);
    int rc = ioctl(fd, SIOCGIFADDR, &req), err = errno; close(fd);
    if(!up) return -1;
    if(rc < 0) return err == EADDRNOTAVAIL ? 0 : -1;
    return ((struct sockaddr_in *)&req.ifr_addr)->sin_addr.s_addr != 0;
}
static void wifi_lease_observe(struct wifi_obs *o, const char *status){
    memset(o, 0, sizeof *o); o->id = -1; o->ip = wifi_ipv4();
    wifi_proc_find("wpa_supplicant", &o->wpa);
    o->dhcp_count = wifi_proc_find_count("udhcpc", &o->dhcp);
    for(const char *l = status; l && *l; ){
        if(!strncmp(l, "id=", 3)) o->id = atoi(l + 3);
        else if(!strncmp(l, "bssid=", 6)) sscanf(l + 6, "%31[^\n]", o->bssid);
        else if(!strncmp(l, "ssid=", 5)){
            const char *end = strchr(l + 5, '\n');
            if(wifi_ssid_decode(l + 5, end ? end - l - 5 : strlen(l + 5), o->ssid, sizeof o->ssid) < 0) o->ssid[0] = 0;
        }
        l = strchr(l, '\n'); if(l) l++;
    }
    o->wpa_completed = (strstr(status, "\nwpa_state=COMPLETED\n") || !strncmp(status, "wpa_state=COMPLETED\n", 20)) ? 1 : 0;
    o->allowed = !g_reco_hold && wifi_want() && wifi_rfkill_blocked() != 1 &&
        o->wpa.pid && o->ip >= 0 && o->id >= 0 && o->ssid[0] && o->wpa_completed;
    if(o->dhcp_count == 1) o->idle = wifi_dhcp_idle(o->dhcp, &o->sockets);
}
static int wifi_lease_matches(const struct wifi_obs *o){
    return wifi_proc_eq(o->wpa, g_lease.net.wpa) &&
        (o->dhcp_count == 1 && wifi_proc_eq(o->dhcp, g_lease.net.dhcp)) &&
        o->id == g_lease.net.id && !strcmp(o->ssid, g_lease.net.ssid) &&
        !strcmp(o->bssid, g_lease.net.bssid) &&
        g_lease.intent == g_intent_epoch && g_lease.player == ipc_generation();
}
static int wifi_lease_signal_context(int sig){
    char status[2048]; run_cap(WCLI "status 2>/dev/null", status, sizeof status);
    struct wifi_obs o; wifi_lease_observe(&o, status);
    if(!o.allowed || o.dhcp_count != 1 || !wifi_lease_matches(&o)) return 0;
    return sig != SIGUSR1 || o.idle;
}
static int wifi_lease_ready(const char *status){
    if(g_lease_sm.phase != WIFI_LEASE_READY) return 0;
    struct wifi_obs o; wifi_lease_observe(&o, status);
    return o.allowed && o.ip == 1 && o.idle && wifi_lease_matches(&o);
}

static int wifi_lease_slow(void){
    return g_lease_sm.phase == WIFI_LEASE_FAILED && !g_conn_active && !g_scanning;
}
static void wifi_lease_retry_reset(void){
    g_lease_retry_ms = 100; g_lease_check = wifi_now() - 100;
}

/* ---- list placeholders ------------------------------------------------ */
static int g_rendered_valid;
static struct {
    int on, ready, conn_phase, n;
    char ip[32];
    struct wnet nets[WNET_MAX];
} g_rendered_list;

static void list_msg(const char *m){
    if(!g_list) return;
    g_rendered_valid = 0;
    scan_stop();
    lv_obj_clean(g_list);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *e = lv_label_create(g_list);
    lv_label_set_text(e, m);
    lv_obj_set_style_text_color(e, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_font(e, TF(UI_14), 0);
}
static void list_msg_scanning(void){
    if(!g_list) return;
    g_rendered_valid = 0;
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
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, ic);
    lv_anim_set_exec_cb(&a, spin_anim_cb);
    lv_anim_set_values(&a, 0, 3600); lv_anim_set_time(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static int get_gateway(char *out, int cap){
    out[0] = 0;
    FILE *f = fopen("/proc/net/route", "r");
    if(!f) return 0;
    char line[256]; int got = 0;
    if(!fgets(line, sizeof line, f)){ fclose(f); return 0; }
    while(fgets(line, sizeof line, f)){
        char iface[32]; unsigned long dest, gw;
        if(sscanf(line, "%31s %lx %lx", iface, &dest, &gw) == 3 && dest == 0 && gw != 0){
            snprintf(out, cap, "%lu.%lu.%lu.%lu",
                     gw & 0xFF, (gw>>8)&0xFF, (gw>>16)&0xFF, (gw>>24)&0xFF);
            got = 1; break;
        }
    }
    fclose(f);
    return got;
}

static void wpa_q(const char *v, char *out, int cap){
    snprintf(out, cap, "\"%s\"", v);
}
static void wpa_hex(const char *v, char *out, int cap){
    int o = 0;
    for(; *v && o + 2 < cap; v++){ snprintf(out + o, 3, "%02x", (unsigned char)*v); o += 2; }
    out[o] = 0;
}

/* ---- connect completion poll -------------------------------------------- */
static uint32_t    g_conn_start;
static char        g_conn_ssid[64];
static int         g_conn_id = -1;
static struct wifi_proc g_conn_owner;

static int wifi_net_temp_disabled(const char *list, int id){
    const char *l = strchr(list, '\n');
    l = l ? l + 1 : "";
    while(*l){
        const char *nl = strchr(l, '\n'); size_t len = nl ? (size_t)(nl - l) : strlen(l);
        char *end; long v = strtol(l, &end, 10);
        if(end != l && *end == '\t' && v == id && (size_t)(end - l) < len){
            const char *f = end;
            int tabs = 0;
            while(f < l + len && tabs < 3){ if(*f == '\t') tabs++; if(tabs < 3) f++; }
            if(tabs == 3){
                f++;
                size_t rest = (size_t)(l + len - f);
                for(size_t i = 0; i + 15 <= rest; i++)
                    if(!strncmp(f + i, "[TEMP-DISABLED]", 15)) return 1;
            }
        }
        if(!nl) break;
        l = nl + 1;
    }
    return 0;
}

static int wifi_discard_attempt(void){
    if(!g_conn_dirty) return 1;
    g_conn_dirty = 2;
    struct wifi_proc owner = {0};
    int parent;
    int found = wifi_proc_stat(g_conn_owner.pid, &owner, &parent);
    if((found && !wifi_proc_eq(owner, g_conn_owner)) ||
       (!found && (errno == ENOENT || errno == ESRCH))){
        if(!wifi_attempt_clear()) return 0;
        g_conn_dirty = g_save_pending = 0;
        g_initial_reload = 0;
        return 1;
    }
    int cleanup __attribute__((cleanup(wifi_rollback_end))) = 1;
    (void)cleanup; g_rolling_back=1;
    char out[64];
    run_cap(WCLI "reconfigure 2>/dev/null", out, sizeof out);
    if(!wifi_reply_ok(out) || !wifi_attempt_clear()) return 0;
    g_conn_dirty = g_save_pending = 0;
    g_initial_reload = 0;
    return 1;
}
static void wifi_scan_snapshot(void);
static void conn_fail(const char *why){
    g_conn_active = 0;
    g_conn_phase = CONN_PHASE_IDLE;
    g_conn_err = CONN_ERR_NONE;
    g_sanitize_pending = 1;
    if(wifi_discard_attempt()) g_sanitize_pending = 0;
    wifi_feedback(why);
    wifi_scan_snapshot();
}
static void conn_poll_cb(void *t){
    (void)t;
    struct wifi_proc owner = {0};
    if(!wifi_proc_find("wpa_supplicant", &owner) || !wifi_proc_eq(owner, g_conn_owner)){
        conn_fail("Couldn't connect"); return;
    }
    char ss[64], ip[32];
    if(wifi_current() && wifi_status(ss, sizeof ss, ip, sizeof ip) && wifi_current() && ip[0] && g_lease.net.id == g_conn_id && !strcmp(ss, g_conn_ssid)){
        g_conn_active = 0;
        g_conn_phase = CONN_PHASE_IDLE;
        g_conn_err = CONN_ERR_NONE;
        snprintf(g_cur_ssid, sizeof g_cur_ssid, "%s", g_conn_ssid);
        g_conn_dirty = 0;
        g_save_pending = 1;
        g_sanitize_pending = 1;
        int saved = wifi_sanitize_savenets();
        if(saved) g_sanitize_pending = 0;
        wifi_feedback(saved ? "Connected" : "Wi-Fi settings not saved");
        wifi_scan_snapshot();
        return;
    }
    if(g_conn_err == CONN_ERR_WRONG_KEY){
        conn_fail("Wrong password");
        return;
    }
    char nl[2048]; run_cap(WCLI "list_networks 2>/dev/null", nl, sizeof nl);
    if(wifi_net_temp_disabled(nl, g_conn_id)){
        conn_fail("Wrong password");
        return;
    }
    if(g_conn_err == CONN_ERR_ASSOC_REJECT && wifi_elapsed(g_conn_start) > 10000){
        conn_fail("Connection rejected by router");
        return;
    }
    /* Keep conn_phase honest against status polling too */
    char st[2048];
    if(run_cap(WCLI "status 2>/dev/null", st, sizeof st)){
        if(strstr(st, "wpa_state=COMPLETED")) g_conn_phase = CONN_PHASE_DHCP;
        else if(strstr(st, "wpa_state=4WAY_HANDSHAKE") || strstr(st, "wpa_state=GROUP_HANDSHAKE") || strstr(st, "wpa_state=ASSOCIATED"))
            g_conn_phase = CONN_PHASE_AUTH;
    }
    if(wifi_elapsed(g_conn_start) > 45000){
        if(strstr(st, "wpa_state=COMPLETED")) conn_fail("Couldn't get IP address");
        else conn_fail("Couldn't connect");
    }
}

static int wifi_reply_ok(const char *out){
    return out[0] == 'O' && out[1] == 'K' && (out[2] == 0 || out[2] == '\n' || out[2] == '\r');
}
static int wifi_step(const char *cmd, const char *step){
    char out[64]; wifi_edit_request(cmd, out, sizeof out);
    if(wifi_reply_ok(out)) return 0;
    fprintf(stderr, "wifi connect: %s failed (reply '%.20s')\n", step, out);
    return -1;
}
static int wifi_psk_len_ok(const char *key){ size_t n = strlen(key); return n >= 8 && n <= 63; }

static const char *wifi_apply_net(int id, const char *ssid, const char *key, int hidden){
    char cmd[512], qssid[160], qkey[300], out[64];
    /* Best-effort reset of stale channel/BSSID locks and enable hidden SSID probing */
    snprintf(cmd, sizeof cmd, "SET_NETWORK %d frequency 0", id); wifi_edit_request(cmd, out, sizeof out);
    snprintf(cmd, sizeof cmd, "SET_NETWORK %d bssid any", id); wifi_edit_request(cmd, out, sizeof out);
    snprintf(cmd, sizeof cmd, "SET_NETWORK %d scan_ssid %d", id, hidden ? 1 : 1); wifi_edit_request(cmd, out, sizeof out);

    wpa_hex(ssid, qssid, sizeof qssid);
    snprintf(cmd, sizeof cmd, "SET_NETWORK %d ssid %s", id, qssid);
    if(wifi_step(cmd, "set ssid")) return "Couldn't configure Wi-Fi";
    if(key && key[0]){
        wpa_q(key, qkey, sizeof qkey);
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d key_mgmt WPA-PSK", id);
        if(wifi_step(cmd, "set key_mgmt")) return "Couldn't configure Wi-Fi";
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d psk %s", id, qkey);
        if(wifi_step(cmd, "set psk")) return "Couldn't set Wi-Fi password";
    } else {
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d key_mgmt NONE", id);
        if(wifi_step(cmd, "set open")) return "Couldn't configure Wi-Fi";
    }
    snprintf(cmd, sizeof cmd, "ENABLE_NETWORK %d", id);
    if(wifi_step(cmd, "enable_network")) return "Couldn't configure Wi-Fi";
    snprintf(cmd, sizeof cmd, "SELECT_NETWORK %d", id);
    if(wifi_step(cmd, "select_network")) return "Couldn't connect to Wi-Fi";
    return NULL;
}

/* ---- connect ------------------------------------------------------------ */
static void wifi_connect(const char *ssid, const char *key, int force_open, int hidden){
    wifi_lease_retry_reset();
    if(!wifi_list_ready()){
        wifi_feedback("Wi-Fi is starting - try again");
        return;
    }
    wifi_pm_sync(1);
    char buf[4096];
    if(key && key[0] && !wifi_psk_len_ok(key)){ wifi_feedback("Password must be 8 to 63 characters"); return; }
    if(g_conn_active){ g_conn_active = 0; g_conn_phase = CONN_PHASE_IDLE; g_conn_err = CONN_ERR_NONE; }
    if(!wifi_discard_attempt()){ wifi_feedback("Couldn't configure Wi-Fi"); return; }
    if(!wifi_initial_reload() || !wifi_proc_find("wpa_supplicant", &g_conn_owner) || !wifi_edit_open()){
        wifi_feedback("Couldn't configure Wi-Fi"); return;
    }
    struct wifi_proc owner = {0};
    if(!wifi_proc_find("wpa_supplicant", &owner) || !wifi_proc_eq(owner, g_conn_owner)){
        wifi_edit_close(); wifi_feedback("Couldn't configure Wi-Fi"); return;
    }
    int id = -1;
    int n = wifi_edit_request("LIST_NETWORKS", buf, sizeof buf);
    if(n <= 0 || n >= (int)sizeof buf - 1 || strncmp(buf, "network id", 10) || buf[n-1] != '\n'){
        wifi_edit_close(); wifi_feedback("Couldn't configure Wi-Fi"); return;
    }
    char *l = strchr(buf, '\n'); if(l) l++;
    while(l && *l){
        char *nl = strchr(l, '\n');
        char *t1 = strchr(l, '\t');
        if(t1 && (!nl || t1 < nl)){
            char *sfld = t1 + 1;
            char *t2 = strchr(sfld, '\t');
            if(t2 && (!nl || t2 < nl) && wifi_ssid_field_eq(sfld, (int)(t2 - sfld), ssid)){
                id = atoi(l); break;
            }
        }
        l = nl ? nl + 1 : NULL;
    }
    int existing = id >= 0;
    if(!wifi_attempt_mark()){
        wifi_edit_close(); wifi_feedback("Couldn't configure Wi-Fi"); return;
    }
    g_conn_dirty = 1;
    g_sanitize_pending = 1;
    if(id < 0){
        char idbuf[32];
        wifi_edit_request("ADD_NETWORK", idbuf, sizeof idbuf);
        char *end = idbuf;
        long nid = strtol(idbuf, &end, 10);
        if(end == idbuf || nid < 0){
            fprintf(stderr, "wifi add_network failed: '%s'\n", idbuf);
            wifi_edit_close();
            g_sanitize_pending = !wifi_discard_attempt();
            wifi_feedback("Couldn't add network");
            return;
        }
        id = (int)nid;
    }
    const char *why;
    if(!key && !force_open && existing){
        char cmd[64], out[64];
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d frequency 0", id); wifi_edit_request(cmd, out, sizeof out);
        snprintf(cmd, sizeof cmd, "SET_NETWORK %d bssid any", id); wifi_edit_request(cmd, out, sizeof out);
        snprintf(cmd, sizeof cmd, "ENABLE_NETWORK %d", id);
        if(wifi_step(cmd, "enable_network")) why = "Couldn't configure Wi-Fi";
        else {
            snprintf(cmd, sizeof cmd, "SELECT_NETWORK %d", id);
            why = wifi_step(cmd, "select_network") ? "Couldn't connect to Wi-Fi" : NULL;
        }
    }else why = wifi_apply_net(id, ssid, force_open ? NULL : key, hidden);
    wifi_edit_close();
    if(why){ g_sanitize_pending = !wifi_discard_attempt(); wifi_feedback(why); return; }
    snprintf(g_conn_ssid, sizeof g_conn_ssid, "%s", ssid);
    g_conn_id = id;
    g_conn_start = wifi_now();
    g_conn_phase = CONN_PHASE_ASSOC;
    g_conn_err = CONN_ERR_NONE;
    g_conn_active = 1;
    g_lease.explicit_connect = 1;
    wifi_feedback("Connecting...");
    wifi_scan_snapshot();
}

static void wifi_disconnect_now(void){
    g_conn_active = 0;
    g_conn_phase = CONN_PHASE_IDLE;
    g_conn_err = CONN_ERR_NONE;
    if(!wifi_discard_attempt()) g_sanitize_pending = 1;
    if(wpa_ready()){
        char status[2048], out[64];
        int cur_id = -1;
        if(run_cap(WCLI "status 2>/dev/null", status, sizeof status)){
            const char *p = strstr(status, "id=");
            if(p) cur_id = atoi(p + 3);
        }
        if(wifi_edit_open()){
            wifi_edit_request("DISCONNECT", out, sizeof out);
            if(cur_id >= 0){
                char cmd[64];
                snprintf(cmd, sizeof cmd, "DISABLE_NETWORK %d", cur_id);
                wifi_edit_request(cmd, out, sizeof out);
            }
            wifi_edit_close();
        }
    }
    struct wifi_proc dhcp = {0};
    if(wifi_proc_find("udhcpc", &dhcp)) kill(dhcp.pid, SIGUSR2);
    g_lease_sm.phase = WIFI_LEASE_NONE;
    g_lease_sm.seen_empty = 0;
    g_lease_settled = 1;
    g_cur_ssid[0] = 0;
    wifi_lease_retry_reset();
    wifi_feedback("Disconnected");
    wifi_scan_snapshot();
}

static void psk_done(const char *text){ if(text && *text) wifi_submit_connect(g_pending_ssid,text); }
static void hidden_psk_done(const char *text){
    if(!text) return;
    if(!text[0]) wifi_submit_connect_opts(g_pending_ssid, NULL, 1, 1);
    else wifi_submit_connect_opts(g_pending_ssid, text, 0, 1);
}
static void hidden_ssid_done(const char *text){
    if(!text || !text[0]) return;
    snprintf(g_pending_ssid, sizeof g_pending_ssid, "%s", text);
    kbinput_open_password("Password (blank if open)", "", hidden_psk_done);
}

/* ---- details screen (SCR_WIFI_INFO) ------------------------------------- */
static lv_obj_t *g_info_values[8];
static unsigned g_info_count;
static char g_info_ssid[64];
static int g_info_saved, g_info_connected, g_info_secured, g_info_in_range;

static void info_row(const char *key, const char *val){
    lv_obj_t *r = lv_obj_create(g_info_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 250, 40);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_50, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *k = lv_label_create(r);
    lv_label_set_text(k, tr(key));
    lv_obj_set_pos(k, 12, 11);
    lv_obj_set_style_text_font(k, TF(UI_14), 0);
    lv_obj_set_style_text_color(k, TC(TEXT_MUTED), 0);
    lv_obj_t *v = lv_label_create(r);
    g_info_values[g_info_count++] = v;
    lv_label_set_text(v, val && val[0] ? val : "-");
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(v, 102, 11); lv_obj_set_size(v, 136, 18);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(v, TF(USER_14), 0);
    lv_obj_set_style_text_color(v, TC(TEXT_PRIMARY), 0);
}
static void info_back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

void wifi_info_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header_cb(root, "Network", info_back_cb);
    g_info_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_info_list);
    lv_obj_set_pos(g_info_list, 55, 84); lv_obj_set_size(g_info_list, 250, 230);
    lv_obj_set_style_pad_bottom(g_info_list, 44, 0);
    lv_obj_set_style_bg_opa(g_info_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(g_info_list, 8, 0);
    lv_obj_set_flex_flow(g_info_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_info_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(g_info_list, LV_DIR_VER);
    ui_setup_scrollbar(g_info_list);
    lv_obj_add_flag(g_info_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

lv_obj_t *wifi_info_scroller(void){ return g_info_list; }

static void info_action_row(const char *label, theme_color_role_t bg, theme_color_role_t bg_p,
                            theme_color_role_t fg, lv_event_cb_t cb, const char *action){
    lv_obj_t *r = lv_button_create(g_info_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 250, 44);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, theme_color(bg), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, theme_color(bg_p), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(r, cb, LV_EVENT_CLICKED, NULL, action, UI_CORE);
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, label); lv_obj_center(t);
    lv_obj_set_style_text_font(t, TF(UI_14), 0);
    lv_obj_set_style_text_color(t, theme_color(fg), 0);
}

static int wifi_remove_ssid(const char *ssid){
    if(g_conn_active){ g_conn_active = 0; g_conn_phase = CONN_PHASE_IDLE; g_conn_err = CONN_ERR_NONE; }
    if(g_conn_dirty){
        g_sanitize_pending = !wifi_discard_attempt();
        return 0;
    }
    wifi_lease_retry_reset();
    if(!wpa_ready() || !ssid || !ssid[0] || !wifi_initial_reload()) return 0;
    if(!wifi_edit_open()) return 0;
    char buf[4096];
    int n = wifi_edit_request("LIST_NETWORKS", buf, sizeof buf);
    if(n <= 0 || n >= (int)sizeof buf - 1 || strncmp(buf, "network id", 10) || buf[n-1] != '\n'){
        wifi_edit_close(); return 0;
    }
    if(!wifi_attempt_mark()){wifi_edit_close();return 0;}
    char *l = strchr(buf, '\n'); if(l) l++;
    int removed = 0, failed = 0;
    while(l && *l){
        char *nl = strchr(l, '\n');
        char *t1 = strchr(l, '\t');
        if(t1 && (!nl || t1 < nl)){
            char *sfld = t1 + 1;
            char *t2 = strchr(sfld, '\t');
            if(t2 && (!nl || t2 < nl) && wifi_ssid_field_eq(sfld, (int)(t2 - sfld), ssid)){
                int nid = atoi(l);
                char cmd[80]; snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %d", nid);
                char out[64]; wifi_edit_request(cmd, out, sizeof out);
                if(strstr(out, "OK")) removed = 1; else failed = 1;
            }
        }
        l = nl ? nl + 1 : NULL;
    }
    if(removed){
        char so[64]; wifi_edit_request("SAVE_CONFIG", so, sizeof so);
        if(!strstr(so, "OK")) failed = 1;
    }
    wifi_edit_close();
    if(!failed && !wifi_attempt_clear()) failed=1;
    if(failed) g_initial_reload=1;
    if(removed && !strcmp(ssid, g_cur_ssid)) g_cur_ssid[0] = 0;
    return removed && !failed;
}
static void wifi_forget_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    wifi_submit(WIFI_FORGET,g_info_ssid,NULL,-1);
    screen_return_to(SCR_WIFI); wifi_open();
}
static void wifi_disconnect_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    wifi_submit(WIFI_DISCONNECT, g_info_ssid, NULL, -1);
    screen_return_to(SCR_WIFI); wifi_open();
}
static void wifi_connect_saved_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    wifi_submit_connect(g_info_ssid, NULL);
    screen_return_to(SCR_WIFI); wifi_open();
}
static void chpass_done(const char *text){
    if(text && *text){
        wifi_submit_connect(g_pending_ssid, text);
        screen_return_to(SCR_WIFI); wifi_open();
    }
}
static void wifi_chpass_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    snprintf(g_pending_ssid, sizeof g_pending_ssid, "%s", g_info_ssid);
    char prompt[80]; snprintf(prompt, sizeof prompt, "Password for %s", g_info_ssid);
    kbinput_open_password(prompt, "", chpass_done);
}
static void info_value(unsigned row, const char *value){
    if(row >= g_info_count || !g_info_values[row]) return;
    const char *text = value && value[0] ? value : "-";
    if(strcmp(lv_label_get_text(g_info_values[row]), text)) lv_label_set_text(g_info_values[row], text);
}
static void wifi_info_build(void){
    if(!g_info_list) return;
    lv_obj_clean(g_info_list);
    g_info_count = 0;
    memset(g_info_values, 0, sizeof g_info_values);

    info_row("Status", "");
    info_row("Network", g_info_ssid);
    info_row("IP Address", "");
    info_row("Router", "");
    info_row("Signal", "");
    info_row("Security", "");
    info_row("Frequency", "");
    info_row("BSSID", "");

    if(g_info_connected){
        info_action_row(tr("Disconnect"), THEME_CLR_SURFACE, THEME_CLR_SURFACE_PRESSED, THEME_CLR_TEXT_PRIMARY, wifi_disconnect_cb, "wifi.disconnect");
        if(g_info_secured)
            info_action_row(tr("Change Password"), THEME_CLR_SURFACE, THEME_CLR_SURFACE_PRESSED, THEME_CLR_TEXT_PRIMARY, wifi_chpass_cb, "wifi.chpass");
    } else if(g_info_saved){
        if(g_info_in_range)
            info_action_row(tr("Connect"), THEME_CLR_CONNECTED_SURFACE, THEME_CLR_SURFACE_PRESSED, THEME_CLR_STATUS_INFO, wifi_connect_saved_cb, "wifi.connect_saved");
        info_action_row(tr("Change Password"), THEME_CLR_SURFACE, THEME_CLR_SURFACE_PRESSED, THEME_CLR_TEXT_PRIMARY, wifi_chpass_cb, "wifi.chpass");
    }
    if(g_info_saved || g_info_connected)
        info_action_row(tr("Forget This Network"), THEME_CLR_DANGER_SURFACE, THEME_CLR_DANGER_SURFACE_PRESSED, THEME_CLR_STATUS_DANGER, wifi_forget_cb, "wifi.cb");
}
static void wifi_info_refresh(void){
    if(!g_info_list || !g_info_count) return;
    int live_conn = wifi_control_on() && g_view.online == 1 && !strcmp(g_view.ssid, g_info_ssid);
    if(live_conn != g_info_connected){
        g_info_connected = live_conn;
        if(live_conn) g_info_in_range = 1;
        wifi_info_build();
    }
    if(live_conn){
        info_value(0, tr("Connected"));
        info_value(1, g_info_ssid);
        for(unsigned i = 2; i < 8; i++) info_value(i, g_view.info[i]);
    } else {
        const struct wnet *found = NULL;
        for(int i = 0; i < g_view.n; i++){
            if(!strcmp(g_view.nets[i].ssid, g_info_ssid)){ found = &g_view.nets[i]; break; }
        }
        int in_range = found ? found->in_range : g_info_in_range;
        int connecting = found ? found->connecting : (g_view.conn_active && !strcmp(g_view.conn_ssid, g_info_ssid));
        info_value(0, connecting ? tr("Connecting...") : (in_range ? tr("Saved") : tr("Not in range")));
        info_value(1, g_info_ssid);
        info_value(2, "");
        info_value(3, "");
        char sigbuf[48] = {0}, freqbuf[48] = {0};
        if(found && found->in_range){
            wifi_format_signal(found->sig, sigbuf, sizeof sigbuf);
            wifi_format_freq(found->freq, freqbuf, sizeof freqbuf);
        }
        info_value(4, sigbuf);
        info_value(5, (found ? found->secured : g_info_secured) ? "WPA/WPA2" : "Open");
        info_value(6, freqbuf);
        info_value(7, "");
    }
}
static void wifi_info_open_ssid(const char *ssid, int saved, int connected, int secured, int in_range){
    snprintf(g_info_ssid, sizeof g_info_ssid, "%s", ssid ? ssid : "");
    g_info_saved = saved;
    g_info_connected = connected;
    g_info_secured = secured;
    g_info_in_range = in_range;
    wifi_hint(WIFI_DETAILS);
    wifi_info_build();
    wifi_info_refresh();
    screen_show(SCR_WIFI_INFO);
}
void wifi_info_open(void){
    int secured = 1;
    for(int i = 0; i < g_view.n; i++){
        if(!strcmp(g_view.nets[i].ssid, g_view.ssid)){ secured = g_view.nets[i].secured; break; }
    }
    wifi_info_open_ssid(g_view.ssid, 1, 1, secured, 1);
}

/* ---- scan list ---------------------------------------------------------- */
enum {
    NET_F_SECURED   = 1 << 0,
    NET_F_CONNECTED = 1 << 1,
    NET_F_SAVED     = 1 << 2,
    NET_F_IN_RANGE  = 1 << 3,
    NET_F_CONNECTING= 1 << 4,
};
static void row_free_cb(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static void net_cb(lv_event_t *e){
    lv_event_code_t code = lv_event_get_code(e);
    if(code != LV_EVENT_CLICKED && code != LV_EVENT_LONG_PRESSED) return;
    lv_obj_t *row = lv_event_get_target(e);
    const char *ssid = (const char*)lv_obj_get_user_data(row);
    intptr_t flags = (intptr_t)lv_event_get_user_data(e);
    int secured   = (flags & NET_F_SECURED) != 0;
    int connected = (flags & NET_F_CONNECTED) != 0;
    int saved     = (flags & NET_F_SAVED) != 0;
    int in_range  = (flags & NET_F_IN_RANGE) != 0;
    if(!ssid || !ssid[0]) return;
    if(!wifi_control_on()) return;

    if(code == LV_EVENT_LONG_PRESSED){
        if(connected || saved) wifi_info_open_ssid(ssid, saved, connected, secured, in_range);
        return;
    }
    if(connected){
        wifi_info_open_ssid(ssid, 1, 1, secured, 1);
    } else if(saved && !in_range){
        wifi_info_open_ssid(ssid, 1, 0, secured, 0);
    } else if(saved){
        wifi_submit_connect(ssid, NULL);
    } else if(secured){
        snprintf(g_pending_ssid, sizeof g_pending_ssid, "%s", ssid);
        char prompt[80]; snprintf(prompt, sizeof prompt, "Password for %s", ssid);
        kbinput_open_password(prompt, "", psk_done);
    } else {
        wifi_submit_connect_opts(ssid, NULL, 1, 0);
    }
}
static void join_other_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if(!wifi_control_on()) return;
    kbinput_open_limited("Network name (SSID)", "", hidden_ssid_done, 32);
}

static void add_signal_bars(lv_obj_t *parent, int x, int y, int sig){
    int bars = sig >= -55 ? 4 : sig >= -67 ? 3 : sig >= -78 ? 2 : 1;
    static const int h[4] = {5, 9, 13, 17};
    for(int i=0;i<4;i++){
        lv_obj_t *b = lv_obj_create(parent);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 4, h[i]);
        lv_obj_set_pos(b, x + i*6, y + (17 - h[i]));
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, (i < bars ? TC(SIGNAL_ACTIVE) : TC(SIGNAL_INACTIVE)), 0);
    }
}

static const char *conn_phase_text(int phase){
    switch(phase){
    case CONN_PHASE_AUTH: return tr("Authenticating...");
    case CONN_PHASE_DHCP: return tr("Obtaining IP...");
    default:              return tr("Connecting...");
    }
}

static void add_net_row(const struct wnet *net, const char *ip, int conn_phase){
    int h = (net->connected || net->connecting) ? 54 : (net->saved ? 52 : 46);
    lv_obj_t *r = lv_button_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 268, h);
    lv_obj_set_style_radius(r, 8, 0);
    int hi = net->connected || net->connecting;
    lv_obj_set_style_bg_color(r, (hi ? TC(CONNECTED_SURFACE) : TC(SURFACE)), 0);
    lv_obj_set_style_bg_opa(r, hi ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    char *dup = strdup(net->ssid);
    lv_obj_set_user_data(r, dup);
    intptr_t flags = (net->secured ? NET_F_SECURED : 0) |
                     (net->connected ? NET_F_CONNECTED : 0) |
                     (net->saved ? NET_F_SAVED : 0) |
                     (net->in_range ? NET_F_IN_RANGE : 0) |
                     (net->connecting ? NET_F_CONNECTING : 0);
    ui_on(r, net_cb, LV_EVENT_CLICKED, (void*)flags, "wifi.net", UI_CORE);
    if(net->saved && !net->connected)
        ui_on(r, net_cb, LV_EVENT_LONG_PRESSED, (void*)flags, "wifi.net.hold", UI_CORE);
    lv_obj_add_event_cb(r, row_free_cb, LV_EVENT_DELETE, NULL);

    int tx = (net->connected || net->connecting) ? 34 : 12;
    int right_limit = net->connected ? 182 : (net->in_range ? 196 : 244);

    /* Create the SSID label FIRST so theme_kit classifies it as KIT_ROW_TEXT. */
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, net->ssid);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(t, tx, (net->connected || net->connecting || net->saved) ? 8 : 13);
    lv_obj_set_size(t, right_limit - tx, 20);
    lv_obj_set_style_text_font(t, TF(USER_16), 0);
    lv_obj_set_style_text_color(t, net->in_range ? TC(TEXT_PRIMARY) : TC(TEXT_SECONDARY), 0);

    if(net->connected || net->connecting){
        char sub[80];
        if(net->connected){
            if(ip && ip[0]) snprintf(sub, sizeof sub, "%s - %s", tr("Connected"), ip);
            else snprintf(sub, sizeof sub, "%s", tr("Connected"));
        } else {
            snprintf(sub, sizeof sub, "%s", conn_phase_text(conn_phase));
        }
        lv_obj_t *s = lv_label_create(r);
        lv_label_set_text(s, sub);
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s, tx, 29);
        lv_obj_set_size(s, right_limit - tx, 16);
        lv_obj_set_style_text_font(s, TF(UI_12), 0);
        lv_obj_set_style_text_color(s, net->connected ? TC(STATUS_SUCCESS) : TC(STATUS_INFO), 0);

        lv_obj_t *ck = lv_label_create(r);
        lv_label_set_text(ck, net->connected ? LV_SYMBOL_OK : LV_SYMBOL_WIFI);
        lv_obj_set_pos(ck, 11, 18);
        lv_obj_set_style_text_font(ck, TF(UI_14), 0);
        lv_obj_set_style_text_color(ck, net->connected ? TC(STATUS_SUCCESS) : TC(STATUS_INFO), 0);
    } else if(net->saved){
        lv_obj_t *s = lv_label_create(r);
        lv_label_set_text(s, net->in_range ? tr("Saved - Hold for details") : tr("Not in range"));
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s, tx, 28);
        lv_obj_set_size(s, right_limit - tx, 16);
        lv_obj_set_style_text_font(s, TF(UI_12), 0);
        lv_obj_set_style_text_color(s, net->in_range ? TC(STATUS_INFO) : TC(TEXT_MUTED), 0);
    }

    int pad_x = net->connected ? 200 : 216;
    int bar_x = net->connected ? 216 : 232;
    int bar_y = h / 2 - 9;

    if(net->secured && net->in_range){
        lv_obj_t *shk = lv_obj_create(r);
        lv_obj_remove_style_all(shk);
        lv_obj_clear_flag(shk, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(shk, 8, 8); lv_obj_set_pos(shk, pad_x + 1, bar_y + 1);
        lv_obj_set_style_radius(shk, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(shk, 2, 0);
        lv_obj_set_style_border_color(shk, TC(TEXT_MUTED), 0);
        lv_obj_t *body = lv_obj_create(r);
        lv_obj_remove_style_all(body);
        lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(body, 10, 8); lv_obj_set_pos(body, pad_x, bar_y + 6);
        lv_obj_set_style_radius(body, 2, 0);
        lv_obj_set_style_bg_color(body, TC(TEXT_MUTED), 0);
        lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    }
    if(net->in_range) add_signal_bars(r, bar_x, bar_y, net->sig);
    if(net->connected){
        lv_obj_t *ch = lv_label_create(r);
        lv_label_set_text(ch, LV_SYMBOL_RIGHT);
        lv_obj_set_pos(ch, 248, h / 2 - 8);
        lv_obj_set_style_text_font(ch, TF(UI_14), 0);
        lv_obj_set_style_text_color(ch, TC(TEXT_MUTED), 0);
    }
}

static void add_join_other_row(void){
    lv_obj_t *r = lv_button_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 268, 44);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(r, TC(RAISED_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(r, join_other_cb, LV_EVENT_CLICKED, NULL, "wifi.join_other", UI_CORE);
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, tr_sym(LV_SYMBOL_PLUS, "Join Other Network"));
    lv_obj_center(t);
    lv_obj_set_style_text_font(t, TF(UI_14), 0);
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);
}

#define WNET_NO_RSSI (-1000)

static int net_cmp(const void *a, const void *b){
    const struct wnet *x = (const struct wnet *)a;
    const struct wnet *y = (const struct wnet *)b;
    if(x->connected != y->connected) return y->connected - x->connected;
    if(x->connecting != y->connecting) return y->connecting - x->connecting;
    if(x->in_range != y->in_range) return y->in_range - x->in_range;
    if(x->saved != y->saved) return y->saved - x->saved;
    return y->sig - x->sig;
}

static int wifi_scan_order(char *buf, char *saved_list, const char *cur, int cur_rssi, int cur_freq, int cur_secured,
                           const char *conn_ssid, struct wnet *out){
    char saved_ssids[WNET_MAX][64];
    int n_saved = 0;
    if(saved_list && saved_list[0]){
        char *l = strchr(saved_list, '\n'); if(l) l++;
        while(l && *l && n_saved < WNET_MAX){
            char *nl = strchr(l, '\n');
            char *t1 = strchr(l, '\t');
            if(t1 && (!nl || t1 < nl)){
                char *sfld = t1 + 1;
                char *t2 = strchr(sfld, '\t');
                if(t2 && (!nl || t2 < nl)){
                    char ds[64];
                    if(wifi_ssid_decode(sfld, (int)(t2 - sfld), ds, sizeof ds) > 0){
                        int dup = 0;
                        for(int i = 0; i < n_saved; i++) if(!strcmp(saved_ssids[i], ds)){ dup = 1; break; }
                        if(!dup) snprintf(saved_ssids[n_saved++], 64, "%s", ds);
                    }
                }
            }
            l = nl ? nl + 1 : NULL;
        }
    }

    int n = 0;
    char *l = strchr(buf, '\n'); if(l) l++;
    while(l && *l){
        char *nl = strchr(l, '\n');
        if(!nl) break;
        *nl = 0;
        char *f[5] = {0,0,0,0,0}; int nf = 0; char *p = l;
        f[nf++] = p;
        while(nf < 5 && (p = strchr(p, '\t'))){ *p++ = 0; f[nf++] = p; }
        char ssid[64];
        if(nf == 5 && f[4][0] && wifi_ssid_decode(f[4], (int)strlen(f[4]), ssid, sizeof ssid) > 0){
            int freq = atoi(f[1]);
            int sig = atoi(f[2]);
            int dup = -1;
            for(int i=0;i<n;i++) if(!strcmp(out[i].ssid, ssid)){ dup = i; break; }
            if(dup >= 0){
                if(sig > out[dup].sig){ out[dup].sig = sig; out[dup].freq = freq; }
            } else if(n < WNET_MAX){
                memset(&out[n], 0, sizeof out[n]);
                snprintf(out[n].ssid, sizeof out[n].ssid, "%s", ssid);
                out[n].sig = sig;
                out[n].freq = freq;
                out[n].secured = (strstr(f[3], "WPA") || strstr(f[3], "PSK") || strstr(f[3], "WEP")) != 0;
                out[n].in_range = 1;
                for(int j = 0; j < n_saved; j++){
                    if(!strcmp(saved_ssids[j], ssid)){ out[n].saved = 1; break; }
                }
                n++;
            }
        }
        l = nl + 1;
    }

    if(cur && cur[0]){
        int at = -1;
        for(int i=0;i<n;i++) if(!strcmp(out[i].ssid, cur)){ at = i; break; }
        if(at >= 0){
            out[at].connected = 1;
            out[at].saved = 1;
            if(cur_rssi != WNET_NO_RSSI) out[at].sig = cur_rssi;
            if(cur_freq > 0) out[at].freq = cur_freq;
        } else {
            if(n >= WNET_MAX) n--;
            memset(&out[n], 0, sizeof out[n]);
            snprintf(out[n].ssid, sizeof out[n].ssid, "%s", cur);
            out[n].sig = (cur_rssi == WNET_NO_RSSI) ? -100 : cur_rssi;
            out[n].freq = cur_freq;
            out[n].secured = cur_secured;
            out[n].connected = 1;
            out[n].saved = 1;
            out[n].in_range = 1;
            n++;
        }
    }

    if(conn_ssid && conn_ssid[0]){
        for(int i = 0; i < n; i++){
            if(!strcmp(out[i].ssid, conn_ssid) && !out[i].connected) out[i].connecting = 1;
        }
    }

    /* Append saved profiles that are not currently in range */
    for(int j = 0; j < n_saved && n < WNET_MAX; j++){
        int found = 0;
        for(int i = 0; i < n; i++){
            if(!strcmp(out[i].ssid, saved_ssids[j])){ found = 1; break; }
        }
        if(!found){
            memset(&out[n], 0, sizeof out[n]);
            memcpy(out[n].ssid, saved_ssids[j], sizeof out[n].ssid);
            out[n].ssid[sizeof out[n].ssid - 1] = 0;
            out[n].sig = WNET_NO_RSSI;
            out[n].secured = 1;
            out[n].saved = 1;
            out[n].in_range = 0;
            if(conn_ssid && !strcmp(saved_ssids[j], conn_ssid)) out[n].connecting = 1;
            n++;
        }
    }

    if(n > 1) qsort(out, (size_t)n, sizeof out[0], net_cmp);
    return n;
}

static int wifi_parse_rssi(const char *b){
    const char *r = strstr(b, "RSSI=");
    if(!r) return WNET_NO_RSSI;
    r += 5;
    char *end; long v = strtol(r, &end, 10);
    if(end == r || (*end && *end != '\n' && *end != '\r') || v > 0 || v < -200) return WNET_NO_RSSI;
    return (int)v;
}
static int wifi_status_secured(const char *st){
    for(const char *l = st; l && *l; ){
        if(!strncmp(l, "key_mgmt=", 9)){
            const char *k = l + 9;
            return !(!strncmp(k, "NONE", 4) && (k[4] == 0 || k[4] == '\n' || k[4] == '\r'));
        }
        l = strchr(l, '\n'); if(l) l++;
    }
    return 0;
}
static int wifi_cur_rssi(void){
    char b[512];
    if(!run_cap(WCLI "signal_poll 2>/dev/null", b, sizeof b)) return WNET_NO_RSSI;
    return wifi_parse_rssi(b);
}

/* ---- radio toggle ------------------------------------------------------- */
static void sw_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_VALUE_CHANGED) return;
    wifi_power_set(lv_obj_has_state(g_sw, LV_STATE_CHECKED));
}
static void rescan_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) start_scan(); }
static void back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

void wifi_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header_cb(root, "Wi-Fi", back_cb);

    lv_obj_t *trow = lv_obj_create(root);
    lv_obj_remove_style_all(trow);
    lv_obj_set_pos(trow, 50, 64); lv_obj_set_size(trow, 208, 48);
    lv_obj_set_style_radius(trow, 12, 0);
    lv_obj_set_style_bg_color(trow, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(trow, LV_OPA_70, 0);
    lv_obj_clear_flag(trow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(trow);
    lv_label_set_text(tl, "Wi-Fi");
    lv_obj_set_pos(tl, 16, 14);
    lv_obj_set_style_text_font(tl, TF(UI_16), 0);
    lv_obj_set_style_text_color(tl, TC(TEXT_PRIMARY), 0);

    g_sw = lv_switch_create(trow);
    lv_obj_set_size(g_sw, 46, 24);
    lv_obj_set_ext_click_area(g_sw, 10);
    lv_obj_align(g_sw, LV_ALIGN_RIGHT_MID, -14, 0);
    ui_on(g_sw, sw_cb, LV_EVENT_VALUE_CHANGED, NULL, "wifi.sw.value", UI_CORE);

    lv_obj_t *rb = lv_button_create(root);
    lv_obj_remove_style_all(rb);
    lv_obj_set_pos(rb, 266, 72); lv_obj_set_size(rb, 36, 32);
    lv_obj_set_ext_click_area(rb, 8);
    lv_obj_set_style_radius(rb, 10, 0);
    lv_obj_set_style_bg_color(rb, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(rb, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(rb, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(rb, rescan_cb, LV_EVENT_CLICKED, NULL, "wifi.rescan", UI_CORE);
    lv_obj_t *rl = lv_label_create(rb);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(rl, TF(UI_16), 0);
    lv_obj_set_style_text_color(rl, TC(TEXT_SECONDARY), 0);
    lv_obj_center(rl);
    g_rescan_icon = rl;

    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 40, 126); lv_obj_set_size(g_list, 280, 192);
    lv_obj_set_style_pad_bottom(g_list, 44, 0);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(g_list, 6, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    ui_setup_scrollbar(g_list);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

lv_obj_t *wifi_scroller(void){ return g_list; }

void wifi_open(void){
    if(g_sw){
        if(wifi_control_on()) lv_obj_add_state(g_sw, LV_STATE_CHECKED);
        else                lv_obj_clear_state(g_sw, LV_STATE_CHECKED);
    }
    screen_show(SCR_WIFI);
    start_scan();
}

/* ---- Linux control-interface adapter ------------------------------------ */
#ifndef WIFI_CONTROL_PATH
#define WIFI_CONTROL_PATH "/var/run/wpa_supplicant/wlan0"
#endif
static int wifi_current(void){
    return wifi_worker_epoch==atomic_load(&g_intent_epoch) && wifi_worker_player==ipc_generation();
}
static int wifi_socket_open(void){
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(fd<0) return -1;
    struct sockaddr_un local={.sun_family=AF_UNIX},remote={.sun_family=AF_UNIX};
    snprintf(remote.sun_path,sizeof remote.sun_path,"%s",WIFI_CONTROL_PATH);
    if(bind(fd,(struct sockaddr *)&local,sizeof local.sun_family) ||
       connect(fd,(struct sockaddr *)&remote,sizeof remote)){close(fd);return -1;}
    return fd;
}
static int wifi_request_on(int *fd,const char *cmd,char *out,int cap){
    out[0]=0;
    if(*fd<0 || cap<2) return 0;
    pthread_mutex_lock(&wifi_mu);
    int allowed=wifi_current() || (g_rolling_back && !strcmp(cmd,"RECONFIGURE"));
    ssize_t sent=allowed ? send(*fd,cmd,strlen(cmd),MSG_NOSIGNAL) : -1;
    pthread_mutex_unlock(&wifi_mu);
    if(sent!=(ssize_t)strlen(cmd)) goto failed;
    uint32_t began=wifi_now();
    for(;;){
        int left=1000-(int)wifi_elapsed(began);
        struct pollfd p[2]={{.fd=*fd,.events=POLLIN},{.fd=wifi_pipe[0],.events=POLLIN}};
        if(left<=0) goto failed;
        int r=poll(p,2,left);
        if(r<0 && errno==EINTR) continue;
        if(r<=0)goto failed;
        if(p[1].revents){
            char drain[64];while(read(wifi_pipe[0],drain,sizeof drain)>0){}
            if(!wifi_current() && !g_rolling_back)goto failed;
        }
        if(!(p[0].revents&POLLIN)){
            if(p[0].revents)goto failed;
            continue;
        }
        int n=recv(*fd,out,cap-1,MSG_TRUNC);
        if(n<=0 || n>=cap-1) goto failed;
        out[n]=0;
        if(out[0]=='<') goto failed;
        return n;
    }
failed:
    out[0]=0; close(*fd); *fd=-1; return 0;
}
static int wifi_capture(const char *cmd,char *out,int cap,int timeout){
    out[0]=0;
    pthread_mutex_lock(&wifi_mu);
    unsigned h=wifi_current() ? proc_start(cmd,timeout,cap) : 0;
    wifi_capture_handle=h;
    pthread_mutex_unlock(&wifi_mu);
    if(!h) return 0;
    int r=proc_wait(h,out,cap);
    pthread_mutex_lock(&wifi_mu); if(wifi_capture_handle==h) wifi_capture_handle=0; pthread_mutex_unlock(&wifi_mu);
    if(r!=PROC_COMPLETE) out[0]=0;
    return (int)strlen(out);
}
static void wifi_session_end(int *unused){(void)unused;wifi_edit_close();}
static void wifi_feedback(const char *text){
    if(!wifi_current()) return;
    snprintf(g_work.toast,sizeof g_work.toast,"%s",text); g_work.toast_seq++;
}
static int wifi_list_ready(void){
    return wifi_current() && wifi_want() && !g_reco_hold && wifi_rfkill_blocked()!=1 && wpa_ready();
}
static void wifi_pm_sync(int radio_up){
    uint32_t ready_age = (g_lease_sm.phase == WIFI_LEASE_READY) ? wifi_elapsed(g_lease.ready_at) : 0;
    int want_pm = wifi_pm_desired(radio_up, atomic_load(&wifi_lit), g_scanning,
                                  g_conn_active, g_lease_sm.phase == WIFI_LEASE_READY, ready_age);
    if(want_pm < 0){ g_wifi_pm_mode = -1; return; }
    if(want_pm != g_wifi_pm_mode){
        char cmd[64];
        snprintf(cmd, sizeof cmd, "wl PM %d >/dev/null 2>&1", want_pm);
        system(cmd);
        g_wifi_pm_mode = want_pm;
    }
}
static void wifi_lease_step(void){
    if(!wifi_want()){
        g_lease_sm.phase = WIFI_LEASE_FAILED;
        g_lease_sm.since = wifi_now();
        g_lease.net.allowed = 0;
        g_lease_settled = 1;
        return;
    }
    char status[2048]; run_cap(WCLI "status 2>/dev/null", status, sizeof status);
    struct wifi_obs o; wifi_lease_observe(&o, status);
    int prev_phase = g_lease_sm.phase;
    wifi_lease_in_t in = {
        .allowed = o.allowed,
        .ip = o.ip,
        .dhcp_count = o.dhcp_count,
        .dhcp_idle = o.idle,
        .same_network = wifi_lease_matches(&o),
        .explicit_connect = g_lease.explicit_connect,
        .now_ms = wifi_now()
    };
    int rebind = 0;
    unsigned actions = wifi_lease_sm_step(&g_lease_sm, &in, &rebind);
    if(rebind){
        g_lease.net = o;
        g_lease.intent = g_intent_epoch;
        g_lease.player = ipc_generation();
        g_lease.epoch++;
        g_lease.explicit_connect = 0;
    }
    if(actions & WIFI_ACT_ENSURE_DHCP)  wifi_dhcp_ensure(o.dhcp_count);
    if(actions & WIFI_ACT_DHCP_RELEASE) wifi_dhcp_signal(o.dhcp, SIGUSR2);
    if(actions & WIFI_ACT_FLUSH_IP)     system("ifconfig wlan0 0.0.0.0 >/dev/null 2>&1");
    if(actions & WIFI_ACT_DHCP_RENEW)   wifi_dhcp_signal(o.dhcp, SIGUSR1);

    if(g_lease_sm.phase == WIFI_LEASE_READY && prev_phase != WIFI_LEASE_READY)
        g_lease.ready_at = wifi_now();

    g_lease.net.allowed = o.allowed && o.ip == 1 && o.idle && g_lease_sm.phase == WIFI_LEASE_READY;
    if(!o.allowed || g_lease_sm.phase == WIFI_LEASE_READY || g_lease_sm.phase == WIFI_LEASE_FAILED)
        g_lease_settled = 1;
    g_lease_check = wifi_now();
    if(wifi_lease_slow()){ g_lease_retry_ms *= 2; if(g_lease_retry_ms > 4000) g_lease_retry_ms = 4000; }
    else g_lease_retry_ms = 100;
}
static void wifi_scan_start(void){
    if(!wifi_list_ready()) return;
    wifi_pm_sync(1);
    char out[64]; run_cap(WCLI "scan 2>/dev/null", out, sizeof out);
    if(!wifi_reply_ok(out) && !strstr(out,"FAIL-BUSY")){ wifi_feedback("Couldn't scan Wi-Fi"); return; }
    g_scanning = 1; g_scan_due = wifi_now() + 2500;
}
static void wifi_snapshot_read(void){
    char status[2048], signal[512], gw[32], *p;
    g_work.ready = wifi_want() && !g_reco_hold && wpa_ready();
    g_work.online = !wifi_want() ? 0 : g_reco_hold || !g_lease_settled ? -1 :
        g_lease.net.allowed && g_lease_sm.phase == WIFI_LEASE_READY &&
        g_lease.intent == g_intent_epoch && g_lease.player == ipc_generation();
    g_work.ssid[0] = g_work.ip[0] = 0;
    if(g_work.ready) wifi_status(g_work.ssid, sizeof g_work.ssid, g_work.ip, sizeof g_work.ip);
    memset(g_work.info, 0, sizeof g_work.info);
    snprintf(g_work.info[2], 64, "%s", g_work.online == 1 ? g_work.ip : "");
    if(g_work.online == 1 && get_gateway(gw, sizeof gw)) snprintf(g_work.info[3], 64, "%s", gw);
    if(atomic_load(&wifi_page) == SCR_WIFI_INFO && g_work.ready){
        run_cap(WCLI "signal_poll 2>/dev/null", signal, sizeof signal);
        int r = wifi_parse_rssi(signal);
        if(r != WNET_NO_RSSI) wifi_format_signal(r, g_work.info[4], sizeof g_work.info[4]);
        run_cap(WCLI "status 2>/dev/null", status, sizeof status);
        if((p = strstr(status, "key_mgmt="))) sscanf(p + 9, "%63[^\n]", g_work.info[5]);
        if(strstr(g_work.info[5], "NONE")) strcpy(g_work.info[5], "Open");
        int freq;
        if((p = strstr(status, "\nfreq=")) && sscanf(p + 6, "%d", &freq) == 1)
            wifi_format_freq(freq, g_work.info[6], sizeof g_work.info[6]);
        if((p = strstr(status, "bssid="))) sscanf(p + 6, "%63[^\n]", g_work.info[7]);
    }
    g_work.scanning = g_scanning;
    g_work.conn_active = g_conn_active;
    g_work.conn_phase = g_conn_phase;
    snprintf(g_work.conn_ssid, sizeof g_work.conn_ssid, "%s", g_conn_active ? g_conn_ssid : "");
}
static void wifi_scan_snapshot(void){
    if(!wifi_list_ready()){
        g_scanning = 0; wifi_scan_requested = 1; wifi_scan_retry_due = wifi_now() + 2500;
        return;
    }
    char *buf = malloc(65536);
    char *saved = malloc(8192);
    if(!buf || !saved){ free(buf); free(saved); wifi_feedback("Couldn't scan Wi-Fi"); return; }
    buf[0] = saved[0] = 0;
    run_cap(WCLI "scan_results 2>/dev/null", buf, 65536);
    run_cap(WCLI "list_networks 2>/dev/null", saved, 8192);
    char ssid[64], ip[32], status[2048];
    if(!wifi_status(ssid, sizeof ssid, ip, sizeof ip)) ssid[0] = 0;
    run_cap(WCLI "status 2>/dev/null", status, sizeof status);
    int cur_freq = 0; const char *fp = strstr(status, "\nfreq=");
    if(fp) sscanf(fp + 6, "%d", &cur_freq);
    g_work.n = wifi_scan_order(buf, saved, ssid, wifi_cur_rssi(), cur_freq, wifi_status_secured(status),
                               g_conn_active ? g_conn_ssid : "", g_work.nets);
    free(buf); free(saved);
    g_scanning = 0;
}
/* Loss of monitor/control sockets only closes sockets and resets retry timers;
 * it must NOT tear down a healthy LEASE_READY connection. */
static void wifi_monitor_lost(void){
    if(g_monitor_fd >= 0) close(g_monitor_fd);
    g_monitor_fd = -1; g_monitor_ping = 0;
    if(g_control_fd >= 0) close(g_control_fd);
    g_control_fd = -1;
    wifi_lease_retry_reset();
}
static int wifi_monitor_expired(void){
    if(g_monitor_ping && (int32_t)(wifi_now() - g_monitor_due) >= 0){ wifi_monitor_lost(); return 1; }
    return 0;
}
static int wifi_monitor_open(void){
    if(g_monitor_fd >= 0) return 1;
    g_monitor_fd = wifi_socket_open(); char out[64];
    return wifi_request_on(&g_monitor_fd, "ATTACH", out, sizeof out) > 0 && wifi_reply_ok(out);
}
static int wifi_monitor_drain(void){
    int changed = 0;
    for(int i = 0; i < 64 && g_monitor_fd >= 0; i++){
        char b[4096]; int n = recv(g_monitor_fd, b, sizeof b - 1, MSG_TRUNC | MSG_DONTWAIT);
        if(n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        if(n <= 0 || n >= (int)sizeof b - 1){ wifi_monitor_lost(); return 1; }
        b[n] = 0;
        if(!strcmp(b, "PONG\n") || !strcmp(b, "PONG")){ g_monitor_ping = 0; continue; }
        changed = 1;
        if(strstr(b, "CTRL-EVENT-TERMINATING")){
            wifi_monitor_lost();
            if(g_lease_sm.phase == WIFI_LEASE_READY){ g_lease_sm.phase = WIFI_LEASE_NONE; }
            break;
        }
        if(g_conn_active){
            if(strstr(b, "4-Way Handshake failed") || strstr(b, "pre-shared key may be incorrect") || strstr(b, "reason=WRONG_KEY"))
                g_conn_err = CONN_ERR_WRONG_KEY;
            else if(strstr(b, "CTRL-EVENT-ASSOC-REJECT"))
                g_conn_err = CONN_ERR_ASSOC_REJECT;
            else if(strstr(b, "Trying to associate") || strstr(b, "Associated with"))
                g_conn_phase = CONN_PHASE_AUTH;
        }
        if(strstr(b, "CTRL-EVENT-DISCONNECTED")){
            g_lease.net.allowed = 0;
            if(g_lease_sm.phase == WIFI_LEASE_READY) g_lease_sm.phase = WIFI_LEASE_NONE;
        }
        if(strstr(b, "CTRL-EVENT-CONNECTED")){
            if(g_conn_active) g_conn_phase = CONN_PHASE_DHCP;
        }
        if(strstr(b, "CTRL-EVENT-SCAN-RESULTS") && g_scanning) g_scan_due = wifi_now();
    }
    if(changed) wifi_lease_retry_reset();
    return changed;
}
static void wifi_netlink_open(void){
    if(g_netlink_fd >= 0) return;
    g_netlink_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
    if(g_netlink_fd < 0) return;
    struct sockaddr_nl a = {.nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE};
    if(bind(g_netlink_fd, (struct sockaddr *)&a, sizeof a)){ close(g_netlink_fd); g_netlink_fd = -1; }
}
static int wifi_netlink_drain(void){
    int changed = 0;
    for(int i = 0; i < 64 && g_netlink_fd >= 0; i++){
        union { struct nlmsghdr align; char bytes[8192]; } packet;
        char *b = packet.bytes; int n = recv(g_netlink_fd, b, sizeof packet.bytes, MSG_TRUNC | MSG_DONTWAIT);
        if(n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        if(n <= 0 || n > (int)sizeof packet.bytes){
            close(g_netlink_fd); g_netlink_fd = -1; wifi_lease_retry_reset(); return 1;
        }
        changed = 1;
        int left = n; struct nlmsghdr *h = (struct nlmsghdr *)b;
        for(; NLMSG_OK(h, left); h = NLMSG_NEXT(h, left)){
            if(h->nlmsg_type == NLMSG_OVERRUN || h->nlmsg_type == NLMSG_ERROR){
                close(g_netlink_fd); g_netlink_fd = -1; wifi_lease_retry_reset(); return 1;
            }
        }
        if(left){ close(g_netlink_fd); g_netlink_fd = -1; wifi_lease_retry_reset(); return 1; }
    }
    return changed;
}
static void wifi_publish(void){
    if(!wifi_current()) return;
    g_work.epoch = wifi_worker_epoch; g_work.player = wifi_worker_player;
    pthread_mutex_lock(&wifi_mu);
    int changed = memcmp(&g_mail, &g_work, sizeof g_work) != 0;
    if(changed){ g_mail = g_work; wifi_mail_ready = 1; }
    pthread_mutex_unlock(&wifi_mu);
    if(changed) ui_notify(UI_WAKE_WIFI);
}
static void wifi_power_apply(void){
    g_conn_active = 0;
    g_conn_phase = CONN_PHASE_IDLE;
    g_conn_err = CONN_ERR_NONE;
    g_wifi_pm_mode = -1;
    if(wifi_want()){
        wifi_scan_requested = 1; wifi_scan_retry_due = wifi_now();
        if(!g_wifi_last_start || wifi_elapsed(g_wifi_last_start) > 30000)
            if(system("/usr/bin/wifi_up.sh >/dev/null 2>&1 &") == 0) g_wifi_last_start = wifi_now();
        g_sanitize_pending = 1;
    }else{
        g_scanning = 0; wifi_scan_requested = 0; wifi_discard_attempt(); wifi_lease_step();
        system("[ -x /usr/data/bin/diskos-web.sh ] && /bin/sh /usr/data/bin/diskos-web.sh stop >/dev/null 2>&1");
        system("/usr/bin/wifi_down.sh >/dev/null 2>&1"); g_wifi_last_start = 0;
    }
}
static int wifi_launch_stop(int off){
    if(g_wifi_launch <= 0) return 1;
    pid_t prior = waitpid(g_wifi_launch, NULL, WNOHANG);
    if(prior == g_wifi_launch || (prior < 0 && errno == ECHILD)){ g_wifi_launch = 0; return 1; }
    if(prior < 0) return 0;
    pthread_mutex_lock(&wifi_mu);
    int allowed = wifi_current() && (!off || !wifi_want());
    if(allowed){
        if(off && g_wifi_launch_grouped) kill(-g_wifi_launch, SIGKILL);
        kill(g_wifi_launch, SIGKILL);
    }
    pthread_mutex_unlock(&wifi_mu);
    if(!allowed) return 0;
    uint32_t began = wifi_now();
    for(;;){
        pid_t reaped = waitpid(g_wifi_launch, NULL, WNOHANG);
        if(reaped == g_wifi_launch || (reaped < 0 && errno == ECHILD)){ g_wifi_launch = 0; return 1; }
        if((reaped < 0 && errno != EINTR) || wifi_elapsed(began) >= 1000) return 0;
        poll(NULL, 0, 10);
    }
}
static int wifi_reload_transaction(void){
    g_sanitize_pending = 1; g_conn_active = 0; g_conn_phase = CONN_PHASE_IDLE;
    if(!wifi_discard_attempt() || !wifi_launch_stop(0)) return 0;
    wifi_edit_close(); wifi_monitor_lost(); return 1;
}
static int wifi_reload_park(void){
    pthread_mutex_lock(&wifi_mu);
    int prepared = wifi_current() && !wifi_has_request;
    if(prepared) atomic_store(&wifi_reload_freeze, 1);
    pthread_mutex_unlock(&wifi_mu); return prepared;
}
static uint32_t wifi_seed_due;
static void wifi_seed_poll(void){
    if(wifi_seed < 0 || (int32_t)(wifi_now() - wifi_seed_due) < 0) return;
    char b[32]; run_cap("sqlite3 /usr/data/fiio/db/sysconfig.db \"SELECT WIFI_STATUS FROM SYSCONFIG WHERE ID=1\" 2>/dev/null", b, sizeof b);
    pthread_mutex_lock(&wifi_mu);
    if((b[0] == '0' || b[0] == '1') && wifi_current() && atomic_load(&g_intent_epoch) == 1){
        g_work.seed = b[0] - '0'; atomic_store(&wifi_desired, g_work.seed); wifi_seed = -1;
    }
    pthread_mutex_unlock(&wifi_mu);
    wifi_seed_due = wifi_now() + 5000;
}
struct wifi_worker_state {
    uint32_t health, reco, conn, control_failed_at, last_reconnect;
    struct stat peer;
    struct wifi_proc daemon;
    int control_failed, hold;
};
static int wifi_worker_step(struct wifi_worker_state *state){
    struct wifi_request request = {0}; unsigned hints;
    pthread_mutex_lock(&wifi_mu);
    if(wifi_has_request){ request = wifi_queued; wifi_has_request = 0; memset(wifi_queued.key, 0, sizeof wifi_queued.key); }
    hints = wifi_hints; wifi_hints = 0;
    unsigned epoch = atomic_load(&g_intent_epoch), player = ipc_generation();
    pthread_mutex_unlock(&wifi_mu);
    if(epoch != wifi_worker_epoch || player != wifi_worker_player){
        g_conn_active = 0; g_conn_phase = CONN_PHASE_IDLE; g_conn_err = CONN_ERR_NONE;
        g_work.toast[0] = 0;
        if(g_save_pending && !g_conn_dirty) g_initial_reload = 1;
        wifi_discard_attempt();
        wifi_worker_epoch = epoch; wifi_worker_player = player;
        wifi_lease_retry_reset(); state->health = state->reco = wifi_now();
    }
    if(wifi_seed >= 0 && !request.kind) wifi_seed_poll();
    if(request.kind) wifi_seed = -1;
    if(request.kind && request.epoch == epoch && wifi_current()){
        g_work.reloaded = 0;
        if(request.kind != WIFI_RELOAD) g_reco_hold = 0;
        if(request.kind == WIFI_POWER){ wifi_power_apply(); }
        else if(request.kind == WIFI_CONNECT){
            wifi_connect(request.ssid, request.has_key ? request.key : NULL, request.force_open, request.hidden);
            state->conn = wifi_now();
        }
        else if(request.kind == WIFI_DISCONNECT){ wifi_disconnect_now(); }
        else if(request.kind == WIFI_FORGET){
            wifi_feedback(wifi_remove_ssid(request.ssid) ? "Network forgotten" : "Couldn't forget network");
            hints |= WIFI_SCAN;
        }
        else if(request.kind == WIFI_RELOAD){
            g_work.reloaded = wifi_reload_transaction();
            if(g_work.reloaded && !wifi_want() && wifi_radio_on())
                g_work.reloaded = system("/usr/bin/wifi_down.sh >/dev/null 2>&1") == 0;
            if(g_work.reloaded && wifi_reload_park()){
                g_work.ready = 0; g_work.online = wifi_want() ? -1 : 0; g_work.scanning = 0;
                wifi_publish(); return -1;
            }
            g_work.reloaded = 0;
        }
        memset(request.key, 0, sizeof request.key); state->health = wifi_now();
    }
    uint32_t now = wifi_now(); int refresh = wifi_monitor_drain();
    if(wifi_monitor_expired()){ refresh = 1; state->health = now; }
    if((int32_t)(now - state->reco) >= 0){ wifi_reconcile_poll(); state->reco = wifi_now() + 1000; }
    if(!!g_reco_hold != state->hold){ state->hold = !!g_reco_hold; refresh = 1; }
    if((int32_t)(now - state->health) >= 0){
        state->health = now + 5000; refresh = 1;
        struct stat st = {0}; struct wifi_proc current = {0}; wifi_proc_find("wpa_supplicant", &current);
        if(stat(WIFI_CONTROL_PATH, &st) || st.st_dev != state->peer.st_dev || st.st_ino != state->peer.st_ino || !wifi_proc_eq(current, state->daemon)){
            wifi_monitor_lost(); state->peer = st; state->daemon = current;
            g_wifi_pm_mode = -1;
        }
        if(wifi_want() && !g_reco_hold){
            if(!wifi_monitor_open()) wifi_monitor_lost();
            else if(!g_monitor_ping){
                g_monitor_ping = 1; g_monitor_due = wifi_now() + 4000;
                if(send(g_monitor_fd, "PING", 4, MSG_NOSIGNAL) != 4) wifi_monitor_lost();
            }
        }
        wifi_netlink_open();
        int up = wifi_radio_on(), want = wifi_want(), ready = up && want ? wpa_ready() : 0;
        if(!up || !want || ready) state->control_failed = 0;
        if(up && want && !ready){
            if(!state->control_failed){ state->control_failed = 1; state->control_failed_at = now; }
            else if(wifi_elapsed(state->control_failed_at) >= 30000){
                if(system("killall -9 wpa_supplicant >/dev/null 2>&1") == 0){ up = 0; state->control_failed = 0; }
            }
        }
        if(!g_reco_hold && wifi_current()){
            if(up && want && g_sanitize_pending && !g_conn_active && wifi_sanitize_savenets()) g_sanitize_pending = 0;
            int blocked = wifi_rfkill_blocked(); want = wifi_want();
            if(up && want && blocked == 1) system("rfkill unblock wifi; ifconfig wlan0 up");
            if(!up && want && (!g_wifi_last_start || wifi_elapsed(g_wifi_last_start) > 30000)){
                g_sanitize_pending = 1; g_wifi_pm_mode = -1;
                if(system("/usr/bin/wifi_up.sh >/dev/null 2>&1 &") == 0) g_wifi_last_start = wifi_now();
            } else if(!want && (up || blocked != 1)){
                g_wifi_pm_mode = -1;
                system("[ -x /usr/data/bin/diskos-web.sh ] && /bin/sh /usr/data/bin/diskos-web.sh stop >/dev/null 2>&1");
                system("/usr/bin/wifi_down.sh >/dev/null 2>&1");
            }
            /* Kick supplicant auto-reconnect if idle with saved networks */
            if(up && want && ready && !g_conn_active && g_lease_sm.phase != WIFI_LEASE_READY &&
               (!state->last_reconnect || wifi_elapsed(state->last_reconnect) >= 15000)){
                char stbuf[2048];
                if(run_cap(WCLI "status 2>/dev/null", stbuf, sizeof stbuf) && strstr(stbuf, "wpa_state=INACTIVE")){
                    char nl[2048], out[64];
                    if(run_cap(WCLI "list_networks 2>/dev/null", nl, sizeof nl)){
                        const char *row = strchr(nl, '\n');
                        if(row && row[1] >= '0' && row[1] <= '9'){
                            state->last_reconnect = wifi_now();
                            run_cap(WCLI "enable_network all 2>/dev/null", out, sizeof out);
                            run_cap(WCLI "reconnect 2>/dev/null", out, sizeof out);
                        }
                    }
                }
            }
        }
    }
    refresh |= wifi_netlink_drain();
    if(hints & WIFI_SCAN){ wifi_scan_requested = 1; wifi_scan_retry_due = wifi_now(); }
    if(!g_reco_hold && wifi_current()){
        if(hints & WIFI_INPUT) wifi_lease_retry_reset();
        if(refresh || (g_lease_sm.phase != WIFI_LEASE_READY && wifi_elapsed(g_lease_check) >= g_lease_retry_ms)){
            int phase = g_lease_sm.phase, allowed = g_lease.net.allowed, settled = g_lease_settled;
            wifi_lease_step();
            refresh |= phase != g_lease_sm.phase || allowed != g_lease.net.allowed || settled != g_lease_settled;
        }
        if(wifi_scan_requested && atomic_load(&wifi_page) == SCR_WIFI &&
           (refresh || (int32_t)(wifi_now() - wifi_scan_retry_due) >= 0)){
            if(wifi_list_ready()){ wifi_scan_requested = 0; wifi_scan_start(); }
            else wifi_scan_retry_due = wifi_now() + 2500;
        }
        if(g_scanning && (int32_t)(wifi_now() - g_scan_due) >= 0){ wifi_scan_snapshot(); refresh = 1; }
        if(g_conn_active && (refresh || (int32_t)(wifi_now() - state->conn) >= 0)){
            conn_poll_cb(NULL); state->conn = wifi_now() + 1000; refresh = 1;
        }
        const char *current = g_lease.net.allowed && g_lease_sm.phase == WIFI_LEASE_READY ? g_lease.net.ssid : "";
        if(atomic_load(&wifi_page) == SCR_WIFI && !g_scanning && strcmp(current, g_cur_ssid)){
            snprintf(g_cur_ssid, sizeof g_cur_ssid, "%s", current);
            wifi_scan_snapshot(); refresh = 1;
        }
        wifi_pm_sync(wifi_radio_on());
    }
    if(refresh || hints || request.kind){ wifi_snapshot_read(); wifi_publish(); }
    now = wifi_now(); uint32_t wait = 5000;
    wait = ui_deadline_min(wait, now, state->health);
    if(wifi_seed >= 0) wait = ui_deadline_min(wait, now, wifi_seed_due);
    if(g_monitor_ping) wait = ui_deadline_min(wait, now, g_monitor_due);
    if(g_reco_hold) wait = ui_deadline_min(wait, now, state->reco);
    if(!g_reco_hold){
        if(g_conn_active) wait = ui_deadline_min(wait, now, state->conn);
        if(g_scanning) wait = ui_deadline_min(wait, now, g_scan_due);
        if(wifi_scan_requested && atomic_load(&wifi_page) == SCR_WIFI)
            wait = ui_deadline_min(wait, now, wifi_scan_retry_due);
        if(g_lease_sm.phase == WIFI_LEASE_READY && !atomic_load(&wifi_lit) && g_wifi_pm_mode != 2)
            wait = ui_deadline_min(wait, now, g_lease.ready_at + 5000u);
    }
    if(!g_reco_hold && wifi_want() && g_lease_sm.phase != WIFI_LEASE_READY)
        wait = ui_deadline_min(wait, now, g_lease_check + g_lease_retry_ms);
    return (int)wait;
}
static void *wifi_worker(void *unused){
    (void)unused;
    uint32_t now = wifi_now();
    struct wifi_worker_state state = {.health = now, .reco = now, .conn = now};
    for(;;){
        if(atomic_load(&wifi_reload_freeze)){
            struct pollfd f = {wifi_pipe[0], POLLIN, 0}; poll(&f, 1, -1);
            char b[64]; while(read(wifi_pipe[0], b, sizeof b) > 0){}
            continue;
        }
        int wait = wifi_worker_step(&state);
        if(wait < 0) continue;
        struct pollfd fds[3] = {{wifi_pipe[0], POLLIN, 0}, {g_monitor_fd, POLLIN, 0}, {g_netlink_fd, POLLIN, 0}};
        poll(fds, 3, wait); char b[64]; while(read(wifi_pipe[0], b, sizeof b) > 0){}
    }
    return NULL;
}
static void wifi_poke(void){ char b = 0; if(wifi_pipe[1] >= 0) while(write(wifi_pipe[1], &b, 1) < 0 && errno == EINTR){} }
void wifi_init_intent(void){
    if(wifi_started) return;
    int want = cfg_get_int("wifi_on", -1); wifi_seed = want < 0 ? 0 : -1;
    (void)bt_radio_on();
    atomic_store(&wifi_desired, want != 0); wifi_seed_due = wifi_now(); g_wifi_theme_reload = getenv("DISKOS_THEME_RELOAD") != NULL;
    if(pipe2(wifi_pipe, O_CLOEXEC | O_NONBLOCK)) return;
    pthread_t t;
    if(pthread_create(&t, NULL, wifi_worker, NULL)){ close(wifi_pipe[0]); close(wifi_pipe[1]); wifi_pipe[0] = wifi_pipe[1] = -1; return; }
    pthread_detach(t); wifi_started = 1;
}
static void wifi_hint(unsigned hints){
    wifi_init_intent(); pthread_mutex_lock(&wifi_mu); wifi_hints |= hints; wifi_poke(); pthread_mutex_unlock(&wifi_mu);
}
static void wifi_submit_full(int kind, const char *ssid, const char *key, int on, int force_open, int hidden){
    wifi_init_intent(); pthread_mutex_lock(&wifi_mu);
    if(kind != WIFI_RELOAD) atomic_store(&wifi_reload_freeze, 0);
    if(kind == WIFI_POWER) atomic_store(&wifi_desired, !!on);
    unsigned epoch = atomic_fetch_add(&g_intent_epoch, 1) + 1;
    wifi_queued = (struct wifi_request){
        .epoch = epoch, .kind = kind, .on = on,
        .has_key = key != NULL, .force_open = force_open, .hidden = hidden
    };
    snprintf(wifi_queued.ssid, sizeof wifi_queued.ssid, "%s", ssid ? ssid : "");
    snprintf(wifi_queued.key, sizeof wifi_queued.key, "%s", key ? key : "");
    wifi_has_request = 1;
    if(wifi_capture_handle) proc_cancel(wifi_capture_handle);
    wifi_poke(); pthread_mutex_unlock(&wifi_mu);
}
static void wifi_submit(int kind, const char *ssid, const char *key, int on){
    wifi_submit_full(kind, ssid, key, on, 0, 0);
}
static void wifi_submit_connect_opts(const char *ssid, const char *key, int force_open, int hidden){
    if(key && *key && !wifi_psk_len_ok(key)){ ui_toast("Password must be 8 to 63 characters"); return; }
    wifi_submit_full(WIFI_CONNECT, ssid, key, -1, force_open, hidden);
}
static void wifi_submit_connect(const char *ssid, const char *key){
    wifi_submit_connect_opts(ssid, key, 0, 0);
}
void wifi_power_set(int on){
    g_view.ready = 0; g_view.online = on ? -1 : 0;
    wifi_submit(WIFI_POWER, NULL, NULL, on);
    cfg_set_int("wifi_on", !!on);
    if(g_sw){ if(on) lv_obj_add_state(g_sw, LV_STATE_CHECKED); else lv_obj_clear_state(g_sw, LV_STATE_CHECKED); }
    if(screen_current() == SCR_WIFI) list_msg(on ? "Starting Wi-Fi..." : "Wi-Fi is off");
}
int wifi_toggle(void){ int on = !wifi_control_on(); wifi_power_set(on); return on; }
int wifi_power_pending(void){ return wifi_control_on() && !g_view.ready; }
int wifi_connectivity(void){
    if(!wifi_control_on()) return 0;
    if(g_view.epoch != atomic_load(&g_intent_epoch) || g_view.player != ipc_generation()) return -1;
    return g_view.online;
}
static void start_scan(void){
    if(screen_current() != SCR_WIFI) return;
    if(!wifi_control_on()){ list_msg("Wi-Fi is off"); rescan_spin_set(0); return; }
    if(!g_view.ready) list_msg("Starting Wi-Fi...");
    else if(g_view.n == 0) list_msg_scanning();
    else rescan_spin_set(ui_screen_backlight_on());
    wifi_hint(WIFI_SCAN);
}
static void scan_fill(void){
    if(screen_current() != SCR_WIFI || !g_list) return;
    int lit = ui_screen_backlight_on();
    int on = wifi_control_on();
    rescan_spin_set(lit && on && g_view.ready && g_view.scanning);
    if(!on){ list_msg("Wi-Fi is off"); return; }
    if(!g_view.ready){ list_msg("Starting Wi-Fi..."); return; }
    if(g_view.scanning && g_view.n == 0){ list_msg_scanning(); return; }

    int same = g_rendered_valid &&
               g_rendered_list.on == on &&
               g_rendered_list.ready == g_view.ready &&
               g_rendered_list.conn_phase == g_view.conn_phase &&
               g_rendered_list.n == g_view.n &&
               !strcmp(g_rendered_list.ip, g_view.ip) &&
               memcmp(g_rendered_list.nets, g_view.nets, (size_t)g_view.n * sizeof(struct wnet)) == 0;
    if(same) return;

    g_rendered_list.on = on;
    g_rendered_list.ready = g_view.ready;
    g_rendered_list.conn_phase = g_view.conn_phase;
    g_rendered_list.n = g_view.n;
    snprintf(g_rendered_list.ip, sizeof g_rendered_list.ip, "%s", g_view.ip);
    if(g_view.n > 0) memcpy(g_rendered_list.nets, g_view.nets, (size_t)g_view.n * sizeof(struct wnet));
    g_rendered_valid = 1;

    int32_t scroll_y = lv_obj_get_scroll_y(g_list);
    scan_stop();
    lv_obj_clean(g_list);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int added_saved_hdr = 0;
    for(int i = 0; i < g_view.n; i++){
        if(!g_view.nets[i].in_range && g_view.nets[i].saved && !added_saved_hdr){
            added_saved_hdr = 1;
            lv_obj_t *hdr = lv_label_create(g_list);
            lv_label_set_text(hdr, tr("Saved Networks"));
            lv_obj_set_width(hdr, 268);
            lv_obj_set_style_pad_top(hdr, 6, 0);
            lv_obj_set_style_text_font(hdr, TF(UI_12), 0);
            lv_obj_set_style_text_color(hdr, TC(TEXT_MUTED), 0);
            kit_section(hdr);
        }
        add_net_row(&g_view.nets[i], g_view.ip, g_view.conn_phase);
    }
    if(g_view.n == 0){
        lv_obj_t *e = lv_label_create(g_list);
        lv_label_set_text(e, tr("No networks found"));
        lv_obj_set_style_pad_ver(e, 14, 0);
        lv_obj_set_style_text_color(e, TC(TEXT_MUTED), 0);
        lv_obj_set_style_text_font(e, TF(UI_14), 0);
    }
    add_join_other_row();
    if(scroll_y > 0) lv_obj_scroll_to_y(g_list, scroll_y, LV_ANIM_OFF);
}
void wifi_supervise(void){
    wifi_init_intent();
    static unsigned player;
    unsigned generation = ipc_generation();
    int page = screen_current(), lit = ui_screen_backlight_on();
    uint32_t activity = lv_tick_get() - lv_display_get_inactive_time(NULL);
    int changed = generation != player; player = generation;
    changed |= atomic_exchange(&wifi_page, page) != page;
    changed |= atomic_exchange(&wifi_lit, lit) != lit;
    changed |= atomic_exchange(&wifi_activity, activity) != activity;
    if(changed)
        wifi_hint(WIFI_INPUT | (page == SCR_WIFI_INFO ? WIFI_DETAILS : 0));
    struct wifi_snapshot mail; int ready;
    pthread_mutex_lock(&wifi_mu); ready = wifi_mail_ready; mail = g_mail; wifi_mail_ready = 0; pthread_mutex_unlock(&wifi_mu);
    if(!lit){ scan_stop(); rescan_spin_set(0); }
    if(!ready || mail.epoch != atomic_load(&g_intent_epoch) || mail.player != ipc_generation()) return;
    unsigned toast = g_view.toast_seq; g_view = mail;
    if(mail.seed >= 0 && cfg_get_int("wifi_on", -1) < 0) cfg_set_int("wifi_on", mail.seed);
    if(mail.toast_seq != toast && mail.toast[0]) ui_toast(tr(mail.toast));
    if(lit){
        if(page == SCR_WIFI) scan_fill();
        if(page == SCR_WIFI_INFO) wifi_info_refresh();
    }
}
int wifi_prepare_reload(void){
    if(wifi_reload_requested && g_view.epoch == wifi_reload_requested) return g_view.reloaded;
    if(!wifi_reload_requested){
        wifi_reload_player = ipc_generation();
        wifi_submit(WIFI_RELOAD, NULL, NULL, -1); wifi_reload_requested = atomic_load(&g_intent_epoch);
    }
    return 0;
}
int wifi_reload_result(void){
    if(!wifi_reload_requested || wifi_reload_requested != atomic_load(&g_intent_epoch) || wifi_reload_player != ipc_generation()) return -1;
    if(g_view.epoch != wifi_reload_requested) return 2;
    return g_view.reloaded ? 1 : 0;
}
void wifi_reload_cancel(void){
    wifi_reload_requested = 0; atomic_store(&wifi_reload_freeze, 0); wifi_hint(WIFI_INPUT);
}
