/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "radio_cfg.h"
#include "fwcaps.h"
#include "sqlite3.h"
#include <pthread.h>
#include <stddef.h>

/* Runtime database of the stock player. The path is the one musicdb.c (MDB_SYSCONFIG_PATH) and power.c already
 * read SYSCONFIG from, and docs/RE_CATALOGUE.md lists it; the design's "usr/data/fiio/db/sysconfig.db" is the same
 * file. Live content on the Disc is UNVERIFIED for these two columns (the fixture roots hold no runtime DB). */
#ifndef RADIO_CFG_DB_PATH
#define RADIO_CFG_DB_PATH "/usr/data/fiio/db/sysconfig.db"
#endif

static radio_cfg_err_t map_rc(int rc, radio_cfg_err_t other){
    int p = rc & 0xff;                                       /* primary code of an extended one */
    if(p == SQLITE_BUSY || p == SQLITE_LOCKED) return RADIO_CFG_BUSY;
    return other;
}

/* One column: integer 0/1 only. NULL, text, real, blob or any other integer is not a saved choice. */
static int column_state(sqlite3_stmt *st, int col, radio_state_t *out){
    if(sqlite3_column_type(st, col) != SQLITE_INTEGER) return 0;
    sqlite3_int64 v = sqlite3_column_int64(st, col);
    if(v != 0 && v != 1) return 0;
    *out = v ? RADIO_STATE_ON : RADIO_STATE_OFF;
    return 1;
}

static pthread_once_t radio_vfs_once = PTHREAD_ONCE_INIT;
static sqlite3_vfs radio_vfs;
static sqlite3_vfs *radio_base_vfs;
static int radio_vfs_rc = SQLITE_ERROR;

static int radio_ro_open(sqlite3_vfs *vfs, const char *name,
                         sqlite3_file *file, int flags, int *out_flags){
    (void)vfs;
    if(!(flags & SQLITE_OPEN_READONLY) ||
       (flags & (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                 SQLITE_OPEN_DELETEONCLOSE | SQLITE_OPEN_WAL |
                 SQLITE_OPEN_MAIN_JOURNAL))){
        return SQLITE_READONLY;
    }
    return radio_base_vfs->xOpen(radio_base_vfs, name, file,
                                 flags, out_flags);
}

static int radio_ro_delete(sqlite3_vfs *vfs, const char *name, int sync_dir){
    (void)vfs;
    (void)name;
    (void)sync_dir;
    return SQLITE_READONLY;
}

static void radio_ro_vfs_init(void){
    radio_base_vfs = sqlite3_vfs_find("unix");
    if(!radio_base_vfs) return;
    radio_vfs = *radio_base_vfs;
    radio_vfs.zName = "diskos-radio-ro";
    radio_vfs.xOpen = radio_ro_open;
    radio_vfs.xDelete = radio_ro_delete;
    radio_vfs_rc = sqlite3_vfs_register(&radio_vfs, 0);
}

radio_saved_t radio_cfg_read_path(const char *db_path){
    radio_saved_t r = { RADIO_STATE_UNKNOWN, RADIO_STATE_UNKNOWN, RADIO_CFG_IO };
    sqlite3 *db = NULL; sqlite3_stmt *st = NULL;
    if(!db_path) { r.err = RADIO_CFG_NO_DB; return r; }
    /* existing file only (no SQLITE_OPEN_CREATE), read-only: no journal, no schema, no write of any kind */
    if(pthread_once(&radio_vfs_once, radio_ro_vfs_init) != 0)
        goto done;
    int rc = radio_vfs_rc;
    if(rc != SQLITE_OK) goto done;
    rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY,
                         "diskos-radio-ro");
    if(rc != SQLITE_OK){ r.err = map_rc(rc, RADIO_CFG_NO_DB); goto done; }
    sqlite3_busy_timeout(db, RADIO_CFG_BUSY_MS);
    /* both columns in one statement: a missing table or column fails here, at prepare time */
    rc = sqlite3_prepare_v2(db, "SELECT WIFI_STATUS, BT_STATUS FROM SYSCONFIG WHERE ID=1;", -1, &st, NULL);
    if(rc != SQLITE_OK){ r.err = map_rc(rc, RADIO_CFG_SCHEMA); goto done; }
    rc = sqlite3_step(st);
    if(rc == SQLITE_DONE){ r.err = RADIO_CFG_NO_ROW; goto done; }
    if(rc != SQLITE_ROW){ r.err = map_rc(rc, RADIO_CFG_IO); goto done; }
    radio_state_t w, b;
    int ok = column_state(st, 0, &w) && column_state(st, 1, &b);
    rc = sqlite3_step(st);                                   /* exactly one row: a second one makes the pair untrusted */
    if(rc == SQLITE_ROW){ r.err = RADIO_CFG_MULTI_ROW; goto done; }
    if(rc != SQLITE_DONE){ r.err = map_rc(rc, RADIO_CFG_IO); goto done; }
    if(!ok){ r.err = RADIO_CFG_BAD_VALUE; goto done; }
    r.wifi = w; r.bt = b; r.err = RADIO_CFG_OK;
done:
    if(st) sqlite3_finalize(st);
    if(db) sqlite3_close(db);
    return r;
}

radio_saved_t radio_cfg_read(void){ return radio_cfg_read_path(RADIO_CFG_DB_PATH); }

/* Firmware capability table. Handler VAs are mq_player addresses from the design (tag -> handler also re-resolved
 * with tools/fw_tag_handler.py). EVERY row is static RE only: nothing here has been exercised on a Disc.
 * Handlers (BT off/on, Wi-Fi off/on) and persistence evidence per row:
 *   V2.57  06c0 0x4f0dcc  06c1 0x4f0e98  0700 0x4f1a00  0701 0x4f19bc
 *          Wi-Fi: off saves key 40=0 in the handler (0x4f1a00-0x4f1a28); on saves key 40=1 only after a successful
 *          async bring-up (callback 0x4ed050, save at 0x4ed078-0x4ed088) -> persists_now = 1.
 *          BT: RAM byte +0x43 only (0x4f0de8 / 0x4f0eb8), saved by the shutdown saver (0x4e6ff0 -> 0x4ea22c) -> 0.
 *          Boot: both saved on -> WIFI_STATUS rewritten to 0, BT kept (test 0x4ecf94-0x4ecfa4, write 0x4ed024-0x4ed030).
 *   V2.40  06c0 0x4e7ca4  06c1 0x4e7d70  0700 0x4e882c  0701 0x4e87e4
 *          Wi-Fi: on/off write key 40 in the handler (0x4e8804-0x4e8818 / 0x4e8844-0x4e8854) -> 1.
 *          BT: RAM only (0x4e7cc0 / 0x4e7d9c), bulk save 0x4e14b0-0x4e14c4 -> 0.
 *          Boot: loader 0x4e3f30 opens saved Wi-Fi (0x4e3f5c-0x4e4004); NO both-on correction.
 *   V2.28  06c0 0x4d99d8  06c1 0x4d9a50  0700 0x4da058  0701 0x4da01c
 *          the handlers change RAM without an immediate saver call, but the save lifecycle was not RE'd -> Wi-Fi
 *          persistence UNVERIFIED, treated as NO; boot lifecycle not examined (UNVERIFIED).
 *   V2.09  06c0 0x496dd4  06c1 0x496e4c  0700 0x4973ec  0701 0x4973b0
 *          same position as V2.28: UNVERIFIED persistence and boot lifecycle. */
static const radio_fw_caps_t TABLE[] = {
    /* ver  cmds                                  wifi_now bt_now both_on unverified */
    { 209, RADIO_CAP_CMD_BT | RADIO_CAP_CMD_WIFI,  0,       0,     0,      RADIO_UV_WIFI_PERSIST | RADIO_UV_BOOT_RULE },
    { 228, RADIO_CAP_CMD_BT | RADIO_CAP_CMD_WIFI,  0,       0,     0,      RADIO_UV_WIFI_PERSIST | RADIO_UV_BOOT_RULE },
    { 240, RADIO_CAP_CMD_BT | RADIO_CAP_CMD_WIFI,  1,       0,     0,      0 },
    { 257, RADIO_CAP_CMD_BT | RADIO_CAP_CMD_WIFI,  1,       0,     1,      0 },
};

const radio_fw_caps_t *radio_fw_caps(int ver){
    for(size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++)
        if(TABLE[i].ver == ver) return &TABLE[i];
    return NULL;
}

const radio_fw_caps_t *radio_fw_caps_running(void){ return radio_fw_caps(fw_os_ver()); }
