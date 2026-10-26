/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef RADIO_BT_H
#define RADIO_BT_H
/* Bluetooth on/off as stock does it: the stock command AND a narrow SYSCONFIG.BT_STATUS update.
 *
 * Why both: the stock player keeps BT in RAM (06c0/06c1 do not save) and its shutdown save writes RAM back to
 * BT_STATUS, so a DB-only write is undone by the next graceful shutdown. Owner decisions 67 + 69: stock's saved
 * choice is the one authority.
 *
 * ORDER (decided here): command FIRST, then the DB.
 *   - DB first, command fails: DB = new, RAM = old -> the next graceful shutdown restores the OLD value (the exact
 *     overwrite this module exists to prevent), and nothing tells the caller. So that order is refused.
 *   - send fails: nothing written (RAM old, DB old), reported SEND_FAILED.
 * After queue acceptance, RAM is unknown until execution is confirmed. DB errors require retry; PENDING requires
 * reconciliation in the same player generation. A generation change invalidates execution evidence.
 *
 * Frames: 06c1000C0000 = on, 06c0000C0000 = off (the TAG selects, the value is zero, never ...0001).
 * Never writes WIFI_STATUS, never another column or row, never creates a file, table or row.
 * Evidence level: STATIC reverse engineering (per-firmware addresses in radio_cfg.c). UNVERIFIED on a Disc.
 *
 * Blocking API: call from a worker thread, never the UI thread. One set at a time process-wide: a call made while
 * another is running returns RADIO_BT_BUSY at once. Nothing is held across the IPC send except that one-set lock. */
#include "radio_cfg.h"

typedef enum {
    RADIO_BT_OK = 0,           /* internal DB-write success; public setters return PENDING */
    RADIO_BT_BUSY,             /* another set is running; nothing sent, nothing written */
    RADIO_BT_UNSUPPORTED,      /* firmware unknown or without the BT tags; nothing sent, nothing written */
    RADIO_BT_DEFERRED,         /* automatic send held back by the IPC layer; nothing written, ask again later */
    RADIO_BT_SEND_FAILED,      /* frame not sent; nothing written */
    RADIO_BT_DB_BUSY,          /* frame sent; DB locked past the bounded wait; DB unchanged */
    RADIO_BT_DB_SCHEMA,        /* frame sent; DB missing / not a database / no SYSCONFIG row or column; DB unchanged */
    RADIO_BT_DB_COMMIT,        /* frame sent; the write or commit failed and was rolled back; DB unchanged */
    RADIO_BT_PENDING           /* frame queued and BT_STATUS committed; player execution unconfirmed */
} radio_bt_result_t;

typedef struct {
    int (*send)(const char *frame, void *ctx);   /* 0 = queued, 1 = deferred, anything else = failed */
    void *ctx;
    const char *db_path;
    const radio_fw_caps_t *caps;                 /* NULL = unsupported */
} radio_bt_ops_t;

radio_bt_result_t radio_bt_set_with(const radio_bt_ops_t *ops, int on);   /* injected seams: tests, other policies */

/* Production: the real IPC and stock's sysconfig.db. A user action uses ipc_send_cmd (a failure raises the toast);
 * an automatic one (boot/recovery) passes the player generation it decided for and uses ipc_send_automatic. */
/* Called from a worker. Execute run(frame, run_ctx) on the main
 * thread and return its result. Do not return before run finishes.
 * The main thread must remain free to service this dispatch. */
typedef int (*radio_bt_dispatch_fn)(
    int (*run)(const char *, void *),
    const char *frame, void *run_ctx, void *dispatch_ctx);

radio_bt_result_t radio_bt_set_user(
    int on, radio_bt_dispatch_fn dispatch, void *ctx);
radio_bt_result_t radio_bt_set_automatic(
    int on, unsigned generation,
    radio_bt_dispatch_fn dispatch, void *ctx);

#endif /* RADIO_BT_H */
