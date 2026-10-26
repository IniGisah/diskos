/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_rtp.h"
#include "airplay_crypto.h"
#include "airplay_raop.h"
#include <string.h>
static void be32(uint8_t *b, uint32_t n){ for(int i = 0; i < 4; i++) b[i] = (uint8_t)(n >> (24 - i * 8)); }
static void be64(uint8_t *b, uint64_t n){ for(int i = 0; i < 8; i++) b[i] = (uint8_t)(n >> (56 - i * 8)); }
static void put_bits(uint8_t *b, size_t *at, uint32_t v, unsigned n){
    while(n){ --n; b[*at / 8] |= (uint8_t)(((v >> n) & 1u) << (7 - *at % 8)); ++*at; }
}
int aps_alac_bits(const void *pcm, size_t frames, unsigned bits, uint8_t *out, size_t cap){
    size_t bytes = bits == 24 ? 6 : 4;
    if((bits != 16 && bits != 24) || !pcm || !frames || frames > APS_PACKET_FRAMES || cap < frames * bytes + 8) return -1;
    if(bits == 24) for(size_t i = 0; i < frames * 2; i++)
        if(((const int32_t *)pcm)[i] < -8388608 || ((const int32_t *)pcm)[i] > 8388607) return -1;
    memset(out, 0, frames * bytes + 8); size_t at = 0;
    put_bits(out, &at, 1, 3);  /* channel-pair element */
    put_bits(out, &at, 0, 4);  /* element instance */
    put_bits(out, &at, 0, 12); /* unused */
    put_bits(out, &at, 9, 4);  /* explicit length, uncompressed, no low-byte shift */
    put_bits(out, &at, (uint32_t)frames, 32);
    for(size_t i = 0; i < frames * 2; i++){
        uint32_t sample = bits == 24 ? (uint32_t)((const int32_t *)pcm)[i] : (uint16_t)((const int16_t *)pcm)[i];
        put_bits(out, &at, sample, bits);
    }
    put_bits(out, &at, 7, 3);  /* end element, followed by zero alignment */
    return (int)((at + 7) / 8);
}
int aps_alac(const int16_t *pcm, size_t n, uint8_t *out, size_t cap){ return aps_alac_bits(pcm, n, 16, out, cap); }
int aps_rtp_begin(aps_rtp *r, int secure, const uint8_t key[32]){
    uint8_t seed[10]; if(aps_random(seed, sizeof seed)) return -1;
    memset(r, 0, sizeof *r); r->seq = (uint16_t)(seed[0] | (unsigned)seed[1] << 8);
    memcpy(&r->timestamp, seed + 2, 4); memcpy(&r->ssrc, seed + 6, 4);
    r->secure = secure; r->first = 1; if(secure) memcpy(r->key, key, 32);
    return 0;
}
int aps_rtp_encode_bits(aps_rtp *r, const void *pcm, size_t frames, unsigned bits, aps_packet *p){
    uint8_t encoded[APS_PACKET_MAX], nonce[12];
    if(r->counter == UINT64_MAX) return -1;
    int n = aps_alac_bits(pcm, frames, bits, encoded, sizeof encoded); if(n < 0) return -1;
    p->seq = r->seq; p->serial = r->counter;
    p->bytes[0] = APS_RTP_V2; p->bytes[1] = r->first ? APS_RTP_AUDIO_FIRST : APS_RTP_AUDIO;
    p->bytes[2] = (uint8_t)(r->seq >> 8); p->bytes[3] = (uint8_t)r->seq;
    be32(p->bytes + 4, r->timestamp); be32(p->bytes + 8, r->ssrc);
    if(r->secure == 1){
        aps_nonce(nonce, r->counter);
        if(aps_seal(r->key, nonce, p->bytes + 4, 8, encoded, (size_t)n, p->bytes + 12, p->bytes + 12 + n)) return -1;
        memcpy(p->bytes + 28 + n, nonce + 4, 8); p->n = (uint16_t)(n + 36);
    } else {
        if(r->secure == 2){ if(aps_raop_encrypt(r->key, r->iv, encoded, (size_t)n, p->bytes + 12)) return -1; }
        else memcpy(p->bytes + 12, encoded, (size_t)n);
        p->n = (uint16_t)(n + 12);
    }
    ++r->counter; ++r->seq; r->timestamp += (uint32_t)frames; r->first = 0; return 0;
}
int aps_rtp_encode(aps_rtp *r, const int16_t *pcm, size_t n, aps_packet *p){ return aps_rtp_encode_bits(r, pcm, n, 16, p); }
void aps_rtp_retain(aps_rtp *r, const aps_packet *p){ r->history[p->seq % APS_RETAIN] = *p; }
const aps_packet *aps_rtp_find(const aps_rtp *r, uint16_t seq){
    const aps_packet *p = &r->history[seq % APS_RETAIN];
    return p->n && p->seq == seq && p->serial < r->counter && r->counter - p->serial <= APS_RETAIN ? p : NULL;
}
int aps_retransmit_request(const uint8_t *b, size_t n, uint16_t *request, uint16_t *seq, unsigned *count){
    if(n != 8 || b[0] != 0x80 || (b[1] & 0x7f) != 0x55) return -1;
    *request = (uint16_t)((unsigned)b[2] << 8 | b[3]);
    *seq = (uint16_t)((unsigned)b[4] << 8 | b[5]); *count = (unsigned)b[6] << 8 | b[7];
    return !*count || *count > 32 ? -1 : 0;
}
uint64_t aps_ntp(uint64_t ns){
    uint64_t sec = ns / UINT64_C(1000000000), rem = ns % UINT64_C(1000000000);
    return ((sec + UINT64_C(2208988800)) << 32) | ((rem << 32) / UINT64_C(1000000000));
}
int aps_timing_reply(const uint8_t *b, size_t n, uint64_t receive, uint64_t transmit, uint8_t out[32]){
    if(n != 32 || b[0] != 0x80 || (b[1] & 0x7f) != 0x52 || transmit < receive) return -1;
    memset(out, 0, 32); out[0] = APS_RTP_V2; out[1] = APS_RTP_TIMING_REPLY; out[2] = b[2]; out[3] = b[3];
    memcpy(out + 8, b + 24, 8); be64(out + 16, receive); be64(out + 24, transmit); return 0;
}
void aps_sync(uint32_t audible, uint32_t submitted, uint64_t ntp, int first, uint8_t out[20]){
    memset(out, 0, 20); out[0] = first ? APS_RTP_FIRST : APS_RTP_V2; out[1] = APS_RTP_SYNC; out[3] = 7;
    be32(out + 4, audible); be64(out + 8, ntp); be32(out + 16, submitted);
}
