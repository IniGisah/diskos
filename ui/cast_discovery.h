/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_CAST_DISCOVERY_H
#define DISKOS_CAST_DISCOVERY_H
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#define CD_MAX 16     /* results kept; later receivers are dropped */
#define CD_LINE 1536  /* longer avahi lines are rejected whole */
typedef struct { char id[64], name[64], model[48], addr[16]; uint16_t port; } cast_receiver;
/* Parser state (also the result list). Pure: no I/O. */
typedef struct {
    cast_receiver r[CD_MAX]; size_t n;
    char line[CD_LINE]; size_t used; int bad;
} cast_disc;
void cast_disc_init(cast_disc *);
/* Feed arbitrary chunks (lines may be split anywhere). Only resolved ("=")
 * wlan0 IPv4 _googlecast._tcp lines count: valid unicast address, advertised
 * port 1..65535, TXT fn (else service name) / md / id. Duplicates by TXT id,
 * else service name, keep the first position and latest resolved details.
 * Malformed or oversized lines are dropped whole. cast_disc_end flushes a
 * final line that lacks a newline. */
void cast_disc_feed(cast_disc *, const char *, size_t);
void cast_disc_end(cast_disc *);
enum { CD_FOUND, CD_NONE, CD_MISSING, CD_EXEC, CD_DAEMON, CD_TIMEOUT, CD_CANCEL, CD_ERROR };
/* One bounded job; zero-initialise. Any thread may call cast_disc_cancel. */
typedef struct { _Atomic int cancel; _Atomic pid_t pid; } cast_disc_job;
void cast_disc_cancel(cast_disc_job *);
/* Blocking; call from a worker, never the LVGL thread. Runs
 * /usr/bin/avahi-browse -rpt _googlecast._tcp (fixed argv, no shell) and kills
 * and reaps exactly that pid at the deadline (1..30000 ms) or on cancel.
 * SIGCHLD must be at its default (not ignored, no SA_NOCLDWAIT) and no other
 * thread may reap this child, else nothing is started and CD_ERROR returns.
 * CD_FOUND results (also a deadline with partial results); CD_NONE clean exit,
 * nothing usable; CD_MISSING no such program; CD_EXEC it could not start;
 * CD_DAEMON it ran and failed (daemon unavailable); CD_TIMEOUT deadline with
 * nothing; CD_CANCEL; CD_ERROR local failure. The list is empty unless FOUND. */
int cast_disc_run(cast_disc_job *, cast_disc *, unsigned deadline_ms);
/* Shared supervised byte-stream runner. Fixed service types only. */
int cast_disc_run_lines(cast_disc_job *, const char *type, unsigned ms,
                        void (*feed)(void *, const char *, size_t), void *ctx);
#endif
