/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CUSTOM_SAVER_PICKER_H
#define CUSTOM_SAVER_PICKER_H
#include "custom_saver_apply.h"

/* Data model of the custom lock-screen image picker (no widgets). Lists the flat card folder
 * /tmp/sdcard/CustomCover as stock does (mq_ui@0x47b6cc-0x47b724): jpg/jpeg/png/bmp/gif in any case; regular files only
 * (a diskOS choice: stock checks no file type),
 * GIFs <= 4194304 bytes and <= 364x364. Stills also respect the limits custom_still_start enforces, so the
 * list never offers an image apply would refuse. Browsing is read-only and never selects or installs anything.
 *
 * Not thread-safe: one scan object belongs to one thread (the UI timer). csp_step() reads at most `budget` directory
 * entries per call so the LVGL thread never blocks long. The card lease (sd_io_begin/end) is held only inside
 * csp_step() and csp_revalidate(), around the directory handle and every file handle, and released on every exit
 * path; the position between steps is a directory offset, not an open handle.
 *
 * Skipped (not images the user put there): dot-files (also macOS "._x.gif" shadows; a diskOS choice -
 * stock skips only "." and ".."), counted separately in dotfiles; names without a listed extension. Counted in csp_stats so nothing vanishes: over-limit
 * files, unreadable or invalid headers, names too long or with control characters (apply refuses both), and
 * non-regular entries with a listed extension (directory, symlink, fifo, device). Files with zero bytes or a
 * wrong header are "invalid". The list keeps the first CSP_MAX_ENTRIES in sort order whatever the readdir order is;
 * the rest are counted in hidden. Order: ASCII case-insensitive, ties byte-wise, as the Files browser (strcasecmp). */
#define CSP_DIR "/tmp/sdcard/CustomCover"
#define CSP_MAX_ENTRIES 128
#define CSP_NAME_MAX 232           /* longest name apply accepts (255 - strlen(folder + slash)) plus NUL */
#define CSP_EXAMINE_MAX 8192       /* directory entries read per scan; beyond it the scan stops and reports truncated */

typedef enum { CSP_SCANNING, CSP_READY, CSP_EMPTY, CSP_NO_CARD, CSP_ERROR } csp_state;
typedef enum { CSP_REV_OK, CSP_REV_GONE, CSP_REV_CHANGED, CSP_REV_REFUSED, CSP_REV_UNREADABLE, CSP_REV_NO_CARD,
               CSP_REV_INDEX } csp_reval;
typedef struct { char name[CSP_NAME_MAX]; int kind; long long size; } csp_entry;   /* kind: CUSTOM_SOURCE_GIF / _STILL */
typedef struct {
    unsigned examined;      /* directory entries read */
    unsigned too_large, dimensions, invalid, unreadable, name_long, name_bad, not_regular;
    unsigned hidden;        /* eligible but beyond CSP_MAX_ENTRIES */
    unsigned dotfiles;      /* dot-files skipped (macOS ._ shadows included); not part of the excluded count */
    int truncated;          /* CSP_EXAMINE_MAX reached: more files may exist */
    int folder_missing;     /* card present, CustomCover absent (state EMPTY) */
} csp_stats;
typedef struct csp_scan csp_scan;

csp_scan *csp_open(const char *dir);                   /* NULL = CSP_DIR; the folder is not touched until a step */
void csp_close(csp_scan *s);
void csp_restart(csp_scan *s);                         /* forget results, rescan from the start */
csp_state csp_step(csp_scan *s, unsigned budget);      /* NO_CARD retries on the next call; READY/EMPTY/ERROR are final */
csp_state csp_state_of(const csp_scan *s);
int csp_count(const csp_scan *s);
const csp_entry *csp_entry_at(const csp_scan *s, int i);   /* NULL out of range */
const csp_stats *csp_stats_of(const csp_scan *s);
int csp_path(const csp_scan *s, int i, char *out, size_t len);   /* 0 on success; the full path apply takes */
/* Re-lstat()s and rechecks entry i before any later use: GONE (file or folder removed), CHANGED (replaced by a
 * non-regular file or different size), REFUSED (now over a limit or bad header), UNREADABLE, NO_CARD. List untouched. */
csp_reval csp_revalidate(csp_scan *s, int i);
#endif
