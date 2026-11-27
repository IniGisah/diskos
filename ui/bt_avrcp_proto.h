/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef DISKOS_BT_AVRCP_PROTO_H
#define DISKOS_BT_AVRCP_PROTO_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define AVRCP_LINEBUF_CAP 2048

typedef struct {
    char buf[AVRCP_LINEBUF_CAP];
    size_t len;
    int dropping;
} avrcp_linebuf_t;

static inline void avrcp_linebuf_init(avrcp_linebuf_t *lb) {
    if (!lb) return;
    lb->len = 0;
    lb->dropping = 0;
    lb->buf[0] = '\0';
}

/* Feed raw stream bytes into an incremental line buffer. Calls on_line() for each
 * complete '\n'-terminated line (with trailing '\r' stripped). Oversized lines
 * without '\n' are discarded up to the next '\n' so subsequent frames recover cleanly. */
static inline void avrcp_linebuf_feed(avrcp_linebuf_t *lb, const char *data, size_t n,
                                      void (*on_line)(const char *line, void *ctx), void *ctx) {
    if (!lb || !data || n == 0) return;
    size_t pos = 0;
    while (pos < n) {
        if (lb->dropping) {
            const char *nl = (const char *)memchr(data + pos, '\n', n - pos);
            if (!nl) return;
            pos = (size_t)(nl - data) + 1;
            lb->dropping = 0;
            lb->len = 0;
            continue;
        }
        size_t avail = (sizeof(lb->buf) - 1) - lb->len;
        if (avail == 0) {
            lb->len = 0;
            lb->dropping = 1;
            continue;
        }
        size_t take = n - pos;
        if (take > avail) take = avail;
        memcpy(lb->buf + lb->len, data + pos, take);
        lb->len += take;
        pos += take;

        for (;;) {
            char *nl = (char *)memchr(lb->buf, '\n', lb->len);
            if (!nl) break;
            size_t line_len = (size_t)(nl - lb->buf);
            size_t trim = line_len;
            while (trim > 0 && lb->buf[trim - 1] == '\r') trim--;
            lb->buf[trim] = '\0';
            if (on_line && trim > 0) on_line(lb->buf, ctx);
            size_t rem = lb->len - (line_len + 1);
            if (rem > 0) memmove(lb->buf, nl + 1, rem);
            lb->len = rem;
        }

        if (lb->len == sizeof(lb->buf) - 1 && pos < n) {
            lb->len = 0;
            lb->dropping = 1;
        }
    }
}

/* Copy metadata field with guaranteed NUL termination, replacing CR/LF with spaces
 * so tag strings cannot split protocol lines. */
static inline void avrcp_sanitize_field(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) {
        char c = src[i];
        dst[i] = (c == '\r' || c == '\n') ? ' ' : c;
    }
    dst[i] = '\0';
}

#endif /* DISKOS_BT_AVRCP_PROTO_H */
