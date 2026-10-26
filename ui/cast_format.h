/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_CAST_FORMAT_H
#define DISKOS_CAST_FORMAT_H
#include <stddef.h>
#include <stdint.h>
/* Cast format classifier: which file is this (by its BYTES, never its extension), may it be cast, and with what MIME type.
 * Pure and host-testable: no allocation, no globals, no threads. A whole classification reads at most CF_BUDGET bytes in at
 * most CF_MAX_READS reads, however large the file; every length field is range-checked against the file size and the
 * budget, so a hostile header can only end in an UNKNOWN verdict.
 * Unrecognised formats are conservatively excluded. */
#define CF_BUDGET 65536
#define CF_MAX_READS 256
#define CF_ID(x) x,
#define CF_STR(x) #x,
#define CF_CONTAINERS(X) X(CF_K_NONE) X(CF_K_MPEG) X(CF_K_MP4) X(CF_K_FLAC) X(CF_K_OGG) X(CF_K_WAV) X(CF_K_AIFF) X(CF_K_ASF) X(CF_K_APE) X(CF_K_DSD) X(CF_K_WAVPACK) X(CF_K_ISO)
#define CF_CODECS(X) X(CF_C_NONE) X(CF_C_MP3) X(CF_C_AAC) X(CF_C_ALAC) X(CF_C_FLAC) X(CF_C_OPUS) X(CF_C_VORBIS) X(CF_C_PCM) X(CF_C_OTHER)
#define CF_VERDICTS(X) X(CF_V_OK) X(CF_V_EXCLUDED) X(CF_V_UNKNOWN)   /* EXCLUDED: known, do not offer. UNKNOWN: could not tell, inspect again before LOAD. */
#define CF_REASONS(X) X(CF_R_NONE) X(CF_R_IO) X(CF_R_MALFORMED) X(CF_R_BUDGET) X(CF_R_EMPTY) X(CF_R_UNRECOGNIZED) X(CF_R_CUE) \
    X(CF_R_FORMAT) /* known excluded family: ALAC, AIFF, WMA, APE, DSD, WavPack, SACD ISO */ \
    X(CF_R_UNTESTED) /* parsed, but not a combination v1 offers (no evidence it plays) */ \
    X(CF_R_RATE_UNKNOWN) X(CF_R_RATE_TOO_HIGH) /* above 96 kHz */ X(CF_R_RECEIVER) /* the receiver profile excludes it */
enum { CF_CONTAINERS(CF_ID) CF_K_COUNT };
enum { CF_CODECS(CF_ID) CF_C_COUNT };
enum { CF_VERDICTS(CF_ID) CF_V_COUNT };
enum { CF_REASONS(CF_ID) CF_R_COUNT };
/* Reads n bytes at off into buf; returns n, anything else is an I/O failure. Never asked for bytes past `size`. */
typedef int (*cast_format_read_fn)(void *ctx, uint64_t off, void *buf, size_t n);
typedef struct {
    cast_format_read_fn read; void *ctx; uint64_t size;
    int is_cue, is_iso;   /* the library's rows (SONG.IS_CUE / IS_ISO, musicdb.h mdb_song_subtrack): virtual tracks are never offered */
} cast_format_src;
typedef struct {
    int container, codec, verdict, reason;
    uint32_t rate;        /* Hz as decoded (Opus: 48000); 0 = not in the header */
    int bits, channels;   /* bits: PCM and FLAC only */
    const char *mime;     /* contentType for the Cast LOAD (set once the codec is known; offer the file only on verdict OK) */
} cast_format_info;
void cast_format_classify(const cast_format_src *s, cast_format_info *out);
/* fd must be a regular file (fstat'd, then pread); anything else is UNKNOWN/IO and is never read. */
void cast_format_classify_fd(int fd, int is_cue, int is_iso, cast_format_info *out);
/* Receiver profile: `model` is the mDNS md= value (NULL = unknown receiver, no extra rule). Returns the verdict for this file
 * on that receiver and sets *reason. Only a receiver with evidence gets a rule: "Google Home Mini" excludes FLAC above 48 kHz. */
int cast_format_profile(const char *model, const cast_format_info *in, int *reason);
#endif
