/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_CAST_CTLW_H
#define DISKOS_CAST_CTLW_H
#include <stdint.h>
#include "cast_ctl.h"
/* Cast controller worker (slice 14e1): ONE thread that owns a cast_ctl and everything that touches the database or reads a file for it
 * (queue snapshot, row lookups, classification, the shuffle walk). The LVGL thread talks to it through two bounded mailboxes and never
 * waits for it; the worker never calls the Cast session, the bridge or LVGL (no cast_session_*, no cb_*): it only turns events into
 * actions, and the LVGL thread executes the actions.
 * Thread: created lazily by the first cw_start (the first ENTER), then left PARKED on a condition variable for the life of the process
 * (a join on LOCAL could block the LVGL thread behind a slow database or SD read). cw_destroy joins it; production never calls it,
 * the host tests do (ASan leak check).
 * Event mailbox (LVGL -> worker): CW_EV_N ordinary slots plus one LOAD-result slot; post never blocks.
 *   SNAPSHOT coalesces into a tail SNAPSHOT. SNAPSHOT, NEXT, PREV and JUMP are droppable.
 *   Other ordinary events evict the oldest droppable event when full. With none available they are refused;
 *   LOCAL also sets a force-local flag, so teardown cannot be lost.
 *   LOAD_STARTED / LOAD_REFUSED always use the separate result slot. A newer result replaces an older one:
 *   results arrive in LOAD-action order and only the newest LOAD can still be pending. LOCAL clears the slot.
 * Action mailbox (worker -> LVGL), CW_ACT_N slots, rows copied (a.row points into the slot): REPUBLISH coalesces into a queued
 *   REPUBLISH, a TOAST equal to the one before it is dropped, everything else (LOAD, PLAY_AFTER_ADOPT, STAY_PAUSED, STOP_MEDIA) is
 *   kept: when it is full the WORKER waits for the LVGL thread to drain (only the worker ever waits).
 * SELECT (slice 14e2): cw_post COPIES e->ids (n <= CQ_MAX_OCCURRENCES, else refused with -1: never truncated) into a malloc'd array owned by
 *   the mailbox slot; the worker frees it after the step, cw_destroy frees the ones never taken. SELECT is never dropped like a droppable.
 * Epoch (14e2 C2): a posted LOCAL ends the cast: every queued action is discarded, actions of events posted before it are never queued, and
 *   events posted before it (other than LOCAL) are skipped by the worker, so nothing of a finished cast executes after the next ENTER.
 * Time: every posted event is stamped with now() (production: lv_tick_get) so the controller's LOAD deadline runs on the LVGL clock. */
#define CW_EV_N 16
#define CW_EV_DROP 8
#define CW_ACT_N 8
typedef struct { cc_act a; cc_row row; } cw_act;   /* a.row (when set) points at this slot's row */
typedef struct cw cw;
/* Production deps: classify = cast_format on the file's bytes through an sd_io lease (the 13b bridge technique, with the library's CUE/ISO
 * flags), row = the database default, seed from /dev/urandom, load_ms as given. root: the SD mount (no trailing slash), must outlive the worker. */
void cw_deps_sd(cast_ctl_deps *d, const char *root, int load_ms);
/* NULL on failure. now: millisecond clock stamped onto events. */
cw  *cw_start(const cast_ctl_deps *d, uint32_t (*now)(void));
void cw_destroy(cw *w);                                  /* tests: tear down, join, free */
/* The start index of a selection: pos1 = the 1-based position the caller computed (< 1 = the first), tap_id = the tapped SONG.ID (0 = unknown).
 * With a tap_id the song AT that index is used if it is the tapped one, else the tapped song is looked up; -1 when it is not in the list (or
 * the position is outside it with no tap_id, or the list is empty): the caller refuses, it never starts at index 0 by default. Pure. */
int  cw_start_index(const int *ids, int n, int pos1, int tap_id);
/* LVGL side. Returns 0 queued, 1 coalesced into a queued SNAPSHOT, -1 refused (droppable and no room, a full mailbox, or a SELECT with no ids / too many). */
int  cw_post(cw *w, const cc_ev *e);
/* ENTER: the worker takes the LIVE snapshot itself (e->q is ignored): play_mode, the handoff's pos_id and path. */
int  cw_post_enter(cw *w, const cc_ev *e, int play_mode, int pos_id, const char *path);
int  cw_take(cw *w, cw_act *out);                        /* 1 = one action copied out, 0 = none; never blocks */
int  cw_wait_idle(cw *w, int ms);                        /* tests: 1 when the event mailbox is empty and the worker is between events */
void cw_counts(cw *w, unsigned *coalesced, unsigned *dropped, unsigned *lost);
/* Published VIEW (slice 14f). The cursor (order[], pos) and the snapshot belong to the worker; the LVGL thread never reads them. After every worker
 * step the worker builds a cw_view (the queue generation, the cursor position and mode, and the next CW_VIEW_N occurrences in PLAY order from pos, each
 * with the database id and CUE/ISO flags its row is fetched by) on its own stack and copies it into the worker under the mutex: a memcpy of
 * sizeof(cw_view) (about 1 KiB), no allocation and no database access under the lock. Up Next reads only this copy. occ[] = base indices; pos is the
 * position of occ[0] in play order; cnt = min(CW_VIEW_N, n_queue - pos). Not active = no cast is up (or ENTER has not finished): "Queue updating...". */
#define CW_VIEW_N 101   /* the current occurrence + Up Next's UN_SHOW (100) */
typedef struct {
    int active, pos, mode, n_queue, src, cnt;   /* src: CQ_SRC_* of the snapshot */
    uint32_t qgen;
    int occ[CW_VIEW_N], id[CW_VIEW_N];
    unsigned char fl[CW_VIEW_N];
} cw_view;
int  cw_view_get(cw *w, cw_view *out);                   /* LVGL side: copy under the mutex; 1 = active view copied, 0 = none (out zeroed) */
int  cw_view_same(const cw_view *a, const cw_view *b);   /* the Up Next stale check: both active, same qgen, pos and mode. Pure */
int  cw_jump(cw *w, uint32_t qgen, int occ);             /* LVGL side: post a JUMP (never blocks). 0 queued, -1 refused (mailbox full / closing): "Queue is updating - try again" */
void cw_set_ui(cw *w);                                   /* the worker the screens talk to (main.c sets it after cw_start); NULL when none */
cw  *cw_ui(void);
#endif
