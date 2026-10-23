/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_RAOP_H
#define DISKOS_AIRPLAY_RAOP_H
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t key[16], iv[16], challenge[16]; } aps_raop_key;
int aps_raop_create(aps_raop_key *, char wrapped[345], char iv[25], char challenge[25]);
/* Verify the legacy common-key RSA response before audio. This is the RAOP
 * protocol challenge, not a claim of unique receiver identity authentication. */
int aps_raop_check(const aps_raop_key *, const char *response, const uint8_t ipv4[4]);
int aps_raop_encrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t *, size_t, uint8_t *);
#endif
