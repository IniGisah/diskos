/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef SCANNER_H
#define SCANNER_H
/* diskOS music scanner - walks the SD, reads ID3 tags, rebuilds song.db's SONG table.
 * All calls are safe from the LVGL/main thread; the scan itself runs on a worker thread. */
enum { OLD_TAGS_AUTO, OLD_TAGS_WESTERN, OLD_TAGS_CYRILLIC };
void scanner_set_old_tags(int mode);          /* main-thread config mirror; next scan snapshots it */
void scanner_abort(void);                     /* stop a running walk (SD access revoked) */
int  scanner_cancel(void);                    /* user cancel: 1 = accepted (stops within ~1 s, DB rolled back), 0 = refused (not scanning / already committing) */
enum { SCAN_OUT_NONE, SCAN_OUT_COMMITTED, SCAN_OUT_CANCELLED, SCAN_OUT_FAILED, SCAN_OUT_NO_SD };
int  scanner_outcome(void);                   /* the finished scan's terminal outcome (SCAN_OUT_*) */
int  scanner_start(void);                     /* begin a rescan; 0 = started, -1 = busy/failed */
int  scanner_active(void);                    /* 1 while a scan is running */
void scanner_progress(int *done, int *total); /* files done so far; total is 0 until finished */
int  scanner_take_finished(void);             /* returns 1 exactly once after a scan finishes */
int  scanner_take_committed(void);            /* 1 once if any scan committed since the last call (survives a new start) */
int  scanner_no_sd(void);                     /* 1 if the last scan aborted: no SD mounted (library kept) */
int  scanner_skipped(void);                   /* unreadable/overlong files from the completed scan */
int  scanner_unsupported(void);               /* count of audio files present but not indexable (AAC/M4A/OGG/...) */

/* Audiobook chapters (Nero 'chpl' in an M4B/M4A). */
#define CHAP_TITLE 128
typedef struct { long start_ms; char title[CHAP_TITLE]; } chapter_t;
/* Blocking worker reader. Caller owns buffers; no shared state or UI calls.
 * Invalid arguments yield READ_FAILURE. Non-OK fields have count/string cleared. */
typedef enum {
    BOOK_META_OK, BOOK_META_ABSENT, BOOK_META_UNSUPPORTED,
    BOOK_META_READ_FAILURE, BOOK_META_ADMISSION_REFUSED
} book_meta_outcome_t;
typedef struct {
    book_meta_outcome_t chapters, narrator;
    int count;
} book_meta_result_t;
book_meta_result_t scan_read_book_meta(const char *path, chapter_t *out, int max,
                                      char *narrator, int cap);
int  scan_read_chapters(const char *path, chapter_t *out, int max);   /* count read (0 if none); chpl only */
int  scan_cue_files(const char *cue_path, char (*out)[1024], int max, int (*seen)(const char *));   /* Browse Files: the library files a .cue sheet names (the scanner's bounded resolver); count */
int  scan_read_narrator(const char *path, char *out, int cap);        /* 1 if the m4b names a narrator (composer/0xA9 + "wrt") */
#endif
