/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef RECEIVER_H
#define RECEIVER_H
#include "ipc.h"

/* Logical IDs, never picker row indices, stock enums, or 0657 wire values.
 * Slice 2 must bind each row action explicitly (including USB Audio). */
typedef enum {
    RECEIVER_SOURCE_UNKNOWN = -1,
    RECEIVER_SOURCE_LOCAL = 0x100,
    RECEIVER_SOURCE_AIRPLAY
} receiver_source;
typedef enum {
    RECEIVER_UNCERTAIN, RECEIVER_PENDING_ARM, RECEIVER_PENDING, RECEIVER_SELECTED
} receiver_phase;
typedef struct {
    receiver_phase phase;
    receiver_source source, target;
    unsigned generation;
    uint64_t source_sequence, mode_change_sequence;
} receiver_snapshot;

/* All receiver functions run on the main thread. No frames, timers, UI or SD work.
 * Slice 2: capability refusal before mutation; other admission checks remain there. */
int receiver_available(void);
/* Receiver-side admission hook, consulted by main.c before entry and again before every disruptive frame.
 * NULL = nothing blocks entry; otherwise the toast text. A Cast session (or a waiting picker request) is checked by
 * main.c's receiver_blocked through ui_cast_phase(); this hook stays NULL (no AirPlay sender exists). */
const char *receiver_entry_blocked_reason(void);
/* Slice 2: returns 1 when ownership is reserved in PENDING_ARM, 0 on refusal.
 * Poll receiver_get_snapshot each UI tick; send NO frames until PENDING. Recheck
 * settling, Wi-Fi, storage, route and sender admission immediately before sending
 * via ipc_send_automatic with snapshot generation; handle IPC_SEND_DEFERRED.
 * Arming never waits for RX on the UI thread. The RX boundary gives receipt
 * freshness only, not command-response correlation (a607 has no request ID). */
int receiver_expect(receiver_source target);
/* Slice 2: partial send, timeout or cancelled transition requires reconciliation.
 * Never infer Local or retry entry merely because a send failed. */
void receiver_uncertain(void);
/* Slices 2-9: reconcile fresh source evidence and copy state. Invalidates on a
 * pending restart or generation change, even before main reconnect bookkeeping.
 * SELECTED means source confirmed only, not advertised/connected/playing.
 * Slices 3-4 extend this separate snapshot with metadata/art, never local tracks. */
void receiver_get_snapshot(receiver_snapshot *out);
/* Observation (default on): an UNCERTAIN receiver with no transition arms an RX boundary, and fresh AirPlay source
 * evidence after it (a607 mode 10, or a length-framed a202 with work_mode AIRPLAY) makes it SELECTED/AirPlay with
 * that frame's metadata, no frame sent. main.c turns it off while it owns a transition or a failed receiver. */
void receiver_observe(int on);
/* Read-only copy for slice 5; source lifecycle reconciliation stays main-thread.
 * Revisions describe receipt order only; stock song IDs do not order sender events. */
void receiver_get_metadata(ipc_receiver_meta *out);
/* aa05 has no item provenance or conversion result: bind by receipt only.
 * Stock parity can show an old cover for the current item; see ipc.c. */
typedef struct {
    unsigned generation;
    uint64_t session, item, revision;
    int present;
} receiver_art_key;
void receiver_art_get(receiver_art_key *out);
int receiver_art_current(const receiver_art_key *key);
int receiver_art_equal(const receiver_art_key *a, const receiver_art_key *b);
/* Worker only, under the existing decode lock; temporary output, no SD lease/cache. */
int art_receiver_decode(const receiver_art_key *key, const char *cover,
                        const char *thumb, const char *backdrop, unsigned cancel_gen);
#endif
