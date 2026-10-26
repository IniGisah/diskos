/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_STORE_H
#define DISKOS_AIRPLAY_STORE_H
#include <stdint.h>
typedef struct {
    char client_id[37], peer_id[65];
    uint8_t seed[32], peer_key[32];
} aps_identity;
/* dirfd is an already securely opened directory under /usr/data; no symlinks
 * in its path. Receiver identity is hashed into a fixed ASCII basename.
 * -1 = invalid/IO, 1 = absent, 0 = loaded/saved. Never logs secrets.
 * Save is 0600, fsync + atomic rename + directory fsync. Load requires exact
 * size, regular type, owner, mode 0600 and single link; no repair in place. */
int aps_store_load(int dirfd, const char *receiver, aps_identity *);
int aps_store_save(int dirfd, const char *receiver, const aps_identity *);
int aps_store_forget(int dirfd, const char *receiver);
int aps_identity_new(aps_identity *);
#endif
