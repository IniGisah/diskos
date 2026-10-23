/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OTA_POLICY_H
#define OTA_POLICY_H
#include <stdint.h>

/* Persist all three strings in one transaction, independently of opt-in/reset.
 * state: "1F" = first enable, "1R" = reserved attempt; boot: canonical lowercase
 * UUID; elapsed: canonical unsigned decimal boot-relative seconds (uint64_t).
 * Each string fits a separate config value. Partial/missing fields are corrupt,
 * never absent. No wall time or lookup result belongs in this record. */
typedef struct {
    char state[3], boot[37], elapsed[21];
} ota_policy_record_t;
typedef struct {
    void *ctx;
    /* load: 1 complete record, 0 never enabled, -1 unreadable/partial record. */
    int (*load)(void *, ota_policy_record_t *);
    /* save: 0 only after durable atomic commit; any error returns nonzero. */
    int (*save)(void *, const ota_policy_record_t *);
    /* clock: 0 on success; stable boot UUID and nondecreasing elapsed seconds.
     * The adapter must use a boot-relative clock, never wall-clock time. */
    int (*clock)(void *, char boot[37], uint64_t *seconds);
} ota_policy_io_t;
typedef struct {
    ota_policy_io_t io;
    int blocked;
} ota_policy_t;
/* Single UI-thread owner. No I/O during init; storage must outlive this object. */
void ota_policy_init(ota_policy_t *p, const ota_policy_io_t *io);
/* Call after opt-in is durably enabled. Preserve established state on toggles. */
int ota_policy_enable(ota_policy_t *p);
/* Call only at an eligible idle opportunity. enabled must reflect durable
 * permission. Return 1 ONLY after reserving; failures/cancellation consume it.
 * A failed save blocks this instance, including subsequent enable calls. */
int ota_policy_reserve(ota_policy_t *p, int enabled);
/* Before a manual lookup while enabled, refresh the automatic cooldown even
 * when not due. Failure must not prevent the explicit manual lookup itself. */
int ota_policy_note_manual(ota_policy_t *p, int enabled);
/* Production callbacks: separate durable record, kernel boot ID, monotonic time. */
ota_policy_io_t ota_policy_device_io(void);
#endif
