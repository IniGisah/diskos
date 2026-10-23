/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_CAST_BRIDGE_H
#define DISKOS_CAST_BRIDGE_H
#include <stddef.h>
#include <stdint.h>
#include "cast_handoff.h"
#include "cast_http.h"
#include "cast_session.h"
/* Cast bridge (slice 13b): the glue between the handoff machine (cast_handoff.c), the media server (cast_http.c) and the receiver
 * session (cast_session.c). It turns a handoff serve_load into "approve the file, build the capability URL, send LOAD", a remote
 * stop into a media STOP, a revoke into a new server token, and the session snapshot into the machine's LOAD / STOP events.
 * LVGL thread only (the ho_sink callbacks run there); no threads of its own. cast_http_stop() is externally exclusive, so the server is
 * created lazily by the first serve and stopped only by cb_release(), on this one thread, after revoke. Everything the Disc
 * touches is behind the cb_ops table so the host test can inject the Wi-Fi address and a fake session; production uses CB_REAL. */
#define CB_URL_MAX 160
#define CB_EV_MAX 4
typedef struct {
    int      (*wlan_ip)(char *out, size_t n);                                    /* dotted IPv4 of wlan0; 0 ok */
    cast_http *(*http_start)(const char *ipv4, const char *sd_root);
    void     (*http_stop)(cast_http *);
    int      (*sess_load)(cast_session *, const cs_load *);
    int      (*sess_stop)(cast_session *);
    uint64_t (*sess_snap)(cast_session *, cs_snapshot *);
} cb_ops;
extern const cb_ops CB_REAL;
typedef struct {
    const cb_ops *ops; const char *root;      /* root: the SD mount (no trailing slash) every served path must lie beneath */
    cast_session *sess;                        /* not owned; set per receiver by cb_attach() */
    cast_http *http; char ip[16];              /* lazily created, stopped by cb_release(); the wlan0 address it listens on */
    int load_armed, stop_armed;                /* an edge is waiting for its job to finish */
    uint32_t load_attempt, stop_attempt; unsigned load_seq, stop_seq;   /* the attempt and the session's *_seq this edge belongs to */
} cb;
/* Pure helpers. Path: root + "/" + a library-relative path with no empty, "." or ".." component and no control byte; anything else
 * (outside the root, the root itself, relative, trailing slash) is refused with -1. rel gets the part after the root. */
int cb_rel_path(const char *root, const char *path, char *rel, size_t n);
/* http://<ipv4>:<port>/<token>/<id>; -1 when it does not fit. */
int cb_url(char *out, size_t n, const char *ipv4, unsigned port, const char *token, const char *id);
void cb_init(cb *b, const cb_ops *ops, const char *sd_root);
void cb_attach(cb *b, cast_session *s);    /* a new receiver session: forgets pending edges */
/* The ho_sink pieces. serve_load: -1 on any failure (nothing is faked, the machine rolls back). stop: -1 when the STOP request is not
 * accepted. revoke: new token, the old URL is dead. release: forgets pending edges and stops the server (revoke first). */
int  cb_serve_load(cb *b, uint32_t attempt, const char *path, long position_ms, const char *title, const char *artist, const char *album);
/* serve_load_next (slice 14d, the controller's per-track LOAD): cb_serve_load without the handoff edge. It never sets or clears load_armed /
 * stop_armed / load_attempt, so cb_events stays silent for it and the controller reads load_seq / load_state from the snapshot itself.
 * Returns 0 and the load_seq it started, or -1 (nothing armed, *load_seq untouched). */
int  cb_serve_load_next(cb *b, const char *path, long position_ms, const char *title, const char *artist, const char *album, unsigned *load_seq);
int  cb_stop(cb *b, uint32_t attempt);
void cb_revoke(cb *b);
void cb_release(cb *b);
/* Snapshot -> machine events, in the order they must be delivered (lifecycle first, then LOAD result, owned media, STOP result).
 * LOAD_OK / LOAD_FAILED fire once, only for the load_seq this attempt started; REMOTE_STOPPED (or REMOTE_LOST when the receiver
 * refused or timed out the STOP: the machine's STOPPING handles it exactly like its own timeout, only sooner) once for the stop_seq it
 * started. An older or newer seq, or an attempt other than the one that armed the edge, never fires. Returns the count (<= CB_EV_MAX). */
int  cb_events(cb *b, const cs_snapshot *s, uint32_t attempt, ho_evtype out[CB_EV_MAX]);
#endif
