/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CUSTOM_SAVER_OVERLAY_H
#define CUSTOM_SAVER_OVERLAY_H

/* Presentation model for the custom lock-screen image: which overlays are drawn over it, in which font choice and
 * where. ONE global presentation, stored in diskos.conf (never in the stock theme database, which is not touched).
 * This module is model only: names, validation, persistence. Fonts are chosen by id here; the renderer maps an id
 * to a real font later. UI-thread use only (the saver reads the value once when it starts). */

/* Stable overlay bits. The numbers are persisted: never renumber, only append. */
#define CUSTOM_OV_TIME       0x01
#define CUSTOM_OV_DATE       0x02
#define CUSTOM_OV_BATTERY    0x04
#define CUSTOM_OV_TRACK      0x08
#define CUSTOM_OV_WEATHER    0x10
#define CUSTOM_OV_ALL_BITS   0x1f
#define CUSTOM_OV_DEFAULT_MASK  (CUSTOM_OV_TIME | CUSTOM_OV_DATE)

/* The five designed font choices (persisted ids: never renumber). */
typedef enum { CUSTOM_OV_FONT_THEME, CUSTOM_OV_FONT_PLAIN, CUSTOM_OV_FONT_DIGITAL, CUSTOM_OV_FONT_MONO,
               CUSTOM_OV_FONT_ROUNDED, CUSTOM_OV_FONT_COUNT } custom_ov_font;
typedef enum { CUSTOM_OV_POS_TOP, CUSTOM_OV_POS_CENTRE, CUSTOM_OV_POS_BOTTOM, CUSTOM_OV_POS_COUNT } custom_ov_pos;

typedef struct { unsigned mask; custom_ov_font font; custom_ov_pos pos; } custom_ov_t;

/* Why a stored value was not used as written (one per field). */
typedef enum {
    CUSTOM_OV_OK,         /* present and valid */
    CUSTOM_OV_MISSING,    /* key absent: default */
    CUSTOM_OV_MALFORMED,  /* not a plain decimal number (empty, sign, space, hex, trailing text): default */
    CUSTOM_OV_RANGE,      /* number too large, or an unknown font/position id: default */
    CUSTOM_OV_MASKED      /* mask only: unknown bits were dropped, known bits kept */
} custom_ov_issue;
typedef struct { custom_ov_issue mask, font, pos; } custom_ov_report;

void custom_ov_defaults(custom_ov_t *out);                  /* mask 3 (time+date), font Theme, position Bottom */
/* Pure parse of the three stored strings (NULL = key absent). Never fails: every bad field falls back to its default. */
void custom_ov_parse(const char *mask, const char *font, const char *pos, custom_ov_t *out, custom_ov_report *rep);
/* The live value the saver reads: parsed from the config store on every call. Writes nothing. rep may be NULL. */
custom_ov_t custom_ov_get(custom_ov_report *rep);

const char *custom_ov_font_name(int font);                  /* "Theme" "Plain" "Digital" "Mono" "Rounded"; NULL if unknown */
const char *custom_ov_pos_name(int pos);                    /* "Top" "Centre" "Bottom"; NULL if unknown */
const char *custom_ov_bit_name(unsigned bit);               /* "Time" "Date" "Battery" "Track" "Weather"; NULL if not one bit */

/* Draft: edits never reach the live value until custom_ov_draft_save() succeeds. */
void custom_ov_draft_begin(void);                           /* draft = live value (discards any older draft) */
custom_ov_t custom_ov_draft_get(void);
int  custom_ov_draft_set_bit(unsigned bit, int on);         /* 0 ok, -1 not exactly one known bit */
int  custom_ov_draft_set_font(int font);                    /* 0 ok, -1 unknown id (draft unchanged) */
int  custom_ov_draft_set_pos(int pos);                      /* 0 ok, -1 unknown id (draft unchanged) */
int  custom_ov_draft_dirty(void);                           /* draft differs from the live value */
/* Persist through one config batch.
 * 0: durable success, or unchanged with no durability retry pending.
 * -1: save failed; live value unchanged; draft retained.
 * -2: file replaced but durability unconfirmed; live value follows
 *     the draft; draft retained and Save retries persistence. */
int  custom_ov_draft_save(void);
void custom_ov_draft_discard(void);                         /* Back: drop the draft, live value untouched */

#ifndef CUSTOM_OV_MODEL_ONLY   /* defined by host tests that compile the model without LVGL, the theme and the catalog */
#include "lvgl/lvgl.h"
#include "screens.h"   /* weather_snapshot_t */
/* ---- Font resolver and localized text (slice 11). Which face draws the overlay's clock and its other text. ---- */

/* Supporting-text size roles (the theme's UI 14 / 16 / 20 faces; never stepped by the Font Size setting). */
typedef enum { CUSTOM_OV_SIZE_SMALL, CUSTOM_OV_SIZE_NORMAL, CUSTOM_OV_SIZE_LARGE, CUSTOM_OV_SIZE_COUNT } custom_ov_size;
typedef struct {
    const lv_font_t *clock;   /* digits, colon, space, hyphen, A/M/P only: use custom_ov_pick() for any other text */
    const lv_font_t *text;    /* the supporting face with the international/CJK fallback chain attached */
} custom_ov_faces_t;

/* The faces for a font choice. Theme follows the ACTIVE theme; Plain/Digital/Mono/Rounded are fixed whatever the
 * active theme is (Plain = Montserrat clock + the international chain, Digital = hi-fi, Mono = blueprint, Rounded =
 * stone). Never activates a theme. Unknown choice or size: .clock and .text are NULL. The pointers stay valid until
 * the next call; call again after a language or theme change. */
custom_ov_faces_t custom_ov_faces(int font, int size);
/* 1 = every character of s has a glyph in font f ITSELF (its fallback chain is not consulted); "" is covered; 0 for
 * NULL or malformed UTF-8. This is the clock-font guard. */
int custom_ov_font_covers(const lv_font_t *f, const char *s);
/* The face to draw s with: the clock face only when it covers all of s, else the text face. */
const lv_font_t *custom_ov_pick(const custom_ov_faces_t *faces, const char *s);

/* Short date for the overlay in language `lang` (an i18n_lang_t): translated weekday and month tokens in that
 * language's conventional order, e.g. en "Fri 17 Oct", de "Fr. 17. Okt"; ja, zh, tw and ko write month and day as numbers
 * with the language's unit characters. wday 0..6
 * (Sunday = 0), mday 1..31, mon 0..11. Writes at most cap bytes including the NUL and returns the length; 0 and an
 * empty string for an out-of-range argument. CUSTOM_OV_DATE_MAX always suffices. */
#define CUSTOM_OV_DATE_MAX 48
int custom_ov_date_text(int lang, int wday, int mday, int mon, char *dst, int cap);
/* "AM" or "PM" in language lang: 12-hour clocks show the language's own words in zh, tw, ja and ko; every other
 * language keeps AM / PM (they read a 24-hour clock). */
const char *custom_ov_meridiem(int lang, int pm);

/* ---- Overlay group (slice 12): the composition drawn over the custom image. No feeds, no saver lifecycle here. ---- */

/* Geometry (design): the padded group stays inside the safe box and is at most 232 x 224, centred on x = 180. */
#define CUSTOM_OV_SAFE_X0 64
#define CUSTOM_OV_SAFE_Y0 68
#define CUSTOM_OV_SAFE_X1 296
#define CUSTOM_OV_SAFE_Y1 292
#define CUSTOM_OV_MAX_W   232
#define CUSTOM_OV_MAX_H   224
#define CUSTOM_OV_PAD_X   12
#define CUSTOM_OV_PAD_Y   8
#define CUSTOM_OV_GAP     4      /* between rows */
#define CUSTOM_OV_GAP_X   12     /* between the date and the battery in a shared row */
#define CUSTOM_OV_SCRIM_OPA 184  /* black, about 72% */
#define CUSTOM_OV_RADIUS  14

typedef struct { int x, y, w, h; } custom_ov_box;

/* Measured pixel sizes of the WORST-CASE content of every enabled element (so a changing value never moves a box).
 * Plain numbers: the layout itself touches no font and draws nothing. */
typedef struct {
    int clock_w, clock_h;        /* widest "dd:dd" in the clock face: advance width and INK height (unscaled) */
    int mer_w, mer_asc, mer_desc;/* widest meridiem word; line box above / below its baseline; mer_w 0 = 24-hour clock */
    int line_h;                  /* supporting text row height */
    int date_w;                  /* widest short date */
    int mark_w, bat_w;           /* charge marker slot and widest "100%" */
    int wx_icon_w, wx_temp_w, wx_cached_w, wx_h;
    int pause_w;                 /* pause marker slot before the title */
} custom_ov_metrics_t;

typedef struct {
    int x, y, w, h;              /* the group with its scrim (screen coordinates); all 0 when nothing is enabled */
    int scale;                   /* clock scale, 256 = natural */
    int stacked;                 /* date and battery on separate rows */
    custom_ov_box clock, mer, date, mark, bat, pause, title, artist, wx;   /* w = 0: not part of this group */
} custom_ov_layout_t;

/* Pure: every box from the metrics, the enabled mask and the vertical position. Row order: time, date/battery,
 * title, artist, weather. Rows of enabled overlays are always reserved. */
void custom_ov_layout(const custom_ov_metrics_t *m, unsigned mask, int pos, custom_ov_layout_t *out);

typedef struct {
    char time[16];               /* digits only, e.g. "5:53" or "17:05" */
    char meridiem[16];           /* "" in 24-hour mode */
    char date[CUSTOM_OV_DATE_MAX];
    int battery;                 /* 0..100, -1 unknown */
    int charging;
    int has_track, paused;
    char title[96], artist[96];
    int wx_band;                 /* wx_band_t: HIDDEN / FRESH / CACHED */
    int wx_icon, wx_temp;        /* WX_ICON_*, degrees */
    char wx_unit;                /* 'C', 'F' or 0 */
} custom_ov_data_t;

typedef enum { CUSTOM_OV_PART_ROOT, CUSTOM_OV_PART_CLOCK, CUSTOM_OV_PART_MER, CUSTOM_OV_PART_DATE, CUSTOM_OV_PART_BAT,
               CUSTOM_OV_PART_MARK, CUSTOM_OV_PART_TITLE, CUSTOM_OV_PART_ARTIST, CUSTOM_OV_PART_PAUSE,
               CUSTOM_OV_PART_WX_ICON, CUSTOM_OV_PART_WX_TEMP, CUSTOM_OV_PART_WX_CACHED, CUSTOM_OV_PART_COUNT } custom_ov_part;
typedef struct custom_ov_group custom_ov_group_t;

/* "..." (ASCII) cut at a character boundary so that the text fits max_w in font f; returns the drawn width. */
int custom_ov_fit_text(const lv_font_t *f, const char *s, int max_w, char *dst, int cap);
/* Build the group on `parent` (the 360 x 360 saver root; the group is not clickable). model: enabled overlays, font
 * choice, position; lang: i18n_lang_t. Sizes are measured now and kept until the next create. NULL on failure.
 * With no overlay enabled the group has no children and nothing is drawn. */
custom_ov_group_t *custom_ov_group_create(lv_obj_t *parent, const custom_ov_t *model, int lang, const custom_ov_data_t *d);
/* New values: only the labels whose text changed are touched, and nothing is measured, unless the 24-hour / 12-hour
 * form changed (the one value that changes reserved space). */
void custom_ov_group_update(custom_ov_group_t *g, const custom_ov_data_t *d);
void custom_ov_group_destroy(custom_ov_group_t *g);       /* deletes its objects */
const custom_ov_layout_t *custom_ov_group_layout(const custom_ov_group_t *g);
int custom_ov_group_relayouts(const custom_ov_group_t *g);/* how many times the boxes were computed (1 after create) */
lv_obj_t *custom_ov_group_part(const custom_ov_group_t *g, int part);   /* NULL when the part does not exist */

/* ---- Overlay feeds (slice 13): build custom_ov_data_t from the app's existing state and push it to a live group. ----
 * No timer, no polling and no lifecycle here: the saver's own clock tick, the status poll and the saver's track and
 * playing setters call in. The group is attached by the saver lifecycle (slice 14); with none attached the feed only
 * remembers its inputs, so a later attach starts from current values. */
typedef struct {
    int lang;                              /* i18n_lang_t of the group */
    int h24;                               /* the existing time_24h preference */
    int hour, min, wday, mday, mon;        /* local time: 0..23, 0..59, Sunday = 0, 1..31, 0..11 */
    int battery, charging;                 /* battery -1 = unknown */
    int has_track, playing;                /* has_track: a track is loaded (playing or paused) */
    const char *title, *artist;            /* title already carries the player's fallback; NULL = none */
    const weather_snapshot_t *wx;          /* a snapshot taken for THIS build (the band is recomputed here); NULL = none */
} custom_ov_inputs_t;
/* Pure: no clock, no config, no widgets. 24-hour time is "HH:MM", 12-hour is "H:MM" with the meridiem apart. The clock
 * text holds digits and the colon only. Text is cut at a character boundary. The weather band comes from
 * weather_snapshot_band(in->wx) and nothing is kept: icon and temperature are zero while it is hidden. */
void custom_ov_build_data(const custom_ov_inputs_t *in, custom_ov_data_t *out);

/* Source seam (host tests): fills in->h24, hour, min, wday, mday, mon from a fake clock and takes the weather snapshot
 * into *wx (a fake one). NULL = the real clock, the time_24h preference and weather_snapshot_get(). */
typedef void (*custom_ov_now_fn)(custom_ov_inputs_t *in, weather_snapshot_t *wx);
void custom_ov_feed_set_sources(custom_ov_now_fn fn);
/* Attach the live group (initial = the data it was created with, so the first tick pushes nothing if nothing changed;
 * NULL = unknown). NULL g: detach, before the group is destroyed. The feed keeps no pointer to any part. */
void custom_ov_feed_attach(custom_ov_group_t *g, int lang, const custom_ov_data_t *initial);
/* The data a group should be created with right now: real sources, remembered battery and track. */
void custom_ov_feed_current(int lang, custom_ov_data_t *out);
int  custom_ov_feed_pushes(void);                             /* custom_ov_group_update calls made by the feed so far */
/* Settings Preview only (slice 17d2): the feed fills a Now Playing overlay that has no track and a Weather overlay that has no
 * reading with labelled samples. note != NULL turns it on (the lock screen never calls this, so it never shows a sample); NULL turns
 * it off. note() runs after a feed update changed what the notes say. Samples start no weather request and change no feed input. */
#define CUSTOM_OV_NOTE_SAMPLE_TRACK 1u    /* the title and artist drawn are samples */
#define CUSTOM_OV_NOTE_SAMPLE_WEATHER 2u  /* the weather drawn is a sample */
#define CUSTOM_OV_NOTE_WEATHER_OFF 4u     /* weather is off globally: no weather is drawn */
void custom_ov_feed_preview(void (*note)(void));
unsigned custom_ov_feed_notes(void);                          /* CUSTOM_OV_NOTE_* of the latest build or update; 0 when the preview mode is off */
#endif

#endif
