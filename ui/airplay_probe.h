/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_PROBE_H
#define DISKOS_AIRPLAY_PROBE_H
#include <stdint.h>
#include <sys/types.h>
#include "cast_handoff.h"
/* AirPlay proof supervisor (proof builds only). One helper child on a private control channel (SOCK_SEQPACKET, fd 3 in the child).
 * Frame = type, attempt (u32 LE), payload. Parent -> child: 'N' PIN digits,
 * 'G' go (eight hex digits of start milliseconds + approved descriptor), 'C' cancel.
 * Child -> parent: 'P' PIN wanted, 'R' ready, 'S' streaming, 'D' done, 'F' failed. A frame with another attempt is dropped.
 *
 *   AP_PREP: helper pairs and prepares while the Disc keeps playing; its own deadline (a typed PIN must not race HO_PREP_MS);
 *   'R' -> a FRESH local snapshot starts cast_handoff (SELECT + REMOTE_READY): ONE local pause, START only after ho_step has the
 *   qualified local state and the strict PCM closure that ho_drain_step proves after ONE guarded Local teardown (V2.57 pause keeps PCM running). AP_RUN: the handoff owns the session (ho is the caller's, so the 16b guards
 *   read it). An SD lease is taken before the file is opened and held until the child is REAPED; a child that cannot be reaped
 *   faults SD admission and is never reported drained. */
#define AP_PREP_MS 180000u   /* pairing + typed PIN; device-tune */
typedef struct {
    const char *helper; const char *const *extra;   /* helper path; optional NULL-terminated extra argv (after --ctl 3 --attempt N) */
    int  (*pause)(void);                 /* the one local toggle */
    int  (*cast_busy)(void);             /* a Cast request is waiting (the machine itself is read directly) */
    void (*say)(ho_msg m);
    int  (*reap)(pid_t pid);             /* 1 = reaped (default waitpid WNOHANG); test seam */
    uint32_t prep_ms, term_ms, kill_ms, fault_ms;   /* 0 = defaults: AP_PREP_MS, 1500, 1500, 2000 */
    int  (*teardown)(unsigned generation);   /* one guarded Local reselect (modes_local_teardown): 0 queued, -1 failed, IPC_SEND_DEFERRED pending */
} ap_cfg;
typedef enum { AP_IDLE = 0, AP_PREP, AP_RUN } ap_phase;
typedef struct {
    ho *h; ap_cfg cfg; ap_phase phase;
    pid_t pid; int ctl, lease, pin_wanted, termed, faulted;   /* termed: 0 none, 1 cancel sent, 2 SIGTERM, 3 SIGKILL */
    uint32_t now, serial, deadline, term_at, kill_at, fault_at, hgen; int hsrc;
} ap_probe;
void ap_init(ap_probe *a, ho *h, const ap_cfg *c);
int  ap_start(ap_probe *a, uint32_t now, const ho_local *l);   /* 0 = helper started; < 0 refused (Cast owns the machine, child still going, no track) */
void ap_tick(ap_probe *a, uint32_t now, const ho_local *l);    /* every loop while ap_active(); l = the Disc right now (NULL = unknown) */
int  ap_pin(ap_probe *a, const char *digits);                  /* 4..8 digits, only after a PIN request */
void ap_cancel(ap_probe *a, uint32_t now);
int  ap_shutdown(ap_probe *a, uint32_t now, uint32_t budget_ms);   /* cancel, then pump until reaped; 1 = nothing left */
int  ap_active(const ap_probe *a);        /* a session or an unreaped child exists: also the power hold */
int  ap_pin_wanted(const ap_probe *a);
#endif
