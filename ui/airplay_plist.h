/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_PLIST_H
#define DISKOS_AIRPLAY_PLIST_H
#include "airplay_rtsp.h"
#include <plist/plist.h>
/* Validate object counts, expanded node count, byte lengths and depth BEFORE
 * calling libplist. The parser never sees XML, JSON, oversized or cyclic data. */
plist_t aps_plist_read(const void *, size_t);
int aps_plist_request(aps_rtsp *, const char *method, plist_t, aps_reply *, unsigned ms);
int aps_plist_request_path(aps_rtsp *, const char *method, const char *path, plist_t, aps_reply *, unsigned ms);
int aps_plist_uint(plist_t dict, const char *key, uint64_t *);
int aps_plist_port(plist_t dict, const char *key, uint16_t *);
void aps_plist_int(plist_t dict, const char *key, uint64_t);
void aps_plist_text(plist_t dict, const char *key, const char *);
#endif
