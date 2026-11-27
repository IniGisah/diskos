/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "modelock.h"
#include "bt_rx_logic.h"
#include "ui_wake.h"
#include "device_names.h"
#include "screens.h"
#include "anim.h"
#include "ipc.h"
#include "config.h"
#include "theme.h"
#include "theme_kit.h"
#include "i18n.h"
#include "btalias.h"
#include <stdatomic.h>
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
#include <dirent.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>

/* State of mode lockdown */
static int g_lock_mode = 0;
static lv_obj_t *g_lock_root = NULL;

/* Header widgets */
static lv_obj_t *g_hdr_title;

/* Mode containers (Mode 1 USB DAC, Mode 3 USB Storage; Mode 2 uses SCR_NOWPLAYING) */
static lv_obj_t *g_cont_dac;
static lv_obj_t *g_cont_storage;

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
static pthread_mutex_t g_worker_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_worker_cond;
static int g_worker_cond_init = 0;
static int g_worker_started = 0;
static int g_stop_pipe[2] = { -1, -1 };
static _Atomic int g_worker_run = 0;
static pthread_mutex_t g_time_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_bt_pairing_until = 0;
static _Atomic int s_bt_force_drop = 0;
static uint64_t s_bt_drop_until = 0;
static uint64_t s_rx_enter_ms = 0;
static uint64_t s_last_hci_heal_ms = 0;
static uint64_t s_last_btd_heal_ms = 0;
static char g_bt_player_path[96] = {0};
static lv_timer_t *g_poll_timer = NULL;

static uint64_t rx_u64_load(const uint64_t *p){
    pthread_mutex_lock(&g_time_mu);
    uint64_t v = *p;
    pthread_mutex_unlock(&g_time_mu);
    return v;
}

static void rx_u64_store(uint64_t *p, uint64_t v){
    pthread_mutex_lock(&g_time_mu);
    *p = v;
    pthread_mutex_unlock(&g_time_mu);
}

/* Fast interlaced inquiry/page scan tuning for BCM43438 in receiver mode */
#define BT_RX_FAST_INQ_SH \
    "hciconfig hci0 class 0x200414 2>/dev/null; " \
    "hciconfig hci0 inqtype 1 2>/dev/null; " \
    "hciconfig hci0 inqparms 36:512 2>/dev/null; " \
    "hciconfig hci0 pageparms 36:512 2>/dev/null; " \
    "hciconfig hci0 piscan 2>/dev/null"

/* Full D-Bus + HCI + agent bring-up for reliable phone discovery and pairing.
 * Uses bounded dbus-send (--reply-timeout=1000) instead of interactive bluetoothctl
 * commands that hang indefinitely whenever org.bluez D-Bus isn't ready. */
#define BT_RX_ENABLE_DISCOVERY_SH \
    "rfkill unblock bluetooth 2>/dev/null; " \
    "hciconfig hci0 up 2>/dev/null; " \
    "( pidof fiio_bluetoothctl >/dev/null 2>&1 || pidof bt-agent >/dev/null 2>&1 || bt-agent -c NoInputNoOutput -d >/dev/null 2>&1 & ); " \
    "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Powered variant:boolean:true >/dev/null 2>&1; " \
    "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:DiscoverableTimeout variant:uint32:0 >/dev/null 2>&1; " \
    "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:PairableTimeout variant:uint32:0 >/dev/null 2>&1; " \
    "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Pairable variant:boolean:true >/dev/null 2>&1; " \
    "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Discoverable variant:boolean:true >/dev/null 2>&1; " \
    BT_RX_FAST_INQ_SH

static uint64_t rx_now_ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

void bt_rx_log(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    char buf[128];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    diskos_trace("%s", buf);
}

/* Non-blocking background shell runner (double-fork to avoid zombies & UI stalls). */
static void rx_bg_system(const char *cmd){
    if(!cmd || !cmd[0]) return;
    pid_t pid = fork();
    if(pid < 0) return;
    if(pid == 0){
        setsid();
        pid_t pid2 = fork();
        if(pid2 < 0) _exit(127);
        if(pid2 > 0) _exit(0);
        int devnull = open("/dev/null", O_RDWR);
        if(devnull >= 0){
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if(devnull > 2) close(devnull);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int st;
    while(waitpid(pid, &st, 0) < 0 && errno == EINTR){}
}

/* Dedicated cancellable subprocess capture for RX paths (never consumes PROC_SLOTS). */
static int rx_run_cap_ex(const char *cmd, char *out, int maxlen, int timeout_ms,
                         int watch_stop_pipe, int *out_exit){
    if(out_exit) *out_exit = -1;
    if(out && maxlen > 0) out[0] = '\0';
    if(!cmd || !cmd[0]) return -1;

    int pfd[2];
    if(pipe2(pfd, O_CLOEXEC) != 0) return -1;

    pid_t pid = fork();
    if(pid < 0){
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if(pid == 0){
        setsid();
        int devnull = open("/dev/null", O_RDONLY);
        if(devnull >= 0){
            dup2(devnull, STDIN_FILENO);
            if(devnull > 2) close(devnull);
        }
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);

    int fl = fcntl(pfd[0], F_GETFL, 0);
    if(fl >= 0) fcntl(pfd[0], F_SETFL, fl | O_NONBLOCK);

    int stop_rd = watch_stop_pipe ? g_stop_pipe[0] : -1;
    uint64_t deadline = rx_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 500);
    int total = 0;
    int cap = (out && maxlen > 0) ? (maxlen - 1) : 0;
    int aborted = 0;
    char drain[512];

    for(;;){
        uint64_t now = rx_now_ms();
        if(watch_stop_pipe && !atomic_load(&g_worker_run)){
            aborted = 1;
            break;
        }
        if(now >= deadline){
            aborted = 1;
            break;
        }
        int rem = (int)(deadline - now);
        if(rem > 100) rem = 100;

        struct pollfd fds[2];
        int nfds = 1;
        fds[0].fd = pfd[0];
        fds[0].events = POLLIN | POLLHUP;
        fds[0].revents = 0;
        if(stop_rd >= 0){
            fds[1].fd = stop_rd;
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            nfds = 2;
        }

        int r = poll(fds, (nfds_t)nfds, rem);
        if(r < 0){
            if(errno == EINTR) continue;
            aborted = 1;
            break;
        }
        if(nfds == 2 && (fds[1].revents & POLLIN)){
            aborted = 1;
            break;
        }
        if(fds[0].revents & (POLLIN | POLLHUP | POLLERR)){
            int eof = 0;
            for(;;){
                char *dst = (total < cap) ? (out + total) : drain;
                size_t want = (total < cap) ? (size_t)(cap - total) : sizeof drain;
                ssize_t n = read(pfd[0], dst, want);
                if(n > 0){
                    if(dst != drain) total += (int)n;
                } else if(n == 0){
                    eof = 1;
                    break;
                } else {
                    if(errno == EINTR) continue;
                    if(errno == EAGAIN || errno == EWOULDBLOCK) break;
                    eof = 1;
                    break;
                }
            }
            if(eof) break;
        }
    }

    close(pfd[0]);
    if(aborted){
        kill(-pid, SIGKILL);
    }
    int status = 0;
    while(waitpid(pid, &status, 0) < 0 && errno == EINTR){}
    if(aborted){
        if(out && maxlen > 0) out[0] = '\0';
        return -1;
    }
    if(out && maxlen > 0) out[total] = '\0';
    if(out_exit && WIFEXITED(status)) *out_exit = WEXITSTATUS(status);
    return total;
}

static int exec_cmd_capture(const char *cmd, char *out, int maxlen, int timeout_ms){
    return rx_run_cap_ex(cmd, out, maxlen, timeout_ms, 0, NULL);
}

static void rx_worker_sync_init_locked(void){
    if(!g_worker_cond_init){
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&g_worker_cond, &ca);
        pthread_condattr_destroy(&ca);
        g_worker_cond_init = 1;
    }
    if(g_stop_pipe[0] < 0){
        if(pipe2(g_stop_pipe, O_CLOEXEC | O_NONBLOCK) != 0){
            g_stop_pipe[0] = -1;
            g_stop_pipe[1] = -1;
        }
    }
}

static void rx_drain_stop_pipe(void){
    if(g_stop_pipe[0] < 0) return;
    char tmp[32];
    while(read(g_stop_pipe[0], tmp, sizeof tmp) > 0){}
}

static void rx_worker_poke(void){
    pthread_mutex_lock(&g_worker_mu);
    if(g_worker_cond_init){
        pthread_cond_broadcast(&g_worker_cond);
    }
    pthread_mutex_unlock(&g_worker_mu);
}

static int rx_proc_has_comm(const char *target){
    DIR *dir = opendir("/proc");
    if(!dir) return 0;
    struct dirent *de;
    size_t tlen = strlen(target);
    int found = 0;
    while((de = readdir(dir)) != NULL){
        if(!isdigit((unsigned char)de->d_name[0]) || strlen(de->d_name) > 15) continue;
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

static int rx_proc_has_cmdline(const char *prog, const char *arg_substr){
    DIR *dir = opendir("/proc");
    if(!dir) return 0;
    struct dirent *de;
    size_t plen = strlen(prog);
    int found = 0;
    while((de = readdir(dir)) != NULL){
        if(!isdigit((unsigned char)de->d_name[0]) || strlen(de->d_name) > 15) continue;
        char path[64], buf[512];
        snprintf(path, sizeof path, "/proc/%s/cmdline", de->d_name);
        FILE *f = fopen(path, "r");
        if(!f) continue;
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        if(n == 0) continue;
        buf[n] = '\0';
        const char *argv0 = buf;
        const char *base = strrchr(argv0, '/');
        base = base ? base + 1 : argv0;
        if(strcmp(base, prog) != 0 && (plen < 15 || strncmp(base, prog, plen) != 0)) continue;
        if(!arg_substr || !arg_substr[0]){
            found = 1;
            break;
        }
        size_t off = strlen(argv0) + 1;
        while(off < n){
            const char *arg = buf + off;
            if(strstr(arg, arg_substr)){
                found = 1;
                break;
            }
            off += strlen(arg) + 1;
        }
        if(found) break;
    }
    closedir(dir);
    return found;
}

static void rx_worker_sleep_ms(int ms){
    if(ms <= 0 || !atomic_load(&g_worker_run)) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if(ts.tv_nsec >= 1000000000L){
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_worker_mu);
    rx_worker_sync_init_locked();
    if(atomic_load(&g_worker_run)){
        pthread_cond_timedwait(&g_worker_cond, &g_worker_mu, &ts);
    }
    pthread_mutex_unlock(&g_worker_mu);
}

static void *bt_rx_worker(void *arg);
static void rx_state_clear_locked(void);
static void rx_mac_to_dbus(const char *mac, char *out, size_t cap);
static int  rx_mac_valid(const char *mac);
static int  rx_is_audio_source(const char *mac);

static int rx_is_audio_src_cb(const char *mac, void *ctx){
    (void)ctx;
    return rx_is_audio_source(mac);
}

static void bt_rx_worker_stop(void){
    pthread_t th = 0;
    int do_join = 0;
    pthread_mutex_lock(&g_worker_mu);
    if(g_worker_started){
        atomic_store(&g_worker_run, 0);
        if(g_stop_pipe[1] >= 0){
            char b = 1;
            ssize_t wr = write(g_stop_pipe[1], &b, 1);
            (void)wr;
        }
        if(g_worker_cond_init){
            pthread_cond_broadcast(&g_worker_cond);
        }
        th = g_worker_th;
        g_worker_started = 0;
        do_join = 1;
    } else {
        atomic_store(&g_worker_run, 0);
    }
    pthread_mutex_unlock(&g_worker_mu);
    if(do_join){
        pthread_join(th, NULL);
    }
}

void bt_rx_enter(void){
    int mfd = open("/tmp/.diskos_bt_rx", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if(mfd >= 0) close(mfd);
    pthread_mutex_lock(&g_bt_mu);
    rx_state_clear_locked();
    dn_bt_name_copy(g_bt_state.local_name, sizeof g_bt_state.local_name);
    pthread_mutex_unlock(&g_bt_mu);
    rx_u64_store(&s_bt_pairing_until, 0);
    atomic_store(&s_bt_force_drop, 1);
    rx_u64_store(&s_bt_drop_until, 0);
    rx_u64_store(&s_rx_enter_ms, rx_now_ms());
    rx_u64_store(&s_last_hci_heal_ms, 0);
    rx_u64_store(&s_last_btd_heal_ms, 0);
    bt_rx_worker_ensure();
}

void bt_rx_prepare_exit(void){
    unlink("/tmp/.diskos_bt_rx");
    char mac[20] = {0};
    char dbus_mac[32] = {0};
    pthread_mutex_lock(&g_bt_mu);
    if(g_bt_state.connected && g_bt_state.mac[0]){
        snprintf(mac, sizeof mac, "%s", g_bt_state.mac);
        if(g_bt_state.dbus_mac[0]) snprintf(dbus_mac, sizeof dbus_mac, "%s", g_bt_state.dbus_mac);
        else rx_mac_to_dbus(mac, dbus_mac, sizeof dbus_mac);
    }
    rx_state_clear_locked();
    pthread_mutex_unlock(&g_bt_mu);

    rx_u64_store(&s_bt_pairing_until, 0);
    atomic_store(&s_bt_force_drop, 1);
    rx_u64_store(&s_bt_drop_until, 0);
    bt_rx_worker_stop();

    char cmd[1024];
    snprintf(cmd, sizeof cmd,
        "%s%s%s"
        "for m in $(hcitool con 2>/dev/null | awk '/ACL/{print $3}'); do "
        "  dm=$(echo \"$m\" | tr ':' '_'); "
        "  dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez \"/org/bluez/hci0/dev_${dm}\" org.bluez.Device1.Disconnect >/dev/null 2>&1; "
        "done; "
        "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Discoverable variant:boolean:false >/dev/null 2>&1; "
        "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Pairable variant:boolean:false >/dev/null 2>&1; "
        "killall -9 bt-agent 2>/dev/null",
        dbus_mac[0] ? "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0/dev_" : "",
        dbus_mac[0] ? dbus_mac : "",
        dbus_mac[0] ? " org.bluez.Device1.Disconnect >/dev/null 2>&1; " : "");
    diskos_trace("bt_rx_prepare_exit: mac=%s", mac);
    rx_bg_system(cmd);
}

void bt_rx_worker_ensure(void){
    if(ui_get_source_mode() == 2 && access("/tmp/.diskos_bt_rx", F_OK) != 0){
        int mfd = open("/tmp/.diskos_bt_rx", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if(mfd >= 0) close(mfd);
    }
    pthread_mutex_lock(&g_bt_mu);
    if(!g_bt_state.local_name[0]){
        dn_bt_name_copy(g_bt_state.local_name, sizeof g_bt_state.local_name);
    }
    pthread_mutex_unlock(&g_bt_mu);

    pthread_mutex_lock(&g_worker_mu);
    rx_worker_sync_init_locked();
    if(!g_worker_started){
        rx_drain_stop_pipe();
        atomic_store(&g_worker_run, 1);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        if(pthread_create(&g_worker_th, &attr, bt_rx_worker, NULL) == 0){
            g_worker_started = 1;
        } else {
            atomic_store(&g_worker_run, 0);
        }
        pthread_attr_destroy(&attr);
    }
    pthread_mutex_unlock(&g_worker_mu);
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
        uint64_t now_ms = rx_now_ms();
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

static void rx_heal_ipc_async_cb(void *arg){
    (void)arg;
    if(ui_get_source_mode() == 2){
        ipc_send_cmd("0657000C0006");
    }
}

/* Ensure Bluetooth Receiver daemons & HCI interface are running and healthy without thrashing. */
static void bt_rx_stack_heal(uint64_t now_ms){
    uint64_t enter_ms = rx_u64_load(&s_rx_enter_ms);
    uint64_t last_hci_heal = rx_u64_load(&s_last_hci_heal_ms);
    uint64_t last_btd_heal = rx_u64_load(&s_last_btd_heal_ms);
    int bt_enabling = (access("/tmp/bt_enabling", F_OK) == 0);
    int hci_dev = (access("/sys/class/bluetooth/hci0", F_OK) == 0);
    int patchram_up = rx_proc_has_comm("brcm_patchram_plus");
    int btd_sink = rx_proc_has_cmdline("bluetoothd", "--mode=sink");
    int hci_up = 0;
    if(hci_dev){
        char chk[128] = {0};
        if(rx_run_cap_ex("hciconfig hci0 2>/dev/null | grep -q RUNNING && echo 1",
                         chk, sizeof chk, 400, 1, NULL) < 0){
            return;
        }
        hci_up = (strchr(chk, '1') != NULL);
        /* mq_player's init_sink_server polls `hciconfig hci0 | grep -q RUNNING` without
         * calling `hciconfig hci0 up` when hci0 attaches DOWN; bring it up gently without aborting. */
        if(!hci_up && patchram_up){
            rx_run_cap_ex("rfkill unblock bluetooth 2>/dev/null; hciconfig hci0 up 2>/dev/null",
                          NULL, 0, 500, 1, NULL);
            if(rx_run_cap_ex("hciconfig hci0 2>/dev/null | grep -q RUNNING && echo 1",
                             chk, sizeof chk, 300, 1, NULL) > 0 && strchr(chk, '1')){
                hci_up = 1;
            }
        }
    }

    if(!hci_dev || !hci_up){
        /* Give mq_player's init_sink_server a short window first; if its 0ms rfkill toggle failed
         * to create/run hci0, attach brcm_patchram_plus + hci0 UP ourselves WITHOUT re-sending
         * 0657 6 so mq_player's waiting bt_sink_thread proceeds directly to bluetoothd --mode=sink. */
        uint32_t first_delay = patchram_up ? 2200u : 1200u;
        if(!bt_rx_should_heal(now_ms, enter_ms, last_hci_heal, first_delay, 8000, 0, bt_enabling)){
            return;
        }
        rx_u64_store(&s_last_hci_heal_ms, now_ms);
        diskos_trace("bt_rx_stack_heal: bringing up brcm_patchram_plus + hci0 for sink (dev=%d hci=%d pr=%d btd_sink=%d)",
                     hci_dev, hci_up, patchram_up, btd_sink);
        rx_bg_system(
            "killall -9 brcm_patchram_plus bluealsa bluealsa-custom bluetoothd bt-agent fiio_bluetoothctl 2>/dev/null; "
            "hciconfig hci0 down 2>/dev/null; "
            "rfkill block bluetooth 2>/dev/null; sleep 0.2; "
            "rfkill unblock bluetooth 2>/dev/null; sleep 0.2; "
            "brcm_patchram_plus --enable_lpm --enable_hci --no2bytes --tosleep 200000 --baudrate 3000000 "
            "  --patchram /lib/firmware/bt_bcm/BCM4343A1_001.002.009.1026.1055.hcd /dev/ttyS0 >/tmp/patchram.log 2>&1 & "
            "i=0; while [ \"$i\" -lt 25 ]; do "
            "  [ -e /tmp/.diskos_bt_rx ] || exit 0; "
            "  hciconfig hci0 up 2>/dev/null; "
            "  hciconfig hci0 2>/dev/null | grep -q RUNNING && break; "
            "  sleep 0.3; i=$((i+1)); "
            "done");
        return;
    }

    if(!bt_rx_should_heal(now_ms, enter_ms, last_btd_heal, 4500, 6000, 0, bt_enabling)){
        return;
    }

    int ba_up = rx_proc_has_comm("bluealsa") || rx_proc_has_comm("bluealsa-custom");
    if(!btd_sink){
        rx_u64_store(&s_last_btd_heal_ms, now_ms);
        diskos_trace("bt_rx_stack_heal: hci0 RUNNING without sink bluetoothd, requesting 0657 6 (ba=%d)", ba_up);
        /* Keep brcm_patchram_plus + hci0 alive so mq_player 0657 6 immediately skips firmware
         * reload and proceeds directly to launching bluetoothd --mode=sink. */
        if(rx_run_cap_ex(
            "killall -9 bluealsa bluealsa-custom bluetoothd bt-agent fiio_bluetoothctl 2>/dev/null",
            NULL, 0, 1500, 1, NULL) >= 0 && atomic_load(&g_worker_run)){
            lv_async_call(rx_heal_ipc_async_cb, NULL);
        }
    } else if(!ba_up){
        rx_u64_store(&s_last_btd_heal_ms, now_ms);
        diskos_trace("bt_rx_stack_heal: restarting bluealsa sink without interrupting bluetoothd");
        rx_bg_system(
            "BA=/opt/diskos/bin/bluealsa; [ -x /usr/data/bluealsa ] && BA=/usr/data/bluealsa; "
            "$BA -S --device=hci0 --profile=a2dp-sink -p hfp-ag >/tmp/bluealsa.log 2>&1 & "
            "sleep 0.5; "
            BT_RX_ENABLE_DISCOVERY_SH);
    }
}

/* Background worker thread: polls Bluetooth connection and AVRCP track info */
static void *bt_rx_worker(void *arg){
    (void)arg;
    char *pbuf = malloc(32768);
    if(!pbuf) return NULL;
    char buf[4096];
    char mac[20] = {0};
    char dbus_mac[32] = {0};
    char name[64] = {0};
    char pcm_path[128] = {0};
    char player_path[96] = {0};
    char local_name[64] = {0};
    int inq_tuned = 0;
    int disc_cooldown = 0;
    int was_connected = 0;
    int ready_loss_streak = 0;
    int logged_not_found = 0;
    uint32_t last_raw_pos = 0;

    dn_bt_name_copy(local_name, sizeof local_name);
    diskos_trace("bt_worker: thread started");

    while(atomic_load(&g_worker_run)){
        if(ui_get_source_mode() != 2){
            inq_tuned = 0;
            disc_cooldown = 0;
            was_connected = 0;
            ready_loss_streak = 0;
            rx_worker_sleep_ms(500);
            continue;
        }

        if(atomic_exchange(&s_bt_force_drop, 0)){
            pcm_path[0] = 0;
            player_path[0] = 0;
            mac[0] = 0;
            dbus_mac[0] = 0;
            name[0] = 0;
            inq_tuned = 0;
            disc_cooldown = 0;
            ready_loss_streak = 0;
            last_raw_pos = 0;
        }

        uint64_t now_ms = rx_now_ms();
        uint64_t drop_until = rx_u64_load(&s_bt_drop_until);
        int force_drop_active = 0;
        if(drop_until != 0){
            if(now_ms < drop_until){
                force_drop_active = 1;
            } else {
                rx_u64_store(&s_bt_drop_until, 0);
            }
        }

        /* 1. Single-call connection reconciliation (never trusts stale bt_state alone) */
        int pr = rx_run_cap_ex(
            "printf '===PCM===\\n'; bluealsa-cli list-pcms 2>/dev/null; "
            "printf '\\n===CON===\\n'; hcitool con 2>/dev/null; "
            "printf '\\n===OBJ===\\n'; dbus-send --system --print-reply --dest=org.bluez / "
            "org.freedesktop.DBus.ObjectManager.GetManagedObjects 2>/dev/null",
            pbuf, 32768, 700, 1, NULL);
        if(pr < 0){
            if(!atomic_load(&g_worker_run)) break;
            rx_worker_sleep_ms(500);
            continue;
        }

        const char *sec_pcm = strstr(pbuf, "===PCM===");
        const char *sec_con = strstr(pbuf, "===CON===");
        const char *sec_obj = strstr(pbuf, "===OBJ===");
        char pcm_slice[1024] = {0};
        char con_slice[1024] = {0};
        const char *obj_slice = sec_obj ? (sec_obj + 9) : "";

        if(sec_pcm){
            const char *s = sec_pcm + 9;
            const char *e = sec_con ? sec_con : (sec_obj ? sec_obj : (pbuf + strlen(pbuf)));
            size_t len = (size_t)(e - s);
            if(len >= sizeof pcm_slice) len = sizeof(pcm_slice) - 1;
            memcpy(pcm_slice, s, len);
            pcm_slice[len] = '\0';
        }
        if(sec_con){
            const char *s = sec_con + 9;
            const char *e = sec_obj ? sec_obj : (pbuf + strlen(pbuf));
            size_t len = (size_t)(e - s);
            if(len >= sizeof con_slice) len = sizeof(con_slice) - 1;
            memcpy(con_slice, s, len);
            con_slice[len] = '\0';
        }

        char bz_mac[18] = {0};
        char bz_name[64] = {0};
        bt_rx_extract_bluez_connected(obj_slice, rx_is_audio_src_cb, NULL,
                                      bz_mac, (int)sizeof bz_mac,
                                      bz_name, (int)sizeof bz_name);

        int stale_bt_state = 0;
        FILE *f_bs = fopen("/usr/data/fiio/bt_state", "r");
        if(f_bs){
            int ch = fgetc(f_bs);
            fclose(f_bs);
            if(ch == '1') stale_bt_state = 1;
        }

        bt_rx_conn_info_t conn;
        bt_rx_reconcile_conn(pcm_slice, con_slice, bz_mac, bz_name,
                             stale_bt_state, force_drop_active,
                             rx_is_audio_src_cb, NULL, &conn);

        if(conn.ignored_stale_bt_state && was_connected){
            diskos_trace("bt_worker: ignored stale /usr/data/fiio/bt_state=1 (no live ACL/sink PCM)");
        }

        if(!conn.connected){
            pcm_path[0] = 0;
            player_path[0] = 0;
            mac[0] = 0;
            dbus_mac[0] = 0;
            name[0] = 0;
            last_raw_pos = 0;
            dn_bt_name_copy(local_name, sizeof local_name);
            if(!logged_not_found || was_connected){
                diskos_trace("bt_worker: disconnected / waiting for phone");
                logged_not_found = 1;
            }
            bt_rx_adapter_state_t adapter;
            bt_rx_extract_adapter_state(obj_slice, &adapter);
            int btd_sink = rx_proc_has_cmdline("bluetoothd", "--mode=sink");
            int ba_up = rx_proc_has_comm("bluealsa") || rx_proc_has_comm("bluealsa-custom");
            int rx_ready = btd_sink && ba_up &&
                           adapter.present && adapter.powered &&
                           adapter.discoverable && adapter.pairable;

            pthread_mutex_lock(&g_bt_mu);
            int prev_ready = g_bt_state.ready;
            int new_ready = rx_ready;
            if(rx_ready){
                ready_loss_streak = 0;
            } else if(prev_ready && btd_sink && ba_up){
                /* Keep previous readiness across a single transient D-Bus glitch while daemons live. */
                if(++ready_loss_streak < 2) new_ready = 1;
            } else {
                ready_loss_streak = 0;
            }
            rx_state_clear_locked();
            g_bt_state.ready = new_ready;
            g_bt_player_path[0] = 0;
            snprintf(g_bt_state.local_name, sizeof g_bt_state.local_name, "%s", local_name);
            pthread_mutex_unlock(&g_bt_mu);

            if(was_connected || prev_ready != new_ready){
                was_connected = 0;
                ui_notify(UI_WAKE_BT);
            }

            if(disc_cooldown > 0) disc_cooldown--;
            if(btd_sink && (!inq_tuned || disc_cooldown == 0 ||
               (adapter.present && (!adapter.powered || !adapter.discoverable || !adapter.pairable)))){
                char hci_chk[256] = {0};
                if(rx_run_cap_ex("hciconfig hci0 2>/dev/null | grep RUNNING",
                                 hci_chk, sizeof hci_chk, 300, 1, NULL) > 0 && hci_chk[0]){
                    int has_iscan = (strstr(hci_chk, "ISCAN") != NULL);
                    int has_agent = rx_proc_has_comm("bt-agent") || rx_proc_has_comm("fiio_bluetoothctl");
                    int dbus_ready = adapter.present && adapter.powered && adapter.discoverable && adapter.pairable;
                    if(!has_iscan || (adapter.present && !dbus_ready) || !has_agent){
                        diskos_trace("bt_worker: enabling discovery (iscan=%d dbus=%d agent=%d)",
                                     has_iscan, dbus_ready, has_agent);
                        rx_bg_system("hcitool con 2>/dev/null | grep -q ACL || { " BT_RX_ENABLE_DISCOVERY_SH "; }");
                        if(adapter.present && local_name[0] && strcmp(adapter.alias, local_name) != 0){
                            btalias_replay(local_name, 500);
                        }
                        inq_tuned = 1;
                        disc_cooldown = 3;
                    } else if(!inq_tuned && dbus_ready){
                        rx_bg_system(BT_RX_FAST_INQ_SH);
                        if(local_name[0] && strcmp(adapter.alias, local_name) != 0){
                            btalias_replay(local_name, 500);
                        }
                        inq_tuned = 1;
                        disc_cooldown = 3;
                    }
                }
            }

            bt_rx_stack_heal(now_ms);
            uint64_t enter_ms = rx_u64_load(&s_rx_enter_ms);
            int sleep_ms = (!new_ready && enter_ms && now_ms >= enter_ms && (now_ms - enter_ms) < 15000)
                         ? 350 : ((!new_ready || (enter_ms && now_ms >= enter_ms && (now_ms - enter_ms) < 20000)) ? 600 : 1500);
            rx_worker_sleep_ms(sleep_ms);
            continue;
        }

        ready_loss_streak = 0;

        if(conn.has_sink_pcm){
            rx_u64_store(&s_bt_pairing_until, 0);
            rx_u64_store(&s_bt_drop_until, 0);
        }
        logged_not_found = 0;
        inq_tuned = 0;
        disc_cooldown = 0;

        int newly_connected = (!was_connected || strcasecmp(mac, conn.mac) != 0);
        if(newly_connected){
            snprintf(mac, sizeof mac, "%s", conn.mac);
            rx_mac_to_dbus(mac, dbus_mac, sizeof dbus_mac);
            pcm_path[0] = 0;
            player_path[0] = 0;
            /* Extract exact sink PCM path if present in pcm_slice */
            const char *lp = pcm_slice;
            while(*lp){
                const char *eol = strchr(lp, '\n');
                size_t ll = eol ? (size_t)(eol - lp) : strlen(lp);
                char line[192];
                if(ll >= sizeof line) ll = sizeof(line) - 1;
                memcpy(line, lp, ll);
                line[ll] = '\0';
                char *dp = strstr(line, "/org/bluealsa/");
                if(dp && (strstr(dp, "/a2dpsnk/") || strstr(dp, "/a2dp-sink/"))){
                    char *ep = dp;
                    while(*ep && *ep != ' ' && *ep != '\t' && *ep != '\r') ep++;
                    *ep = '\0';
                    if(bt_rx_path_safe(dp)){
                        snprintf(pcm_path, sizeof pcm_path, "%s", dp);
                    }
                    break;
                }
                lp = eol ? eol + 1 : lp + strlen(lp);
            }
            char alias[64] = {0};
            if(btalias_get(mac, alias, sizeof alias, 80) == BTALIAS_OK && alias[0]){
                snprintf(name, sizeof name, "%s", alias);
            } else if(conn.name[0]){
                snprintf(name, sizeof name, "%s", conn.name);
            } else {
                snprintf(name, sizeof name, "Connected Device");
            }
            last_raw_pos = 0;
            diskos_trace("bt_worker: connected mac=%s name='%s'", mac, name);
        } else if(conn.name[0] && !strcmp(name, "Connected Device")){
            snprintf(name, sizeof name, "%s", conn.name);
        }
        was_connected = 1;

        char found_player[96] = {0};
        if(bt_rx_extract_player_path(obj_slice, mac, found_player, (int)sizeof found_player)){
            snprintf(player_path, sizeof player_path, "%s", found_player);
        } else if(!player_path[0] || !bt_rx_path_safe(player_path)){
            snprintf(player_path, sizeof player_path, "/org/bluez/hci0/dev_%s/player0", dbus_mac);
        }

        /* 2. Single-call MediaPlayer1 + BlueALSA info query */
        char qcmd[768];
        if(pcm_path[0] && bt_rx_path_safe(pcm_path)){
            snprintf(qcmd, sizeof qcmd,
                "printf '===MP===\\n'; "
                "dbus-send --system --print-reply --dest=org.bluez %s "
                "org.freedesktop.DBus.Properties.GetAll string:org.bluez.MediaPlayer1 2>/dev/null; "
                "printf '\\n===BA===\\n'; "
                "bluealsa-cli info \"%s\" 2>/dev/null",
                player_path, pcm_path);
        } else {
            snprintf(qcmd, sizeof qcmd,
                "printf '===MP===\\n'; "
                "dbus-send --system --print-reply --dest=org.bluez %s "
                "org.freedesktop.DBus.Properties.GetAll string:org.bluez.MediaPlayer1 2>/dev/null; "
                "printf '\\n===BA===\\n'; "
                "bluealsa-cli info /org/bluealsa/hci0/dev_%s/a2dpsnk/source 2>/dev/null || "
                "bluealsa-cli info /org/bluealsa/hci0/dev_%s/a2dp-sink/source 2>/dev/null",
                player_path, dbus_mac, dbus_mac);
        }

        uint64_t sample_time = rx_now_ms();
        buf[0] = '\0';
        int qrc = rx_run_cap_ex(qcmd, buf, sizeof buf, 600, 1, NULL);
        if(qrc < 0 && !atomic_load(&g_worker_run)) break;

        const char *mp_sec = strstr(buf, "===MP===");
        const char *ba_sec = strstr(buf, "===BA===");
        char mp_slice[3072] = {0};
        const char *ba_slice = ba_sec ? (ba_sec + 8) : "";
        if(mp_sec){
            const char *s = mp_sec + 8;
            const char *e = ba_sec ? ba_sec : (buf + strlen(buf));
            size_t len = (size_t)(e - s);
            if(len >= sizeof mp_slice) len = sizeof(mp_slice) - 1;
            memcpy(mp_slice, s, len);
            mp_slice[len] = '\0';
        }

        bt_rx_mediaplayer_props_t mp;
        bt_rx_dbus_parse_mediaplayer_props(mp_slice, &mp);
        if(!mp.has_status && !mp.has_title){
            bt_rx_extract_mediaplayer_from_objs(obj_slice, player_path, &mp);
        }
        int cur_playing = 0;
        pthread_mutex_lock(&g_bt_mu);
        cur_playing = g_bt_state.is_playing;
        pthread_mutex_unlock(&g_bt_mu);

        int is_play = cur_playing;
        if(mp.has_status){
            if(strcasecmp(mp.status, "playing") == 0) is_play = 1;
            else if(strcasecmp(mp.status, "paused") == 0 ||
                    strcasecmp(mp.status, "stopped") == 0 ||
                    strcasecmp(mp.status, "error") == 0) is_play = 0;
        } else if(qrc >= 0 && !newly_connected){
            /* No MediaPlayer1 Status reported: preserve current state if timed out,
             * otherwise only clear if no player properties at all. */
            if(!mp.has_title && !mp.has_position) is_play = 0;
        }
        const char *title = mp.title;
        const char *artist = mp.artist;
        const char *album = mp.album;
        uint32_t dur_ms = mp.has_duration ? mp.duration_ms : 0;
        uint32_t pos_ms = mp.has_position ? mp.position_ms : 0;
        int has_pos = mp.has_position;

        /* 3. Parse real-time BlueALSA Codec, Sample Rate, and Format */
        char codec_str[64] = {0};
        if(ba_slice[0]){
            char cname[24] = {0};
            char rate[20] = {0};
            char bits[16] = {0};
            const char *c = strstr(ba_slice, "Selected codec:");
            if(c){
                c += 15; while(*c == ' ' || *c == '\t') c++;
                const char *end = c; while(*end && *end != '\r' && *end != '\n') end++;
                int len = (int)(end - c);
                if(len > 0 && len < (int)sizeof(cname)){
                    memcpy(cname, c, (size_t)len);
                    cname[len] = 0;
                }
            }
            const char *s = strstr(ba_slice, "Sampling:");
            if(s){
                s += 9; while(*s == ' ' || *s == '\t') s++;
                int hz = atoi(s);
                if(hz >= 1000){
                    if(hz % 1000 == 0) snprintf(rate, sizeof rate, "%dkHz", hz / 1000);
                    else snprintf(rate, sizeof rate, "%d.%dkHz", hz / 1000, (hz % 1000) / 100);
                }
            }
            const char *f = strstr(ba_slice, "Format:");
            if(f){
                char fline[32] = {0};
                f += 7; while(*f == ' ' || *f == '\t') f++;
                const char *fend = f; while(*fend && *fend != '\r' && *fend != '\n') fend++;
                int flen = (int)(fend - f);
                if(flen > 0 && flen < (int)sizeof(fline)){
                    memcpy(fline, f, (size_t)flen);
                    fline[flen] = 0;
                    if(strstr(fline, "S32")) snprintf(bits, sizeof bits, "24/32bit");
                    else if(strstr(fline, "S24")) snprintf(bits, sizeof bits, "24bit");
                    else if(strstr(fline, "S16")) snprintf(bits, sizeof bits, "16bit");
                }
            }
            if(cname[0]){
                if(bt_rx_low_latency_enabled() && strcmp(cname, "SBC") != 0 && pcm_path[0] && bt_rx_path_safe(pcm_path)){
                    char lcmd[256];
                    snprintf(lcmd, sizeof lcmd, "bluealsa-cli codec \"%s\" SBC 2>/dev/null", pcm_path);
                    rx_bg_system(lcmd);
                }
                if(rate[0] && bits[0])
                    snprintf(codec_str, sizeof codec_str, "%s \xE2\x80\xA2 %s \xE2\x80\xA2 %s", cname, rate, bits);
                else if(rate[0])
                    snprintf(codec_str, sizeof codec_str, "%s \xE2\x80\xA2 %s", cname, rate);
                else
                    snprintf(codec_str, sizeof codec_str, "%s", cname);
            }
        }
        /* Fallback: if BlueALSA returned nothing, check /usr/data/bt_rx_codec */
        if(codec_str[0] == 0){
            FILE *fc = fopen("/usr/data/bt_rx_codec", "r");
            if(fc){
                char raw[48] = {0};
                char cbuf[32] = {0};
                if(fgets(raw, sizeof raw, fc)){
                    char *comma = strchr(raw, ',');
                    if(comma) *comma = 0;
                    if(bt_rx_clean_codec_label(raw, cbuf, (int)sizeof cbuf)){
                        if(!strcmp(cbuf, "LDAC")) snprintf(codec_str, sizeof codec_str, "LDAC \xE2\x80\xA2 96kHz \xE2\x80\xA2 24/32bit");
                        else if(!strcmp(cbuf, "AAC")) snprintf(codec_str, sizeof codec_str, "AAC \xE2\x80\xA2 44.1kHz \xE2\x80\xA2 16bit");
                        else if(!strcmp(cbuf, "SBC")) snprintf(codec_str, sizeof codec_str, "SBC \xE2\x80\xA2 44.1kHz \xE2\x80\xA2 16bit");
                        else if(!strcmp(cbuf, "aptX")) snprintf(codec_str, sizeof codec_str, "aptX \xE2\x80\xA2 44.1kHz \xE2\x80\xA2 16bit");
                        else if(!strcmp(cbuf, "aptX-HD")) snprintf(codec_str, sizeof codec_str, "aptX HD \xE2\x80\xA2 48kHz \xE2\x80\xA2 24bit");
                        else snprintf(codec_str, sizeof codec_str, "%s", cbuf);
                    }
                }
                fclose(fc);
            }
        }

        if(bt_rx_low_latency_enabled()){
            if(codec_str[0]){
                char tmp[80];
                snprintf(tmp, sizeof tmp, "LL \xE2\x80\xA2 %s", codec_str);
                snprintf(codec_str, sizeof codec_str, "%.63s", tmp);
            } else {
                snprintf(codec_str, sizeof codec_str, "LL \xE2\x80\xA2 SBC \xE2\x80\xA2 44.1kHz \xE2\x80\xA2 16bit");
            }
        }

        /* 4. Update shared state under mutex */
        pthread_mutex_lock(&g_bt_mu);
        g_bt_state.ready = 1;
        g_bt_state.connected = 1;
        int was_playing = g_bt_state.is_playing;
        g_bt_state.is_playing = is_play;
        snprintf(g_bt_state.local_name, sizeof g_bt_state.local_name, "%s", local_name);
        snprintf(g_bt_state.mac, sizeof g_bt_state.mac, "%s", mac);
        snprintf(g_bt_state.dbus_mac, sizeof g_bt_state.dbus_mac, "%s", dbus_mac);
        snprintf(g_bt_state.dev_name, sizeof g_bt_state.dev_name, "%s", name);
        snprintf(g_bt_player_path, sizeof g_bt_player_path, "%s", player_path);

        int track_changed = 0;
        if(title[0]){
            if(strcmp(g_bt_state.title, title) != 0 ||
               strcmp(g_bt_state.artist, artist) != 0 ||
               strcmp(g_bt_state.album, album) != 0){
                track_changed = 1;
            }
            snprintf(g_bt_state.title, sizeof g_bt_state.title, "%s", title);
            snprintf(g_bt_state.artist, sizeof g_bt_state.artist, "%s", artist);
            snprintf(g_bt_state.album, sizeof g_bt_state.album, "%s", album);
        } else if(qrc >= 0 && !is_play && !was_playing){
            if(g_bt_state.title[0] || g_bt_state.artist[0] || g_bt_state.album[0]){
                track_changed = 1;
            }
            g_bt_state.title[0] = 0;
            g_bt_state.artist[0] = 0;
            g_bt_state.album[0] = 0;
        }

        if(dur_ms > 0) g_bt_state.duration_ms = dur_ms;
        else if(track_changed) g_bt_state.duration_ms = 0;

        if(track_changed){
            g_bt_state.position_ms = has_pos ? pos_ms : 0;
            g_bt_state.pos_anchor_ms = is_play ? sample_time : 0;
            last_raw_pos = has_pos ? pos_ms : 0;
        } else if(was_playing != is_play){
            if(!is_play){
                if(g_bt_state.pos_anchor_ms > 0 && sample_time > g_bt_state.pos_anchor_ms){
                    g_bt_state.position_ms += (uint32_t)(sample_time - g_bt_state.pos_anchor_ms);
                    if(g_bt_state.duration_ms > 0 && g_bt_state.position_ms > g_bt_state.duration_ms)
                        g_bt_state.position_ms = g_bt_state.duration_ms;
                }
                g_bt_state.pos_anchor_ms = 0;
            } else {
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
        if(newly_connected || track_changed || was_playing != is_play || codec_changed){
            diskos_trace("bt_worker: play=%d (was=%d) title='%s' dur=%u pos=%u codec='%s'",
                         is_play, was_playing, g_bt_state.title, g_bt_state.duration_ms,
                         g_bt_state.position_ms, g_bt_state.codec);
            ui_notify(UI_WAKE_BT);
        }
        pthread_mutex_unlock(&g_bt_mu);

        rx_worker_sleep_ms(1500);
    }
    free(pbuf);
    return NULL;
}

typedef struct {
    char dbus_mac[32];
    char player_path[96];
    char ctrl_method[16];
    char player_method[16];
    const char *ipc_cmd;
} rx_avrcp_req_t;

static void rx_ipc_async_cb(void *arg){
    const char *cmd = (const char *)arg;
    if(cmd && ui_get_source_mode() == 2){
        ipc_send_cmd(cmd);
    }
}

static void *rx_avrcp_worker(void *arg){
    rx_avrcp_req_t *req = (rx_avrcp_req_t *)arg;
    char cmd[384];
    int ex = -1;
    snprintf(cmd, sizeof cmd,
        "dbus-send --system --print-reply --reply-timeout=800 --dest=org.bluez "
        "/org/bluez/hci0/dev_%s org.bluez.MediaControl1.%s >/dev/null 2>&1",
        req->dbus_mac, req->ctrl_method);
    rx_run_cap_ex(cmd, NULL, 0, 1000, 0, &ex);
    if(ex != 0){
        const char *pp = req->player_path[0] ? req->player_path : NULL;
        if(pp && bt_rx_path_safe(pp)){
            snprintf(cmd, sizeof cmd,
                "dbus-send --system --print-reply --reply-timeout=800 --dest=org.bluez "
                "%s org.bluez.MediaPlayer1.%s >/dev/null 2>&1",
                pp, req->player_method);
        } else {
            snprintf(cmd, sizeof cmd,
                "dbus-send --system --print-reply --reply-timeout=800 --dest=org.bluez "
                "/org/bluez/hci0/dev_%s/player0 org.bluez.MediaPlayer1.%s >/dev/null 2>&1",
                req->dbus_mac, req->player_method);
        }
        rx_run_cap_ex(cmd, NULL, 0, 1000, 0, &ex);
    }
    if(ex != 0 && req->ipc_cmd){
        diskos_trace("rx_avrcp: dbus failed (dev=%s), falling back to ipc %s", req->dbus_mac, req->ipc_cmd);
        lv_async_call(rx_ipc_async_cb, (void *)req->ipc_cmd);
    } else if(ex == 0){
        /* Wait briefly for phone AVRCP Status/Track update, then wake poll worker immediately. */
        poll(NULL, 0, 180);
        rx_worker_poke();
    }
    free(req);
    return NULL;
}

/* Single authoritative AVRCP dispatch per press: primary D-Bus MediaControl1,
 * falling back to MediaPlayer1 then ipc 0201 ONLY if D-Bus delivery fails. */
static void rx_avrcp_dispatch(bt_rx_avrcp_act_t act){
    char dbus_mac[32] = {0};
    char ppath[96] = {0};
    int is_playing = 0;
    int connected = 0;
    pthread_mutex_lock(&g_bt_mu);
    connected = g_bt_state.connected;
    if(connected && g_bt_state.dbus_mac[0]){
        snprintf(dbus_mac, sizeof dbus_mac, "%s", g_bt_state.dbus_mac);
        snprintf(ppath, sizeof ppath, "%s", g_bt_player_path);
        is_playing = g_bt_state.is_playing;
    }
    pthread_mutex_unlock(&g_bt_mu);

    bt_rx_avrcp_plan_t plan = bt_rx_plan_avrcp(act, connected, dbus_mac, is_playing);
    if(!plan.use_dbus){
        ipc_send_cmd(plan.ipc_cmd);
        return;
    }

    rx_avrcp_req_t *req = calloc(1, sizeof(*req));
    if(!req){
        ipc_send_cmd(plan.ipc_cmd);
        return;
    }
    snprintf(req->dbus_mac, sizeof req->dbus_mac, "%s", dbus_mac);
    snprintf(req->player_path, sizeof req->player_path, "%s", ppath);
    snprintf(req->ctrl_method, sizeof req->ctrl_method, "%s", plan.ctrl_method);
    snprintf(req->player_method, sizeof req->player_method, "%s", plan.player_method);
    req->ipc_cmd = plan.ipc_cmd;

    pthread_attr_t attr;
    pthread_t th;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    if(pthread_create(&th, &attr, rx_avrcp_worker, req) != 0){
        free(req);
        ipc_send_cmd(plan.ipc_cmd);
    }
    pthread_attr_destroy(&attr);
}

void bt_rx_play_pause(void){
    diskos_trace("bt_rx_play_pause: dispatch");
    rx_avrcp_dispatch(BT_RX_AVRCP_ACT_PLAY_PAUSE);
}

void bt_rx_next(void){
    rx_avrcp_dispatch(BT_RX_AVRCP_ACT_NEXT);
}

void bt_rx_prev(void){
    rx_avrcp_dispatch(BT_RX_AVRCP_ACT_PREV);
}

static int rx_mac_valid(const char *mac){
    return bt_rx_mac_valid(mac);
}

static char s_bt_adapter_path[128] = {0};

/*
 * Returns 1 if mac is an Audio Source (can transmit audio to diskOS in BT Receiver mode:
 * phones, tablets, PCs, smart TVs).
 * Returns 0 if mac is a pure Audio Sink (TWS earbuds, headphones, Bluetooth speakers),
 * or non-audio peripheral (keyboards, mice).
 * If device info cannot be found or read (e.g. host testing), returns 1 (fail-open).
 */
static int rx_is_audio_source(const char *mac){
    if(!rx_mac_valid(mac)) return 0;

    char up_mac[20];
    for(int i = 0; mac[i] && i < 19; i++){
        char c = mac[i];
        if(c >= 'a' && c <= 'z') c -= ('a' - 'A');
        up_mac[i] = c;
        up_mac[i + 1] = 0;
    }

    char info_path[256] = {0};

    /* Fast path: check cached adapter path */
    if(s_bt_adapter_path[0]){
        snprintf(info_path, sizeof info_path, "%s/%s/info", s_bt_adapter_path, up_mac);
        if(access(info_path, R_OK) != 0){
            info_path[0] = 0;
        }
    }

    /* Discovery path: search adapter directories */
    if(!info_path[0]){
        static const char *bases[] = {
            "/usr/data/storebluetooth/lib/bluetooth",
            "/var/lib/bluetooth",
            NULL
        };
        for(int b = 0; bases[b]; b++){
            DIR *dir = opendir(bases[b]);
            if(!dir) continue;
            struct dirent *de;
            while((de = readdir(dir)) != NULL){
                if(de->d_name[0] == '.' || !strcmp(de->d_name, "cache")) continue;
                char test_path[256];
                snprintf(test_path, sizeof test_path, "%s/%s/%s/info", bases[b], de->d_name, up_mac);
                if(access(test_path, R_OK) == 0){
                    snprintf(s_bt_adapter_path, sizeof s_bt_adapter_path, "%s/%s", bases[b], de->d_name);
                    snprintf(info_path, sizeof info_path, "%s", test_path);
                    break;
                }
            }
            closedir(dir);
            if(info_path[0]) break;
        }
    }

    /* If info file not found on disk, fail open (keep device) */
    if(!info_path[0]) return 1;

    FILE *f = fopen(info_path, "r");
    if(!f) return 1;

    char buf[2048];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if(n == 0) return 1;
    buf[n] = 0;

    /*
     * Check Bluetooth Services UUIDs:
     * 0000110a: Audio Source (A2DP Source - phones, tablets, PCs, smart TVs).
     * 0000110b: Audio Sink (A2DP Sink - TWS earbuds, headphones, speakers).
     */
    char *svc = strstr(buf, "Services=");
    if(svc){
        char *end = strpbrk(svc, "\r\n");
        if(end) *end = 0;

        int has_src = (strcasestr(svc, "0000110a") != NULL);
        int has_snk = (strcasestr(svc, "0000110b") != NULL);

        if(has_src) return 1; /* Transmits audio */
        if(has_snk) return 0; /* Pure sink (earbuds/headphones/speakers) */

        /* If services are listed but neither audio source nor audio sink,
         * fall through to check Class of Device instead of blindly excluding! */
    }

    /*
     * If Services= was not populated yet, check Bluetooth Class of Device (CoD):
     */
    char *cls = strstr(buf, "Class=");
    if(cls){
        long cod = strtol(cls + 6, NULL, 0);
        int major = (cod >> 8) & 0x1f;
        int minor = (cod >> 2) & 0x3f;

        /* Major: Computer (0x01) or Phone (0x02) -> Audio Source */
        if(major == 0x01 || major == 0x02) return 1;

        /* Major: Audio/Video (0x04) */
        if(major == 0x04){
            /* Minor: Headset (0x01), Handsfree (0x02), Loudspeaker (0x05),
             * Headphones (0x06), Portable Audio (0x07), Car audio (0x08), HiFi (0x0a) */
            if(minor == 0x01 || minor == 0x02 || minor == 0x05 ||
               minor == 0x06 || minor == 0x07 || minor == 0x08 || minor == 0x0a){
                return 0;
            }
        }
    }

    return 1;
}

int bt_rx_get_paired_devices(bt_rx_device_t *out, int max_devs){
    if(!out || max_devs <= 0) return 0;
    char buf[4096];
    int n = exec_cmd_capture("bluetoothctl paired-devices 2>/dev/null", buf, sizeof buf, 400);

    char cur_mac[20] = {0};
    int is_conn = 0;
    pthread_mutex_lock(&g_bt_mu);
    is_conn = g_bt_state.connected;
    if(is_conn && g_bt_state.mac[0])
        snprintf(cur_mac, sizeof cur_mac, "%s", g_bt_state.mac);
    pthread_mutex_unlock(&g_bt_mu);

    int count = 0;
    if(n > 0){
        char *line = buf;
        while(line && *line && count < max_devs){
            char *nl = strchr(line, '\n');
            if(nl) *nl = 0;
            char *cr = strchr(line, '\r');
            if(cr) *cr = 0;

            /* format: "Device AA:BB:CC:DD:EE:FF Friendly Name" */
            if(!strncmp(line, "Device ", 7) && strlen(line) > 7 + 17){
                char mac[20];
                memcpy(mac, line + 7, 17);
                mac[17] = 0;
                const char *raw_name = line + 7 + 17;
                while(*raw_name == ' ' || *raw_name == '\t') raw_name++;

                if(rx_mac_valid(mac)){
                    int is_cur = (is_conn && cur_mac[0] && !strcasecmp(cur_mac, mac));
                    /* In BT Receiver mode, filter out non-audio sources (e.g. TWS earbuds, headphones, speakers) */
                    if(!is_cur && !rx_is_audio_source(mac)){
                        diskos_trace("bt_rx_get_paired_devices: filtering out non-source %s (%s)", mac, raw_name);
                        line = nl ? (nl + 1) : NULL;
                        continue;
                    }

                    snprintf(out[count].mac, sizeof out[count].mac, "%s", mac);
                    /* bluetoothctl paired-devices already emits the device's Alias; avoid blocking
                     * the UI thread with per-device D-Bus spawns. */
                    if(raw_name[0]){
                        snprintf(out[count].name, sizeof out[count].name, "%s", raw_name);
                    } else {
                        snprintf(out[count].name, sizeof out[count].name, "%s", mac);
                    }
                    out[count].connected = is_cur ? 1 : 0;
                    count++;
                }
            }
            line = nl ? (nl + 1) : NULL;
        }
    }

    /* Fallback: if bluetoothctl failed, stalled, or returned 0 devices,
     * read paired devices directly from BlueZ filesystem directories so
     * the paired device list is never empty during daemon glitches or restarts! */
    if(count == 0){
        static const char *bases[] = {
            "/usr/data/storebluetooth/lib/bluetooth",
            "/var/lib/bluetooth",
            NULL
        };
        for(int b = 0; bases[b] && count < max_devs; b++){
            DIR *dir = opendir(bases[b]);
            if(!dir) continue;
            struct dirent *ade;
            while((ade = readdir(dir)) != NULL && count < max_devs){
                if(ade->d_name[0] == '.' || !strcmp(ade->d_name, "cache")) continue;
                char ad_path[256];
                snprintf(ad_path, sizeof ad_path, "%s/%s", bases[b], ade->d_name);
                DIR *devdir = opendir(ad_path);
                if(!devdir) continue;
                struct dirent *dde;
                while((dde = readdir(devdir)) != NULL && count < max_devs){
                    if(dde->d_name[0] == '.') continue;
                    char dmac[20] = {0};
                    snprintf(dmac, sizeof dmac, "%s", dde->d_name);
                    if(!rx_mac_valid(dmac)) continue;

                    char ipath[300];
                    snprintf(ipath, sizeof ipath, "%s/%s/info", ad_path, dmac);
                    FILE *f = fopen(ipath, "r");
                    if(!f) continue;
                    char ibuf[1536];
                    size_t rn = fread(ibuf, 1, sizeof ibuf - 1, f);
                    fclose(f);
                    if(rn == 0) continue;
                    ibuf[rn] = 0;

                    /* Check if device was paired / has a link key */
                    if(!strstr(ibuf, "Key=") && !strstr(ibuf, "LinkKey") && !strstr(ibuf, "Paired=true"))
                        continue;

                    if(!rx_is_audio_source(dmac)) continue;

                    /* Extract Name= */
                    char dname[64] = {0};
                    char *np = strstr(ibuf, "Name=");
                    if(np){
                        np += 5; while(*np == ' ' || *np == '\t') np++;
                        char *ne = strpbrk(np, "\r\n");
                        int nlen = ne ? (int)(ne - np) : (int)strlen(np);
                        if(nlen > 0 && nlen < (int)sizeof(dname)){
                            memcpy(dname, np, nlen);
                            dname[nlen] = 0;
                        }
                    }

                    int is_cur = (is_conn && cur_mac[0] && !strcasecmp(cur_mac, dmac));
                    snprintf(out[count].mac, sizeof out[count].mac, "%s", dmac);
                    char alias[64] = {0};
                    if(btalias_get(dmac, alias, sizeof alias, 80) == BTALIAS_OK && alias[0]){
                        snprintf(out[count].name, sizeof out[count].name, "%s", alias);
                    } else if(dname[0]){
                        snprintf(out[count].name, sizeof out[count].name, "%s", dname);
                    } else {
                        snprintf(out[count].name, sizeof out[count].name, "%s", dmac);
                    }
                    out[count].connected = is_cur ? 1 : 0;
                    count++;
                }
                closedir(devdir);
            }
            closedir(dir);
        }
    }

    /* Move connected device to index 0 if present */
    for(int i = 1; i < count; i++){
        if(out[i].connected){
            bt_rx_device_t tmp = out[i];
            for(int j = i; j > 0; j--) out[j] = out[j - 1];
            out[0] = tmp;
            break;
        }
    }
    return count;
}

void bt_rx_connect_to(const char *mac, const char *name){
    if(!rx_mac_valid(mac)) return;
    char old_mac[20] = {0};
    pthread_mutex_lock(&g_bt_mu);
    if(g_bt_state.connected && g_bt_state.mac[0]){
        if(!strcasecmp(g_bt_state.mac, mac)){
            pthread_mutex_unlock(&g_bt_mu);
            ui_toast(tr("Already connected"));
            return;
        }
        snprintf(old_mac, sizeof old_mac, "%s", g_bt_state.mac);
        rx_state_clear_locked();
    }
    pthread_mutex_unlock(&g_bt_mu);

    rx_u64_store(&s_bt_pairing_until, 0);
    if(old_mac[0]){
        atomic_store(&s_bt_force_drop, 1);
        rx_u64_store(&s_bt_drop_until, rx_now_ms() + 2000);
    }

    char dbus_mac[32] = {0}, old_dbus[32] = {0};
    rx_mac_to_dbus(mac, dbus_mac, sizeof dbus_mac);
    if(old_mac[0]) rx_mac_to_dbus(old_mac, old_dbus, sizeof old_dbus);

    char toast_buf[96];
    snprintf(toast_buf, sizeof toast_buf, "%s %s...", tr("Connecting to"), (name && name[0]) ? name : mac);
    ui_toast(toast_buf);

    char cmd[2048];
    if(old_mac[0]){
        snprintf(cmd, sizeof cmd,
            "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0/dev_%s org.bluez.Device1.Disconnect >/dev/null 2>&1; "
            "for m in $(hcitool con 2>/dev/null | awk '/ACL/{print $3}'); do "
            "  [ \"$m\" = \"%s\" ] && continue; "
            "  dm=$(echo \"$m\" | tr ':' '_'); "
            "  dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez \"/org/bluez/hci0/dev_${dm}\" org.bluez.Device1.Disconnect >/dev/null 2>&1; "
            "done; "
            "sleep 0.4; "
            "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Powered variant:boolean:true >/dev/null 2>&1; "
            "( pidof fiio_bluetoothctl >/dev/null 2>&1 || pidof bt-agent >/dev/null 2>&1 || bt-agent -c NoInputNoOutput -d >/dev/null 2>&1 & ); "
            "dbus-send --system --print-reply --reply-timeout=4000 --dest=org.bluez /org/bluez/hci0/dev_%s org.bluez.Device1.Connect >/dev/null 2>&1 || "
            "( sleep 0.5; "
            "  dbus-send --system --print-reply --reply-timeout=4000 --dest=org.bluez /org/bluez/hci0/dev_%s org.bluez.Device1.ConnectProfile string:0000110a-0000-1000-8000-00805f9b34fb >/dev/null 2>&1 || "
            "  bluetoothctl --timeout 5 connect %s >/dev/null 2>&1 )",
            old_dbus, mac, dbus_mac, dbus_mac, mac);
    } else {
        snprintf(cmd, sizeof cmd,
            "for m in $(hcitool con 2>/dev/null | awk '/ACL/{print $3}'); do "
            "  [ \"$m\" = \"%s\" ] && continue; "
            "  dm=$(echo \"$m\" | tr ':' '_'); "
            "  dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez \"/org/bluez/hci0/dev_${dm}\" org.bluez.Device1.Disconnect >/dev/null 2>&1; "
            "done; "
            "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Powered variant:boolean:true >/dev/null 2>&1; "
            "( pidof fiio_bluetoothctl >/dev/null 2>&1 || pidof bt-agent >/dev/null 2>&1 || bt-agent -c NoInputNoOutput -d >/dev/null 2>&1 & ); "
            "dbus-send --system --print-reply --reply-timeout=4000 --dest=org.bluez /org/bluez/hci0/dev_%s org.bluez.Device1.Connect >/dev/null 2>&1 || "
            "( sleep 0.5; "
            "  dbus-send --system --print-reply --reply-timeout=4000 --dest=org.bluez /org/bluez/hci0/dev_%s org.bluez.Device1.ConnectProfile string:0000110a-0000-1000-8000-00805f9b34fb >/dev/null 2>&1 || "
            "  bluetoothctl --timeout 5 connect %s >/dev/null 2>&1 )",
            mac, dbus_mac, dbus_mac, mac);
    }
    diskos_trace("bt_rx_connect_to: target=%s (old=%s)", mac, old_mac);
    rx_bg_system(cmd);
}

static void rx_state_clear_locked(void){
    g_bt_state.ready = 0;
    g_bt_state.connected = 0;
    g_bt_state.is_playing = 0;
    g_bt_state.mac[0] = 0;
    g_bt_state.dbus_mac[0] = 0;
    g_bt_state.dev_name[0] = 0;
    g_bt_state.title[0] = 0;
    g_bt_state.artist[0] = 0;
    g_bt_state.album[0] = 0;
    g_bt_state.codec[0] = 0;
    g_bt_state.duration_ms = 0;
    g_bt_state.position_ms = 0;
    g_bt_state.pos_anchor_ms = 0;
}

static void rx_mac_to_dbus(const char *mac, char *out, size_t cap){
    if(!out || cap == 0) return;
    size_t i = 0;
    for(; mac && mac[i] && i + 1 < cap; i++){
        char c = mac[i];
        if(c == ':') c = '_';
        else if(c >= 'a' && c <= 'f') c -= ('a' - 'A');
        out[i] = c;
    }
    out[i] = 0;
}

static void rx_bt_addr_remove(const char *path, const char *mac){
    if(!path || !mac || !mac[0]) return;
    FILE *f = fopen(path, "r");
    if(!f) return;

    char kept[16][64];
    int nkept = 0;
    char line[256];
    while(fgets(line, sizeof line, f)){
        char *nl = strpbrk(line, "\r\n");
        if(nl) *nl = 0;
        if(strcasestr(line, mac)) continue;
        const char *p = line;
        if(!strncmp(p, "Device ", 7)){
            const char *colon = strchr(p + 7, ':');
            if(colon && colon > p + 7 && (colon - (p + 7)) <= 3){
                p = colon + 1;
                while(*p == ' ' || *p == '\t') p++;
            }
        }
        if(*p && nkept < 16){
            snprintf(kept[nkept], sizeof kept[nkept], "%.63s", p);
            nkept++;
        }
    }
    fclose(f);

    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *wf = fopen(tmp, "w");
    if(!wf) return;
    for(int i = 0; i < nkept; i++){
        fprintf(wf, "Device %d: %s\n", i + 1, kept[i]);
    }
    if(fclose(wf) == 0){
        rename(tmp, path);
    } else {
        unlink(tmp);
    }
}

void bt_rx_disconnect(void){
    char mac[20] = {0};
    char dbus_mac[32] = {0};
    pthread_mutex_lock(&g_bt_mu);
    if(g_bt_state.connected && g_bt_state.mac[0]){
        snprintf(mac, sizeof mac, "%s", g_bt_state.mac);
        if(g_bt_state.dbus_mac[0]) snprintf(dbus_mac, sizeof dbus_mac, "%s", g_bt_state.dbus_mac);
        else rx_mac_to_dbus(mac, dbus_mac, sizeof dbus_mac);
        rx_state_clear_locked();
    }
    pthread_mutex_unlock(&g_bt_mu);

    if(mac[0]){
        atomic_store(&s_bt_force_drop, 1);
        rx_u64_store(&s_bt_drop_until, rx_now_ms() + 2500);
        char cmd[4096];
        snprintf(cmd, sizeof cmd,
            "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0/dev_%s org.bluez.Device1.Disconnect >/dev/null 2>&1; "
            "sleep 0.6; "
            BT_RX_ENABLE_DISCOVERY_SH,
            dbus_mac);
        diskos_trace("bt_rx_disconnect: mac=%s", mac);
        rx_bg_system(cmd);
        ui_toast(tr("Disconnected"));
    } else {
        ui_toast(tr("No device connected"));
    }
}

void bt_rx_forget(const char *mac){
    if(!rx_mac_valid(mac)) return;
    int is_cur = 0;
    int still_connected = 0;
    pthread_mutex_lock(&g_bt_mu);
    if(g_bt_state.connected && !strcasecmp(g_bt_state.mac, mac)){
        is_cur = 1;
        rx_state_clear_locked();
    } else if(g_bt_state.connected){
        still_connected = 1;
    }
    pthread_mutex_unlock(&g_bt_mu);

    if(is_cur){
        atomic_store(&s_bt_force_drop, 1);
        rx_u64_store(&s_bt_drop_until, rx_now_ms() + 2500);
    }

    rx_bt_addr_remove("/usr/data/fiio/bt_addr", mac);

    char dbus_mac[32] = {0};
    rx_mac_to_dbus(mac, dbus_mac, sizeof dbus_mac);

    char cmd[4096];
    if(!still_connected){
        snprintf(cmd, sizeof cmd,
            "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0/dev_%s org.bluez.Device1.Disconnect >/dev/null 2>&1; "
            "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0 org.bluez.Adapter1.RemoveDevice objpath:/org/bluez/hci0/dev_%s >/dev/null 2>&1; "
            "hciconfig hci0 delkey %s 2>/dev/null; "
            "sleep 0.5; "
            BT_RX_ENABLE_DISCOVERY_SH,
            dbus_mac, dbus_mac, mac);
    } else {
        snprintf(cmd, sizeof cmd,
            "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0/dev_%s org.bluez.Device1.Disconnect >/dev/null 2>&1; "
            "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0 org.bluez.Adapter1.RemoveDevice objpath:/org/bluez/hci0/dev_%s >/dev/null 2>&1; "
            "hciconfig hci0 delkey %s 2>/dev/null",
            dbus_mac, dbus_mac, mac);
    }
    diskos_trace("bt_rx_forget: mac=%s", mac);
    rx_bg_system(cmd);
    ui_toast(tr("Device forgotten"));
}

void bt_rx_force_pairing(void){
    uint64_t now = rx_now_ms();
    rx_u64_store(&s_bt_pairing_until, now + 60000);
    atomic_store(&s_bt_force_drop, 1);
    rx_u64_store(&s_bt_drop_until, now + 2500);

    char mac[20] = {0};
    char dbus_mac[32] = {0};
    pthread_mutex_lock(&g_bt_mu);
    if(g_bt_state.connected && g_bt_state.mac[0]){
        snprintf(mac, sizeof mac, "%s", g_bt_state.mac);
        if(g_bt_state.dbus_mac[0]) snprintf(dbus_mac, sizeof dbus_mac, "%s", g_bt_state.dbus_mac);
        else rx_mac_to_dbus(mac, dbus_mac, sizeof dbus_mac);
        rx_state_clear_locked();
    }
    pthread_mutex_unlock(&g_bt_mu);

    char cmd[4096];
    if(mac[0]){
        snprintf(cmd, sizeof cmd,
            "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0/dev_%s org.bluez.Device1.Disconnect >/dev/null 2>&1; "
            "for m in $(hcitool con 2>/dev/null | awk '/ACL/{print $3}'); do "
            "  dm=$(echo \"$m\" | tr ':' '_'); "
            "  dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez \"/org/bluez/hci0/dev_${dm}\" org.bluez.Device1.Disconnect >/dev/null 2>&1; "
            "done; "
            "sleep 0.6; "
            "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Discoverable variant:boolean:false >/dev/null 2>&1; "
            BT_RX_ENABLE_DISCOVERY_SH,
            dbus_mac);
    } else {
        snprintf(cmd, sizeof cmd,
            "for m in $(hcitool con 2>/dev/null | awk '/ACL/{print $3}'); do "
            "  dm=$(echo \"$m\" | tr ':' '_'); "
            "  dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez \"/org/bluez/hci0/dev_${dm}\" org.bluez.Device1.Disconnect >/dev/null 2>&1; "
            "done; "
            "sleep 0.6; "
            "dbus-send --system --print-reply --reply-timeout=1000 --dest=org.bluez /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Discoverable variant:boolean:false >/dev/null 2>&1; "
            BT_RX_ENABLE_DISCOVERY_SH);
    }
    diskos_trace("bt_rx_force_pairing: active");
    rx_bg_system(cmd);
    ui_toast(tr("Pairing mode active"));
}

int bt_rx_is_pairing(void){
    uint64_t until = rx_u64_load(&s_bt_pairing_until);
    if(!until) return 0;
    if(rx_now_ms() >= until){
        rx_u64_store(&s_bt_pairing_until, 0);
        return 0;
    }
    return 1;
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
    g_hdr_title = ui_header_cb(root, "USB DAC", modelock_back_btn_cb);

    /* 2. Container for USB DAC (Mode 1) */
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
    if(mode == 2){
        /* Bluetooth Receiving UI is consolidated onto SCR_NOWPLAYING (bt_view_tick). */
        if(g_lock_mode != 0) modelock_close();
        bt_rx_enter();
        screen_show(SCR_NOWPLAYING);
        return;
    }
    if(g_lock_mode != 0 && g_lock_mode != mode){
        modelock_close();
    }
    g_lock_mode = mode;

    /* Hide all sub-containers first */
    if(g_cont_dac)     lv_obj_add_flag(g_cont_dac, LV_OBJ_FLAG_HIDDEN);
    if(g_cont_storage) lv_obj_add_flag(g_cont_storage, LV_OBJ_FLAG_HIDDEN);

    if(mode == 1){
        /* USB DAC */
        if(g_hdr_title)  theme_title_text(g_hdr_title, "USB DAC");
        if(g_cont_dac)   lv_obj_remove_flag(g_cont_dac, LV_OBJ_FLAG_HIDDEN);
        if(!g_poll_timer){
            g_poll_timer = lv_timer_create(modelock_poll_cb, 300, NULL);
        }
    } else if(mode == 3){
        /* USB Storage */
        if(g_hdr_title)  theme_title_text(g_hdr_title, "USB Storage");
        if(g_cont_storage) lv_obj_remove_flag(g_cont_storage, LV_OBJ_FLAG_HIDDEN);
    }

    /* Update colors to current accent */
    lv_color_t acc = ui_current_accent();
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
    g_lock_mode = 0;
    exit_modal_close();

    if(g_poll_timer){
        lv_timer_del(g_poll_timer);
        g_poll_timer = NULL;
    }
}

int modelock_is_active(void){
    return (g_lock_mode != 0);
}

int modelock_get_mode(void){
    return g_lock_mode;
}
