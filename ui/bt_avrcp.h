/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef DISKOS_BT_AVRCP_H
#define DISKOS_BT_AVRCP_H

/* Initialize AVRCP bridge: ensures diskos-avrcpd is running and connects to it */
void bt_avrcp_init(void);

/* Descriptor for ui_wake_wait polling (-1 if disconnected) */
int bt_avrcp_fd(void);

/* Poll for incoming AVRCP commands from Bluetooth device */
void bt_avrcp_poll(void);

/* Publish playback state and track metadata to AVRCP daemon */
void bt_avrcp_sync_state(int playing, const char *title, const char *artist, const char *album);

#endif /* DISKOS_BT_AVRCP_H */
