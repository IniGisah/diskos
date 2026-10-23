/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "custom_saver_overlay.h"
#include "theme.h"
#include "theme_kit.h"
#include "i18n.h"

/* SCR_CUSTOM_PREVIEW (slice 17d): the Custom page's Preview. It draws the complete result with the REAL lock-screen builder
 * (saver.c saver_custom_preview_start: the saved image in the saver's own fit, the overlay group and feed) using the DRAFT as the
 * model, so what shows is what Save would store. It never begins the draft explicitly (reading it begins one only if none exists)
 * and never changes it: Back returns to it, and screenmgr.c drops it on any other exit (custom_area).
 *
 * Samples and pills exist only here: build() turns the overlay feed's preview mode on (custom_ov_feed_preview), so an empty Now Playing
 * or Weather overlay shows a labelled sample; the lock screen never sets it and stop() always clears it. One pill at a time, bottom
 * centre: "Sample data" (wins: it labels invented content), else "Weather is off", else (no usable image: the Minimal fallback lives in
 * the saver's own root and cannot be reused without a copy) "No image saved" over black.
 *
 * STOP: the GIF object (its destructor closes the decoder and the file and deletes its timer), the overlay group, the decode-cache
 * entry and the weather hold are deleted, not hidden, on Back, swipe, Home, Quick Settings, the saver (custom_preview_leave from
 * screenmgr.c transition) and on a dark panel (custom_preview_dark from saver_dark). A dark panel leaves this screen current and
 * empty; the first lit draw builds it again. Weather requests stay held for the whole life of the composition. */
#define PV_BLACK 0x000000u   /* a picture is the background: black and white whatever the theme, as the lock screen */
#define PV_WHITE 0xFFFFFFu
int ui_screen_backlight_on(void);   /* main.c: 0 while the panel is dark */

static lv_obj_t *s_root, *s_back, *s_pill;
enum { PILL_NONE, PILL_SAMPLE, PILL_WX_OFF, PILL_NO_IMAGE };
static int s_kind;
static int s_dark;   /* 1 = stopped by a dark panel, rebuild on the first lit draw; 2 = that rebuild is queued */

static void pill(int kind)   /* one pill (24 px, bottom centre, over the group's bottom edge y292); same kind = nothing to do */
{
    if(kind == s_kind) return;
    s_kind = kind;
    if(s_pill){ lv_obj_delete(s_pill); s_pill = NULL; }
    if(!kind || !s_root) return;
    lv_obj_t *p = s_pill = lv_obj_create(s_root);
    lv_obj_remove_style_all(p);
    kit_keep(p);
    lv_obj_clear_flag(p, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(p, LV_SIZE_CONTENT, 24);
    lv_obj_set_style_pad_hor(p, 12, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_bg_color(p, theme_color_from_rgb(PV_BLACK), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_50, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, theme_color_from_rgb(PV_WHITE), 0);
    lv_obj_set_style_border_opa(p, LV_OPA_30, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *t = lv_label_create(p);
    kit_keep(t);
    lv_label_set_text(t, tr(kind == PILL_SAMPLE ? "Sample data" : kind == PILL_WX_OFF ? "Weather is off" : "No image saved"));
    lv_obj_set_style_text_font(t, TF(UI_14), 0);
    lv_obj_set_style_text_color(t, theme_color_from_rgb(PV_WHITE), 0);
    lv_obj_align(p, LV_ALIGN_TOP_MID, 0, 304);
}

static void note(void)   /* the pill for what the composition shows now; only overlays the draft enables count */
{
    unsigned n = custom_ov_feed_notes(), m = custom_ov_draft_get().mask;
    pill(((n & CUSTOM_OV_NOTE_SAMPLE_TRACK) && (m & CUSTOM_OV_TRACK)) || ((n & CUSTOM_OV_NOTE_SAMPLE_WEATHER) && (m & CUSTOM_OV_WEATHER)) ? PILL_SAMPLE :
         (n & CUSTOM_OV_NOTE_WEATHER_OFF) && (m & CUSTOM_OV_WEATHER) ? PILL_WX_OFF : PILL_NONE);
}

static void stop(void)   /* the feed's preview mode ends with the composition: the lock screen never sees a sample */
{
    saver_custom_preview_stop();
    custom_ov_feed_preview(NULL);
    pill(PILL_NONE);
}

static void build(void)
{
    if(!s_root) return;
    if(!ui_screen_backlight_on()){ s_dark = 1; return; }
    s_dark = 0;
    custom_ov_feed_preview(note);
    if(saver_custom_preview_start(s_root)) note();
    else{ custom_ov_feed_preview(NULL); pill(saver_custom_preview_on() ? PILL_NONE : PILL_NO_IMAGE); }   /* black base + the pill */
    if(s_back) lv_obj_move_foreground(s_back);   /* the container was added last: Back stays on top */
}

static void rebuild_async(void *u)
{
    (void)u;
    if(s_dark == 2 && screen_current() == SCR_CUSTOM_PREVIEW && ui_screen_backlight_on() && !saver_custom_preview_on()) build();
}

static void root_ev(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if(c == LV_EVENT_DELETE){ stop(); s_root = s_back = s_pill = NULL; s_kind = s_dark = 0; }
    else if(c == LV_EVENT_DRAW_MAIN_BEGIN && s_dark == 1 && ui_screen_backlight_on() && screen_current() == SCR_CUSTOM_PREVIEW){
        s_dark = 2;   /* no widget is touched while drawing */
        lv_async_call(rebuild_async, NULL);
    }
}

static void back_cb(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_CLICKED) screen_back(); }

void custom_preview_create(lv_obj_t *root)
{
    s_root = root;
    kit_isolate(root);
    lv_obj_set_style_bg_color(root, theme_color_from_rgb(PV_BLACK), 0);   /* black whatever the theme, as the lock screen */
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(root, root_ev, LV_EVENT_ALL, NULL);
    lv_obj_t *bk = lv_obj_create(root);   /* the Back disc of the approved picker preview, where the header's Back is */
    s_back = bk;
    lv_obj_remove_style_all(bk);
    lv_obj_set_pos(bk, 72, 22);
    lv_obj_set_size(bk, 44, 44);
    lv_obj_clear_flag(bk, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bk, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(bk, 10);
    lv_obj_set_style_radius(bk, 22, 0);
    lv_obj_set_style_bg_color(bk, theme_color_from_rgb(PV_BLACK), 0);
    lv_obj_set_style_bg_opa(bk, LV_OPA_50, 0);
    ui_on(bk, back_cb, LV_EVENT_CLICKED, NULL, "custom_preview.back", UI_CORE);
    lv_obj_t *ic = lv_label_create(bk);
    lv_label_set_text(ic, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(ic, TF(UI_20), 0);
    lv_obj_set_style_text_color(ic, theme_color_from_rgb(PV_WHITE), 0);
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, -2);
}

void custom_preview_enter(void){ s_dark = 0; stop(); build(); }
void custom_preview_leave(void){ stop(); s_dark = 0; }
void custom_preview_dark(void)
{
    stop();
    s_dark = s_root && screen_current() == SCR_CUSTOM_PREVIEW ? 1 : 0;
}
