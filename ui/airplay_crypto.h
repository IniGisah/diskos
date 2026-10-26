/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_CRYPTO_H
#define DISKOS_AIRPLAY_CRYPTO_H
#include <stddef.h>
#include <stdint.h>
/* Single worker. No logging. All functions return 0 on success, -1 on failure.
 * No plaintext or key output is usable following any failure. */
int aps_random(void *, size_t);
int aps_hash(const void *, size_t, uint8_t out[64]);
int aps_kdf(const void *, size_t, const char *salt, const char *info, uint8_t out[32]);
int aps_seal(const uint8_t key[32], const uint8_t nonce[12], const void *ad, size_t an,
             const void *plain, size_t n, void *cipher, uint8_t tag[16]);
int aps_open(const uint8_t key[32], const uint8_t nonce[12], const void *ad, size_t an,
             const void *cipher, size_t n, const uint8_t tag[16], void *plain);
void aps_nonce(uint8_t out[12], uint64_t counter);
int aps_exchange(const uint8_t secret[32], const uint8_t peer[32], uint8_t shared[32]);
void aps_public(const uint8_t secret[32], uint8_t public_key[32]);
void aps_sign(const uint8_t seed[32], const void *, size_t, uint8_t signature[64]);
void aps_sign_public(const uint8_t seed[32], uint8_t public_key[32]);
int aps_check(const uint8_t public_key[32], const void *, size_t, const uint8_t signature[64]);
void aps_wipe(void *, size_t);
int aps_equal(const void *, const void *, size_t);
/* HAP SRP-6a, SHA-512, RFC 5054 3072-bit group, generator 5. Caller supplies
 * a fresh random 32-byte exponent. B must be exactly 384 bytes; rejects B=0,
 * B>=N, u=0 and S=0. No persistent credential until BOTH proofs/signatures pass. */
int aps_srp(const char *pin, const uint8_t salt[16], const uint8_t B[384],
            const uint8_t secret[32], uint8_t A[384], uint8_t proof[64],
            uint8_t expected[64], uint8_t key[64]);
#endif
