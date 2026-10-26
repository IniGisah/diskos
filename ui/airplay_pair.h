/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_PAIR_H
#define DISKOS_AIRPLAY_PAIR_H
#include "airplay_store.h"
#include "airplay_tlv.h"
enum { APS_PAIR_AUTH = -2, APS_PAIR_SIGNATURE = -3 };
typedef struct {
    void *ctx;
    int (*post)(void *, const char *path, const aps_tlv *, aps_tlv *response);
    int (*pin)(void *, char out[9]);  /* called after receiver issued M2 */
} aps_pair_io;
/* No persistence or logging here. Success is the only permission to save.
 * Failure wipes credential outputs, session keys and all temporary secrets. */
int aps_pair_setup(const aps_pair_io *, aps_identity *);
int aps_pair_verify(const aps_pair_io *, const aps_identity *, uint8_t write_key[32], uint8_t read_key[32], uint8_t audio_key[32]);
/* Authentication gate shared by setup and verify. Identifier length, exact
 * remembered identity and Ed25519 signature all MUST pass. */
int aps_pair_peer(const aps_identity *, const uint8_t peer_ephemeral[32],
                  const uint8_t our_ephemeral[32], const aps_tlv *decrypted);
#endif
