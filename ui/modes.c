/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include <stdint.h>
#include <stdio.h>

/* Working Mode (audio source) picker - mirrors stock's "Working mode" list. Tapping a mode replays
 * the captured switch sequence via ui_set_source_mode() and marks it selected.
 * 0=Local 1=USB-DAC 4=USB-DAC-Output 2=BT-Receiving 3=USB-Storage. */

typedef struct {
    int mode;
    const char *name;
    const char *sub;
    const char *toast;
} modeinfo_t;

static const modeinfo_t MODES[] = {
    { 0, "Local Playback",      "Play from the microSD card",          "Switching to local playback" },
    { 1, "USB DAC",             "Be a USB sound card for a PC",        "Switching to USB DAC" },
    { 4, "USB DAC Output",      "Output audio to an external USB DAC", "Switching to USB DAC output" },
    { 2, "Bluetooth Receiving", "Play audio sent from a phone",        "Switching to Bluetooth receiving" },
    { 3, "USB Storage",         "Open the card on a computer",         "Switching to USB storage" },
};
#define N_MODES ((int)(sizeof(MODES)/sizeof(MODES[0])))

static lv_obj_t *g_check[N_MODES];   /* per-row checkmark label */
static lv_obj_t *g_row[N_MODES];     /* per-row button (for the selected highlight) */

static void mark_selected_mode(int cur){
    for(int i=0;i<N_MODES;i++){
        int m = MODES[i].mode;
        if(g_check[i]){ lv_label_set_text(g_check[i], m==cur ? LV_SYMBOL_OK : "");
                        lv_obj_set_style_text_color(g_check[i], ui_current_accent(), 0); }  /* track accent changes */
        if(g_row[i]){   /* selected row gets an accent ring + slightly lifted fill */
            lv_obj_set_style_border_width(g_row[i], m==cur ? 2 : 0, 0);
            lv_obj_set_style_border_color(g_row[i], ui_current_accent(), 0);
            lv_obj_set_style_bg_color(g_row[i], lv_color_hex(m==cur ? 0x242426 : 0x1C1C1E), 0);
        }
    }
}
static void mark_selected(void){ mark_selected_mode(ui_source_switch_failed() ? -1 : ui_get_source_mode()); }


static uint32_t g_last_switch = 0;   /* debounce: a switch takes a few seconds to apply in the player */

/* Pending state: the tapped row shows a "switching" glyph (not the confirmed checkmark) while the
 * gadget switch is in flight - there is no source-mode completion readback, so after the switch
 * window we settle to the selection best-effort (matches the honest "Switching..." toast). */
static void mark_pending(int m){
    for(int i=0;i<N_MODES;i++){
        int row_m = MODES[i].mode;
        if(g_check[i]){ lv_label_set_text(g_check[i], row_m==m ? LV_SYMBOL_REFRESH : "");
                        lv_obj_set_style_text_color(g_check[i], ui_current_accent(), 0); }
        if(g_row[i]){   /* highlight the row being switched to */
            lv_obj_set_style_border_width(g_row[i], row_m==m ? 2 : 0, 0);
            lv_obj_set_style_border_color(g_row[i], ui_current_accent(), 0);
            lv_obj_set_style_bg_color(g_row[i], lv_color_hex(row_m==m ? 0x242426 : 0x1C1C1E), 0);
        }
    }
}

static lv_timer_t *g_settle = NULL;
static int g_pending_mode = -1;   /* the mode a switch is settling to (so reopening the screen keeps showing "switching") */
/* M17: after the switch window, settle to the CONFIRMED mode read from the real USB gadget state,
 * not a blind assumption. If the gadget shows the switch didn't take, reflect reality + say so. */
static void settle_cb(lv_timer_t *t){
    (void)t;
    if(ui_source_switch_pending()) return;
    if(lv_tick_elaps(g_last_switch) < 3200) return;
    lv_timer_del(g_settle); g_settle = NULL;
    int intended = g_pending_mode; g_pending_mode = -1;
    if(ui_source_switch_failed()){ mark_selected_mode(-1); return; }
    /* M17: DISPLAY the ACTUAL gadget state (read-only) instead of a blind timer assumption. Do NOT
     * mutate the intent mirror (g_source_mode) - it also guards coldplug, and a transient mid-transition
     * sample must not flip that guard. */
    int show = (intended >= 0) ? intended : ui_get_source_mode();
    if(intended == 0 || intended == 1 || intended == 3 || intended == 4){   /* USB modes are readback-confirmable */
        int actual = ui_detect_source_mode();
        show = actual;
        if(actual != intended) ui_toast("Mode didn't switch");
    }
    /* intended == 2 (BT receiving) is gadget-invisible and needs a phone to connect - no reliable
     * readback here, so show the intent without asserting a false confirmation. */
    mark_selected_mode(show);
}

static void row_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if(idx < 0 || idx >= N_MODES) return;
    int m = MODES[idx].mode;
    /* Serialise: ignore taps while the previous switch is still applying (the player's gadget
     * state-machine is asynchronous). NB we do NOT early-return on "same mode" - re-issuing must
     * always be allowed so Local works as a recover even if our cached mode is stale. */
    if(g_last_switch && lv_tick_elaps(g_last_switch) < 3000){ ui_toast("Switching..."); return; }
    g_last_switch = lv_tick_get();
    if(ui_set_source_mode(m) == 0){
        g_pending_mode = m;
        mark_pending(m);      /* async switch in flight: show "switching", not a confirmed selection */
        if(g_settle) lv_timer_del(g_settle);
        g_settle = lv_timer_create(settle_cb, 500, NULL);   /* settle to the checkmark after the switch window */
        ui_toast(MODES[idx].toast);
    }
}

void modes_open(void){
    if(g_settle && g_pending_mode >= 0) mark_pending(g_pending_mode);  /* a switch is still settling - keep showing it */
    else mark_selected();
    screen_show(SCR_WORKMODE);
}

void modes_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* back button (kept out of the clipped top-left corner) */
    ui_header(root, "Working Mode");   /* shared standard header */

    /* vertical list of mode rows */
    lv_obj_t *col = lv_obj_create(root);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 300, 250); lv_obj_set_pos(col, 30, 76);
    lv_obj_set_style_pad_bottom(col, 44, 0);   /* last mode row scrolls clear of the round bezel */
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);

    for(int i=0;i<N_MODES;i++){
        lv_obj_t *row = lv_button_create(col);
        g_row[i] = row;
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 276, 54);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1C1C1E), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x2C2C2E), LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, MODES[i].name);
        lv_obj_set_pos(nm, 16, 9);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(0xFFFFFF), 0);

        lv_obj_t *sb = lv_label_create(row);
        lv_label_set_text(sb, MODES[i].sub);
        lv_obj_set_pos(sb, 16, 30);
        lv_obj_set_width(sb, 210);                       /* keep clear of the right-side checkmark */
        lv_label_set_long_mode(sb, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(sb, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(sb, lv_color_hex(0x8E8E93), 0);

        g_check[i] = lv_label_create(row);
        lv_label_set_text(g_check[i], "");
        lv_obj_align(g_check[i], LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_set_style_text_color(g_check[i], ui_current_accent(), 0);
        lv_obj_set_style_text_font(g_check[i], &lv_font_montserrat_18, 0);
    }
    mark_selected();
}
