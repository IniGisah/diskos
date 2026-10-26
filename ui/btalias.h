/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef DISKOS_BTALIAS_H
#define DISKOS_BTALIAS_H
#include <stddef.h>

typedef enum {
    BTALIAS_OK, BTALIAS_ABSENT, BTALIAS_TIMEOUT, BTALIAS_ERROR,
    BTALIAS_MISMATCH, BTALIAS_INVALID
} btalias_result_t;

/* Blocking: worker threads ONLY. No LVGL or persistence. NULL MAC selects hci0;
 * otherwise exactly six colon-separated hex octets. Set validates new ASCII
 * names without normalization; an empty device alias resets to remote Name.
 * One deadline covers Set and readback (plus at most 100 ms reap grace).
 * ERROR includes malformed/oversize replies and local execution failures.
 * ABSENT means missing executable, bus/service unavailable, or no reply.
 * Calls may run concurrently; do not change the process environment meanwhile. */
btalias_result_t btalias_set(const char *mac, const char *alias, int timeout_ms);
/* Reads the effective Alias, preserving remote UTF-8. Output unchanged on failure. */
btalias_result_t btalias_get(const char *mac, char *out, size_t cap, int timeout_ms);
/* Trusted replay of a previously stored bounded UTF-8 adapter name. */
btalias_result_t btalias_replay(const char *, int timeout_ms);
btalias_result_t btalias_owner(char out[64], int timeout_ms);
btalias_result_t btalias_paired(char (*macs)[18], size_t capacity, size_t *count, int timeout_ms);
#endif
