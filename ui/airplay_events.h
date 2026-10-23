/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_EVENTS_H
#define DISKOS_AIRPLAY_EVENTS_H
#include "airplay_remote.h"
#include "airplay_plist.h"
typedef struct {
    uint8_t input[16384], output[1042]; size_t used, written, length;
    int64_t deadline; unsigned last_seq; int have_seq;
} aps_events;
/* Pure audio-session MediaRemote plist adapter. 1 command, 0 informational or
 * unsupported, -1 malformed. This is NOT the type-130 protobuf command enum. */
int aps_event_command(const void *, size_t, aps_message *);
int aps_events_service(aps_events *, aps_rtsp *, aps_remote *);
/* 0 accepted, 1 optional advertisement rejected, -1 connection/auth failure. */
int aps_events_advertise(aps_rtsp *, aps_reply *);
#endif
