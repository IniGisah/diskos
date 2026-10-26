/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_feed.h"
#include <string.h>
static int current(const audio_feed *f, uint64_t g){
    if(!f || !g || !f->timeline.generation) return AF_ARGUMENT;
    return g == f->timeline.generation ? AF_OK : AF_STALE;
}
static int result(int r){
    return r == ATL_OK ? AF_OK : r == ATL_FULL ? AF_FULL : r == ATL_STALE ? AF_STALE :
           r == ATL_ORDER ? AF_ORDER : AF_ARGUMENT;
}
static size_t capacity(const audio_feed *f){ return f->bits == 24 ? AUDIO_FEED_FRAMES / 2 : AUDIO_FEED_FRAMES; }
int audio_feed_begin_pcm(audio_feed *f, uint64_t g, unsigned rate, unsigned bits){
    if(!f || !g || (bits != 16 && bits != 24) || (rate != 44100 && rate != 48000) || g <= f->timeline.generation) return AF_ARGUMENT;
    /* Preserve offer serials across reset; generation remains the primary key. */
    uint64_t ticket = f->ticket;
    memset(f, 0, sizeof *f); f->ticket = ticket; f->bits = bits;
    return result(audio_timeline_begin(&f->timeline, g, rate));
}
int audio_feed_begin(audio_feed *f, uint64_t g, unsigned rate){ return audio_feed_begin_pcm(f, g, rate, 16); }
size_t audio_feed_space(const audio_feed *f){
    return f && f->timeline.generation && !f->offer ? capacity(f) - f->used : 0;
}
static int push_pcm(audio_feed *f, uint64_t g, uint64_t item,
                    uint64_t occurrence, uint64_t offset, const void *pcm, size_t frames, unsigned bits){
    int rc = current(f, g); if(rc) return rc;
    size_t cap = capacity(f), stride = bits == 24 ? 8 : 4;
    if(f->bits != bits || !item || !occurrence || !pcm || !frames || frames > cap ||
       offset > UINT64_MAX - frames ||
       f->timeline.submitted > UINT64_MAX - f->used ||
       frames > UINT64_MAX - f->timeline.submitted - f->used) return AF_ARGUMENT;
    if(bits == 24) for(size_t i = 0; i < frames * 2; i++)
        if(((const int32_t *)pcm)[i] < -8388608 || ((const int32_t *)pcm)[i] > 8388607) return AF_ARGUMENT;
    if(f->offer) return AF_BUSY;
    if(f->used > cap - frames) return AF_FULL;
    if(item == f->last_item){
        if(f->ended || occurrence != f->last_occurrence || offset != f->next_offset) return AF_ORDER;
    } else {
        if(f->last_item && !f->ended) return AF_ORDER;
        for(unsigned i = 0; i < f->timeline.n; i++)
            if(f->timeline.span[(f->timeline.head + i) % AUDIO_TIMELINE_ITEMS].item == item) return AF_ORDER;
        for(size_t i = 0; i < f->n; i++) if(f->span[i].item == item) return AF_ORDER;
    }
    int merge = f->n && f->span[f->n - 1].item == item;
    if(!merge && f->n == AUDIO_TIMELINE_ITEMS) return AF_FULL;
    size_t tail = (f->head + f->used) % cap;
    size_t first = frames < cap - tail ? frames : cap - tail;
    memcpy((uint8_t *)f->pcm + tail * stride, pcm, first * stride);
    memcpy(f->pcm, (const uint8_t *)pcm + first * stride, (frames - first) * stride);
    if(!merge) f->span[f->n++] = (af_span){item, occurrence, offset, 0, 0};
    f->span[f->n - 1].frames += frames; f->used += frames;
    f->last_item = item; f->last_occurrence = occurrence; f->next_offset = offset + frames; f->ended = 0;
    return AF_OK;
}
int audio_feed_push(audio_feed *f, uint64_t g, uint64_t item, uint64_t occ, uint64_t off, const int16_t *p, size_t n){ return push_pcm(f, g, item, occ, off, p, n, 16); }
int audio_feed_push24(audio_feed *f, uint64_t g, uint64_t item, uint64_t occ, uint64_t off, const int32_t *p, size_t n){ return push_pcm(f, g, item, occ, off, p, n, 24); }
int audio_feed_end(audio_feed *f, uint64_t g, uint64_t item){
    int rc = current(f, g); if(rc) return rc;
    if(!item || item != f->last_item) return AF_ORDER;
    if(f->offer) return AF_BUSY;
    if(f->ended) return AF_OK;
    if(f->n) f->span[f->n - 1].ended = 1;
    else { rc = result(audio_timeline_end(&f->timeline, g, item)); if(rc) return rc; }
    f->ended = 1; return AF_OK;
}
/* Prepare a copy before any external acceptance: no queue or ledger mutation. */
static int reserve(const audio_feed *f, size_t frames, audio_timeline *trial){
    *trial = f->timeline;
    for(size_t i = 0; frames && i < f->n; i++){
        const af_span *s = &f->span[i]; size_t n = s->frames < frames ? s->frames : frames;
        int rc = audio_timeline_append(trial, trial->generation, s->item, s->occurrence, s->offset, n);
        if(rc) return result(rc);
        if(n == s->frames && s->ended){
            rc = audio_timeline_end(trial, trial->generation, s->item); if(rc) return result(rc);
        }
        frames -= n;
    }
    return frames ? AF_ARGUMENT : AF_OK;
}
int audio_feed_peek(audio_feed *f, uint64_t g, size_t limit, af_packet *out){
    int rc = current(f, g); if(rc) return rc;
    if(!out || !limit || limit > (f->bits == 24 ? AUDIO_FEED_PACKET / 2 : AUDIO_FEED_PACKET)) return AF_ARGUMENT;
    if(!f->offer){
        if(!f->used) return AF_EMPTY;
        if(f->ticket == UINT64_MAX) return AF_ARGUMENT; /* never reuse an offer identity */
        size_t n = f->used < limit ? f->used : limit;
        audio_timeline trial; rc = reserve(f, n, &trial); if(rc) return rc;
        size_t cap = capacity(f), stride = f->bits == 24 ? 8 : 4;
        size_t first = n < cap - f->head ? n : cap - f->head;
        memcpy(f->offered, (uint8_t *)f->pcm + f->head * stride, first * stride);
        memcpy((uint8_t *)f->offered + first * stride, f->pcm, (n - first) * stride);
        f->offer = n; f->ticket++;
    }
    *out = (af_packet){f->offered, f->offer, g, f->ticket, f->bits == 24 ? f->offered24 : NULL, f->bits}; return AF_OK;
}
int audio_feed_accept(audio_feed *f, const af_packet *p, size_t frames){
    if(!p) return AF_ARGUMENT;
    int rc = current(f, p->generation); if(rc) return rc;
    if(!f->offer || p->ticket != f->ticket || p->frames != f->offer || p->pcm != f->offered || p->bits != f->bits || (f->bits == 24 && p->pcm24 != f->offered24)) return AF_ORDER;
    if(frames > f->offer) return AF_ARGUMENT;
    if(!frames) return AF_OK;
    audio_timeline trial; rc = reserve(f, frames, &trial); if(rc) return rc;
    f->timeline = trial; f->head = (f->head + frames) % capacity(f); f->used -= frames;
    for(size_t left = frames; left;){
        af_span *s = &f->span[0]; size_t n = s->frames < left ? s->frames : left;
        s->offset += n; s->frames -= n; left -= n;
        if(!s->frames){ f->n--; memmove(f->span, f->span + 1, f->n * sizeof *s); memset(&f->span[f->n], 0, sizeof *s); }
    }
    f->offer = 0; return AF_OK;
}
int audio_feed_observe(audio_feed *f, uint64_t g, uint64_t frame, atl_view *out){
    int rc = current(f, g); if(rc) return rc;
    if(!out) return AF_ARGUMENT;
    rc = result(audio_timeline_observe(&f->timeline, g, frame));
    return rc ? rc : result(audio_timeline_get(&f->timeline, g, out));
}
int audio_feed_view(const audio_feed *f, uint64_t g, atl_view *out){
    int rc = current(f, g); return rc ? rc : result(audio_timeline_get(&f->timeline, g, out));
}
