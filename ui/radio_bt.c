/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "radio_bt.h"
#include "ipc.h"
#include "sqlite3.h"
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>

#ifndef RADIO_CFG_DB_PATH
#define RADIO_CFG_DB_PATH "/usr/data/fiio/db/sysconfig.db"   /* same file radio_cfg.c reads */
#endif

/* Stock frames (static RE, V2.09..V2.57 tables in radio_cfg.c): tag 06c1 = BT on, 06c0 = BT off, value always 0. */
#define BT_FRAME_ON  "06c1000C0000"
#define BT_FRAME_OFF "06c0000C0000"

static pthread_mutex_t g_set_mu = PTHREAD_MUTEX_INITIALIZER;

static radio_bt_result_t db_fail(int rc, radio_bt_result_t other){
    int p = rc & 0xff;
    if(p == SQLITE_BUSY || p == SQLITE_LOCKED) return RADIO_BT_DB_BUSY;
    return other;
}

/* One narrow write in its own short transaction: existing file only (no CREATE), bounded busy wait, exactly one row
 * touched or the whole thing is rolled back. Nothing is held across the IPC send (it already happened). */
static radio_bt_result_t db_write_bt(const char *path, int on){
    radio_bt_result_t res = RADIO_BT_DB_COMMIT;
    sqlite3 *db = NULL; sqlite3_stmt *st = NULL;
    char sql[96]; int in_txn = 0, rc;
    if(!path) return RADIO_BT_DB_SCHEMA;
    rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL);
    if(rc != SQLITE_OK){ res = db_fail(rc, RADIO_BT_DB_SCHEMA); goto done; }  /* missing / unreadable: never created */
    sqlite3_busy_timeout(db, RADIO_CFG_BUSY_MS);
    snprintf(sql, sizeof sql, "UPDATE SYSCONFIG set BT_STATUS = %d where ID = 1;", on ? 1 : 0);
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);                /* no table / column / not a database */
    if(rc != SQLITE_OK){ res = db_fail(rc, RADIO_BT_DB_SCHEMA); goto done; }
    rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    if(rc != SQLITE_OK){ res = db_fail(rc, RADIO_BT_DB_COMMIT); goto done; }
    in_txn = 1;
    rc = sqlite3_step(st);
    if(rc != SQLITE_DONE){ res = db_fail(rc, RADIO_BT_DB_COMMIT); goto done; }
    if(sqlite3_changes(db) != 1){ res = RADIO_BT_DB_SCHEMA; goto done; }   /* no ID=1 row, or more than one */
    rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    if(rc != SQLITE_OK){ res = db_fail(rc, RADIO_BT_DB_COMMIT); goto done; }
    in_txn = 0; res = RADIO_BT_OK;
done:
    if(st) sqlite3_finalize(st);
    if(db && in_txn) sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    if(db) sqlite3_close(db);
    return res;
}

radio_bt_result_t radio_bt_set_with(const radio_bt_ops_t *ops, int on){
    radio_bt_result_t res;
    if(!ops || !ops->send || !ops->caps || !(ops->caps->cmds & RADIO_CAP_CMD_BT)) return RADIO_BT_UNSUPPORTED;
    if(pthread_mutex_trylock(&g_set_mu) != 0) return RADIO_BT_BUSY;
    int sent = ops->send(on ? BT_FRAME_ON : BT_FRAME_OFF, ops->ctx);       /* command first: see the header */
    if(sent == IPC_SEND_DEFERRED) res = RADIO_BT_DEFERRED;
    else if(sent != 0) res = RADIO_BT_SEND_FAILED;
    else {
        res = db_write_bt(ops->db_path, on);
        if(res == RADIO_BT_OK) res = RADIO_BT_PENDING;
    }
    pthread_mutex_unlock(&g_set_mu);
    return res;
}

typedef struct {
    radio_bt_dispatch_fn dispatch;
    void *ctx;
    unsigned generation;
    int automatic;
} bt_dispatch_t;

static int send_on_main(const char *frame, void *ctx){
    bt_dispatch_t *d = ctx;
    if(d->automatic)
        return ipc_send_automatic(frame, d->generation);
    return ipc_send_cmd(frame) == 0 ? 0 : -1;
}

static int send_dispatched(const char *frame, void *ctx){
    bt_dispatch_t *d = ctx;
    return d->dispatch(send_on_main, frame, d, d->ctx);
}

static radio_bt_result_t set_production(
    int on, unsigned generation, int automatic,
    radio_bt_dispatch_fn dispatch, void *ctx){
    if(!dispatch) return RADIO_BT_DEFERRED;
    bt_dispatch_t d = { dispatch, ctx, generation, automatic };
    radio_bt_ops_t o = {
        send_dispatched, &d, RADIO_CFG_DB_PATH,
        radio_fw_caps_running()
    };
    return radio_bt_set_with(&o, on);
}

radio_bt_result_t radio_bt_set_user(
    int on, radio_bt_dispatch_fn dispatch, void *ctx){
    return set_production(on, 0, 0, dispatch, ctx);
}

radio_bt_result_t radio_bt_set_automatic(
    int on, unsigned generation,
    radio_bt_dispatch_fn dispatch, void *ctx){
    return set_production(on, generation, 1, dispatch, ctx);
}
