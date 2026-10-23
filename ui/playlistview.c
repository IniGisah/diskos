/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "i18n.h"
#include "theme.h"
#include "theme_kit.h"
#include "musicdb.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Playlist detail (SCR_PLVIEW): opened by tapping a playlist in the Library.
 * Header name + Play / Shuffle + a horizontal 3-dot menu (Edit name / Delete).
 * Deleting removes only the playlist + its membership rows; the SONG table and
 * the files are never touched. */

static long g_pid;
static char g_name[160];
static lv_obj_t *g_title_lbl, *g_song_list;
static lv_obj_t *g_play_btn, *g_shuffle_btn;   /* LV_STATE_DISABLED when the playlist is empty */
static lv_obj_t *g_menu, *g_dialog;   /* transient popups on lv_layer_top */

/* Deletes async: callers run inside LV_EVENT_CLICKED handlers on descendants of *p, so a
 * synchronous lv_obj_del here would free the very button whose click is still being
 * dispatched (UAF once the event returns). *p is cleared immediately so no path can
 * double-close while the async delete is pending. */
static void close_pop(lv_obj_t **p){ if(*p){ lv_obj_delete_async(*p); *p = NULL; } }

/* Enable/disable the transport buttons for an empty playlist: at 0 songs Play/Shuffle would only
 * toast, so show them disabled. LV_STATE_DISABLED (not clearing CLICKABLE) so the theme kit still
 * classifies and styles them like a populated playlist's, and LVGL sends no press/click to them.
 * The disabled look (one neutral colour for both labels, faded button) is set up in plview_create. */
static void pl_set_transport_enabled(int on){
    lv_obj_t *btns[2] = { g_play_btn, g_shuffle_btn };
    for(int i=0;i<2;i++){
        if(!btns[i]) continue;
        lv_obj_t *l = lv_obj_get_child(btns[i], 0);
        if(on){ lv_obj_remove_state(btns[i], LV_STATE_DISABLED); if(l) lv_obj_remove_state(l, LV_STATE_DISABLED); }
        else  { lv_obj_add_state(btns[i], LV_STATE_DISABLED);    if(l) lv_obj_add_state(l, LV_STATE_DISABLED); }
    }
}

/* ---- song list ---------------------------------------------------------- */
static void plv_reload(void);   /* fwd */
/* Long-press a song row -> remove it from the playlist (L32). Uses the 1-based display ordinal
 * bound to the row, matching mdb_playlist_songs()'s order. */
static void plv_remove_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_LONG_PRESSED) return;
    int pos = (int)(intptr_t)lv_event_get_user_data(e);
    if(mdb_playlist_remove_at(g_pid, pos)){
        ui_toast("Removed from playlist");
        plv_reload();                 /* rebuild so ordinals stay correct */
        library_refresh();            /* playlist row count / Now-Playing add-state */
    } else ui_toast("Couldn't remove song - try again");
}
static void plv_song_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_SHORT_CLICKED) return;   /* SHORT_CLICKED so a long-press-to-remove never also plays */
    int pos = (int)(intptr_t)lv_event_get_user_data(e);   /* 1-based */
    if(ui_play_playlist(g_pid, pos)) screen_show(SCR_NOWPLAYING);   /* refused / not sent: stay (toasted) */
}
/* Gaps preserve scroll extent and row-index decoration while rows stream.
 * Theme row marks repeat every three children, so a gap keeps that phase. */
typedef struct plv_gap {
    int lo, hi;
    lv_obj_t *pad[3];
    struct plv_gap *next;
} plv_gap_t;
static plv_gap_t *g_gaps;
static mdb_song_t *g_songs;
static unsigned char g_built[8192];
static int g_fill_i, g_fill_n, g_loading;
static int g_two_line;   /* some snapshot row has both a title and an artist line */
static lv_timer_t *g_fill_timer;

/* A theme that numbers rows (Blueprint) reads the snapshot here, never the rows built so far: the row count, whether
 * any row is two-line, and a built row's 1-based position (0 = not a song row of this list). */
int playlistview_row_pos(lv_obj_t *row, int *rows, int *two_line){
    if(rows) *rows = g_fill_n;
    if(two_line) *two_line = g_two_line;
    if(!row || !g_song_list || lv_obj_get_parent(row) != g_song_list) return 0;
    return (int)(intptr_t)lv_obj_get_user_data(row);
}

static void plv_fill_stop(void){
    if(g_fill_timer){ lv_timer_del(g_fill_timer); g_fill_timer = NULL; }
    free(g_songs); g_songs = NULL;
}
static void plv_gaps_free(void){
    while(g_gaps){ plv_gap_t *next = g_gaps->next; free(g_gaps); g_gaps = next; }
}
static int plv_gap_size(plv_gap_t *g){
    int n = g->hi - g->lo;
    int pads = n ? (n % 3 ? n % 3 : 3) : 0;
    for(int j=0;j<3;j++){
        if(j >= pads){ if(g->pad[j]) lv_obj_delete(g->pad[j]); g->pad[j] = NULL; continue; }
        if(!g->pad[j]){
            g->pad[j] = lv_obj_create(g_song_list);
            lv_obj_remove_style_all(g->pad[j]);
            lv_obj_clear_flag(g->pad[j], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            kit_keep(g->pad[j]);
        }
        lv_obj_set_size(g->pad[j], 268, j ? 46 : (n - pads + 1)*52 - 6);
    }
    return pads;
}
static void plv_add_row(int i){
    if(!g_songs || g_built[i]) return;
    plv_gap_t **link = &g_gaps;
    while(*link && (*link)->hi <= i) link = &(*link)->next;
    plv_gap_t *gap = *link;
    if(!gap || i < gap->lo) return;
    plv_gap_t *right = NULL;
    if(i > gap->lo && i+1 < gap->hi){
        right = calloc(1, sizeof *right);
        if(!right) return;
        right->lo = i+1; right->hi = gap->hi; right->next = gap->next;
    }
    int index = lv_obj_get_index(gap->pad[0]);
    lv_obj_t *r = lv_button_create(g_song_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 268, 46);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, TC(LIST_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(r, (void*)(intptr_t)(i+1));   /* playlistview_row_pos */
    ui_on(r, plv_song_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)(i+1), "playlistview.plv_song", UI_CORE);
    ui_on(r, plv_remove_cb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)(i+1), "playlistview.plv_remove.long", UI_CORE);  /* L32: hold to remove */
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, g_songs[i].title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(t, 12, 5); lv_obj_set_size(t, 244, 19);
    lv_obj_set_style_text_font(t, TF(USER_16), 0);   /* CJK titles like Library/Search */
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);
    lv_obj_t *a = lv_label_create(r);
    lv_label_set_text(a, g_songs[i].artist);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(a, 12, 25); lv_obj_set_size(a, 244, 16);
    lv_obj_set_style_text_font(a, TF(USER_14), 0);
    lv_obj_set_style_text_color(a, TC(TEXT_SECONDARY), 0);
    if(!strcmp(theme_kit()->id, "terminal") && g_songs[i].dur_ms > 0){
        long long sec = ((long long)g_songs[i].dur_ms + 500) / 1000;
        char dur[16]; snprintf(dur, sizeof dur, "%lld:%02lld", sec / 60, sec % 60);
        lv_obj_t *d = lv_label_create(r);
        lv_label_set_text(d, dur);
        lv_obj_set_pos(d, 198, 17); lv_obj_set_size(d, 58, 18);
        lv_obj_set_style_text_font(d, TF(UI_14), 0);
        lv_obj_set_style_text_color(d, TC(TEXT_SECONDARY), 0);
        kit_row_value(r, t, a, d, KIT_VALUE_DURATION);
    }
    theme_list_row(r);

    g_built[i] = 1;
    if(i == gap->lo){
        gap->lo++;
        int pads = plv_gap_size(gap);
        lv_obj_move_to_index(r, index++);
        for(int j=0;j<pads;j++) lv_obj_move_to_index(gap->pad[j], index++);
        if(!pads){ *link = gap->next; free(gap); }
        return;
    }
    gap->hi = i;
    int left = plv_gap_size(gap);
    int tail = right ? plv_gap_size(right) : 0;
    for(int j=0;j<left;j++) lv_obj_move_to_index(gap->pad[j], index++);
    lv_obj_move_to_index(r, index++);
    for(int j=0;j<tail;j++) lv_obj_move_to_index(right->pad[j], index++);
    if(left){ if(right) gap->next = right; }
    else { *link = right ? right : gap->next; free(gap); }
}
static void plv_visible(void){
    if(g_loading || !g_songs) return;
    g_loading = 1;
    int first = lv_obj_get_scroll_y(g_song_list) / 52;
    if(first < 0) first = 0;
    int end = first + 7; if(end > g_fill_n) end = g_fill_n;
    for(int i=first;i<end;i++) plv_add_row(i);
    g_loading = 0;
}
static void plv_scroll_cb(lv_event_t *e){
    if(lv_event_get_code(e)==LV_EVENT_SCROLL && screen_current()==SCR_PLVIEW) plv_visible();
}
static void plv_fill_cb(lv_timer_t *t){
    (void)t;
    if(screen_current()!=SCR_PLVIEW){ plv_fill_stop(); return; }
    uint32_t start = lv_tick_get();
    int end = g_fill_i + 40; if(end > g_fill_n) end = g_fill_n;
    g_loading = 1;
    do { plv_add_row(g_fill_i++); }
    while(g_fill_i < end && lv_tick_elaps(start) < 4);
    g_loading = 0;
    if(g_fill_i >= g_fill_n) plv_fill_stop();
}
static void plv_reload_read(void){
    theme_title_text(g_title_lbl, g_name);
    if(!g_song_list) return;
    g_loading = 1;
    plv_fill_stop();
    plv_gaps_free();
    lv_obj_clean(g_song_list);   /* existing entry/removal scroll reset */
    memset(g_built, 0, sizeof g_built);
    g_fill_i = g_fill_n = g_two_line = 0;
    int cnt = mdb_playlist_count(g_pid);
    /* Display cap only; playback and Shuffle still use the full playlist. */
    if(cnt > 8192) cnt = 8192;
    g_songs = cnt > 0 ? malloc((size_t)cnt * sizeof *g_songs) : NULL;
    int n = g_songs ? mdb_playlist_songs(g_pid, g_songs, cnt) : 0;
    if(n > 0) g_gaps = calloc(1, sizeof *g_gaps);
    if(n > 0 && !g_gaps) n = 0;
    pl_set_transport_enabled(n > 0);
    if(n<=0){
        plv_fill_stop();
        lv_obj_t *l = lv_label_create(g_song_list);
        lv_label_set_text(l, "Empty playlist\nAdd songs from Now Playing");
        lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0);
        lv_obj_set_style_text_font(l, TF(UI_14), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        g_loading = 0;
        return;
    }
    g_fill_n = n;
    for(int i=0;i<n && !g_two_line;i++) g_two_line = g_songs[i].title[0] && g_songs[i].artist[0];
    g_gaps->hi = n;
    plv_gap_size(g_gaps);
    int first = n < 18 ? n : 18;
    for(;g_fill_i<first;g_fill_i++) plv_add_row(g_fill_i);
    g_loading = 0;
    if(g_fill_i < n) g_fill_timer = lv_timer_create(plv_fill_cb, 16, NULL);
    else plv_fill_stop();
    { static int hinted = 0; if(!hinted) hinted = ui_toast_hint("Hold a song to remove it"); }
}
static void plv_reload(void){
    static long shown_pid;
    static char shown_name[sizeof g_name]="Playlist";
    if(!mdb_ui_read_begin()){
        g_pid=shown_pid; snprintf(g_name,sizeof g_name,"%s",shown_name);
        ui_toast("Library busy - reopen to refresh"); return;
    }
    plv_reload_read();
    shown_pid=g_pid; snprintf(shown_name,sizeof shown_name,"%s",g_name);
    mdb_ui_read_end();
}
/* Public: rebuild the list from the DB. Called by the screen manager on EVERY entry
 * to SCR_PLVIEW (incl. back-nav), so tap positions can't go stale after the playlist's
 * membership changes elsewhere (e.g. add-to-playlist from Now Playing). */
void plview_refresh(void){ plv_reload(); }

void plview_open(long pid, const char *name){
    g_pid = pid;
    snprintf(g_name, sizeof g_name, "%s", name ? name : "Playlist");
    screen_show(SCR_PLVIEW);   /* transition() calls plview_refresh() -> plv_reload() */
}

/* ---- play / shuffle ----------------------------------------------------- */
static void play_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int count=mdb_playlist_count(g_pid);
    if(count < 1){ ui_toast(count<0 ? "Library busy - try again" : "Playlist is empty"); return; }
    ui_set_workmode(0);
    if(ui_play_playlist(g_pid, 1)) screen_show(SCR_NOWPLAYING);
}
static void shuffle_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int c = mdb_playlist_count(g_pid);
    if(c < 1){ ui_toast(c<0 ? "Library busy - try again" : "Playlist is empty"); return; }
    srand(lv_tick_get());
    ui_set_workmode(1);                       /* shuffle play-mode */
    if(ui_play_playlist(g_pid, rand()%c + 1)) screen_show(SCR_NOWPLAYING);   /* + random start track */
}

/* ---- delete confirm ----------------------------------------------------- */
static void del_yes_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    close_pop(&g_dialog);
    int ok = mdb_playlist_delete(g_pid);
    if(ok){
        ui_toast("Playlist deleted");
        screen_back();                        /* back to the Playlists list */
        library_refresh();                    /* rebuild it so the deleted playlist is gone */
    } else {
        ui_toast("Delete failed");            /* stay put on failure */
    }
}
static void del_no_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) close_pop(&g_dialog); }

static lv_obj_t *card(int w, int h){
    /* full-screen modal backdrop on the top layer: CATCHES taps so they can't reach the playlist
     * controls beneath (this confirm was a bare floating card before). Stored as g_dialog so
     * close_pop removes the backdrop AND the card child together. No dismiss-on-tap - use the buttons. */
    lv_obj_t *bg = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bg);
    lv_obj_set_size(bg, 360, 360);
    lv_obj_center(bg);
    lv_obj_set_style_bg_color(bg, TC(SCRIM), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_50, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    g_dialog = bg;
    lv_obj_t *c = lv_obj_create(bg);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_center(c);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_bg_color(c, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, TC(BORDER), 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}
static lv_obj_t *card_btn(lv_obj_t *p, int y, int w, const char *txt, lv_color_t bg, lv_color_t fg, lv_event_cb_t cb){
    lv_obj_t *b = lv_button_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, 40);
    lv_obj_set_ext_click_area(b, 4);   /* 40px pill -> ~48px touch target on the capacitive panel */
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    ui_on(b, cb, LV_EVENT_CLICKED, NULL, "playlistview.cb", UI_CORE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, TF(UI_16), 0);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_center(l);
    return b;
}
static void show_delete_confirm(void){
    close_pop(&g_menu);
    lv_obj_t *dc = card(264, 200);   /* card() sets g_dialog = the full-screen backdrop; content goes on dc */
    lv_obj_t *t = lv_label_create(dc);
    lv_label_set_text(t, "Delete playlist?");
    lv_obj_set_style_text_font(t, TF(UI_20), 0);
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_t *s = lv_label_create(dc);
    lv_label_set_text(s, "The songs are kept.");
    lv_obj_set_style_text_font(s, TF(UI_14), 0);
    lv_obj_set_style_text_color(s, TC(TEXT_MUTED), 0);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 54);
    card_btn(dc, 88,  200, "Delete", TC(DANGER_BUTTON_SURFACE), TC(STATUS_DANGER), del_yes_cb);
    card_btn(dc, 134, 200, "Cancel", TC(SURFACE_RAISED), TC(TEXT_PRIMARY), del_no_cb);
}

/* ---- rename ------------------------------------------------------------- */
static void rename_done(const char *name){
    if(!name || !name[0]) return;
    if(strlen(name) >= sizeof g_name){ ui_toast("Name too long"); return; }
    if(mdb_playlist_rename(g_pid, name)){
        snprintf(g_name, sizeof g_name, "%s", name);
        theme_title_text(g_title_lbl, g_name);
        library_refresh();   /* rebuild the Playlists list so its row shows the new name (delete already does this) */
        ui_toast("Renamed");
    } else {
        ui_toast("Rename failed");
    }
}
static void rename_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    close_pop(&g_menu);
    kbinput_open_limited("Playlist name", g_name, rename_done, sizeof g_name-1);
}

/* ---- 3-dot menu --------------------------------------------------------- */
static void menu_dismiss_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) close_pop(&g_menu); }
static void del_menu_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) show_delete_confirm(); }
static void export_cb(lv_event_t *e){   /* L37: write the playlist to /tmp/sdcard/<name>.m3u */
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    close_pop(&g_menu);
    char fn[112];
    if(mdb_playlist_export(g_pid, g_name, fn, sizeof fn)){
        char msg[160]; snprintf(msg, sizeof msg, "Exported to %s", fn);
        ui_toast(msg);
    } else ui_toast("Couldn't export playlist");
}
static void open_menu_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    close_pop(&g_menu);
    /* full-screen catcher so a tap outside dismisses */
    g_menu = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_menu);
    lv_obj_set_size(g_menu, 360, 360);
    lv_obj_center(g_menu);
    lv_obj_set_style_bg_color(g_menu, TC(SCRIM), 0);
    lv_obj_set_style_bg_opa(g_menu, LV_OPA_50, 0);
    lv_obj_add_flag(g_menu, LV_OBJ_FLAG_CLICKABLE);
    ui_on(g_menu, menu_dismiss_cb, LV_EVENT_CLICKED, NULL, "playlistview.menu_dismiss", UI_CORE);
    lv_obj_t *c = lv_obj_create(g_menu);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 240, 176);
    lv_obj_center(c);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_bg_color(c, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    card_btn(c, 12,  216, "Edit Name",       TC(SURFACE_RAISED), TC(TEXT_PRIMARY), rename_cb);
    card_btn(c, 64,  216, "Export to SD",    TC(SURFACE_RAISED), TC(STATUS_INFO), export_cb);
    card_btn(c, 116, 216, "Delete Playlist", TC(SURFACE_RAISED), TC(STATUS_DANGER), del_menu_cb);
}

static void back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

/* ---- screen ------------------------------------------------------------- */
static lv_obj_t *icon_btn(lv_obj_t *root, int x, int y, int w, const char *sym,
                          lv_color_t fg, lv_event_cb_t cb){
    lv_obj_t *b = lv_button_create(root);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, 40);
    lv_obj_set_ext_click_area(b, 6);   /* easier hit, esp. the 36px menu button */
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(b, cb, LV_EVENT_CLICKED, NULL, "playlistview.cb", UI_CORE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_width(l, w - 8);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_center(l);
    return b;
}
void plview_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    g_title_lbl = ui_header_cb(root, "Playlist", back_cb);   /* shared header; dynamic title = playlist name */
    lv_obj_set_style_text_align(g_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    if(!ui_edge_nav()) lv_obj_align(g_title_lbl, LV_ALIGN_TOP_MID, 0, 30);   /* edge-nav themes place their own */

    /* Play / Shuffle / 3-dots row */
    /* 84 + 6 + 108 + 6 + 36 = 240, centred (60..300): Shuffle is the longer word, and holds it in a monospace face */
    /* Play is the go colour: Default's green, a unique theme's own accent */
    g_play_btn    = icon_btn(root, 60,  68, 84,  tr_sym(LV_SYMBOL_PLAY, "Play"),   theme_def() ? TC(ACCENT_PRIMARY) : TC(STATUS_SUCCESS), play_cb);
    g_shuffle_btn = icon_btn(root, 150, 68, 108, tr_sym(LV_SYMBOL_SHUFFLE, "Shuffle"), TC(TEXT_PRIMARY), shuffle_cb);
    /* disabled (empty playlist): one neutral colour for both - 40% of a dark accent read as dead while
     * 40% of white still read as live, so the pair looked mismatched */
    lv_obj_t *tb[2] = { g_play_btn, g_shuffle_btn };
    for(int i=0;i<2;i++){
        lv_obj_set_style_opa(tb[i], LV_OPA_60, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(lv_obj_get_child(tb[i], 0), TC(TEXT_DISABLED), LV_STATE_DISABLED);
    }
    /* "more/options" (edit/rename/delete). Drawn "three dots" - Apple-Music-style More - because the
     * bullet glyph isn't in this tree's montserrat; LV_SYMBOL_LIST misread as a track list. */
    lv_obj_t *menu_btn = icon_btn(root, 264, 68, 36, "", TC(TEXT_SECONDARY), open_menu_cb);
    for(int i=0;i<3;i++){
        lv_obj_t *d = lv_obj_create(menu_btn);
        lv_obj_remove_style_all(d);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);   /* decorative: let taps fall through to the button */
        lv_obj_set_size(d, 4, 4);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, TC(TEXT_SECONDARY), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_align(d, LV_ALIGN_CENTER, (i-1)*7, 0);   /* -7 / 0 / +7 px */
    }

    g_song_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_song_list);
    lv_obj_set_pos(g_song_list, 41, 118);   /* its 268 px rows (5 px in) land centred on the screen */
    lv_obj_set_size(g_song_list, 290, 226);
    lv_obj_set_style_pad_bottom(g_song_list, 44, 0);   /* last row scrolls clear of the round bottom bezel */
    lv_obj_set_style_bg_opa(g_song_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_song_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_song_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_song_list, 6, 0);
    lv_obj_set_scroll_dir(g_song_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_song_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_song_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_event_cb(g_song_list, plv_scroll_cb, LV_EVENT_SCROLL, NULL);
}

lv_obj_t *playlistview_scroller(void){ return g_song_list; }
