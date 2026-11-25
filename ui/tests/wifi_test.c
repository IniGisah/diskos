/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Host unit tests for pure Wi-Fi lease state machine, Broadcom PM policy,
 * and formatting helpers in ui/wifi_logic.h. */
#include "wifi_logic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_no_deadlock_when_ip_zero(void){
    wifi_lease_sm_t sm = { .phase = WIFI_LEASE_FAILED, .since = 1000 };
    wifi_lease_in_t in = {
        .allowed = 1, .ip = 0, .dhcp_count = 1, .dhcp_idle = 1,
        .same_network = 0, .explicit_connect = 1, .now_ms = 5000
    };
    int rebind = 0;
    unsigned act = wifi_lease_sm_step(&sm, &in, &rebind);
    /* Must jump straight to ACQUIRE + SIGUSR1, never RELEASE/SIGUSR2 */
    assert(rebind == 1);
    assert(sm.phase == WIFI_LEASE_ACQUIRE);
    assert(act == WIFI_ACT_DHCP_RENEW);

    /* Next poll: IP acquired and hook idle -> READY */
    in.same_network = 1;
    in.explicit_connect = 0;
    in.ip = 1;
    in.now_ms = 6200;
    act = wifi_lease_sm_step(&sm, &in, &rebind);
    assert(rebind == 0);
    assert(sm.phase == WIFI_LEASE_READY);
    assert(act == WIFI_ACT_NONE);
}

static void test_boot_preserves_existing_ip(void){
    wifi_lease_sm_t sm = { .phase = WIFI_LEASE_NONE };
    wifi_lease_in_t in = {
        .allowed = 1, .ip = 1, .dhcp_count = 1, .dhcp_idle = 1,
        .same_network = 0, .explicit_connect = 0, .now_ms = 100
    };
    int rebind = 0;
    unsigned act = wifi_lease_sm_step(&sm, &in, &rebind);
    assert(rebind == 1);
    assert(sm.phase == WIFI_LEASE_READY);
    assert(act == WIFI_ACT_NONE);
}

static void test_network_switch_releases_and_flushes_stale_ip(void){
    wifi_lease_sm_t sm = { .phase = WIFI_LEASE_READY, .since = 100 };
    wifi_lease_in_t in = {
        .allowed = 1, .ip = 1, .dhcp_count = 1, .dhcp_idle = 1,
        .same_network = 0, .explicit_connect = 1, .now_ms = 1000
    };
    int rebind = 0;
    unsigned act = wifi_lease_sm_step(&sm, &in, &rebind);
    assert(rebind == 1);
    assert(sm.phase == WIFI_LEASE_RELEASE);
    assert(act == WIFI_ACT_DHCP_RELEASE);

    /* Still has stale IP at 3500ms -> flush fallback */
    in.same_network = 1;
    in.explicit_connect = 0;
    in.now_ms = 3500;
    act = wifi_lease_sm_step(&sm, &in, &rebind);
    assert(sm.phase == WIFI_LEASE_RELEASE);
    assert(act == WIFI_ACT_FLUSH_IP);

    /* IP cleared at 3600ms -> acquire + SIGUSR1 */
    in.ip = 0;
    in.now_ms = 3600;
    act = wifi_lease_sm_step(&sm, &in, &rebind);
    assert(sm.phase == WIFI_LEASE_ACQUIRE);
    assert(act == WIFI_ACT_DHCP_RENEW);

    /* IP bound at 4800ms -> READY */
    in.ip = 1;
    in.now_ms = 4800;
    act = wifi_lease_sm_step(&sm, &in, &rebind);
    assert(sm.phase == WIFI_LEASE_READY);
    assert(act == WIFI_ACT_NONE);
}

static void test_acquire_rekicks_and_timeout(void){
    wifi_lease_sm_t sm = { .phase = WIFI_LEASE_NONE };
    wifi_lease_in_t in = {
        .allowed = 1, .ip = 0, .dhcp_count = 1, .dhcp_idle = 1,
        .same_network = 0, .explicit_connect = 1, .now_ms = 1000
    };
    unsigned act = wifi_lease_sm_step(&sm, &in, NULL);
    assert(sm.phase == WIFI_LEASE_ACQUIRE);
    assert(act == WIFI_ACT_DHCP_RENEW);

    in.same_network = 1;
    in.now_ms = 3000;
    assert(wifi_lease_sm_step(&sm, &in, NULL) == WIFI_ACT_NONE);

    in.now_ms = 6000; /* >= 5000ms elapsed */
    assert(wifi_lease_sm_step(&sm, &in, NULL) == WIFI_ACT_DHCP_RENEW);
    in.now_ms = 6500; /* already kicked at 5000ms */
    assert(wifi_lease_sm_step(&sm, &in, NULL) == WIFI_ACT_NONE);

    in.now_ms = 13000; /* >= 12000ms elapsed */
    assert(wifi_lease_sm_step(&sm, &in, NULL) == WIFI_ACT_DHCP_RENEW);

    in.now_ms = 26000; /* >= 25000ms timeout */
    assert(wifi_lease_sm_step(&sm, &in, NULL) == WIFI_ACT_NONE);
    assert(sm.phase == WIFI_LEASE_FAILED);

    /* Retries after 2000ms cooldown */
    in.now_ms = 28000;
    assert(wifi_lease_sm_step(&sm, &in, NULL) == WIFI_ACT_DHCP_RENEW);
    assert(sm.phase == WIFI_LEASE_ACQUIRE);
}

static void test_dhcp_missing_or_duplicate_and_drop(void){
    wifi_lease_sm_t sm = { .phase = WIFI_LEASE_NONE };
    wifi_lease_in_t in = {
        .allowed = 1, .ip = 0, .dhcp_count = 0, .dhcp_idle = 0,
        .same_network = 0, .explicit_connect = 0, .now_ms = 100
    };
    assert(wifi_lease_sm_step(&sm, &in, NULL) == WIFI_ACT_ENSURE_DHCP);

    in.dhcp_count = 2;
    assert(wifi_lease_sm_step(&sm, &in, NULL) == WIFI_ACT_ENSURE_DHCP);

    /* Single udhcpc appears, obtains IP, then loses IP mid-session */
    in.dhcp_count = 1; in.dhcp_idle = 1; in.ip = 0; in.now_ms = 200;
    assert(wifi_lease_sm_step(&sm, &in, NULL) == WIFI_ACT_DHCP_RENEW);
    assert(sm.phase == WIFI_LEASE_ACQUIRE);

    in.same_network = 1; in.ip = 1; in.now_ms = 500;
    assert(wifi_lease_sm_step(&sm, &in, NULL) == WIFI_ACT_NONE);
    assert(sm.phase == WIFI_LEASE_READY);

    in.ip = 0; in.now_ms = 9000;
    assert(wifi_lease_sm_step(&sm, &in, NULL) == WIFI_ACT_DHCP_RENEW);
    assert(sm.phase == WIFI_LEASE_ACQUIRE);
}

static void test_pm_policy_and_formatting(void){
    assert(wifi_pm_desired(0, 0, 0, 0, 1, 10000) == -1);
    assert(wifi_pm_desired(1, 1, 0, 0, 1, 10000) == 0);
    assert(wifi_pm_desired(1, 0, 1, 0, 1, 10000) == 0);
    assert(wifi_pm_desired(1, 0, 0, 1, 1, 10000) == 0);
    assert(wifi_pm_desired(1, 0, 0, 0, 0, 10000) == 0);
    assert(wifi_pm_desired(1, 0, 0, 0, 1, 4999) == 0);
    assert(wifi_pm_desired(1, 0, 0, 0, 1, 5000) == 2);

    assert(wifi_freq_to_channel(2412) == 1);
    assert(wifi_freq_to_channel(2437) == 6);
    assert(wifi_freq_to_channel(2472) == 13);
    assert(wifi_freq_to_channel(2484) == 14);
    assert(wifi_freq_to_channel(0) == 0);

    char buf[64];
    wifi_format_freq(2437, buf, sizeof buf);
    assert(strcmp(buf, "2437 MHz (Ch 6)") == 0);
    wifi_format_signal(-52, buf, sizeof buf);
    assert(strcmp(buf, "-52 dBm (Excellent)") == 0);
    wifi_format_signal(-65, buf, sizeof buf);
    assert(strcmp(buf, "-65 dBm (Good)") == 0);
    wifi_format_signal(-74, buf, sizeof buf);
    assert(strcmp(buf, "-74 dBm (Fair)") == 0);
    wifi_format_signal(-85, buf, sizeof buf);
    assert(strcmp(buf, "-85 dBm (Weak)") == 0);
}

int main(void){
    test_no_deadlock_when_ip_zero();
    test_boot_preserves_existing_ip();
    test_network_switch_releases_and_flushes_stale_ip();
    test_acquire_rekicks_and_timeout();
    test_dhcp_missing_or_duplicate_and_drop();
    test_pm_policy_and_formatting();
    puts("wifi_test: all assertions passed");
    return 0;
}
