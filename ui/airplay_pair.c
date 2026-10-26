/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent persistent HAP controller. No transient or unauthenticated
 * fallback. TLV state order and every received proof/signature are mandatory. */
#include "airplay_pair.h"
#include "airplay_crypto.h"
#include <string.h>
#define TRY(x) do { if((x) != 0) goto end; } while(0)
static int nonce_seal(const uint8_t key[32], const char label[8], const aps_tlv *in, aps_tlv *out){
    uint8_t nonce[12] = {0}; memcpy(nonce + 4, label, 8);
    if(in->n > APS_TLV_MAX - 16) return -1;
    int r = aps_seal(key, nonce, NULL, 0, in->b, in->n, out->b, out->b + in->n);
    out->n = r ? 0 : in->n + 16; return r;
}
static int nonce_open(const uint8_t key[32], const char label[8], const aps_tlv *in, aps_tlv *out){
    uint8_t nonce[12] = {0}; memcpy(nonce + 4, label, 8);
    if(in->n <= 16) return -1;
    int r = aps_open(key, nonce, NULL, 0, in->b, in->n - 16, in->b + in->n - 16, out->b);
    out->n = r ? 0 : in->n - 16; return r;
}
static int take(const aps_tlv *t, unsigned tag, aps_tlv *out){
    int n = aps_tlv_get(t->b, t->n, tag, out->b, sizeof out->b);
    if(n <= 0) return -1;
    out->n = (size_t)n; return 0;
}
static int identifier(const aps_tlv *t, char id[65]){
    int n = aps_tlv_get(t->b, t->n, 1, id, 64);
    if(n <= 0) return -1;
    for(int i = 0; i < n; i++) if((unsigned char)id[i] < 0x21 || (unsigned char)id[i] > 0x7e) return -1;
    id[n] = 0; return n;
}
int aps_pair_peer(const aps_identity *k, const uint8_t peer[32], const uint8_t our[32], const aps_tlv *t){
    char id[65]; uint8_t sig[64], msg[128];
    int n = identifier(t, id);
    if(n < 0 || strcmp(id, k->peer_id) || aps_tlv_get(t->b, t->n, 10, sig, sizeof sig) != 64) return -1;
    memcpy(msg, peer, 32); memcpy(msg + 32, id, (size_t)n); memcpy(msg + 32 + n, our, 32);
    int rc = aps_check(k->peer_key, msg, 64 + (size_t)n, sig);
    aps_wipe(msg, sizeof msg); return rc;
}
int aps_pair_setup(const aps_pair_io *io, aps_identity *out){
    aps_identity k = {0}; aps_tlv tx = {0}, rx = {0}, sub = {0}, enc = {0};
    uint8_t salt[16], B[384], a[32], A[384], m[64], expected[64], K[64];
    uint8_t encrypt[32], x[32], pk[32], sig[64], msg[128]; char pin[9] = {0};
    int rc = -1;
    TRY(aps_identity_new(&k)); TRY(aps_random(a, sizeof a));
    TRY(io->post(io->ctx, "/pair-pin-start", &tx, &rx));
    TRY(aps_tlv_byte(&tx, 6, 1)); TRY(aps_tlv_byte(&tx, 0, 0));
    TRY(io->post(io->ctx, "/pair-setup", &tx, &rx)); TRY(aps_tlv_state(&rx, 2));
    int bn = aps_tlv_get(rx.b, rx.n, 3, B, sizeof B);
    if(aps_tlv_get(rx.b, rx.n, 2, salt, sizeof salt) != 16 || bn <= 0) goto end;
    if(bn < 384){ memmove(B + 384 - bn, B, (size_t)bn); memset(B, 0, 384 - (size_t)bn); }
    TRY(io->pin(io->ctx, pin)); TRY(aps_srp(pin, salt, B, a, A, m, expected, K));
    aps_wipe(pin, sizeof pin);
    tx.n = 0; TRY(aps_tlv_byte(&tx, 6, 3)); TRY(aps_tlv_put(&tx, 3, A, sizeof A)); TRY(aps_tlv_put(&tx, 4, m, sizeof m));
    TRY(io->post(io->ctx, "/pair-setup", &tx, &rx));
    uint8_t error;
    if(aps_tlv_get(rx.b, rx.n, 7, &error, 1) == 1 && error == 2){ rc = APS_PAIR_AUTH; goto end; }
    TRY(aps_tlv_state(&rx, 4));
    if(aps_tlv_get(rx.b, rx.n, 4, m, sizeof m) != 64 || !aps_equal(m, expected, 64)){ rc = APS_PAIR_AUTH; goto end; }
    TRY(aps_kdf(K, 64, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", encrypt));
    TRY(aps_kdf(K, 64, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info", x));
    aps_sign_public(k.seed, pk);
    memcpy(msg, x, 32); memcpy(msg + 32, k.client_id, 36); memcpy(msg + 68, pk, 32);
    aps_sign(k.seed, msg, 100, sig);
    TRY(aps_tlv_put(&sub, 1, k.client_id, 36)); TRY(aps_tlv_put(&sub, 3, pk, 32)); TRY(aps_tlv_put(&sub, 10, sig, 64));
    TRY(nonce_seal(encrypt, "PS-Msg05", &sub, &enc));
    tx.n = 0; TRY(aps_tlv_byte(&tx, 6, 5)); TRY(aps_tlv_put(&tx, 5, enc.b, enc.n));
    TRY(io->post(io->ctx, "/pair-setup", &tx, &rx)); TRY(aps_tlv_state(&rx, 6));
    TRY(take(&rx, 5, &enc)); TRY(nonce_open(encrypt, "PS-Msg06", &enc, &sub));
    int n = identifier(&sub, k.peer_id);
    if(n < 0 || aps_tlv_get(sub.b, sub.n, 3, k.peer_key, 32) != 32 ||
       aps_tlv_get(sub.b, sub.n, 10, sig, 64) != 64) goto end;
    TRY(aps_kdf(K, 64, "Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info", x));
    memcpy(msg, x, 32); memcpy(msg + 32, k.peer_id, (size_t)n); memcpy(msg + 32 + n, k.peer_key, 32);
    if(aps_check(k.peer_key, msg, 64 + (size_t)n, sig)){ rc = APS_PAIR_SIGNATURE; goto end; }
    *out = k; rc = 0;
end:
    if(rc) aps_wipe(out, sizeof *out);
    aps_wipe(&k, sizeof k); aps_wipe(&tx, sizeof tx); aps_wipe(&rx, sizeof rx);
    aps_wipe(&sub, sizeof sub); aps_wipe(&enc, sizeof enc); aps_wipe(pin, sizeof pin);
    aps_wipe(a, sizeof a); aps_wipe(K, sizeof K); aps_wipe(m, sizeof m); aps_wipe(expected, sizeof expected);
    aps_wipe(encrypt, sizeof encrypt); aps_wipe(x, sizeof x); aps_wipe(msg, sizeof msg);
    return rc;
}
int aps_pair_verify(const aps_pair_io *io, const aps_identity *k, uint8_t write_key[32], uint8_t read_key[32], uint8_t audio_key[32]){
    uint8_t secret[32], ours[32], peer[32], shared[32], encrypt[32], sig[64], msg[128];
    aps_tlv tx = {0}, rx = {0}, sub = {0}, enc = {0}; int rc = -1;
    TRY(aps_random(secret, 32)); aps_public(secret, ours);
    TRY(aps_tlv_byte(&tx, 6, 1)); TRY(aps_tlv_put(&tx, 3, ours, 32));
    TRY(io->post(io->ctx, "/pair-verify", &tx, &rx));
    uint8_t error;
    if(aps_tlv_get(rx.b, rx.n, 7, &error, 1) == 1 && error == 2){ rc = APS_PAIR_AUTH; goto end; }
    TRY(aps_tlv_state(&rx, 2));
    if(aps_tlv_get(rx.b, rx.n, 3, peer, 32) != 32) goto end;
    TRY(aps_exchange(secret, peer, shared));
    TRY(aps_kdf(shared, 32, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", encrypt));
    TRY(take(&rx, 5, &enc)); TRY(nonce_open(encrypt, "PV-Msg02", &enc, &sub));
    /* This gate is deliberately fatal. No client signature or session key may
     * leave the process after an invalid receiver signature or identity. */
    TRY(aps_pair_peer(k, peer, ours, &sub));
    size_t n = strnlen(k->client_id, 37); if(n != 36) goto end;
    memcpy(msg, ours, 32); memcpy(msg + 32, k->client_id, n); memcpy(msg + 32 + n, peer, 32);
    aps_sign(k->seed, msg, n + 64, sig); sub.n = 0;
    TRY(aps_tlv_put(&sub, 1, k->client_id, n)); TRY(aps_tlv_put(&sub, 10, sig, 64));
    TRY(nonce_seal(encrypt, "PV-Msg03", &sub, &enc));
    tx.n = 0; TRY(aps_tlv_byte(&tx, 6, 3)); TRY(aps_tlv_put(&tx, 5, enc.b, enc.n));
    TRY(io->post(io->ctx, "/pair-verify", &tx, &rx));
    if(aps_tlv_get(rx.b, rx.n, 7, &error, 1) == 1 && error == 2){ rc = APS_PAIR_AUTH; goto end; }
    TRY(aps_tlv_state(&rx, 4));
    TRY(aps_kdf(shared, 32, "Control-Salt", "Control-Write-Encryption-Key", write_key));
    TRY(aps_kdf(shared, 32, "Control-Salt", "Control-Read-Encryption-Key", read_key));
    memcpy(audio_key, shared, 32); rc = 0;
end:
    if(rc){ aps_wipe(write_key, 32); aps_wipe(read_key, 32); aps_wipe(audio_key, 32); }
    aps_wipe(secret, sizeof secret); aps_wipe(shared, sizeof shared); aps_wipe(encrypt, sizeof encrypt);
    aps_wipe(msg, sizeof msg); aps_wipe(&tx, sizeof tx); aps_wipe(&rx, sizeof rx);
    aps_wipe(&sub, sizeof sub); aps_wipe(&enc, sizeof enc); return rc;
}
#undef TRY
