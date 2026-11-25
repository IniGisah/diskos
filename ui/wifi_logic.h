/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Pure, deterministic Wi-Fi DHCP lease state machine, Broadcom PM policy,
 * and formatting helpers shared between ui/wifi.c and host unit tests. */
#ifndef DISKOS_WIFI_LOGIC_H
#define DISKOS_WIFI_LOGIC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum {
    WIFI_LEASE_NONE = 0,
    WIFI_LEASE_RELEASE,
    WIFI_LEASE_ACQUIRE,
    WIFI_LEASE_READY,
    WIFI_LEASE_FAILED
};

enum {
    WIFI_ACT_NONE         = 0,
    WIFI_ACT_ENSURE_DHCP  = 1 << 0,
    WIFI_ACT_DHCP_RELEASE = 1 << 1,
    WIFI_ACT_FLUSH_IP     = 1 << 2,
    WIFI_ACT_DHCP_RENEW   = 1 << 3
};

typedef struct {
    int phase;
    uint32_t since;
    int flushed;
    int seen_empty;
    unsigned rekick_mask;
} wifi_lease_sm_t;

typedef struct {
    int allowed;          /* want && !reco_hold && !rfkill_blocked && wpa_up && wpa_completed */
    int ip;               /* -1 = iface down, 0 = no IPv4 addr, 1 = has IPv4 addr */
    int dhcp_count;       /* 0 = absent, 1 = single udhcpc, >1 = duplicate, <0 = unknown */
    int dhcp_idle;        /* 1 = no running udhcpc hook script child */
    int same_network;     /* 1 = matches active lease owner/network/epoch/player */
    int explicit_connect; /* 1 = user explicitly initiated/switched connection */
    uint32_t now_ms;
} wifi_lease_in_t;

static inline unsigned wifi_lease_sm_step(wifi_lease_sm_t *sm, const wifi_lease_in_t *in,
                                           int *out_rebind_owner)
{
    if (out_rebind_owner) *out_rebind_owner = 0;

    if (!in->allowed) {
        if (sm->phase == WIFI_LEASE_RELEASE || sm->phase == WIFI_LEASE_ACQUIRE) {
            sm->phase = WIFI_LEASE_FAILED;
            sm->since = in->now_ms;
        } else if (sm->phase == WIFI_LEASE_READY) {
            sm->phase = WIFI_LEASE_NONE;
            sm->since = in->now_ms;
        }
        return WIFI_ACT_NONE;
    }

    if (in->dhcp_count != 1) {
        /* Missing or duplicate udhcpc: let caller ensure a single client. */
        if (sm->phase == WIFI_LEASE_READY && in->ip != 1) {
            sm->phase = WIFI_LEASE_NONE;
            sm->since = in->now_ms;
        }
        return WIFI_ACT_ENSURE_DHCP;
    }

    int start_fresh = (sm->phase == WIFI_LEASE_NONE) ||
                      (!in->same_network) ||
                      (sm->phase == WIFI_LEASE_FAILED && (uint32_t)(in->now_ms - sm->since) >= 2000u);

    if (start_fresh) {
        if (out_rebind_owner) *out_rebind_owner = 1;
        sm->since = in->now_ms;
        sm->flushed = 0;
        sm->rekick_mask = 0;

        /* Preserve an already-valid IP on boot/startup when not switching networks. */
        if (sm->phase == WIFI_LEASE_NONE && !in->explicit_connect && in->ip == 1 && in->dhcp_idle) {
            sm->phase = WIFI_LEASE_READY;
            sm->seen_empty = 0;
            return WIFI_ACT_NONE;
        }

        /* Never send SIGUSR2 when wlan0 already has no IP; go straight to SIGUSR1. */
        if (in->ip == 0) {
            sm->phase = WIFI_LEASE_ACQUIRE;
            sm->seen_empty = 1;
            return WIFI_ACT_DHCP_RENEW;
        }

        /* Stale IP exists on a new network or explicit reconnect: release first. */
        sm->phase = WIFI_LEASE_RELEASE;
        sm->seen_empty = 0;
        return WIFI_ACT_DHCP_RELEASE;
    }

    if (sm->phase == WIFI_LEASE_RELEASE) {
        if (in->ip == 0) {
            sm->phase = WIFI_LEASE_ACQUIRE;
            sm->since = in->now_ms;
            sm->seen_empty = 1;
            sm->flushed = 0;
            sm->rekick_mask = 0;
            return WIFI_ACT_DHCP_RENEW;
        }
        uint32_t elapsed = (uint32_t)(in->now_ms - sm->since);
        if (elapsed >= 8000u) {
            sm->phase = WIFI_LEASE_FAILED;
            sm->since = in->now_ms;
            return WIFI_ACT_NONE;
        }
        if (elapsed >= 2500u && !sm->flushed) {
            sm->flushed = 1;
            return WIFI_ACT_FLUSH_IP;
        }
        return WIFI_ACT_NONE;
    }

    if (sm->phase == WIFI_LEASE_ACQUIRE) {
        if (in->ip == 1 && in->dhcp_idle) {
            sm->phase = WIFI_LEASE_READY;
            sm->since = in->now_ms;
            return WIFI_ACT_NONE;
        }
        uint32_t elapsed = (uint32_t)(in->now_ms - sm->since);
        if (elapsed >= 25000u) {
            sm->phase = WIFI_LEASE_FAILED;
            sm->since = in->now_ms;
            return WIFI_ACT_NONE;
        }
        if (in->ip == 0) {
            if (elapsed >= 12000u && !(sm->rekick_mask & 2u)) {
                sm->rekick_mask |= 2u;
                return WIFI_ACT_DHCP_RENEW;
            }
            if (elapsed >= 5000u && !(sm->rekick_mask & 1u)) {
                sm->rekick_mask |= 1u;
                return WIFI_ACT_DHCP_RENEW;
            }
        }
        return WIFI_ACT_NONE;
    }

    if (sm->phase == WIFI_LEASE_READY) {
        if (in->ip == 0) {
            sm->phase = WIFI_LEASE_ACQUIRE;
            sm->since = in->now_ms;
            sm->seen_empty = 1;
            sm->rekick_mask = 0;
            return WIFI_ACT_DHCP_RENEW;
        }
        return WIFI_ACT_NONE;
    }

    return WIFI_ACT_NONE;
}

/* Broadcom AP6212 / BCM43438 power-save policy:
 * Keep CAM (PM 0) whenever screen is lit, scanning, connecting, or DHCP is not
 * yet settled for >= 5000ms. Only enter DTIM sleep (PM 2) when dark and idle. */
static inline int wifi_pm_desired(int radio_up, int screen_lit, int scanning,
                                  int conn_active, int lease_ready, uint32_t ready_age_ms)
{
    if (!radio_up) return -1;
    if (screen_lit || scanning || conn_active || !lease_ready || ready_age_ms < 5000u)
        return 0;
    return 2;
}

static inline int wifi_freq_to_channel(int freq_mhz)
{
    if (freq_mhz >= 2412 && freq_mhz <= 2472 && ((freq_mhz - 2412) % 5) == 0)
        return 1 + (freq_mhz - 2412) / 5;
    if (freq_mhz == 2484) return 14;
    if (freq_mhz >= 5180 && freq_mhz <= 5885 && ((freq_mhz - 5000) % 5) == 0)
        return (freq_mhz - 5000) / 5;
    return 0;
}

static inline const char *wifi_rssi_quality(int rssi)
{
    if (rssi >= -55) return "Excellent";
    if (rssi >= -67) return "Good";
    if (rssi >= -78) return "Fair";
    return "Weak";
}

static inline void wifi_format_freq(int freq_mhz, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (freq_mhz <= 0) { out[0] = 0; return; }
    int ch = wifi_freq_to_channel(freq_mhz);
    if (ch > 0) snprintf(out, cap, "%d MHz (Ch %d)", freq_mhz, ch);
    else snprintf(out, cap, "%d MHz", freq_mhz);
}

static inline void wifi_format_signal(int rssi, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (rssi > 0 || rssi < -200) { out[0] = 0; return; }
    snprintf(out, cap, "%d dBm (%s)", rssi, wifi_rssi_quality(rssi));
}

#endif /* DISKOS_WIFI_LOGIC_H */
