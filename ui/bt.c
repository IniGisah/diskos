/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "fwcaps.h"
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"       /* cfg_get_int/cfg_set_int: persist the BT on/off intent */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>

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
 * centre band; the list carries on/scanning/empty state; connected ✓ row taps
 * through to details).  NOTE: routing the player's audio onto a connected BT
 * sink is the stock mq_player's job (it has bluealsa support) - building the
 * profile here is stage 1; confirm real headphone playback with the user. */

static lv_obj_t *g_sw, *g_list, *g_info_list;
static lv_timer_t *g_scan_timer;
static lv_timer_t *g_scanwait_timer;   /* bt_open's non-destructive "wait for adapter then scan" poll */
static uint32_t    g_scanwait_start;
static lv_timer_t *g_bt_autoroute_timer;
static char g_sel_mac[20];     /* device selected for the details screen */
static char g_bt_autorouted[20];

static void start_scan(void);             /* fwd */
static void scan_timer_cb(lv_timer_t *t);  /* fwd */
static void scan_abort(void);             /* fwd */
static void scan_kick(void);              /* fwd - off-thread re-enumerate (no discovery window) */
static void bt_autoroute_start(void);      /* fwd */
static void bt_autoroute_stop(void);       /* fwd */

/* The refresh glyph INSIDE the "Scanning" message spins while a scan runs. It lives
 * in g_list, so it must be stopped before g_list is cleaned (else the anim references
 * a freed object). scan_stop() is called at the top of every g_list-clearing path. */
static lv_obj_t *g_scan_icon;
static void spin_anim_cb(void *o, int32_t v){ lv_obj_set_style_transform_rotation((lv_obj_t*)o, v, 0); }
static void scan_stop(void){
    if(g_scan_icon){ lv_anim_delete(g_scan_icon, spin_anim_cb); g_scan_icon = NULL; }
}

/* ---- header helpers (local copies) -------------------------------------- */

/* Children run_cap_bounded killed but could not reap within its bound (e.g. stuck in uninterruptible
 * sleep). They stay unreaped - so their pids stay ours - and are retried, never waited on, by later calls.
 * Main thread only (both callers run on LVGL timers), so no lock. */
#define BT_STUCK_MAX 8
static pid_t g_bt_stuck[BT_STUCK_MAX];

static void bt_reap_stuck(void){
    for(int i = 0; i < BT_STUCK_MAX; i++){
        if(g_bt_stuck[i] > 0){
            if(waitpid(g_bt_stuck[i], NULL, WNOHANG) != 0){
                g_bt_stuck[i] = 0;
            } else {
                kill(-g_bt_stuck[i], SIGKILL);
                kill(g_bt_stuck[i], SIGKILL);
            }
        }
    }
}

/* Like run_cap, but the child runs in its OWN process group and is HARD-KILLED after `timeout_ms`, so a
 * wedged bluetoothd/bluealsa can never hang the UI thread. The device has NO `timeout` binary, so we
 * bound it in C. Returns bytes captured (0 on failure/timeout). Used for the periodic BT state/route
 * probes that run on LVGL timers (the main thread).
 * Every step is bounded: the group is established from BOTH sides of the fork (so it exists before the
 * parent can signal it), the direct child is killed as well as its group, and reaping is non-blocking
 * with a short cap - a child that will not die is parked in g_bt_stuck instead of blocking the UI. */
static int run_cap_bounded(const char *cmd, char *out, int cap, int timeout_ms){
    out[0] = 0;
    if(cap < 1) return 0;
    bt_reap_stuck();
    int slot = -1;
    for(int i = 0; i < BT_STUCK_MAX; i++) if(g_bt_stuck[i] <= 0){ slot = i; break; }
    if(slot < 0){
        /* Force reuse of the oldest slot rather than permanently locking out all probes */
        slot = 0;
        kill(-g_bt_stuck[0], SIGKILL);
        kill(g_bt_stuck[0], SIGKILL);
        waitpid(g_bt_stuck[0], NULL, WNOHANG);
        g_bt_stuck[0] = 0;
    }
    int fds[2];
    if(pipe(fds) != 0) return 0;
    pid_t pid = fork();
    if(pid < 0){ close(fds[0]); close(fds[1]); return 0; }
    if(pid == 0){                              /* child: stdout -> pipe; own group for kill(-pid) */
        close(fds[0]);
        dup2(fds[1], 1);
        for(int i = 3; i < 256; i++) close(i);
        if(setpgid(0, 0) != 0 && getpgrp() != getpid()) _exit(126);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    setpgid(pid, pid);                         /* parent side too: the group exists before we go on */
    close(fds[1]);
    int n = 0;
    uint32_t start = lv_tick_get();
    for(;;){
        int left = timeout_ms - (int)lv_tick_elaps(start);
        if(left <= 0) break;                   /* deadline hit */
        struct pollfd pf = { fds[0], POLLIN, 0 };
        int pr = poll(&pf, 1, left);
        if(pr <= 0) break;                     /* timeout or poll error */
        int r = read(fds[0], out + n, cap - 1 - n);
        if(r <= 0) break;                      /* EOF or read error */
        n += r;
        if(n >= cap - 1) break;
    }
    out[n] = 0;
    close(fds[0]);
    /* The child is unreaped, so pid and its group are still ours: kill both, whatever happened. */
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    for(int i = 0; i < 20; i++){               /* bounded reap: at most ~100 ms */
        if(waitpid(pid, NULL, WNOHANG) != 0) return n;
        usleep(5000);
    }
    g_bt_stuck[slot] = pid;                    /* stuck in the kernel: retry later, never block on it */
    return n;
}
/* Shared entry for other modules (wifi.c radio reconciliation): the same bounded capture - the command runs in its
 * own process group and is killed at timeout_ms, so a wedged tool can never block the UI thread. */
int ui_run_cap_bounded(const char *cmd, char *out, int cap, int timeout_ms){ return run_cap_bounded(cmd, out, cap, timeout_ms); }

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
    lv_label_set_text(t, "Scanning");
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

/* powered = bluetoothd up AND adapter Powered: yes */
static int bt_on(void){
    char b[64]; run_cap_bounded("pidof bluetoothd 2>/dev/null", b, sizeof b, 200);   /* process read - can't hang */
    if(!b[0]) return 0;
    /* bluetoothctl talks to bluetoothd over D-Bus and can stall if the daemon is wedged - bound it so
     * this probe (called from UI poll timers) never freezes the main thread. */
    char s[2048]; run_cap_bounded("bluetoothctl show 2>/dev/null", s, sizeof s, 250);
    return strstr(s, "Powered: yes") != NULL;
}

/* Cheap "is the BT radio enabled?" for the status icon + QS tile: reads the bluetooth rfkill soft-block
 * from /sys (no process spawn). Reflects reality regardless of who set it - diskOS bt_disable() rfkill-
 * blocks, and the stock boot can bring BT up per its own SYSCONFIG - unlike the persisted cfg intent,
 * which goes stale when the stock firmware enables BT out from under us. Falls back to the intent if no
 * bluetooth rfkill node exists. */
int bt_radio_on(void){
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
    return found ? 1 : cfg_get_int("bt_on", 0);
}

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

static int sync_system(const char *cmd){
    pid_t pid = fork();
    if(pid == 0){
        for(int i = 3; i < 256; i++) close(i);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    if(pid > 0){
        int status;
        waitpid(pid, &status, 0);
        return status;
    }
    return -1;
}

/* a2dp-source audio engine. --sbc-quality=medium (bit-pool ~33) keeps SBC encode load
 * under this X2000 CPU's ceiling: stock's default-quality stereo SBC stutters, but medium
 * quality plays clean STEREO with headroom (on-device: ~86% idle). bluetoothctl's own agent
 * (set up in bt_enable) handles just-works pairing, so no separate bt-agent. */
static void bt_ensure_services(void){
    bg_system("pidof bluealsa >/dev/null 2>&1 || bluealsa -S --device=hci0 --profile=a2dp-source "
           "--sbc-quality=medium --ldac-abr --ldac-quality=standard "
           "--codec=sbc --initial-volume=48 >/tmp/bluealsa.log 2>&1 &");
}
/* ---- A2DP codec choice (parity C17/C18) ----------------------------------------------------------------
 * Stock V2.57 picks the codec with the player command 06b3 (VALUE1: 0 SBC, 1 AAC, 2/3/4 LDAC mobile/standard/high;
 * mq_player handler 0x4f0980 maps it to the worker 0x4112b4, which reads /usr/data/bt_codec, then calls BlueALSA
 * SelectCodec over D-Bus; LDAC quality goes through /usr/data/bt_pipe_recv). BlueALSA itself is left as stock
 * launches it - on the device its status advertises SBC AAC LDAC endpoints whatever --codec says - so a choice needs
 * no daemon restart, and this only ever sends the frame diskOS already sent, with another VALUE1. The choice is
 * read on the next route to a Bluetooth device (connect/reconnect), once BlueALSA has that device's PCM. DEVICE-UNVERIFIED with headphones. */
const char *const BT_CODEC_LABEL[BT_CODEC_N] = { "SBC", "AAC", "LDAC Mobile", "LDAC Standard", "LDAC High" };
static int bt_mac_valid(const char *mac);
static int bt_codec_frame_value(const char *mac, int v, char *out, int cap);
#define BTC_AAC  2u
#define BTC_LDAC 4u
/* The stock VALUE1 to send: the best codec supported by the sink that does not exceed the user's preference.
 * Priority hierarchy matches stock firmware: LDAC (quality per choice) -> AAC -> SBC. */
int bt_codec_pick(int choice, unsigned avail, int avail_known){
    if(choice < 0 || choice >= BT_CODEC_N) return 0;
    if(!avail_known) return 0;   /* sink capabilities not yet known: keep SBC */

    /* If user chose LDAC (2=Mobile, 3=Standard, 4=High):
     * Prefer LDAC if available; fall back to AAC if available; otherwise SBC. */
    if(choice >= 2 && choice <= 4){
        if(avail & BTC_LDAC) return choice;
        if(avail & BTC_AAC)  return 1;
        return 0;
    }

    /* If user chose AAC (1):
     * Prefer AAC if available; otherwise SBC. */
    if(choice == 1){
        if(avail & BTC_AAC)  return 1;
        return 0;
    }

    return 0;
}
/* Codecs a sink offers, from `bluealsa-cli info <pcm>`: the words on the "Available codecs:" line and on the lines
 * after it that carry no ':'. *known = 0 when that header is missing (unparseable/failed query). */
unsigned bt_codec_parse_avail(const char *info, int *known){
    unsigned m = 0;
    if(known) *known = 0;
    const char *h = info ? strstr(info, "Available codecs:") : NULL;
    if(!h) return 0;
    if(known) *known = 1;
    const char *p = h + 17;
    for(int first = 1; *p;){
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        if(!first && memchr(p, ':', n)) break;
        first = 0;
        for(size_t i = 0; i < n;){
            size_t j = i;
            while(j < n && ((p[j]>='A'&&p[j]<='Z')||(p[j]>='a'&&p[j]<='z')||(p[j]>='0'&&p[j]<='9'))) j++;
            if(j - i == 3 && !strncasecmp(p + i, "AAC", 3)) m |= BTC_AAC;
            else if(j - i == 4 && !strncasecmp(p + i, "LDAC", 4)) m |= BTC_LDAC;
            i = j > i ? j : i + 1;
        }
        if(!e) break;
        p = e + 1;
    }
    return m;
}
/* Shell command that prints "PCM=<path>" then the A2DP source PCM's info for a device (MAC already validated);
 * it prints nothing while BlueALSA has no such PCM. */
static int bt_codec_query_cmd(const char *mac, char *out, int cap){
    if(!bt_mac_valid(mac)) return -1;
    char d[24]; int n = 0;
    for(int i = 0; i < 17; i++) d[n++] = mac[i] == ':' ? '_' : mac[i];
    d[n] = 0;
    int r = snprintf(out, cap, "p=$(bluealsa-cli list-pcms 2>/dev/null | grep -i 'dev_%s/a2dp' | head -n 1); "
                               "[ -n \"$p\" ] && { echo \"PCM=$p\"; bluealsa-cli info \"$p\" 2>/dev/null; }", d);
    return (r > 0 && r < cap) ? 0 : -1;
}
/* Thread-safe bounded capture for the probe worker (the shared run_cap_bounded is main-thread only). */
static int bt_probe_run(const char *cmd, char *out, int cap, int timeout_ms){
    out[0] = 0;
    int fds[2];
    if(pipe(fds) != 0) return 0;
    pid_t pid = fork();
    if(pid < 0){ close(fds[0]); close(fds[1]); return 0; }
    if(pid == 0){
        close(fds[0]); dup2(fds[1], 1);
        for(int i = 3; i < 256; i++) close(i);
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL); _exit(127);
    }
    setpgid(pid, pid);
    close(fds[1]);
    int n = 0, left = timeout_ms;
    while(left > 0 && n < cap - 1){
        struct pollfd pf = { fds[0], POLLIN, 0 };
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        if(poll(&pf, 1, left) <= 0) break;
        int r = read(fds[0], out + n, cap - 1 - n);
        if(r <= 0) break;
        n += r;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        left -= (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
    }
    out[n] = 0;
    close(fds[0]);
    kill(-pid, SIGKILL); kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);                        /* worker thread: blocking here never touches the UI */
    return n;
}
/* Result of the last probe, shared between the worker threads and the UI thread. Every route change (a route
 * to a device, back to analog, or a lost link) bumps gen; a probe result carries the gen it was started under and is
 * dropped if it is no longer current, so a stale answer can never trigger a codec switch on a later connection. */
static struct { char mac[18]; int state; /* 0 idle, 1 running, 2 done */ int pcm, known; unsigned avail;
                int choice, misses, armed, applied; unsigned gen; } g_cq;
static pthread_mutex_t g_cq_mu = PTHREAD_MUTEX_INITIALIZER;
#define BT_PCM_TRIES 15        /* probe ticks (2 s apart, ~30 s) to wait for the PCM before giving up (stays on SBC) */
struct bt_probe_arg { unsigned gen; char mac[18]; };
static void *bt_codec_probe_thread(void *arg){
    struct bt_probe_arg *pa = arg;
    char cmd[240], info[1536];
    info[0] = 0;
    if(bt_codec_query_cmd(pa->mac, cmd, sizeof cmd) == 0) bt_probe_run(cmd, info, sizeof info, 700);
    int known = 0; unsigned avail = bt_codec_parse_avail(info, &known);
    pthread_mutex_lock(&g_cq_mu);
    if(pa->gen == g_cq.gen){                       /* still the route this probe was started for */
        g_cq.pcm = strstr(info, "PCM=") != NULL; g_cq.known = known; g_cq.avail = avail; g_cq.state = 2;
    }
    pthread_mutex_unlock(&g_cq_mu);
    free(pa);
    return NULL;
}
/* The route frame: 06b3 + length + VALUE1 0 (SBC) + MAC - byte-for-byte what diskOS has always sent, so routing
 * itself never waits on codec state. Returns 0, or -1 for a bad MAC / short buffer. */
int ui_bt_codec_frame(const char *mac, char *out, int cap){
    return bt_codec_frame_value(mac, 0, out, cap);
}
static int bt_codec_frame_value(const char *mac, int v, char *out, int cap){
    if(!bt_mac_valid(mac)) return -1;
    int r = snprintf(out, cap, "06b3%04X%04X%s", (unsigned)(12 + 17), (unsigned)v, mac);
    return (r > 0 && r < cap) ? 0 : -1;
}
/* AAC/LDAC. The route's 06c1 makes the player (re)start bluetoothd and BlueALSA (mq_player_257.dis: bt_source_control
 * killall + relaunch), so the device's A2DP PCM appears only AFTER the route, and the player's codec worker drops a
 * 06b3 sent while it is missing. So the route goes out with SBC as always (audio starts at once) and this then waits,
 * off the UI thread, for the PCM: when it shows and lists the chosen codec, the choice is sent as a second 06b3.
 * One step per call (UI timer tick); returns 1 when finished (sent, unavailable, gave up, or the route changed). */
int bt_codec_upgrade_step(void){
    char mac[18], f[48]; int send = -1, done = 0; struct bt_probe_arg *spawn = NULL;
    pthread_mutex_lock(&g_cq_mu);
    snprintf(mac, sizeof mac, "%s", g_cq.mac);
    if(!g_cq.armed || strcmp(ui_route_mac(), mac)) done = 1;                 /* cancelled or routed away: forget it */
    else if(g_cq.state == 2){
        if(g_cq.pcm){
            int v = bt_codec_pick(g_cq.choice, g_cq.avail, g_cq.known);      /* unknown/unlisted -> SBC = nothing to do */
            if(v != 0) send = v;
            done = 1; g_cq.applied = 1;
        } else if(++g_cq.misses >= BT_PCM_TRIES){ done = 1; g_cq.applied = 1; }
        g_cq.state = 0;
    }
    if(!done && g_cq.state == 0 && (spawn = malloc(sizeof *spawn)) != NULL){
        spawn->gen = g_cq.gen; snprintf(spawn->mac, sizeof spawn->mac, "%s", mac); g_cq.state = 1;
    }
    if(done) g_cq.armed = 0;
    pthread_mutex_unlock(&g_cq_mu);
    if(send > 0 && bt_codec_frame_value(mac, send, f, sizeof f) == 0) ipc_send_cmd(f);
    if(spawn){ pthread_t t;
               if(pthread_create(&t, NULL, bt_codec_probe_thread, spawn) == 0) pthread_detach(t);
               else { pthread_mutex_lock(&g_cq_mu); if(spawn->gen == g_cq.gen) g_cq.state = 0; pthread_mutex_unlock(&g_cq_mu); free(spawn); } }
    return done;
}
static lv_timer_t *g_cq_timer;
static void bt_codec_upgrade_cb(lv_timer_t *t){
    if(bt_codec_upgrade_step()){ lv_timer_del(t); g_cq_timer = NULL; }
}
/* Any route change invalidates a pending upgrade and every probe still running for it. */
void ui_bt_codec_upgrade_cancel(void){
    pthread_mutex_lock(&g_cq_mu);
    g_cq.gen++; g_cq.armed = 0; g_cq.applied = 0; g_cq.state = 0;
    pthread_mutex_unlock(&g_cq_mu);
}
/* Called after a route to `mac`. fresh = a full route sequence just re-sent the SBC frame: it cancels whatever was
 * pending and, if AAC/LDAC is chosen, arms a new upgrade. fresh = 0 (already routed there, e.g. a tap) keeps a
 * pending or finished upgrade as it is. SBC (the default) leaves nothing armed: no timer, probe or extra frame. */
void ui_bt_codec_upgrade_arm(const char *mac, int fresh){
    if(!bt_mac_valid(mac)) return;
    int choice = cfg_get_int("bt_codec", 0);
    if(choice < 0 || choice >= BT_CODEC_N) choice = 0;
    if(fw_os_ver() != 257) choice = 0;   /* the 06b3 codec mapping is only decoded for V2.57: elsewhere stay on SBC */
    pthread_mutex_lock(&g_cq_mu);
    int same = !strcmp(g_cq.mac, mac) && (g_cq.armed || g_cq.applied);
    if(!fresh && (same || choice == 0)){ pthread_mutex_unlock(&g_cq_mu); return; }
    g_cq.gen++; g_cq.armed = 0; g_cq.applied = 0; g_cq.state = 0; g_cq.misses = 0;
    snprintf(g_cq.mac, sizeof g_cq.mac, "%s", mac);
    if(choice > 0){ g_cq.choice = choice; g_cq.armed = 1; }
    pthread_mutex_unlock(&g_cq_mu);
    if(choice > 0 && !g_cq_timer) g_cq_timer = lv_timer_create(bt_codec_upgrade_cb, 2000, NULL);
}
/* Full stock bring-up: download the chip firmware patch over /dev/ttyS0 (creates a
 * working hci0 - the step our old bluez-only path lacked), then bluetoothd + agent. */
static void bt_enable(void){
    /* Guard against a SECOND concurrent bring-up: if the marker is already present a bring-up is
     * in flight, and starting another detached subshell would have the two kill each other's
     * patchram/daemons (e.g. boot-restore + a user toggle firing close together) -> BT left broken.
     * bt_disable() removes the marker, so a real off->on still re-enables. */
    if(access("/tmp/bt_enabling", F_OK) == 0) return;
    /* Create the cancel marker SYNCHRONOUSLY before the backgrounded subshell, so it exists by
     * the time we return. bt_enable()/bt_disable() are both main-thread + serialized, so a
     * later bt_disable() `rm` always beats the async subshell - no touch-vs-rm race. */
    sync_system("touch /tmp/bt_enabling 2>/dev/null");
    bg_system(
        "( killall -9 fiio_bluetoothctl brcm_patchram_plus bluetoothd bluealsa 2>/dev/null; "
        "  hciconfig hci0 down 2>/dev/null; "
        /* power-cycle the BT core (BT_REG_ON via rfkill) BEFORE patchram, so the chip
         * re-syncs whether this is a fresh boot OR a re-enable after a prior patchram
         * (without the block/unblock cycle, re-patchram on an already-firmwared chip
         * hangs and hci0 never appears - verified on-device). */
        "  rfkill block bluetooth; sleep 1; "
        "  [ -e /tmp/bt_enabling ] || exit; "     /* disabled during the block? leave BT blocked (off) */
        "  rfkill unblock bluetooth; sleep 1; "
        "  [ -e /tmp/bt_enabling ] || exit; "
        "  brcm_patchram_plus --enable_lpm --enable_hci --no2bytes --tosleep 200000 --baudrate 3000000 "
        "    --patchram /lib/firmware/bt_bcm/BCM4343A1_001.002.009.1026.1055.hcd /dev/ttyS0 >/tmp/patchram.log 2>&1 & "
        /* patchram can take well over 5s to attach hci0; keep retrying `up` until it's
         * RUNNING (up to ~20s) so we never give up before the chip is ready and leave hci0
         * DOWN (which made bt_on() report "off" and the toggle time out). Each pass re-checks
         * the marker so a toggle-off aborts the loop promptly. */
        "  i=0; while [ \"$i\" -lt 40 ]; do [ -e /tmp/bt_enabling ] || exit; hciconfig hci0 up 2>/dev/null; "
        "    hciconfig hci0 2>/dev/null | grep -q RUNNING && break; sleep 0.5; i=$((i+1)); done; "
        "  [ -e /tmp/bt_enabling ] || exit; "     /* disabled during bring-up -> don't start the daemons */
        "  /usr/project/bluetoothd --noplugin=sap --plugin=a2dp,avrcp --mode=source >/tmp/btd.log 2>&1 & "
        "  bluealsa -S --device=hci0 --profile=a2dp-source --sbc-quality=medium "
        "    --ldac-abr --ldac-quality=standard --codec=sbc --initial-volume=48 >/tmp/bluealsa.log 2>&1 & "
        /* hci0 UP alone leaves bluez Powered:no - power the controller on explicitly (needs
         * bluetoothd, hence after its start) so bt_on()'s "Powered: yes" check passes. */
        "  sleep 1; [ -e /tmp/bt_enabling ] || exit; "
        "  bluetoothctl power on; hciconfig hci0 piscan; hciconfig hci0 class 0x200414; "
        "  bluetoothctl agent on; bluetoothctl default-agent; bluetoothctl pairable on; "
        "  rm -f /tmp/bt_enabling "     /* enable finished: drop the marker */
        ") >/dev/null 2>&1 &");
    /* NB: bluealsa is started INSIDE the subshell above (after hci0 is RUNNING +
     * bluetoothd), not here, so it never races the patchram attach. */
}
static void bt_disable(void){
    scan_abort();          /* cancel any pending/active scan + the bt_open observer (covers the radio-timeout
                            * OFF path, which reaches here without a caller-side scan_abort). LVGL-touching,
                            * so it MUST stay on the main thread. */
    bt_autoroute_stop();
    /* Tear the stack down in a BACKGROUNDED subshell so the UI never blocks: `bluetoothctl power off`
     * stalls for the D-Bus timeout if bluetoothd is wedged, and running that synchronously on the main
     * thread froze the whole UI on a BT-off tap (Reddit report 2026-09-21). The device has no `timeout`
     * binary, so instead of a graceful daemon power-off (which could hang the subshell before the kill)
     * we KILL the daemons FIRST - direct, fast, no D-Bus round-trip - then drop the interface and
     * rfkill-block. rm the enable marker first so any in-flight bt_enable() subshell aborts. */
    bg_system("( rm -f /tmp/bt_enabling; "
           "killall -9 bluealsa bluetoothd brcm_patchram_plus fiio_bluetoothctl bt-agent 2>/dev/null; "
           "hciconfig hci0 down >/dev/null 2>&1; "
           "rfkill block bluetooth >/dev/null 2>&1 ) >/dev/null 2>&1 &");
}

/* A Bluetooth MAC must be exactly AA:BB:CC:DD:EE:FF (hex + colons) before it is ever
 * interpolated into a bluetoothctl shell command. bluez scan output is already this
 * format, so this is defensive: a malformed/hostile address can carry no shell
 * metacharacters past this gate. */
static int bt_mac_valid(const char *mac){
    if(!mac) return 0;
    for(int i = 0; i < 17; i++){
        char c = mac[i];
        if((i % 3) == 2){ if(c != ':') return 0; }
        else if(!((c>='0'&&c<='9')||(c>='A'&&c<='F')||(c>='a'&&c<='f'))) return 0;
    }
    return mac[17] == 0;
}

static int bt_dev_connected(const char *mac);
static int g_bt_autoroute_misses = 0;

/* Route once per connected bluealsa A2DP sink. Keeping the MAC latched while the
 * PCM exists preserves a manual switch back to analog until the sink reconnects. */
static void bt_autoroute_poll_cb(lv_timer_t *t){
    (void)t;
    /* The post-restart settle guard lives in ui_route_bt (covers every routing path); this poll keeps
     * firing during the window and routes on the first tick past it (ui_route_bt returns "not routed"
     * meanwhile, so g_bt_autorouted is not latched and the retry stands). */
    char path[512], mac[20];
    int found = 0;
    /* bounded: bluealsa-cli talks to bluealsa; run directly without shell pipe to avoid extra forks. */
    if(run_cap_bounded("bluealsa-cli list-pcms 2>/dev/null", path, sizeof path, 500) > 0){
        char *line = path;
        while(line && *line && !found){
            char *next = strchr(line, '\n');
            if(next) *next = '\0';
            /* Match an A2DP sink endpoint (a2dpsrc / a2dp-source) */
            if(strstr(line, "a2dp") && (strstr(line, "a2dpsrc") || strstr(line, "a2dp-source"))){
                char *dev = strstr(line, "dev_");
                if(dev){
                    dev += 4;
                    char *slash = strchr(dev, '/');
                    if(slash && slash - dev == 17){
                        memcpy(mac, dev, 17); mac[17] = 0;
                        for(int i = 0; i < 17; i++) if(mac[i] == '_') mac[i] = ':';
                        found = bt_mac_valid(mac);
                    }
                }
            }
            if(!next) break;
            line = next + 1;
        }
    }
    if(!found){
        if(g_bt_autorouted[0]){
            if(++g_bt_autoroute_misses >= 3){
                /* Only tear down the route and toast if the device is genuinely disconnected */
                if(!bt_radio_on() || !bt_dev_connected(g_bt_autorouted)){
                    g_bt_autorouted[0] = 0;
                    g_bt_autoroute_misses = 0;
                    ui_route_analog();
                    ui_toast("Bluetooth disconnected");
                    ui_bt_codec_upgrade_cancel();
                } else {
                    /* Device is still connected at BT layer (e.g. codec renegotiation or BlueALSA settling) */
                    bt_ensure_services();
                }
            }
        } else {
            ui_bt_codec_upgrade_cancel();
        }
        return;
    }
    g_bt_autoroute_misses = 0;
    if(strcmp(mac, g_bt_autorouted)){
        if(ui_route_bt(mac) == 0)     /* latch only on a successful route, else retry on the next poll */
            snprintf(g_bt_autorouted, sizeof g_bt_autorouted, "%s", mac);
    }
}

static void bt_autoroute_start(void){
    if(g_bt_autoroute_timer) return;
    g_bt_autoroute_misses = 0;
    g_bt_autoroute_timer = lv_timer_create(bt_autoroute_poll_cb, 3000, NULL);
}
static void bt_autoroute_stop(void){
    if(g_bt_autoroute_timer){ lv_timer_del(g_bt_autoroute_timer); g_bt_autoroute_timer = NULL; }
    g_bt_autorouted[0] = 0;
    g_bt_autoroute_misses = 0;
}
/* The player restarted: a fresh mq_player defaults to local/analog output, so any "already routed to X"
 * memory is stale. Forget it so the auto-route poll re-routes the still-connected speaker (and so the
 * local re-init isn't wrongly suppressed). If BT is on but the poll timer died, re-arm it. */
void bt_notify_player_restart(void){
    g_bt_autorouted[0] = 0;
    g_bt_autoroute_misses = 0;
    if(bt_on() && !g_bt_autoroute_timer) bt_autoroute_start();
}

/* ---- pair + connect ----------------------------------------------------- */
/* Extract a `bluetoothctl info` property value: the line whose first non-blank token is `key`
 * (e.g. "Connected:" / "Icon:"), returning its trimmed value. Line-anchored so a Name:/Alias:
 * value that merely CONTAINS the text can't spoof it. Fills out + returns 1 on match, else out="",0. */
static int bt_info_prop(const char *buf, const char *key, char *out, int cap){
    if(cap <= 0) return 0;
    size_t klen = strlen(key);
    for(const char *l = buf; l && *l; ){
        const char *p = l; while(*p==' '||*p=='\t') p++;
        if(!strncmp(p, key, klen)){
            p += klen; while(*p==' '||*p=='\t') p++;
            int i=0; while(p[i] && p[i]!='\n' && p[i]!='\r' && i<cap-1){ out[i]=p[i]; i++; }
            while(i>0 && (out[i-1]==' '||out[i-1]=='\t')) i--;   /* trim trailing whitespace so "yes " compares == "yes" */
            out[i]=0; return 1;
        }
        const char *nl = strchr(l, '\n'); l = nl ? nl+1 : NULL;
    }
    out[0]=0;
    return 0;
}
/* a device is connected when `bluetoothctl info <mac>` reports "Connected: yes" or kernel has an active ACL */
static int bt_dev_connected(const char *mac){
    if(!bt_mac_valid(mac)) return 0;
    char cmd[160], buf[2048];
    snprintf(cmd, sizeof cmd, "bluetoothctl info %s 2>/dev/null", mac);
    if(run_cap_bounded(cmd, buf, sizeof buf, 350) > 0){
        /* match the "Connected:" PROPERTY line (after indentation), not a Name:/Alias:
         * that merely contains the text "Connected: yes". */
        for(char *l = buf; l && *l; ){
            char *nl = strchr(l, '\n'); if(nl) *nl = 0;
            char *p = l; while(*p == ' ' || *p == '\t') p++;
            if(!strncmp(p, "Connected:", 10)) return strstr(p, "yes") != NULL;
            if(!nl) break;
            l = nl + 1;
        }
    }
    /* Fallback: kernel HCI level connection check (direct, no D-Bus dependency) */
    if(run_cap_bounded("hcitool con 2>/dev/null", buf, sizeof buf, 200) > 0){
        char upper_mac[20];
        for(int i = 0; i < 17; i++){
            char c = mac[i];
            if(c >= 'a' && c <= 'z') c -= 32;
            upper_mac[i] = c;
        }
        upper_mac[17] = 0;
        if(strstr(buf, upper_mac)) return 1;
    }
    return 0;
}

/* honest completion: pair+connect is async (and slow), so poll for the real
 * result for up to 25s and toast it, instead of silently firing-and-forgetting. */
static lv_timer_t *g_bt_conn_timer;
static uint32_t    g_bt_conn_start;
static char        g_bt_conn_mac[32];
static void bt_conn_poll_cb(lv_timer_t *t){
    (void)t;
    if(bt_dev_connected(g_bt_conn_mac)){
        lv_timer_del(g_bt_conn_timer); g_bt_conn_timer = NULL;
        /* Connected at the BT layer. Only claim "Connected" (audio routed) if the route sequence
         * actually went through; otherwise stay honest and leave g_bt_autorouted unset so the
         * autoroute poll keeps retrying the route. */
        if(ui_route_bt(g_bt_conn_mac) == 0){
            snprintf(g_bt_autorouted, sizeof g_bt_autorouted, "%s", g_bt_conn_mac);
            g_bt_autoroute_misses = 0;
            ui_toast("Connected");
        } else {
            ui_toast("Paired - audio stays on player");
        }
        scan_kick();                        /* instant re-list (device already known) -> ✓, no 13s re-scan */
        return;
    }
    if(lv_tick_elaps(g_bt_conn_start) > 25000){   /* pairing can be slow */
        lv_timer_del(g_bt_conn_timer); g_bt_conn_timer = NULL;
        ui_toast("Couldn't connect");
        scan_kick();                        /* re-list the current devices, no fresh discovery */
    }
}

static void bt_connect(const char *mac){
    if(!bt_mac_valid(mac)){ ui_toast("Bad device address"); return; }
    char cmd[256];
    snprintf(cmd, sizeof cmd,
             "( bluetoothctl pair %s; bluetoothctl trust %s; bluetoothctl connect %s ) >/dev/null 2>&1 &",
             mac, mac, mac);
    bg_system(cmd);
    snprintf(g_bt_conn_mac, sizeof g_bt_conn_mac, "%s", mac);
    ui_toast("Connecting...");
    g_bt_conn_start = lv_tick_get();
    if(g_bt_conn_timer) lv_timer_del(g_bt_conn_timer);
    g_bt_conn_timer = lv_timer_create(bt_conn_poll_cb, 1500, NULL);
}
static void bt_disconnect(const char *mac){
    if(!bt_mac_valid(mac)) return;
    if(!strcmp(mac, g_bt_autorouted)) g_bt_autorouted[0] = 0;
    g_bt_autoroute_misses = 0;
    ui_route_analog();          /* return audio to the DAC before dropping the A2DP link */
    char cmd[128];
    snprintf(cmd, sizeof cmd, "bluetoothctl disconnect %s >/dev/null 2>&1 &", mac);
    bg_system(cmd);
    ui_toast("Disconnecting...");
}

/* ---- details screen (SCR_BT_INFO) --------------------------------------- */
static void info_row(const char *key, const char *val){
    lv_obj_t *r = lv_obj_create(g_info_list);
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
    if(bt_mac_valid(g_sel_mac)){
        bt_disconnect(g_sel_mac);   /* route audio back to analog + clear g_route/autoroute BEFORE removing the sink */
        char cmd[128];
        snprintf(cmd, sizeof cmd, "( bluetoothctl remove %s ) >/dev/null 2>&1 &", g_sel_mac);
        bg_system(cmd);
        ui_toast("Device forgotten");
    }
    screen_back();
    if(g_scan_timer) lv_timer_del(g_scan_timer);
    g_scan_timer = lv_timer_create(scan_timer_cb, 3000, NULL);
    lv_timer_set_repeat_count(g_scan_timer, 1);
}
static void info_action_row(const char *label, lv_event_cb_t cb){
    lv_obj_t *r = lv_button_create(g_info_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 250, 44);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, TC(DANGER_SURFACE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, TC(DANGER_SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(r, cb, LV_EVENT_CLICKED, NULL, "bt.cb", UI_CORE);
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, label); lv_obj_center(t);
    lv_obj_set_style_text_font(t, TF(UI_14), 0);
    lv_obj_set_style_text_color(t, TC(STATUS_DANGER), 0);
}
static void info_disc_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    bt_disconnect(g_sel_mac);
    screen_back();
    /* re-list shortly so the now-disconnected device loses its ✓ / blue styling
     * (mirrors the connect path; disconnect is async via bluetoothctl). */
    if(g_scan_timer) lv_timer_del(g_scan_timer);
    g_scan_timer = lv_timer_create(scan_timer_cb, 4000, NULL);
    lv_timer_set_repeat_count(g_scan_timer, 1);
}
static void info_back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

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

    /* Disconnect button under the info rows */
    lv_obj_t *db = lv_button_create(root);
    lv_obj_remove_style_all(db);
    lv_obj_set_pos(db, 110, 286); lv_obj_set_size(db, 140, 38);
    lv_obj_set_style_radius(db, 12, 0);
    lv_obj_set_style_bg_color(db, TC(DANGER_PANEL_SURFACE), 0);
    lv_obj_set_style_bg_opa(db, LV_OPA_COVER, 0);
    ui_on(db, info_disc_cb, LV_EVENT_CLICKED, NULL, "bt.info_disc", UI_CORE);
    lv_obj_t *dl = lv_label_create(db);
    lv_label_set_text(dl, "Disconnect");
    lv_obj_center(dl);
    lv_obj_set_style_text_font(dl, TF(UI_16), 0);
    lv_obj_set_style_text_color(dl, TC(STATUS_DANGER_STRONG), 0);
}

void bt_info_open(void){
    if(!g_info_list || !bt_mac_valid(g_sel_mac)) return;
    lv_obj_clean(g_info_list);
    char cmd[128], buf[2048], val[96];
    snprintf(cmd, sizeof cmd, "bluetoothctl info %s 2>/dev/null", g_sel_mac);
    run_cap_bounded(cmd, buf, sizeof buf, 400);
    char *p = strstr(buf, "Name: ");
    if(p){ sscanf(p+6, "%95[^\n]", val); info_row("Name", val); }
    info_row("Address", g_sel_mac);
    int is_conn = 0;
    { char v[96];   /* line-anchored property reads: a device NAME can't spoof Connected/Icon */
      is_conn = (bt_info_prop(buf,"Connected:",v,sizeof v) && !strcmp(v,"yes"));
      info_row("Connected", is_conn ? "Yes" : "No");
      info_row("Paired",    (bt_info_prop(buf,"Paired:",   v,sizeof v) && !strcmp(v,"yes")) ? "Yes" : "No");
      if(bt_info_prop(buf,"Icon:",v,sizeof v)) info_row("Type", v); }

    /* Active Codec query from BlueALSA */
    char codec_val[64] = "-";
    if(is_conn || !strcmp(g_sel_mac, ui_route_mac())){
        char bcmd[256], binfo[1536];
        if(bt_codec_query_cmd(g_sel_mac, bcmd, sizeof bcmd) == 0 &&
           run_cap_bounded(bcmd, binfo, sizeof binfo, 400) > 0){
            char *sc = strstr(binfo, "Selected codec:");
            if(sc){
                sc += 15;
                while(*sc == ' ' || *sc == '\t') sc++;
                char *eol = strpbrk(sc, "\r\n");
                int clen = eol ? (int)(eol - sc) : (int)strlen(sc);
                if(clen > 0 && clen < 24){
                    char cname[32];
                    memcpy(cname, sc, clen); cname[clen] = '\0';
                    char *smp = strstr(binfo, "Sampling:");
                    int khz = 0;
                    if(smp && sscanf(smp + 9, "%d", &khz) == 1 && khz > 0){
                        snprintf(codec_val, sizeof codec_val, "%s (%d kHz)", cname, khz / 1000);
                    } else {
                        snprintf(codec_val, sizeof codec_val, "%s", cname);
                    }
                }
            }
        }
    }
    info_row("Codec", codec_val);
    info_row("Audio", "On (beta)");   /* routing works; SBC over this CPU can be rough. short: value label is 142px */
    info_action_row("Forget This Device", info_forget_cb);   /* C13: unpair + untrust */
    screen_show(SCR_BT_INFO);
}

/* ---- device list -------------------------------------------------------- */
static void row_free_cb(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static void dev_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    lv_obj_t *row = lv_event_get_target(e);
    const char *mac = (const char*)lv_obj_get_user_data(row);
    int connected = (intptr_t)lv_event_get_user_data(e);
    if(!mac || !mac[0]) return;
    if(connected){
        snprintf(g_sel_mac, sizeof g_sel_mac, "%s", mac);
        ui_route_bt(mac);        /* tapping a connected audio device routes playback to it (no-op if already) */
        bt_info_open();
    } else {
        bt_connect(mac);
        list_msg("Connecting " LV_SYMBOL_BLUETOOTH);
        /* re-list shortly so the new connection picks up its ✓ */
        if(g_scan_timer) lv_timer_del(g_scan_timer);
        g_scan_timer = lv_timer_create(scan_timer_cb, 4000, NULL);
        lv_timer_set_repeat_count(g_scan_timer, 1);
    }
}

static void add_dev_row(const char *mac, const char *name, int connected){
    lv_obj_t *r = lv_button_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 268, 46);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, (connected ? TC(CONNECTED_SURFACE) : TC(SURFACE)), 0);
    lv_obj_set_style_bg_opa(r, connected ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    char *dup = strdup(mac);
    lv_obj_set_user_data(r, dup);
    ui_on(r, dev_cb, LV_EVENT_CLICKED, (void*)(intptr_t)connected, "bt.dev", UI_CORE);
    lv_obj_add_event_cb(r, row_free_cb, LV_EVENT_DELETE, NULL);

    int tx = 12;
    if(connected){
        lv_obj_t *ck = lv_label_create(r);
        lv_label_set_text(ck, LV_SYMBOL_OK);
        lv_obj_set_pos(ck, 12, 15);
        lv_obj_set_style_text_font(ck, TF(UI_14), 0);
        lv_obj_set_style_text_color(ck, TC(STATUS_INFO), 0);
        tx = 34;
    }
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, name);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(t, tx, 13); lv_obj_set_size(t, 220 - tx, 20);
    lv_obj_set_style_text_font(t, TF(USER_16), 0);   /* BT device names are user data: Cyrillic/CJK-capable (issue #3) */
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);

    lv_obj_t *ic = lv_label_create(r);
    lv_label_set_text(ic, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_pos(ic, 240, 14);
    lv_obj_set_style_text_font(ic, TF(UI_14), 0);
    lv_obj_set_style_text_color(ic, (connected ? TC(STATUS_INFO) : TC(TEXT_MUTED)), 0);
}

/* Show only real AUDIO devices (bluez Icon = audio-card / audio-headset / ...),
 * so BLE gadgets and the "LE-" advertisement entries (which carry no A2DP) are
 * hidden and the user can only tap a connectable sink.  Connected ✓ comes from
 * bluez's own "Connected: yes" (not a raw ACL link, which over-reports). */
/* Enumeration runs OFF the main thread: `bluetoothctl devices` plus a per-device
 * `bluetoothctl info` is up to ~60 blocking popen() spawns - running that inline froze
 * the LVGL loop for seconds (H5) and risked the fiio_init hardware watchdog. The worker
 * builds ONLY plain data; the main thread renders rows from it (LVGL is main-thread-only).
 * A generation counter drops a superseded worker's result if a newer scan started. */
typedef struct { char mac[20]; char name[128]; int connected; } bt_scan_dev_t;
static pthread_mutex_t g_scan_mu = PTHREAD_MUTEX_INITIALIZER;
static bt_scan_dev_t   g_scan_res[40];
static int             g_scan_n = 0;         /* [g_scan_mu] result count */
static int             g_scan_done = 0;      /* [g_scan_mu] worker finished -> main renders */
static int             g_scan_running = 0;   /* [g_scan_mu] a worker is in flight */
static unsigned        g_scan_gen = 0;       /* [g_scan_mu] bumped per scan; worker tags its result */
static lv_timer_t     *g_scanpoll_timer;
static int             g_scanpoll_n = 0;
static int             g_scan_pending = 0;   /* main-thread: a re-enumerate was requested while a worker ran */

static void *scan_worker(void *arg){
    unsigned my_gen = (unsigned)(intptr_t)arg;
    char buf[8192]; bt_probe_run("bluetoothctl devices 2>/dev/null", buf, sizeof buf, 800);
    bt_scan_dev_t res[40]; int n = 0;
    char *l = buf;
    while(l && *l && n < 40){
        char *nl = strchr(l, '\n'); if(nl) *nl = 0;
        /* line: "Device AA:BB:CC:DD:EE:FF Friendly Name" */
        if(!strncmp(l, "Device ", 7) && strlen(l) > 7+17){
            char mac[20]; memcpy(mac, l+7, 17); mac[17]=0;
            const char *name = l + 7 + 17 + 1;
            if(name[0] && bt_mac_valid(mac)){
                char cmd[128], info[2048];
                snprintf(cmd, sizeof cmd, "bluetoothctl info %s 2>/dev/null", mac);
                bt_probe_run(cmd, info, sizeof info, 500);
                char icon[64], cv[16];   /* line-anchored: a spoofed device name can't fake Icon/Connected */
                if(bt_info_prop(info, "Icon:", icon, sizeof icon) && !strncmp(icon, "audio", 5)){  /* audio sinks only */
                    snprintf(res[n].mac,  sizeof res[n].mac,  "%s", mac);
                    snprintf(res[n].name, sizeof res[n].name, "%s", name);
                    res[n].connected = (bt_info_prop(info,"Connected:",cv,sizeof cv) && !strcmp(cv,"yes"));
                    n++;
                }
            }
        }
        if(!nl) break;
        l = nl + 1;
    }
    pthread_mutex_lock(&g_scan_mu);
    if(my_gen == g_scan_gen){        /* still current (not abandoned): publish results + free the latch.
                                      * Only the current-gen worker clears g_scan_running, so an
                                      * abandoned worker can't stomp a newer scan's state. */
        memcpy(g_scan_res, res, (size_t)n * sizeof(bt_scan_dev_t));
        g_scan_n = n; g_scan_done = 1;
        g_scan_running = 0;
    }
    pthread_mutex_unlock(&g_scan_mu);
    return NULL;
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
    pthread_mutex_unlock(&g_scan_mu);
    for(int i = 0; i < n; i++) if(local[i].connected)  add_dev_row(local[i].mac, local[i].name, 1);  /* connected first */
    for(int i = 0; i < n; i++) if(!local[i].connected) add_dev_row(local[i].mac, local[i].name, 0);
    if(n == 0) list_msg("No audio devices found");
    /* NOTE: audio output routing to the BT sink is intentionally DISABLED.
     * bluez+bluealsa does SBC encoding in software, which is too heavy for this
     * MIPS CPU (laggy audio + watchdog reboots), and the 0666 output-mux command
     * destabilises mq_player's local audio engine.  The page still pairs/connects
     * devices, but the player always uses its native DAC output.  Smooth BT audio
     * would require the stock Broadcom BSA (hardware a2dp), a separate effort. */
}

/* main thread: poll until the worker finishes, then render. Keeps waiting (does NOT
 * render stale results early); only a wedged worker (~30s) is abandoned - and then the
 * generation is bumped + the latch freed so its late write is dropped and new scans work. */
static void scanpoll_cb(lv_timer_t *t){
    (void)t;
    int done;
    pthread_mutex_lock(&g_scan_mu); done = g_scan_done; pthread_mutex_unlock(&g_scan_mu);
    if(!done){
        if(++g_scanpoll_n < 100) return;   /* keep waiting (~30s) for the worker */
        pthread_mutex_lock(&g_scan_mu); g_scan_gen++; g_scan_running = 0; pthread_mutex_unlock(&g_scan_mu);
    }
    if(g_scanpoll_timer){ lv_timer_del(g_scanpoll_timer); g_scanpoll_timer = NULL; }
    if(g_scan_pending){ g_scan_pending = 0; scan_kick(); return; }   /* a refresh was queued mid-run -> re-enumerate fresh instead of rendering the stale result */
    scan_render();
}

/* (re)start the render poll timer (main thread) */
static void scan_ensure_poll(void){
    g_scanpoll_n = 0;
    if(g_scanpoll_timer) lv_timer_del(g_scanpoll_timer);
    g_scanpoll_timer = lv_timer_create(scanpoll_cb, 300, NULL);
}

/* main thread: spawn the off-thread enumeration. If one is already running, just make
 * sure the poll is live so its result still renders (don't pile up a second worker). */
static void scan_kick(void){
    pthread_mutex_lock(&g_scan_mu);
    if(g_scan_running){   /* a worker is mid-run: queue a fresh re-enumerate for when it finishes,
                           * so a stale (e.g. pre-connect) result isn't the final render */
        pthread_mutex_unlock(&g_scan_mu); g_scan_pending = 1; scan_ensure_poll(); return;
    }
    g_scan_running = 1; g_scan_done = 0; g_scan_n = 0; g_scan_gen++;   /* clear stale results: a timed-out/failed scan then renders honestly, not the last list */
    unsigned gen = g_scan_gen;
    pthread_mutex_unlock(&g_scan_mu);
    g_scan_pending = 0;   /* this fresh worker satisfies any queued refresh */
    pthread_t th;
    if(pthread_create(&th, NULL, scan_worker, (void*)(intptr_t)gen) == 0){
        pthread_detach(th);
        scan_ensure_poll();     /* render when the worker signals done */
    } else {   /* couldn't spawn -> drop the latch, kill any stale poll, render what we have */
        pthread_mutex_lock(&g_scan_mu); g_scan_running = 0; pthread_mutex_unlock(&g_scan_mu);
        if(g_scanpoll_timer){ lv_timer_del(g_scanpoll_timer); g_scanpoll_timer = NULL; }
        scan_render();
    }
}

static void scan_timer_cb(lv_timer_t *t){
    (void)t;
    if(g_scan_timer){ lv_timer_del(g_scan_timer); g_scan_timer = NULL; }
    scan_kick();   /* enumerate off-thread; scanpoll_cb renders when done (no main-thread freeze) */
}
/* Cancel every pending scan artifact and abandon any in-flight worker (bump gen so its
 * late result is dropped). Used when Bluetooth is turned off so a delayed scan can't
 * render device rows over the "Bluetooth is off" message. */
static void scan_abort(void){
    if(g_scan_timer){ lv_timer_del(g_scan_timer); g_scan_timer = NULL; }
    if(g_scanpoll_timer){ lv_timer_del(g_scanpoll_timer); g_scanpoll_timer = NULL; }
    if(g_scanwait_timer){ lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL; }  /* cancel the bt_open observer too */
    pthread_mutex_lock(&g_scan_mu);
    g_scan_gen++; g_scan_running = 0; g_scan_n = 0; g_scan_done = 0;
    pthread_mutex_unlock(&g_scan_mu);
}
static void start_scan(void){
    if(!bt_on()){ list_msg("Bluetooth is off"); return; }
    list_msg_scanning();
    /* Classic BR/EDR inquiry - what BT speakers/headphones use - needs ~10-12s to find and
     * resolve a device's name/class; the old 6s window quit before speakers ever appeared. */
    bg_system("bluetoothctl --timeout 13 scan on >/dev/null 2>&1 &");
    if(g_scan_timer) lv_timer_del(g_scan_timer);
    g_scan_timer = lv_timer_create(scan_timer_cb, 13500, NULL);
    lv_timer_set_repeat_count(g_scan_timer, 1);
}

/* ---- radio toggle ------------------------------------------------------- */
/* bt_enable() is slow (rfkill cycle + patchram firmware download + hci0 bring-up,
 * several seconds). A fixed settle timer would either fire too early ("No audio
 * devices found" before the radio is up) or never actively scan. Poll bt_on()
 * until the adapter is Powered, then run a real scan. */
static lv_timer_t *g_radio_timer;
static uint32_t    g_radio_start;
static void radio_on_poll_cb(lv_timer_t *t){
    (void)t;
    if(bt_on()){
        lv_timer_del(g_radio_timer); g_radio_timer = NULL;
        bt_ensure_services();
        bt_autoroute_start();
        start_scan();
        return;
    }
    if(lv_tick_elaps(g_radio_start) > 28000){   /* 2s rfkill + ~20s up-loop + daemon/power settle */
        lv_timer_del(g_radio_timer); g_radio_timer = NULL;
        /* bring-up failed: drop the persisted intent so the QS tile + boot-restore don't keep
         * showing/enforcing "on" for a radio that never came up. Tear the stack down so the cleared
         * intent matches a real off state (a late/half-started enable can't linger powered), and
         * sync the screen switch. */
        cfg_set_int("bt_on", 0);
        if(g_sw) lv_obj_clear_state(g_sw, LV_STATE_CHECKED);
        bt_disable();
        list_msg("Couldn't turn on Bluetooth");
    }
}
static void sw_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_VALUE_CHANGED) return;
    int on = lv_obj_has_state(g_sw, LV_STATE_CHECKED);
    if(on){
        cfg_set_int("bt_on", 1);   /* persist intent so boot restores it (mirror WiFi) */
        bt_enable();
        list_msg("Turning on " LV_SYMBOL_BLUETOOTH);
        g_radio_start = lv_tick_get();
        if(g_scanwait_timer){ lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL; }  /* radio poll owns bring-up now - keep the two mutually exclusive */
        if(g_radio_timer) lv_timer_del(g_radio_timer);
        g_radio_timer = lv_timer_create(radio_on_poll_cb, 1000, NULL);
    } else {
        cfg_set_int("bt_on", 0);   /* OFF before teardown so boot-restore won't resurrect it */
        if(g_radio_timer){ lv_timer_del(g_radio_timer); g_radio_timer = NULL; }
        if(g_bt_conn_timer){ lv_timer_del(g_bt_conn_timer); g_bt_conn_timer = NULL; }  /* else its 25s timeout scan_kick()s while BT is off */
        scan_abort();          /* cancel any pending/active scan so it can't render over "off" */
        ui_route_analog();     /* return audio to the DAC before killing the BT stack */
        bt_disable();
        list_msg("Bluetooth is off");
    }
}

/* Quick Settings tile short-press: flip BT + persist intent, no screen-specific UI.
 * Mirrors sw_cb's actions (bt_boot_restore/keepalive enforce the intent). Returns new state. */
int bt_toggle(void){
    int on = !bt_radio_on();   /* flip the ACTUAL radio state, not the (possibly stale) persisted intent */
    cfg_set_int("bt_on", on);
    if(on){
        bt_enable();
        g_radio_start = lv_tick_get();
        if(g_scanwait_timer){ lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL; }
        if(g_radio_timer) lv_timer_del(g_radio_timer);
        g_radio_timer = lv_timer_create(radio_on_poll_cb, 1000, NULL);
    } else {
        if(g_radio_timer){ lv_timer_del(g_radio_timer); g_radio_timer = NULL; }        /* cancel the bring-up poll too */
        if(g_bt_conn_timer){ lv_timer_del(g_bt_conn_timer); g_bt_conn_timer = NULL; }  /* don't let it scan_kick() after BT off */
        scan_abort();          /* cancel any pending/active scan */
        ui_route_analog();     /* return audio to the DAC before killing the BT stack */
        bt_disable();
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
static lv_timer_t *g_bootrestore_timer;
static uint32_t    g_bootrestore_start;
static void bootrestore_poll_cb(lv_timer_t *t){
    (void)t;
    if(bt_on()){
        lv_timer_del(g_bootrestore_timer); g_bootrestore_timer = NULL;
        bt_ensure_services();
        bt_autoroute_start();          /* speaker auto-reconnects -> auto-route routes it */
        return;
    }
    if(lv_tick_elaps(g_bootrestore_start) > 28000){       /* patchram + power-on settle */
        lv_timer_del(g_bootrestore_timer); g_bootrestore_timer = NULL;
    }
}
void bt_boot_restore(void){
    bt_init_intent();
    if(cfg_get_int("bt_on", 0) != 1) return;              /* was off -> stay off */
    if(bt_on()){ bt_ensure_services(); bt_autoroute_start(); return; }   /* already up (UI restart) */
    bt_enable();
    g_bootrestore_start = lv_tick_get();
    if(g_bootrestore_timer) lv_timer_del(g_bootrestore_timer);
    g_bootrestore_timer = lv_timer_create(bootrestore_poll_cb, 1000, NULL);
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
    lv_obj_set_style_text_font(rl, TF(UI_16), 0);
    lv_obj_set_style_text_color(rl, TC(TEXT_SECONDARY), 0);
    lv_obj_center(rl);

    /* Honesty: audio routing to a BT speaker works now (auto-routes on connect), but SBC
     * sw-encode on this MIPS CPU can be rough (occasional artifacts) - label it beta. */
    lv_obj_t *note = lv_label_create(root);
    lv_label_set_text(note, "Connect a speaker to play audio (beta)");
    lv_obj_set_pos(note, 30, 120); lv_obj_set_width(note, 300);   /* clear of the card (and a theme's edge under it) */
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(note, TF(UI_12), 0);
    lv_obj_set_style_text_color(note, TC(TEXT_FOOTNOTE), 0);

    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 40, 138); lv_obj_set_size(g_list, 280, 180);
    lv_obj_set_style_pad_bottom(g_list, 44, 0);   /* last row scrolls clear of the round bottom bezel */
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(g_list, 6, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

/* Non-destructive "wait for the adapter, then scan" poll used when the BT screen is opened while the
 * radio is on/coming up (from a QS toggle, or brought up externally). Unlike radio_on_poll_cb it NEVER
 * powers BT off on a timeout - it just stops trying, so a flaky bt_on() probe can't kill a live radio. */
static void scanwait_poll_cb(lv_timer_t *t){
    (void)t;
    if(screen_current() != SCR_BT){                 /* user left the BT screen -> stop waiting */
        lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL; return;
    }
    if(bt_on()){
        lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL;
        bt_ensure_services(); bt_autoroute_start(); start_scan();
        return;
    }
    if(lv_tick_elaps(g_scanwait_start) > 28000){    /* gave up waiting - do NOT tear the radio down */
        lv_timer_del(g_scanwait_timer); g_scanwait_timer = NULL;
        list_msg(bt_radio_on() ? "Bluetooth is starting up" : "Bluetooth is off");
    }
}

void bt_open(void){
    /* "should BT be on" = the intent OR the live radio. Intent is the reliable signal right after a QS
     * toggle, because bt_enable() rfkill-blocks-then-unblocks, so bt_radio_on() reads 0 for a moment. */
    int want = cfg_get_int("bt_on", 0) || bt_radio_on();
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
    if(bt_on()){                                   /* fully up -> scan immediately */
        bt_ensure_services(); bt_autoroute_start(); start_scan();
    } else if(want){                                /* on/coming up (e.g. just toggled from Quick Settings)
                                                     * -> poll until Powered, then scan. NON-destructive:
                                                     * unlike radio_on_poll_cb, this never powers BT off on a
                                                     * timeout, so observing an externally-enabled radio (or a
                                                     * flaky bt_on() probe) can't kill a working controller. */
        list_msg_scanning();
        g_scanwait_start = lv_tick_get();
        if(g_scanwait_timer) lv_timer_del(g_scanwait_timer);
        g_scanwait_timer = lv_timer_create(scanwait_poll_cb, 1000, NULL);
    } else {
        list_msg("Bluetooth is off");
    }
}
