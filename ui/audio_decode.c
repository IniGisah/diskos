/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _FILE_OFFSET_BITS 64
#include "audio_decode.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#define DR_WAV_LIBSNDFILE_COMPAT
#include "dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#define DR_MP3_FLOAT_OUTPUT
#include "dr_mp3.h"

typedef union { max_align_t align; size_t size; } allocation;
struct audio_decode {
    int fd, opened, stock_fd, stock_owned; pid_t stock_pid;
    int64_t cursor, bytes;
    size_t used, limit;
    int (*cancel)(void *); void *ctx;
    ad_info info; uint64_t position; ad_result error;
    union { drflac *flac; drwav wav; drmp3 mp3; } u;
};
static atomic_int stock_available;
int audio_decode_stock_available(void){ return atomic_load(&stock_available); }
#include "audio_stock_client.h"
static int stopped(audio_decode *d){
    if(d->error == AD_OK && d->cancel && d->cancel(d->ctx)) d->error = AD_CANCELLED;
    return d->error != AD_OK;
}
static void *alloc_mem(size_t n, void *ctx){
    audio_decode *d = ctx;
    if(stopped(d)) return NULL;
    if(n > SIZE_MAX - sizeof(allocation) || n + sizeof(allocation) > d->limit - d->used){ d->error = AD_MEMORY; return NULL; }
    allocation *a = malloc(sizeof *a + n);
    if(!a){ d->error = AD_MEMORY; return NULL; }
    a->size = n + sizeof *a; d->used += a->size;
    return a + 1;
}
static void free_mem(void *p, void *ctx){
    if(p){ allocation *a = (allocation *)p - 1; ((audio_decode *)ctx)->used -= a->size; free(a); }
}
static void *realloc_mem(void *p, size_t n, void *ctx){
    if(!p) return alloc_mem(n, ctx);
    if(!n){ free_mem(p, ctx); return NULL; }
    audio_decode *d = ctx; allocation *a = (allocation *)p - 1;
    if(stopped(d)) return NULL;
    size_t old = a->size;
    if(n > SIZE_MAX - sizeof *a || n + sizeof *a > d->limit - (d->used - old)){ d->error = AD_MEMORY; return NULL; }
    a = realloc(a, n + sizeof *a);
    if(!a){ d->error = AD_MEMORY; return NULL; }
    a->size = n + sizeof *a; d->used = d->used - old + a->size;
    return a + 1;
}
static size_t read_bytes(void *ctx, void *out, size_t n){
    audio_decode *d = ctx; size_t got = 0;
    while(got < n && !stopped(d)){
        size_t want = n - got;
        if(want > 65536) want = 65536;
        ssize_t r = pread(d->fd, (char *)out + got, want, (off_t)d->cursor);
        if(r < 0){ if(errno == EINTR) continue; d->error = AD_IO; break; }
        if(!r) break;
        got += (size_t)r; d->cursor += r;
    }
    return got;
}
static unsigned seek_bytes(audio_decode *d, int offset, int origin){
    if(stopped(d)) return 0;
    int64_t base = origin == 0 ? 0 : origin == 1 ? d->cursor : d->bytes;
    if((offset > 0 && base > INT64_MAX - offset) || base + offset < 0) return 0;
    d->cursor = base + offset; return 1;
}
static drflac_bool32 flac_seek(void *c, int n, drflac_seek_origin o){ return seek_bytes(c, n, o); }
static drwav_bool32 wav_seek(void *c, int n, drwav_seek_origin o){ return seek_bytes(c, n, o); }
static drmp3_bool32 mp3_seek(void *c, int n, drmp3_seek_origin o){ return seek_bytes(c, n, o); }
static drflac_bool32 flac_tell(void *c, drflac_int64 *p){ *p = ((audio_decode *)c)->cursor; return 1; }
static drmp3_bool32 mp3_tell(void *c, drmp3_int64 *p){ *p = ((audio_decode *)c)->cursor; return 1; }

ad_result audio_decode_open(audio_decode **out, int fd, size_t limit, int (*cancel)(void *), void *ctx){
    if(!out) return AD_ARGUMENT;
    *out = NULL;
    if(!limit) limit = AUDIO_DECODE_HEAP_MAX;
    if(limit > AUDIO_DECODE_HEAP_MAX || limit < sizeof(audio_decode)) return AD_MEMORY;
    struct stat st;
    if(fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) return AD_IO;
    audio_decode *d = calloc(1, sizeof *d);
    if(!d) return AD_MEMORY;
    d->stock_fd = -1;
    d->fd = fcntl(fd, F_DUPFD_CLOEXEC, 4); d->bytes = st.st_size;
    d->limit = limit; d->used = sizeof *d; d->cancel = cancel; d->ctx = ctx;
    if(d->fd < 0){ free(d); return AD_IO; }
    unsigned char h[12]; size_t n = read_bytes(d, h, sizeof h); d->cursor = 0;
    d->info.frames = UINT64_MAX;
    if(n >= 4 && !memcmp(h, "fLaC", 4)){
        drflac_allocation_callbacks ac = { d, alloc_mem, realloc_mem, free_mem };
        d->info.codec = AD_FLAC;
        d->u.flac = drflac_open(read_bytes, flac_seek, flac_tell, d, &ac);
        if(d->u.flac){
            d->opened = 1; d->info.rate = d->u.flac->sampleRate; d->info.channels = d->u.flac->channels;
            d->info.bits = d->u.flac->bitsPerSample;
            if(d->u.flac->totalPCMFrameCount) d->info.frames = d->u.flac->totalPCMFrameCount;
        }
    } else if(n == 12 && (!memcmp(h, "RIFF", 4) || !memcmp(h, "RF64", 4)) && !memcmp(h + 8, "WAVE", 4)){
        drwav_allocation_callbacks ac = { d, alloc_mem, realloc_mem, free_mem };
        d->info.codec = AD_WAV;
        /* No onTell: keep the declared length instead of dr_wav's permissive
         * clamp to the physical file size, so an incomplete song is an error. */
        if(drwav_init(&d->u.wav, read_bytes, wav_seek, NULL, d, &ac)){
            d->opened = 1; d->info.rate = d->u.wav.sampleRate; d->info.channels = d->u.wav.channels;
            d->info.bits = d->u.wav.bitsPerSample; d->info.frames = d->u.wav.totalPCMFrameCount;
            if(d->u.wav.translatedFormatTag != DR_WAVE_FORMAT_PCM) d->error = AD_FORMAT;
            if(d->u.wav.dataChunkDataPos > (uint64_t)d->bytes ||
               d->u.wav.dataChunkDataSize > (uint64_t)d->bytes - d->u.wav.dataChunkDataPos) d->error = AD_TRUNCATED;
        }
    } else if(n >= 3 && (!memcmp(h, "ID3", 3) || (h[0] == 0xff && (h[1] & 0xe0) == 0xe0))){
        drmp3_allocation_callbacks ac = { d, alloc_mem, realloc_mem, free_mem };
        d->info.codec = AD_MP3;
        if(drmp3_init(&d->u.mp3, read_bytes, mp3_seek, mp3_tell, NULL, d, &ac)){
            d->opened = 1; d->info.rate = d->u.mp3.sampleRate; d->info.channels = d->u.mp3.channels;
            if(d->u.mp3.totalPCMFrameCount != DRMP3_UINT64_MAX)
                d->info.frames = drmp3_get_pcm_frame_count(&d->u.mp3);
        }
    }
    if(!d->opened && d->error == AD_OK) d->error = stock_open(d);
    if(d->opened && (d->info.channels < 1 || d->info.channels > 2 || d->info.rate < 8000 || d->info.rate > (d->info.codec == AD_STOCK ? AS_RATE_MAX : 384000))) d->error = AD_FORMAT;
    if(d->error != AD_OK){ ad_result r = d->error; audio_decode_close(d); return r; }
    *out = d; return AD_OK;
}
const ad_info *audio_decode_info(const audio_decode *d){ return d ? &d->info : NULL; }
ad_result audio_decode_read(audio_decode *d, float *pcm, size_t capacity, size_t *received){
    if(received) *received = 0;
    if(!d || !received || capacity > AUDIO_DECODE_FRAMES_MAX || (capacity && !pcm)) return AD_ARGUMENT;
    if(stopped(d)) return d->error;
    if(!capacity) return AD_OK;
    if(d->position == d->info.frames) return AD_EOF;
    uint64_t n;
    if(d->info.codec == AD_STOCK){
        ad_result r = stock_read(d, pcm, capacity, received);
        d->position += *received; return r;
    }
    if(d->info.codec == AD_FLAC) n = drflac_read_pcm_frames_f32(d->u.flac, capacity, pcm);
    else if(d->info.codec == AD_WAV) n = drwav_read_pcm_frames_f32(&d->u.wav, capacity, pcm);
    else n = drmp3_read_pcm_frames_f32(&d->u.mp3, capacity, pcm);
    d->position += n; *received = (size_t)n;
    if(!n && d->info.frames != UINT64_MAX && d->position < d->info.frames && d->error == AD_OK) d->error = AD_TRUNCATED;
    if(stopped(d)) return d->error;
    return n ? AD_OK : AD_EOF;
}
ad_result audio_decode_seek(audio_decode *d, uint64_t frame){
    if(!d || (d->info.frames != UINT64_MAX && frame > d->info.frames)) return AD_ARGUMENT;
    if(stopped(d)) return d->error;
    if(frame == d->info.frames){ d->position = frame; return AD_OK; }
    if(d->info.codec == AD_STOCK){
        size_t n = 0; ad_result r = stock_request(d, AS_SEEK, 0, frame, &n, stock_now()+120000);
        if(r == AD_OK) d->position = frame;
        return r;
    }
    int ok;
    if(d->info.codec == AD_FLAC) ok = drflac_seek_to_pcm_frame(d->u.flac, frame);
    else if(d->info.codec == AD_WAV) ok = drwav_seek_to_pcm_frame(&d->u.wav, frame);
    else ok = drmp3_seek_to_pcm_frame(&d->u.mp3, frame);
    if(!ok && d->error == AD_OK) d->error = AD_DECODE;
    if(stopped(d)) return d->error;
    d->position = frame; return AD_OK;
}
uint64_t audio_decode_position(const audio_decode *d){ return d ? d->position : 0; }
size_t audio_decode_heap(const audio_decode *d){ return d ? d->used : 0; }
void audio_decode_close(audio_decode *d){
    if(!d) return;
    stock_close(d);
    if(d->opened && d->info.codec != AD_STOCK){
        if(d->info.codec == AD_FLAC) drflac_close(d->u.flac);
        else if(d->info.codec == AD_WAV) drwav_uninit(&d->u.wav);
        else drmp3_uninit(&d->u.mp3);
    }
    close(d->fd); free(d);
}
