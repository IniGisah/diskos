/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_QUEUE_H
#define DISKOS_AIRPLAY_QUEUE_H
#include "airplay_ipc.h"
#include "cast_ctlw.h"
typedef struct aps_queue aps_queue;
/* The parent holds its SD lease from before start until child reap AND queue
 * join. This reader never starts the stock player or writes the library. */
aps_queue *aps_queue_start(int ctl, uint32_t attempt, const char *sd_root,
                           int mode, int pos_id, const aps_message *first);
int aps_queue_request(aps_queue *, const aps_message *);
int aps_queue_select(aps_queue *, const int *, int n, int start, int mode, int kind);
int aps_queue_mode(aps_queue *, int mode);
int aps_queue_view(aps_queue *, cw_view *);
int aps_queue_jump(aps_queue *, uint32_t qgen, int occurrence);
int aps_queue_edit(aps_queue *, uint32_t qgen, int occurrence, unsigned operation);
void aps_queue_observe(aps_queue *, const aps_message *);
void aps_queue_stop(aps_queue *);
/* 0: the current track decodes. APS_CHECK_DECODER: the stock helper could not be
 * prepared (retryable; the track is not excluded). -1: any other refusal. */
#define APS_CHECK_DECODER (-2)
int aps_queue_check(const char *root, int mode, int pos_id, const char *path,
                    int (*cancel)(void *), void *ctx, unsigned *rate, unsigned *bits);
int aps_media_open(const char *root, const char *path);
#endif
