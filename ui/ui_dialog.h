/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* ui_dialog.h - the shared shell for floating dialogs: a panel that is exactly as tall as its content, capped by the
 * round screen. Parts (each carries a stable identity, see ui_dialog_part_of):
 *   scrim (optional full-screen catcher) > panel > viewport (vertical scroller) > content (natural-height stack)
 *                                                > actions (optional, outside the scroller)
 * Callers choose the width and fill content/actions; they never choose the height or a y position.
 *
 * Theme: geometry belongs to this helper, appearance to the theme kit (theme_kit.c). The kit classifies the parts by
 * identity (panel = popup, scrim = scrim, wrappers = plain containers, action children = buttons), never by size, so a
 * wrapper cannot turn into a card or a row. The panel is measured only after the kit has styled the dialog (final
 * fonts, borders, external draw extent), and re-measured only when a measured quantity changes (see ui_dialog_pass). */
#ifndef UI_DIALOG_H
#define UI_DIALOG_H
#include "lvgl/lvgl.h"
#include "theme.h"

typedef enum {
    UI_DLG_NONE = 0,     /* not a dialog part */
    UI_DLG_SCRIM,
    UI_DLG_PANEL,
    UI_DLG_VIEWPORT,
    UI_DLG_CONTENT,
    UI_DLG_ACTIONS,
} ui_dialog_part_t;

typedef struct {
    int32_t width;       /* panel width in px (required) */
    int32_t margin;      /* reserve inside the circle for external drawing (clamped to >= 4) */
    int     scrim;       /* 1 = build the full-screen catcher under the panel */
    int     actions;     /* 1 = build the fixed action row below the viewport */
    int     fit_width;   /* try narrower widths before allowing body overflow */
} ui_dialog_cfg_t;

typedef struct {
    lv_obj_t *scrim;     /* NULL when cfg.scrim == 0 */
    lv_obj_t *panel;
    lv_obj_t *viewport;
    lv_obj_t *content;   /* put labels etc. here (column flex, full width) */
    lv_obj_t *actions;   /* NULL when cfg.actions == 0 (row flex, centred) */
    int32_t   width;
    int32_t   margin;
    int32_t   fit_width; /* preferred maximum width, or 0 for fixed width */
} ui_dialog_t;

/* tallest rectangle of this width that stays inside the 360 px circle with `margin` reserved; 0 if it cannot fit */
int32_t ui_dialog_circle_cap(int32_t width, int32_t margin);
/* build the parts under `parent` (the scrim, or the panel itself when there is no scrim); not yet sized in height */
ui_dialog_t ui_dialog_create(lv_obj_t *parent, const ui_dialog_cfg_t *cfg);
/* a dialog button (108x40, 4 px extra click area, raised fill, label in `col`) inside the actions row; `cb` runs on
 * CLICKED through ui_on() under `action`. Two buttons that do not fit side by side are stacked, in the same order. */
lv_obj_t *ui_dialog_button(const ui_dialog_t *d, const char *text, theme_color_role_t col, lv_event_cb_t cb,
                           const char *action);
/* a wrapped, centred label filling the content width (caller styles font and colour) */
lv_obj_t *ui_dialog_label(const ui_dialog_t *d, const char *text);
/* apply the theme to the dialog, then measure the laid-out content, size the panel to min(natural, cap), give the
 * overflow to the viewport, clamp its scroll position and centre the panel. Call after the content is final. Later
 * changes (text, visibility, width, fonts, borders) are picked up by the kit pass: see ui_dialog_pass. */
void ui_dialog_finalize(ui_dialog_t *d);
/* the dirty-flag reflow, run by the kit before every redraw: a dialog is measured again only when it is new, was
 * restyled (ui_dialog_restyled) or one of the quantities its height comes from changed (content height, actions
 * height or arrangement, width, padding, border, external draw extent). Scrolling and value changes never do. */
void ui_dialog_pass(void);
void ui_dialog_restyled(lv_obj_t *root);   /* kit_restyle(root): dialogs under root are measured again */
#ifdef DISKOS_HOST
uint32_t ui_dialog_measures(void);         /* host tests: how many times a dialog was measured */
#endif
/* which part `o` is (UI_DLG_NONE for anything else); int so the host tree dump can reference it weakly */
int ui_dialog_part_of(const lv_obj_t *o);   /* a ui_dialog_part_t value */
#endif
