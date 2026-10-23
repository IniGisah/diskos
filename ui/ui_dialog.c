/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* ui_dialog.c - shared dialog shell: content-sized panel, circle-capped, overflow scrolls inside the viewport only. */
#include "ui_dialog.h"
#include "theme.h"
#include "theme_kit.h"
#include "lvgl/src/core/lv_obj_draw_private.h"   /* lv_obj_get_ext_draw_size: a theme's external drawing (Stone's edge) */
#include <stdlib.h>
#include <string.h>

#define SCREEN_R   180
#define PAD_V      14      /* panel padding above/below the parts */
#define PAD_H      20
#define GAP        12      /* viewport to actions */
#define ID_MAGIC   0x44470000u   /* tags our own objects through user_data */
#define COL_GAP    8       /* between action buttons */

/* every live dialog, for the dirty-flag pass. `fp` = the quantities the last measurement came from. */
typedef struct { int32_t pw, ch, ah, pt, pb, gap, bw, ext, avail, need, hidden; } fp_t;
typedef struct dlg { ui_dialog_t d; fp_t fp; int dirty; struct dlg *next; } dlg_t;
static dlg_t *g_dlgs;
static int g_busy;           /* finalize is running: nothing re-enters it */
static uint32_t g_measures;

static int32_t isqrt32(int64_t v)   /* floor(sqrt(v)) for v >= 0, integers only */
{
    if(v <= 0) return 0;
    int64_t x = v, y = (x + 1) / 2;
    while(y < x){ x = y; y = (x + v / x) / 2; }
    return (int32_t)x;
}

int32_t ui_dialog_circle_cap(int32_t width, int32_t margin)
{
    if(margin < 4) margin = 4;
    if(width <= 0 || margin >= SCREEN_R) return 0;
    int64_t r = SCREEN_R - margin;
    if((int64_t)width >= 2 * r) return 0;
    int64_t v = 4 * r * r - (int64_t)width * width;     /* floor(2*sqrt(r^2-(w/2)^2)) == isqrt(4r^2-w^2) */
    return v <= 0 ? 0 : isqrt32(v);
}

/* inflate by `ext` on every side and keep the corners inside the circle: Stone's edge is drawn outside the panel */
static int32_t cap_with_ext(int32_t width, int32_t margin, int32_t ext)
{
    int32_t cap = ui_dialog_circle_cap(width, margin);
    if(ext > 0 && ext < SCREEN_R){
        int64_t w2 = (int64_t)width + 2 * ext;
        int32_t c2 = w2 >= 2 * SCREEN_R ? 0 : isqrt32(4 * (int64_t)SCREEN_R * SCREEN_R - w2 * w2) - 2 * ext;
        if(c2 < 0) c2 = 0;
        if(c2 < cap) cap = c2;
    }
    return cap;
}

static void tag(lv_obj_t *o, ui_dialog_part_t p){ lv_obj_set_user_data(o, (void *)(uintptr_t)(ID_MAGIC | (uint32_t)p)); }

int ui_dialog_part_of(const lv_obj_t *o)
{
    uintptr_t u = (uintptr_t)lv_obj_get_user_data((lv_obj_t *)o);
    /* the exact tag values only: the kit asks this of every object, and user_data elsewhere holds real pointers */
    return (u > ID_MAGIC && u <= ID_MAGIC + UI_DLG_ACTIONS) ? (int)(u - ID_MAGIC) : UI_DLG_NONE;
}

static void forget_cb(lv_event_t *e)
{
    dlg_t *gone = lv_event_get_user_data(e);
    for(dlg_t **p = &g_dlgs; *p; p = &(*p)->next)
        if(*p == gone){ *p = gone->next; free(gone); return; }
}

static lv_obj_t *part(lv_obj_t *parent, ui_dialog_part_t p)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    tag(o, p);
    return o;
}

ui_dialog_t ui_dialog_create(lv_obj_t *parent, const ui_dialog_cfg_t *cfg)
{
    ui_dialog_t d = {0};
    d.width = cfg->width;
    d.margin = cfg->margin < 4 ? 4 : cfg->margin;
    d.fit_width = cfg->fit_width ? cfg->width : 0;
    lv_obj_t *host = parent;
    if(cfg->scrim){
        d.scrim = part(parent, UI_DLG_SCRIM);
        lv_obj_set_size(d.scrim, 360, 360); lv_obj_center(d.scrim);
        lv_obj_set_style_bg_color(d.scrim, TC(SCRIM), 0);
        lv_obj_set_style_bg_opa(d.scrim, LV_OPA_70, 0);
        lv_obj_clear_flag(d.scrim, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(d.scrim, LV_OBJ_FLAG_CLICKABLE);          /* absorb taps */
        host = d.scrim;
    }
    d.panel = part(host, UI_DLG_PANEL);
    lv_obj_set_size(d.panel, d.width, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(d.panel, 18, 0);
    lv_obj_set_style_bg_color(d.panel, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(d.panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(d.panel, PAD_H, 0);
    lv_obj_set_style_pad_ver(d.panel, PAD_V, 0);
    lv_obj_set_style_pad_row(d.panel, GAP, 0);
    lv_obj_clear_flag(d.panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(d.panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(d.panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    d.viewport = part(d.panel, UI_DLG_VIEWPORT);
    lv_obj_set_size(d.viewport, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_add_flag(d.viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(d.viewport, LV_DIR_VER);
    lv_obj_clear_flag(d.viewport, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scrollbar_mode(d.viewport, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(d.viewport, TC(TEXT_SECONDARY),
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_width(d.viewport, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(d.viewport, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_border_width(d.viewport, 0, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(d.viewport, 0, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(d.viewport, 0, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_top(d.viewport, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_bottom(d.viewport, 2, LV_PART_SCROLLBAR);

    d.content = part(d.viewport, UI_DLG_CONTENT);
    lv_obj_set_size(d.content, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(d.content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(d.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(d.content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(d.content, 8, 0);

    if(cfg->actions){
        d.actions = part(d.panel, UI_DLG_ACTIONS);
        lv_obj_set_size(d.actions, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_clear_flag(d.actions, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(d.actions, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(d.actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(d.actions, COL_GAP, 0);
        lv_obj_set_style_pad_row(d.actions, COL_GAP, 0);
    }
    dlg_t *e = calloc(1, sizeof *e);
    if(e){
        e->d = d; e->dirty = 1; e->next = g_dlgs; g_dlgs = e;
        lv_obj_add_event_cb(d.panel, forget_cb, LV_EVENT_DELETE, e);
    }
    return d;
}

lv_obj_t *ui_dialog_button(const ui_dialog_t *d, const char *text, theme_color_role_t col, lv_event_cb_t cb,
                           const char *action)
{
    lv_obj_t *b = lv_button_create(d->actions);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 108, 40);
    lv_obj_set_ext_click_area(b, 4);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    if(cb) ui_on(b, cb, LV_EVENT_CLICKED, NULL, action, UI_CORE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, TF(UI_16), 0);
    lv_obj_set_style_text_color(l, theme_color(col), 0);
    lv_obj_center(l);
    return b;
}

lv_obj_t *ui_dialog_label(const ui_dialog_t *d, const char *text)
{
    lv_obj_t *l = lv_label_create(d->content);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

/* a flex change under a content-sized parent can need a second layout pass before the sizes are stable */
static void layout_settle(lv_obj_t *o)
{
    int32_t h = -1;
    for(int i = 0; i < 3; i++){
        lv_obj_update_layout(o);
        int32_t n = lv_obj_get_height(o);
        if(n == h) break;
        h = n;
    }
}

/* what the actions row needs side by side vs what it has */
static void actions_room(const ui_dialog_t *d, int32_t *need, int32_t *avail, int *n)
{
    *need = 0; *avail = 0; *n = 0;
    if(!d->actions) return;
    lv_obj_update_layout(d->panel);
    for(uint32_t i = 0; i < lv_obj_get_child_count(d->actions); i++){
        lv_obj_t *b = lv_obj_get_child(d->actions, (int32_t)i);
        if(lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN)) continue;
        *need += lv_obj_get_width(b)
               + lv_obj_get_style_margin_left(b, LV_PART_MAIN)
               + lv_obj_get_style_margin_right(b, LV_PART_MAIN)
               + (*n ? lv_obj_get_style_pad_column(d->actions, LV_PART_MAIN) : 0);
        (*n)++;
    }
    *avail = lv_obj_get_content_width(d->actions);
}

static void fp_read(const ui_dialog_t *d, fp_t *f)
{
    int n;
    memset(f, 0, sizeof *f);
    f->hidden = lv_obj_has_flag(d->viewport, LV_OBJ_FLAG_HIDDEN)
              | (lv_obj_has_flag(d->content, LV_OBJ_FLAG_HIDDEN) << 1)
              | ((d->actions && lv_obj_has_flag(d->actions, LV_OBJ_FLAG_HIDDEN)) << 2);
    lv_obj_update_layout(d->panel);
    f->pw = lv_obj_get_width(d->panel);
    f->ch = lv_obj_get_height(d->content);
    f->ah = d->actions ? lv_obj_get_height(d->actions) : 0;
    f->pt = lv_obj_get_style_pad_top(d->panel, 0); f->pb = lv_obj_get_style_pad_bottom(d->panel, 0);
    f->gap = lv_obj_get_style_pad_row(d->panel, 0); f->bw = lv_obj_get_style_border_width(d->panel, 0);
    f->ext = lv_obj_get_ext_draw_size(d->panel);
    actions_room(d, &f->need, &f->avail, &n);
}

/* the padding tiers of a fit_width dialog: the standard look first, a tighter one only when the text does not fit
 * otherwise. The scroll gutter exists only while the body really overflows. */
typedef struct { int32_t padh, padv, gap, gutter; } tier_t;
static void tier_set(ui_dialog_t *d, const tier_t *t)
{
    lv_obj_set_style_pad_hor(d->panel, t->padh, 0);
    lv_obj_set_style_pad_ver(d->panel, t->padv, 0);
    lv_obj_set_style_pad_row(d->panel, t->gap, 0);
    lv_obj_set_style_pad_right(d->viewport, t->gutter, 0);
}

/* widest width (<= fit_width) whose circle cap holds all of the content in this tier; else the width with the least
 * overflow. Never shrinks controls or turns a horizontal button pair into a stack. Returns that overflow in px. */
static int32_t search_width(ui_dialog_t *d, const tier_t *t, int32_t *best_w)
{
    int32_t least_overflow = INT32_MAX;
    tier_set(d, t);
    *best_w = lv_obj_get_width(d->panel);

    for(int32_t w = d->fit_width & ~1; w >= 2; w -= 2){
        lv_obj_set_width(d->panel, w);
        lv_obj_set_height(d->panel, LV_SIZE_CONTENT);
        lv_obj_set_height(d->viewport, LV_SIZE_CONTENT);
        layout_settle(d->panel);

        int32_t need, avail;
        int n;
        actions_room(d, &need, &avail, &n);
        if(need > avail) break;

        int32_t cap = cap_with_ext(w, d->margin, lv_obj_get_ext_draw_size(d->panel)) & ~1;
        if(cap <= 0) continue;

        int32_t overflow = lv_obj_get_height(d->panel) - cap;
        if(overflow < 0) overflow = 0;
        if(overflow < least_overflow){
            least_overflow = overflow;
            *best_w = w;
        }
        if(!overflow) break;     /* descending widths: widest complete fit */
    }
    return least_overflow;
}

static void choose_width(ui_dialog_t *d)
{
    if(!d->fit_width){
        /* Measure without a gutter on every finalize, including after
         * shortening text that previously needed scrolling. */
        lv_obj_set_style_pad_right(d->viewport, 0, 0);
        lv_obj_set_height(d->panel, LV_SIZE_CONTENT);
        lv_obj_set_height(d->viewport, LV_SIZE_CONTENT);
        layout_settle(d->panel);

        int32_t cap = cap_with_ext(
            d->width, d->margin,
            lv_obj_get_ext_draw_size(d->panel)) & ~1;
        if(lv_obj_get_height(d->panel) > cap)
            lv_obj_set_style_pad_right(d->viewport, 6, 0);
        return;
    }

    static const tier_t standard = { PAD_H, PAD_V, GAP, 0 };
    static const tier_t compact = { 14, 10, 8, 0 };
    static const tier_t scrolling = { 14, 10, 8, 6 };
    const tier_t *use = &standard;
    int32_t w = 0;

    if(search_width(d, &standard, &w) > 0){
        use = &compact;
        if(search_width(d, &compact, &w) > 0){
            use = &scrolling;
            search_width(d, &scrolling, &w);
        }
    }

    tier_set(d, use);
    lv_obj_set_width(d->panel, w);
    d->width = w;
}

void ui_dialog_finalize(ui_dialog_t *d)
{
    if(g_busy) return;
    g_busy = 1;
    /* 0. final theme first: classification by identity, font substitution and fallback, borders, the theme's external
     * drawing. Styled objects are skipped, so this is cheap when nothing new was added. */
    kit_pass(d->scrim ? d->scrim : d->panel);
    lv_obj_update_layout(d->panel);
    d->width = lv_obj_get_width(d->panel);   /* the caller may have changed it */

    /* 1. two buttons that do not fit side by side stack in the same order (no target shrinks) */
    int32_t need, avail; int n;
    actions_room(d, &need, &avail, &n);
    if(d->actions){
        int stack = n > 1 && need > avail;
        lv_obj_set_flex_flow(d->actions, stack ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(d->actions, stack ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
    }

    /* 2. natural size: a scrolled viewport under-reports its content, so measure from the top and restore later */
    g_measures++;
    int32_t keep = lv_obj_get_scroll_y(d->viewport);
    if(keep) lv_obj_scroll_to_y(d->viewport, 0, LV_ANIM_OFF);
    choose_width(d);
    lv_obj_set_height(d->panel, LV_SIZE_CONTENT);
    lv_obj_set_height(d->viewport, LV_SIZE_CONTENT);
    layout_settle(d->panel);
    int32_t natural = lv_obj_get_height(d->panel);
    int32_t vnat = lv_obj_get_height(d->viewport);
    int32_t cap = cap_with_ext(d->width, d->margin, lv_obj_get_ext_draw_size(d->panel)) & ~1;   /* even: a centred odd height sticks out 1/2 px */

    /* 3. H = min(natural, cap); the overflow comes out of the viewport alone, the actions stay whole */
    if(natural > cap){
        int32_t vh = vnat - (natural - cap);
        lv_obj_set_height(d->viewport, vh < 0 ? 0 : vh);
        lv_obj_set_height(d->panel, cap);
    }
    else lv_obj_set_height(d->panel, (natural + 1) & ~1);   /* freeze an even height, still <= cap */
    layout_settle(d->panel);

    /* 4. a shorter text must not leave the scroller parked past its end */
    int32_t room = lv_obj_get_height(d->content) - lv_obj_get_height(d->viewport);
    if(room < 0) room = 0;
    if(keep > room) keep = room;
    if(keep) lv_obj_scroll_to_y(d->viewport, keep, LV_ANIM_OFF);

    /* 5. centre only once the height is final, and remember what this measurement came from */
    lv_obj_center(d->panel);
    lv_obj_update_layout(d->panel);     /* settle the final coordinates for the caller */
    for(dlg_t *e = g_dlgs; e; e = e->next)
        if(e->d.panel == d->panel){ e->dirty = 0; fp_read(d, &e->fp); break; }
    g_busy = 0;
}

void ui_dialog_pass(void)
{
    if(g_busy) return;
    for(dlg_t *e = g_dlgs; e; e = e->next){
        if(!e->dirty){
            fp_t f; fp_read(&e->d, &f);
            if(!memcmp(&f, &e->fp, sizeof f)) continue;
        }
        ui_dialog_finalize(&e->d);
    }
}

void ui_dialog_restyled(lv_obj_t *root)
{
    for(dlg_t *e = g_dlgs; e; e = e->next)
        for(lv_obj_t *o = e->d.panel; o; o = lv_obj_get_parent(o))
            if(o == root){ e->dirty = 1; break; }
}

#ifdef DISKOS_HOST
uint32_t ui_dialog_measures(void){ return g_measures; }
#endif
