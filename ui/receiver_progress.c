/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "receiver_progress.h"
#include <limits.h>
#include <string.h>

void rp_init(rp_t *p){ memset(p,0,sizeof *p); p->playing=1; }

/* Item change (title/artist/album/id): the previous position and the duration
 * both belong to the old item. Capture: the new title first carries the OLD
 * duration (237000), corrected ~3 s later (212000). */
void rp_item(rp_t *p, uint64_t item, uint64_t now_ms){
    if(p->synced && item==p->item) return;
    p->synced=1; p->item=item; p->item_ms=now_ms;
    p->duration_ms=0; p->reports=0; p->pos_valid=0; p->frozen_ms=0;
}
void rp_duration(rp_t *p, uint64_t item, long duration_ms, int reports, uint64_t now_ms){
    (void)now_ms;
    if(!p->synced || item!=p->item || reports<1) return;
    /* Duplicate frames can repeat the previous item's duration. Only a
     * changed value qualifies early; otherwise use the time fallback. */
    if(!p->reports) p->reports=1;
    else if(duration_ms!=p->duration_ms) p->reports=2;
    p->duration_ms=duration_ms;
}
static long rp_now(const rp_t *p, uint64_t now_ms){
    if(!p->playing) return p->frozen_ms;
    uint64_t delta=now_ms>p->anchor_at ? now_ms-p->anchor_at : 0;
    if(delta>(uint64_t)(LONG_MAX-p->anchor_ms)) return LONG_MAX;
    return p->anchor_ms+(long)delta;
}
/* A sample belongs to the item it arrived under (a103 has no item id). */
void rp_position(rp_t *p, uint64_t item, long position_ms, uint64_t now_ms){
    if(!p->synced || item!=p->item || position_ms<0) return;
    /* The latest sample wins, even backwards: a skip restarts the count from ~0. */
    p->pos_valid=1; p->anchor_ms=position_ms; p->anchor_at=now_ms; p->fresh_at=now_ms;
    p->frozen_ms=position_ms;
}
void rp_playing(rp_t *p, int playing, uint64_t now_ms){
    playing=playing!=0;
    if(playing==p->playing) return;
    if(!playing && p->pos_valid && now_ms>p->fresh_at &&
       now_ms-p->fresh_at>RP_STALE_MS)
        p->pos_valid=0;
    if(p->pos_valid){
        if(!playing) p->frozen_ms=rp_now(p,now_ms);       /* pause freezes */
        else { p->anchor_ms=p->frozen_ms; p->anchor_at=now_ms; p->fresh_at=now_ms; }
    }
    p->playing=playing;
}
void rp_get(const rp_t *p, uint64_t now_ms, rp_out *out){
    memset(out,0,sizeof *out);
    if(!p->synced || !p->pos_valid || p->duration_ms<=0) return;       /* unknown: no bar */
    /* Trust a changed duration value, or the unverified 4 s fallback. */
    if(p->reports<2 && now_ms>=p->item_ms && now_ms-p->item_ms<RP_DURATION_MS) return;
    /* a103 is ~1 Hz and stops while paused: only a playing player can go stale. */
    if(p->playing && now_ms>p->fresh_at && now_ms-p->fresh_at>RP_STALE_MS) return;
    long e=rp_now(p,now_ms);
    if(e>p->duration_ms) e=p->duration_ms;
    if(e<0) e=0;
    out->show=1; out->elapsed_ms=e; out->duration_ms=p->duration_ms;
    out->fraction=(int)((int64_t)e*1000/p->duration_ms);
}
void rp_sync(rp_t *p, const ipc_receiver_meta *m, int playing, uint64_t now_ms){
    if(p->synced && (m->generation!=p->gen || m->session_revision!=p->session)) rp_init(p);
    p->gen=m->generation; p->session=m->session_revision;
    int fresh=!p->synced || m->item_revision!=p->item;
    if(!m->item_revision) return;                    /* no item announced yet */
    if(fresh){
        rp_item(p,m->item_revision,m->item_rx_ms);
        /* Sequence numbers keep counting across items: only reports made under
         * this item count, and at most the latest one is known. */
        if(m->dur_seq!=p->dur_seq && m->dur_rx_ms>=m->item_rx_ms)
            rp_duration(p,m->item_revision,m->duration_ms,1,m->dur_rx_ms);
    } else if(m->dur_seq!=p->dur_seq){
        unsigned d=m->dur_seq-p->dur_seq;
        rp_duration(p,m->item_revision,m->duration_ms,d>2 ? 2 : (int)d,m->dur_rx_ms);
    }
    p->dur_seq=m->dur_seq;
    if(m->pos_seq!=p->pos_seq){
        if(m->pos_item_revision==m->item_revision && m->progress_valid)
            rp_position(p,m->item_revision,m->position_ms,m->pos_rx_ms);
        p->pos_seq=m->pos_seq;
    }
    rp_playing(p,playing,now_ms);
}
