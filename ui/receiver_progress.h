/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef RECEIVER_PROGRESS_H
#define RECEIVER_PROGRESS_H
#include <stdint.h>
#include "ipc.h"

/* Pure progress model for the AirPlay receiver (no LVGL, no clock of its own:
 * every call passes the monotonic time in ms). Inputs are a103 position samples,
 * a202 item/duration reports and the play/pause state; output is what to draw. */
#define RP_STALE_MS 3000       /* playing with no a103 for this long: hide */
#define RP_DURATION_MS 4000    /* a duration after an item change is trusted after this long */

typedef struct {
    int show;                  /* 0: draw no bar and no times */
    long elapsed_ms, duration_ms;
    int fraction;              /* elapsed/duration in permille, 0..1000 */
} rp_out;

typedef struct {
    uint64_t item, item_ms;    /* current item revision and when it began */
    long duration_ms; int reports; /* 0: absent, 1: first value, 2: value changed */
    int pos_valid; long anchor_ms; uint64_t anchor_at, fresh_at; /* sample, its time, last time it was live */
    int playing; long frozen_ms;        /* paused: the value held */
    uint64_t gen, session; unsigned pos_seq, dur_seq; int synced;
} rp_t;

void rp_init(rp_t *p);
void rp_item(rp_t *p, uint64_t item, uint64_t now_ms);
void rp_duration(rp_t *p, uint64_t item, long duration_ms, int reports, uint64_t now_ms);
void rp_position(rp_t *p, uint64_t item, long position_ms, uint64_t now_ms);
void rp_playing(rp_t *p, int playing, uint64_t now_ms);
void rp_get(const rp_t *p, uint64_t now_ms, rp_out *out);
/* Feed the model from a receiver snapshot (new samples are detected by sequence). */
void rp_sync(rp_t *p, const ipc_receiver_meta *m, int playing, uint64_t now_ms);
#endif
