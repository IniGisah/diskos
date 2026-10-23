/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "custom_picker_ui.h"
#include "custom_saver_picker.h"
#include "custom_saver_overlay.h"
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "i18n.h"
#include "sdio.h"
#include "custom_saver_apply.h"
#include "lvgl/src/libs/gif/gifdec.h"
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Choose Image list: a 290 x 232 viewport at (41, 70) of 268 x 56 rows with a 6 px gap,
 * and a footer line at y 309.
 *
 * Scan: an LVGL timer steps the data model PK_BUDGET directory entries per tick (the model reads no more per call),
 * so the UI thread never blocks on a long walk. Rows are built once, when the scan is READY, never while it runs: the
 * list is sorted, so a row added mid-scan could land above the one under the user's finger. They are then added a few
 * per tick so a full 128-row list does not stall one frame. The timer exists only while the screen is shown and lit:
 * custom_picker_leave() (screen manager), the dark panel and the root's deletion each delete it and close the scan.
 *
 * Skipped files: the footer sums too_large + dimensions ("too large"), invalid + unreadable + name_long + name_bad +
 * not_regular ("unusable") and hidden ("list full"); one reason shows in brackets, a mix shows the count alone, and
 * a truncated scan shows "N+". Dot-files are not counted (the model keeps them apart).
 *
 * Preview (slice 16b1): a tap opens a full-screen sub-view of THIS screen (not a screen of its own: custom_picker_leave() frees the
 * scan, so a separate screen would lose it; here Back returns to the same list, scroll position and scan intact). The entry is
 * revalidated and copied at the tap. A GIF plays from the card file under a card lease held for its life; a PNG within the still
 * cap (2 MiB file, 1024 x 1024: custom_still_load's bounds) is decoded once into RAM. JPEG and BMP show a placeholder: LVGL's
 * decoders (tjpgd, bmp) take lower-case extensions only and re-read the card on every redraw outside a lease. Everything
 * (GIF object with its decoder, file and timer; pixels; lease; weather hold) is released by pv_destroy() on Back, leaving the
 * screen, dark (custom_picker_dark() from main.c, or the 100 ms watch) and the root's deletion. */
int ui_screen_backlight_on(void);   /* main.c: 0 while the panel is dark */

#define PK_BUDGET      24     /* directory entries per scan tick */
#define PK_TICK_MS     10     /* scan and row-building tick */
#define PK_IDLE_MS     500    /* READY/EMPTY/ERROR: only watches for dark / leaving */
#define PK_WATCH_MS    100    /* preview: dark / leaving watch */
#define PK_STILL_BYTES (2 * 1024 * 1024)   /* largest still previewed (custom_still_load enforces it and 1024 x 1024 itself) */
#define PK_RETRY_MS    1000   /* NO_CARD: look for the card again */
#define PK_SHOW_SCAN_MS 400   /* the "looking" note appears only when a scan is slower than this: a quick one never flashes */
#define PK_ROWS_TICK   8      /* rows built per tick (4 ms cap) */
#define PK_ROW_W       268
#define PK_ROW_H       56
#define PK_LIST_X      41
#define PK_LIST_Y      70
#define PK_LIST_W      290
#define PK_LIST_H      232
#define PK_FOOT_Y      309
#define PK_NAME_W      192
#define PK_CHIP_W      44
#define PK_CHIP_H      24
#define PK_BLACK       0u         /* the preview is drawn over a picture: black and white, whatever the theme (as the lock screen is) */
#define PK_WHITE       16777215u
#define PK_PH_GREY     2105376u   /* the placeholder behind "Preview not available" (a neutral grey: the Back disc and badge show on it) */
#define PK_PV_NAME_W   240    /* preview name and meta lines */

static lv_obj_t *s_root, *s_list, *s_body, *s_foot;
static csp_scan *s_scan;
static lv_timer_t *s_timer;
static int s_shown = -1;        /* csp_state the widgets show, -1 = none yet */
static int s_built;             /* rows built so far */
static int s_suspended;         /* freed while dark; the next draw after the wake starts again */
static int s_note;              /* the "looking" note is up */
static uint32_t s_t0;           /* tick the current scan started */
static lv_obj_t *s_pv;          /* the preview sub-view, NULL = the list */
static csp_entry s_pe;          /* the previewed entry, copied at the tap: a rescan of the list cannot change it */
static char s_ppath[320];
static lv_image_dsc_t s_pdsc;
static unsigned char *s_ppx;    /* the decoded PNG while it is shown */
static int s_please, s_phold;   /* card lease (a GIF's file handle), weather request hold: each taken and released once */
static int s_ap;                       /* apply state (AP_*), UI thread: set by the tap itself */
static lv_obj_t *s_aov;                /* the "Applying image..." layer */
static lv_timer_t *s_atimer;
static void use_image(const char *path, const csp_entry *en);
static custom_picker_use_cb_t s_use = use_image;   /* Use Image applies; a caller (the tests) may replace it */

static void tick(lv_timer_t *t);
static void start_scan(void);
static void begin_scan(void);
static void async_fail(void *u);

void custom_picker_set_use_cb(custom_picker_use_cb_t cb){ s_use = cb ? cb : use_image; }
int custom_picker_preview_open(void){ return s_pv != NULL; }
csp_scan *custom_picker_scan(void){ return s_scan; }
int custom_picker_rows(void){ return s_built; }

static void clear_widgets(void){
    if(s_list) lv_obj_clean(s_list);
    if(s_body) lv_obj_clean(s_body);
    if(s_foot) lv_obj_add_flag(s_foot, LV_OBJ_FLAG_HIDDEN);
    if(s_list) lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    s_shown = -1; s_built = 0; s_note = 0;
}

/* timer and scan, nothing else (also the root's DELETE path, when the widgets are going anyway) */
static void drop_scan(void){
    if(s_timer){ lv_timer_delete(s_timer); s_timer = NULL; }
    if(s_scan){ csp_close(s_scan); s_scan = NULL; }
}

static void pv_destroy(void);

/* The rows stay built while away (no blank list during the slide-out); the next entry clears them before it scans. */
void custom_picker_leave(void){
    pv_destroy();
    drop_scan();
    s_suspended = 0;
}

static void root_ev(lv_event_t *e){
    lv_event_code_t c = lv_event_get_code(e);
    if(c == LV_EVENT_DELETE){        /* the root is deleted: the screen is being rebuilt, the widgets go with it */
        pv_destroy();
        drop_scan();
        s_root = s_list = s_body = s_foot = s_aov = NULL;   /* an apply in flight goes on; its result is a toast */
        s_shown = -1; s_built = 0; s_suspended = 0; s_note = 0;
    } else if(c == LV_EVENT_DRAW_MAIN_BEGIN){   /* the first draw after a dark panel is lit again */
        if(s_suspended && ui_screen_backlight_on() && screen_current() == SCR_CUSTOM_PICKER){
            s_suspended = 0;
            begin_scan();                 /* no widget is touched while drawing: the first tick builds them */
            if(!s_scan) lv_async_call(async_fail, NULL);
        }
    }
}

/* ---- rows ---- */
static void fmt_size(char *o, size_t n, long long sz){
    if(sz < 1024) snprintf(o, n, "%lld B", sz);
    else if(sz < 1024 * 1024) snprintf(o, n, "%lld KB", (sz + 512) / 1024);
    else snprintf(o, n, "%.1f MB", (double)sz / (1024.0 * 1024.0));
}

static const char *chip_text(const csp_entry *en){
    if(en->kind == CUSTOM_SOURCE_GIF) return "GIF";
    const char *dot = strrchr(en->name, '.');
    if(dot && !strcasecmp(dot, ".png")) return "PNG";
    if(dot && !strcasecmp(dot, ".bmp")) return "BMP";
    return "JPG";                    /* .jpg and .jpeg in any case */
}

static void pv_open(int i);

static void row_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED || !s_scan) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= csp_count(s_scan)) return;
    pv_open(i);
}

static void add_row(int i){
    const csp_entry *en = csp_entry_at(s_scan, i);
    if(!en) return;
    lv_obj_t *r = lv_button_create(s_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, PK_ROW_W, PK_ROW_H);
    lv_obj_set_style_radius(r, 12, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i, "custom_picker.row", UI_CORE);
    kit_row_layout(r, KIT_ROW_PLAIN);

    lv_obj_t *nm = lv_label_create(r);
    char fit[CSP_NAME_MAX + 8];
    custom_ov_fit_text(TF(USER_16), en->name, PK_NAME_W, fit, sizeof fit);
    lv_label_set_text(nm, fit);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(nm, 14, 6);
    lv_obj_set_size(nm, PK_NAME_W, 24);
    lv_obj_set_style_text_font(nm, TF(USER_16), 0);
    lv_obj_set_style_text_color(nm, TC(TEXT_PRIMARY), 0);

    char sz[24];
    fmt_size(sz, sizeof sz, en->size);
    lv_obj_t *sl = lv_label_create(r);
    lv_label_set_text(sl, sz);
    lv_obj_set_pos(sl, 14, 31);
    lv_obj_set_style_text_font(sl, TF(UI_14), 0);
    lv_obj_set_style_text_color(sl, TC(TEXT_MUTED), 0);

    lv_obj_t *chip = lv_obj_create(r);
    lv_obj_remove_style_all(chip);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(chip, PK_CHIP_W, PK_CHIP_H);
    lv_obj_set_pos(chip, PK_ROW_W - 12 - PK_CHIP_W, (PK_ROW_H - PK_CHIP_H) / 2);
    lv_obj_set_style_radius(chip, PK_CHIP_H / 2, 0);
    lv_obj_set_style_bg_color(chip, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_t *ct = lv_label_create(chip);
    lv_label_set_text(ct, chip_text(en));
    lv_obj_set_style_text_font(ct, TF(UI_14), 0);
    lv_obj_set_style_text_color(ct, en->kind == CUSTOM_SOURCE_GIF ? ui_current_accent() : TC(TEXT_SECONDARY), 0);
    lv_obj_center(ct);
    theme_list_row(r);
}

/* ---- preview ---- */
static void pv_destroy(void){
    if(s_pv){ lv_obj_t *o = s_pv; s_pv = NULL; lv_obj_delete(o); }   /* a GIF's destructor closes its decoder and file and deletes its timer */
    if(s_ppx){ lv_image_cache_drop(&s_pdsc); custom_still_free(s_ppx); s_ppx = NULL; }
    if(s_please){ s_please = 0; sd_io_end(); }
    if(s_phold){ s_phold = 0; weather_hold(0); }
    if(s_timer) lv_timer_set_period(s_timer, s_scan && csp_state_of(s_scan) == CSP_READY && s_built < csp_count(s_scan) ? PK_TICK_MS : PK_IDLE_MS);   /* rows still being built: keep stepping */
}

/* Back (the swipe and every other path, all via screen_back) while the preview is up closes it and leaves the list as it was; no preview: 0, the screen manager navigates. */
int custom_picker_busy(void){ return s_ap != 0; }
int custom_picker_back(void){
    if(s_ap) return 1;                    /* applying: Back is refused, the layer would lose its screen */
    if(!s_pv) return 0;
    pv_destroy();
    return 1;
}

/* The panel went dark (main.c, beside saver_dark): the GIF's own 10 ms timer must not decode another frame. The list is
 * suspended exactly as the dark tick does; the first draw after the wake scans again and shows the list. */
void custom_picker_dark(void){
    if(!s_pv) return;
    pv_destroy();
    drop_scan();
    clear_widgets();
    s_suspended = 1;
}

static void pv_back_async(void *u){ (void)u; pv_destroy(); }   /* the button is inside the sub-view: not deleted in its own event */
static void pv_back_cb(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_CLICKED && s_pv) lv_async_call(pv_back_async, NULL); }
static void pv_use_cb(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_CLICKED && s_pv) s_use(s_ppath, &s_pe); }

static lv_obj_t *pv_shape(int x, int y, int w, int h, int r, lv_opa_t opa){   /* black, translucent: the image shows through */
    lv_obj_t *o = lv_obj_create(s_pv);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, theme_color_from_rgb(PK_BLACK), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    return o;
}

/* white over a picture, whatever the theme (the picture is the background, as on the lock screen) */
static lv_obj_t *pv_text(lv_obj_t *p, const char *t, const lv_font_t *f, int x, int y, int w, lv_opa_t opa){
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, theme_color_from_rgb(PK_WHITE), 0);
    lv_obj_set_style_text_opa(l, opa, 0);
    return l;
}

/* ---- Use Image: apply (slice 16b2) ----
 * The tap (UI thread) sets the one-shot guard s_ap, copies the path and defers to lv_async_call: ap_begin destroys the preview
 * (and the Use button that was tapped) outside its own click, covers the picker with a quiet "Applying image..." layer (the
 * "Looking for images..." idiom: dimmed screen, centred text, no animation) that swallows touches, and starts the work.
 *  - GIF: a short-lived detached worker runs custom_gif_apply(path, ap_decode). Its trial decode needs LVGL's allocator and lv_fs,
 *    so ap_decode (worker) posts a request and waits on a condition; ap_tick (UI thread, 100 ms; the main loop keeps running at
 *    200 ms with the panel off) serves it with ap_serve and signals. The UI thread never waits for the worker (it only reads
 *    ap_fin under the mutex), so the two cannot deadlock. A request nobody picks up within AP_UI_WAIT_S is withdrawn and the apply
 *    ends DECODE with the previous image kept; a request the UI thread has started is always waited for.
 *  - Still (PNG, JPEG, BMP): custom_still_start, then custom_still_poll from ap_tick.
 * Back is refused while it runs (the layer would lose its screen); other exits leave it running and the result is a toast.
 * Dark panel: the apply continues; the result is read (custom_still_poll hands it out once) and shown once, on the first lit tick.
 * Process ownership for custom_saver_db.h:50: stock mq_ui is not running while diskOS is (the boot watchdog's pgrep -x mq_ui
 * matches our normalised argv[0], docs/NOTES.md:1972, main.c argv[0] dispatch). */
int settings_saver_select_custom(void);   /* settings.c: the Saver Style page's own writer */
#define AP_TICK_MS   100
#define AP_UI_WAIT_S 10
enum { AP_IDLE, AP_QUEUED, AP_RUN };
static int s_akind;                       /* UI thread */
static char s_apath[320];
static pthread_mutex_t ap_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ap_cv = PTHREAD_COND_INITIALIZER;
static int ap_req, ap_rc, ap_fin;       /* under ap_mu: decode request 0 none, 1 asked, 2 serving, 3 answered; its answer; the result is in */
static custom_gif_status ap_st;
static char ap_stage[320];

static int ap_decode(const char *stage){      /* worker thread */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += AP_UI_WAIT_S;
    pthread_mutex_lock(&ap_mu);
    snprintf(ap_stage, sizeof ap_stage, "%s", stage);
    ap_req = 1;
    while(ap_req != 3){
        int r = ap_req == 1 ? pthread_cond_timedwait(&ap_cv, &ap_mu, &ts) : pthread_cond_wait(&ap_cv, &ap_mu);
        if(r && ap_req == 1){ ap_req = 0; pthread_mutex_unlock(&ap_mu); return -1; }
    }
    ap_req = 0;
    int rc = ap_rc;
    pthread_mutex_unlock(&ap_mu);
    return rc;
}

static void ap_serve(void){                   /* UI thread: the same first-frame test as custom_saver_apply.c ga_decode */
    char p[sizeof ap_stage + 2];
    pthread_mutex_lock(&ap_mu);
    if(ap_req != 1){ pthread_mutex_unlock(&ap_mu); return; }
    ap_req = 2;
    snprintf(p, sizeof p, "A:%s", ap_stage);
    pthread_mutex_unlock(&ap_mu);
    gd_GIF *g = gd_open_gif_file(p);
    int rc = g && gd_get_frame(g) == 1 ? 0 : -1;
    if(g) gd_close_gif(g);
    pthread_mutex_lock(&ap_mu);
    ap_rc = rc; ap_req = 3;
    pthread_cond_signal(&ap_cv);
    pthread_mutex_unlock(&ap_mu);
}

static void *ap_worker(void *u){
    (void)u;
    custom_gif_status st = custom_gif_apply(s_apath, ap_decode);
    pthread_mutex_lock(&ap_mu);
    ap_st = st; ap_fin = 1;
    pthread_mutex_unlock(&ap_mu);
    return NULL;
}

/* Every failure keeps the previous image (decision 34), except RESTORE, which says it could not. */
static const char *ap_msg(custom_gif_status st){
    switch(st){
    case CUSTOM_GIF_OK: return "Image set";
    case CUSTOM_GIF_BUSY: case CUSTOM_GIF_CONVERSION: return "Image may still be applying - try again later";
    case CUSTOM_GIF_STORAGE_FULL: return "Not enough storage to set this image.";
    case CUSTOM_GIF_RESTORE: return "Could not restore the previous image.";
    default: return "Could not use this image.";
    }
}

static void ap_tick(lv_timer_t *t){
    (void)t;
    ap_serve();
    if(!ui_screen_backlight_on()) return;
    custom_gif_status st;
    pthread_mutex_lock(&ap_mu);
    int fin = ap_fin;
    st = ap_st;
    pthread_mutex_unlock(&ap_mu);
    if(!fin && s_akind != CUSTOM_SOURCE_GIF){
        custom_still_state k = custom_still_poll(&st);
        fin = k == CUSTOM_STILL_DONE || k == CUSTOM_STILL_UNRESOLVED;   /* no a810 in time: unknown, and the API blocks the next apply */
        if(k == CUSTOM_STILL_UNRESOLVED) st = CUSTOM_GIF_CONVERSION;
    }
    if(!fin) return;
    lv_timer_delete(s_atimer); s_atimer = NULL;
    if(s_aov){ lv_obj_t *o = s_aov; s_aov = NULL; lv_obj_delete(o); }
    s_ap = AP_IDLE;
    if(!st) settings_saver_select_custom();   /* decision 80: a successful apply also selects Custom, once; a failure changes nothing */
    ui_toast(ap_msg(st));
    if(!st && screen_current() == SCR_CUSTOM_PICKER) screen_back();
}

static void ap_begin(void *u){
    (void)u;
    pv_destroy();
    if(s_root){
        s_aov = lv_obj_create(s_root);
        lv_obj_remove_style_all(s_aov);
        lv_obj_set_size(s_aov, 360, 360);
        lv_obj_add_flag(s_aov, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_aov, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(s_aov, theme_color_from_rgb(PK_BLACK), 0);
        lv_obj_set_style_bg_opa(s_aov, LV_OPA_70, 0);
        lv_obj_center(pv_text(s_aov, tr("Applying image..."), TF(UI_18), 0, 0, 216, LV_OPA_COVER));
    }
    pthread_mutex_lock(&ap_mu);
    ap_req = ap_fin = 0; ap_st = CUSTOM_GIF_OK;
    pthread_mutex_unlock(&ap_mu);
    s_ap = AP_RUN;
    s_atimer = lv_timer_create(ap_tick, AP_TICK_MS, NULL);
    custom_gif_status st = CUSTOM_GIF_OK;
    if(s_akind == CUSTOM_SOURCE_GIF){
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 256 * 1024);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if(pthread_create(&th, &at, ap_worker, NULL)) st = CUSTOM_GIF_STORAGE;
        pthread_attr_destroy(&at);
    } else st = custom_still_start(s_apath);
    if(st){ pthread_mutex_lock(&ap_mu); ap_st = st; ap_fin = 1; pthread_mutex_unlock(&ap_mu); }   /* refused at once: the next tick says why */
}

static void use_image(const char *path, const csp_entry *en){
    if(s_ap) return;                      /* one-shot: a second tap before the layer is up starts nothing */
    s_ap = AP_QUEUED;
    snprintf(s_apath, sizeof s_apath, "%s", path);
    s_akind = en->kind;
    if(lv_async_call(ap_begin, NULL) != LV_RESULT_OK) s_ap = AP_IDLE;
}

static int pv_picture(void){   /* 1 = a picture is shown; 0 = nothing usable, the caller draws the placeholder */
    char src[340];
    if(s_pe.kind == CUSTOM_SOURCE_GIF){
        if(!sd_io_begin()) return 0;
        s_please = 1;                      /* the lease covers the file handle until pv_destroy() */
        lv_obj_t *g = lv_gif_create(s_pv);
        snprintf(src, sizeof src, "A:%s", s_ppath);
        lv_gif_set_src(g, src);
        if(lv_gif_is_loaded(g) && lv_gif_get_status(g) > 0){   /* opened AND its first frame decoded */
            const lv_image_dsc_t *d = lv_image_get_src(g);
            uint32_t side = d->header.w < d->header.h ? d->header.w : d->header.h;
            lv_image_set_scale(g, (256u * 360u + side - 1u) / side);
            lv_obj_center(g);
            return 1;
        }
        lv_obj_delete(g);
        s_please = 0; sd_io_end();
        return 0;
    }
    if(strcmp(chip_text(&s_pe), "PNG") || s_pe.size > PK_STILL_BYTES || !sd_io_begin()) return 0;
    int w = 0, h = 0;
    s_ppx = custom_still_load(s_ppath, &w, &h);
    sd_io_end();
    if(!s_ppx) return 0;
    s_pdsc = (lv_image_dsc_t){ .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = (uint32_t)w, .h = (uint32_t)h,
                                           .stride = (uint32_t)w * 4 }, .data_size = (uint32_t)w * (uint32_t)h * 4, .data = s_ppx };
    lv_obj_t *im = lv_image_create(s_pv);
    lv_image_set_src(im, &s_pdsc);
    lv_image_set_scale(im, (uint32_t)((256 * 360 + (w < h ? w : h) - 1) / (w < h ? w : h)));   /* full-bleed: the circle crops it */
    lv_obj_center(im);
    return 1;
}

/* Name on at most two lines (decision 78): line one is the longest prefix that fits, line two the rest, cut with "...". */
static void pv_name(const char *name, int bottom){
    const lv_font_t *f = TF(USER_16);
    int lh = lv_font_get_line_height(f), n = (int)strlen(name), j = 0;
    char l1[CSP_NAME_MAX], l2[CSP_NAME_MAX + 8];
    lv_point_t sz;
    lv_text_get_size(&sz, name, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    l2[0] = 0;
    snprintf(l1, sizeof l1, "%s", name);
    if(sz.x > PK_PV_NAME_W){
        for(int k = 1; k <= n; k++){
            if(k < n && ((unsigned char)name[k] & 0xC0) == 0x80) continue;   /* only cut before a character */
            memcpy(l1, name, (size_t)k); l1[k] = 0;
            lv_text_get_size(&sz, l1, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if(sz.x > PK_PV_NAME_W) break;
            j = k;
        }
        memcpy(l1, name, (size_t)j); l1[j] = 0;
        custom_ov_fit_text(f, name + j, PK_PV_NAME_W, l2, sizeof l2);
    }
    pv_text(s_pv, l1, f, 60, bottom - lh * (l2[0] ? 2 : 1), PK_PV_NAME_W, LV_OPA_COVER);
    if(l2[0]) pv_text(s_pv, l2, f, 60, bottom - lh, PK_PV_NAME_W, LV_OPA_COVER);
}

static void pv_build(void){
    s_pv = lv_obj_create(s_root);
    kit_isolate(s_pv);
    lv_obj_remove_style_all(s_pv);
    lv_obj_set_size(s_pv, 360, 360);
    lv_obj_clear_flag(s_pv, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_pv, theme_color_from_rgb(PK_BLACK), 0);
    lv_obj_set_style_bg_opa(s_pv, LV_OPA_COVER, 0);
    if(!pv_picture()){
        lv_obj_set_style_bg_color(s_pv, theme_color_from_rgb(PK_PH_GREY), 0);   /* neutral dark grey, whatever the theme */
        lv_obj_t *l = pv_text(s_pv, tr("Preview not available for this image"), TF(UI_16), 72, 120, 216, LV_OPA_70);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    lv_obj_t *g = pv_shape(0, 184, 360, 176, 0, LV_OPA_COVER);   /* dark gradient behind the text: 0 % at the top to 92 % */
    lv_obj_set_style_bg_grad_dir(g, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_grad_color(g, theme_color_from_rgb(PK_BLACK), 0);
    lv_obj_set_style_bg_main_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_opa(g, (lv_opa_t)235, 0);
    lv_obj_t *b = pv_shape(232, 36, 50, 24, 12, LV_OPA_50);
    lv_obj_center(pv_text(b, chip_text(&s_pe), TF(UI_14), 0, 0, 50, LV_OPA_COVER));
    lv_obj_t *bk = pv_shape(72, 22, 44, 44, 22, LV_OPA_50);   /* Back where the header's Back is */
    lv_obj_add_flag(bk, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(bk, 10);
    ui_on(bk, pv_back_cb, LV_EVENT_CLICKED, NULL, "custom_picker.preview_back", UI_CORE);
    lv_obj_t *ic = pv_text(bk, LV_SYMBOL_LEFT, TF(UI_20), 0, 0, 44, LV_OPA_COVER);
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, -2);
    pv_name(s_pe.name, 262);
    char sz[24], meta[48];
    fmt_size(sz, sizeof sz, s_pe.size);
    snprintf(meta, sizeof meta, "%s - %s", chip_text(&s_pe), sz);
    pv_text(s_pv, meta, TF(UI_14), 60, 267, PK_PV_NAME_W, LV_OPA_80);
    lv_obj_t *u = lv_button_create(s_pv);
    lv_obj_remove_style_all(u);
    lv_obj_set_pos(u, 96, 292);
    lv_obj_set_size(u, 168, 44);
    lv_obj_set_ext_click_area(u, 4);
    lv_obj_set_style_radius(u, 22, 0);
    lv_obj_set_style_bg_color(u, ui_current_accent(), 0);
    lv_obj_set_style_bg_opa(u, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(u, LV_OPA_80, LV_STATE_PRESSED);
    ui_on(u, pv_use_cb, LV_EVENT_CLICKED, NULL, "custom_picker.use_image", UI_CORE);
    lv_obj_t *ul = pv_text(u, tr("Use Image"), TF(UI_18), 0, 0, 168, LV_OPA_COVER);
    lv_obj_center(ul);
}

static void pv_open(int i){
    if(s_pv || !s_scan || i < 0 || i >= csp_count(s_scan)) return;
    csp_reval r = csp_revalidate(s_scan, i);
    const csp_entry *en = csp_entry_at(s_scan, i);
    if(r != CSP_REV_OK || !en || csp_path(s_scan, i, s_ppath, sizeof s_ppath)){
        /* the file changed, vanished or is refused since the scan: stay on the list, say so, and list the folder again */
        ui_toast(r == CSP_REV_NO_CARD ? "Insert an SD card." : r == CSP_REV_GONE || r == CSP_REV_CHANGED ? "That image changed or was removed." : "Can't use this image.");
        csp_restart(s_scan);
        s_shown = -1;                     /* the next tick rebuilds the rows (not here: this runs inside a row's own click) */
        s_t0 = lv_tick_get();
        if(s_timer) lv_timer_set_period(s_timer, PK_TICK_MS);
        return;
    }
    s_pe = *en;
    weather_hold(1);                      /* no weather request starts while the preview is up; released once by pv_destroy() */
    s_phold = 1;
    pv_build();
    if(s_timer) lv_timer_set_period(s_timer, PK_WATCH_MS);
}

/* ---- messages ---- */
static lv_obj_t *msg(const char *text, int y, int w, const lv_font_t *f, theme_color_role_t col){
    lv_obj_t *l = lv_label_create(s_body);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, tr(text));
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, (360 - w) / 2, y - PK_LIST_Y);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, theme_color(col), 0);
    return l;
}

static void async_start(void *u){
    (void)u;
    if(screen_current() == SCR_CUSTOM_PICKER && !s_scan && ui_screen_backlight_on()) start_scan();
}

/* Try Again: the widgets are replaced by the next tick (not here, inside the button's own event) */
static void retry_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if(!s_scan){ lv_async_call(async_start, NULL); return; }   /* the scan itself could not be opened: open it again */
    csp_restart(s_scan);
    s_t0 = lv_tick_get();
    if(s_timer) lv_timer_set_period(s_timer, PK_TICK_MS);
}

static void show_error(void){
    msg("Can't read the CustomCover folder.", 156, 240, TF(UI_18), THEME_CLR_TEXT_PRIMARY);
    lv_obj_t *b = lv_button_create(s_body);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 150, 44);
    lv_obj_set_pos(b, 105, 220 - PK_LIST_Y);
    lv_obj_set_ext_click_area(b, 4);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, TC(RAISED_PRESSED), LV_STATE_PRESSED);
    ui_on(b, retry_cb, LV_EVENT_CLICKED, NULL, "custom_picker.retry", UI_CORE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, tr("Try Again"));
    lv_obj_set_style_text_font(l, TF(UI_18), 0);
    lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0);
    lv_obj_center(l);
    theme_list_row(b);
}

/* ---- footer ---- */
static void footer(void){
    const csp_stats *st = csp_stats_of(s_scan);
    unsigned big = st->too_large + st->dimensions;
    unsigned bad = st->invalid + st->unreadable + st->name_long + st->name_bad + st->not_regular;
    unsigned full = st->hidden;
    unsigned n = big + bad + full;
    if(!n && !st->truncated){ lv_obj_add_flag(s_foot, LV_OBJ_FLAG_HIDDEN); return; }
    char head[64], text[128];
    if(!n) snprintf(head, sizeof head, "%s", tr("Some files not checked"));
    else if(st->truncated) snprintf(head, sizeof head, tr("%u+ files skipped"), n);
    else if(n == 1) snprintf(head, sizeof head, "%s", tr("1 file skipped"));
    else snprintf(head, sizeof head, tr("%u files skipped"), n);
    int kinds = (big > 0) + (bad > 0) + (full > 0);
    if(n && kinds == 1)
        snprintf(text, sizeof text, "%s (%s)", head, tr(big ? "too large" : bad ? "unusable" : "list full"));
    else snprintf(text, sizeof text, "%s", head);
    char fit[128];
    int maxw = ui_round_width(PK_FOOT_Y + 20, 4);
    lv_point_t sz;
    lv_text_get_size(&sz, text, TF(UI_14), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if(sz.x > maxw) snprintf(text, sizeof text, "%s", head);   /* too wide for the glass: the count alone, then cut */
    custom_ov_fit_text(TF(UI_14), text, maxw, fit, sizeof fit);
    lv_label_set_text(s_foot, fit);
    lv_obj_clear_flag(s_foot, LV_OBJ_FLAG_HIDDEN);
}

/* ---- the state the widgets show ---- */
static void show_state(csp_state st){
    clear_widgets();
    s_shown = (int)st;
    switch(st){
    case CSP_NO_CARD:
        msg("Insert an SD card.", 170, 240, TF(UI_18), THEME_CLR_TEXT_PRIMARY);
        break;
    case CSP_EMPTY:
        msg("Add images to CustomCover on your SD card.", 127, 216, TF(UI_18), THEME_CLR_TEXT_PRIMARY);
        msg("JPG, PNG, BMP or GIF", 202, 216, TF(UI_14), THEME_CLR_TEXT_MUTED);
        footer();
        break;
    case CSP_ERROR:
        show_error();
        break;
    case CSP_READY:
        lv_obj_clear_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
        footer();
        break;
    default:
        break;
    }
}

static void tick(lv_timer_t *t){
    (void)t;
    if(!s_scan) return;
    if(!ui_screen_backlight_on()){            /* dark: no stepping; free everything, the first draw after the wake restarts */
        pv_destroy();
        drop_scan();
        clear_widgets();
        s_suspended = 1;
        return;
    }
    if(screen_current() != SCR_CUSTOM_PICKER){ custom_picker_leave(); return; }
    csp_state st = csp_state_of(s_scan);
    if(st == CSP_SCANNING || st == CSP_NO_CARD) st = csp_step(s_scan, PK_BUDGET);
    if(st == CSP_SCANNING){
        if(s_shown != CSP_SCANNING){ clear_widgets(); s_shown = CSP_SCANNING; lv_timer_set_period(s_timer, PK_TICK_MS); }
    } else if((int)st != s_shown){
        show_state(st);
        lv_timer_set_period(s_timer, st == CSP_NO_CARD ? PK_RETRY_MS : st == CSP_READY ? PK_TICK_MS : PK_IDLE_MS);
    }
    if(st == CSP_SCANNING && !s_note && lv_tick_elaps(s_t0) >= PK_SHOW_SCAN_MS){
        s_note = 1;
        msg("Looking for images...", 170, 240, TF(UI_16), THEME_CLR_TEXT_MUTED);
    }
    if(st == CSP_READY && s_built < csp_count(s_scan)){
        uint32_t t0 = lv_tick_get();
        for(int k = 0; k < PK_ROWS_TICK && s_built < csp_count(s_scan); k++){
            add_row(s_built++);
            if(lv_tick_elaps(t0) >= 4) break;
        }
        if(s_built >= csp_count(s_scan)) lv_timer_set_period(s_timer, PK_IDLE_MS);
    }
    if(s_pv) lv_timer_set_period(s_timer, PK_WATCH_MS);   /* a preview is up: look for dark / leaving often */
}

static void begin_scan(void){
    s_t0 = lv_tick_get();
    s_scan = csp_open(NULL);
    if(s_scan) s_timer = lv_timer_create(tick, PK_TICK_MS, NULL);
}

static void start_scan(void){
    drop_scan();
    clear_widgets();
    begin_scan();
    if(!s_scan) show_state(CSP_ERROR);   /* out of memory: the same Try Again opens it again */
}

static void async_fail(void *u){
    (void)u;
    if(screen_current() == SCR_CUSTOM_PICKER && !s_scan && s_list) show_state(CSP_ERROR);
}

void custom_picker_enter(void){
    s_suspended = 0;
    pv_destroy();
    if(!ui_screen_backlight_on()){ drop_scan(); clear_widgets(); s_suspended = 1; return; }
    start_scan();
}

void custom_picker_open(void){ screen_show(SCR_CUSTOM_PICKER); }

void custom_picker_create(lv_obj_t *root){
    s_root = root;
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header(root, "Choose Image");

    s_list = lv_obj_create(root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_pos(s_list, PK_LIST_X, PK_LIST_Y);
    lv_obj_set_size(s_list, PK_LIST_W, PK_LIST_H);
    lv_obj_set_style_pad_bottom(s_list, 6, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);

    s_body = lv_obj_create(root);        /* the message states; transparent and not clickable, so it never covers Back */
    lv_obj_remove_style_all(s_body);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_body, 0, PK_LIST_Y);
    lv_obj_set_size(s_body, 360, 360 - PK_LIST_Y);

    /* A full-width fixed layer keeps the footer out of the header's body shift. */
    lv_obj_t *foot_box = lv_obj_create(root);
    lv_obj_remove_style_all(foot_box);
    lv_obj_clear_flag(foot_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(foot_box, 360, 20);
    lv_obj_set_pos(foot_box, 0, PK_FOOT_Y);
    s_foot = lv_label_create(foot_box);
    lv_label_set_long_mode(s_foot, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(s_foot, 232, 20);
    lv_obj_set_pos(s_foot, 64, 0);
    lv_obj_set_style_text_align(s_foot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_foot, TF(UI_14), 0);
    lv_obj_set_style_text_color(s_foot, TC(TEXT_SECONDARY), 0);
    lv_obj_add_flag(s_foot, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(root, root_ev, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(root, root_ev, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
}
