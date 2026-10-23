/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_MRP_H
#define DISKOS_AIRPLAY_MRP_H
#include "airplay_remote.h"
#include "airplay_plist.h"
typedef struct {
    aps_rtsp channel;
    char display_name[33];
    uint8_t input[16384], output[4096], wire[1042];
    size_t used, length, at, wire_n, wire_at;
    int64_t deadline;
    uint64_t sequence;
    char identifiers[8][129]; unsigned next_identifier;
    pb_view state; int have_state; int64_t state_at;
} aps_mrp;
void aps_mrp_init(aps_mrp *);
/* Only after pair-verify. Unsupported SETUP leaves the audio-session path in use. */
int aps_mrp_open(aps_mrp *, aps_rtsp *, aps_reply *, const char *host,
                 const uint8_t secret[32], const char *identity, const char *session);
int aps_mrp_service(aps_mrp *, aps_remote *);
int aps_mrp_volume(aps_mrp *, int percent); /* -1 queues a read, never a write */
int aps_mrp_state(aps_mrp *, const pb_view *);
/* Pure bounded protobuf adapter: 1 command/observation, 0 unsupported, -1 malformed. */
int aps_mrp_command(const void *, size_t, aps_message *);
void aps_mrp_close(aps_mrp *);
#endif
