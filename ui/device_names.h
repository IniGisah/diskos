/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_DEVICE_NAMES_H
#define DISKOS_DEVICE_NAMES_H
#include <stddef.h>
#define DN_BT_BYTES 248
#define DN_DEFAULT_BT "SNOWSKY DISC"
#define DN_DEFAULT_SENDER "Snowsky Disc"
#define DN_SAVED "/usr/data/diskos-network-name"
#define DN_BT "/usr/data/fiio/bt_name.txt"
#define DN_INTENT "/usr/data/diskos-names-reset"
typedef enum { DN_FAILED=-1, DN_UNCERTAIN=-2, DN_OK=0 } dn_result;
typedef struct {
    char saved[33], active[33], bluetooth[DN_BT_BYTES+1];
    int saved_valid, bluetooth_valid, capability, activated, pending;
} dn_snapshot;
/* Blocking filesystem work belongs on the names owner, never in LVGL callbacks. */
void dn_read(dn_snapshot *);
int dn_save_network(const char *); /* empty restores stock */
int dn_save_bt(const char *);      /* new ASCII entry */
int dn_saved_bt_valid(const char *); /* bounded UTF-8 replay, not entry */
void dn_sender_copy(char out[33]); /* active snapshot only, stock fallback */
void dn_bt_name_copy(char *out, size_t out_sz);
/* Shared checked persistence for the reset journal. Missing read returns 1. */
int dn_read_file(const char *, char *, size_t, size_t *);
int dn_replace(const char *, const char *, size_t);
int dn_remove(const char *);
#endif
