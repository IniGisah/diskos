/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_RTP_H
#define DISKOS_AIRPLAY_RTP_H
#include <stddef.h>
#include <stdint.h>
#define APS_PACKET_MAX 1500
#define APS_PACKET_FRAMES 352
#define APS_RETAIN 256
enum { APS_RTP_V2 = 0x80, APS_RTP_FIRST = 0x90, APS_RTP_AUDIO = 0x60,
       APS_RTP_AUDIO_FIRST = 0xe0, APS_RTP_TIMING_REPLY = 0xd3,
       APS_RTP_SYNC = 0xd4, APS_RTP_RESEND = 0xd6 };
typedef struct { uint16_t seq; uint16_t n; uint64_t serial; uint8_t bytes[APS_PACKET_MAX]; } aps_packet;
typedef struct {
    uint16_t seq; uint32_t timestamp, ssrc; uint64_t counter;
    int secure, first; uint8_t key[32], iv[16];
    aps_packet history[APS_RETAIN];
} aps_rtp;
/* Escape-frame ALAC, no compression or predictor: bit-exact s16 stereo.
 * Apple ALAC bitstream description; authored here, no proof source linked.
 * Explicit sample count preserves a partial final packet without zero padding. */
int aps_alac_bits(const void *, size_t frames, unsigned bits, uint8_t *, size_t cap);
int aps_alac(const int16_t *, size_t frames, uint8_t *, size_t cap);
int aps_rtp_begin(aps_rtp *, int secure, const uint8_t key[32]);
/* Encoder takes ownership of one nonce/sequence; caller retains the identical
 * encoded packet on retry. Flush NEVER resets counters. */
int aps_rtp_encode_bits(aps_rtp *, const void *, size_t frames, unsigned bits, aps_packet *);
int aps_rtp_encode(aps_rtp *, const int16_t *, size_t frames, aps_packet *);
void aps_rtp_retain(aps_rtp *, const aps_packet *);
const aps_packet *aps_rtp_find(const aps_rtp *, uint16_t seq);
int aps_retransmit_request(const uint8_t *, size_t, uint16_t *request, uint16_t *seq, unsigned *count);
uint64_t aps_ntp(uint64_t unix_ns);
int aps_timing_reply(const uint8_t *, size_t, uint64_t receive, uint64_t transmit, uint8_t out[32]);
void aps_sync(uint32_t audible, uint32_t submitted, uint64_t ntp, int first, uint8_t out[20]);
#endif
