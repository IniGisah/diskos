/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "artmgr.h"
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "artstore.h"
#include "artcache.h"
#include "netart.h"
#include "musicdb.h"
#include "sdio.h"
#include "ui_dialog.h"
#include "i18n.h"
#include "track_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

static void fmt_bytes(uint64_t bytes, char *out, int cap){
    if(bytes < 1024){
        snprintf(out, cap, "%llu B", (unsigned long long)bytes);
    } else if(bytes < 1024ULL * 1024){
        snprintf(out, cap, "%.1f KB", (double)bytes / 1024.0);
    } else if(bytes < 1024ULL * 1024 * 1024){
        snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, cap, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

void artmgr_get_info(art_cache_info_t *out){
    if(!out) return;
    memset(out, 0, sizeof *out);
    artstore_stats(&out->store_bytes, &out->store_pics, &out->store_sources);
    netart_stats(&out->netart_bytes, &out->netart_files);
    artcache_legacy_stats(&out->legacy_bytes, &out->legacy_entries);
    out->total_bytes = out->store_bytes + out->netart_bytes + out->legacy_bytes;
    out->total_songs = mdb_total_song_count();
    out->cached_songs = out->store_sources;
}

int artmgr_clear_cache(int online_only){
    artmgr_gen_stop();
    if(online_only){
        netart_clear();
    } else {
        artcache_clear_all();
        ui_art_retry();
    }
    return 0;
}

/* ---- on-demand cache generation worker ------------------------------------ */
static pthread_t g_gen_thread;
static _Atomic int g_gen_running = 0;
static _Atomic int g_gen_cancel = 0;
static art_gen_status_t g_gen_stat;
static pthread_mutex_t g_gen_mu = PTHREAD_MUTEX_INITIALIZER;
static lv_timer_t *g_timer;

int artmgr_gen_active(void){
    return atomic_load(&g_gen_running);
}

void artmgr_gen_status(art_gen_status_t *out){
    if(!out) return;
    pthread_mutex_lock(&g_gen_mu);
    *out = g_gen_stat;
    pthread_mutex_unlock(&g_gen_mu);
}

void artmgr_gen_stop(void){
    if(atomic_load(&g_gen_running)){
        atomic_store(&g_gen_cancel, 1);
    }
}

static void *art_gen_worker(void *arg){
    (void)arg;
    nice(10);

    int total = mdb_total_song_count();
    pthread_mutex_lock(&g_gen_mu);
    g_gen_stat.running = 1;
    g_gen_stat.done = 0;
    g_gen_stat.total = total;
    g_gen_stat.generated = 0;
    g_gen_stat.skipped = 0;
    g_gen_stat.failed = 0;
    g_gen_stat.cancelled = 0;
    pthread_mutex_unlock(&g_gen_mu);

    const char *PC = "/tmp/gen_cover.bmp";
    const char *PT = "/tmp/gen_thumb.bmp";
    const char *PB = "/tmp/gen_backdrop.bmp";

    int id = 0;
    char path[TRACK_PATH_MAX];
    while(!atomic_load(&g_gen_cancel) && mdb_prewarm_next(id, &id, path, sizeof path)){
        if(!path[0]) continue;
        if(!sd_io_allowed()) break;

        /* Heat throttling & live decode yielding */
        while(!atomic_load(&g_gen_cancel)){
            int temp = ui_battery_temp_dc();
            if(temp >= 420){ usleep(500 * 1000); continue; }
            if(ui_art_is_busy()){ usleep(200 * 1000); continue; }
            break;
        }
        if(atomic_load(&g_gen_cancel)) break;

        char key[24];
        int cached = artcache_lookup(path, key, sizeof key);
        if(cached == 1){
            pthread_mutex_lock(&g_gen_mu);
            g_gen_stat.done++;
            g_gen_stat.skipped++;
            pthread_mutex_unlock(&g_gen_mu);
            usleep(5 * 1000);
            continue;
        }
        if(cached < 0){
            pthread_mutex_lock(&g_gen_mu);
            g_gen_stat.done++;
            g_gen_stat.failed++;
            pthread_mutex_unlock(&g_gen_mu);
            usleep(5 * 1000);
            continue;
        }

        unsigned spec = 0;
        ui_decode_lock();
        int rc = -2;
        if(ui_decode_speculative(&spec)){
            art_resolution res;
            rc = artcache_resolve_if(key, path, PC, PT, PB, 2, spec, NULL, &res);
            if(rc == 0 && res.state == ART_PRESENT && !ui_accent_is_static()){
                char acc_fp[128] = "";
                artcache_accent_key(&res.picture, acc_fp, sizeof acc_fp);
                uint8_t *buf = malloc(148 * 148 * 4);
                if(buf){
                    uint32_t rgb;
                    if(ui_read_cover_bmp(PC, buf) == 0 && ui_accent_from_buf(buf, &rgb)){
                        mdb_set_song_accent(path, (int)(rgb & 0xFFFFFF), acc_fp);
                    }
                    free(buf);
                }
            }
        }
        ui_decode_unlock();

        pthread_mutex_lock(&g_gen_mu);
        g_gen_stat.done++;
        if(rc == 0) g_gen_stat.generated++;
        else g_gen_stat.failed++;
        pthread_mutex_unlock(&g_gen_mu);

        usleep(40 * 1000);
    }

    unlink(PC); unlink(PT); unlink(PB);

    int was_cancelled = atomic_load(&g_gen_cancel);
    pthread_mutex_lock(&g_gen_mu);
    g_gen_stat.running = 0;
    g_gen_stat.cancelled = was_cancelled;
    pthread_mutex_unlock(&g_gen_mu);

    atomic_store(&g_gen_running, 0);
    return NULL;
}

int artmgr_gen_start(void){
    if(atomic_load(&g_gen_running)) return 0;
    atomic_store(&g_gen_cancel, 0);
    atomic_store(&g_gen_running, 1);
    if(pthread_create(&g_gen_thread, NULL, art_gen_worker, NULL) != 0){
        atomic_store(&g_gen_running, 0);
        return -1;
    }
    pthread_detach(g_gen_thread);
    if(g_timer) lv_timer_resume(g_timer);
    return 0;
}

/* ---- UI screen implementation ------------------------------------------- */
static lv_obj_t *g_root;
static lv_obj_t *g_lbl_info_sub;
static lv_obj_t *g_lbl_gen_sub;
static lv_obj_t *g_lbl_gen_act;
static lv_obj_t *g_lbl_prewarm_sub;
static lv_obj_t *g_lbl_online_act;
static int g_was_running = 0;

static const char *const PREWARM_MODES[] = { "Covers only", "When idle", "Idle & charging" };

static void artmgr_back_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED){
        if(!artmgr_gen_active() && g_timer) lv_timer_pause(g_timer);
        screen_back();
    }
}

static lv_obj_t *g_dlg_scrim;

static void dlg_deleted_cb(lv_event_t *e){
    if(g_dlg_scrim == lv_event_get_target(e)) g_dlg_scrim = NULL;
}

static void dlg_close_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *scrim = (lv_obj_t *)lv_event_get_user_data(e);
    if(scrim){
        if(scrim == g_dlg_scrim) g_dlg_scrim = NULL;
        lv_obj_delete_async(scrim);
    }
}

static void artmgr_show_info_dialog(void){
    if(g_dlg_scrim) return;
    art_cache_info_t info;
    artmgr_get_info(&info);

    char s_total[32], s_store[32], s_net[32], s_leg[32];
    fmt_bytes(info.total_bytes, s_total, sizeof s_total);
    fmt_bytes(info.store_bytes, s_store, sizeof s_store);
    fmt_bytes(info.netart_bytes, s_net, sizeof s_net);
    fmt_bytes(info.legacy_bytes, s_leg, sizeof s_leg);

    ui_dialog_cfg_t cfg = { .width = 296, .margin = 4, .scrim = 1, .actions = 1, .fit_width = 1 };
    ui_dialog_t d = ui_dialog_create(lv_layer_top(), &cfg);
    g_dlg_scrim = d.scrim;
    lv_obj_add_event_cb(d.scrim, dlg_deleted_cb, LV_EVENT_DELETE, NULL);
    ui_on(d.scrim, dlg_close_cb, LV_EVENT_CLICKED, d.scrim, "artmgr.info_close", UI_CORE);

    lv_obj_t *t = ui_dialog_label(&d, tr("Artwork Cache Details"));
    lv_obj_set_style_text_font(t, TF(UI_18), 0);
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);

    lv_obj_set_style_pad_row(d.content, 6, 0);

    char line[128];
    snprintf(line, sizeof line, "%s: %s", tr("Total Cache Size"), s_total);
    lv_obj_t *l1 = ui_dialog_label(&d, line);
    lv_obj_set_style_text_font(l1, TF(UI_14), 0);
    lv_obj_set_style_text_color(l1, TC(TEXT_PRIMARY), 0);

    snprintf(line, sizeof line, "%s: %s (%d pics, %d tracks)", tr("SQLite Database"), s_store, info.store_pics, info.store_sources);
    lv_obj_t *l2 = ui_dialog_label(&d, line);
    lv_obj_set_style_text_font(l2, TF(UI_14), 0);
    lv_obj_set_style_text_color(l2, TC(TEXT_SECONDARY), 0);

    snprintf(line, sizeof line, "%s: %s (%d covers)", tr("Online Art"), s_net, info.netart_files);
    lv_obj_t *l3 = ui_dialog_label(&d, line);
    lv_obj_set_style_text_font(l3, TF(UI_14), 0);
    lv_obj_set_style_text_color(l3, TC(TEXT_SECONDARY), 0);

    if(info.legacy_entries > 0 || info.legacy_bytes > 0){
        snprintf(line, sizeof line, "%s: %s (%d entries)", tr("Legacy Cache"), s_leg, info.legacy_entries);
        lv_obj_t *ll = ui_dialog_label(&d, line);
        lv_obj_set_style_text_font(ll, TF(UI_14), 0);
        lv_obj_set_style_text_color(ll, TC(TEXT_SECONDARY), 0);
    }

    int pct = info.total_songs > 0 ? (info.cached_songs * 100) / info.total_songs : 0;
    snprintf(line, sizeof line, "%s: %d / %d songs (%d%%)", tr("Library Coverage"), info.cached_songs, info.total_songs, pct);
    lv_obj_t *l4 = ui_dialog_label(&d, line);
    lv_obj_set_style_text_font(l4, TF(UI_14), 0);
    lv_obj_set_style_text_color(l4, TC(TEXT_SECONDARY), 0);

    lv_obj_t *l5 = ui_dialog_label(&d, tr("Storage: SD Card (/.diskos)"));
    lv_obj_set_style_text_font(l5, TF(UI_14), 0);
    lv_obj_set_style_text_color(l5, TC(TEXT_MUTED), 0);

    ui_dialog_button(&d, tr("OK"), THEME_CLR_ACCENT_EMPHASIS, dlg_close_cb, "artmgr.ok");
    ui_dialog_finalize(&d);
}

static void clear_confirmed_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *scrim = (lv_obj_t *)lv_event_get_user_data(e);
    if(scrim){
        if(scrim == g_dlg_scrim) g_dlg_scrim = NULL;
        lv_obj_delete_async(scrim);
    }
    artmgr_clear_cache(0);
    artmgr_refresh();
    ui_toast(tr("Artwork cache cleared"));
}

void artmgr_confirm_clear_dialog(void){
    if(g_dlg_scrim) return;
    ui_dialog_cfg_t cfg = { .width = 280, .margin = 8, .scrim = 1, .actions = 1, .fit_width = 1 };
    ui_dialog_t d = ui_dialog_create(lv_layer_top(), &cfg);
    g_dlg_scrim = d.scrim;
    lv_obj_add_event_cb(d.scrim, dlg_deleted_cb, LV_EVENT_DELETE, NULL);
    ui_on(d.scrim, dlg_close_cb, LV_EVENT_CLICKED, d.scrim, "artmgr.clear_cancel", UI_CORE);

    lv_obj_t *t = ui_dialog_label(&d, tr("Clear Artwork Cache?"));
    lv_obj_set_style_text_font(t, TF(UI_18), 0);
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);

    lv_obj_t *b = ui_dialog_label(&d, tr("All cached album covers and thumbnails will be deleted and regenerated when viewed or on demand."));
    lv_obj_set_style_text_font(b, TF(UI_14), 0);
    lv_obj_set_style_text_color(b, TC(TEXT_MUTED), 0);

    ui_dialog_button(&d, tr("Cancel"), THEME_CLR_TEXT_SECONDARY, dlg_close_cb, "artmgr.clear_cancel");
    ui_dialog_button(&d, tr("Clear"), THEME_CLR_STATUS_WARNING, clear_confirmed_cb, "artmgr.clear_confirm");
    ui_dialog_finalize(&d);
}

static void clear_online_confirmed_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *scrim = (lv_obj_t *)lv_event_get_user_data(e);
    if(scrim){
        if(scrim == g_dlg_scrim) g_dlg_scrim = NULL;
        lv_obj_delete_async(scrim);
    }
    artmgr_clear_cache(1);
    artmgr_refresh();
    ui_toast(tr("Online artwork cleared"));
}

static void artmgr_confirm_clear_online_dialog(void){
    if(g_dlg_scrim) return;
    ui_dialog_cfg_t cfg = { .width = 280, .margin = 8, .scrim = 1, .actions = 1, .fit_width = 1 };
    ui_dialog_t d = ui_dialog_create(lv_layer_top(), &cfg);
    g_dlg_scrim = d.scrim;
    lv_obj_add_event_cb(d.scrim, dlg_deleted_cb, LV_EVENT_DELETE, NULL);
    ui_on(d.scrim, dlg_close_cb, LV_EVENT_CLICKED, d.scrim, "artmgr.online_cancel", UI_CORE);

    lv_obj_t *t = ui_dialog_label(&d, tr("Clear Online Art?"));
    lv_obj_set_style_text_font(t, TF(UI_18), 0);
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);

    lv_obj_t *b = ui_dialog_label(&d, tr("Delete all album covers downloaded from online search?"));
    lv_obj_set_style_text_font(b, TF(UI_14), 0);
    lv_obj_set_style_text_color(b, TC(TEXT_MUTED), 0);

    ui_dialog_button(&d, tr("Cancel"), THEME_CLR_TEXT_SECONDARY, dlg_close_cb, "artmgr.online_cancel");
    ui_dialog_button(&d, tr("Purge"), THEME_CLR_STATUS_WARNING, clear_online_confirmed_cb, "artmgr.online_confirm");
    ui_dialog_finalize(&d);
}

static lv_obj_t *art_card(lv_obj_t *list){
    lv_obj_t *row = lv_button_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 268, 50);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(row, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    theme_list_row(row);
    return row;
}

static void card_info_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) artmgr_show_info_dialog();
}

static void card_gen_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if(artmgr_gen_active()){
        artmgr_gen_stop();
    } else {
        if(artmgr_gen_start() == 0){
            artmgr_refresh();
        } else {
            ui_toast(tr("Couldn't start cache generation"));
        }
    }
}

static void card_clear_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) artmgr_confirm_clear_dialog();
}

static void card_clear_online_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) artmgr_confirm_clear_online_dialog();
}

static void card_prewarm_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int cur = cfg_get_int("artcache", 0);
    int next = (cur + 1) % 3;
    cfg_set_int("artcache", next);
    ui_set_prewarm_mode(next);
    artmgr_refresh();
}

static void card_online_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int cur = cfg_get_int("online_art", 0);
    int next = cur ? 0 : 1;
    cfg_set_int("online_art", next);
    if(!next) netart_cancel();
    ui_art_retry();
    artmgr_refresh();
}

static void artmgr_timer_cb(lv_timer_t *t){
    (void)t;
    if(screen_current() != SCR_ARTMGR && !artmgr_gen_active()){
        lv_timer_pause(t);
        return;
    }

    art_gen_status_t st;
    artmgr_gen_status(&st);

    if(st.running){
        g_was_running = 1;
        if(screen_current() == SCR_ARTMGR){
            if(g_lbl_gen_sub){
                char b[64];
                int pct = st.total > 0 ? (st.done * 100) / st.total : 0;
                snprintf(b, sizeof b, "%s %d / %d (%d%%)", tr("Caching"), st.done, st.total, pct);
                lv_label_set_text(g_lbl_gen_sub, b);
            }
            if(g_lbl_gen_act){
                lv_label_set_text(g_lbl_gen_act, tr("Stop"));
                lv_obj_set_style_text_color(g_lbl_gen_act, TC(STATUS_WARNING), 0);
            }
        }
    } else if(g_was_running){
        g_was_running = 0;
        artmgr_refresh();
        if(st.cancelled){
            ui_toast(tr("Cache generation stopped"));
        } else {
            char b[64];
            snprintf(b, sizeof b, "%s (%d %s)", tr("Cache generation complete"), st.generated, tr("new"));
            ui_toast(b);
        }
        if(screen_current() != SCR_ARTMGR) lv_timer_pause(t);
    } else {
        if(screen_current() != SCR_ARTMGR) lv_timer_pause(t);
    }
}

void artmgr_refresh(void){
    if(!g_root) return;

    if(g_timer){
        if(screen_current() == SCR_ARTMGR || artmgr_gen_active()) lv_timer_resume(g_timer);
        else lv_timer_pause(g_timer);
    }

    art_cache_info_t info;
    artmgr_get_info(&info);

    char s_tot[32];
    fmt_bytes(info.total_bytes, s_tot, sizeof s_tot);

    if(g_lbl_info_sub){
        char b[64];
        int pct = info.total_songs > 0 ? (info.cached_songs * 100) / info.total_songs : 0;
        snprintf(b, sizeof b, "%s  •  %d %s (%d%%)", s_tot, info.store_pics, tr("covers"), pct);
        lv_label_set_text(g_lbl_info_sub, b);
    }

    art_gen_status_t st;
    artmgr_gen_status(&st);
    if(st.running){
        if(g_lbl_gen_sub){
            char b[64];
            int pct = st.total > 0 ? (st.done * 100) / st.total : 0;
            snprintf(b, sizeof b, "%s %d / %d (%d%%)", tr("Caching"), st.done, st.total, pct);
            lv_label_set_text(g_lbl_gen_sub, b);
        }
        if(g_lbl_gen_act){
            lv_label_set_text(g_lbl_gen_act, tr("Stop"));
            lv_obj_set_style_text_color(g_lbl_gen_act, TC(STATUS_WARNING), 0);
        }
    } else {
        if(g_lbl_gen_sub){
            char b[64];
            int pct = info.total_songs > 0 ? (info.cached_songs * 100) / info.total_songs : 0;
            snprintf(b, sizeof b, "%d / %d %s (%d%%)", info.cached_songs, info.total_songs, tr("cached"), pct);
            lv_label_set_text(g_lbl_gen_sub, b);
        }
        if(g_lbl_gen_act){
            lv_label_set_text(g_lbl_gen_act, tr("Start"));
            lv_obj_set_style_text_color(g_lbl_gen_act, TC(ACCENT_PRIMARY), 0);
        }
    }

    if(g_lbl_prewarm_sub){
        int m = cfg_get_int("artcache", 0);
        if(m < 0 || m > 2) m = 0;
        lv_label_set_text(g_lbl_prewarm_sub, tr(PREWARM_MODES[m]));
    }

    if(g_lbl_online_act){
        int on = cfg_get_int("online_art", 0);
        lv_label_set_text(g_lbl_online_act, tr(on ? "On" : "Off"));
        lv_obj_set_style_text_color(g_lbl_online_act, on ? TC(ACCENT_PRIMARY) : TC(TEXT_MUTED), 0);
    }
}

void artmgr_open(void){
    artmgr_refresh();
    screen_show(SCR_ARTMGR);
    if(g_timer) lv_timer_resume(g_timer);
}

void artmgr_screen_create(lv_obj_t *root){
    g_root = root;
    ui_header_cb(root, tr("Artwork"), artmgr_back_cb);

    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 30, 70);
    lv_obj_set_size(list, 300, 250);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    /* 1. Cache Info Card */
    lv_obj_t *card_info = art_card(list);
    ui_on(card_info, card_info_cb, LV_EVENT_CLICKED, NULL, "artmgr.info", UI_CORE);
    lv_obj_t *l_it = lv_label_create(card_info);
    lv_label_set_text(l_it, tr("Cache Status"));
    lv_obj_set_pos(l_it, 18, 6);
    lv_obj_set_size(l_it, 200, 20);
    lv_label_set_long_mode(l_it, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l_it, TF(UI_16), 0);
    lv_obj_set_style_text_color(l_it, TC(TEXT_PRIMARY), 0);

    g_lbl_info_sub = lv_label_create(card_info);
    lv_label_set_text(g_lbl_info_sub, "");
    lv_obj_set_pos(g_lbl_info_sub, 18, 27);
    lv_obj_set_size(g_lbl_info_sub, 210, 18);
    lv_label_set_long_mode(g_lbl_info_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(g_lbl_info_sub, TF(UI_14), 0);
    lv_obj_set_style_text_color(g_lbl_info_sub, TC(TEXT_SECONDARY), 0);

    lv_obj_t *ch_info = lv_label_create(card_info);
    lv_label_set_text(ch_info, LV_SYMBOL_RIGHT);
    lv_obj_align(ch_info, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_text_font(ch_info, TF(UI_14), 0);
    lv_obj_set_style_text_color(ch_info, TC(TEXT_MUTED), 0);

    /* 2. On-Demand Cache Generation Card */
    lv_obj_t *card_gen = art_card(list);
    ui_on(card_gen, card_gen_cb, LV_EVENT_CLICKED, NULL, "artmgr.gen", UI_CORE);
    lv_obj_t *l_gt = lv_label_create(card_gen);
    lv_label_set_text(l_gt, tr("Generate Cache"));
    lv_obj_set_pos(l_gt, 18, 6);
    lv_obj_set_size(l_gt, 175, 20);
    lv_label_set_long_mode(l_gt, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l_gt, TF(UI_16), 0);
    lv_obj_set_style_text_color(l_gt, TC(TEXT_PRIMARY), 0);

    g_lbl_gen_sub = lv_label_create(card_gen);
    lv_label_set_text(g_lbl_gen_sub, "");
    lv_obj_set_pos(g_lbl_gen_sub, 18, 27);
    lv_obj_set_size(g_lbl_gen_sub, 175, 18);
    lv_label_set_long_mode(g_lbl_gen_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(g_lbl_gen_sub, TF(UI_14), 0);
    lv_obj_set_style_text_color(g_lbl_gen_sub, TC(TEXT_SECONDARY), 0);

    g_lbl_gen_act = lv_label_create(card_gen);
    lv_label_set_text(g_lbl_gen_act, tr("Start"));
    lv_obj_align(g_lbl_gen_act, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_text_font(g_lbl_gen_act, TF(UI_14), 0);
    lv_obj_set_style_text_color(g_lbl_gen_act, TC(ACCENT_PRIMARY), 0);

    /* 3. Clear Artwork Cache Card */
    lv_obj_t *card_clear = art_card(list);
    ui_on(card_clear, card_clear_cb, LV_EVENT_CLICKED, NULL, "artmgr.clear", UI_CORE);
    lv_obj_t *l_ct = lv_label_create(card_clear);
    lv_label_set_text(l_ct, tr("Clear Cache"));
    lv_obj_set_pos(l_ct, 18, 6);
    lv_obj_set_size(l_ct, 175, 20);
    lv_label_set_long_mode(l_ct, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l_ct, TF(UI_16), 0);
    lv_obj_set_style_text_color(l_ct, TC(TEXT_PRIMARY), 0);

    lv_obj_t *l_cs = lv_label_create(card_clear);
    lv_label_set_text(l_cs, tr("Delete all cached covers"));
    lv_obj_set_pos(l_cs, 18, 27);
    lv_obj_set_size(l_cs, 175, 18);
    lv_label_set_long_mode(l_cs, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l_cs, TF(UI_14), 0);
    lv_obj_set_style_text_color(l_cs, TC(TEXT_SECONDARY), 0);

    lv_obj_t *l_ca = lv_label_create(card_clear);
    lv_label_set_text(l_ca, tr("Clear"));
    lv_obj_align(l_ca, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_text_font(l_ca, TF(UI_14), 0);
    lv_obj_set_style_text_color(l_ca, TC(STATUS_WARNING), 0);

    /* 4. Clear Online Art Card */
    lv_obj_t *card_online_clr = art_card(list);
    ui_on(card_online_clr, card_clear_online_cb, LV_EVENT_CLICKED, NULL, "artmgr.clr_net", UI_CORE);
    lv_obj_t *l_ot = lv_label_create(card_online_clr);
    lv_label_set_text(l_ot, tr("Clear Online Art"));
    lv_obj_set_pos(l_ot, 18, 6);
    lv_obj_set_size(l_ot, 175, 20);
    lv_label_set_long_mode(l_ot, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l_ot, TF(UI_16), 0);
    lv_obj_set_style_text_color(l_ot, TC(TEXT_PRIMARY), 0);

    lv_obj_t *l_os = lv_label_create(card_online_clr);
    lv_label_set_text(l_os, tr("Delete downloaded covers"));
    lv_obj_set_pos(l_os, 18, 27);
    lv_obj_set_size(l_os, 175, 18);
    lv_label_set_long_mode(l_os, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l_os, TF(UI_14), 0);
    lv_obj_set_style_text_color(l_os, TC(TEXT_SECONDARY), 0);

    lv_obj_t *l_oa = lv_label_create(card_online_clr);
    lv_label_set_text(l_oa, tr("Purge"));
    lv_obj_align(l_oa, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_text_font(l_oa, TF(UI_14), 0);
    lv_obj_set_style_text_color(l_oa, TC(TEXT_MUTED), 0);

    /* 5. Background Prewarm Mode Card */
    lv_obj_t *card_pw = art_card(list);
    ui_on(card_pw, card_prewarm_cb, LV_EVENT_CLICKED, NULL, "artmgr.prewarm", UI_CORE);
    lv_obj_t *l_pt = lv_label_create(card_pw);
    lv_label_set_text(l_pt, tr("Background Prewarm"));
    lv_obj_set_pos(l_pt, 18, 6);
    lv_obj_set_size(l_pt, 150, 20);
    lv_label_set_long_mode(l_pt, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l_pt, TF(UI_16), 0);
    lv_obj_set_style_text_color(l_pt, TC(TEXT_PRIMARY), 0);

    g_lbl_prewarm_sub = lv_label_create(card_pw);
    lv_label_set_text(g_lbl_prewarm_sub, "");
    lv_obj_set_pos(g_lbl_prewarm_sub, 18, 27);
    lv_obj_set_size(g_lbl_prewarm_sub, 150, 18);
    lv_label_set_long_mode(g_lbl_prewarm_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(g_lbl_prewarm_sub, TF(UI_14), 0);
    lv_obj_set_style_text_color(g_lbl_prewarm_sub, TC(TEXT_SECONDARY), 0);

    lv_obj_t *ch_pw = lv_label_create(card_pw);
    lv_label_set_text(ch_pw, LV_SYMBOL_RIGHT);
    lv_obj_align(ch_pw, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_text_font(ch_pw, TF(UI_14), 0);
    lv_obj_set_style_text_color(ch_pw, TC(TEXT_MUTED), 0);

    /* 6. Online Album Art Toggle Card */
    lv_obj_t *card_net = art_card(list);
    ui_on(card_net, card_online_cb, LV_EVENT_CLICKED, NULL, "artmgr.online", UI_CORE);
    lv_obj_t *l_nt = lv_label_create(card_net);
    lv_label_set_text(l_nt, tr("Online Album Art"));
    lv_obj_set_pos(l_nt, 18, 6);
    lv_obj_set_size(l_nt, 175, 20);
    lv_label_set_long_mode(l_nt, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l_nt, TF(UI_16), 0);
    lv_obj_set_style_text_color(l_nt, TC(TEXT_PRIMARY), 0);

    lv_obj_t *l_ns = lv_label_create(card_net);
    lv_label_set_text(l_ns, tr("Fetch from iTunes if missing"));
    lv_obj_set_pos(l_ns, 18, 27);
    lv_obj_set_size(l_ns, 175, 18);
    lv_label_set_long_mode(l_ns, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l_ns, TF(UI_14), 0);
    lv_obj_set_style_text_color(l_ns, TC(TEXT_SECONDARY), 0);

    g_lbl_online_act = lv_label_create(card_net);
    lv_label_set_text(g_lbl_online_act, "");
    lv_obj_align(g_lbl_online_act, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_text_font(g_lbl_online_act, TF(UI_14), 0);

    g_timer = lv_timer_create(artmgr_timer_cb, 300, NULL);
    lv_timer_pause(g_timer);

    artmgr_refresh();
}
