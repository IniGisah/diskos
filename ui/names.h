/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef DISKOS_NAMES_H
#define DISKOS_NAMES_H

#define NAMES_MAX_BYTES 32
#define NAMES_STORED_BYTES 248
int names_saved_bt_valid(const char *);

typedef enum { NAMES_BT, NAMES_NETWORK } names_field_t;
typedef enum {
    NAMES_OK, NAMES_EMPTY, NAMES_TOO_LONG, NAMES_ASCII, NAMES_CONTROL,
    NAMES_HOST_CHARS, NAMES_HOST_EDGE, NAMES_WORDS
} names_result_t;

/* stored is the original value, or NULL for new entry. An unchanged stored
 * value is accepted byte for byte; callers must not normalize or rewrite it. */
names_result_t names_check(names_field_t field, const char *text, const char *stored);
/* For changed/new BT entry only. On failure out is untouched; no truncation. */
names_result_t names_bt_normalize(const char *text, char out[NAMES_MAX_BYTES + 1]);
const char *names_reason(names_result_t result); /* NULL means valid */

#endif
