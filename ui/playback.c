/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include "playback.h"

static pb_snapshot g_eff;
static pb_view g_local;   /* last view built from the raw player state */
static pb_remote_state g_remote;
static pb_source g_source;
static pb_remote_sink g_sinks[PB_SRC_COUNT];

static void cp(char *d, size_t n, const char *s){
    if(!s) s = "";
    size_t used = strnlen(s, n - 1);
    memcpy(d, s, used); memset(d + used, 0, n - used);
}

static void compose(void){
    g_eff.source = g_source;
    g_eff.v = g_source != PB_SRC_LOCAL ? g_remote.v : g_local;
}

void playback_update(const track_state_t *raw, const char *title, int playing){
    memset(&g_local, 0, sizeof g_local);
    if(raw){
        g_local.have_track = raw->have_track;
        cp(g_local.title, sizeof g_local.title, title);
        cp(g_local.artist, sizeof g_local.artist, raw->artist);
        cp(g_local.album, sizeof g_local.album, raw->album);
        cp(g_local.path, sizeof g_local.path, raw->path);
        g_local.position_ms = raw->position_ms;
        g_local.duration_ms = raw->duration_ms;
        g_local.playing = playing;
        g_local.queue_pos = raw->pos_id;
    }
    compose();
}

static int remote_source(pb_source source){ return source == PB_SRC_CAST || source == PB_SRC_AIRPLAY; }
int playback_set_remote(pb_source source, const pb_remote_state *c){
    if(!remote_source(source)) return PB_E_SOURCE;
    if(!c || !c->active){
        if(g_source != source) return 0;
        memset(&g_remote, 0, sizeof g_remote); g_source = PB_SRC_LOCAL;
    }
    else {
        if(g_source != PB_SRC_LOCAL && g_source != source) return PB_E_BUSY;
        g_remote = *c; g_source = source;
        g_remote.v.title[sizeof g_remote.v.title - 1] = g_remote.v.artist[sizeof g_remote.v.artist - 1] = 0;
        g_remote.v.album[sizeof g_remote.v.album - 1] = g_remote.v.path[sizeof g_remote.v.path - 1] = g_remote.v.dest[sizeof g_remote.v.dest - 1] = 0;
    }
    compose();   /* release restores the latest raw-derived Local view */
    return 0;
}
void playback_set_cast(const pb_cast_state *c){ (void)playback_set_remote(PB_SRC_CAST, c); }
const pb_snapshot *playback_effective(void){ return &g_eff; }
pb_source playback_source(void){ return g_source; }
int playback_set_remote_sink(pb_source source, pb_remote_sink sink){
    if(!remote_source(source)) return PB_E_SOURCE;
    g_sinks[source] = sink; return 0;
}
void playback_set_cast_sink(pb_cast_sink f){ (void)playback_set_remote_sink(PB_SRC_CAST, f); }

void playback_display_state(track_state_t *out, const track_state_t *raw){
    *out = *raw;
    if(g_source == PB_SRC_LOCAL) return;   /* LOCAL: byte-for-byte what the consumers read today */
    const pb_view *v = &g_remote.v;
    out->have_track = v->have_track;
    cp(out->title, sizeof out->title, v->title); cp(out->artist, sizeof out->artist, v->artist);
    cp(out->album, sizeof out->album, v->album); cp(out->path, sizeof out->path, v->path);
    out->playing_num[0] = 0;
    if(v->queue_pos > 0 && v->queue_len > 0) snprintf(out->playing_num, sizeof out->playing_num, "%d/%d", v->queue_pos, v->queue_len);
    out->position_ms = v->position_ms; out->duration_ms = v->duration_ms;
    out->state = v->playing ? 2 : 1;
    out->pos_id = 0; out->sample_rate = 0; out->is_dsd = out->is_favorite = out->is_cue = out->is_sacd = 0;
    out->title_ready = 1; out->provisional = 0;
}

int playback_view_changed(void){
    static pb_source s_src = PB_SRC_LOCAL; static pb_view s_v;
    const pb_view *v = &g_remote.v;
    if(g_source == PB_SRC_LOCAL){
        int was = s_src != PB_SRC_LOCAL; s_src = PB_SRC_LOCAL;
        return was;
    }
    int same = s_src == g_source && s_v.have_track == v->have_track && s_v.playing == v->playing && s_v.queue_pos == v->queue_pos &&
               s_v.queue_len == v->queue_len && s_v.duration_ms == v->duration_ms && s_v.position_ms / 1000 == v->position_ms / 1000 &&
               !strcmp(s_v.path, v->path) && !strcmp(s_v.title, v->title) && !strcmp(s_v.artist, v->artist) && !strcmp(s_v.album, v->album) && !strcmp(s_v.dest, v->dest);
    s_src = g_source; s_v = *v;
    return !same;
}

static int to_sink(pb_cmd c, long arg){
    pb_remote_sink sink = g_sinks[g_source];
    if(!sink) return PB_E_NOSINK;
    return sink(c, arg) < 0 ? PB_E_BADCMD : 0;
}
int playback_command(const char *frame, int (*local)(const char *frame)){
    if(!frame) return PB_E_BADCMD;
    if(g_source == PB_SRC_LOCAL) return local ? local(frame) : PB_E_BADCMD;
    if(!strcmp(frame, "0201000C0000")) return to_sink(PB_CMD_TOGGLE, 0);
    if(!strcmp(frame, "0201000C0001")) return to_sink(PB_CMD_NEXT, 0);
    if(!strcmp(frame, "0201000C0002")) return to_sink(PB_CMD_PREV, 0);
    return PB_E_BADCMD;
}
int playback_seek(long ms, int (*local)(long ms)){
    if(g_source == PB_SRC_LOCAL) return local ? local(ms) : PB_E_BADCMD;
    return to_sink(PB_CMD_SEEK, ms < 0 ? 0 : ms);
}
int playback_volume(int level, int (*local)(int level)){
    if(g_source == PB_SRC_LOCAL) return local ? local(level) : PB_E_BADCMD;
    return to_sink(PB_CMD_VOLUME, level);
}
