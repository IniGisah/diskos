/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_CAST_QUEUE_H
#define DISKOS_CAST_QUEUE_H
#include <stdint.h>
#include "musicdb.h"
#include "cast_format.h"
/* Cast queue snapshot: an immutable, compact copy of the player's live queue taken when casting starts. It stores identities
 * only (arrays of LIST_SONG_0.ID, one per occurrence, so a song queued twice is two occurrences) and fetches path and display
 * metadata for a bounded window on demand; there is no whole-library copy. Build it on a worker thread, not the LVGL thread
 * (it reads the database). Pure data after that: free of locks, safe to read from any thread. Cursor movement, repeat and
 * skipping are the next slice; this one only captures and reads. */
#define CQ_MAX_OCCURRENCES 32768   /* Per occurrence the snapshot holds 9 bytes: base 4 + (shuf 4 live | sid 4 selection) + flags 1 (the CUE/ISO bits and, in a
                                    * selection, the kind bit CQ_F_PLAYLIST: the kind rides in the existing flags byte, so 14g adds no bytes), and the
                                    * cursor adds order[] 4 more = 13 bytes = 416 KiB at the cap, 14% of the 3 MiB steady-RAM target for
                                    * all of Cast (a ~3300-song library: ~42 KiB, 3300 x 13 B = 41.9 KiB). While a new selection is built the old snapshot is still
                                    * alive (it is swapped, then freed), so the transient peak is twice that, 832 KiB, plus the caller's
                                    * 4-byte-per-occurrence id array. A refusal at the cap means something other than a normal queue,
                                    * and is CQ_TOO_LARGE, never a truncation. */
#define CQ_F_CUE 1                 /* per-occurrence flags, the inputs cast_format_src.is_cue / is_iso want */
#define CQ_F_ISO 2
#define CQ_F_PLAYLIST 4            /* SELECTION kind: sid[] is a CUSTOM_PLAYLIST.ID (a playlist occurrence), not a SONG.ID; set on every occurrence of a playlist selection */
enum { CQ_OK = 0, CQ_EMPTY, CQ_REBUILDING /* "Queue is updating - try again" */, CQ_AMBIGUOUS, CQ_TOO_LARGE, CQ_DB_ERROR };
enum { CQ_SRC_LIVE = 0, CQ_SRC_SELECTION };   /* LIVE: today's copy of the stock queue (LIST_SONG_0/3). SELECTION: built from our own SONG ids */
typedef struct {
    int src;                   /* CQ_SRC_* (0 = LIVE, so a zeroed or fabricated snapshot is a live one) */
    uint32_t generation;       /* process-wide, +1 per successful snapshot; 0 = no snapshot */
    int play_mode;             /* the player's play mode 0..4; 1 = shuffle */
    int n;                     /* occurrences */
    int cur;                   /* current occurrence: index into base[] (never found by path or title) */
    int cur_play;              /* its position in play order (== cur unless shuffled) */
    int *base;                 /* LIVE: LIST_SONG_0.ID ascending = the base order; the occurrence identity. SELECTION: synthetic 1..n, so the
                                * cursor's bsearch code runs unchanged. A SELECTION base_id is synthetic: it is NOT a player queue
                                * position or POS_ID, and must never be sent to the player as a jump target or queue position */
    int *shuf;                 /* LIVE shuffle only: LIST_SONG_3.POS_ID in play order, an exact permutation of base[]; else NULL (always NULL for SELECTION) */
    int *sid;                  /* SELECTION only: SONG.ID of each occurrence, in play (= base) order; the same song twice is two entries; else NULL */
    unsigned char *flags;      /* per base index: CQ_F_CUE | CQ_F_ISO */
} cast_queue;
/* cur_pos_id / cur_path identify the current row: pos_id first, and then the row's path must also equal cur_path (stricter
 * than mdb_upnext); with cur_pos_id 0, a path only when exactly one row has it. Worker thread only (opens a connection and can
 * wait on database locks). Free an existing snapshot with cast_queue_free before reusing *q.
 * On any result but CQ_OK, *q is empty (and safe to free). */
int  cast_queue_snapshot(cast_queue *q, int play_mode, int cur_pos_id, const char *cur_path);
/* SELECTION source: the queue is our own list of SONG.IDs (a library list from mdb_list_song_ids, a play plan's ids, or favourites),
 * start_index = the occurrence to play first, play_mode = the user's work mode 0..4 (shuf stays NULL; the cursor shuffles in memory).
 * ids is copied; each occurrence's CUE/ISO flags are read in one transaction. Worker thread only. Results: CQ_OK; CQ_EMPTY (n <= 0);
 * CQ_TOO_LARGE (n > CQ_MAX_OCCURRENCES, nothing truncated); CQ_REBUILDING (a row is gone, or the play mode is unknown); CQ_AMBIGUOUS
 * (start_index outside 0..n-1); CQ_DB_ERROR. On any result but CQ_OK, *q is empty (and safe to free). */
int  cast_queue_from_ids(cast_queue *q, const int *ids, int n, int start_index, int play_mode);
/* Slice 14g: the same with the occurrence kind. kind 0 = SONG.ID (cast_queue_from_ids), 1 = CUSTOM_PLAYLIST.ID of a playlist (mdb_playlist_rows): the ids are then
 * resolved through the playlist reader, never as SONG.IDs, and a CUSTOM_PLAYLIST.ID that equals an unrelated SONG.ID cannot pick the wrong song. Any other kind: CQ_DB_ERROR. */
int  cast_queue_from_rows(cast_queue *q, const int *ids, int n, int start_index, int play_mode, int kind);
void cast_queue_free(cast_queue *q);
/* Rows for play-order positions k .. k+n-1, clamped to the end. ord = 1-based base position; flags (optional) = CQ_F_* of each.
 * Returns the rows written (0 when n <= 0 or k is outside the queue), -CQ_REBUILDING if the live queue no longer has an
 * occurrence (SELECTION: the SONG row vanished) or its CUE/ISO kind changed since the snapshot, or -CQ_DB_ERROR; both are returned
 * negated. n is capped at CQ_PAGE_MAX. SELECTION rows carry base_id = the synthetic occurrence id (never a player queue position: do not send it to the player). */
/* Worker thread only: like the snapshot, it opens a connection and can wait on database locks. */
#define CQ_PAGE_MAX 32
int  cast_queue_page(const cast_queue *q, int k, int n, mdb_qrow_t *out, unsigned char *flags);

/* ---- Cursor (slice 11): which occurrence plays next. Pure state machine over a snapshot: no sockets, no LVGL, no LOAD sending, no
 * database. Every event returns an action for the caller to carry out; LOAD actions carry a generation (token) that the caller hands
 * back with cast_cursor_adopted / cast_cursor_failed, so events about a superseded LOAD (Next pressed while it was pending) do nothing.
 * Single thread (the session controller). The snapshot must outlive the cursor. */
#define CQ_PREV_RESTART_MS 11000   /* Previous at or past this position restarts the current item. Stock V2.57 mq_player: 0201 table
                                    * 0x838e98 -> 0x41fd70 -> 0x4ed84c -> 0x42a198; Previous 0x42a300 compares whole seconds (0x453190
                                    * truncates ms/1000) with slti 11 at 0x42a308, else seek-zero 0x42a638. Manual Previous/Next wrap in
                                    * every mode (0x424b28-0x424b34, 0x424b5c-0x424b70). Cast deliberately wraps shuffle through its
                                    * retained permutation instead of stock's regeneration/direction state (0x85f380, 0x4521bc). */
#define CQ_EARLY_REMAIN_S 10.0     /* a finish is only "early" if more than this much of the track was still to play ... */
#define CQ_EARLY_MOVED_S 2.0       /* ... and the playhead moved less than this from where we started or last sought */
enum { CA_NOTHING = 0, CA_LOAD, CA_STAY_PAUSED };
enum { CA_QUEUE_FINISHED = 1, CA_NEEDS_CHOICE };          /* reason of STAY_PAUSED */
enum { CA_C_LOAD_FAILED = 1, CA_C_EARLY_FINISH, CA_C_RECEIVER_ERROR, CA_C_EXCLUDED, CA_C_UNKNOWN_FORMAT, CA_C_NONE_PLAYABLE };   /* cause of NEEDS_CHOICE */
typedef struct {
    int kind, reason, cause;
    int k;            /* LOAD: base index of the occurrence to load. STAY_PAUSED: the occurrence that stays selected */
    int start_ms;     /* LOAD: start position */
    int skipped;      /* known-excluded occurrences passed over for this action ("Skipped 2 unsupported tracks"), any kind */
    uint32_t gen;     /* LOAD: token; STAY_PAUSED: the new current token (older ones are stale) */
} cast_action;
/* CF_V_OK / CF_V_EXCLUDED / CF_V_UNKNOWN for base index idx (slice 9 classification, may read the file); dur_ms out (0 = unknown). */
typedef int (*cast_classify_fn)(void *ctx, int idx, int *dur_ms);
typedef struct {
    const cast_queue *q; cast_classify_fn classify; void *ctx;
    int mode;                  /* 0 sequential, 1 shuffle, 2 repeat one, 3 repeat all, 4 single */
    int pos, *order;           /* the session's actual play order (base indices, a permutation) and the current position in it */
    int pend, media_id, dur_ms; /* LOAD issued but not adopted; the adopted mediaSessionId (0 = none or consumed); its duration */
    double base, prog;         /* where the playhead started or was last sought to; furthest position seen since */
    uint32_t gen; uint64_t rng;
} cast_cursor;
/* seed: tests pass a constant; production seeds from /dev/urandom (cast_http.c rand_hex is private, and the slice-1 entropy helper
 * is the TLS callback), read by the caller. Returns 0, or -1 (bad arguments, allocation, or a shuffle list that is not a permutation). */
int  cast_cursor_open(cast_cursor *c, const cast_queue *q, cast_classify_fn fn, void *ctx, uint64_t seed);
void cast_cursor_close(cast_cursor *c);
cast_action cast_cursor_play(cast_cursor *c, int start_ms);                    /* explicit selection: load the current item */
void cast_cursor_adopted(cast_cursor *c, uint32_t gen, int media_id);          /* the owned media session for LOAD gen appeared */
void cast_cursor_progress(cast_cursor *c, uint32_t gen, int media_id, double media_time);    /* MEDIA_STATUS currentTime of the owned session */
void cast_cursor_seeked(cast_cursor *c, double seconds);                       /* a SEEK was sent: progress is measured from here */
cast_action cast_cursor_finished(cast_cursor *c, uint32_t gen, int media_id, int idle_reason, double media_time);   /* CS_IDLE_* of cast_session.h */
cast_action cast_cursor_failed(cast_cursor *c, uint32_t gen);
cast_action cast_cursor_next(cast_cursor *c);
cast_action cast_cursor_prev(cast_cursor *c, int position_ms);
int  cast_cursor_set_mode(cast_cursor *c, int mode);                            /* 0 ok, -1 invalid; shuffle on keeps the current item and the played prefix (AFTER rule) */
/* Slice 14c. attach_playing: the handoff already LOADed the current occurrence, so adopt it without a second LOAD (no audible restart):
 * new generation (older events stale), nothing pending, media_id/dur_ms as given (media_id <= 0 = none: no FINISHED will match), the playhead
 * base/progress at start_ms. Returns the new generation. A FINISHED of that media then advances exactly once; a duplicate is a no-op. */
uint32_t cast_cursor_attach_playing(cast_cursor *c, int start_ms, int media_id, int dur_ms);
/* jump: explicit selection of order[pos] (an Up Next row): moves pos and loads that occurrence at 0, exactly as cast_cursor_play treats an
 * explicit choice (an excluded or unknown one is returned as the choice, never skipped). Every older generation becomes stale. pos outside
 * 0..n-1 returns CA_NOTHING and changes nothing. pos is a position in the session's play order (order[]), not a base index. */
cast_action cast_cursor_jump(cast_cursor *c, int pos);
/* start_shuffled: FULL shuffle start (owner decision 77). Mode 1 only: order[0] = the current occurrence, every other occurrence (also
 * those before it in the list) shuffled after it with the cursor's rng, pos = 0, then cast_cursor_play(start_ms). In any other mode it is
 * exactly cast_cursor_play (nothing is shuffled: the caller asked to start, not to shuffle). set_mode(1) keeps its AFTER rule. */
cast_action cast_cursor_start_shuffled(cast_cursor *c, int start_ms);
/* Slice 14f (Up Next while casting). The cursor's order[] is the order that will PLAY: it differs from the snapshot's own order (what
 * cast_queue_page pages) after set_mode(1), after start_shuffled, and for every shuffled SELECTION, so Up Next must be built from it.
 * cast_cursor_at: the base index at play position p, -1 outside 0..n-1 (or no cursor). cast_cursor_pos_of: the play position of a base index, -1 if none.
 * Pure; the cursor belongs to the controller's worker thread, so these are worker-side only. */
int  cast_cursor_at(const cast_cursor *c, int p);
int  cast_cursor_pos_of(const cast_cursor *c, int base_idx);
/* The database id a row of occurrence idx is fetched by: LIVE LIST_SONG_0.ID (base[idx]), SELECTION SONG.ID or (flags & CQ_F_PLAYLIST) CUSTOM_PLAYLIST.ID (sid[idx]); -1 for idx outside the queue. */
int  cast_queue_id_at(const cast_queue *q, int idx);
/* Display rows for an explicit list of occurrences (base indices), in the order given: a page by indices. ids / want are what cast_queue_id_at and
 * q->flags[] hold for each idx, so a caller that only has a copy of them (the published view) needs no snapshot. out[i].ord = idx[i] + 1 (SELECTION
 * base_id = idx[i] + 1, synthetic: never a player position). Returns n rows, 0 for n <= 0 / bad arguments, -CQ_REBUILDING (a row is gone, or its CUE/ISO
 * kind changed), -CQ_DB_ERROR. Reads the database in chunks of CQ_PAGE_MAX, one transaction each. */
int  cast_rows_by_ids(int src, const int *ids, const unsigned char *want, const int *idx, int n, mdb_qrow_t *out);
/* Same, taking the ids and flags from a snapshot. 0 when an index is outside it. flags (optional) = CQ_F_* of each. Worker thread only, like cast_queue_page. */
int  cast_queue_page_idx(const cast_queue *q, const int *idx, int n, mdb_qrow_t *out, unsigned char *flags);
#endif
