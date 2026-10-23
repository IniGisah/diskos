/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cast_queue.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static _Atomic uint32_t g_generation;
void cast_queue_free(cast_queue *q){
    if(!q) return;
    free(q->base); free(q->shuf); free(q->sid); free(q->flags);
    memset(q, 0, sizeof *q);
}
static void stamp(cast_queue *q){
    q->generation = atomic_fetch_add(&g_generation, 1) + 1;
    if(!q->generation) q->generation = atomic_fetch_add(&g_generation, 1) + 1;   /* 0 is "none" after a 2^32 wrap */
}
int cast_queue_snapshot(cast_queue *q, int play_mode, int cur_pos_id, const char *cur_path){
    if(!q) return CQ_DB_ERROR;
    memset(q, 0, sizeof *q);
    if(play_mode < 0 || play_mode > 4) return CQ_REBUILDING;      /* mode unknown: do not guess an order */
    mdb_qsnap_t s;
    int r = mdb_queue_snapshot(play_mode == 1, cur_pos_id, cur_path, CQ_MAX_OCCURRENCES, &s);
    if(r <= 0){
        switch(r){
        case MDB_UPNEXT_EMPTY:      return CQ_EMPTY;
        case MDB_UPNEXT_NOTFOUND:
        case MDB_UPNEXT_BUSY:       return CQ_REBUILDING;         /* shuffle list not (yet) a permutation, or a writer holds the DB */
        case MDB_QSNAP_AMBIGUOUS:   return CQ_AMBIGUOUS;
        case MDB_QSNAP_TOO_LARGE:   return CQ_TOO_LARGE;
        default:                    return CQ_DB_ERROR;
        }
    }
    q->n = s.n; q->cur = s.cur; q->base = s.base; q->shuf = s.shuf; q->flags = s.flags;
    q->play_mode = play_mode;
    q->cur_play = s.cur;
    if(s.shuf){
        int id = s.base[s.cur];
        for(int i = 0; i < s.n; i++) if(s.shuf[i] == id){ q->cur_play = i; break; }
    }
    q->src = CQ_SRC_LIVE;
    stamp(q);
    return CQ_OK;
}
static int sel_rows(int pl, const int *ids, int n, mdb_srow_t *rows, unsigned char *got){   /* the reader for the kind: SONG or CUSTOM_PLAYLIST */
    return pl ? mdb_playlist_rows_get(ids, n, rows, got) : mdb_song_rows(ids, n, rows, got);
}
int cast_queue_from_ids(cast_queue *q, const int *ids, int n, int start_index, int play_mode){
    return cast_queue_from_rows(q, ids, n, start_index, play_mode, 0);
}
int cast_queue_from_rows(cast_queue *q, const int *ids, int n, int start_index, int play_mode, int kind){
    if(!q) return CQ_DB_ERROR;
    memset(q, 0, sizeof *q);
    if(!ids || (kind != 0 && kind != 1)) return CQ_DB_ERROR;
    if(n <= 0) return CQ_EMPTY;
    if(n > CQ_MAX_OCCURRENCES) return CQ_TOO_LARGE;               /* never truncate a selection */
    if(play_mode < 0 || play_mode > 4) return CQ_REBUILDING;      /* mode unknown: do not guess an order */
    if(start_index < 0 || start_index >= n) return CQ_AMBIGUOUS;  /* the occurrence to play does not exist */
    int *base = malloc((size_t)n * sizeof *base), *sid = malloc((size_t)n * sizeof *sid);
    unsigned char *flags = malloc((size_t)n);
    if(!base || !sid || !flags){ free(base); free(sid); free(flags); return CQ_DB_ERROR; }
    memcpy(sid, ids, (size_t)n * sizeof *sid);
    for(int i = 0; i < n; i++) base[i] = i + 1;                   /* synthetic ascending occurrence ids */
    int r = sel_rows(kind, sid, n, NULL, flags);                   /* one transaction: every row exists, and its CUE/ISO flags */
    if(r != n){
        free(base); free(sid); free(flags);
        return (r == MDB_UPNEXT_NOTFOUND || r == MDB_UPNEXT_BUSY) ? CQ_REBUILDING : CQ_DB_ERROR;
    }
    if(kind) for(int i = 0; i < n; i++) flags[i] |= CQ_F_PLAYLIST;  /* the kind of every occurrence */
    q->src = CQ_SRC_SELECTION;
    q->n = n; q->cur = q->cur_play = start_index; q->play_mode = play_mode;
    q->base = base; q->sid = sid; q->flags = flags;
    stamp(q);
    return CQ_OK;
}
static int page_selection(const cast_queue *q, int k, int n, mdb_qrow_t *out, unsigned char *flags){
    mdb_srow_t *rows = malloc((size_t)n * sizeof *rows);
    if(!rows) return -CQ_DB_ERROR;
    unsigned char got[CQ_PAGE_MAX];
    int pl = q->flags[k] & CQ_F_PLAYLIST, r = 0;
    for(int i = 1; i < n; i++) if((q->flags[k + i] & CQ_F_PLAYLIST) != pl) r = -1;   /* a snapshot has one kind; mixed cannot be read by one reader */
    if(r){ free(rows); return -CQ_REBUILDING; }
    r = sel_rows(pl, q->sid + k, n, rows, got);
    if(pl) for(int i = 0; i < n; i++) got[i] |= CQ_F_PLAYLIST;
    if(r == MDB_UPNEXT_NOTFOUND || r == MDB_UPNEXT_BUSY){ free(rows); return -CQ_REBUILDING; }
    if(r != n){ free(rows); return -CQ_DB_ERROR; }
    for(int i = 0; i < n; i++)
        if(got[i] != q->flags[k + i]){ free(rows); return -CQ_REBUILDING; }   /* same id, different kind: a scan ran meanwhile */
    for(int i = 0; i < n; i++){
        mdb_qrow_t *w = &out[i]; memset(w, 0, sizeof *w);
        w->base_id = q->base[k + i]; w->ord = k + i + 1; w->dur_ms = rows[i].dur_ms;
        snprintf(w->title, sizeof w->title, "%s", rows[i].title);
        snprintf(w->artist, sizeof w->artist, "%s", rows[i].artist);
        snprintf(w->path, sizeof w->path, "%s", rows[i].path);
        if(flags) flags[i] = got[i];
    }
    free(rows);
    return n;
}
int cast_queue_page(const cast_queue *q, int k, int n, mdb_qrow_t *out, unsigned char *flags){
    if(!q || !q->n || !out || n <= 0 || k < 0 || k >= q->n) return 0;
    if(n > CQ_PAGE_MAX) n = CQ_PAGE_MAX;
    if(n > q->n - k) n = q->n - k;
    if(q->src == CQ_SRC_SELECTION) return page_selection(q, k, n, out, flags);
    int ids[CQ_PAGE_MAX], idx[CQ_PAGE_MAX]; unsigned char got[CQ_PAGE_MAX];
    for(int i = 0; i < n; i++){
        ids[i] = q->shuf ? q->shuf[k + i] : q->base[k + i];
        int *b = bsearch(&ids[i], q->base, (size_t)q->n, sizeof(int), mdb_id_cmp);
        if(!b) return CQ_REBUILDING;                              /* cannot happen for a valid snapshot */
        idx[i] = (int)(b - q->base);
    }
    int r = mdb_queue_rows(ids, n, out, got);
    if(r == MDB_UPNEXT_NOTFOUND || r == MDB_UPNEXT_BUSY) return -CQ_REBUILDING;
    if(r != n) return -CQ_DB_ERROR;
    for(int i = 0; i < n; i++){
        if(got[i] != q->flags[idx[i]]) return -CQ_REBUILDING;     /* same id, different kind: rebuilt */
        out[i].ord = idx[i] + 1;
        if(flags) flags[i] = got[i];
    }
    return n;
}

/* ---- Cursor (slice 11) ---- */
#include "cast_session.h"
static uint32_t rnd(cast_cursor *c, uint32_t m){   /* xorshift64*; the modulo bias is below 1e-4 for m <= CQ_MAX_OCCURRENCES */
    uint64_t x = c->rng; x ^= x >> 12; x ^= x << 25; x ^= x >> 27; c->rng = x;
    return (uint32_t)(((x * 2685821657736338717ULL) >> 32) % m);
}
int cast_cursor_open(cast_cursor *c, const cast_queue *q, cast_classify_fn fn, void *ctx, uint64_t seed){
    if(!c) return -1;
    memset(c, 0, sizeof *c);
    if(!q || !q->n || !fn || q->play_mode < 0 || q->play_mode > 4) return -1;
    int *o = malloc((size_t)q->n * sizeof *o);
    if(!o) return -1;
    for(int i = 0; i < q->n; i++){
        if(!q->shuf){ o[i] = i; continue; }
        int *b = bsearch(&q->shuf[i], q->base, (size_t)q->n, sizeof(int), mdb_id_cmp);
        if(!b){ free(o); return -1; }
        o[i] = (int)(b - q->base);
    }
    c->q = q; c->classify = fn; c->ctx = ctx; c->mode = q->play_mode; c->order = o;
    c->pos = q->shuf ? q->cur_play : q->cur; c->rng = seed ? seed : 0x9E3779B97F4A7C15ULL;
    return 0;
}
void cast_cursor_close(cast_cursor *c){ if(c){ free(c->order); memset(c, 0, sizeof *c); } }
static cast_action load(cast_cursor *c, int p, int start_ms, int dur, int skipped){   /* issue a LOAD of order[p]: a new token, older events go stale */
    c->pos = p; c->gen++; c->pend = 1; c->media_id = 0; c->dur_ms = dur; c->base = c->prog = start_ms / 1000.0;
    cast_action a = { CA_LOAD, 0, 0, c->order[p], start_ms, skipped, c->gen };
    return a;
}
static cast_action stay(cast_cursor *c, int reason, int cause, int skipped){   /* pause on the current item; nothing pending, older events stale */
    c->gen++; c->pend = 0; c->media_id = 0;
    cast_action a = { CA_STAY_PAUSED, reason, cause, c->order[c->pos], 0, skipped, c->gen };
    return a;
}
static cast_action choice(cast_cursor *c, int verdict, int skipped){ return stay(c, CA_NEEDS_CHOICE, verdict == CF_V_EXCLUDED ? CA_C_EXCLUDED : CA_C_UNKNOWN_FORMAT, skipped); }
/* First playable occurrence from pos in direction dir: at most n looks, each occurrence once (wrap also ends on the current one).
 * Returns its position, -1 when none (end of queue without wrap, or all excluded), -2 when one could not be classified. */
static int walk(cast_cursor *c, int dir, int wrap, int *dur, int *skipped){
    int n = c->q->n;
    for(int i = 1; i <= n; i++){
        int p = c->pos + dir * i;
        if(p < 0 || p >= n){ if(!wrap) return -1; p = ((p % n) + n) % n; }
        int v = c->classify(c->ctx, c->order[p], dur);
        if(v == CF_V_OK) return p;
        if(v != CF_V_EXCLUDED){
            c->pos = p;
            return -2;
        }
        (*skipped)++;
    }
    return -1;
}
static cast_action reload(cast_cursor *c, int skipped){   /* the current item again from 0 (Repeat One, Previous restart) */
    int d = 0, v = c->classify(c->ctx, c->order[c->pos], &d);
    return v == CF_V_OK ? load(c, c->pos, 0, d, skipped) : choice(c, v, skipped);
}
static cast_action step(cast_cursor *c, int natural){   /* natural finish or manual Next */
    int d = 0, sk = 0, wrap = !natural || c->mode == 1 || c->mode == 3;
    if(natural && c->mode == 4) return stay(c, CA_QUEUE_FINISHED, 0, 0);
    if(natural && c->mode == 2) return reload(c, 0);
    int p = walk(c, 1, wrap, &d, &sk);
    if(p >= 0) return load(c, p, 0, d, sk);
    if(p == -2) return stay(c, CA_NEEDS_CHOICE, CA_C_UNKNOWN_FORMAT, sk);
    return wrap ? stay(c, CA_NEEDS_CHOICE, CA_C_NONE_PLAYABLE, sk) : stay(c, CA_QUEUE_FINISHED, 0, sk);
}
cast_action cast_cursor_play(cast_cursor *c, int start_ms){
    int d = 0, v = c->classify(c->ctx, c->order[c->pos], &d);
    return v == CF_V_OK ? load(c, c->pos, start_ms, d, 0) : choice(c, v, 0);   /* an explicit selection is never skipped */
}
void cast_cursor_adopted(cast_cursor *c, uint32_t gen, int media_id){
    if(c->pend && gen == c->gen && media_id > 0){ c->pend = 0; c->media_id = media_id; }
}
void cast_cursor_progress(cast_cursor *c, uint32_t gen, int media_id, double t){
    if(gen == c->gen && !c->pend && media_id > 0 &&
       media_id == c->media_id && t > c->prog)
        c->prog = t;
}
void cast_cursor_seeked(cast_cursor *c, double s){ c->base = c->prog = s; }   /* earlier progress says nothing about the new position */
cast_action cast_cursor_finished(cast_cursor *c, uint32_t gen, int media_id, int idle, double t){
    cast_action none = { CA_NOTHING, 0, 0, c->order[c->pos], 0, 0, c->gen };
    if(gen != c->gen || c->pend || !c->media_id ||
       media_id != c->media_id)
        return none;   /* not the current owned LOAD: stale or duplicate */
    if(idle != CS_IDLE_FINISHED && idle != CS_IDLE_ERROR) return none;   /* interrupted/cancelled: someone else's doing, not an advance */
    c->media_id = 0;                                                       /* consumed: a duplicate FINISHED finds nothing */
    if(idle == CS_IDLE_ERROR) return stay(c, CA_NEEDS_CHOICE, CA_C_RECEIVER_ERROR, 0);
    double reach = t > c->prog ? t : c->prog;
    if(c->dur_ms > 0 &&
       reach - c->base < CQ_EARLY_MOVED_S &&
       c->dur_ms / 1000.0 - c->base > CQ_EARLY_REMAIN_S)
        return stay(c, CA_NEEDS_CHOICE, CA_C_EARLY_FINISH, 0);   /* a track with real time left ended without moving */
    return step(c, 1);
}
cast_action cast_cursor_failed(cast_cursor *c, uint32_t gen){
    if(!c->pend || gen != c->gen){ cast_action none = { CA_NOTHING, 0, 0, c->order[c->pos], 0, 0, c->gen }; return none; }
    return stay(c, CA_NEEDS_CHOICE, CA_C_LOAD_FAILED, 0);
}
cast_action cast_cursor_next(cast_cursor *c){ return step(c, 0); }
cast_action cast_cursor_prev(cast_cursor *c, int position_ms){
    int d = 0, sk = 0;
    if(position_ms >= CQ_PREV_RESTART_MS) return reload(c, 0);
    int p = walk(c, -1, 1, &d, &sk);
    if(p >= 0) return load(c, p, 0, d, sk);
    return p == -2 ? stay(c, CA_NEEDS_CHOICE, CA_C_UNKNOWN_FORMAT, sk) : reload(c, sk);   /* nothing before it is playable: restart this one */
}
static void shuffle_from(cast_cursor *c, int f){   /* Fisher-Yates over order[f..n-1] with the cursor's own rng: the one shuffle algorithm */
    for(int i = c->q->n - 1; i > f; i--){
        int j = f + (int)rnd(c, (uint32_t)(i - f + 1)), t = c->order[i]; c->order[i] = c->order[j]; c->order[j] = t;
    }
}
int cast_cursor_set_mode(cast_cursor *c, int mode){
    if(mode < 0 || mode > 4) return -1;
    int n = c->q->n, was = c->mode == 1;
    c->mode = mode;
    if(mode == 1 && !was){          /* keep the played prefix and the current item; Fisher-Yates the rest, once per enable */
        shuffle_from(c, c->pos + 1);
    } else if(mode != 1 && was){    /* base order again, resumed from the current occurrence */
        c->pos = c->order[c->pos];
        for(int i = 0; i < n; i++) c->order[i] = i;
    }
    return 0;
}
uint32_t cast_cursor_attach_playing(cast_cursor *c, int start_ms, int media_id, int dur_ms){
    c->gen++; c->pend = 0; c->media_id = media_id > 0 ? media_id : 0; c->dur_ms = dur_ms > 0 ? dur_ms : 0;
    c->base = c->prog = start_ms / 1000.0;
    return c->gen;
}
cast_action cast_cursor_jump(cast_cursor *c, int pos){
    if(pos < 0 || pos >= c->q->n){ cast_action none = { CA_NOTHING, 0, 0, c->order[c->pos], 0, 0, c->gen }; return none; }
    c->pos = pos;
    return cast_cursor_play(c, 0);   /* load() / stay() bump the generation: every older event is stale */
}
cast_action cast_cursor_start_shuffled(cast_cursor *c, int start_ms){
    if(c->mode != 1) return cast_cursor_play(c, start_ms);   /* not shuffle: a plain explicit start (the order is never touched) */
    int t = c->order[c->pos]; c->order[c->pos] = c->order[0]; c->order[0] = t;   /* the current occurrence first ... */
    c->pos = 0;
    shuffle_from(c, 1);                                                          /* ... every other occurrence shuffled after it */
    return cast_cursor_play(c, start_ms);
}
/* ---- Slice 14f: play-order accessors and rows by explicit occurrences ---- */
int cast_cursor_at(const cast_cursor *c, int p){ return c && c->q && c->order && p >= 0 && p < c->q->n ? c->order[p] : -1; }
int cast_cursor_pos_of(const cast_cursor *c, int b){
    if(!c || !c->q || !c->order || b < 0 || b >= c->q->n) return -1;
    for(int p = 0; p < c->q->n; p++) if(c->order[p] == b) return p;
    return -1;
}
int cast_queue_id_at(const cast_queue *q, int i){
    if(!q || i < 0 || i >= q->n) return -1;
    return q->src == CQ_SRC_SELECTION ? q->sid[i] : q->base[i];
}
int cast_rows_by_ids(int src, const int *ids, const unsigned char *want, const int *idx, int n, mdb_qrow_t *out){
    if(!ids || !want || !idx || !out || n <= 0) return 0;
    for(int o = 0; o < n; o += CQ_PAGE_MAX){
        int m = n - o < CQ_PAGE_MAX ? n - o : CQ_PAGE_MAX, r; unsigned char got[CQ_PAGE_MAX];
        if(src == CQ_SRC_SELECTION){
            mdb_srow_t *rows = malloc((size_t)m * sizeof *rows);
            if(!rows) return -CQ_DB_ERROR;
            int pl = want[o] & CQ_F_PLAYLIST;
            for(int i = 1; i < m; i++) if((want[o + i] & CQ_F_PLAYLIST) != pl){ free(rows); return -CQ_REBUILDING; }   /* one kind per chunk */
            r = sel_rows(pl, ids + o, m, rows, got);
            if(pl) for(int i = 0; i < m; i++) got[i] |= CQ_F_PLAYLIST;
            for(int i = 0; i < m && r == m; i++){
                mdb_qrow_t *w = &out[o + i]; memset(w, 0, sizeof *w);
                w->base_id = idx[o + i] + 1; w->dur_ms = rows[i].dur_ms;
                snprintf(w->title, sizeof w->title, "%s", rows[i].title);
                snprintf(w->artist, sizeof w->artist, "%s", rows[i].artist);
                snprintf(w->path, sizeof w->path, "%s", rows[i].path);
            }
            free(rows);
        } else r = mdb_queue_rows(ids + o, m, out + o, got);
        if(r == MDB_UPNEXT_NOTFOUND || r == MDB_UPNEXT_BUSY) return -CQ_REBUILDING;
        if(r != m) return -CQ_DB_ERROR;
        for(int i = 0; i < m; i++){
            if(got[i] != want[o + i]) return -CQ_REBUILDING;           /* same id, different kind: rebuilt */
            out[o + i].ord = idx[o + i] + 1;
        }
    }
    return n;
}
int cast_queue_page_idx(const cast_queue *q, const int *idx, int n, mdb_qrow_t *out, unsigned char *flags){
    if(!q || !q->n || !idx || !out || n <= 0) return 0;
    int *ids = malloc((size_t)n * sizeof *ids), r = n; unsigned char *w = malloc((size_t)n);
    if(!ids || !w){ free(ids); free(w); return -CQ_DB_ERROR; }
    for(int i = 0; i < n && r == n; i++){
        int id = cast_queue_id_at(q, idx[i]);
        if(id < 0) r = 0; else { ids[i] = id; w[i] = q->flags[idx[i]]; }
    }
    if(r == n) r = cast_rows_by_ids(q->src, ids, w, idx, n, out);
    if(r == n && flags) memcpy(flags, w, (size_t)n);
    free(ids); free(w);
    return r;
}
