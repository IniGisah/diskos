/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_timeline.h"
#include <stddef.h>
#include <string.h>
static int current(const audio_timeline *t, uint64_t generation){
    if(!t || !t->generation || !generation) return ATL_ARGUMENT;
    return t->generation == generation ? ATL_OK : ATL_STALE;
}
static atl_span *last(audio_timeline *t){ return &t->span[(t->head + t->n - 1) % AUDIO_TIMELINE_ITEMS]; }
int audio_timeline_begin(audio_timeline *t, uint64_t generation, unsigned rate){
    if(!t || !generation || rate < 8000 || rate > 384000) return ATL_ARGUMENT;
    memset(t, 0, sizeof *t); t->generation = generation; t->rate = rate;
    return ATL_OK;
}
int audio_timeline_append(audio_timeline *t, uint64_t generation, uint64_t item,
                          uint64_t occurrence, uint64_t offset, uint64_t frames){
    int rc = current(t, generation);
    if(rc) return rc;
    if(!item || !occurrence || !frames || offset > UINT64_MAX - frames || t->submitted > UINT64_MAX - frames)
        return ATL_ARGUMENT;
    if(t->n){
        atl_span *p = last(t);
        if(p->item == item){
            if(p->ended || p->occurrence != occurrence || offset != p->offset + p->count) return ATL_ORDER;
            p->count += frames; t->submitted += frames; return ATL_OK;
        }
        if(!p->ended) return ATL_ORDER;
        for(unsigned i = 0; i < t->n; i++) if(t->span[(t->head + i) % AUDIO_TIMELINE_ITEMS].item == item) return ATL_ORDER;
    }
    if(t->n == AUDIO_TIMELINE_ITEMS) return ATL_FULL;
    atl_span *p = &t->span[(t->head + t->n) % AUDIO_TIMELINE_ITEMS];
    memset(p, 0, sizeof *p); p->item = item; p->occurrence = occurrence;
    p->start = t->submitted; p->count = frames; p->offset = offset;
    t->n++; t->submitted += frames; return ATL_OK;
}
int audio_timeline_end(audio_timeline *t, uint64_t generation, uint64_t item){
    int rc = current(t, generation);
    if(rc) return rc;
    if(!item || !t->n) return ATL_ARGUMENT;
    atl_span *p = last(t);
    if(p->item != item) return ATL_ORDER;
    p->ended = 1; return ATL_OK;
}
int audio_timeline_observe(audio_timeline *t, uint64_t generation, uint64_t frame){
    int rc = current(t, generation);
    if(rc) return rc;
    if(!t->n || frame > t->submitted) return ATL_ARGUMENT;
    if(t->have_playhead && frame < t->audible) return ATL_ORDER;
    while(t->n > 1){
        unsigned next = (t->head + 1) % AUDIO_TIMELINE_ITEMS;
        if(frame < t->span[next].start) break;
        memset(&t->span[t->head], 0, sizeof t->span[t->head]); t->head = next; t->n--;
    }
    t->audible = frame; t->have_playhead = 1; return ATL_OK;
}
static uint64_t milliseconds(uint64_t frames, unsigned rate){
    uint64_t whole = frames / rate, part = (frames % rate) * 1000 / rate;
    if(whole > (UINT64_MAX - part) / 1000) return UINT64_MAX;
    return whole * 1000 + part;
}
int audio_timeline_get(const audio_timeline *t, uint64_t generation, atl_view *out){
    int rc = current(t, generation);
    if(rc) return rc;
    if(!out) return ATL_ARGUMENT;
    atl_view v = {0};
    if(t->n && t->have_playhead){
        const atl_span *p = &t->span[t->head];
        uint64_t used = t->audible - p->start;
        v.have_item = 1; v.item = p->item; v.occurrence = p->occurrence;
        v.input_ended = p->ended; v.position_frames = p->offset + used;
        v.position_ms = milliseconds(v.position_frames, t->rate);
        v.drained = t->n == 1 && p->ended && t->audible == t->submitted;
    }
    *out = v; return ATL_OK;
}
