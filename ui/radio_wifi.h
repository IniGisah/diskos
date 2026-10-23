/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef RADIO_WIFI_H
#define RADIO_WIFI_H
/* Wi-Fi on/off as stock does it: the stock command, and a narrow SYSCONFIG.WIFI_STATUS update ONLY where the
 * player does not save the value itself. Same structure and result semantics as radio_bt.h (read that first).
 *
 * Frames: 0701000C0000 = on, 0700000C0000 = off (the TAG selects, the value is zero, never ...0001).
 *
 * Per firmware (radio_fw_caps_t.wifi_persists_now, static RE, UNVERIFIED on a Disc):
 *   persists_now = 1 (V2.40 on+off immediately; V2.57 off immediately, on only after its asynchronous bring-up
 *     succeeds): the command is sent and diskOS writes NOTHING. A diskOS write would falsely complete a V2.57
 *     enable that later fails. Result PENDING; completion is observed later from the saved value / player report.
 *   persists_now = 0 (V2.28, V2.09: the handlers change RAM without an immediate save): command FIRST, then the
 *     narrow UPDATE, the radio_bt order, for the same reason (a DB-only write is undone by a shutdown save of RAM;
 *     a DB write before a failed send leaves DB new / RAM old). Their shutdown bulk saver writes WIFI_STATUS from the
 *     same RAM byte the handlers set (V2.28: byte 0x81b5e2, stores 0x4da048/0x4da078, saver reads base 0x81b5a0+0x42
 *     at 0x4d5ec0, key 40 at 0x4d5ec4, called from shutdown 0x4d3284; V2.09: byte 0x822db6, stores 0x4973dc/
 *     0x49740c, saver reads 0x822d74+0x42 at 0x493880, key 40 at 0x493884, called from 0x490ca4), so once the
 *     command executes RAM and DB agree; until then the result is PENDING and the caller reconciles, as for BT.
 * Never writes BT_STATUS, another column or row, a file, table or row. No full-row rewrite. Unsupported firmware
 * (no Wi-Fi tags, unknown version) sends and writes nothing.
 *
 * Blocking API: call from a worker thread, never the UI thread. One set at a time process-wide (BUSY otherwise). */
#include "radio_cfg.h"

typedef enum {
    RADIO_WIFI_OK = 0,         /* internal DB-write success; public setters return PENDING */
    RADIO_WIFI_BUSY,           /* another set is running; nothing sent, nothing written */
    RADIO_WIFI_UNSUPPORTED,    /* firmware unknown or without the Wi-Fi tags; nothing sent, nothing written */
    RADIO_WIFI_DEFERRED,       /* automatic send held back by the IPC layer; nothing written, ask again later */
    RADIO_WIFI_SEND_FAILED,    /* frame not sent; nothing written */
    RADIO_WIFI_DB_BUSY,        /* frame sent; DB locked past the bounded wait; DB unchanged (fallback firmware only) */
    RADIO_WIFI_DB_SCHEMA,      /* frame sent; DB missing / not a database / no SYSCONFIG row or column; DB unchanged */
    RADIO_WIFI_DB_COMMIT,      /* frame sent; the write or commit failed and was rolled back; DB unchanged */
    RADIO_WIFI_PENDING         /* frame queued (and, on fallback firmware, WIFI_STATUS committed); unconfirmed */
} radio_wifi_result_t;

typedef struct {
    int (*send)(const char *frame, void *ctx);   /* 0 = queued, 1 = deferred, anything else = failed */
    void *ctx;
    const char *db_path;
    const radio_fw_caps_t *caps;                 /* NULL = unsupported */
} radio_wifi_ops_t;

radio_wifi_result_t radio_wifi_set_with(const radio_wifi_ops_t *ops, int on);   /* injected seams */

/* Production wrappers: same dispatcher contract as radio_bt_dispatch_fn (run(frame, run_ctx) executes on the main
 * thread, the worker waits). A user action uses ipc_send_cmd; an automatic one passes the player generation. */
typedef int (*radio_wifi_dispatch_fn)(
    int (*run)(const char *, void *),
    const char *frame, void *run_ctx, void *dispatch_ctx);

radio_wifi_result_t radio_wifi_set_user(int on, radio_wifi_dispatch_fn dispatch, void *ctx);
radio_wifi_result_t radio_wifi_set_automatic(int on, unsigned generation,
                                             radio_wifi_dispatch_fn dispatch, void *ctx);

#endif /* RADIO_WIFI_H */
