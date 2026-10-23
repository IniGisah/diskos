/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_SESSION_H
#define DISKOS_AIRPLAY_SESSION_H
#include "airplay_ipc.h"
#include "airplay_pair.h"
#include "airplay_plist.h"
#include "airplay_rtp.h"
#include "airplay_remote.h"
#include "airplay_raop.h"
#include "airplay_events.h"
#include "airplay_mrp.h"
#include <netinet/in.h>
typedef struct {
    aps_rtsp rtsp, events; aps_remote remote; aps_events event_state; aps_mrp mrp;
    aps_reply reply;
    aps_rtp rtp; aps_raop_key legacy_key; int legacy_rsa;
    rd_receiver target;
    char sender_name[33]; /* active identity copied when the session is created */
    struct sockaddr_in data_peer, control_peer, timing_peer;
    int data, control, timing, legacy, prepared, started, error, quiet, verified;
    uint16_t data_port, control_port, timing_port;
    unsigned rate, bits, packet_frames; uint64_t formats;
    uint8_t audio_key[32];
    uint32_t anchor_rtp; int64_t wall_ns, mono_ms, last_sync, last_feedback, rtx_window; unsigned rtx_sent;
} aps_session;
int aps_session_forget(const char *receiver_id);
void aps_session_init(aps_session *, int (*cancel)(void *), void *);
int aps_session_info(aps_session *, const void *body, size_t);
int aps_session_prepare(aps_session *, const rd_receiver *, const aps_pair_io *);
int aps_session_start(aps_session *, unsigned source_rate, unsigned source_bits);
int aps_session_packet(aps_session *, const void *, size_t frames);
/* Service timing and bounded retransmissions without blocking. */
int aps_session_service(aps_session *);
int aps_session_flush(aps_session *);
int aps_session_pause(aps_session *);
int aps_session_resume(aps_session *);
int aps_session_art(aps_session *, const void *, size_t, uint32_t track_start);
int aps_session_metadata(aps_session *, const pb_view *, uint32_t track_start);
int aps_session_progress(aps_session *, const pb_view *, uint32_t track_start);
int aps_session_volume_read(aps_session *, int *percent);
/* Explicit intent only: 0 acknowledged, 1 MRP queued awaiting observation, -1 refused. */
int aps_session_volume(aps_session *, int percent);
int aps_session_close(aps_session *);
unsigned aps_quality_bits(uint64_t formats, unsigned source_rate, unsigned source_bits, unsigned *bits);
unsigned aps_quality(uint64_t formats, unsigned source_rate);
int aps_session_pair_post(void *, const char *, const aps_tlv *, aps_tlv *);
#endif
