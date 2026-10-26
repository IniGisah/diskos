/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_BT_PREPARE_H
#define DISKOS_BT_PREPARE_H
#ifdef DISKOS_TEST_BT_PREPARE
#include "ipc.h"
#include "bt_codec_observe.h"
/* Development candidate: no transport effects without the explicit build flag.
 * Selected codec is family evidence only; LDAC quality remains unverified. */
enum { BP_IDLE, BP_PAUSE, BP_POSITION, BP_ROUTE, BP_CAPS, BP_CODEC,
       BP_VERIFY, BP_VOLUME, BP_SEEK, BP_SEEKED, BP_RELEASE, BP_DONE, BP_CANCELLED, BP_FAILED };
typedef struct {
    track_state_t current, report;
    unsigned transport, report_serial, playing_frames, poweroff;
    int qualified, permitted;
} bp_input;
typedef struct {
    void *ctx;
    int (*send)(void *, const char *, int release, unsigned report, unsigned playing_frames);
    int (*probe_start)(void *, uint64_t *);
    int (*probe_read)(void *, uint64_t, bc_snapshot *, int *known, unsigned *available);
    void (*probe_cancel)(void *, uint64_t);
    void (*routed)(void *, const char *);
} bp_sink;
typedef struct {
    int phase, was_playing, choice, selected, route_step, held, queried;
    unsigned generation, transport, report, position_seq, playing_frames, poweroff, endpoint_sequence;
    uint32_t deadline, query_at;
    uint64_t attempt, probe;
    long position;
    track_state_t saved;
    char mac[18];
} bp_prepare;
int bp_active(const bp_prepare *);
int bp_begin(bp_prepare *, uint32_t now, const char *mac, int choice, int playing, const bp_input *);
void bp_cancel(bp_prepare *, const bp_sink *);
void bp_tick(bp_prepare *, uint32_t now, const bp_input *, const bp_sink *);
/* Native read-only probe. Start: 0 accepted, 1 worker busy, -1 refused.
 * Read: 0 pending, 1 complete matching capture, -1 failed/stale. Cancel never
 * frees the busy latch of a worker which has not actually finished. */
int bt_prepare_probe_start(const char *mac, unsigned generation, uint64_t *id);
int bt_prepare_probe_read(uint64_t id, unsigned generation, bc_snapshot *, int *, unsigned *);
void bt_prepare_probe_cancel(uint64_t id);
int bt_codec_pick(int choice, unsigned available, int known);
int bt_prepare_codec_frame(const char *mac, int choice, char *, int capacity);
void ui_bt_route_prepare_cancel(void);
#endif
#endif
