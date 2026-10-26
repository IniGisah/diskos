/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_pin.h"
#include <string.h>
int airplay_pin_edit(char digits[9], unsigned key){
    size_t n = strnlen(digits, 9); if(n > 8 || key > 10) return -1;
    if(key == 10){ if(n) digits[n - 1] = 0; return 0; }
    if(n == 8) return -1;
    digits[n] = (char)('0' + key); digits[n + 1] = 0; return 0;
}
#ifndef AIRPLAY_PIN_MODEL_ONLY
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "theme_kit.h"
#include "i18n.h"
#include <stdio.h>
static struct {
    lv_obj_t *root, *digits, *pair;
    char pin[9]; uint32_t attempt;
    void (*done)(uint32_t, const char *);
} P;
int airplay_pin_active(void){ return P.root != NULL; }
void airplay_pin_close(void){
    if(P.root) lv_obj_delete(P.root);
    /* Volatile wipe: do not retain the PIN in a static buffer after close. */
    volatile char *p = P.pin; for(unsigned i = 0; i < sizeof P.pin; i++) p[i] = 0;
    memset(&P, 0, sizeof P);
}
static lv_obj_t *label(lv_obj_t *parent, const char *s, int y, int big){
    lv_obj_t *l = lv_label_create(parent); lv_obj_set_width(l, 236); lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(l, s); lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, big ? TF(UI_18) : TF(UI_12), 0);
    lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0); lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0); return l;
}
static void key(lv_event_t *e){
    unsigned k = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    if(!P.root) return;
    if(k >= 11){
        if(k == 11 && strlen(P.pin) < 4) return;
        void (*done)(uint32_t, const char *) = P.done; uint32_t attempt = P.attempt;
        char digits[9]; memcpy(digits, P.pin, sizeof digits); airplay_pin_close();
        if(done) done(attempt, k == 11 ? digits : NULL);
        volatile char *p = digits; for(unsigned i = 0; i < sizeof digits; i++) p[i] = 0;
        return;
    }
    (void)airplay_pin_edit(P.pin, k);
    lv_label_set_text(P.digits, P.pin[0] ? P.pin : "_ _ _ _");
    if(strlen(P.pin) >= 4) lv_obj_remove_state(P.pair, LV_STATE_DISABLED); else lv_obj_add_state(P.pair, LV_STATE_DISABLED);
}
static lv_obj_t *button(const char *s, int x, int y, int w, unsigned k){
    lv_obj_t *b = lv_button_create(P.root); lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y); lv_obj_set_size(b, w, 44); lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(b, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_color(b, TC(RAISED_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b, TC(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_color(b, TC(TEXT_DISABLED), LV_STATE_DISABLED);
    ui_on(b, key, LV_EVENT_CLICKED, (void *)(uintptr_t)k, "airplay.pin.key", UI_CORE);
    lv_obj_t *l = lv_label_create(b); lv_label_set_text(l, tr(s)); lv_obj_center(l);
    lv_obj_set_style_text_font(l, k < 10 ? TF(UI_18) : TF(UI_12), 0);
    return b;
}
void airplay_pin_open(uint32_t attempt, const char *receiver, void (*done)(uint32_t, const char *)){
    airplay_pin_close(); P.attempt = attempt; P.done = done;
    P.root = lv_obj_create(lv_layer_top()); lv_obj_remove_style_all(P.root);
    lv_obj_set_size(P.root, 360, 360); lv_obj_remove_flag(P.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(P.root, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(P.root, TC(CANVAS), 0);
    lv_obj_t *title_area = lv_obj_create(P.root);
    lv_obj_remove_style_all(title_area);
    lv_obj_remove_flag(title_area, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(title_area, 0, 0);
    lv_obj_set_size(title_area, 360, 82);
    lv_obj_t *heading = ui_title_label(title_area, "Beam", TF(HEADER));
    theme_title_text(heading, tr("Beam"));
    label(P.root, tr("Enter the PIN shown on"), 66, 0);
    lv_obj_t *name = label(P.root, receiver ? receiver : "", 82, 0);
    lv_obj_set_style_text_font(name, theme_font_fixed(THEME_FONT_USER_14), 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_height(name, lv_font_get_line_height(theme_font_fixed(THEME_FONT_USER_14)));
    P.digits = label(P.root, "_ _ _ _", 104, 1);
    for(unsigned i = 0; i < 9; i++){
        char s[2] = { (char)('1' + i), 0 }; button(s, 90 + (int)(i % 3) * 62, 130 + (int)(i / 3) * 47, 56, i + 1);
    }
    button("Del", 90, 271, 56, 10); button("0", 152, 271, 56, 0);
    P.pair = button("Pair", 214, 271, 56, 11); lv_obj_add_state(P.pair, LV_STATE_DISABLED);
    lv_obj_t *cancel = button("Cancel", 110, 315, 140, 12);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
    kit_pass(P.root);
    ui_headers_fit(title_area);
    /* Lift the fixed heading band without clipping its box above the screen.
     * A cap carries its top offset in padding; other titles carry it in y. */
    int pad = lv_obj_get_style_pad_top(heading, 0), inset = pad < 8 ? pad : 8;
    lv_obj_set_style_pad_top(heading, pad - inset, 0);
    lv_obj_set_y(heading, lv_obj_get_y(heading) - 8 + inset);
    lv_obj_update_layout(title_area);
    if(theme_kit()->title_placed) theme_kit()->title_placed(heading);
}
#endif
