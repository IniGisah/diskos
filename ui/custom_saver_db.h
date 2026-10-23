/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CUSTOM_SAVER_DB_H
#define CUSTOM_SAVER_DB_H
#define CUSTOM_SAVER_DB_PATH "/usr/data/fiio/db/theme.db"
#define CUSTOM_SAVER_BUSY_MS 3000 /* mq_player@0x446114: li a1,3000 before sqlite3_busy_timeout */

typedef enum {
    CUSTOM_SAVER_OK, CUSTOM_SAVER_MISSING_DB, CUSTOM_SAVER_SCHEMA,
    CUSTOM_SAVER_NO_ACTIVE, CUSTOM_SAVER_UNKNOWN, CUSTOM_SAVER_NULL,
    CUSTOM_SAVER_PATH_LONG, CUSTOM_SAVER_PATH, CUSTOM_SAVER_BUSY,
    CUSTOM_SAVER_DB_ERROR, CUSTOM_SAVER_ARGUMENT, CUSTOM_SAVER_WAL,
    CUSTOM_SAVER_SYSTEM, CUSTOM_SAVER_RECOVERY, CUSTOM_SAVER_NOTIFY
} custom_saver_status;

typedef struct {
    char path[1024];
    int pos_id, type, alpha, lock_time, lock_date, lock_battery;
    int lock_id3, lock_mem_type, front_color;
} custom_saver_asset;

/* NULL db_path uses the stock path. Output is zero on every failure.
 * USE is authoritative, matching stock's SELECT; never reconcile LOCK_THEME.
 * Accept stock's two saved filenames and the HTTP lock_screen subtree only.
 * Paths are lexical storage references, not opened/decoded here. Consumers must
 * validate the opened asset before use (replacement races). Existing path
 * components that are symlinks are rejected; a missing asset file is allowed.
 * WAL is refused: reading it could create shared memory, and persistent WAL
 * mode is not cleared by closing a writer. A crashed rollback-journal writer
 * leaves a hot journal that only a writer can recover; this read-only reader
 * reports CUSTOM_SAVER_RECOVERY and leaves it untouched. Retrying the read
 * cannot fix that; the stock writer must open the database and roll it back.
 * No writes, schema creation, busy wait, or asset existence check. */
custom_saver_status custom_saver_db_read(const char *db_path, custom_saver_asset *out);

/* The saved custom image: the one custom row (POS_ID 0, IS_SYSTEM 0) whatever USE says, with the reader's path, type and symlink
 * rules. A clock or other system row may be active; the image stays installed in the custom row. OK or any non-OK status
 * (no row, NULL or unusable path, ambiguous rows); output zero on failure. Same read-only, no-wait rules as custom_saver_db_read. */
custom_saver_status custom_saver_db_saved(const char *db_path, custom_saver_asset *out);

/* Select one CUSTOM_THEME row the way stock does (mq_ui@0x421fe0: USE=1 on the
 * POS_ID/IS_SYSTEM row, then USE=0 on every other row), but in one BEGIN
 * IMMEDIATE transaction; any failure rolls back and the old selection stays.
 * Only USE changes: no overlay/presentation column, other row or schema is
 * touched (stock owns them). The custom row is (0, 0) with the validated asset
 * path the caller installed; a system theme is (0..4, 1) with asset NULL. The
 * row must exist once and still hold exactly asset as PATH (readers' path and
 * symlink rules, checked before the DB is opened); otherwise PATH/UNKNOWN.
 * Refusals, all without writes: WAL (header check + VFS) -> WAL, hot rollback
 * journal -> RECOVERY (only a stock writer may replay it), lock held longer
 * than busy_ms -> BUSY, read-only file -> DB_ERROR, missing columns -> SCHEMA.
 * It blocks up to busy_ms (stock mq_player uses 3000; mq_ui has no busy
 * handler): call it from a worker thread, never the UI thread.
 * The database path itself must contain no symlink component (CUSTOM_SAVER_PATH).
 *
 * Precondition: the caller must exclude stock mq_ui's theme-selection
 * writer for the operation, including any selection already in progress.
 * SQLite locking cannot serialize this transaction with stock's pair of
 * autocommit UPDATEs. A stock pair can straddle our transaction and leave
 * no active row after our successful post-check. Enforce this exclusion
 * through UI process ownership before integrating this API. */
custom_saver_status custom_saver_db_select(const char *db_path, int pos_id, int is_system,
                                           const char *asset);
custom_saver_status custom_saver_db_select_ex(const char *db_path, int pos_id, int is_system,
                                              const char *asset, int busy_ms);

/* Install an asset on the custom row and select it, as stock does when a GIF is picked (mq_ui@0x422124 sets
 * NAME/PATH/TYPE on (0, 0), then 0x421fe0 flips USE), in ONE transaction. The row must exist exactly once;
 * type is 200 or 204. Same refusals, busy rules and caller exclusion as select. notify runs after every check
 * and before COMMIT, while the database lock is held (keep it short): nonzero rolls everything back and
 * returns CUSTOM_SAVER_NOTIFY. */
typedef int (*custom_saver_db_notify)(void *arg);
custom_saver_status custom_saver_db_install(const char *db_path, const char *name, const char *asset,
                                            int type, int busy_ms, custom_saver_db_notify notify,
                                            void *arg);
/* Previous pathless 0810 value (system 0..4 or Custom 5) for in-process compensation.
 * Caller excludes other theme writers until apply completes. No writes or recovery. */
custom_saver_status custom_saver_db_previous(const char *path, unsigned *style);
#endif
