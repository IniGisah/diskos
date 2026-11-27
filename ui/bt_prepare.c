/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bt_prepare.h"
#include "bt_tx_logic.h"
#include "screens.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int bp_active(const bp_prepare *p) {
    return p && p->phase >= BP_PAUSE && p->phase <= BP_RELEASE;
}

static int same(const track_state_t *a, const track_state_t *b) {
    return a->have_track == b->have_track && a->playback_token == b->playback_token &&
        !strcmp(a->path, b->path) && a->song_id_valid == b->song_id_valid &&
        a->song_id_raw == b->song_id_raw && a->pos_id_valid == b->pos_id_valid && a->pos_id_raw == b->pos_id_raw;
}

static int loaded(const bp_prepare *p, const bp_input *i) {
    return i->qualified && i->report.player_generation == p->generation &&
        (!p->saved.have_track || (i->report.have_track && i->report.player_confirmed && !i->report.provisional)) &&
        same(&i->report, &p->saved) && (!p->saved.have_track || i->report.state == 1);
}

static unsigned codec_family_for_choice(int selected) {
    if (selected == 0) return BC_CODEC_SBC;
    if (selected == 1) return BC_CODEC_AAC;
    if (selected >= 2 && selected <= 4) return BC_CODEC_LDAC;
    if (selected == 5) return BC_CODEC_APTX;
    if (selected == 6) return BC_CODEC_APTX_HD;
    return BC_CODEC_SBC;
}

static int choice_for_codec_family(unsigned family, int orig_choice) {
    switch (family) {
    case BC_CODEC_AAC:     return 1;
    case BC_CODEC_LDAC:    return (orig_choice >= 2 && orig_choice <= 4) ? orig_choice : 3;
    case BC_CODEC_APTX:    return 5;
    case BC_CODEC_APTX_HD: return 6;
    default:               return 0;
    }
}

int bp_begin_ex(bp_prepare *p, uint32_t now, const char *mac, int choice, int playing, int reconfig_only, const bp_input *i) {
    char frame[48];
    if (!p || bp_active(p) || !i || !i->permitted || !i->qualified || choice < 0 || choice >= BT_CODEC_N ||
        p->attempt == UINT64_MAX || i->report.player_generation != i->current.player_generation ||
        (i->current.have_track && (!i->current.player_confirmed || i->current.provisional)) ||
        (playing && !i->current.have_track) || bt_prepare_codec_frame(mac, choice, frame, sizeof frame) != 0) {
        return -1;
    }
    uint64_t attempt = p->attempt + 1;
    memset(p, 0, sizeof *p);
    p->attempt = attempt;
    p->phase = BP_PAUSE;
    p->choice = choice;
    p->was_playing = playing != 0;
    p->reconfig_only = reconfig_only != 0;
    p->saved = i->current;
    p->generation = i->current.player_generation;
    p->transport = i->transport;
    p->report = i->report_serial;
    p->poweroff = i->poweroff;
    p->deadline = now + 12000;
    p->query_at = now;
    snprintf(p->mac, sizeof p->mac, "%s", mac);
    for (int n = 0; n < 17; n++) {
        if (p->mac[n] >= 'a' && p->mac[n] <= 'f') p->mac[n] -= 32;
    }
    return 0;
}

int bp_begin(bp_prepare *p, uint32_t now, const char *mac, int choice, int playing, const bp_input *i) {
    return bp_begin_ex(p, now, mac, choice, playing, 0, i);
}

static void finish(bp_prepare *p, const bp_sink *k, int phase) {
    if (k) {
        if (p->probe && k->probe_cancel) k->probe_cancel(k->ctx, p->probe);
        if (p->codec_job && k->codec_cancel) k->codec_cancel(k->ctx, p->codec_job);
    }
    p->probe = 0;
    p->codec_job = 0;
    p->phase = phase;
}

void bp_cancel(bp_prepare *p, const bp_sink *k) {
    if (bp_active(p)) finish(p, k, BP_CANCELLED);
}

static int send(bp_prepare *p, const bp_sink *k, const bp_input *i, const char *f, int release) {
    int rc = k->send(k->ctx, f, release, i->report_serial, i->playing_frames);
    if (rc < 0) finish(p, k, BP_FAILED);
    return rc;
}

static void query(bp_prepare *p, uint32_t now, const bp_input *i, const bp_sink *k) {
    if ((int32_t)(now - p->query_at) < 0) return;
    if (send(p, k, i, "02020008", 0) == 0) {
        p->queried = 1;
        p->query_at = now + 250;
    }
}

static int probe(bp_prepare *p, const bp_sink *k, bc_snapshot *s, int *known, unsigned *avail) {
    if (!p->probe) {
        int rc = k->probe_start(k->ctx, &p->probe);
        if (rc < 0) finish(p, k, BP_FAILED);
        return 0;
    }
    int rc = k->probe_read(k->ctx, p->probe, s, known, avail);
    if (rc) {
        k->probe_cancel(k->ctx, p->probe);
        p->probe = 0;
    }
    return rc;
}

void bp_tick(bp_prepare *p, uint32_t now, const bp_input *i, const bp_sink *k) {
    if (!bp_active(p) || !k) return;
    if (!i || !i->permitted || i->current.player_generation != p->generation ||
        i->transport != p->transport || i->poweroff != p->poweroff || !same(&i->current, &p->saved) ||
        (p->held && i->playing_frames != p->playing_frames)) {
        finish(p, k, BP_CANCELLED);
        return;
    }
    if ((int32_t)(now - p->deadline) >= 0) {
        finish(p, k, BP_FAILED);
        return;
    }
    switch (p->phase) {
    case BP_PAUSE:
        if (!p->saved.have_track) {
            p->held = 1;
            p->playing_frames = i->playing_frames;
            p->phase = p->reconfig_only ? BP_CAPS : BP_ROUTE;
            break;
        }
        if (p->was_playing && !p->route_step) {
            if (send(p, k, i, "0201000C0000", 0) != 0) break;
            p->route_step = 1;
            p->report = i->report_serial;
            break;
        }
        if (loaded(p, i) && (!p->was_playing || i->report_serial != p->report)) {
            p->held = 1;
            p->playing_frames = i->playing_frames;
            p->position_seq = i->current.pos_seq;
            p->position_wait_at = now;
            p->phase = BP_POSITION;
        } else {
            query(p, now, i, k);
        }
        break;

    case BP_POSITION:
        if (!loaded(p, i)) break;
        if (i->current.pos_seq != p->position_seq && i->current.pos_seq &&
            (int32_t)(i->current.pos_seq - i->current.path_seq) > 0 &&
            i->current.position_ms >= 0 && i->current.position_ms <= INT32_MAX) {
            p->position = i->current.position_ms;
            p->phase = p->reconfig_only ? BP_CAPS : BP_ROUTE;
            p->route_step = 0;
        } else if (((!p->was_playing && (int32_t)(now - p->position_wait_at) >= 500) ||
                    (p->was_playing && (int32_t)(now - p->position_wait_at) >= 1200)) &&
                   i->current.pos_seq && (int32_t)(i->current.pos_seq - i->current.path_seq) > 0 &&
                   i->current.position_ms >= 0 && i->current.position_ms <= INT32_MAX) {
            /* mq_player stops emitting a103 when paused; settle from existing paused position. */
            p->position = i->current.position_ms;
            p->phase = p->reconfig_only ? BP_CAPS : BP_ROUTE;
            p->route_step = 0;
        } else if (((!p->was_playing && (int32_t)(now - p->position_wait_at) >= 1000) ||
                    (p->was_playing && (int32_t)(now - p->position_wait_at) >= 1500)) &&
                   i->current.position_ms == 0) {
            p->position = 0;
            p->phase = p->reconfig_only ? BP_CAPS : BP_ROUTE;
            p->route_step = 0;
        } else {
            query(p, now, i, k);
        }
        break;

    case BP_ROUTE: {
        static const char *const frames[] = {
            "0666000C0006", "0642000C0000", "0657000C0008",
            "06c1000C0000", "0666000C0002", "0657000C0008"
        };
        if (send(p, k, i, frames[p->route_step], 0) != 0) break;
        if (p->route_step == 4 && k->routed) k->routed(k->ctx, p->mac);
        if (++p->route_step == 6) p->phase = BP_CAPS;
        break;
    }

    case BP_CAPS: {
        bc_snapshot s;
        int known = 0;
        unsigned avail = 0;
        int rc = probe(p, k, &s, &known, &avail);
        if (rc > 0 && s.adapter == 0 && !strcmp(s.mac, p->mac)) {
            p->endpoint_sequence = s.sequence;
            p->observed_codec = s.codec;
            snprintf(p->pcm, sizeof p->pcm, "%s", s.pcm);
            p->selected = bt_codec_pick(p->choice, avail, known);
            if (p->selected < 0) p->selected = 0;
            if (p->selected != p->choice && k->toast_fallback) {
                k->toast_fallback(k->ctx, p->choice, p->selected);
            }
            unsigned target_family = codec_family_for_choice(p->selected);
            if (s.codec != target_family && k->codec_start && k->codec_read && p->pcm[0]) {
                p->phase = BP_APPLY_CODEC;
                p->apply_started = 0;
                p->apply_done = 0;
            } else {
                p->phase = BP_CODEC;
            }
        }
        break;
    }

    case BP_APPLY_CODEC: {
        if (!p->apply_started) {
            const char *cname = bt_tx_codec_cli_name(p->selected);
            int rc = k->codec_start(k->ctx, p->pcm, cname, &p->codec_job);
            if (rc == 0) {
                p->apply_started = 1;
            } else if (rc < 0) {
                /* Couldn't start CLI switch; fall back to observed codec and continue */
                p->selected = choice_for_codec_family(p->observed_codec, p->selected);
                p->phase = BP_CODEC;
            }
            break;
        }
        int rc = k->codec_read(k->ctx, p->codec_job);
        if (rc == 0) break;
        if (k->codec_cancel) k->codec_cancel(k->ctx, p->codec_job);
        p->codec_job = 0;
        if (rc < 0) {
            /* SEP switch rejected; fall back to observed codec so audio still plays */
            p->selected = choice_for_codec_family(p->observed_codec, p->selected);
        }
        p->phase = BP_CODEC;
        break;
    }

    case BP_CODEC: {
        char f[48];
        if (bt_prepare_codec_frame(p->mac, p->selected, f, sizeof f) != 0) {
            finish(p, k, BP_FAILED);
            break;
        }
        if (send(p, k, i, f, 0) == 0) {
            p->phase = BP_VERIFY;
            p->report = i->report_serial;
            p->queried = 0;
            p->query_at = now;
            p->verify_at = now;
        }
        break;
    }

    case BP_VERIFY: {
        query(p, now, i, k);
        if (!bp_active(p)) break;
        bc_snapshot s;
        int known = 0;
        unsigned avail = 0;
        int rc = probe(p, k, &s, &known, &avail);
        unsigned family = codec_family_for_choice(p->selected);
        if (rc > 0 && s.adapter == 0 && !strcmp(s.mac, p->mac)) {
            p->observed_codec = s.codec;
            int fresh_report = i->report_serial != p->report;
            /* mq_player is paused/stopped across BP_ROUTE+BP_CODEC and may stop answering
             * 02020008 even when was_playing was 1; settle once verify_at + 1500ms passes. */
            int quiet_verify = (int32_t)(now - p->verify_at) >= 1500;
            if (p->queried && (fresh_report || quiet_verify) && loaded(p, i)) {
                if (s.codec == family || (int32_t)(now - p->verify_at) >= 1500) {
                    p->phase = BP_VOLUME;
                }
            }
        }
        break;
    }

    case BP_VOLUME: {
        if (i->current.volume_seq && i->current.volume >= 0 && i->current.volume <= VOL_MAX) {
            char f[16];
            snprintf(f, sizeof f, "0715000C%04X", (unsigned)i->current.volume);
            if (send(p, k, i, f, 0) != 0) break;
        }
        p->phase = (p->saved.have_track && p->position > 0) ? BP_SEEK :
                   (p->was_playing ? BP_RELEASE : BP_DONE);
        break;
    }

    case BP_SEEK: {
        if (!loaded(p, i)) break;
        char f[32];
        snprintf(f, sizeof f, "01030010%08lX", (unsigned long)p->position);
        if (send(p, k, i, f, 0) == 0) {
            p->position_seq = i->current.pos_seq;
            p->seek_at = now;
            p->phase = BP_SEEKED;
        }
        break;
    }

    case BP_SEEKED:
        if (loaded(p, i) &&
            ((i->current.pos_seq != p->position_seq && i->current.position_ms >= 0 &&
              i->current.position_ms <= INT32_MAX &&
              labs(i->current.position_ms - p->position) <= 2000) ||
             (int32_t)(now - p->seek_at) >= 2500)) {
            p->phase = p->was_playing ? BP_RELEASE : BP_DONE;
        }
        break;

    case BP_RELEASE:
        if (loaded(p, i) && send(p, k, i, "0201000C0000", 1) == 0) {
            p->phase = BP_DONE;
        }
        break;
    }
}
