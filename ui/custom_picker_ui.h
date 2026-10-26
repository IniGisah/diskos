/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CUSTOM_PICKER_UI_H
#define CUSTOM_PICKER_UI_H
#include "lvgl/lvgl.h"
#include "custom_saver_picker.h"

/* SCR_CUSTOM_PICKER: the list of images in the card folder CustomCover (data model: custom_saver_picker.h). The list never selects or installs
 * anything. All UI-thread calls.
 *
 * Entry and exit go through the screen manager: custom_picker_open() is screen_show(SCR_CUSTOM_PICKER); the manager
 * calls custom_picker_enter() on every entry (opens a fresh scan and starts the step timer) and custom_picker_leave()
 * on every exit (deletes the timer, closes the scan; it keeps the built rows until the next entry). While the panel is dark the screen frees the
 * scan and the timer by itself and starts again on the first draw after the wake. */
void custom_picker_create(lv_obj_t *root);
void custom_picker_enter(void);
void custom_picker_leave(void);
void custom_picker_open(void);            /* what Custom settings (slice 17) calls */

/* A row was tapped: a PREVIEW of that entry opens inside this screen (slice 16b1). It is a sub-view of SCR_CUSTOM_PICKER, so the
 * scan, the list and its scroll position stay alive under it and Back returns to the list. The preview copies the entry (name,
 * path, size, kind) at the tap and revalidates it first; a GIF animates only while the preview is visible and the panel is lit.
 * "Use Image" calls the hook below once per tap with the copied path and entry. The default hook applies the image (slice 16b2, see
 * "Use Image: apply" in custom_picker_ui.c); NULL restores it. While an apply runs, custom_picker_back() refuses Back. */
typedef void (*custom_picker_use_cb_t)(const char *path, const csp_entry *entry);
void custom_picker_set_use_cb(custom_picker_use_cb_t cb);
int custom_picker_preview_open(void);        /* 1 while the preview is up */
int custom_picker_back(void);                /* screen_back() while this screen is current: 1 = it closed the open preview (no navigation), 0 = no preview */
int custom_picker_busy(void);                /* 1 while a Use Image apply runs: Back and the Home swipe are refused */
void custom_picker_dark(void);               /* main.c draw_set_dark(1), beside saver_dark(): the panel goes dark, close the preview now */

struct csp_scan;
struct csp_scan *custom_picker_scan(void);   /* the live scan (NULL when none): 16b reads entries/paths and revalidates */
int custom_picker_rows(void);                /* rows built so far */
#endif
