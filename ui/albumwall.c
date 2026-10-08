/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "sdio.h"
#include "musicdb.h"
#include "artcache.h"
#include "config.h"
#include "i18n.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>

/* Album Wall: an Apple-style COVER FLOW that FLOWS. The centred album faces the viewer; the covers to
 * each side are angled (an affine squash, dimmed) with a faded reflection. A swipe slides the whole strip
 * one album over, with the two covers changing orientation cross-fading between a "facing" and a "turned"
 * pose. No GPU: each cover is decoded once and BAKED into two ready-to-blit sprites (a 148px facing sprite
 * and a 64px turned sprite, reflection included), kept in an LRU + prefetched in a window - so the flow
 * animation only translates + blends static bitmaps (no per-frame transforms) and stays smooth. Covers
 * come from the art cache (a track from the album must have been played); un-cached albums show a dark
 * placeholder. Tap the centre to play, long-press to open its tracks (dispatched from main.c). */

#define AW_SRC    148      /* cached cover native size (must match artcache's c.bmp) */
#define AW_REFL   32       /* reflection height */
#define AW_GAP    2        /* gap between a cover and its reflection */
#define AW_SPR_H  (AW_SRC + AW_GAP + AW_REFL)   /* 182: baked sprite height (cover + gap + reflection) */
#define AW_SIDE_W 64       /* turned (side) sprite width */
#define AW_DIM    174      /* side brightness: RGB * 174/256 ~= 0.68 */
#define AW_CARDS  9        /* rendered cards: bases -4..+4 (buffers for entering/leaving) */
#define AW_LRU    13       /* baked sprites resident (9 visible span + prefetch) */
#define AW_PREFETCH 5      /* decode this far each side while browsing */

/* slot centres at integer positions -2..+2; the strip interpolates between them */
static const float SLOT_X[5] = { 52, 90, 180, 270, 308 };

static lv_obj_t *g_root;
static lv_obj_t *g_front[AW_CARDS];   /* facing sprite widget per card */
static lv_obj_t *g_side[AW_CARDS];    /* turned sprite widget per card */
static lv_obj_t *g_initial;
static lv_obj_t *g_name, *g_artist, *g_counter;
static lv_obj_t *g_sb_track = NULL, *g_sb_thumb = NULL;
static lv_obj_t *g_az_btn, *g_grid;
static lv_obj_t *g_lhint = NULL, *g_lhint_lbl = NULL;
static uint32_t  g_lhint_tick = 0;
static char      g_lhint_ch = 0;
static lv_timer_t *g_lhint_timer = NULL;

static char  (*g_names)[MDB_STR];
static char  (*g_artists)[MDB_STR];
static int    *g_counts;
static int     g_names_cap = 0;   /* allocated entries in the three album buffers (grows with the library) */
static int     g_aw_inited = 0;   /* one-time blank-sprite + LRU init done */
static int     g_nalb;
static int     g_cur;
static float    g_off;                /* strip offset during a step animation (0 at rest) */
static int      g_animating;
static int      g_drag_on;
static uint32_t g_tickctr;
static uint8_t  g_src[AW_SRC*AW_SRC*4];   /* scratch decode buffer (UI thread only) */

/* a baked cover: facing sprite (front) + turned sprite (side), each cover + reflection */
typedef struct {
    int      idx;
    int      has_cover;
    uint8_t *fbuf;                     /* AW_SRC   * AW_SPR_H * 4 ARGB */
    uint8_t *sbuf;                     /* AW_SIDE_W* AW_SPR_H * 4 ARGB */
    lv_image_dsc_t fdsc, sdsc;
    uint32_t tick;
} sprite_t;
static sprite_t g_lru[AW_LRU];
static sprite_t g_blank;               /* dark placeholder (no art) */

/* ---- decode + bake ------------------------------------------------------------------------------ */
static int aw_read_bmp(const char *path, uint8_t *buf){
    FILE *f = fopen(path, "rb"); if(!f) return -1;
    uint8_t hdr[54]; if(fread(hdr,1,54,f)!=54){ fclose(f); return -1; }
    uint32_t off = hdr[10]|(hdr[11]<<8)|(hdr[12]<<16)|((uint32_t)hdr[13]<<24);
    int w = hdr[18]|(hdr[19]<<8), h = hdr[22]|(hdr[23]<<8), bpp = hdr[28]|(hdr[29]<<8);
    uint32_t comp = hdr[30]|(hdr[31]<<8)|(hdr[32]<<16)|((uint32_t)hdr[33]<<24);
    if(w!=AW_SRC || h!=AW_SRC || bpp!=24 || comp!=0){ fclose(f); return -1; }
    uint8_t row[AW_SRC*3];
    if(fseek(f, off, SEEK_SET)!=0){ fclose(f); return -1; }
    for(int yy=0; yy<AW_SRC; yy++){
        if(fread(row,1,AW_SRC*3,f)!=(size_t)(AW_SRC*3)){ fclose(f); return -1; }
        uint8_t *d = buf + (AW_SRC-1-yy)*AW_SRC*4;
        for(int x=0;x<AW_SRC;x++){ d[x*4]=row[x*3]; d[x*4+1]=row[x*3+1]; d[x*4+2]=row[x*3+2]; d[x*4+3]=0xFF; }
    }
    fclose(f); return 0;
}
static void aw_set_dsc(lv_image_dsc_t *d, uint8_t *buf, int w, int h){
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_ARGB8888; d->header.w = w; d->header.h = h; d->header.stride = w*4;
    d->data = buf; d->data_size = (uint32_t)w*h*4;
}
static inline int refl_alpha(int y){ return 64 - (64*y)/(AW_REFL-1); }   /* 64 -> 0 */

/* facing sprite: cover rows, a transparent gap, then the mirrored+faded reflection */
static void aw_bake_front(const uint8_t *src, uint8_t *f){
    for(int y=0;y<AW_SRC;y++) memcpy(f + y*AW_SRC*4, src + y*AW_SRC*4, AW_SRC*4);   /* cover, opaque */
    memset(f + AW_SRC*AW_SRC*4, 0, AW_GAP*AW_SRC*4);                                /* gap */
    for(int y=0;y<AW_REFL;y++){
        const uint8_t *s = src + (AW_SRC-1-y)*AW_SRC*4; int a = refl_alpha(y);
        uint8_t *d = f + (AW_SRC+AW_GAP+y)*AW_SRC*4;
        for(int x=0;x<AW_SRC;x++){ d[x*4]=s[x*4]; d[x*4+1]=s[x*4+1]; d[x*4+2]=s[x*4+2]; d[x*4+3]=(uint8_t)a; }
    }
}
/* turned sprite: horizontally squashed 148->64 + dimmed, same cover/gap/reflection structure */
static void aw_bake_side(const uint8_t *src, uint8_t *s){
    for(int y=0;y<AW_SRC;y++){
        const uint8_t *sr = src + y*AW_SRC*4; uint8_t *d = s + y*AW_SIDE_W*4;
        for(int dx=0;dx<AW_SIDE_W;dx++){
            const uint8_t *p = sr + (dx*AW_SRC/AW_SIDE_W)*4;
            d[dx*4+0]=(uint8_t)(p[0]*AW_DIM>>8); d[dx*4+1]=(uint8_t)(p[1]*AW_DIM>>8);
            d[dx*4+2]=(uint8_t)(p[2]*AW_DIM>>8); d[dx*4+3]=0xFF;
        }
    }
    memset(s + AW_SRC*AW_SIDE_W*4, 0, AW_GAP*AW_SIDE_W*4);
    for(int y=0;y<AW_REFL;y++){
        const uint8_t *sr = src + (AW_SRC-1-y)*AW_SRC*4; int a = refl_alpha(y);
        uint8_t *d = s + (AW_SRC+AW_GAP+y)*AW_SIDE_W*4;
        for(int dx=0;dx<AW_SIDE_W;dx++){
            const uint8_t *p = sr + (dx*AW_SRC/AW_SIDE_W)*4;
            d[dx*4+0]=(uint8_t)(p[0]*AW_DIM>>8); d[dx*4+1]=(uint8_t)(p[1]*AW_DIM>>8);
            d[dx*4+2]=(uint8_t)(p[2]*AW_DIM>>8); d[dx*4+3]=(uint8_t)a;
        }
    }
}

static void aw_bake(sprite_t *sp, int idx){
    sp->idx = idx; sp->has_cover = 0;
    if(idx >= 0 && idx < g_nalb){
        int ids[6]; int n = mdb_album_track_ids(g_names[idx], ids, 6);
        for(int i=0;i<n;i++){
            char track[512]; track[0]=0; mdb_song_path(ids[i], track, sizeof track);
            char cov[600];
            int loaded = 0;
            if(track[0] && sd_io_begin()){
                loaded = artcache_cover_path(track, cov, sizeof cov)==0 && aw_read_bmp(cov, g_src)==0;
                sd_io_end();
            }
            if(loaded){
                aw_bake_front(g_src, sp->fbuf); aw_bake_side(g_src, sp->sbuf);
                aw_set_dsc(&sp->fdsc, sp->fbuf, AW_SRC, AW_SPR_H);
                aw_set_dsc(&sp->sdsc, sp->sbuf, AW_SIDE_W, AW_SPR_H);
                sp->has_cover = 1; return;
            }
        }
    }
    /* MISS: point at the shared placeholder. Never leave the descriptors untouched - a reused LRU slot
     * would otherwise still display the PREVIOUS album's baked cover. */
    sp->fdsc = g_blank.fdsc;
    sp->sdsc = g_blank.sdsc;
}
/* is this album currently on screen (a card)? Such entries must never be evicted while their image
 * widget still points at their pixel buffer, or the prefetch would swap the cover under a visible card. */
static int aw_visible(int idx){
    if(g_nalb <= 0 || idx < 0) return 0;
    int d = ((idx - g_cur) % g_nalb + g_nalb) % g_nalb;   /* circular distance 0..g_nalb-1 */
    int half = AW_CARDS/2;
    return d <= half || d >= g_nalb - half;
}
static sprite_t *aw_sprite(int idx){
    for(int i=0;i<AW_LRU;i++)
        if(g_lru[i].idx == idx){ g_lru[i].tick = ++g_tickctr; return &g_lru[i]; }
    sprite_t *lru = NULL;
    for(int i=0;i<AW_LRU;i++){                              /* evict the oldest NON-visible entry */
        if(aw_visible(g_lru[i].idx)) continue;
        if(!lru || g_lru[i].tick < lru->tick) lru = &g_lru[i];
    }
    if(!lru) lru = &g_lru[0];                               /* unreachable: AW_LRU > visible span */
    if(!lru->fbuf) lru->fbuf = malloc(AW_SRC*AW_SPR_H*4);
    if(!lru->sbuf) lru->sbuf = malloc(AW_SIDE_W*AW_SPR_H*4);
    if(!lru->fbuf || !lru->sbuf){ free(lru->fbuf); free(lru->sbuf); lru->fbuf=NULL; lru->sbuf=NULL; lru->idx=-1; return NULL; }
    aw_bake(lru, idx); lru->tick = ++g_tickctr; return lru;
}

/* ---- flow geometry ------------------------------------------------------------------------------ */
static int aw_wrap(int idx){ return ((idx % g_nalb) + g_nalb) % g_nalb; }
static float aw_xmap(float p){
    if(p <= -2) return SLOT_X[0] + (p + 2) * (SLOT_X[1]-SLOT_X[0]);
    if(p >=  2) return SLOT_X[4] + (p - 2) * (SLOT_X[4]-SLOT_X[3]);
    int i = (int)floorf(p) + 2; if(i<0) i=0; if(i>3) i=3;
    float frac = p - floorf(p);
    return SLOT_X[i] + (SLOT_X[i+1]-SLOT_X[i]) * frac;
}
/* render one card at continuous position epos (0 = centre). Sets both its sprites' pos + opacity. */
static void aw_render_card(int c){
    float epos = c - (AW_CARDS/2) + g_off;      /* card c's base is (c - centre) */
    lv_obj_t *fi = g_front[c], *si = g_side[c];
    float a = fabsf(epos);
    if(a > 2.75f){ lv_obj_add_flag(fi, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(si, LV_OBJ_FLAG_HIDDEN); return; }

    int album = aw_wrap(g_cur + (c - AW_CARDS/2));
    sprite_t *sp = aw_sprite(album); if(!sp) sp = &g_blank;
    lv_image_set_src(fi, &sp->fdsc);
    lv_image_set_src(si, &sp->sdsc);

    int fade = 255;                              /* edge fade in/out */
    if(a > 2.25f){ fade = (int)((2.75f - a)/0.5f * 255); if(fade<0) fade=0; }
    int fo, so;                                  /* facing vs turned crossfade */
    if(a <= 1.0f){ fo = (int)((1.0f - a)*255); so = (int)(a*255); }
    else { fo = 0; so = 255; }
    fo = fo*fade/255; so = so*fade/255;

    float x = aw_xmap(epos);
    lv_obj_set_pos(fi, (int)(x - AW_SRC/2), 72);
    lv_obj_set_style_image_opa(fi, (lv_opa_t)fo, 0);
    if(fo>0) lv_obj_clear_flag(fi, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(fi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(si, (int)(x - AW_SIDE_W/2), 78);
    lv_obj_set_style_image_opa(si, (lv_opa_t)so, 0);
    if(so>0) lv_obj_clear_flag(si, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(si, LV_OBJ_FLAG_HIDDEN);
}
static void aw_labels(void){
    if(g_nalb<=0) return;
    sprite_t *c = aw_sprite(g_cur);
    if(c && !c->has_cover){
        char in[8]={0}; const char *nm=g_names[g_cur];
        if(nm && nm[0]){ unsigned char c0=(unsigned char)nm[0];
            int len=(c0<0x80)?1:((c0>>5)==0x6)?2:((c0>>4)==0xE)?3:((c0>>3)==0x1E)?4:1;
            for(int i=0;i<len && nm[i];i++) in[i]=nm[i];
            if(len==1 && in[0]>='a'&&in[0]<='z') in[0]-=32;
        }else in[0]='?';
        lv_label_set_text(g_initial, in); lv_obj_clear_flag(g_initial, LV_OBJ_FLAG_HIDDEN);
    }else lv_obj_add_flag(g_initial, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(g_name, g_names[g_cur]);
    lv_label_set_text(g_artist, g_artists[g_cur][0]?g_artists[g_cur]:"Unknown artist");
    char cc[24]; snprintf(cc,sizeof cc,"%d / %d", g_cur+1, g_nalb); lv_label_set_text(g_counter, cc);
}
static void aw_render_all(void){ for(int c=0;c<AW_CARDS;c++) aw_render_card(c); }

/* ---- continuous position + animation ------------------------------------------------------------
 * The strip is a single continuous coordinate g_posf (fractional album index, wraps mod g_nalb).
 * Everything - finger drag, inertial fling, discrete step, rim scroll - just moves g_posf; the render
 * derives g_cur (nearest album, drives the labels) and g_off (sub-album offset, drives the slide). */
static float g_posf;                     /* continuous album position; g_cur=round(g_posf), g_off=g_cur-g_posf */

static void aw_update_scrollbar(void){
    if(!g_sb_track || !g_sb_thumb || g_nalb <= 1) return;
    int track_w = 140;
    int thumb_w = track_w / g_nalb;
    if(thumb_w < 18) thumb_w = 18;
    if(thumb_w > 50) thumb_w = 50;
    lv_obj_set_size(g_sb_thumb, thumb_w, 8);
    int max_x = track_w - thumb_w;
    float norm = (g_nalb > 1) ? g_posf / (float)(g_nalb - 1) : 0.0f;
    if(norm < 0.0f) norm = 0.0f;
    if(norm > 1.0f) norm = 1.0f;
    lv_obj_set_pos(g_sb_thumb, (int)(norm * max_x), 0);
}

static void aw_apply_pos(float P){
    if(g_nalb<=0) return;
    while(P < 0)          P += g_nalb;
    while(P >= g_nalb)    P -= g_nalb;
    g_posf = P;
    int r = (int)lroundf(P);                             /* nearest album (0..g_nalb) */
    int newcur = aw_wrap(r);
    g_off = (float)r - P;                                /* in [-0.5,0.5]; see aw_render_card sign */
    if(newcur != g_cur){
        g_cur = newcur;
        aw_render_all();
        aw_labels();
    }
    else                 aw_render_all();
    aw_update_scrollbar();
}
static int   g_anim_tag;
static float g_anim_from, g_anim_to;
static void aw_anim_exec(void *v, int32_t val){ (void)v; float k=val/1000.0f; aw_apply_pos(g_anim_from + (g_anim_to-g_anim_from)*k); }
static void aw_anim_done(lv_anim_t *a){ (void)a; aw_apply_pos(g_anim_to); g_animating = 0; }
static void aw_anim_to(float to, uint32_t dur){          /* glide g_posf -> to (ease-out), then snap exact */
    lv_anim_delete(&g_anim_tag, NULL);
    g_anim_from = g_posf; g_anim_to = to; g_animating = 1;
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, &g_anim_tag);
    lv_anim_set_values(&a, 0, 1000); lv_anim_set_duration(&a, dur);
    lv_anim_set_exec_cb(&a, aw_anim_exec); lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, aw_anim_done); lv_anim_start(&a);
}

void albumwall_scrub_to(float norm){
    if(g_nalb <= 1) return;
    if(norm < 0.0f) norm = 0.0f;
    if(norm > 1.0f) norm = 1.0f;
    lv_anim_delete(&g_anim_tag, NULL);
    g_animating = 0;
    float target = norm * (float)(g_nalb - 1);
    aw_apply_pos(target);
    albumwall_scroll_letter_tick();
}

static void aw_sb_event_cb(lv_event_t *e){
    (void)e;
    if(g_nalb <= 1 || !g_sb_track) return;
    lv_indev_t *indev = lv_indev_active();
    if(!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(g_sb_track, &a);
    int rel_x = p.x - a.x1;
    int w = lv_area_get_width(&a);
    if(w <= 0) return;
    float norm = (float)rel_x / (float)w;
    albumwall_scrub_to(norm);
}
void albumwall_step(int dir){                            /* discrete: one album (keys / demo) */
    if(g_nalb<=0) return;
    aw_anim_to(roundf(g_posf) + (dir>=0?1.0f:-1.0f), 210);
}
/* continuous nudge (rim scroll) + snap-to-nearest (rim release) */
void albumwall_scroll_rel(float d_alb){ if(g_nalb<=0) return; lv_anim_delete(&g_anim_tag,NULL); g_animating=0; aw_apply_pos(g_posf + d_alb); }
void albumwall_settle(void){ if(g_nalb<=0) return; aw_anim_to(roundf(g_posf), 200); }

/* ---- finger drag + inertial fling --------------------------------------------------------------- */
#define AW_PX_PER_ALB 110.0f                             /* horizontal finger travel that advances one album */
static float    g_drag_base;                             /* g_posf at press */
static int      g_drag_sx;                               /* press x */
static float    g_drag_vel;                              /* smoothed velocity, albums/ms (>0 = toward next) */
static int      g_drag_lastx, g_drag_renderx; static uint32_t g_drag_lastt;
void albumwall_drag_begin(int px){
    if(g_nalb<=0) return;
    if(g_grid && !lv_obj_has_flag(g_grid, LV_OBJ_FLAG_HIDDEN)) return;
    if(g_lhint) lv_obj_add_flag(g_lhint, LV_OBJ_FLAG_HIDDEN);
    lv_anim_delete(&g_anim_tag, NULL); g_animating = 0;  /* grab: stop any fling/step in flight */
    g_drag_on = 1; g_drag_base = g_posf; g_drag_sx = px;
    g_drag_vel = 0; g_drag_lastx = px; g_drag_renderx = px; g_drag_lastt = lv_tick_get();
}
void albumwall_drag(int px){
    if(!g_drag_on || g_nalb<=0) return;
    if(px != g_drag_renderx){                            /* only re-render when the finger actually moved */
        aw_apply_pos(g_drag_base - (float)(px - g_drag_sx)/AW_PX_PER_ALB);  /* finger left => next */
        g_drag_renderx = px;
    }
    uint32_t now = lv_tick_get(), dt = now - g_drag_lastt;
    if(dt >= 8){                                         /* resample velocity on a stable interval */
        float v = -(float)(px - g_drag_lastx)/AW_PX_PER_ALB/(float)dt;
        g_drag_vel = g_drag_vel*0.4f + v*0.6f;
        g_drag_lastx = px; g_drag_lastt = now;
    }
}
void albumwall_drag_cancel(void){ g_drag_on = 0; }
void albumwall_drag_end(void){                           /* release: throw a few albums, decelerate, snap */
    if(!g_drag_on || g_nalb<=0){ g_drag_on = 0; return; }
    g_drag_on = 0;
    float thr = g_drag_vel * 220.0f;                     /* coast ~220ms of the release velocity */
    if(thr >  8) thr =  8; else if(thr < -8) thr = -8;   /* cap the throw so a hard flick stays controllable */
    float target = roundf(g_posf + thr);
    float dist = target - g_posf; if(dist<0) dist=-dist;
    uint32_t dur = (uint32_t)(dist*120.0f) + 130; if(dur > 700) dur = 700;
    aw_anim_to(target, dur);
}
void albumwall_play(void){
    if(g_nalb<=0) return;
    if(ui_get_source_mode() == 4 && !ui_usb_dac_connected()){
        ui_toast("USB DAC not connected"); return;
    }
    cfg_set_int("work_mode",0); ui_set_workmode(0); if(ui_play_list(3,g_names[g_cur],1)) screen_show(SCR_NOWPLAYING);
}
void albumwall_open(void){ if(g_nalb<=0) return; library_open_album(g_names[g_cur]); screen_show(SCR_LIBRARY); }

/* ---- prefetch ----------------------------------------------------------------------------------- */
/* When the background prewarm lands a new cover, drop any placeholder sprites so they re-bake with the
 * now-available art. Re-baking a still-coverless album reproduces the same placeholder pixels, so this
 * is visually a no-op for albums whose art hasn't arrived yet (no flicker). */
static unsigned g_last_ac_gen;
static void aw_rebake_new_covers(void){
    int vis_changed = 0;
    for(int i=0;i<AW_LRU;i++){
        if(g_lru[i].idx>=0 && !g_lru[i].has_cover){        /* only placeholders can gain art */
            if(aw_visible(g_lru[i].idx)) vis_changed = 1;
            g_lru[i].idx = -1;                             /* evict -> fresh bake (with cover if now cached) on next access */
        }
    }
    if(vis_changed){ aw_render_all(); aw_labels(); }       /* repaint the on-screen cards + centre initial */
}

/* ---- background album-cover prewarm seed (decoupled from the Cover Flow view) --------------------
 * Enumerates the album list on the MAIN thread (mdb in-memory caches are not thread-safe) and enqueues
 * the representative track of every album lacking a cached cover, for the ui.c worker to decode. Runs
 * incrementally off its own timer so it never hitches boot/scan, re-walks the whole list each start
 * (cheap: one indexed lookup per album - the WORKER skips albums whose cover is already cached, so no SD
 * file checks run on the UI thread) so it catches same-count library changes, and needs no visit to the
 * Album view. Started at boot, after each scan, and on Cover Flow open. */
static char (*g_seed_names)[MDB_STR];
static int         g_seed_n, g_seed_cur;
static lv_timer_t *g_seed_timer;
static int *g_seed_rep;                                     /* per album: representative SONG.ID (0 = none) */
static void aw_seed_cb(lv_timer_t *t){
    if(!g_seed_names || g_seed_cur >= g_seed_n){ lv_timer_delete(t); g_seed_timer = NULL; return; }
    lv_timer_set_period(t, 50);                                 /* back to full pace (a full queue slows it below) */
    for(int k=0; k<3 && g_seed_cur < g_seed_n; k++){
        /* The album's representative song (its first in library order, recorded when the album cache was built):
         * one indexed ID lookup instead of comparing the album name against every song. Only if that path can't
         * be read fall back to the album's other songs, as before. */
        int ids[6], m = 0;
        if(g_seed_rep && g_seed_rep[g_seed_cur] > 0){ ids[0] = g_seed_rep[g_seed_cur]; m = 1; }
        for(int pass = 0; pass < 2; pass++){
            int found = 0;
            for(int i=0;i<m;i++){
                char track[512]; track[0]=0;
                if(mdb_song_path(ids[i], track, sizeof track) && track[0]){
                    found = 1;
                    if(!ui_prewarm_enqueue(track)){            /* queue full: the worker isn't draining it right now
                                                                 * (heat, card access, a live decode) - retry this album
                                                                 * slowly instead of re-querying it 20x a second */
                        lv_timer_set_period(t, 2000);
                        return;
                    }
                    break;                                      /* one representative track per album */
                }
            }
            if(found || pass) break;
            m = mdb_album_track_ids(g_seed_names[g_seed_cur], ids, 6);   /* fallback: the slow scan */
        }
        g_seed_cur++;
    }
}
void albumwall_prewarm_seed(void){                          /* MAIN thread: (re)start the incremental enqueue walk */
    static char (*artists)[MDB_STR]; static int *counts; static int seed_cap = 0;
    int want = mdb_album_count();                           /* size to the real album count (no >600 truncation) */
    if(want < 1) return;                                    /* <0 = OOM building the cache, 0 = empty -> nothing to seed (retry next start) */
    if(!g_seed_names || seed_cap < want){                   /* main-thread only + repopulated below -> free+malloc safe */
        free(g_seed_names); free(artists); free(counts); free(g_seed_rep);
        g_seed_names = malloc((size_t)want*MDB_STR);
        artists      = malloc((size_t)want*MDB_STR);
        counts       = malloc((size_t)want*sizeof(int));
        g_seed_rep   = malloc((size_t)want*sizeof(int));
        if(!g_seed_names || !artists || !counts || !g_seed_rep){ free(g_seed_names); free(artists); free(counts); free(g_seed_rep); g_seed_names=NULL; artists=NULL; counts=NULL; g_seed_rep=NULL; seed_cap=0; return; }
        seed_cap = want;
    }
    g_seed_n = mdb_albums(g_seed_names, artists, counts, seed_cap); g_seed_cur = 0;
    if(mdb_album_rep_ids(g_seed_rep, seed_cap) != g_seed_n)
        for(int i=0;i<seed_cap;i++) g_seed_rep[i] = 0;      /* reps unavailable -> every album uses the fallback scan */
    if(g_seed_n > 0 && !g_seed_timer) g_seed_timer = lv_timer_create(aw_seed_cb, 50, NULL);
}

static lv_timer_t *g_prefetch;
static void aw_prefetch_cb(lv_timer_t *t){ (void)t;
    if(screen_current()!=SCR_ALBUMWALL || g_nalb<=0) return;
    unsigned g = artcache_gen();
    if(g != g_last_ac_gen){ g_last_ac_gen = g; aw_rebake_new_covers(); }   /* pick up freshly-decoded covers */
    for(int d=1; d<=AW_PREFETCH; d++) for(int s=-1;s<=1;s+=2){
        int idx = aw_wrap(g_cur + s*d), res=0;
        for(int i=0;i<AW_LRU;i++) if(g_lru[i].idx==idx){ res=1; break; }
        if(!res){ aw_sprite(idx); return; }
    }
}


/* ---- lifecycle & fast scrolling -------------------------------------------------- */
static char first_letter(const char *s){
    while(*s==' ') s++;
    char c = toupper((unsigned char)*s);
    return (c>='A'&&c<='Z') ? c : '#';
}

static void aw_jump_to_letter(char L){
    if(g_nalb <= 0 || !g_names) return;
    int idx = -1;
    for(int i = 0; i < g_nalb; i++){
        if(first_letter(g_names[i]) == L){ idx = i; break; }
    }
    if(idx < 0){
        for(int i = 0; i < g_nalb; i++){
            if(first_letter(g_names[i]) >= L){ idx = i; break; }
        }
    }
    if(idx < 0) idx = g_nalb - 1;

    int d = abs(idx - g_cur);
    if(d > g_nalb / 2) d = g_nalb - d;
    if(d <= 4) aw_anim_to((float)idx, 200);
    else       aw_apply_pos((float)idx);
    albumwall_scroll_letter_tick();
}

static void aw_letter_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    aw_jump_to_letter((char)(intptr_t)lv_event_get_user_data(e));
    if(g_grid) lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
}
static void aw_grid_bg_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED && g_grid)
        lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
}
static void aw_az_btn_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED && g_grid)
        lv_obj_clear_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
}
static void aw_lhint_timer_cb(lv_timer_t *t){
    (void)t;
    if(g_lhint && !lv_obj_has_flag(g_lhint, LV_OBJ_FLAG_HIDDEN) && lv_tick_elaps(g_lhint_tick) > 650)
        lv_obj_add_flag(g_lhint, LV_OBJ_FLAG_HIDDEN);
}

void albumwall_scroll_letter_tick(void){
    if(!g_root || !g_lhint || g_nalb <= 0 || !g_names) return;
    char ch = first_letter(g_names[g_cur]);
    if(ch && ch != g_lhint_ch){
        g_lhint_ch = ch;
        char b[2] = {ch, 0};
        lv_label_set_text(g_lhint_lbl, b);
    }
    g_lhint_tick = lv_tick_get();
    lv_obj_clear_flag(g_lhint, LV_OBJ_FLAG_HIDDEN);
}

int albumwall_is_grid_open(void){
    return g_grid && !lv_obj_has_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
}

int albumwall_back(void){
    if(albumwall_is_grid_open()){
        lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
        return 1;
    }
    return 0;
}

static void aw_back_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED){
        if(!albumwall_back()) screen_back();
    }
}
static lv_obj_t *aw_img(void){ lv_obj_t *im = lv_image_create(g_root); lv_obj_add_flag(im, LV_OBJ_FLAG_HIDDEN); return im; }

void albumwall_create(lv_obj_t *root){
    g_root = root;
    if(!g_prefetch) g_prefetch = lv_timer_create(aw_prefetch_cb, 40, NULL);
    if(!g_lhint_timer) g_lhint_timer = lv_timer_create(aw_lhint_timer_cb, 150, NULL);
}

void albumwall_refresh(void){
    if(!g_root) return;
    if(!g_aw_inited){
        /* placeholder sprites (built once; BGRA from the theme's art-wall roles) */
        static uint8_t fb[AW_SRC*AW_SPR_H*4], sb[AW_SIDE_W*AW_SPR_H*4];
        memset(fb,0,sizeof fb); memset(sb,0,sizeof sb);
        uint32_t fc = theme_rgb(THEME_CLR_ART_WALL_FRONT), sc = theme_rgb(THEME_CLR_ART_WALL_SIDE);
        for(int y=0;y<AW_SRC;y++){ for(int x=0;x<AW_SRC;x++){ uint8_t *d=fb+(y*AW_SRC+x)*4; d[0]=fc&255;d[1]=fc>>8&255;d[2]=fc>>16&255;d[3]=0xFF; }
                                   for(int x=0;x<AW_SIDE_W;x++){ uint8_t *d=sb+(y*AW_SIDE_W+x)*4; d[0]=sc&255;d[1]=sc>>8&255;d[2]=sc>>16&255;d[3]=0xFF; } }
        g_blank.has_cover=0; g_blank.idx=-2; aw_set_dsc(&g_blank.fdsc, fb, AW_SRC, AW_SPR_H); aw_set_dsc(&g_blank.sdsc, sb, AW_SIDE_W, AW_SPR_H);
        for(int i=0;i<AW_LRU;i++) g_lru[i].idx=-1;
        g_aw_inited = 1;
    }
    /* Size the album buffers to the REAL album count so a >600-album library is never truncated (the
     * Library list sizes dynamically too). They're repopulated fresh below, so free+malloc on grow is safe. */
    { int want = mdb_album_count();
      if(want < 0){                       /* album cache couldn't be built (OOM) -> don't fake a capacity-1 buffer;
                                           * drop to the "Out of memory" path below instead of a 1-album list */
          free(g_names); free(g_artists); free(g_counts); g_names=NULL; g_artists=NULL; g_counts=NULL; g_names_cap=0;
      } else {
          if(want < 1) want = 1;
          if(!g_names || g_names_cap < want){
              free(g_names); free(g_artists); free(g_counts);
              g_names   = malloc((size_t)want * MDB_STR);
              g_artists = malloc((size_t)want * MDB_STR);
              g_counts  = malloc((size_t)want * sizeof(int));
              if(!g_names||!g_artists||!g_counts){ free(g_names); free(g_artists); free(g_counts); g_names=NULL; g_artists=NULL; g_counts=NULL; g_names_cap=0; }
              else g_names_cap = want;
          }
      }
    }
    lv_anim_delete(&g_anim_tag, NULL);           /* stop any in-flight fling BEFORE lv_obj_clean frees the widgets it animates */
    for(int i=0;i<AW_LRU;i++) g_lru[i].idx=-1;   /* album list may have changed -> reload */
    g_off = 0; g_animating = 0; g_drag_on = 0;

    lv_obj_clean(g_root);
    for(int c=0;c<AW_CARDS;c++){ g_front[c]=NULL; g_side[c]=NULL; }
    lv_obj_set_style_bg_color(g_root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    ui_header_cb(g_root, "Albums", aw_back_cb);

    g_nalb = g_names ? mdb_albums(g_names, g_artists, g_counts, g_names_cap) : 0;
    if(g_nalb <= 0){
        lv_obj_t *e = lv_label_create(g_root);
        lv_label_set_text(e, g_names ? "No albums found" : "Out of memory");
        lv_obj_center(e); lv_obj_set_style_text_color(e, TC(TEXT_MUTED), 0);
        return;
    }
    if(g_cur >= g_nalb) g_cur = 0;
    g_posf = g_cur; g_drag_on = 0;   /* sync the continuous coordinate to the (possibly clamped) centre album */
    albumwall_prewarm_seed();        /* (re)kick the background cover fill for the current album set */

    /* create cards: sides (turned) first, then fronts (facing) so a facing cover draws over a turned one;
     * within each, outer cards first so the centre is on top */
    static const int zc[AW_CARDS] = { 0,8,1,7,2,6,3,5,4 };
    for(int k=0;k<AW_CARDS;k++) g_side[zc[k]]  = aw_img();
    for(int k=0;k<AW_CARDS;k++) g_front[zc[k]] = aw_img();

    g_initial = lv_label_create(g_root);
    lv_obj_set_style_text_font(g_initial, TF(USER_20), 0);
    lv_obj_set_style_text_color(g_initial, TC(TEXT_MUTED), 0);
    lv_obj_align(g_initial, LV_ALIGN_TOP_MID, 0, 72 + AW_SRC/2 - 12);
    lv_obj_add_flag(g_initial, LV_OBJ_FLAG_HIDDEN);

    g_name = lv_label_create(g_root);
    lv_label_set_long_mode(g_name, LV_LABEL_LONG_DOT); lv_obj_set_size(g_name, 260, 24);
    lv_obj_set_style_text_align(g_name, LV_TEXT_ALIGN_CENTER, 0); lv_obj_align(g_name, LV_ALIGN_TOP_MID, 0, 266);
    lv_obj_set_style_text_font(g_name, TF(USER_18), 0); lv_obj_set_style_text_color(g_name, TC(TEXT_PRIMARY), 0);

    g_artist = lv_label_create(g_root);
    lv_label_set_long_mode(g_artist, LV_LABEL_LONG_DOT); lv_obj_set_size(g_artist, 220, 20);
    lv_obj_set_style_text_align(g_artist, LV_TEXT_ALIGN_CENTER, 0); lv_obj_align(g_artist, LV_ALIGN_TOP_MID, 0, 292);
    lv_obj_set_style_text_font(g_artist, TF(USER_16), 0); lv_obj_set_style_text_color(g_artist, TC(TEXT_MUTED), 0);

    /* album position scroll bar: track + sliding thumb */
    g_sb_track = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_sb_track);
    lv_obj_set_size(g_sb_track, 140, 8);
    lv_obj_align(g_sb_track, LV_ALIGN_TOP_MID, 0, 312);
    lv_obj_set_style_bg_color(g_sb_track, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(g_sb_track, LV_OPA_70, 0);
    lv_obj_set_style_radius(g_sb_track, 4, 0);
    lv_obj_clear_flag(g_sb_track, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(g_sb_track, aw_sb_event_cb, LV_EVENT_CLICKED, NULL, "albumwall.sb_click", UI_CORE);
    ui_on(g_sb_track, aw_sb_event_cb, LV_EVENT_PRESSING, NULL, "albumwall.sb_drag", UI_CORE);

    g_sb_thumb = lv_obj_create(g_sb_track);
    lv_obj_remove_style_all(g_sb_thumb);
    lv_obj_set_style_bg_color(g_sb_thumb, TC(TEXT_PRIMARY), 0);
    lv_obj_set_style_bg_opa(g_sb_thumb, LV_OPA_90, 0);
    lv_obj_set_style_radius(g_sb_thumb, 4, 0);
    lv_obj_clear_flag(g_sb_thumb, LV_OBJ_FLAG_SCROLLABLE);

    g_counter = lv_label_create(g_root);
    lv_obj_set_style_text_font(g_counter, TF(UI_14), 0); lv_obj_set_style_text_color(g_counter, TC(TEXT_DISABLED), 0);
    if(g_nalb <= 1){
        lv_obj_add_flag(g_sb_track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(g_counter, LV_ALIGN_TOP_MID, 0, 318);
    } else {
        lv_obj_clear_flag(g_sb_track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(g_counter, LV_ALIGN_TOP_MID, 0, 324);
        aw_update_scrollbar();
    }

    /* "A-Z" button (bottom-right) opens the alphabet grid */
    g_az_btn = lv_button_create(g_root);
    lv_obj_remove_style_all(g_az_btn);
    lv_obj_set_pos(g_az_btn, 302, 158); lv_obj_set_size(g_az_btn, 44, 44);
    lv_obj_set_style_radius(g_az_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_az_btn, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(g_az_btn, LV_OPA_90, 0);
    ui_on(g_az_btn, aw_az_btn_cb, LV_EVENT_CLICKED, NULL, "albumwall.az_btn", UI_CORE);
    lv_obj_t *azl = lv_label_create(g_az_btn);
    lv_label_set_text(azl, "A-Z");
    lv_obj_set_style_text_font(azl, TF(UI_14), 0);
    lv_obj_set_style_text_color(azl, TC(TEXT_PRIMARY), 0);
    lv_obj_center(azl);
    if(g_nalb <= 12) lv_obj_add_flag(g_az_btn, LV_OBJ_FLAG_HIDDEN);

    /* alphabet hint overlay (rim-scroll / fast scroll position indicator) */
    g_lhint = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_lhint);
    lv_obj_set_size(g_lhint, 96, 96);
    lv_obj_center(g_lhint);
    lv_obj_set_style_radius(g_lhint, 22, 0);
    lv_obj_set_style_bg_color(g_lhint, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(g_lhint, LV_OPA_80, 0);
    lv_obj_clear_flag(g_lhint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_lhint, LV_OBJ_FLAG_HIDDEN);
    g_lhint_lbl = lv_label_create(g_lhint);
    lv_obj_set_style_text_font(g_lhint_lbl, TF(UI_40), 0);
    lv_obj_set_style_text_color(g_lhint_lbl, TC(TEXT_PRIMARY), 0);
    lv_label_set_text(g_lhint_lbl, "A");
    lv_obj_center(g_lhint_lbl);

    /* alphabet grid overlay */
    g_grid = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_grid);
    lv_obj_set_size(g_grid, 360, 360); lv_obj_set_pos(g_grid, 0, 0);
    lv_obj_set_style_bg_color(g_grid, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(g_grid, LV_OPA_80, 0);
    lv_obj_add_flag(g_grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_grid, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(g_grid, aw_grid_bg_cb, LV_EVENT_CLICKED, NULL, "albumwall.grid_bg", UI_CORE);
    lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
    {
        lv_obj_t *gt = lv_label_create(g_grid);
        lv_label_set_text(gt, tr("Jump to"));
        lv_obj_align(gt, LV_ALIGN_TOP_MID, 0, 20);
        lv_obj_set_style_text_font(gt, TF(UI_14), 0);
        lv_obj_set_style_text_color(gt, TC(TEXT_MUTED), 0);
    }
    {
        static const char *AZ = "ABCDEFGHIJKLMNOPQRSTUVWXYZ#";
        int cols = 5, cw = 52, ch = 48, n = 27;
        int gw = cols * cw, x0 = (360 - gw) / 2;
        int rows = (n + cols - 1) / cols;
        int y0 = 48;
        for(int i = 0; i < n; i++){
            int r = i / cols, c = i % cols;
            int cells_in_row = (r == rows - 1) ? (n - r * cols) : cols;
            int row_x0 = x0 + ((cols - cells_in_row) * cw) / 2;
            lv_obj_t *cell = lv_button_create(g_grid);
            lv_obj_remove_style_all(cell);
            lv_obj_set_pos(cell, row_x0 + c * cw, y0 + r * ch);
            lv_obj_set_size(cell, cw - 4, ch - 4);
            lv_obj_set_style_radius(cell, 8, 0);
            lv_obj_set_style_bg_color(cell, TC(ACCENT_PRIMARY), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_STATE_PRESSED);
            lv_obj_set_style_text_color(cell, TC(TEXT_PRIMARY), 0);
            lv_obj_set_style_text_color(cell, TC(ON_ACCENT), LV_STATE_PRESSED);
            ui_on(cell, aw_letter_cb, LV_EVENT_CLICKED, (void *)(intptr_t)AZ[i], "albumwall.letter", UI_CORE);
            lv_obj_t *l = lv_label_create(cell);
            char b[2] = {AZ[i], 0};
            lv_label_set_text(l, b);
            lv_obj_set_style_text_font(l, TF(UI_20), 0);
            lv_obj_center(l);
        }
    }

    aw_render_all(); aw_labels();
}
