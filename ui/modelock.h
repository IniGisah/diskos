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
