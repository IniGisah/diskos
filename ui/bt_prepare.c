/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bt_prepare.h"
#ifdef DISKOS_TEST_BT_PREPARE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int bp_active(const bp_prepare *p){ return p->phase >= BP_PAUSE && p->phase <= BP_RELEASE; }
static int same(const track_state_t *a, const track_state_t *b){
    return a->have_track == b->have_track && a->playback_token == b->playback_token &&
        !strcmp(a->path, b->path) && a->song_id_valid == b->song_id_valid &&
        a->song_id_raw == b->song_id_raw && a->pos_id_valid == b->pos_id_valid && a->pos_id_raw == b->pos_id_raw;
}
static int loaded(const bp_prepare *p, const bp_input *i){
    return i->qualified && i->report.player_generation == p->generation &&
        (!p->saved.have_track || (i->report.have_track && i->report.player_confirmed && !i->report.provisional)) &&
        same(&i->report, &p->saved) && (!p->saved.have_track || i->report.state == 1);
}
int bp_begin(bp_prepare *p, uint32_t now, const char *mac, int choice, int playing, const bp_input *i){
    char frame[48];
    if(bp_active(p) || !i || !i->permitted || !i->qualified || choice < 1 || choice > 4 ||
       p->attempt == UINT64_MAX || !i->current.player_generation ||
       (i->current.have_track && (!i->current.player_confirmed || i->current.provisional)) ||
       (playing && !i->current.have_track) || bt_prepare_codec_frame(mac, choice, frame, sizeof frame) != 0)
        return -1;
    uint64_t attempt = p->attempt + 1;
    memset(p, 0, sizeof *p); p->attempt = attempt;
    p->phase = BP_PAUSE; p->choice = choice; p->was_playing = playing != 0;
    p->saved = i->current; p->generation = i->current.player_generation;
    p->transport = i->transport; p->report = i->report_serial; p->poweroff = i->poweroff;
    p->deadline = now + 12000; p->query_at = now;
    snprintf(p->mac, sizeof p->mac, "%s", mac);
    for(int n = 0; n < 17; n++) if(p->mac[n] >= 'a' && p->mac[n] <= 'f') p->mac[n] -= 32;
    return 0;
}
static void finish(bp_prepare *p, const bp_sink *k, int phase){
    if(p->probe) k->probe_cancel(k->ctx, p->probe);
    p->probe = 0; p->phase = phase;
}
void bp_cancel(bp_prepare *p, const bp_sink *k){ if(bp_active(p)) finish(p, k, BP_CANCELLED); }
static int send(bp_prepare *p, const bp_sink *k, const bp_input *i, const char *f, int release){
    int rc = k->send(k->ctx, f, release, i->report_serial, i->playing_frames);
    if(rc < 0) finish(p, k, BP_FAILED);
    return rc;
}
static void query(bp_prepare *p, uint32_t now, const bp_input *i, const bp_sink *k){
    if((int32_t)(now - p->query_at) < 0) return;
    if(send(p, k, i, "02020008", 0) == 0){ p->queried = 1; p->query_at = now + 250; }
}
static int probe(bp_prepare *p, const bp_sink *k, bc_snapshot *s, int *known, unsigned *avail){
    if(!p->probe){
        int rc = k->probe_start(k->ctx, &p->probe);
        if(rc < 0) finish(p, k, BP_FAILED);
        return 0;
    }
    int rc = k->probe_read(k->ctx, p->probe, s, known, avail);
    if(rc){ k->probe_cancel(k->ctx, p->probe); p->probe = 0; }
    return rc;
}
void bp_tick(bp_prepare *p, uint32_t now, const bp_input *i, const bp_sink *k){
    if(!bp_active(p)) return;
    if(!i || !i->permitted || i->current.player_generation != p->generation ||
       i->transport != p->transport || i->poweroff != p->poweroff || !same(&i->current, &p->saved) ||
       (p->held && i->playing_frames != p->playing_frames)){
        finish(p, k, BP_CANCELLED); return;
    }
    if((int32_t)(now - p->deadline) >= 0){ finish(p, k, BP_FAILED); return; }
    switch(p->phase){
    case BP_PAUSE:
        if(!p->saved.have_track){
            p->held = 1; p->playing_frames = i->playing_frames; p->phase = BP_ROUTE; break;
        }
        if(p->was_playing && !p->route_step){
            if(send(p, k, i, "0201000C0000", 0) != 0) break;
            p->route_step = 1; p->report = i->report_serial; break;
        }
        if(i->report_serial != p->report && loaded(p, i)){
            p->held = 1; p->playing_frames = i->playing_frames;
            p->position_seq = i->current.pos_seq; p->phase = BP_POSITION;
        } else query(p, now, i, k);
        break;
    case BP_POSITION:
        if(!loaded(p, i)) break;
        if(i->current.pos_seq != p->position_seq && i->current.pos_seq &&
           (int32_t)(i->current.pos_seq - i->current.path_seq) > 0 &&
           i->current.position_ms >= 0 && i->current.position_ms <= INT32_MAX){
            p->position = i->current.position_ms; p->phase = BP_ROUTE; p->route_step = 0;
        }
        break;
    case BP_ROUTE: {
        static const char *const frames[] = {"0666000C0006", "0642000C0000", "0657000C0008",
                                             "06c1000C0000", "0666000C0002", "0657000C0008"};
        if(send(p, k, i, frames[p->route_step], 0) != 0) break;
        if(p->route_step == 4) k->routed(k->ctx, p->mac);
        if(++p->route_step == 6) p->phase = BP_CAPS;
        break;
    }
    case BP_CAPS: {
        bc_snapshot s; int known; unsigned avail;
        int rc = probe(p, k, &s, &known, &avail);
        if(rc > 0 && known && s.adapter == 0 && !strcmp(s.mac, p->mac)){
            p->endpoint_sequence = s.sequence;
            p->selected = bt_codec_pick(p->choice, avail, known);
            if(p->selected != p->choice){ finish(p, k, BP_FAILED); break; }
            p->phase = BP_CODEC;
        }
        break;
    }
    case BP_CODEC: {
        char f[48];
        if(bt_prepare_codec_frame(p->mac, p->selected, f, sizeof f) != 0){ finish(p, k, BP_FAILED); break; }
        if(send(p, k, i, f, 0) == 0){
            p->phase = BP_VERIFY; p->report = i->report_serial; p->queried = 0; p->query_at = now;
        }
        break;
    }
    case BP_VERIFY: {
        query(p, now, i, k);
        if(!bp_active(p)) break;
        bc_snapshot s; int known; unsigned avail;
        int rc = probe(p, k, &s, &known, &avail);
        if(rc > 0 && s.sequence != p->endpoint_sequence){ finish(p, k, BP_CANCELLED); break; }
        unsigned family = p->selected == 0 ? BC_CODEC_SBC : p->selected == 1 ? BC_CODEC_AAC : BC_CODEC_LDAC;
        if(rc > 0 && s.codec == family && s.adapter == 0 && !strcmp(s.mac, p->mac) &&
           p->queried && i->report_serial != p->report && loaded(p, i))
            p->phase = BP_VOLUME;
        break;
    }
    case BP_VOLUME: {
        if(i->current.volume_seq && i->current.volume >= 0 && i->current.volume <= VOL_MAX){
            char f[16]; snprintf(f, sizeof f, "0715000C%04X", (unsigned)i->current.volume);
            if(send(p, k, i, f, 0) != 0) break;
        }
        p->phase = p->saved.have_track ? BP_SEEK : BP_DONE; break;
    }
    case BP_SEEK: {
        if(!loaded(p, i)) break;
        char f[32]; snprintf(f, sizeof f, "01030010%08lX", (unsigned long)p->position);
        if(send(p, k, i, f, 0) == 0){ p->position_seq = i->current.pos_seq; p->phase = BP_SEEKED; }
        break;
    }
    case BP_SEEKED:
        if(loaded(p, i) && i->current.pos_seq != p->position_seq && i->current.position_ms >= 0 &&
           i->current.position_ms <= INT32_MAX &&
           labs(i->current.position_ms - p->position) <= 2000)
            p->phase = p->was_playing ? BP_RELEASE : BP_DONE;
        break;
    case BP_RELEASE:
        if(loaded(p, i) && send(p, k, i, "0201000C0000", 1) == 0) p->phase = BP_DONE;
        break;
    }
}
#endif
