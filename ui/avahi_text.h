/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AVAHI_TEXT_H
#define DISKOS_AVAHI_TEXT_H
/* Shared text decoding for the Cast and AirPlay discovery parsers. */
#include <stddef.h>
#include <string.h>
/* Avahi escapes: \DDD decimal byte, \c literal. Writes at most cap-1 bytes,
 * stopping at stop (NUL, ';' or '"' per caller). Returns length or -1. */
static int unesc(const char **in, char *out, size_t cap, char stop)
{
    const char *s = *in; size_t n = 0;
    while (*s && *s != stop) {
        int c = (unsigned char)*s++;
        if (c == '\\') {
            if (s[0] >= '0' && s[0] <= '9') {
                if (s[1] < '0' || s[1] > '9' || s[2] < '0' || s[2] > '9') return -1;
                c = (s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0'); s += 3;
                if (c < 1 || c > 255) return -1;
            } else if (!(c = (unsigned char)*s++)) return -1;
        }
        if (c < 0x20 || c == 0x7f || n + 1 >= cap) return -1;
        out[n++] = (char)c;
    }
    out[n] = 0; *in = s;
    return (int)n;
}
/* Strict UTF-8 (no overlongs, surrogates or code points above 10FFFF). */
static int utf8_ok(const unsigned char *p)
{
    while (*p) {
        unsigned c = *p++; int k; unsigned long cp;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c < 0xe0) { k = 1; cp = c & 0x1f; }
        else if (c >= 0xe0 && c < 0xf0) { k = 2; cp = c & 0x0f; }
        else if (c >= 0xf0 && c < 0xf5) { k = 3; cp = c & 7; }
        else return 0;
        for (int i = 0; i < k; i++) { if ((*p & 0xc0) != 0x80) return 0; cp = cp << 6 | (*p++ & 0x3f); }
        if ((k == 2 && cp < 0x800) || (k == 3 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp < 0xe000)) return 0;
    }
    return 1;
}
/* Copy src into dst[cap]; too long: truncate on a character boundary or fail. */
static int put(char *dst, size_t cap, const char *src, int truncate)
{
    size_t n = strlen(src);
    if (!utf8_ok((const unsigned char *)src)) return -1;
    if (n >= cap) {
        if (!truncate) return -1;
        for (n = cap - 1; n && ((unsigned char)src[n] & 0xc0) == 0x80; n--) ;
    }
    memcpy(dst, src, n); dst[n] = 0;
    return 0;
}
/* Value of key in an avahi TXT list: "k=v" "k=v" ...; 1 found, 0 absent, -1 malformed. */
static int txt_get(const char *s, const char *key, char *out, size_t cap)
{
    int found = 0; size_t kl = strlen(key); out[0] = 0;
    for (;;) {
        char tok[300];
        while (*s == ' ') s++;
        if (!*s) return found;
        if (*s++ != '"' || unesc(&s, tok, sizeof tok, '"') < 0 || *s++ != '"' || (*s && *s != ' ')) return -1;
        if (!strncmp(tok, key, kl) && tok[kl] == '=') {
            if (found || strlen(tok + kl + 1) >= cap) return -1;
            strcpy(out, tok + kl + 1); found = 1;
        }
    }
}
static int port_of(const char *s)
{
    long v = 0; size_t n = strlen(s);
    if (!n || n > 5) return 0;
    for (; *s; s++) { if (*s < '0' || *s > '9') return 0; v = v * 10 + *s - '0'; }
    return v >= 1 && v <= 65535 ? (int)v : 0;
}
#endif
