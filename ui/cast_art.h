/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_CAST_ART_H
#define DISKOS_CAST_ART_H
#include <stddef.h>
/* Cast cover art (slice 20): the cached cover of the beamed track as a PNG for the receiver. The cover cache (artcache.c) holds one
 * 148 x 148, 24-bit BMP per track; nothing is decoded here, the BMP is only validated and re-encoded with LodePNG (LVGL's copy, encoder only).
 * Everything is bounded: the file must be exactly that BMP, the encoder may not allocate more than CA_ALLOC_MAX at a time and the PNG may
 * not exceed CA_PNG_MAX. HTTP waits up to 15 seconds for a cache publication before answering 404; the LOAD is never delayed.
 * Generation: cast_art_arm() names the track whose cover may be served and returns its generation; arming another track (or clearing)
 * invalidates every earlier generation, so a cover for an earlier track is never served for a later one. */
#define CA_PX 148            /* the cached cover is this many pixels square */
#ifndef CA_PNG_MAX
#define CA_PNG_MAX (256 * 1024)
#endif
#ifndef CA_ALLOC_MAX
#define CA_ALLOC_MAX (1024 * 1024)
#endif
#ifndef CA_JPEG_MAX
#define CA_JPEG_MAX (256 * 1024)
#endif
#if CA_JPEG_MAX < 1 || CA_JPEG_MAX > 256 * 1024
#error "CA_JPEG_MAX must be between 1 and 256 KiB"
#endif
/* AirPlay: JPEG quality 85, falling back to PNG from the same RGB buffer.
 * Same ownership/descriptor contract as cast_art_png_fd; no realloc for JPEG. */
int cast_art_airplay_fd(int fd, unsigned char **data, size_t *len);
/* Validate the cover BMP at path and encode it. 0: *png is a malloc'd PNG of *len bytes (the caller frees it). -1: no image, nothing allocated. Any thread. */
/* Approved descriptor; caller owns its SD lease. Offset is preserved. */
int cast_art_empty_png(unsigned char **png, size_t *len);
int cast_art_png_fd(int fd, unsigned char **png, size_t *len);
int cast_art_png(const char *bmp, unsigned char **png, size_t *len);
/* LVGL thread, when a LOAD is built: remember the track (an absolute path) and return its generation. No cache I/O here: whether a cover
 * exists is found out only when the receiver fetches it; HTTP may wait for a late cache publication without delaying the LOAD. */
unsigned cast_art_arm(const char *track);
/* The HTTP worker, when the receiver asks for the image: the PNG of the armed track if gen is still the current generation (re-checked after the
 * encode), else -1. 0 ok (caller frees *png). */
int cast_art_fetch(unsigned gen, unsigned char **png, size_t *len);
/* Forget the track; every generation becomes stale. */
void cast_art_clear(void);
#endif
