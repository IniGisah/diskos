/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AUDIO_DECODE_H
#define DISKOS_AUDIO_DECODE_H
#include <stddef.h>
#include <stdint.h>
/* Worker-thread decoder for an approved regular-file descriptor. No path lookup,
 * player commands, LVGL, networking, resampling or output gain. The caller holds
 * its SD lease until audio_decode_close (and until its worker is reaped).
 * One instance belongs to one thread. cancel may read an atomic flag. */
#define AUDIO_DECODE_HEAP_MAX (8u * 1024u * 1024u)
#define AUDIO_DECODE_FRAMES_MAX 4096u
/* AD_UNAVAILABLE: the embedded stock helper could not be prepared this time
 * (private copy or its verification failed). Retryable; never a format verdict. */
typedef enum { AD_OK, AD_EOF, AD_FORMAT, AD_IO, AD_MEMORY, AD_CANCELLED, AD_TRUNCATED, AD_ARGUMENT, AD_DECODE, AD_UNAVAILABLE } ad_result;
typedef enum { AD_FLAC, AD_WAV, AD_MP3, AD_STOCK } ad_codec;
typedef struct {
    ad_codec codec;
    unsigned rate, channels, bits;       /* source precision; MP3 float decode uses bits=0 */
    uint64_t frames;                     /* UINT64_MAX if not available without a full scan */
} ad_info;
typedef struct audio_decode audio_decode;
/* True after a helper has loaded and passed its stock-library runtime gate. */
int audio_decode_stock_available(void);
/* Private worker channel: the sender creates and reaps every stock helper. */
void audio_decode_stock_supervise(int fd, uint32_t generation);
/* Copies the descriptor, never changes the caller's file offset, and closes only
 * its own copy. limit includes instance + decoder allocations; 0 selects MAX.
 * Native FLAC, integer PCM WAV and MP3; mono/stereo, 8..384 kHz. No extension guess.
 * Other formats use the isolated stock-library helper embedded in this program
 * (a verified private copy, see audio_stock_process.h) if its runtime gate passes.
 * Stock DSD may produce up to 1411200 Hz PCM before SpeexDSP conversion.
 * The native allocation limit excludes that process (separate 96 MiB limit).
 * MP3 uses the pinned decoder's delay/padding handling; no full-file count scan. */
ad_result audio_decode_open(audio_decode **out, int fd, size_t limit, int (*cancel)(void *), void *ctx);
const ad_info *audio_decode_info(const audio_decode *d);
/* Interleaved source-channel float PCM, no padding or mono duplication. 16/24-bit
 * integer sources are exact in float. Destination has capacity*channels floats.
 * received is set even on an error: an incomplete file is never a normal EOF.
 * capacity=0 is a no-op. Errors other than EOF/ARGUMENT require close/reopen. */
ad_result audio_decode_read(audio_decode *d, float *pcm, size_t capacity, size_t *received);
ad_result audio_decode_seek(audio_decode *d, uint64_t frame);
uint64_t audio_decode_position(const audio_decode *d);
size_t audio_decode_heap(const audio_decode *d);   /* live instance + decoder allocation bytes */
void audio_decode_close(audio_decode *d);
#endif
