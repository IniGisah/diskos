/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPUSTITLE_WORKER_H
#define OPUSTITLE_WORKER_H
#include <stdint.h>
#include "opustitle.h"
typedef struct opus_title_worker opus_title_worker;
typedef struct {
    uint64_t player_generation, song_id, pos_id;
    unsigned song_id_valid, pos_id_valid;
} otw_identity;
enum { OTW_KIND_KNOWN=1, OTW_CUE=2, OTW_SACD=4 };
typedef struct {
    otw_identity identity;
    uint64_t identity_epoch, serial, media_epoch;
    unsigned flags;
    char path[1024];
} otw_key;
enum { OTW_FAILURE_NONE, OTW_FAILURE_TIMEOUT, OTW_FAILURE_WORKER };
typedef struct {
    otw_key key;
    opus_title_result result; /* selected title, including original on failure */
    unsigned failure;
} otw_commit;
/* All controller calls belong to ONE caller thread; no caller lock may be held.
 * clock returns monotonic milliseconds, must be thread-safe; NULL uses CLOCK_MONOTONIC.
 * Creation starts the only worker. Failure still yields a usable fallback controller.
 * NULL means allocation/init failure. Destroy joins; a stalled kernel read can delay it. */
opus_title_worker *otw_create(uint64_t (*clock)(void *), void *ctx);
void otw_destroy(opus_title_worker *w);
/* Copied exact strings: returns -1 for NULL/oversized input, otherwise 0.
 * Repeated identity metadata cannot thaw a commitment. Before commitment a genuine
 * title can win. Evidence changes invalidate pending work, preserving its deadline;
 * missing kind evidence waits for restoration or timeout. Committed titles stay frozen.
 * Eligibility requires only KIND_KNOWN, an absolute path, a case-insensitive .opus,
 * .oga or .ogg extension, and exact (case-sensitive) full basename/title equality.
 * Unknown flags wait without reading until restoration or expiry.
 * Empty candidate titles wait too; neither can suppress a genuine title.
 * The caller must supply complete successfully decoded strings.
 * Reader NOT_APPLICABLE commits the original stock player title once. Successful submit never waits for a lock. */
int otw_submit(opus_title_worker *w, otw_identity identity, const char *path,
               const char *player_title, unsigned flags);
void otw_identity_end(opus_title_worker *w); /* no-track or observed load boundary */
void otw_player_generation(opus_title_worker *w, uint64_t generation);
void otw_library_commit(opus_title_worker *w, uint64_t media_epoch);
/* Call regularly while pending: retries dispatch if mailbox mutex was busy.
 * Returns one selection per identity unless publication rejects it; now uses the clock's time base. */
int otw_poll(opus_title_worker *w, uint64_t now, otw_commit *out);
/* Publication rejected this provisional selection. Reopen the same identity with
 * its original title/deadline so the caller can submit newer copied evidence. */
void otw_reject_commit(opus_title_worker *w);
int otw_title_ready(const opus_title_worker *w);
#endif
