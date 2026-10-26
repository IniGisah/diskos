/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AUDIO_TIMELINE_H
#define DISKOS_AUDIO_TIMELINE_H
#include <stdint.h>
#define AUDIO_TIMELINE_ITEMS 8
enum { ATL_OK, ATL_ARGUMENT = -1, ATL_STALE = -2, ATL_FULL = -3, ATL_ORDER = -4 };
typedef struct {
    uint64_t item, occurrence, start, count, offset;
    int ended;
} atl_span;
typedef struct {
    uint64_t generation, submitted, audible;
    unsigned rate, head, n; int have_playhead;
    atl_span span[AUDIO_TIMELINE_ITEMS];
} audio_timeline;
typedef struct {
    int have_item, input_ended, drained;
    uint64_t item, occurrence, position_frames, position_ms;
} atl_view;
/* Pure, single-worker ledger, no allocation, clock, transport or UI. All counters
 * are OUTPUT PCM frames at one fixed rate, never source frames or RTP uint32.
 * item = unique nonzero playback-pass token (also new on repeat-one); occurrence
 * = nonzero queue occurrence, so duplicate songs are distinct. Inspect state only
 * through get; publish a copied view to the UI, never share this mutable object.
 * Begin/reset with a fresh nonzero generation, including after seek/flush. Invalid
 * calls and stale generations never mutate state. Rate range is 8..384 kHz. */
int audio_timeline_begin(audio_timeline *, uint64_t generation, unsigned rate);
/* Record only frames ACCEPTED into the transport's ordered send buffer. offset
 * is the first frame's track-relative output position (nonzero after a seek).
 * Same-item appends must be contiguous; a new item needs the previous end marker.
 * FULL means no change: wait for audible retirement before committing more spans.
 * To reserve capacity for a packet, append to a stack copy, then commit that copy
 * only after the transport accepts the entire batch. For a partial acceptance,
 * rebuild from the live ledger with only the accepted prefix. End only after all
 * this item's output is accepted, including the converter's drained tail.
 * EOF/end does not move the audible view. Empty tracks create no audible span. */
int audio_timeline_append(audio_timeline *, uint64_t generation, uint64_t item,
                          uint64_t occurrence, uint64_t offset, uint64_t frames);
int audio_timeline_end(audio_timeline *, uint64_t generation, uint64_t item);
/* Adapter supplies a qualified monotonic audible playhead relative to begin=0,
 * from its scheduled/confirmed transport anchor. No anchor is invented here.
 * Frames outside submitted content or behind the prior playhead are refused.
 * At an exact boundary the NEXT submitted item becomes visible. Only observe can
 * advance the visible item; decoder EOF, buffering and appends cannot. */
int audio_timeline_observe(audio_timeline *, uint64_t generation, uint64_t frame);
/* Before the first observe: no item. input_ended marks submitted EOF, not audible
 * completion. drained = last submitted item ended and its
 * last frame reached; not a queue-finished event. Pause freezes by not advancing
 * observe. position_ms floors from absolute frames and saturates on uint64 overflow. */
int audio_timeline_get(const audio_timeline *, uint64_t generation, atl_view *);
#endif
