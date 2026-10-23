/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_REMOTE_DISCOVERY_H
#define DISKOS_REMOTE_DISCOVERY_H
#include <stddef.h>
#include <stdint.h>
#define RD_MAX 32
#define RD_LINE 4096
typedef enum { RD_CAST, RD_AIRPLAY, RD_RAOP } rd_protocol;
typedef struct {
    rd_protocol protocol;
    char id[128], name[64], model[48], addr[16];
    uint16_t port;
    int stable_id;                  /* 0 = service-name fallback, not a saved-pairing key */
    int have_features, have_flags;  /* absent and advertised zero remain distinct */
    int have_encryption, password; uint32_t encryption;
    uint64_t features, flags;       /* advertisement only, not negotiated capabilities */
} rd_receiver;
enum { RD_OPEN, RD_FINISHED, RD_EXPIRED, RD_CANCELLED, RD_ARGUMENT = -1, RD_STALE = -2 };
/* Pure, single-threaded parser: no child, socket, device or LVGL access. Feed the
 * resolved output of a supervised Avahi job for any of the three service types.
 * Only wlan0 IPv4, local-domain unicast endpoints count. Each protocol retains
 * its own identity: identical names/addresses never merge Cast with AirPlay or
 * AirPlay with RAOP. Duplicates keep position and replace advertised details.
 * Missing AirPlay deviceid uses a service-name fallback; MAC IDs are normalized.
 * All stored names are UTF-8, truncated only on complete character boundaries.
 * Overlong/malformed lines are dropped whole. No authentication is inferred. */
typedef struct {
    rd_receiver r[RD_MAX]; size_t n;
    uint64_t token; uint32_t start_ms, budget_ms;
    int state, bad; size_t used; char line[RD_LINE];
} remote_disc;
/* Start/reset with a unique nonzero caller generation and 1..30000 ms budget.
 * Invalid arguments do not change the previous attempt. Timestamps are monotonic
 * wrapping uint32 milliseconds; call at least once within a full clock wrap.
 * Feed/finish return state; STALE (another attempt) / ARGUMENT never mutate it.
 * Deadline freezes complete results and discards the unfinished line. Clean EOF
 * flushes a final line without newline. Cancel clears results. No late bytes or
 * stale cancellation can replace the latest attempt's results. The caller must
 * poll time with feed(NULL, 0) when no bytes arrive, and stop/reap its own browser
 * child on expiry/cancel. This parser does not supervise that child. */
int remote_disc_begin(remote_disc *, uint64_t token, uint32_t now_ms, uint32_t budget_ms);
int remote_disc_feed(remote_disc *, uint64_t token, uint32_t now_ms, const char *, size_t);
int remote_disc_finish(remote_disc *, uint64_t token, uint32_t now_ms);
int remote_disc_cancel(remote_disc *, uint64_t token);
#endif
