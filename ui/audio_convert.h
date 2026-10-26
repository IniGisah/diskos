/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AUDIO_CONVERT_H
#define DISKOS_AUDIO_CONVERT_H
#include "audio_decode.h"
typedef struct audio_convert audio_convert;
/* Worker-only pull conversion: borrowed decoder -> stereo signed 16-bit PCM at
 * 44100 or 48000 Hz. No EQ, gain or receiver-volume writes. Close converter before
 * decoder. Caller seed controls TPDF dither reproducibly; it is not a secret.
 * Matching-rate integer <=16-bit input bypasses both resampling and dither.
 * Conversion uses the pinned SpeexDSP float resampler, quality 8. */
/* Precision-aware entry: 16 or 24 bits. The 24-bit API returns signed int32
 * samples in [-8388608,8388607], with no left shift or gain. */
ad_result audio_convert_open_pcm(audio_convert **out, audio_decode *, unsigned rate, unsigned bits, uint64_t seed);
ad_result audio_convert_read24(audio_convert *, int32_t *stereo, size_t capacity, size_t *received);
ad_result audio_convert_open(audio_convert **out, audio_decode *decoder, unsigned rate, uint64_t seed);
ad_result audio_convert_read(audio_convert *c, int16_t *stereo, size_t capacity, size_t *received);
/* Cancels all buffered old audio, seeks in SOURCE frames and restarts the filter.
 * Failure invalidates the converter. Output counts restart from zero on success. */
ad_result audio_convert_seek(audio_convert *c, uint64_t source_frame);
uint64_t audio_convert_frames(const audio_convert *c);
void audio_convert_close(audio_convert *c);
#endif
