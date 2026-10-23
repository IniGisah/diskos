/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent HAP primitives using the pinned licensed crypto libraries.
 * Protocol: RFC 5054 section 2; RFC 5869; RFC 8439; RFC 8032. */
#include "airplay_crypto.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <mbedtls/bignum.h>
#include <mbedtls/chachapoly.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/sha512.h>
#include <monocypher.h>
#include <monocypher-ed25519.h>
void aps_wipe(void *p, size_t n){ crypto_wipe(p, n); }
int aps_equal(const void *a, const void *b, size_t n){
    const uint8_t *x = a, *y = b; unsigned v = 0;
    for(size_t i = 0; i < n; i++) v |= x[i] ^ y[i];
    return v == 0;
}
int aps_random(void *p, size_t n){
    uint8_t *b = p; size_t k = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if(fd < 0) return -1;
    while(k < n){
        ssize_t r = read(fd, b + k, n - k);
        if(r < 0 && errno == EINTR) continue;
        if(r <= 0){ close(fd); aps_wipe(p, n); return -1; }
        k += (size_t)r;
    }
    close(fd); return 0;
}
int aps_hash(const void *p, size_t n, uint8_t out[64]){ return mbedtls_sha512(p, n, out, 0) ? -1 : 0; }
int aps_kdf(const void *p, size_t n, const char *salt, const char *info, uint8_t out[32]){
    int r = mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA512),
                         (const uint8_t *)salt, strlen(salt), p, n,
                         (const uint8_t *)info, strlen(info), out, 32);
    if(r) aps_wipe(out, 32);
    return r ? -1 : 0;
}
void aps_nonce(uint8_t out[12], uint64_t n){
    memset(out, 0, 12);
    for(int i = 0; i < 8; i++) out[i + 4] = (uint8_t)(n >> (8 * i));
}
int aps_seal(const uint8_t key[32], const uint8_t nonce[12], const void *ad, size_t an,
             const void *plain, size_t n, void *cipher, uint8_t tag[16]){
    mbedtls_chachapoly_context c; mbedtls_chachapoly_init(&c);
    int r = mbedtls_chachapoly_setkey(&c, key);
    if(!r) r = mbedtls_chachapoly_encrypt_and_tag(&c, n, nonce, ad, an, plain, cipher, tag);
    mbedtls_chachapoly_free(&c);
    if(r){ aps_wipe(cipher, n); aps_wipe(tag, 16); }
    return r ? -1 : 0;
}
int aps_open(const uint8_t key[32], const uint8_t nonce[12], const void *ad, size_t an,
             const void *cipher, size_t n, const uint8_t tag[16], void *plain){
    mbedtls_chachapoly_context c; mbedtls_chachapoly_init(&c);
    int r = mbedtls_chachapoly_setkey(&c, key);
    if(!r) r = mbedtls_chachapoly_auth_decrypt(&c, n, nonce, ad, an, tag, cipher, plain);
    mbedtls_chachapoly_free(&c);
    if(r) aps_wipe(plain, n);
    return r ? -1 : 0;
}
void aps_public(const uint8_t secret[32], uint8_t out[32]){ crypto_x25519_public_key(out, secret); }
int aps_exchange(const uint8_t secret[32], const uint8_t peer[32], uint8_t out[32]){
    static const uint8_t zero[32];
    crypto_x25519(out, secret, peer);
    return aps_equal(out, zero, 32) ? -1 : 0;
}
void aps_sign_public(const uint8_t seed[32], uint8_t out[32]){
    uint8_t copy[32], sk[64]; memcpy(copy, seed, 32);
    crypto_ed25519_key_pair(sk, out, copy); aps_wipe(sk, sizeof sk); aps_wipe(copy, sizeof copy);
}
void aps_sign(const uint8_t seed[32], const void *p, size_t n, uint8_t out[64]){
    uint8_t copy[32], sk[64], pk[32]; memcpy(copy, seed, 32);
    crypto_ed25519_key_pair(sk, pk, copy); crypto_ed25519_sign(out, sk, p, n);
    aps_wipe(sk, sizeof sk); aps_wipe(copy, sizeof copy);
}
int aps_check(const uint8_t pk[32], const void *p, size_t n, const uint8_t sig[64]){
    return crypto_ed25519_check(sig, pk, p, n) ? -1 : 0;
}
static const char prime[] =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
    "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
    "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
    "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33"
    "A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864"
    "D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E2"
    "08E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";
/* No allocations depend on a peer length. MPI objects are wiped by mpi_free. */
int aps_srp(const char *pin, const uint8_t salt[16], const uint8_t B[384],
            const uint8_t secret[32], uint8_t A[384], uint8_t proof[64],
            uint8_t expected[64], uint8_t key[64]){
    enum { N, G, K, X, U, S, PRIV, PUB, PEER, T, E, COUNT };
    mbedtls_mpi v[COUNT]; uint8_t buf[1024], h[64], ng[64], hg[64];
    int rc = -1; size_t pn = pin ? strlen(pin) : 0;
    for(int i = 0; i < COUNT; i++) mbedtls_mpi_init(&v[i]);
#define DO(x) do { if((x) != 0) goto end; } while(0)
    if(pn < 4 || pn > 8) goto end;
    for(size_t i = 0; i < pn; i++) if(pin[i] < '0' || pin[i] > '9') goto end;
    DO(mbedtls_mpi_read_string(&v[N], 16, prime)); DO(mbedtls_mpi_lset(&v[G], 5));
    DO(mbedtls_mpi_read_binary(&v[PEER], B, 384));
    if(mbedtls_mpi_cmp_int(&v[PEER], 0) <= 0 || mbedtls_mpi_cmp_mpi(&v[PEER], &v[N]) >= 0) goto end;
    DO(mbedtls_mpi_read_binary(&v[PRIV], secret, 32));
    if(mbedtls_mpi_cmp_int(&v[PRIV], 0) == 0) goto end;
    DO(mbedtls_mpi_exp_mod(&v[PUB], &v[G], &v[PRIV], &v[N], NULL));
    DO(mbedtls_mpi_write_binary(&v[PUB], A, 384));
    DO(mbedtls_mpi_write_binary(&v[N], buf, 384));
    memset(buf + 384, 0, 384); buf[767] = 5;
    DO(aps_hash(buf, 768, h)); DO(mbedtls_mpi_read_binary(&v[K], h, 64));
    memcpy(buf, A, 384); memcpy(buf + 384, B, 384);
    DO(aps_hash(buf, 768, h)); DO(mbedtls_mpi_read_binary(&v[U], h, 64));
    if(!mbedtls_mpi_cmp_int(&v[U], 0)) goto end;
    memcpy(buf, "Pair-Setup:", 11); memcpy(buf + 11, pin, pn);
    DO(aps_hash(buf, 11 + pn, h)); memcpy(buf, salt, 16); memcpy(buf + 16, h, 64);
    DO(aps_hash(buf, 80, h)); DO(mbedtls_mpi_read_binary(&v[X], h, 64));
    DO(mbedtls_mpi_exp_mod(&v[T], &v[G], &v[X], &v[N], NULL));
    DO(mbedtls_mpi_mul_mpi(&v[T], &v[K], &v[T]));
    DO(mbedtls_mpi_sub_mpi(&v[T], &v[PEER], &v[T])); DO(mbedtls_mpi_mod_mpi(&v[T], &v[T], &v[N]));
    DO(mbedtls_mpi_mul_mpi(&v[E], &v[U], &v[X])); DO(mbedtls_mpi_add_mpi(&v[E], &v[E], &v[PRIV]));
    DO(mbedtls_mpi_exp_mod(&v[S], &v[T], &v[E], &v[N], NULL));
    if(!mbedtls_mpi_cmp_int(&v[S], 0)) goto end;
    size_t sn = mbedtls_mpi_size(&v[S]); DO(mbedtls_mpi_write_binary(&v[S], buf, sn)); DO(aps_hash(buf, sn, key));
    DO(mbedtls_mpi_write_binary(&v[N], buf, 384)); DO(aps_hash(buf, 384, ng));
    buf[0] = 5; DO(aps_hash(buf, 1, hg));
    for(int i = 0; i < 64; i++) buf[i] = ng[i] ^ hg[i];
    DO(aps_hash("Pair-Setup", 10, h)); memcpy(buf + 64, h, 64); memcpy(buf + 128, salt, 16);
    /* HAP's proof uses minimal big-endian A/B/S, unlike the padded k and u. */
    size_t na = mbedtls_mpi_size(&v[PUB]), nb = mbedtls_mpi_size(&v[PEER]);
    DO(mbedtls_mpi_write_binary(&v[PUB], buf + 144, na));
    DO(mbedtls_mpi_write_binary(&v[PEER], buf + 144 + na, nb));
    memcpy(buf + 144 + na + nb, key, 64); DO(aps_hash(buf, 208 + na + nb, proof));
    DO(mbedtls_mpi_write_binary(&v[PUB], buf, na)); memcpy(buf + na, proof, 64); memcpy(buf + na + 64, key, 64);
    DO(aps_hash(buf, na + 128, expected)); rc = 0;
end:
    for(int i = 0; i < COUNT; i++) mbedtls_mpi_free(&v[i]);
    aps_wipe(buf, sizeof buf); aps_wipe(h, sizeof h); aps_wipe(ng, sizeof ng); aps_wipe(hg, sizeof hg);
    if(rc){ aps_wipe(A, 384); aps_wipe(proof, 64); aps_wipe(expected, 64); aps_wipe(key, 64); }
#undef DO
    return rc;
}
