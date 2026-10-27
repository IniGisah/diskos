/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#pragma once
#include "lvgl/lvgl.h"

/* Mode Lockdown UI for external audio & device modes:
 * Mode 1: USB DAC (sound card for PC)
 * Mode 2: Bluetooth Receiving (A2DP sink with AVRCP controls & metadata)
 * Mode 3: USB Storage (mass storage card reader)
 * Mode 0 (Local Playback) and Mode 4 (USB DAC Output) do not lock down.
 */

/* Bluetooth AVRCP state cache (thread-safe) */
typedef struct {
    int connected;
    int is_playing;
    char dev_name[64];
    char local_name[64];
    char mac[20];
    char dbus_mac[32];   /* MAC with colons replaced by underscores */
    char title[128];
    char artist[128];
    char album[128];
    char codec[64];
    uint32_t duration_ms;       /* track length in ms from AVRCP */
    uint32_t position_ms;       /* position sample in ms */
    uint64_t pos_anchor_ms;     /* monotonic time when sample was received */
} bt_rx_state_t;

void modelock_create(lv_obj_t *root);
void modelock_open(int mode);
void modelock_close(void);
int  modelock_is_active(void);
int  modelock_get_mode(void);

/* AVRCP transport commands sent to the connected phone in BT receiving mode */
void bt_rx_play_pause(void);
void bt_rx_next(void);
void bt_rx_prev(void);
void bt_rx_sync_volume(int vol);
void modelock_prompt_exit(void);

/* Bluetooth Receiver State & Progress API */
void bt_rx_worker_ensure(void);
void bt_rx_get_state(bt_rx_state_t *out);
void bt_rx_get_progress(uint32_t *out_elapsed_ms, uint32_t *out_dur_ms, int *out_fraction);

/* Diagnostic trace and logging */
void diskos_trace(const char *fmt, ...);
void bt_rx_log(const char *fmt, ...);

