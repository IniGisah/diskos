/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* splash.c - the diskOS boot splash: the pressed disc under one light (the website's first act).
 *
 * The screen is the data side of a disc in the dark. One light passes across it once: where it falls, the track
 * pitch splits it into two small spectra, cut into eight tracks by the grooves, with a white streak and a lit rim.
 * The hub (the real screen) warms in the theme's accent and shows "diskOS"; the light leaves, the hub is the only
 * light left, and it opens: the disc is pushed outwards past the edge, the hub's edge becomes the screen's edge, and
 * Home is underneath.
 * 2.4 s, once per boot, on lv_layer_top; it swallows taps and deletes itself.
 *
 * Cost on the GPU-less renderer: plain circles, rings and short arcs moved by one integer timeline; no images, no
 * transforms, no shadows, no masks beyond the renderer's own circle edges. The light only ever changes arc angles,
 * and each spectrum lives in a small box that follows it (its arcs are clipped to that box), so while it moves only
 * those two boxes are repainted; the hub's glow repaints the hub; the whole screen is repainted only while the hub
 * opens, as the old splash did while its Disc grew. */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "ipc.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(diskos_brand_disk_32)   /* the website's wordmark: Archivo, expanded; "disk" semibold, "OS" light */
LV_FONT_DECLARE(diskos_brand_os_32)

#define SP_MS    2400          /* the whole splash */
#define SP_CX    180
#define SP_R     178           /* the disc's rim, just inside the round glass */
#define SP_SLICE 5             /* colours in one spectrum */
#define SP_GAPS  7             /* grooves between the 8 tracks */
#define SP_GLOW  4

/* the disc, in 1/1000 of its radius (the website's phone geometry: hub .46, mirror band .50-.56, data to .985) */
#define K_HUB   460
#define K_B0    500
#define K_B1    560
#define K_LIT0  640            /* the light is faint on the innermost tracks: spectra start here */
#define K_DATA  985

static struct {
    lv_obj_t *root, *face, *rim, *rim_hl, *lobe[2], *slice[2][SP_SLICE], *streak, *shade[2], *gap[2][(SP_GAPS + 1) / 2],
             *glow[SP_GLOW], *hub, *hub_edge, *word;
    int th, warm, opened, frames, rim_closed;
    uint32_t t0, t_outro;
    int state;
    int exit_th;
    lv_timer_t *timer;
} g_sp;

/* 0..1024 fixed point */
static int clampk(int v){ return v < 0 ? 0 : v > 1024 ? 1024 : v; }
static int win_k(int t, int a, int b){ return clampk((t - a) * 1024 / (b - a)); }
static int ease_out(int k){ int r = 1024 - k; return 1024 - (int)((int64_t)r * r * r >> 20); }      /* cubic out */
static int ease_io(int k){                                                                             /* cubic in-out */
    if(k < 512) return (int)(4 * (int64_t)k * k * k >> 20);
    int r = 1024 - k; return 1024 - (int)(4 * (int64_t)r * r * r >> 20);
}
static int scl(int v, int k){ return (int)((int64_t)v * k >> 10); }
static lv_opa_t opa_k(int max, int k){ return (lv_opa_t)(max * clampk(k) >> 10); }

/* set a style value only when it changes: every style write repaints the whole object, and most of these objects
 * are as big as the disc */
static void sty(lv_obj_t *o, lv_style_prop_t p, int32_t v, lv_style_selector_t sel){
    lv_style_value_t cur;
    if(lv_obj_get_local_style_prop(o, p, &cur, sel) == LV_STYLE_RES_FOUND && cur.num == v) return;
    lv_style_value_t nv = { .num = v };
    lv_obj_set_local_style_prop(o, p, nv, sel);
}

/* every splash object is marked styled: it lives on the top layer, where a theme's styling pass would otherwise
 * restyle it as a popup. The splash is a fixed identity in every theme. */
static lv_obj_t *shape(lv_obj_t *parent){
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    kit_keep(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    return o;
}
static lv_obj_t *ring(lv_obj_t *parent, lv_color_t c, lv_opa_t opa){    /* a circle drawn as its border only */
    lv_obj_t *o = shape(parent);
    lv_obj_set_style_border_color(o, c, 0);
    lv_obj_set_style_border_opa(o, opa, 0);
    return o;
}
static lv_obj_t *arc(lv_obj_t *parent, lv_color_t c, lv_opa_t opa){      /* a bare arc: MAIN part only */
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_style_all(a);
    kit_keep(a);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_arc_color(a, c, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, opa, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(a, 0, 1);
    return a;
}
/* a band from radius r0 to r1 (r0 < r1), centred on the screen: a ring object or an arc */
static void place_band(lv_obj_t *o, int r0, int r1, int is_arc){
    if(r1 < 1) r1 = 1;
    if(r0 > r1 - 1) r0 = r1 - 1;
    lv_obj_set_pos(o, SP_CX - r1, SP_CX - r1);
    lv_obj_set_size(o, 2 * r1, 2 * r1);
    sty(o, is_arc ? LV_STYLE_ARC_WIDTH : LV_STYLE_BORDER_WIDTH, r1 - r0, 0);
}
static void place_disc(lv_obj_t *o, int r){                  /* a filled circle of radius r */
    if(r < 1) r = 1;
    lv_obj_set_pos(o, SP_CX - r, SP_CX - r);
    lv_obj_set_size(o, 2 * r, 2 * r);
}
static int ang(int a){ a %= 360; return a < 0 ? a + 360 : a; }
static void span(lv_obj_t *a, int mid, int half){ lv_arc_set_bg_angles(a, ang(mid - half), ang(mid + half)); }
static int gap_r(int i){ return SP_R * (K_B1 + (K_DATA - K_B1) * (i + 1) / 8) / 1000; }   /* groove i of 7 */

/* mark the box around a sector of the disc dirty, so the arcs that move inside it add no dirty areas of their own
 * (the renderer keeps 32; past that it repaints the whole screen) */
static void sector_box(int r0, int r1, int a0, int a1, lv_area_t *box){
    int x1 = 9999, y1 = 9999, x2 = -9999, y2 = -9999;
    for(int a = a0;; a += 5){
        if(a > a1) a = a1;
        int s = lv_trigo_sin((int16_t)ang(a)), c = lv_trigo_cos((int16_t)ang(a));
        for(int k = 0; k < 2; k++){
            int r = k ? r1 : r0, x = SP_CX + (r * c >> 15), y = SP_CX + (r * s >> 15);
            if(x < x1) x1 = x;
            if(x > x2) x2 = x;
            if(y < y1) y1 = y;
            if(y > y2) y2 = y;
        }
        if(a == a1) break;
    }
    box->x1 = x1 - 4; box->y1 = y1 - 4; box->x2 = x2 + 4; box->y2 = y2 + 4;
}
static void inv_sector(int r0, int r1, int a0, int a1){
    lv_area_t box;
    sector_box(r0, r1, a0, a1, &box);
    lv_obj_invalidate_area(g_sp.root, &box);
}

/* Render one visual state of the splash:
 * th: angle in degrees
 * sp: spectrum spread 0..1024
 * warm: hub warmth 0..1024
 * wm: wordmark opacity 0..1024
 * ok: hub opening progress 0..1024 */
static void splash_draw(int th, int sp, int warm, int wm, int ok){
    int S = 1024 + scl((SP_CX + 10) * 1024 / (SP_R * K_HUB / 1000) - 1024, ok);   /* scale, 1024 = 1 */
    int R = scl(SP_R, S);
#define RK(k) (R * (k) / 1000)

    if(ok > 0 && !g_sp.opened){
        /* the light has gone: its objects leave the draw list, and from here the hub must be a hole onto Home: the
         * root stops painting black and the disc face (the same black) is drawn as a ring instead. Nothing on
         * screen changes in this frame. */
        g_sp.opened = 1;
        lv_obj_t *gone[] = { g_sp.rim_hl, g_sp.streak };
        for(unsigned i = 0; i < sizeof gone / sizeof *gone; i++) lv_obj_add_flag(gone[i], LV_OBJ_FLAG_HIDDEN);
        for(int s = 0; s < 2; s++) lv_obj_add_flag(g_sp.lobe[s], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(g_sp.root, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(g_sp.face, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_sp.opened) place_band(g_sp.face, RK(K_HUB), R + 2, 0);

    /* the rim: drawn out from the light's first touch until it closes, then it stays */
    place_band(g_sp.rim, RK(K_DATA) + 1, R, 1);
    if(g_sp.rim_closed || sp >= 1024){
        g_sp.rim_closed = 1;
        lv_arc_set_bg_angles(g_sp.rim, 0, 360);
    } else {
        int rim_half = scl(180, sp);
        if(rim_half >= 180){ g_sp.rim_closed = 1; lv_arc_set_bg_angles(g_sp.rim, 0, 360); }
        else span(g_sp.rim, 175, rim_half);
    }

    if(!g_sp.opened){
        int r0 = RK(K_LIT0) - 8;
        if(g_sp.th < 0 || abs(th - g_sp.th) > 60){
            inv_sector(r0, R, th - 24, th + 24);
            inv_sector(r0, R, th + 180 - 24, th + 180 + 24);
            if(g_sp.th >= 0){
                inv_sector(r0, R, g_sp.th - 24, g_sp.th + 24);
                inv_sector(r0, R, g_sp.th + 180 - 24, g_sp.th + 180 + 24);
            }
        } else {
            int lo = LV_MIN(th, g_sp.th) - 24, hi = LV_MAX(th, g_sp.th) + 24;
            inv_sector(r0, R, lo, hi);
            inv_sector(r0, R, lo + 180, hi + 180);
        }
        g_sp.th = th;
        place_band(g_sp.rim_hl, RK(K_DATA) + 1, R, 1);
        span(g_sp.rim_hl, th, scl(22, sp));                  /* inside the spectrum's box */
        int half = sp > 0 ? LV_MAX(1, scl(7, sp)) : 0;   /* neighbours overlap by half: the colours blend */
        for(int s = 0; s < 2; s++){
            /* each spectrum lives in a box that follows it: its arcs and grooves are drawn only inside that box */
            int axis = th + 180 * s;
            lv_area_t box;
            sector_box(r0, R, axis - 24, axis + 24, &box);
            lv_obj_set_pos(g_sp.lobe[s], box.x1, box.y1);
            lv_obj_set_size(g_sp.lobe[s], lv_area_get_width(&box), lv_area_get_height(&box));
            int ox = box.x1, oy = box.y1;
            for(int i = 0; i < SP_SLICE; i++){           /* violet to red across the light, mirrored on the far side */
                lv_obj_t *a = g_sp.slice[s][i];
                place_band(a, RK(K_LIT0), RK(K_DATA), 1);
                lv_obj_set_pos(a, SP_CX - RK(K_DATA) - ox, SP_CX - RK(K_DATA) - oy);
                span(a, axis + scl((2 * i - (SP_SLICE - 1)) * 7 / 2 * (s ? -1 : 1), sp), half);
            }
            int rs = RK(K_LIT0 + (K_DATA - K_LIT0) * 45 / 100);
            place_band(g_sp.shade[s], RK(K_LIT0), rs, 1);
            lv_obj_set_pos(g_sp.shade[s], SP_CX - rs - ox, SP_CX - rs - oy);
            span(g_sp.shade[s], axis, half > 0 ? 24 : 0);
            for(int k = 0; k < (SP_GAPS + 1) / 2; k++){  /* the grooves: two per object, its border and its outline */
                int g = gap_r(2 * k);
                lv_obj_set_pos(g_sp.gap[s][k], SP_CX - g - 1 - ox, SP_CX - g - 1 - oy);
            }
            if(s == 0){
                place_band(g_sp.streak, RK(K_LIT0), RK(K_DATA), 1);
                lv_obj_set_pos(g_sp.streak, SP_CX - RK(K_DATA) - ox, SP_CX - RK(K_DATA) - oy);
                span(g_sp.streak, th, sp > 0 ? 1 : 0);
            }
        }
    }

    /* the hub warms in the accent */
    int hr = RK(K_HUB);
    static const int GW[SP_GLOW] = { 4, 10, 18, 28 };  /* outer radius of each glow step past the hub's edge */
    static const int GO[SP_GLOW] = { 54, 30, 15, 6 };
    if(!g_sp.opened && warm != g_sp.warm){
        lv_area_t gb = { SP_CX - hr - GW[SP_GLOW - 1] - 1, SP_CX - hr - GW[SP_GLOW - 1] - 1,
                         SP_CX + hr + GW[SP_GLOW - 1] + 1, SP_CX + hr + GW[SP_GLOW - 1] + 1 };
        lv_obj_invalidate_area(g_sp.root, &gb);
    }
    g_sp.warm = warm;
    for(int i = 0; i < SP_GLOW; i++){
        place_band(g_sp.glow[i], hr, hr + scl(GW[i], S), 0);
        sty(g_sp.glow[i], LV_STYLE_BORDER_OPA, opa_k(GO[i], warm), 0);
    }
    place_disc(g_sp.hub, hr);
    sty(g_sp.hub, LV_STYLE_BG_OPA, 255 - scl(255, ok), 0);
    place_band(g_sp.hub_edge, hr - 1, hr, 0);
    sty(g_sp.hub_edge, LV_STYLE_BORDER_OPA, opa_k(36, scl(warm, 1024 - ok)), 0);

    /* the name, in the hub: rises into place, then dissolves on outro */
    sty(g_sp.word, LV_STYLE_OPA, opa_k(255, wm), 0);
    lv_obj_set_y(g_sp.word, SP_CX - lv_obj_get_height(g_sp.word) / 2 + 2);
#undef RK
}

static void splash_drawn_cb(lv_event_t *e){ (void)e; g_sp.frames++; }   /* a frame actually drawn to the screen */

static void splash_done(void){
    if(g_sp.timer){
        lv_timer_delete(g_sp.timer);
        g_sp.timer = NULL;
    }
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(), splash_drawn_cb, NULL);
    fprintf(stderr, "splash: %d frames drawn in %u ms\n", g_sp.frames, (unsigned)lv_tick_elaps(g_sp.t0));
    if(g_sp.root) lv_obj_delete(g_sp.root);
    memset(&g_sp, 0, sizeof g_sp);
}

int splash_active(void){
    return g_sp.root != NULL;
}

static void splash_timer_cb(lv_timer_t *t){
    (void)t;
    if(!g_sp.root) return;
    uint32_t now = lv_tick_get();
    uint32_t elapsed = lv_tick_elaps(g_sp.t0);

    /* Phase 0: Intro (0..1500 ms) - sweep light in, warm hub, fade in wordmark */
    if(g_sp.state == 0){
        if(elapsed < 1500){
            int cur_t = (int)elapsed;
            int th = 175 + scl(110, ease_io(win_k(cur_t, 150, 1500)));
            int sp = ease_out(win_k(cur_t, 150, 650));
            int warm = ease_out(win_k(cur_t, 650, 1300));
            int wm = ease_out(win_k(cur_t, 950, 1400));
            splash_draw(th, sp, warm, wm, 0);
            return;
        }
        g_sp.state = 1;
    }

    /* Phase 1: Wait / Loop - loop light orbit until player is ready (with 12s backstop) */
    if(g_sp.state == 1){
        int ready = (ipc_player_ready() && ipc_rx_frames() > 0) || elapsed >= 12000;
        if(!ready){
            int loop_ms = (int)(elapsed - 1500);
            int th = 285 + (loop_ms * 360 / 2400);
            splash_draw(th, 1024, 1024, 1024, 0);
            return;
        }
        g_sp.state = 2;
        g_sp.t_outro = now;
        int loop_ms = (int)(elapsed - 1500);
        g_sp.exit_th = 285 + (loop_ms * 360 / 2400);
    }

    /* Phase 2: Outro (0..600 ms) - light dissolves, wordmark fades, hub expands to reveal Home */
    if(g_sp.state == 2){
        uint32_t dt = lv_tick_elaps(g_sp.t_outro);
        if(dt < 600){
            int light_leave = ease_io(win_k((int)dt, 0, 250));
            int sp = scl(1024, 1024 - light_leave);
            int th = g_sp.exit_th + (int)(dt * 360 / 2400);
            int wm = scl(1024, 1024 - ease_out(win_k((int)dt, 0, 200)));
            int warm = scl(1024, 1024 - win_k((int)dt, 100, 400));
            int ok = ease_io(win_k((int)dt, 150, 550));
            splash_draw(th, sp, warm, wm, ok);
            return;
        }
        splash_done();
    }
}

void splash_start(void){
    if(g_sp.root) return;                                           /* one splash at a time */
    static const theme_color_role_t BAND[SP_SLICE] = { THEME_CLR_FIXED_SPLASH_BAND_5, THEME_CLR_FIXED_SPLASH_BAND_4,
        THEME_CLR_FIXED_SPLASH_BAND_3, THEME_CLR_FIXED_SPLASH_BAND_2, THEME_CLR_FIXED_SPLASH_BAND_1 };   /* violet..red */
    static const lv_opa_t BAND_OPA[SP_SLICE] = { 76, 104, 96, 104, 84 };   /* softer at the spectrum's ends */
    lv_color_t accent = TC(ACCENT_PRIMARY);                         /* the light of the screen the hub opens onto */

    g_sp.root = shape(lv_layer_top());
    lv_obj_set_style_radius(g_sp.root, 0, 0);
    lv_obj_set_size(g_sp.root, 360, 360);
    lv_obj_set_style_bg_color(g_sp.root, TC(FIXED_SPLASH_BG), 0);   /* the disc's face, in the dark */
    lv_obj_set_style_bg_opa(g_sp.root, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_sp.root, LV_OBJ_FLAG_CLICKABLE);             /* swallow taps during the splash */

    g_sp.face = ring(g_sp.root, TC(FIXED_SPLASH_BG), LV_OPA_COVER); /* the same face, as a ring, once the hub opens */
    lv_obj_add_flag(g_sp.face, LV_OBJ_FLAG_HIDDEN);
    for(int s = 0; s < 2; s++){
        /* a spectrum: a box that follows the light (its children are clipped to it), holding five colour slices,
         * the white streak (near side only) and the grooves that cut the light into eight tracks */
        g_sp.lobe[s] = shape(g_sp.root);
        lv_obj_set_style_radius(g_sp.lobe[s], 0, 0);
        for(int i = 0; i < SP_SLICE; i++)
            g_sp.slice[s][i] = arc(g_sp.lobe[s], theme_color(BAND[i]), (lv_opa_t)(BAND_OPA[i] * (s ? 2 : 3) / 3));
        if(s == 0) g_sp.streak = arc(g_sp.lobe[s], TC(FIXED_SPLASH_3), 70);
        g_sp.shade[s] = arc(g_sp.lobe[s], TC(FIXED_SPLASH_BG), 130);  /* the light fades towards the hub */
        for(int k = 0; k < (SP_GAPS + 1) / 2; k++){
            int g = gap_r(2 * k);                                   /* border: groove 2k, outline: groove 2k+1 */
            lv_obj_t *o = ring(g_sp.lobe[s], TC(FIXED_SPLASH_HOLE), 130);
            lv_obj_set_size(o, 2 * (g + 1), 2 * (g + 1));
            lv_obj_set_style_border_width(o, 2, 0);
            if(2 * k + 1 < SP_GAPS){
                lv_obj_set_style_outline_color(o, TC(FIXED_SPLASH_HOLE), 0);
                lv_obj_set_style_outline_opa(o, 130, 0);
                lv_obj_set_style_outline_width(o, 2, 0);
                lv_obj_set_style_outline_pad(o, gap_r(2 * k + 1) - 1 - (g + 1), 0);
            }
            g_sp.gap[s][k] = o;
        }
    }
    g_sp.rim = arc(g_sp.root, TC(FIXED_SPLASH_SPINDLE), LV_OPA_COVER);
    g_sp.rim_hl = arc(g_sp.root, TC(FIXED_SPLASH_HUB_LINE), 150);   /* the rim where the light falls on it */
    for(int i = SP_GLOW - 1; i >= 0; i--) g_sp.glow[i] = ring(g_sp.root, accent, 0);   /* widest first */
    g_sp.hub = shape(g_sp.root);                                    /* the real screen, still dark: as black as */
    lv_obj_set_style_bg_color(g_sp.hub, TC(FIXED_SPLASH_BG), 0);    /* the face, so the first frames are one black */
    lv_obj_set_style_bg_opa(g_sp.hub, LV_OPA_COVER, 0);
    g_sp.hub_edge = ring(g_sp.root, TC(FIXED_SPLASH_3), 0);

    /* "diskOS", as on the website: a semibold "disk", a light "OS", expanded */
    g_sp.word = shape(g_sp.root);
    lv_obj_set_style_radius(g_sp.word, 0, 0);
    lv_obj_set_size(g_sp.word, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_sp.word, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_sp.word, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    const lv_font_t *F[2] = { &diskos_brand_disk_32, &diskos_brand_os_32 };
    const char *T[2] = { "disk", "OS" };
    for(int i = 0; i < 2; i++){
        lv_obj_t *l = lv_label_create(g_sp.word);
        kit_keep(l);
        lv_label_set_text(l, T[i]);
        lv_obj_set_style_text_font(l, F[i], 0);
        lv_obj_set_style_text_letter_space(l, -1, 0);                /* the website's -0.02 em */
        lv_obj_set_style_text_color(l, TC(FIXED_SPLASH_3), 0);
    }
    lv_obj_update_layout(g_sp.word);
    lv_obj_set_x(g_sp.word, SP_CX - lv_obj_get_width(g_sp.word) / 2);

    g_sp.th = -1;
    g_sp.t0 = lv_tick_get();
    g_sp.state = 0;
    g_sp.rim_closed = 0;
    lv_display_add_event_cb(lv_display_get_default(), splash_drawn_cb, LV_EVENT_RENDER_READY, NULL);
    splash_draw(175, 0, 0, 0, 0);
    g_sp.timer = lv_timer_create(splash_timer_cb, 16, NULL);
}
