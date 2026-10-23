/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_BT_CODEC_OBSERVE_H
#define DISKOS_BT_CODEC_OBSERVE_H
#include <stddef.h>
#include <stdint.h>
enum { BC_OK, BC_MISSING, BC_MISMATCH, BC_STALE, BC_TIMEOUT, BC_ARGUMENT = -1 };
enum { BC_CODEC_OTHER, BC_CODEC_SBC, BC_CODEC_AAC, BC_CODEC_LDAC };
typedef struct {
    char pcm[128], mac[18], format[20], codec_name[24];
    unsigned codec, adapter, sequence, rate, channels;
    uint32_t observed_ms;
} bc_snapshot;
typedef struct { uint64_t attempt, route, player; } bc_token;
typedef struct {
    bc_token token; char mac[18]; uint32_t deadline;
    int pending, have; bc_snapshot snapshot;
} bc_observer;
/* Pure bounded parser for the stock BlueALSA v4 CLI info, prefixed by the
 * existing query's PCM=<path> line. Caller passes successful complete capture,
 * length <=4096. No query, socket, playback/radio command or allocation here.
 * Requires matching PCM/Device adapter+MAC, A2DP-source and sink mode.
 * Availability is NOT selection. LDAC quality is NOT exposed by this output.
 * Any failure leaves the caller's destination unchanged. */
int bc_parse(const char *, size_t length, const char *expected_mac, bc_snapshot *);
/* Zero-init, single UI/coordinator owner. Parse completion is published only
 * for the current request/route/player before the monotonic tick deadline.
 * max wait is 30 seconds; an old result cannot clear a replacement request.
 * This observes a codec; it does not prove a usable stream or transport intent. */
int bc_begin(bc_observer *, uint64_t route, uint64_t player, const char *mac,
             uint32_t now, uint32_t wait_ms, bc_token *);
int bc_complete(bc_observer *, const bc_token *, uint64_t current_route,
                uint64_t current_player, uint32_t now, const char *, size_t length);
void bc_cancel(bc_observer *);
int bc_get(const bc_observer *, uint64_t route, uint64_t player, bc_snapshot *);
#endif
