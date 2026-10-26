/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_BOOKMETA_H
#define DISKOS_BOOKMETA_H
#include <stdint.h>
#include "scanner.h"
#define BOOKMETA_CHAPTERS 256
#define BOOKMETA_NARRATOR 128
typedef struct bookmeta bookmeta;
typedef struct {
    char path[1024]; /* exact absolute path, never truncated */
    /* Playback generation changes on each track/load, including same-path replay. */
    uint64_t playback_generation, library_revision, media_generation;
} bookmeta_key;
typedef enum {
    BOOKMETA_NONE, BOOKMETA_PENDING, BOOKMETA_COMPLETE,
    BOOKMETA_WORKER_FAILURE, BOOKMETA_FILE_CHANGED
} bookmeta_state;
typedef struct {
    bookmeta_key key;
    uint64_t request_generation;
    bookmeta_state state;
    book_meta_result_t result; /* separate chapter/narrator outcomes when COMPLETE */
    chapter_t chapters[BOOKMETA_CHAPTERS];
    char narrator[BOOKMETA_NARRATOR];
} bookmeta_snapshot;
/* All API calls belong to one UI/controller thread, with no caller lock held.
 * Create starts one worker. Thread-start failure leaves a usable error service;
 * allocation/init failure returns NULL. Destroy joins and may wait for kernel I/O. */
bookmeta *bookmeta_create(void);
void bookmeta_destroy(bookmeta *s);
/* NULL key, !is_book or empty path cancels for no-track/music. Invalid/overlong
 * paths return -1 without changing demand. Same key is a no-op, including errors.
 * Call before poll on wake/track change. Caller supplies current generations;
 * media generation comes from sdio, library revision from ipc_library_epoch(). */
int bookmeta_request(bookmeta *s, const bookmeta_key *key, int is_book);
int bookmeta_retry(bookmeta *s); /* explicit fresh request; retries thread startup */
/* Nonblocking: accepts matching mail once, retries busy dispatch, returns a sticky
 * change flag (including pending/cancel). Only call when publication is allowed.
 * No I/O or UI calls. No worker writes to the UI-owned snapshot below.
 * Pointer remains valid until destroy; contents change only on request/retry/poll.
 * Use the same snapshot for all setters within one update, then poll next update. */
int bookmeta_poll(bookmeta *s);
const bookmeta_snapshot *bookmeta_snapshot_get(const bookmeta *s);
#endif
