/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cast_format.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
/* The scanner (scanner.c mp4_find_child, ogg/flac tag walkers) and songinfo.c (si_probe_*) parse tags over stdio or pread for display and
 * tolerate damage. This classifier needs different guarantees (a hard read budget, a verdict, MP4 sample entry), so it keeps its own
 * small parsers and matches their box-walk rules (32-bit, 64-bit largesize, size 0 = to the end of the parent, no overrun). */
typedef struct { const cast_format_src *s; cast_format_info *o; size_t used; int reads, err; } cf;
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }
static int be16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static int le16(const uint8_t *p) { return p[1] << 8 | p[0]; }
/* The only read path: range-checks against the file size and the budget, then calls the reader. Sets c->err on failure. */
static int rd(cf *c, uint64_t off, void *b, size_t n) {
    if (c->err) return 0;
    if (off > c->s->size || n > c->s->size - off) { c->err = CF_R_MALFORMED; return 0; }   /* truncated: a length field points past the file */
    if (c->used + n > CF_BUDGET || ++c->reads > CF_MAX_READS) { c->err = CF_R_BUDGET; return 0; }
    c->used += n;
    int r = c->s->read(c->s->ctx, off, b, n);
    if (r < 0 || (size_t)r != n) { c->err = CF_R_IO; return 0; }
    return 1;
}
static void set(cf *c, int k, int codec, uint32_t rate, int bits, int ch, const char *mime) {
    c->o->container = k; c->o->codec = codec; c->o->rate = rate; c->o->bits = bits; c->o->channels = ch; c->o->mime = mime;
}
/* ---- MP3: Layer III only (MPEG-1/2/2.5). A frame header is believed only if the next one, when inside the 4 KiB window, agrees. ---- */
static int mp3h(const uint8_t *p, uint32_t *sr, int *ch, uint32_t *fl) {
    static const uint16_t B1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}, B2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    static const uint16_t SR[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
    int v = (p[1] >> 3) & 3, bi = p[2] >> 4, si = (p[2] >> 2) & 3;
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0 || v == 1 || ((p[1] >> 1) & 3) != 1 || bi == 0 || bi == 15 || si == 3) return 0;
    *sr = SR[v == 3 ? 0 : v == 2 ? 1 : 2][si]; *ch = (p[3] >> 6) == 3 ? 1 : 2;
    *fl = (v == 3 ? 144 : 72) * 1000u * (v == 3 ? B1[bi] : B2[bi]) / *sr + ((p[2] >> 1) & 1);
    return 1;
}
static void mp3(cf *c, uint64_t b) {
    uint8_t w[4096 + 4]; uint32_t sr, sr2, fl; int ch, ch2;
    size_t n = c->s->size - b < sizeof w ? (size_t)(c->s->size - b) : sizeof w;
    if (!rd(c, b, w, n)) return;
    for (size_t i = 0; i + 4 <= n; i++) {
        if (!mp3h(w + i, &sr, &ch, &fl)) continue;
        uint8_t next[4]; uint32_t fl2;
        uint64_t left = c->s->size - b - i;
        if (fl > left || left - fl < sizeof next) continue;
        if (!rd(c, b + i + fl, next, sizeof next)) return;
        if (!mp3h(next, &sr2, &ch2, &fl2) ||
            sr2 != sr || fl2 > left - fl) continue;
        set(c, CF_K_MPEG, CF_C_MP3, sr, 0, ch, "audio/mpeg"); return;
    }
}
/* ---- FLAC: STREAMINFO first, then the metadata block chain (<= 64 blocks), then a frame sync ---- */
static void flac(cf *c, uint64_t b) {
    uint8_t h[4], si[34]; uint64_t pos = b + 4;
    for (int i = 0;; i++) {
        if (i == 64 || !rd(c, pos, h, 4)) { if (!c->err) c->err = CF_R_MALFORMED; return; }
        if ((h[0] & 0x7F) == 127 || (i == 0 && ((h[0] & 0x7F) != 0 || (be32(h) & 0xFFFFFF) != 34))) { c->err = CF_R_MALFORMED; return; }
        if (i == 0 && !rd(c, pos + 4, si, 34)) return;
        pos += 4 + ((uint64_t)h[1] << 16 | h[2] << 8 | h[3]);
        if (h[0] & 0x80) break;
    }
    if (!rd(c, pos, h, 2)) return;
    if (h[0] != 0xFF || (h[1] & 0xFE) != 0xF8) { c->err = CF_R_MALFORMED; return; }
    set(c, CF_K_FLAC, CF_C_FLAC, (uint32_t)si[10] << 12 | si[11] << 4 | si[12] >> 4, (((si[12] & 1) << 4) | si[13] >> 4) + 1, ((si[12] >> 1) & 7) + 1, "audio/flac");
}
/* ---- Ogg: first page must be a BOS page; only its first packet's identification header is read ---- */
static void ogg(cf *c) {
    uint8_t h[27], seg[255], p[32]; uint64_t body = 0; unsigned plen = 0, i; int open = 1;
    if (!rd(c, 0, h, 27)) return;
    if (h[4] || !(h[5] & 2) || !h[26] || !rd(c, 27, seg, h[26])) { if (!c->err) c->err = CF_R_MALFORMED; return; }
    for (i = 0; i < h[26]; i++) { body += seg[i]; if (open) { plen += seg[i]; open = seg[i] == 255; } }
    if (h[5] != 2 || open ||
        27 + (uint64_t)h[26] + body > c->s->size || !plen) {
        c->err = CF_R_MALFORMED; return;
    }
    if (!rd(c, 27 + h[26], p, plen < sizeof p ? plen : sizeof p)) return;
    set(c, CF_K_OGG, CF_C_OTHER, 0, 0, 0, "audio/ogg");   /* Speex, Theora, Ogg FLAC...: recognised container, nothing tested */
    if (plen >= 19 && !memcmp(p, "OpusHead", 8) && p[8] < 16) {
        unsigned ch = p[9], fam = p[18];
        if (!fam) {
            if (!ch || ch > 2 || plen != 19) {
                c->err = CF_R_MALFORMED; return;
            }
        } else if (fam == 1 && ch <= 2 && ch) {
            if (plen != 21 + ch || p[19] == 0 ||
                p[20] > p[19] || p[19] + p[20] > 255) {
                c->err = CF_R_MALFORMED; return;
            }
            for (unsigned j = 0; j < ch; j++)
                if (p[21 + j] != 255 &&
                    p[21 + j] >= p[19] + p[20]) {
                    c->err = CF_R_MALFORMED; return;
                }
        } else return; /* recognised but untested */
        set(c, CF_K_OGG, CF_C_OPUS, 48000, 0, ch, "audio/ogg");
    }
    else if (plen >= 30 && !memcmp(p, "\1vorbis", 7) && !le32(p + 7)) {
        unsigned lo = p[28] & 15, hi = p[28] >> 4;
        if (plen != 30 || lo < 6 || hi > 13 || lo > hi ||
            !(p[29] & 1)) {
            c->err = CF_R_MALFORMED; return;
        }
        set(c, CF_K_OGG, CF_C_VORBIS, le32(p + 12), 0, p[11], "audio/ogg");
    }
}
/* ---- WAV: RIFF chunk walk (<= 16 chunks, odd sizes padded), fmt PCM or EXTENSIBLE with the PCM subformat GUID ---- */
static void wav(cf *c) {
    uint8_t h[8], f[40]; uint64_t pos = 12, end;
    if (!rd(c, 4, h, 4)) return;
    end = 8 + (uint64_t)le32(h);
    if (end < 12 || end > c->s->size) {
        c->err = CF_R_MALFORMED; return;
    }
    for (int i = 0; i < 16; i++) {
        if (pos > end || end - pos < 8) {
            c->err = CF_R_MALFORMED; return;
        }
        if (!rd(c, pos, h, 8)) return;
        uint32_t sz = le32(h + 4);
        uint64_t span = (uint64_t)sz + (sz & 1u);
        if (span > end - pos - 8) {
            c->err = CF_R_MALFORMED; return;
        }
        if (!memcmp(h, "fmt ", 4)) {
            int tag, ch, bits; uint32_t rate;
            if (sz < 16 || !rd(c, pos + 8, f, sz < sizeof f ? sz : sizeof f)) { if (!c->err) c->err = CF_R_MALFORMED; return; }
            tag = le16(f); ch = le16(f + 2); rate = le32(f + 4); bits = le16(f + 14);
            if (tag == 0xFFFE) {   /* EXTENSIBLE: the real format is the first two bytes of the subformat GUID, whose tail must be the standard one */
                static const uint8_t G[14] = {0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
                if (sz < 40 || le16(f + 16) < 22 ||
                    (uint32_t)le16(f + 16) > sz - 18) {
                    c->err = CF_R_MALFORMED; return;
                }
                if (memcmp(f + 26, G, 14)) {
                    set(c, CF_K_WAV, CF_C_OTHER, rate, bits, ch, "audio/wav");
                    return;
                }
                tag = le16(f + 24);
            }
            set(c, CF_K_WAV, tag == 1 ? CF_C_PCM : CF_C_OTHER, rate, bits, ch, "audio/wav"); return;
        }
        pos += 8 + (uint64_t)sz + (sz & 1);
    }
    c->err = CF_R_BUDGET;
}
/* ---- MP4: ftyp, then moov/trak/mdia/(hdlr soun)/minf/stbl/stsd found by walking box HEADERS only (bodies are skipped) ---- */
static int box(cf *c, uint64_t *pos, uint64_t end, const char *t, uint64_t *pay, uint64_t *bend) {
    uint8_t h[16];
    while (*pos + 8 <= end) {
        uint64_t sz, hl = 8, at = *pos;
        if (!rd(c, at, h, 8)) return 0;
        sz = be32(h);
        if (sz == 1) {   /* 64-bit largesize */
            if (at + 16 > end) { c->err = CF_R_MALFORMED; return 0; }
            if (!rd(c, at + 8, h + 8, 8)) return 0;
            sz = (uint64_t)be32(h + 8) << 32 | be32(h + 12); hl = 16;
        } else if (!sz) sz = end - at;   /* size 0: to the end of the parent */
        if (sz < hl || sz > end - at) { c->err = CF_R_MALFORMED; return 0; }
        *pos = at + sz;
        if (!memcmp(h + 4, t, 4)) { *pay = at + hl; *bend = *pos; return 1; }
    }
    return 0;
}
static int sub(cf *c, uint64_t p, uint64_t e, const char *t, uint64_t *pay, uint64_t *bend) { return box(c, &p, e, t, pay, bend); }
static int desc(const uint8_t *b, int *p, int n, int tag, int *end) {
    uint32_t len = 0;
    if (*p >= n || b[(*p)++] != tag) return 0;
    for (int k = 0; k < 4; k++) {
        if (*p >= n) return 0;
        unsigned v = b[(*p)++];
        len = (len << 7) | (v & 127);
        if (!(v & 128)) {
            if (len > (uint32_t)(n - *p)) return 0;
            *end = *p + (int)len;
            return 1;
        }
    }
    return 0;
}
static int descriptors(const uint8_t *b, int p, int end) {
    while (p < end) {
        int next;
        if (!desc(b, &p, end, b[p], &next)) return 0;
        p = next;
    }
    return 1;
}
static int esds_oti(const uint8_t *b, int n) {   /* ES_Descriptor (3) then DecoderConfigDescriptor (4); returns objectTypeIndication or -1 */
    int i = 4, ee, de, flags, oti;
    if (!desc(b, &i, n, 3, &ee) || ee - i < 3) return -1;
    flags = b[i + 2]; i += 3;
    if (flags & 0x80) {
        if (ee - i < 2) return -1;
        i += 2;
    }
    if (flags & 0x40) {
        if (i >= ee) return -1;
        int len = b[i++];
        if (len > ee - i) return -1;
        i += len;
    }
    if (flags & 0x20) {
        if (ee - i < 2) return -1;
        i += 2;
    }
    if (!desc(b, &i, ee, 4, &de) || de - i < 13) return -1;
    oti = b[i];
    if ((b[i + 1] >> 2) != 5) return -1;
    if (!descriptors(b, i + 13, de) ||
        !descriptors(b, de, ee) ||
        !descriptors(b, ee, n)) return -1;
    return oti;
}
static void mp4(cf *c) {
    uint64_t top = 0, mv, mve, tr, a, ae, m, me, hd, hde, mi, mie, sb, sbe, sd, sde, es, ese, ep;
    uint8_t h[12], e[36], d[256]; int ver, oti, n;
    if (!box(c, &top, c->s->size, "moov", &mv, &mve)) { if (!c->err) c->err = CF_R_MALFORMED; return; }
    for (tr = mv, n = 0; n < 16 && box(c, &tr, mve, "trak", &a, &ae); n++) {
        if (!sub(c, a, ae, "mdia", &m, &me) || !sub(c, m, me, "hdlr", &hd, &hde)) { if (c->err) return; continue; }
        if (hde - hd < 12 || !rd(c, hd, h, 12)) { if (!c->err) c->err = CF_R_MALFORMED; return; }
        if (memcmp(h + 8, "soun", 4)) continue;
        if (!sub(c, m, me, "minf", &mi, &mie) || !sub(c, mi, mie, "stbl", &sb, &sbe) || !sub(c, sb, sbe, "stsd", &sd, &sde)) { if (c->err) return; continue; }
        if (sde - sd < 8 || !rd(c, sd, h, 8)) {
            if (!c->err) c->err = CF_R_MALFORMED;
            return;
        }
        if (!be32(h + 4)) { c->err = CF_R_MALFORMED; return; }
        ep = sd + 8;   /* version/flags + entry count, then the first sample entry */
        if (sde - sd < 8 + 36 || !rd(c, ep, e, 36) || be32(e) < 36 || be32(e) > sde - ep) { if (!c->err) c->err = CF_R_MALFORMED; return; }
        set(c, CF_K_MP4, CF_C_OTHER, be32(e + 32) >> 16, 0, be16(e + 24), "audio/mp4");
        if (!memcmp(e + 4, "alac", 4)) c->o->codec = CF_C_ALAC;
        else if (!memcmp(e + 4, "mp4a", 4)) {
            ver = be16(e + 16);
            uint64_t kid = ep + 36 + (ver == 1 ? 16 : 0), kend = ep + be32(e);
            if (ver > 1) return;
            if (kid <= kend && sub(c, kid, kend, "esds", &es, &ese) && ese - es >= 8) {
                if (ese - es > sizeof d) {
                    c->err = CF_R_BUDGET; return;
                }
                size_t k = (size_t)(ese - es);
                if (!rd(c, es, d, k)) return;
                oti = esds_oti(d, (int)k);
                if (oti < 0) {
                    c->err = CF_R_MALFORMED; return;
                }
                if (oti == 0x40 || (oti >= 0x66 && oti <= 0x68)) c->o->codec = CF_C_AAC;   /* MPEG-4 AAC, MPEG-2 AAC LC/Main/SSR */
            }
        }
        return;
    }
    if (!c->err) c->err = CF_R_MALFORMED;   /* no audio track description */
}
/* ---- verdict rules shared by every receiver (receiver rules live in cast_format_profile) ---- */
static void judge(cast_format_info *o) {
    int pcm = o->codec == CF_C_FLAC || o->codec == CF_C_PCM;
    o->verdict = CF_V_EXCLUDED; o->reason = CF_R_NONE;
    if (o->codec == CF_C_NONE || o->codec == CF_C_OTHER) o->reason = CF_R_UNTESTED;   /* includes WAV float/ADPCM, Ogg FLAC/Speex, AAC-less MP4 */
    else if (o->codec == CF_C_ALAC) o->reason = CF_R_FORMAT;
    else if (o->channels < 1 || o->channels > 2 || (pcm && o->bits != 16 && o->bits != 24)) o->reason = CF_R_UNTESTED;
    else if (!o->rate) { o->verdict = CF_V_UNKNOWN; o->reason = CF_R_RATE_UNKNOWN; }
    else if (pcm && o->rate > 96000) o->reason = CF_R_RATE_TOO_HIGH;
    else o->verdict = CF_V_OK;
}
/* Leading ID3v2 tag: *base = offset of the first byte after it (0 = none). Syncsafe size; the footer flag (v2.4 only) is counted. */
static void id3(cf *c, uint64_t *base) {
    uint8_t h[10];
    *base = 0;
    if (c->s->size < 10 || !rd(c, 0, h, 10) || memcmp(h, "ID3", 3)) return;
    if (h[3] == 0xFF || h[4] == 0xFF || ((h[6] | h[7] | h[8] | h[9]) & 0x80)) { c->err = CF_R_MALFORMED; return; }
    *base = 10 + ((uint64_t)h[6] << 21 | h[7] << 14 | h[8] << 7 | h[9]) + (h[3] == 4 && (h[5] & 0x10) ? 10 : 0);
    if (*base >= c->s->size) c->err = CF_R_MALFORMED;
}
static void sniff(cf *c) {
    uint8_t h[16]; uint64_t b; size_t n;
    id3(c, &b);
    if (c->err) return;
    n = c->s->size - b < sizeof h ? (size_t)(c->s->size - b) : sizeof h;
    if (!rd(c, b, h, n)) return;
    if (n >= 4 && !memcmp(h, "fLaC", 4)) flac(c, b);
    else if (n >= 4 && (!memcmp(h, "MAC ", 4) || !memcmp(h, "wvpk", 4) || !memcmp(h, "DSD ", 4) || !memcmp(h, "FRM8", 4))) {
        c->o->container = !memcmp(h, "MAC ", 4) ? CF_K_APE : !memcmp(h, "wvpk", 4) ? CF_K_WAVPACK : CF_K_DSD; c->o->verdict = CF_V_EXCLUDED; c->o->reason = CF_R_FORMAT;
    } else if (b) {
        uint32_t sr, fl; int ch;
        if (n >= 4 && mp3h(h, &sr, &ch, &fl)) mp3(c, b);
    }
    else if (n >= 4 && !memcmp(h, "OggS", 4)) ogg(c);
    else if (n >= 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) wav(c);
    else if (n >= 12 && !memcmp(h, "FORM", 4) && (!memcmp(h + 8, "AIFF", 4) || !memcmp(h + 8, "AIFC", 4))) { c->o->container = CF_K_AIFF; c->o->verdict = CF_V_EXCLUDED; c->o->reason = CF_R_FORMAT; }
    else if (n >= 16 && !memcmp(h, "\x30\x26\xb2\x75\x8e\x66\xcf\x11\xa6\xd9\x00\xaa\x00\x62\xce\x6c", 16)) { c->o->container = CF_K_ASF; c->o->verdict = CF_V_EXCLUDED; c->o->reason = CF_R_FORMAT; }
    else if (n >= 8 && !memcmp(h + 4, "ftyp", 4)) mp4(c);
    else {
        uint8_t iso[5];
        if (c->s->size >= 32774 && rd(c, 32769, iso, 5) && !memcmp(iso, "CD001", 5)) { c->o->container = CF_K_ISO; c->o->verdict = CF_V_EXCLUDED; c->o->reason = CF_R_FORMAT; }
        else if (!c->err) mp3(c, 0);
    }
}
void cast_format_classify(const cast_format_src *s, cast_format_info *o) {
    cf c = {s, o, 0, 0, 0};
    memset(o, 0, sizeof *o);
    o->verdict = CF_V_UNKNOWN; o->reason = CF_R_UNRECOGNIZED;
    if (s->is_cue) { o->verdict = CF_V_EXCLUDED; o->reason = CF_R_CUE; return; }
    if (s->is_iso) { o->container = CF_K_ISO; o->verdict = CF_V_EXCLUDED; o->reason = CF_R_FORMAT; return; }
    if (!s->size) { o->verdict = CF_V_EXCLUDED; o->reason = CF_R_EMPTY; return; }
    sniff(&c);
    if (c.err) { o->verdict = CF_V_UNKNOWN; o->reason = c.err; o->mime = NULL; return; }
    if (o->verdict == CF_V_UNKNOWN && o->codec != CF_C_NONE) judge(o);   /* a known-excluded family already has its verdict; no codec = unrecognised */
}
static int fd_read(void *ctx, uint64_t off, void *b, size_t n) { ssize_t r = pread(*(int *)ctx, b, n, (off_t)off); return r < 0 ? -1 : (int)r; }
void cast_format_classify_fd(int fd, int is_cue, int is_iso, cast_format_info *o) {
    struct stat st; cast_format_src s = {fd_read, &fd, 0, is_cue, is_iso};
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) { memset(o, 0, sizeof *o); o->verdict = CF_V_UNKNOWN; o->reason = CF_R_IO; return; }
    s.size = (uint64_t)st.st_size;
    cast_format_classify(&s, o);
}
int cast_format_profile(const char *model, const cast_format_info *in, int *reason) {
    *reason = in->reason;
    if (in->verdict == CF_V_OK && model && !strcmp(model, "Google Home Mini") && in->codec == CF_C_FLAC && in->rate > 48000) { *reason = CF_R_RECEIVER; return CF_V_EXCLUDED; }   /* R4c: 96/192 kHz FLAC finishes silently */
    return in->verdict;
}
