/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "custom_saver_db.h"
#include "path_nosymlink.h"
#include "sqlite3.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(CUSTOM_SAVER_PATH_NOSYMLINK_MAX == sizeof(((custom_saver_asset *)0)->path),
               "path_nosymlink.h length limit must match custom_saver_asset.path");

/* Preserve the unix VFS lock implementation, but forbid writes and shm mapping.
 * Appended method storage keeps the underlying sqlite3_file layout unchanged. */
static sqlite3_vfs reader_vfs, writer_vfs, *base_vfs;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int vfs_status = SQLITE_ERROR, methods_offset;
static int no_shm(sqlite3_file *f, int page, int size, int extend, void volatile **p)
{
    (void)f; (void)page; (void)size; (void)extend;
    *p = NULL;
    return SQLITE_READONLY;
}
static int no_delete(sqlite3_vfs *v, const char *p, int sync)
{
    (void)v; (void)p; (void)sync;
    return SQLITE_READONLY;
}
static int reader_open(sqlite3_vfs *v, const char *p, sqlite3_file *f, int flags, int *got)
{
    (void)v;
    if(!(flags & SQLITE_OPEN_READONLY) || (flags & SQLITE_OPEN_CREATE))
        return SQLITE_READONLY;
    int rc = base_vfs->xOpen(base_vfs, p, f, flags, got);
    if(rc == SQLITE_OK) {
        sqlite3_io_methods *m = (void *)((char *)f + methods_offset);
        *m = *f->pMethods;
        m->xShmMap = no_shm;
        f->pMethods = m;
    }
    return rc;
}
/* Writer VFS: same unix locking, but WAL files/shm and hot-journal playback
 * (the only open of an existing journal read/write without CREATE, sqlite3.c
 * sqlite3PagerSharedLock) are refused before anything is written. */
static int no_shm_w(sqlite3_file *f, int page, int size, int extend, void volatile **p)
{
    (void)f; (void)page; (void)size; (void)extend;
    *p = NULL;
    return SQLITE_READONLY_CANTINIT;
}
static int writer_open(sqlite3_vfs *v, const char *p, sqlite3_file *f, int flags, int *got)
{
    (void)v;
    if(flags & SQLITE_OPEN_WAL) return SQLITE_READONLY_CANTINIT;
    if((flags & SQLITE_OPEN_MAIN_JOURNAL) && (flags & SQLITE_OPEN_READWRITE) &&
       !(flags & SQLITE_OPEN_CREATE)) return SQLITE_READONLY_ROLLBACK;
    int rc = base_vfs->xOpen(base_vfs, p, f, flags, got);
    if(rc == SQLITE_OK) {
        sqlite3_io_methods *m = (void *)((char *)f + methods_offset);
        *m = *f->pMethods;
        m->xShmMap = no_shm_w;
        f->pMethods = m;
    }
    return rc;
}
static void init_vfs(void)
{
    base_vfs = sqlite3_vfs_find("unix");
    if(!base_vfs) return;
    reader_vfs = *base_vfs;
    methods_offset = (base_vfs->szOsFile + 7) & ~7;
    reader_vfs.szOsFile = methods_offset + sizeof(sqlite3_io_methods);
    reader_vfs.zName = "custom-saver-readonly";
    reader_vfs.pNext = NULL;
    reader_vfs.xOpen = reader_open;
    reader_vfs.xDelete = no_delete;
    vfs_status = sqlite3_vfs_register(&reader_vfs, 0);
    if(vfs_status != SQLITE_OK) return;
    writer_vfs = reader_vfs;
    writer_vfs.zName = "custom-saver-writer";
    writer_vfs.xOpen = writer_open;
    writer_vfs.xDelete = base_vfs->xDelete;
    vfs_status = sqlite3_vfs_register(&writer_vfs, 0);
}
static custom_saver_status db_error(int rc)
{
    if(rc == SQLITE_READONLY_ROLLBACK) return CUSTOM_SAVER_RECOVERY;
    rc &= 255;
    if(rc == SQLITE_BUSY || rc == SQLITE_LOCKED) return CUSTOM_SAVER_BUSY;
    if(rc == SQLITE_READONLY) return CUSTOM_SAVER_WAL;
    return CUSTOM_SAVER_DB_ERROR;
}
static int allowed_path(const char *p)
{
    if(!strcmp(p, "/usr/data/fiio/custom_gif.gif") ||
       !strcmp(p, "/usr/data/fiio/custom_cover.png")) return 1;
    const char *root = "/usr/data/fiio/wifi_transfer/pic/lock_screen/";
    size_t n = strlen(root);
    if(strncmp(p, root, n) || !p[n]) return 0;
    for(p += n; *p;) {
        while(*p == '/') p++;
        if(!*p) return 0;
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if(!len || (len == 1 && p[0] == '.') ||
           (len == 2 && p[0] == '.' && p[1] == '.')) return 0;
        if(!end) break;
        p = end + 1;
        if(!*p) return 0;
    }
    return 1;
}
static custom_saver_status read_row(const char *path, custom_saver_asset *out, int saved)
{
    if(!out) return CUSTOM_SAVER_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if(!path) path = CUSTOM_SAVER_DB_PATH;
    struct stat st;
    if(stat(path, &st)) return errno == ENOENT ? CUSTOM_SAVER_MISSING_DB : CUSTOM_SAVER_DB_ERROR;
    pthread_once(&once, init_vfs);
    if(vfs_status != SQLITE_OK) return CUSTOM_SAVER_DB_ERROR;
    sqlite3 *db = NULL;
    sqlite3_stmt *q = NULL;
    custom_saver_asset asset = {0};
    custom_saver_status status;
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_PRIVATECACHE,
                             reader_vfs.zName);
    if(rc != SQLITE_OK) { status = db_error(rc); goto done; }
    sqlite3_extended_result_codes(db, 1);
    /* mq_ui@0x69663c selects USE=1. Inspect all rows to reject malformed
     * selection flags and ambiguous active rows instead of choosing arbitrarily.
     * Names verified against mq_player@0x6b1eb8, stride 96 (V2.57). */
    rc = sqlite3_prepare_v2(db, "SELECT PATH, POS_ID, TYPE, ALPHA, LOCK_TIME, LOCK_DATE, "
        "LOCK_BATTERY, LOCK_ID3, LOCK_MEM_TYPE, FRONT_COLOR, IS_SYSTEM, USE "
        "FROM CUSTOM_THEME", -1, &q, NULL);
    if(rc != SQLITE_OK) {
        status = rc == SQLITE_ERROR ? CUSTOM_SAVER_SCHEMA : db_error(rc);
        goto done;
    }
    status = CUSTOM_SAVER_NO_ACTIVE;
    int found = 0;
    while((rc = sqlite3_step(q)) == SQLITE_ROW) {
        int use_type = sqlite3_column_type(q, 11);
        if(use_type == SQLITE_NULL) { status = CUSTOM_SAVER_NULL; goto done; }
        sqlite3_int64 use = sqlite3_column_int64(q, 11);
        if(use_type != SQLITE_INTEGER || (use != 0 && use != 1)) {
            status = CUSTOM_SAVER_UNKNOWN; goto done;
        }
        if(saved ? (sqlite3_column_int64(q, 1) != 0 || sqlite3_column_int64(q, 10) != 0) : !use) continue;   /* saved: the custom row (0, 0) whatever USE says */
        if(found++) { status = CUSTOM_SAVER_UNKNOWN; goto done; }
        int values[10];
        for(int i = 1; i <= 10; i++) {
            int type = sqlite3_column_type(q, i);
            if(type == SQLITE_NULL) { status = CUSTOM_SAVER_NULL; goto done; }
            sqlite3_int64 value = sqlite3_column_int64(q, i);
            if(type != SQLITE_INTEGER || value < 0 || value > 0x7fffffff) {
                status = CUSTOM_SAVER_UNKNOWN; goto done;
            }
            values[i-1] = (int)value;
        }
        if(values[9] == 1 && values[0] <= 4) { status = CUSTOM_SAVER_SYSTEM; continue; }
        if(values[9] != 0 || (values[1] != 200 && values[1] != 201 &&
           values[1] != 202 && values[1] != 204) || values[2] > 100) {
            status = CUSTOM_SAVER_UNKNOWN; goto done;
        }
        if(sqlite3_column_type(q, 0) == SQLITE_NULL) { status = CUSTOM_SAVER_NULL; goto done; }
        int len = sqlite3_column_bytes(q, 0);
        if(len >= (int)sizeof(asset.path)) { status = CUSTOM_SAVER_PATH_LONG; goto done; }
        const char *p = (const char *)sqlite3_column_text(q, 0);
        if(!p) { status = CUSTOM_SAVER_DB_ERROR; goto done; }
        if(sqlite3_column_type(q, 0) != SQLITE_TEXT || (int)strlen(p) != len || !allowed_path(p) || !custom_saver_path_no_symlinks(p)) {
            status = CUSTOM_SAVER_PATH; goto done;
        }
        memcpy(asset.path, p, len + 1);
        asset.pos_id = values[0]; asset.type = values[1]; asset.alpha = values[2];
        asset.lock_time = values[3]; asset.lock_date = values[4];
        asset.lock_battery = values[5]; asset.lock_id3 = values[6];
        asset.lock_mem_type = values[7]; asset.front_color = values[8];
        status = CUSTOM_SAVER_OK;
    }
    if(rc != SQLITE_DONE) status = db_error(rc);
done:
    sqlite3_finalize(q);
    sqlite3_close(db);
    if(status == CUSTOM_SAVER_OK) *out = asset;
    return status;
}
custom_saver_status custom_saver_db_read(const char *path, custom_saver_asset *out) { return read_row(path, out, 0); }
custom_saver_status custom_saver_db_saved(const char *path, custom_saver_asset *out) { return read_row(path, out, 1); }

/* Stock mq_ui@0x421fe0 runs two autocommit UPDATEs (no BEGIN, no busy handler,
 * results ignored). This runs the same two as one transaction. */
static int run_update(sqlite3 *db, const char *sql, int pos_id, int is_system)
{
    sqlite3_stmt *q;
    int rc = sqlite3_prepare_v2(db, sql, -1, &q, NULL);
    if(rc != SQLITE_OK) return rc;
    sqlite3_bind_int(q, 1, pos_id);
    sqlite3_bind_int(q, 2, is_system);
    rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}
static custom_saver_status select_error(int rc)
{
    if(rc == SQLITE_READONLY_ROLLBACK) return CUSTOM_SAVER_RECOVERY;
    if(rc == SQLITE_READONLY_CANTINIT) return CUSTOM_SAVER_WAL;
    if(rc == SQLITE_ERROR) return CUSTOM_SAVER_SCHEMA;
    rc &= 255;
    return rc == SQLITE_BUSY || rc == SQLITE_LOCKED ? CUSTOM_SAVER_BUSY : CUSTOM_SAVER_DB_ERROR;
}
/* Persistent WAL mode is recorded in the header (bytes 18/19 == 2). */
static custom_saver_status header_check(const char *path)
{
    unsigned char h[100];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if(fd < 0) return errno == ENOENT ? CUSTOM_SAVER_MISSING_DB : CUSTOM_SAVER_DB_ERROR;
    ssize_t n = read(fd, h, sizeof(h));
    close(fd);
    if(n != (ssize_t)sizeof(h) || memcmp(h, "SQLite format 3", 16)) return CUSTOM_SAVER_DB_ERROR;
    return h[18] == 2 || h[19] == 2 ? CUSTOM_SAVER_WAL : CUSTOM_SAVER_OK;
}
/* The target row must be unique and, for a custom row, still reference exactly
 * the validated asset; a system row (pos 0..4) carries no asset. */
static custom_saver_status check_target(sqlite3 *db, int pos_id, int is_system, const char *asset)
{
    sqlite3_stmt *q;
    int rc = sqlite3_prepare_v2(db, "SELECT PATH, TYPE FROM CUSTOM_THEME "
                                "WHERE POS_ID = ?1 AND IS_SYSTEM = ?2", -1, &q, NULL);
    if(rc != SQLITE_OK) return select_error(rc);
    sqlite3_bind_int(q, 1, pos_id);
    sqlite3_bind_int(q, 2, is_system);
    custom_saver_status st = CUSTOM_SAVER_UNKNOWN;
    rc = sqlite3_step(q);
    if(rc == SQLITE_ROW) {
        int ty = sqlite3_column_type(q, 1);
        sqlite3_int64 type = sqlite3_column_int64(q, 1);
        if(is_system) st = CUSTOM_SAVER_OK;
        else if(sqlite3_column_type(q, 0) == SQLITE_NULL || ty == SQLITE_NULL) st = CUSTOM_SAVER_NULL;
        else if(sqlite3_column_type(q, 0) == SQLITE_TEXT && ty == SQLITE_INTEGER &&
                (type == 200 || type == 201 || type == 202 || type == 204) &&
                sqlite3_column_bytes(q, 0) == (int)strlen(asset) &&
                !strcmp((const char *)sqlite3_column_text(q, 0), asset)) st = CUSTOM_SAVER_OK;
        else st = CUSTOM_SAVER_PATH;
        if(st == CUSTOM_SAVER_OK && sqlite3_step(q) != SQLITE_DONE) st = CUSTOM_SAVER_UNKNOWN;
    } else if(rc != SQLITE_DONE) st = select_error(rc);
    sqlite3_finalize(q);
    return st;
}
/* Exactly the target holds USE=1 and no USE value is outside 0/1 (the reader's rules). */
static custom_saver_status check_result(sqlite3 *db, int pos_id, int is_system)
{
    sqlite3_stmt *q;
    int rc = sqlite3_prepare_v2(db, "SELECT COALESCE(SUM(USE IS 1), 0), "
        "COALESCE(SUM(USE IS 1 AND POS_ID IS ?1 AND IS_SYSTEM IS ?2), 0), "
        "COALESCE(SUM(typeof(USE) <> 'integer' OR USE NOT IN (0, 1)), 0) "
        "FROM CUSTOM_THEME", -1, &q, NULL);
    if(rc != SQLITE_OK) return select_error(rc);
    sqlite3_bind_int(q, 1, pos_id);
    sqlite3_bind_int(q, 2, is_system);
    custom_saver_status st = CUSTOM_SAVER_UNKNOWN;
    rc = sqlite3_step(q);
    if(rc == SQLITE_ROW) {
        if(sqlite3_column_int(q, 0) == 1 && sqlite3_column_int(q, 1) == 1 &&
           sqlite3_column_int(q, 2) == 0) st = CUSTOM_SAVER_OK;
    } else st = select_error(rc);
    sqlite3_finalize(q);
    return st;
}
/* mq_ui@0x69678c (written by 0x422124): NAME, PATH and TYPE of the one custom row. */
static custom_saver_status install_row(sqlite3 *db, const char *name, const char *asset, int type)
{
    sqlite3_stmt *q;
    int rc = sqlite3_prepare_v2(db, "UPDATE CUSTOM_THEME SET NAME = ?1, PATH = ?2, TYPE = ?3 "
                                "WHERE POS_ID = 0 AND IS_SYSTEM = 0", -1, &q, NULL);
    if(rc != SQLITE_OK) return select_error(rc);
    sqlite3_bind_text(q, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(q, 2, asset, -1, SQLITE_STATIC);
    sqlite3_bind_int(q, 3, type);
    rc = sqlite3_step(q);
    sqlite3_finalize(q);
    if(rc != SQLITE_DONE) return select_error(rc);
    return sqlite3_changes(db) == 1 ? CUSTOM_SAVER_OK : CUSTOM_SAVER_UNKNOWN;
}
static custom_saver_status select_impl(const char *path, int pos_id, int is_system,
                                       const char *asset, int busy_ms, const char *name, int type,
                                       custom_saver_db_notify notify, void *arg)
{
    if(!path) path = CUSTOM_SAVER_DB_PATH;
    if(pos_id < 0 || (is_system != 0 && is_system != 1) || busy_ms < 0 ||
       busy_ms > CUSTOM_SAVER_BUSY_MS || (is_system ? (asset || pos_id > 4) : !asset))
        return CUSTOM_SAVER_ARGUMENT;
    if(name && (is_system || pos_id || !*name || strlen(name) > 127 || !(type == 200 || type == 204)))
        return CUSTOM_SAVER_ARGUMENT;
    if(asset) {
        if(strlen(asset) >= sizeof(((custom_saver_asset *)0)->path)) return CUSTOM_SAVER_PATH_LONG;
        if(!allowed_path(asset) || !custom_saver_path_no_symlinks(asset)) return CUSTOM_SAVER_PATH;
    }
    if(strlen(path) >= sizeof(((custom_saver_asset *)0)->path))
        return CUSTOM_SAVER_PATH_LONG;
    if(!custom_saver_path_no_symlinks(path)) return CUSTOM_SAVER_PATH;
    struct stat st;
    if(stat(path, &st))
        return errno == ENOENT ? CUSTOM_SAVER_MISSING_DB : CUSTOM_SAVER_DB_ERROR;
    custom_saver_status status = header_check(path);
    if(status != CUSTOM_SAVER_OK) return status;
    pthread_once(&once, init_vfs);
    if(vfs_status != SQLITE_OK) return CUSTOM_SAVER_DB_ERROR;
    sqlite3 *db = NULL;
    int in_tx = 0;
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_PRIVATECACHE |
                             SQLITE_OPEN_NOFOLLOW,
                             writer_vfs.zName);
    if(rc != SQLITE_OK) { status = select_error(rc); goto done; }
    sqlite3_extended_result_codes(db, 1);
    if(sqlite3_db_readonly(db, "main") != 0) { status = CUSTOM_SAVER_DB_ERROR; goto done; }
    sqlite3_busy_timeout(db, busy_ms);
    if((rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL)) != SQLITE_OK) {
        status = select_error(rc); goto done;
    }
    in_tx = 1;
    if(name && (status = install_row(db, name, asset, type)) != CUSTOM_SAVER_OK) goto done;
    if((status = check_target(db, pos_id, is_system, asset)) != CUSTOM_SAVER_OK) goto done;
    if((rc = run_update(db, "UPDATE CUSTOM_THEME SET USE = 1 WHERE POS_ID = ?1 AND "
                        "IS_SYSTEM = ?2 AND USE IS NOT 1", pos_id, is_system)) != SQLITE_OK ||
       (rc = run_update(db, "UPDATE CUSTOM_THEME SET USE = 0 WHERE (POS_ID != ?1 OR "
                        "IS_SYSTEM != ?2) AND USE IS NOT 0", pos_id, is_system)) != SQLITE_OK) {
        status = select_error(rc); goto done;
    }
    if((status = check_result(db, pos_id, is_system)) != CUSTOM_SAVER_OK) goto done;
    if(notify && notify(arg)) { status = CUSTOM_SAVER_NOTIFY; goto done; }
    if((rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL)) != SQLITE_OK) {
        status = select_error(rc); goto done;
    }
    in_tx = 0;
done:
    if(in_tx) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close(db);
    return status;
}
custom_saver_status custom_saver_db_select_ex(const char *path, int pos_id, int is_system,
                                              const char *asset, int busy_ms)
{
    return select_impl(path, pos_id, is_system, asset, busy_ms, NULL, 0, NULL, NULL);
}
custom_saver_status custom_saver_db_install(const char *path, const char *name, const char *asset,
                                            int type, int busy_ms, custom_saver_db_notify notify,
                                            void *arg)
{
    return select_impl(path, 0, 0, asset, busy_ms, name, type, notify, arg);
}
custom_saver_status custom_saver_db_select(const char *path, int pos_id, int is_system,
                                           const char *asset)
{
    return custom_saver_db_select_ex(path, pos_id, is_system, asset, CUSTOM_SAVER_BUSY_MS);
}

/* Snapshot only the player selection for in-process compensation. Stock writers are excluded. */
custom_saver_status custom_saver_db_previous(const char *path, unsigned *style)
{
    custom_saver_status st = header_check(path);
    if(st) return st;
    pthread_once(&once, init_vfs);
    if(vfs_status != SQLITE_OK) return CUSTOM_SAVER_DB_ERROR;
    sqlite3 *db = NULL;
    sqlite3_stmt *q = NULL;
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_PRIVATECACHE, reader_vfs.zName);
    if(rc != SQLITE_OK) { st = db_error(rc); goto done; }
    rc = sqlite3_prepare_v2(db, "SELECT POS_ID,IS_SYSTEM FROM CUSTOM_THEME WHERE USE=1 "
        "AND NOT EXISTS(SELECT 1 FROM CUSTOM_THEME WHERE typeof(USE)<>'integer' OR USE NOT IN(0,1))", -1, &q, NULL);
    if(rc != SQLITE_OK) { st = select_error(rc); goto done; }
    rc = sqlite3_step(q);
    st = CUSTOM_SAVER_UNKNOWN;
    if(rc != SQLITE_ROW) { if(rc != SQLITE_DONE) st = select_error(rc); goto done; }
    if(sqlite3_column_type(q,0)!=SQLITE_INTEGER || sqlite3_column_type(q,1)!=SQLITE_INTEGER) goto done;
    int pos = sqlite3_column_int(q,0), system = sqlite3_column_int(q,1);
    if(system ? (system!=1 || pos<0 || pos>4) : pos<0) goto done;
    *style = system ? (unsigned)pos : 5;
    rc = sqlite3_step(q);
    if(rc == SQLITE_DONE) st = CUSTOM_SAVER_OK;
    else if(rc != SQLITE_ROW) st = select_error(rc);
done:
    sqlite3_finalize(q); sqlite3_close(db); return st;
}
