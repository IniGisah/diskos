/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_BT_PREPARE_H
#define DISKOS_BT_PREPARE_H
#include "ipc.h"
#include "bt_codec_observe.h"

/* Production Bluetooth route & codec preparation state machine.
 * Probes BlueALSA capabilities off-thread, applies the best matching codec
 * without racing mq_player's PCM open, and restores volume/seek/play state. */
enum {
    BP_IDLE, BP_PAUSE, BP_POSITION, BP_ROUTE, BP_CAPS, BP_APPLY_CODEC,
    BP_CODEC, BP_VERIFY, BP_VOLUME, BP_SEEK, BP_SEEKED, BP_RELEASE,
    BP_DONE, BP_CANCELLED, BP_FAILED
};

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
    int (*codec_start)(void *, const char *pcm, const char *codec_name, uint64_t *);
    int (*codec_read)(void *, uint64_t);
    void (*codec_cancel)(void *, uint64_t);
    void (*routed)(void *, const char *);
    void (*toast_fallback)(void *, int choice, int selected);
} bp_sink;

typedef struct {
    int phase, was_playing, choice, selected, route_step, held, queried;
    int reconfig_only, apply_started, apply_done;
    unsigned generation, transport, report, position_seq, playing_frames, poweroff, endpoint_sequence;
    unsigned observed_codec;
    uint32_t deadline, query_at, verify_at, position_wait_at, seek_at;
    uint64_t attempt, probe, codec_job;
    long position;
    track_state_t saved;
    char mac[18];
    char pcm[128];
} bp_prepare;

int bp_active(const bp_prepare *);
int bp_begin(bp_prepare *, uint32_t now, const char *mac, int choice, int playing, const bp_input *);
int bp_begin_ex(bp_prepare *, uint32_t now, const char *mac, int choice, int playing, int reconfig_only, const bp_input *);
void bp_cancel(bp_prepare *, const bp_sink *);
void bp_tick(bp_prepare *, uint32_t now, const bp_input *, const bp_sink *);

/* Native read-only probe. Start: 0 accepted, 1 worker busy, -1 refused.
 * Read: 0 pending, 1 complete matching capture, -1 failed/stale. Cancel never
 * frees the busy latch of a worker which has not actually finished. */
int bt_prepare_probe_start(const char *mac, unsigned generation, uint64_t *id);
int bt_prepare_probe_read(uint64_t id, unsigned generation, bc_snapshot *, int *, unsigned *);
void bt_prepare_probe_cancel(uint64_t id);

/* Native off-thread `bluealsa-cli codec <pcm> <cname>` apply job. */
int bt_prepare_codec_apply_start(const char *pcm, const char *codec_name, unsigned generation, uint64_t *id);
int bt_prepare_codec_apply_read(uint64_t id, unsigned generation);
void bt_prepare_codec_apply_cancel(uint64_t id);

int bt_codec_pick(int choice, unsigned available, int known);
int bt_prepare_codec_frame(const char *mac, int choice, char *, int capacity);
void ui_bt_route_prepare_cancel(void);
int ui_bt_route_prepare_busy(void);
int ui_bt_reconfigure_codec(int choice);
#endif
