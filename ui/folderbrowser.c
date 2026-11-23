#include "indic.h"
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "rowpool.h"
#include "theme.h"
#include "theme_kit.h"
#include "sdio.h"
#include "folderbrowser.h"
#include "musicdb.h"
#include "scanner.h"
#include "config.h"
#include "modes.h"
#include "artcache.h"
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>

/* File/folder browser (SCR_FOLDER).
 *
 * Parity item L24: a browsable directory tree of the SD card (mounted at
 * /tmp/sdcard), so a user can navigate folders and reach a track by its file
 * location rather than only by catalog axis (Songs/Albums/Artists/...).
 *
 * WHAT WORKS
 *   - Full folder navigation: folders (sorted first) then audio files (.mp3 /
 *     .flac / .wav, the set diskOS indexes - see scanner.c is_audio). Tap a
 *     folder to descend; the header back-chevron ascends one level and, at the
 *     SD root, leaves the screen.
 *   - Tap an audio file to PLAY that exact track. The tracks in the current
 *     folder are queued as a type-5 folder plan (mdb_folder_plan), so the queue
 *     scope is restricted to that folder alone (matching stock parity and user
 *     expectation). The tapped track starts immediately.
 *   - Play All / Shuffle buttons at the top of any folder containing audio tracks.
 *   - "A-Z" button and Jump to letter grid for fast navigation across folders/files.
 *   - Alphabet position hint overlay when scrolling or using the rim wheel.
 *   - SACD .iso images the library has IS_ISO rows for, and .cue sheets, are listed too. Tapping one plays that
 *     file's tracks (SONG rows by PATH, TRACK order) as the exact type-5 queue the album drill uses (ui_play_plan).
 *     A sheet names its audio file(s) in FILE lines; the tracks live under that audio PATH.
 */

#define FB_ROOT        "/tmp/sdcard"
#define FB_MAXPATH     1024
#define FB_NAMELEN     256
/* Memory-safe ceiling on entries kept per folder. Folders are always retained (scan pass 0);
 * files fill the rest, so a flat music folder of a few thousand tracks shows in full. True
 * unlimited would need a row-recycling virtual list, which this device's RAM does not favour. */
#define FB_MAX_ENTRIES 4000

#define FB_K_AUDIO 0
#define FB_K_ISO   1     /* SACD image the library has IS_ISO rows for: opens its tracks */
#define FB_K_CUE   2     /* .cue sheet: opens the tracks of the file(s) it names */
typedef struct {
    char name[FB_NAMELEN];
    int  is_dir;
    int  kind;           /* FB_K_* for files */
} fb_entry_t;

static char        g_dir[FB_MAXPATH];
static fb_entry_t *g_ent;                  /* grown on demand up to FB_MAX_ENTRIES */
static char       *g_first;                /* first letter per entry for fast scrolling */
static int         g_nent, g_first_cap;
static int         g_opened;
static lv_obj_t   *g_list;
static lv_obj_t   *g_title;
static lv_obj_t   *g_az_btn;
static lv_obj_t   *g_grid;
static lv_obj_t   *g_lhint = NULL, *g_lhint_lbl = NULL;
static uint32_t    g_lhint_tick = 0;
static char        g_lhint_ch = 0;
static int         g_has_play_header = 0;
static row_pool_t  g_rows;

static char first_letter(const char *s){
    while(*s==' ') s++;
    char c = toupper((unsigned char)*s);
    return (c>='A'&&c<='Z') ? c : '#';
}

/* audio files diskOS can play - mirror scanner.c is_audio() exactly. */
static int fb_has_ext(const char *name, const char *ext){
    size_t nl = strlen(name), el = strlen(ext);
    return nl > el && !strcasecmp(name + nl - el, ext);
}
static int fb_is_audio(const char *name){
    /* mirror scanner.c is_audio()'s music set (SONG-resident, folder-playable). .m4b is intentionally
     * NOT here: audiobooks are isolated in their own BOOKS table + Books menu, out of the folder queue. */
    return fb_has_ext(name, ".mp3") || fb_has_ext(name, ".flac")
        || fb_has_ext(name, ".wav") || fb_has_ext(name, ".m4a")
        /* the scanner's other formats (stock V2.57's list): a file Files shows must be one the scanner indexes */
        || fb_has_ext(name, ".aac") || fb_has_ext(name, ".ogg") || fb_has_ext(name, ".ape")
        || fb_has_ext(name, ".aif") || fb_has_ext(name, ".aiff") || fb_has_ext(name, ".wma")
        || fb_has_ext(name, ".dsf") || fb_has_ext(name, ".dff") || fb_has_ext(name, ".dts")
        || fb_has_ext(name, ".opus") || fb_has_ext(name, ".oga");
}

/* folders first, then files; case-insensitive within each group. */
static int fb_cmp(const void *a, const void *b){
    const fb_entry_t *x = a, *y = b;
    if(x->is_dir != y->is_dir) return y->is_dir - x->is_dir;   /* dir(1) sorts before file(0) */
    return strcasecmp(x->name, y->name);
}

static void fb_fill_stop(void){ row_pool_clear(&g_rows); }

/* Single worker, latest request wins; the UI timer owns all widgets. */
typedef struct {
    fb_entry_t *ent;
    int nent, cap, opened, cancelled;
    unsigned gen;
} fb_result_t;
static pthread_mutex_t g_fb_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_fb_gen;
static int g_fb_running, g_fb_requested, g_fb_ready, g_fb_waiting;
static int g_fb_error_shown;
static char g_fb_path[FB_MAXPATH];
static fb_result_t g_fb_res;
static lv_timer_t *g_fb_poll;

static int fb_current(unsigned gen){
    pthread_mutex_lock(&g_fb_mu);
    int ok = gen == g_fb_gen && g_fb_requested;
    pthread_mutex_unlock(&g_fb_mu);
    return ok && sd_io_allowed();
}

/* Two passes and the original comparator preserve admission and ordering. */
static void fb_scan_leased(const char *dir, fb_result_t *res){
    DIR *d = opendir(dir);
    if(!d){
        if(fb_current(res->gen)){
            d = opendir(dir); res->opened = d != NULL;
            if(d) closedir(d);
        }
        return;
    }
    struct dirent *e;
    /* Two passes so a user's folders are NEVER dropped by the entry cap in a huge directory:
     * pass 0 admits every subdirectory, pass 1 fills the remaining budget with audio files.
     * A flat Music/ with thousands of files (the user's has 3277) otherwise pushed folders past
     * FB_MAX_ENTRIES in readdir order and hid them entirely, because the folders-first ordering
     * only happens in the post-scan sort. We classify from the dirent d_type when the FS provides
     * it (exfat does) and fall back to lstat only on DT_UNKNOWN, so the extra pass adds no stat()
     * cost on the normal path. */
    for(int pass = 0; pass < 2; pass++){
        for(errno = 0; fb_current(res->gen) && (e = readdir(d)); errno = 0){
            const char *nm = e->d_name;
            if(nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;  /* skip . and .. */
            if(res->nent >= FB_MAX_ENTRIES) break;
            if(res->nent >= res->cap){                          /* grow the entry array as needed */
                int nc = res->cap ? res->cap * 2 : 128;
                if(nc > FB_MAX_ENTRIES) nc = FB_MAX_ENTRIES;
                fb_entry_t *ne = realloc(res->ent, (size_t)nc * sizeof *res->ent);
                if(!ne) break;                                /* OOM: keep what we have, never crash */
                res->ent = ne; res->cap = nc;
            }

            int is_dir;
            if(e->d_type == DT_DIR)      is_dir = 1;
            else if(e->d_type == DT_REG) is_dir = 0;
            else {                                            /* DT_UNKNOWN/other: stat to classify */
                char path[FB_MAXPATH];
                int pn = snprintf(path, sizeof path, "%s/%s", dir, nm);
                if(pn <= 0 || pn >= (int)sizeof path) continue;   /* path too long - skip */
                struct stat st;
                if(lstat(path, &st) != 0) continue;           /* orphaned / unreadable - skip (never fatal) */
                if(S_ISDIR(st.st_mode)) is_dir = 1;
                else if(S_ISREG(st.st_mode)) is_dir = 0;
                else continue;                                /* not a folder or a regular file */
            }
            if(is_dir != (pass == 0)) continue;               /* pass 0 = dirs only, pass 1 = files only */
            int kind = FB_K_AUDIO;
            if(!is_dir && !fb_is_audio(nm)){                  /* files: audio we can play, indexed .iso images, .cue sheets */
                if(fb_has_ext(nm, ".cue")) kind = FB_K_CUE;
                else if(fb_has_ext(nm, ".iso")){
                    char ip[FB_MAXPATH];
                    int pn = snprintf(ip, sizeof ip, "%s/%s", dir, nm);
                    if(pn <= 0 || pn >= (int)sizeof ip) continue;
                    int count=mdb_subtrack_count(ip,1);
                    if(count<0){ res->cancelled=1; closedir(d); return; }
                    if(!count) continue;
                    kind = FB_K_ISO;
                } else continue;
            }

            fb_entry_t *slot = &res->ent[res->nent];
            snprintf(slot->name, sizeof slot->name, "%s", nm);
            slot->is_dir = is_dir;
            slot->kind = kind;
            res->nent++;
        }
        if(pass == 0) rewinddir(d);
    }
    closedir(d);
    res->opened = 1;
    if(res->nent == 0 && fb_current(res->gen)){
        d = opendir(dir);
        res->opened = d != NULL;
        if(d) closedir(d);
    }
    if(!fb_current(res->gen)){ res->cancelled = 1; return; }
    if(res->nent > 1) qsort(res->ent, res->nent, sizeof res->ent[0], fb_cmp);
}

static void *fb_worker(void *arg){
    (void)arg;
    for(;;){
        char dir[FB_MAXPATH];
        fb_result_t res = {0};
        pthread_mutex_lock(&g_fb_mu);
        if(!g_fb_requested){
            g_fb_running = 0;
            pthread_mutex_unlock(&g_fb_mu);
            return NULL;
        }
        res.gen = g_fb_gen;
        snprintf(dir, sizeof dir, "%s", g_fb_path);
        pthread_mutex_unlock(&g_fb_mu);
        if(sd_io_begin()){
            fb_scan_leased(dir, &res);
            sd_io_end();
        }
        pthread_mutex_lock(&g_fb_mu);
        if(res.gen == g_fb_gen && g_fb_requested){
            g_fb_res = res;
            g_fb_ready = 1;
            g_fb_running = 0;
            pthread_mutex_unlock(&g_fb_mu);
            return NULL;
        }
        pthread_mutex_unlock(&g_fb_mu);
        free(res.ent);
    }
}

static const char *fb_basename(const char *p);
static void fb_rebuild(void);
static void fb_scan(void);
static void fb_poll_cb(lv_timer_t *t){
    /* Admission alone misses a pulled card, so a visible poll also asks for a lease: a result
     * is only taken while the medium stayed local, and a card that returns needs a fresh scan. */
    int visible = screen_current() == SCR_FOLDER;
    int admitted = visible && sd_io_begin();
    if(admitted) sd_io_end();
    if(!admitted){
        pthread_mutex_lock(&g_fb_mu);
        if(g_fb_requested){ g_fb_gen++; g_fb_requested = 0; }
        free(g_fb_res.ent); memset(&g_fb_res, 0, sizeof g_fb_res); g_fb_ready = 0;
        pthread_mutex_unlock(&g_fb_mu);
        fb_fill_stop();
        /* hidden: just drop the scan; visible: show the error once */
        if(visible && !g_fb_error_shown){
            fb_fill_stop();
            free(g_ent); g_ent = NULL; g_nent = 0; g_opened = 0;
            fb_rebuild(); g_fb_error_shown = 1;
        }
        g_fb_waiting = 1;
        lv_timer_set_period(t, 200);
        return;
    }
    if(g_fb_waiting){ fb_scan(); return; }
    fb_result_t res = {0}; int ready = 0;
    pthread_mutex_lock(&g_fb_mu);
    if(g_fb_ready && g_fb_res.gen == g_fb_gen){
        res = g_fb_res; memset(&g_fb_res, 0, sizeof g_fb_res);
        ready = 1; g_fb_ready = 0; g_fb_requested = 0;
    }
    pthread_mutex_unlock(&g_fb_mu);
    if(!ready) return;
    if(res.cancelled){
        free(res.ent); g_opened = 0; fb_rebuild(); g_fb_waiting = 1; g_fb_error_shown = 1;
        lv_timer_set_period(t, 200);
        return;
    }
    fb_fill_stop();
    free(g_ent); g_ent = res.ent; g_nent = res.nent; g_opened = res.opened;
    fb_rebuild();
    lv_timer_del(t); g_fb_poll = NULL;
}

static void fb_scan(void){
    fb_fill_stop();
    free(g_ent); g_ent = NULL; g_nent = 0;
    if(g_list) lv_obj_clean(g_list);   /* blank while loading, as before */
    if(g_title) theme_title_text(g_title, strcmp(g_dir, FB_ROOT) == 0 ? "Files" : fb_basename(g_dir));
    pthread_mutex_lock(&g_fb_mu);
    g_fb_gen++;
    free(g_fb_res.ent); memset(&g_fb_res, 0, sizeof g_fb_res); g_fb_ready = 0;
    snprintf(g_fb_path, sizeof g_fb_path, "%s", g_dir);
    g_fb_requested = 1;
    int start = !g_fb_running;
    if(start) g_fb_running = 1;
    pthread_mutex_unlock(&g_fb_mu);
    g_fb_waiting = 0;
    g_fb_error_shown = 0;
    if(!g_fb_poll) g_fb_poll = lv_timer_create(fb_poll_cb, 16, NULL);
    else lv_timer_set_period(g_fb_poll, 16);
    if(start){
        pthread_t th;
        if(pthread_create(&th, NULL, fb_worker, NULL) == 0) pthread_detach(th);
        else {
            pthread_mutex_lock(&g_fb_mu);
            g_fb_running = 0; g_fb_requested = 0;
            pthread_mutex_unlock(&g_fb_mu);
            g_opened = 0; fb_rebuild();
            lv_timer_del(g_fb_poll); g_fb_poll = NULL;
        }
    }
}
static const char *fb_basename(const char *p){
    const char *s = strrchr(p, '/');
    return (s && s[1]) ? s + 1 : p;
}

static void fb_empty_label(const char *msg){
    lv_obj_t *l = lv_label_create(g_list);
    lv_label_set_text(l, msg);
    lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_font(l, TF(UI_16), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}

static void fb_row_cb(lv_event_t *e);

static void fb_bind(row_slot_t *s, int fresh){
    if(!s || s->index < 0 || s->index >= g_nent || !g_ent) return;
    fb_entry_t *en=&g_ent[s->index];
    if(fresh){
        s->row=lv_button_create(g_list);
        lv_obj_clear_flag(s->row,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_ext_click_area(s->row,2);
        ui_on(s->row,fb_row_cb,LV_EVENT_CLICKED,s,"folderbrowser.fb_row",UI_CORE);

        s->tile = lv_obj_create(s->row);
        lv_obj_remove_style_all(s->tile);
        lv_obj_set_size(s->tile, 36, 36);
        lv_obj_set_pos(s->tile, 6, 5);
        lv_obj_set_style_radius(s->tile, 6, 0);
        lv_obj_set_style_clip_corner(s->tile, true, 0);
        lv_obj_clear_flag(s->tile, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        s->tile_img = lv_image_create(s->tile);
        lv_obj_set_size(s->tile_img, 36, 36);
        lv_obj_center(s->tile_img);
        lv_obj_clear_flag(s->tile_img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        s->tile_glyph = lv_label_create(s->row);
        lv_obj_set_style_text_font(s->tile_glyph, TF(UI_16), 0);
        lv_obj_clear_flag(s->tile_glyph, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        s->label[0] = s->tile_glyph;
        s->label[1] = indic_label_create(s->row);
        s->label[2] = lv_label_create(s->row);
    }
    row_slot_reset(s);
    lv_obj_t *r=s->row,*ic=s->label[0],*nm=s->label[1],*ch=s->label[2];
    lv_obj_set_size(r,268,46); lv_obj_set_style_radius(r,8,0);
    lv_obj_set_style_bg_color(r,TC(LIST_PRESSED),LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r,LV_OPA_70,LV_STATE_PRESSED);
    kit_row_layout(r,KIT_ROW_ICON);

    int show_art = cfg_get_int("song_art", 1);
    char bmp[600];
    int has_thumb = 0;
    if(show_art && !en->is_dir && en->kind == FB_K_AUDIO && !ui_is_fast_scrolling()){
        char full[FB_MAXPATH];
        snprintf(full, sizeof full, "%s/%s", g_dir, en->name);
        if(artcache_thumb_path(full, bmp, sizeof bmp) == 0){
            has_thumb = 1;
        }
    }

    if(has_thumb){
        char src[620];
        snprintf(src, sizeof src, "A:%s", bmp);
        if(strcmp(s->img_src, src) != 0){
            snprintf(s->img_src, sizeof s->img_src, "%s", src);
            lv_image_set_src(s->tile_img, s->img_src);
        }
        lv_obj_remove_flag(s->tile, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s->tile_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ic, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(nm, 50, 14);
        lv_obj_set_size(nm, 206, 20);
    } else {
        lv_obj_add_flag(s->tile, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ic, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ic, en->is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_AUDIO);
        lv_obj_set_pos(ic, 10, 14);
        lv_obj_set_style_text_font(ic, TF(UI_16), 0);
        lv_obj_set_style_text_color(ic, en->is_dir ? TC(FOLDER_ICON) : TC(TEXT_MUTED), 0);
        lv_obj_set_pos(nm, 40, 14);
        lv_obj_set_size(nm, en->is_dir ? 194 : 216, 20);
    }

    lv_label_set_text(nm,en->name); lv_label_set_long_mode(nm,LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(nm,TF(USER_16),0); lv_obj_set_style_text_color(nm,TC(TEXT_PRIMARY),0);
    lv_label_set_text(ch,en->is_dir?LV_SYMBOL_RIGHT:"");
    lv_obj_set_pos(ch,244,15); lv_obj_set_style_text_font(ch,TF(UI_16),0);
    lv_obj_set_style_text_color(ch,TC(TEXT_DISABLED),0);
    if(!en->is_dir) lv_obj_add_flag(ch,LV_OBJ_FLAG_HIDDEN);
    indic_row_fit(r,nm,NULL);
    theme_list_row(r);
}

static void fb_play_folder_mode(int mode){
    if(g_nent <= 0){ ui_toast("No audio files"); return; }
    if(ui_get_source_mode() == 1){ ui_toast("USB DAC mode: controls on host"); return; }
    if(ui_get_source_mode() == 4 && !ui_usb_dac_connected()){ ui_toast("USB DAC not connected"); return; }
    int n_audio = 0;
    for(int i = 0; i < g_nent; i++){
        if(!g_ent[i].is_dir && g_ent[i].kind == FB_K_AUDIO) n_audio++;
    }
    if(n_audio <= 0){ ui_toast("No audio files"); return; }
    const char **files = malloc((size_t)n_audio * sizeof *files);
    if(!files){ ui_toast("Out of memory"); return; }
    int k = 0;
    for(int i = 0; i < g_nent; i++){
        if(!g_ent[i].is_dir && g_ent[i].kind == FB_K_AUDIO) files[k++] = g_ent[i].name;
    }
    mdb_plan_t plan;
    int ok = mdb_folder_plan(g_dir, files, k, &plan);
    free(files);
    if(!ok){ ui_toast("Not in library"); return; }

    cfg_set_int("work_mode", mode);
    ui_set_workmode(mode);

    int start_id = (mode == 1 && plan.count > 1) ? plan.ids[rand() % plan.count] : plan.ids[0];
    int played = ui_play_plan(&plan, start_id);
    mdb_plan_free(&plan);
    if(played) screen_show(SCR_NOWPLAYING);
}

static void fb_play_all_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) fb_play_folder_mode(0);
}
static void fb_shuffle_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) fb_play_folder_mode(1);
}

static void fb_az_show(int on);

static void fb_jump_to_letter(char target){
    if(!g_nent || !g_first) return;
    char L = toupper((unsigned char)target);
    int idx = -1;
    int ndirs = 0;
    while(ndirs < g_nent && g_ent[ndirs].is_dir) ndirs++;
    for(int i = ndirs; i < g_nent; i++){
        if(g_first[i] == L){ idx = i; break; }
    }
    if(idx < 0 && ndirs > 0){
        for(int i = 0; i < ndirs; i++){
            if(g_first[i] == L){ idx = i; break; }
        }
    }
    if(idx < 0){
        for(int i = ndirs; i < g_nent; i++){
            if(g_first[i] >= L){ idx = i; break; }
        }
    }
    if(idx < 0) idx = g_nent - 1;
    int y = (g_has_play_header ? 52 : 0) + idx * 52;
    lv_obj_scroll_to_y(g_list, y, LV_ANIM_OFF);
    row_pool_update(&g_rows);
}

static void fb_letter_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    fb_jump_to_letter((char)(intptr_t)lv_event_get_user_data(e));
    if(g_grid) lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
}
static void fb_grid_bg_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED && g_grid)
        lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
}
static void fb_az_btn_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED && g_grid)
        lv_obj_clear_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
}
static void fb_az_show(int on){
    if(!g_az_btn || !g_grid) return;
    if(on) lv_obj_clear_flag(g_az_btn, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_add_flag(g_az_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_list){
        lv_obj_set_x(g_list, on ? 30 : 41);
        lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, on ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }
}

static void fb_lhint_timer_cb(lv_timer_t *t){
    (void)t;
    if(g_lhint && !lv_obj_has_flag(g_lhint, LV_OBJ_FLAG_HIDDEN) && lv_tick_elaps(g_lhint_tick) > 650)
        lv_obj_add_flag(g_lhint, LV_OBJ_FLAG_HIDDEN);
}

void folderbrowser_scroll_letter_tick(void){
    if(!g_list || !g_lhint || !g_lhint_lbl || g_nent <= 0 || !g_first || g_first_cap <= 0) return;
    int top = lv_obj_get_scroll_top(g_list);
    int bot = lv_obj_get_scroll_bottom(g_list);
    int total = top + bot;
    int idx = 0;
    if(total > 0){
        idx = (int)(((long long)top * g_nent) / total);
        if(idx >= g_nent) idx = g_nent - 1;
        if(idx >= g_first_cap) idx = g_first_cap - 1;
        if(idx < 0) idx = 0;
    }
    char c = g_first[idx];
    if(c == g_lhint_ch && lv_tick_elaps(g_lhint_tick) < 700) return;
    g_lhint_ch = c;
    g_lhint_tick = lv_tick_get();
    if(g_lhint && g_lhint_lbl){
        char b[2] = {c, 0};
        lv_label_set_text(g_lhint_lbl, b);
        lv_obj_remove_flag(g_lhint, LV_OBJ_FLAG_HIDDEN);
    }
}

static void fb_rebuild(void){
    fb_fill_stop();
    if(g_grid) lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
    if(g_lhint) lv_obj_add_flag(g_lhint, LV_OBJ_FLAG_HIDDEN);
    g_has_play_header = 0;

    if(g_title){
        int at_root = (strcmp(g_dir, FB_ROOT) == 0);
        theme_title_text(g_title, at_root ? "Files" : fb_basename(g_dir));
    }
    if(!g_list) return;
    lv_obj_clean(g_list);

    /* opendir failed at scan time -> either no SD or an unreadable dir. */
    if(g_nent == 0){
        fb_az_show(0);
        if(!g_opened){
            fb_empty_label(strcmp(g_dir, FB_ROOT) == 0 ? "No SD card" : "Can't open folder");
            return;
        }
        fb_empty_label("Empty folder");
        return;
    }

    if(g_nent > g_first_cap){
        char *nf = realloc(g_first, (size_t)g_nent * sizeof *nf);
        if(nf){ g_first = nf; g_first_cap = g_nent; }
    }
    if(g_first){
        for(int i = 0; i < g_nent; i++) g_first[i] = first_letter(g_ent[i].name);
    }

    /* If folder contains playable audio tracks, provide Play All and Shuffle header */
    int n_audio = 0;
    for(int i = 0; i < g_nent; i++){
        if(!g_ent[i].is_dir && g_ent[i].kind == FB_K_AUDIO) n_audio++;
    }
    if(n_audio > 0){
        lv_obj_t *hr = lv_obj_create(g_list);
        lv_obj_remove_style_all(hr);
        lv_obj_add_flag(hr, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_size(hr, 268, 46);
        lv_obj_set_pos(hr, 11, 0);
        lv_obj_clear_flag(hr, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *b_play = lv_button_create(hr);
        lv_obj_remove_style_all(b_play);
        lv_obj_set_size(b_play, 128, 44);
        lv_obj_set_pos(b_play, 2, 1);
        lv_obj_set_style_radius(b_play, 12, 0);
        lv_obj_set_style_bg_color(b_play, TC(SURFACE), 0);
        lv_obj_set_style_bg_opa(b_play, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b_play, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
        ui_on(b_play, fb_play_all_cb, LV_EVENT_CLICKED, NULL, "folderbrowser.play_all", UI_CORE);
        lv_obj_t *lp = lv_label_create(b_play);
        lv_label_set_text(lp, tr_sym(LV_SYMBOL_PLAY, "Play All"));
        lv_obj_set_style_text_font(lp, TF(UI_14), 0);
        lv_obj_set_style_text_color(lp, TC(TEXT_PRIMARY), 0);
        lv_obj_center(lp);

        lv_obj_t *b_shuf = lv_button_create(hr);
        lv_obj_remove_style_all(b_shuf);
        lv_obj_set_size(b_shuf, 128, 44);
        lv_obj_set_pos(b_shuf, 138, 1);
        lv_obj_set_style_radius(b_shuf, 12, 0);
        lv_obj_set_style_bg_color(b_shuf, TC(SURFACE), 0);
        lv_obj_set_style_bg_opa(b_shuf, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b_shuf, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
        ui_on(b_shuf, fb_shuffle_cb, LV_EVENT_CLICKED, NULL, "folderbrowser.shuffle", UI_CORE);
        lv_obj_t *ls = lv_label_create(b_shuf);
        lv_label_set_text(ls, tr_sym(LV_SYMBOL_SHUFFLE, "Shuffle"));
        lv_obj_set_style_text_font(ls, TF(UI_14), 0);
        lv_obj_set_style_text_color(ls, TC(TEXT_PRIMARY), 0);
        lv_obj_center(ls);

        g_has_play_header = 1;
    }

    lv_obj_scroll_to_y(g_list,0,LV_ANIM_OFF);
    if(!row_pool_start(&g_rows,g_list,g_nent,46,6,g_has_play_header ? 52 : 0,11,0,0,1,fb_bind)) ui_toast("Out of memory");
    fb_az_show(g_nent > 12);
}

static void fb_descend(const char *name){
    char next[FB_MAXPATH];
    int n = snprintf(next, sizeof next, "%s/%s", g_dir, name);
    if(n <= 0 || n >= (int)sizeof next){ ui_toast("Path too long"); return; }
    snprintf(g_dir, sizeof g_dir, "%s", next);
    fb_scan();
}

/* Go up one level. At the SD root, leave the screen (the header chevron is the
 * screen's back affordance there). We only ever descend from FB_ROOT, so the
 * parent is always a valid ancestor >= FB_ROOT. */
static void fb_ascend(void){
    if(strcmp(g_dir, FB_ROOT) == 0){ screen_back(); return; }
    char *slash = strrchr(g_dir, '/');
    if(!slash || slash == g_dir || (size_t)(slash - g_dir) < strlen(FB_ROOT)){
        snprintf(g_dir, sizeof g_dir, "%s", FB_ROOT);
    } else {
        *slash = 0;
    }
    fb_scan();
}

static void fb_play(const char *name){
    if(ui_get_source_mode() == 1){ ui_toast("USB DAC mode: controls on host"); return; }
    if(ui_get_source_mode() == 4 && !ui_usb_dac_connected()){ ui_toast("USB DAC not connected"); return; }
    int n_audio = 0;
    int tap_idx = 0;
    for(int i = 0; i < g_nent; i++){
        if(!g_ent[i].is_dir && g_ent[i].kind == FB_K_AUDIO){
            if(!strcmp(g_ent[i].name, name)) tap_idx = n_audio;
            n_audio++;
        }
    }
    if(n_audio > 0){
        const char **files = malloc((size_t)n_audio * sizeof *files);
        if(files){
            int k = 0;
            for(int i = 0; i < g_nent; i++){
                if(!g_ent[i].is_dir && g_ent[i].kind == FB_K_AUDIO) files[k++] = g_ent[i].name;
            }
            mdb_plan_t plan;
            int ok = mdb_folder_plan(g_dir, files, k, &plan);
            free(files);
            if(ok && plan.count > 0 && tap_idx < plan.count){
                cfg_set_int("work_mode", 0);
                ui_set_workmode(0);
                int played = ui_play_plan(&plan, plan.ids[tap_idx]);
                mdb_plan_free(&plan);
                if(played){ screen_show(SCR_NOWPLAYING); return; }
            } else if(ok){
                mdb_plan_free(&plan);
            }
        }
    }

    char full[FB_MAXPATH];
    int n = snprintf(full, sizeof full, "%s/%s", g_dir, name);
    if(n <= 0 || n >= (int)sizeof full){ ui_toast("Path too long"); return; }
    if(ui_play_song_by_path(full)) screen_show(SCR_NOWPLAYING);
}

/* Play the CUE/ISO tracks of `paths` (their SONG rows, TRACK order) through the exact queue the album drill uses. */
static void fb_play_tracks(const char *const *paths, int n){
    if(ui_get_source_mode() == 1){ ui_toast("USB DAC mode: controls on host"); return; }
    if(ui_get_source_mode() == 4 && !ui_usb_dac_connected()){ ui_toast("USB DAC not connected"); return; }
    mdb_plan_t plan;
    if(!mdb_ui_read_begin()){ ui_toast("Library busy - try again"); return; }
    int found=mdb_subtrack_plan(paths,n,&plan);
    mdb_ui_read_end();
    if(found<=0){ ui_toast(found<0 ? "Library busy - try again" : "Not in library"); return; }
    int ok = ui_play_plan(&plan, plan.ids[0]);
    mdb_plan_free(&plan);
    if(ok) screen_show(SCR_NOWPLAYING);
}
/* A .cue sheet: the scanner's own bounded FILE resolver (256 KiB, 64 files, path normalisation, no ".."/absolute) names the
 * library files whose CUE rows are the sheet's tracks. */
#define FB_CUE_FILES 64
static char g_cue_paths[FB_CUE_FILES][FB_MAXPATH];
static int g_cue_failed;
static int fb_cue_seen(const char *path){
    int n=mdb_subtrack_count(path,0);
    if(n<0) g_cue_failed=1;
    return n>0;
}
static void fb_play_sheet(const char *name){
    char full[FB_MAXPATH];
    int n = snprintf(full, sizeof full, "%s/%s", g_dir, name);
    if(n <= 0 || n >= (int)sizeof full){ ui_toast("Path too long"); return; }
    int np = 0;
    g_cue_failed=0;
    if(sd_io_begin()){ np = scan_cue_files(full, g_cue_paths, FB_CUE_FILES, fb_cue_seen); sd_io_end(); }
    if(g_cue_failed){ ui_toast("Library busy - try again"); return; }
    const char *paths[FB_CUE_FILES];
    for(int i = 0; i < np; i++) paths[i] = g_cue_paths[i];
    fb_play_tracks(paths, np);   /* np == 0: "Not in library" */
}
static void fb_play_iso(const char *name){
    char full[FB_MAXPATH];
    int n = snprintf(full, sizeof full, "%s/%s", g_dir, name);
    if(n <= 0 || n >= (int)sizeof full){ ui_toast("Path too long"); return; }
    const char *paths[1] = { full };
    fb_play_tracks(paths, 1);
}

static void fb_row_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = row_slot_index(lv_event_get_user_data(e));
    if(i < 0 || i >= g_nent) return;
    fb_entry_t *en = &g_ent[i];
    if(en->is_dir) fb_descend(en->name);
    else if(en->kind == FB_K_ISO) fb_play_iso(en->name);
    else if(en->kind == FB_K_CUE) fb_play_sheet(en->name);
    else           fb_play(en->name);
}

int folderbrowser_is_grid_open(void){
    return g_grid && !lv_obj_has_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
}

int folderbrowser_back(void){
    if(screen_current() != SCR_FOLDER) return 0;
    if(folderbrowser_is_grid_open()){
        lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
        return 1;
    }
    if(strcmp(g_dir, FB_ROOT) != 0){
        fb_ascend();
        return 1;
    }
    return 0;
}

static void fb_header_back_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED){
        if(!folderbrowser_back()) screen_back();
    }
}

void folderbrowser_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    g_title = ui_header_cb(root, "Files", fb_header_back_cb);   /* back-chevron ascends a level */

    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 41, 72);   /* its 268 px rows (5 px in) land centred on the screen */
    lv_obj_set_size(g_list, 290, 272);
    lv_obj_set_style_pad_bottom(g_list, 44, 0);   /* last row scrolls clear of the round bottom bezel */
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_list, 6, 0);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    ui_setup_scrollbar(g_list);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    /* "A-Z" button (bottom-right) opens the alphabet grid */
    g_az_btn = lv_button_create(root);
    lv_obj_remove_style_all(g_az_btn);
    lv_obj_set_pos(g_az_btn, 302, 158); lv_obj_set_size(g_az_btn, 44, 44);
    lv_obj_set_style_radius(g_az_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_az_btn, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(g_az_btn, LV_OPA_90, 0);
    ui_on(g_az_btn, fb_az_btn_cb, LV_EVENT_CLICKED, NULL, "folderbrowser.az_btn", UI_CORE);
    lv_obj_t *azl = lv_label_create(g_az_btn);
    lv_label_set_text(azl, "A-Z");
    lv_obj_set_style_text_font(azl, TF(UI_14), 0);
    lv_obj_set_style_text_color(azl, TC(TEXT_PRIMARY), 0);
    lv_obj_center(azl);
    lv_obj_add_flag(g_az_btn, LV_OBJ_FLAG_HIDDEN);

    /* alphabet grid overlay */
    g_grid = lv_obj_create(root);
    lv_obj_remove_style_all(g_grid);
    lv_obj_set_size(g_grid, 360, 360); lv_obj_set_pos(g_grid, 0, 0);
    lv_obj_set_style_bg_color(g_grid, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(g_grid, LV_OPA_80, 0);
    lv_obj_add_flag(g_grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_grid, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(g_grid, fb_grid_bg_cb, LV_EVENT_CLICKED, NULL, "folderbrowser.grid_bg", UI_CORE);
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
            ui_on(cell, fb_letter_cb, LV_EVENT_CLICKED, (void *)(intptr_t)AZ[i], "folderbrowser.letter", UI_CORE);
            lv_obj_t *l = lv_label_create(cell);
            char b[2] = {AZ[i], 0};
            lv_label_set_text(l, b);
            lv_obj_set_style_text_font(l, TF(UI_20), 0);
            lv_obj_center(l);
        }
    }

    /* alphabet hint overlay (rim-scroll / fast scroll position indicator) */
    g_lhint = lv_obj_create(root);
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
    lv_timer_create(fb_lhint_timer_cb, 150, NULL);

    snprintf(g_dir, sizeof g_dir, "%s", FB_ROOT);   /* first content built on open() */
}

void folderbrowser_open(void){
    snprintf(g_dir, sizeof g_dir, "%s", FB_ROOT);   /* always (re)enter at the SD root */
    fb_scan();
    screen_show(SCR_FOLDER);
}

lv_obj_t *folderbrowser_scroller(void){ return g_list; }
