/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AUDIO_FEED_H
#define DISKOS_AUDIO_FEED_H
#include <stddef.h>
#include "audio_timeline.h"
#define AUDIO_FEED_FRAMES 4096u
#define AUDIO_FEED_PACKET 1024u
enum { AF_OK, AF_EMPTY, AF_FULL, AF_ARGUMENT = -1, AF_STALE = -2, AF_ORDER = -3, AF_BUSY = -4 };
typedef struct { uint64_t item, occurrence, offset; size_t frames; int ended; } af_span;
typedef struct {
    const int16_t *pcm; size_t frames;
    uint64_t generation, ticket;
    const int32_t *pcm24; unsigned bits;
} af_packet;
typedef struct {
    audio_timeline timeline;
    af_span span[AUDIO_TIMELINE_ITEMS];
    union { int16_t pcm[AUDIO_FEED_FRAMES * 2]; int32_t pcm24[AUDIO_FEED_FRAMES]; };
    union { int16_t offered[AUDIO_FEED_PACKET * 2]; int32_t offered24[AUDIO_FEED_PACKET]; };
    unsigned bits;
    size_t head, used, n, offer;
    uint64_t ticket, last_item, last_occurrence, next_offset;
    int ended;
} audio_feed;
/* 24-bit mode uses signed int32 samples, 2048 staged frames and 512 offered
 * frames in the same storage as s16 mode. No extra feed allocation.
 * Zero-initialize before first begin. Single worker owns all calls. No IO,
 * allocation, transport, volume or clock. Stereo s16 or signed s24 at a fixed 44100/48000 Hz.
 * Hold SD admission until the decoder/worker is closed and reaped, separately.
 * A strictly increasing generation follows a real transport flush/re-anchor
 * on seek. Retire the worker at counter exhaustion; never reuse a generation. */
int audio_feed_begin_pcm(audio_feed *, uint64_t generation, unsigned rate, unsigned bits);
int audio_feed_push24(audio_feed *, uint64_t generation, uint64_t item, uint64_t occurrence, uint64_t offset, const int32_t *, size_t frames);
int audio_feed_begin(audio_feed *, uint64_t generation, unsigned rate);
size_t audio_feed_space(const audio_feed *);
/* Already converted output frames. FULL/BUSY/error consumes nothing: retain
 * the caller's block for retry. New playback-pass items need the previous end;
 * repeated queue occurrences use a fresh item. Offset is output-track-relative.
 * Call end only after the converter's complete tail was successfully staged. */
int audio_feed_push(audio_feed *, uint64_t generation, uint64_t item,
                    uint64_t occurrence, uint64_t offset, const int16_t *, size_t frames);
int audio_feed_end(audio_feed *, uint64_t generation, uint64_t item);
/* Reserve ledger space before exposing PCM to the transport. Repeated peek
 * returns the SAME stable packet until positive acceptance or a reset. While
 * offered, push/end return BUSY; only this worker may observe or accept it.
 * FULL means wait for audible retirement, without losing staged PCM. */
int audio_feed_peek(audio_feed *, uint64_t generation, size_t limit, af_packet *);
/* Accept the prefix copied into transport-owned storage (or fully consumed).
 * A zero-copy transport must retain the offer until it releases the bytes.
 * Zero keeps the
 * offer for retry; a positive prefix invalidates it. Old tickets/generations
 * cannot consume replacement audio. Retransmission must never call accept. */
int audio_feed_accept(audio_feed *, const af_packet *, size_t frames);
/* A qualified audible anchor, never enqueue/decoder position. Pause freezes it.
 * The timeline's view changes only here. EOF and prefetch cannot advance it. */
int audio_feed_observe(audio_feed *, uint64_t generation, uint64_t frame, atl_view *);
int audio_feed_view(const audio_feed *, uint64_t generation, atl_view *);
#endif
