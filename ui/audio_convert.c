/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_convert.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "speex_resampler.h"
#define BLOCK 1024u
struct audio_convert {
    audio_decode *decoder;
    SpeexResamplerState *resampler;
    unsigned rate, channels, input_rate, bits;
    int dither, eof;
    ad_result error;
    uint64_t rng, input_total, output_total;
    size_t count, at;
    float input[BLOCK * 2];
};
static float uniform(audio_convert *c){
    uint64_t x = c->rng;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27; c->rng = x;
    return (float)((x * UINT64_C(2685821657736338717)) >> 40) / 16777216.0f;
}
static int32_t quantize(audio_convert *c, float sample){
    float scale = c->bits == 24 ? 8388608.0f : 32768.0f;
    float x = sample * scale;
    if(c->dither){ float a = uniform(c), b = uniform(c); x += a - b; }
    if(x >= scale - 1) return (int32_t)(scale - 1);
    if(x <= -scale) return (int32_t)-scale;
    return (int32_t)floorf(x + 0.5f);
}
static uint64_t target(const audio_convert *c){
    /* One rational rounding for the entire segment, never per input chunk. */
    uint64_t q = c->input_total / c->input_rate, r = c->input_total % c->input_rate;
    return q * c->rate + (r * c->rate + c->input_rate - 1) / c->input_rate;
}
ad_result audio_convert_open_pcm(audio_convert **out, audio_decode *decoder, unsigned rate, unsigned bits, uint64_t seed){
    if(!out) return AD_ARGUMENT;
    *out = NULL;
    const ad_info *info = audio_decode_info(decoder);
    if(!info || (rate != 44100 && rate != 48000) || (bits != 16 && bits != 24)) return AD_ARGUMENT;
    audio_convert *c = calloc(1, sizeof *c);
    if(!c) return AD_MEMORY;
    c->decoder = decoder; c->rate = rate; c->bits = bits; c->input_rate = info->rate; c->channels = info->channels;
    c->dither = info->bits == 0 || info->bits > bits || info->rate != rate;
    c->rng = seed ? seed : UINT64_C(0x9e3779b97f4a7c15);
    if(info->rate != rate){
        int err;
        c->resampler = speex_resampler_init(info->channels, info->rate, rate, 8, &err);
        if(!c->resampler){ free(c); return AD_MEMORY; }
        if(speex_resampler_skip_zeros(c->resampler) != RESAMPLER_ERR_SUCCESS){ audio_convert_close(c); return AD_DECODE; }
    }
    *out = c; return AD_OK;
}
static ad_result read_pcm(audio_convert *c, void *stereo, size_t capacity, size_t *received, unsigned bits){
    if(received) *received = 0;
    if(!c || c->bits != bits || !received || capacity > AUDIO_DECODE_FRAMES_MAX || (capacity && !stereo)) return AD_ARGUMENT;
    if(c->error != AD_OK) return c->error;
    size_t ignored;
    c->error = audio_decode_read(c->decoder, NULL, 0, &ignored);  /* cancellation also applies to buffered output */
    if(c->error != AD_OK) return c->error;
    while(*received < capacity){
        if(c->eof && c->output_total == target(c)) return *received ? AD_OK : AD_EOF;
        if(c->at == c->count){
            c->at = 0;
            if(!c->eof){
                ad_result r = audio_decode_read(c->decoder, c->input, BLOCK, &c->count);
                if(r != AD_OK && r != AD_EOF){ c->error = r; return r; }
                c->input_total += c->count;
                if(r == AD_EOF) c->eof = 1;
            }
            if(c->eof){
                if(c->output_total == target(c)) return *received ? AD_OK : AD_EOF;
                if(!c->resampler) return *received ? AD_OK : AD_EOF;
                memset(c->input, 0, sizeof c->input); c->count = BLOCK;   /* drain filter delay, trimmed at target */
            }
        }
        float pcm[BLOCK * 2];
        spx_uint32_t in = (spx_uint32_t)(c->count - c->at);
        spx_uint32_t out = (spx_uint32_t)(capacity - *received);
        if(out > BLOCK) out = BLOCK;
        if(c->eof && out > target(c) - c->output_total) out = (spx_uint32_t)(target(c) - c->output_total);
        if(c->resampler){
            int rc = speex_resampler_process_interleaved_float(c->resampler, c->input + c->at * c->channels, &in, pcm, &out);
            if(rc != RESAMPLER_ERR_SUCCESS || (!in && !out)){ c->error = AD_DECODE; return c->error; }
        } else {
            if(out > in) out = in;
            in = out; memcpy(pcm, c->input + c->at * c->channels, out * c->channels * sizeof(float));
        }
        c->at += in;
        for(unsigned i = 0; i < out; i++){
            float left = pcm[i * c->channels], right = pcm[i * c->channels + c->channels - 1];
            if(!isfinite(left) || !isfinite(right)){ c->error = AD_DECODE; return c->error; }
            int32_t l = quantize(c, left), r = c->channels == 1 ? l : quantize(c, right);
            if(bits == 24){ int32_t *p = stereo; p[(*received + i) * 2] = l; p[(*received + i) * 2 + 1] = r; }
            else { int16_t *p = stereo; p[(*received + i) * 2] = (int16_t)l; p[(*received + i) * 2 + 1] = (int16_t)r; }
        }
        *received += out; c->output_total += out;
    }
    return AD_OK;
}
ad_result audio_convert_open(audio_convert **out, audio_decode *d, unsigned rate, uint64_t seed){
    return audio_convert_open_pcm(out, d, rate, 16, seed);
}
ad_result audio_convert_read(audio_convert *c, int16_t *out, size_t cap, size_t *got){ return read_pcm(c, out, cap, got, 16); }
ad_result audio_convert_read24(audio_convert *c, int32_t *out, size_t cap, size_t *got){ return read_pcm(c, out, cap, got, 24); }
ad_result audio_convert_seek(audio_convert *c, uint64_t frame){
    if(!c) return AD_ARGUMENT;
    if(c->error != AD_OK) return c->error;
    c->error = audio_decode_seek(c->decoder, frame);
    if(c->error != AD_OK) return c->error;
    c->at = c->count = 0; c->input_total = c->output_total = 0; c->eof = 0;
    if(c->resampler && (speex_resampler_reset_mem(c->resampler) != RESAMPLER_ERR_SUCCESS ||
                       speex_resampler_skip_zeros(c->resampler) != RESAMPLER_ERR_SUCCESS)) c->error = AD_DECODE;
    return c->error;
}
uint64_t audio_convert_frames(const audio_convert *c){ return c ? c->output_total : 0; }
void audio_convert_close(audio_convert *c){
    if(c){ if(c->resampler) speex_resampler_destroy(c->resampler); free(c); }
}
