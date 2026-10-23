/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef RADIO_CFG_H
#define RADIO_CFG_H
/* Stock's saved radio choice, read-only, plus a per-firmware radio capability table.
 *
 * Owner decisions 67 + 69: diskOS adopts the stock player's saved settings (SYSCONFIG.WIFI_STATUS = key 40,
 * SYSCONFIG.BT_STATUS = key 41) as the one authority for "should this radio be on after a reboot".
 * This module only READS them. It never writes, never creates a database or a table and never substitutes a
 * default: anything it cannot positively read is RADIO_STATE_UNKNOWN, which is not "off".
 *
 * Evidence level: STATIC reverse engineering of the stock mq_player binaries. The per-firmware addresses
 * are in radio_cfg.c; the four tag->handler pairs were re-resolved with tools/fw_tag_handler.py.
 * Behaviour on the Disc is UNVERIFIED. */

typedef enum {
    RADIO_STATE_OFF = 0,
    RADIO_STATE_ON = 1,
    RADIO_STATE_UNKNOWN = 2       /* never read, or read something that is not a clean 0/1 */
} radio_state_t;

typedef enum {
    RADIO_CFG_OK = 0,
    RADIO_CFG_NO_DB,              /* the file could not be opened (missing, unreadable); never created */
    RADIO_CFG_BUSY,               /* locked by a writer for longer than the bounded wait */
    RADIO_CFG_SCHEMA,             /* not a database, or no SYSCONFIG table / WIFI_STATUS / BT_STATUS column */
    RADIO_CFG_NO_ROW,             /* zero rows with ID=1 */
    RADIO_CFG_MULTI_ROW,          /* more than one row with ID=1 */
    RADIO_CFG_BAD_VALUE,          /* NULL, non-integer or not 0/1 in either column */
    RADIO_CFG_IO                  /* any other SQLite error */
} radio_cfg_err_t;

typedef struct {
    radio_state_t wifi;
    radio_state_t bt;
    radio_cfg_err_t err;          /* RADIO_CFG_OK exactly when both states are known */
} radio_saved_t;

/* The two columns are read together, in one statement, so they are one consistent snapshot. When err != OK
 * BOTH states are UNKNOWN (a pair with one bad half is not trusted).
 * Blocking API: call from a worker, never the UI thread. Each call owns its connection.
 * RADIO_CFG_BUSY_MS limits SQLite busy waiting, not total elapsed time or filesystem I/O. */
#define RADIO_CFG_BUSY_MS 250
radio_saved_t radio_cfg_read_path(const char *db_path);   /* tests / alternative locations */
radio_saved_t radio_cfg_read(void);                       /* stock's /usr/data/fiio/db/sysconfig.db */

/* ---- firmware capability table ---- */
/* Which stock command tags exist (tag selects on/off; the value field is sent as zero, see the design). */
#define RADIO_CAP_CMD_BT   0x1u   /* 06c0 = BT off, 06c1 = BT on */
#define RADIO_CAP_CMD_WIFI 0x2u   /* 0700 = Wi-Fi off, 0701 = Wi-Fi on */
/* Facts that are NOT established even by static RE for that firmware; the matching field holds the safe value. */
#define RADIO_UV_WIFI_PERSIST 0x1u   /* whether the Wi-Fi handlers save immediately: treated as NO */
#define RADIO_UV_BOOT_RULE    0x2u   /* the boot-time radio lifecycle was not examined: treated as "no both-on rule" */

typedef struct {
    int ver;                    /* MAIN_OS_VER, as fw_os_ver() reports it */
    unsigned cmds;              /* RADIO_CAP_CMD_* present on this firmware */
    int wifi_persists_now;      /* 1 = the Wi-Fi on/off handlers update SYSCONFIG.WIFI_STATUS on their own path */
    int bt_persists_now;        /* 1 = the BT handlers do (no firmware: 0, BT is saved only by the shutdown saver) */
    int both_on_boot_rule;      /* 1 = boot with both saved on keeps BT, turns Wi-Fi off and saves WIFI_STATUS=0 */
    unsigned unverified;        /* RADIO_UV_* */
} radio_fw_caps_t;

/* NULL for a firmware with no row (unknown / unreadable version): callers must then assume nothing. */
const radio_fw_caps_t *radio_fw_caps(int ver);
const radio_fw_caps_t *radio_fw_caps_running(void);       /* radio_fw_caps(fw_os_ver()) */

#endif /* RADIO_CFG_H */
