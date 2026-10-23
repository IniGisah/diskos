/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_TLV_H
#define DISKOS_AIRPLAY_TLV_H
#include <stddef.h>
#include <stdint.h>
#define APS_TLV_MAX 2048u
typedef struct { uint8_t b[APS_TLV_MAX]; size_t n; } aps_tlv;
/* Strict singleton TLV8. Only consecutive 255-byte fragments may repeat a
 * field. A repeated singleton, trailing byte or truncated value is an error.
 * Absent field returns 0; invalid returns -1; present returns its length. */
int aps_tlv_get(const void *, size_t, unsigned tag, void *out, size_t cap);
int aps_tlv_put(aps_tlv *, unsigned tag, const void *, size_t);
int aps_tlv_byte(aps_tlv *, unsigned tag, unsigned value);
int aps_tlv_state(const aps_tlv *, unsigned state);
#endif
