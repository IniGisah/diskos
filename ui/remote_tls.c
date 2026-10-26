/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "remote_tls.h"
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ctr_drbg.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
struct remote_tls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context rng;
    remote_tls_hooks hooks;
    int fd, error, ready;
    int closing, pending_write;
    const void *pending_buf;
    size_t pending_len;
    int64_t deadline;
};
static int gate(remote_tls *s)
{
    struct timespec ts;
    int64_t now;
    if (s->error) return s->error;
    if (s->hooks.cancelled && s->hooks.cancelled(s->hooks.ctx))
        return s->error = RT_CANCEL;
    if (s->hooks.now_ms) now = s->hooks.now_ms(s->hooks.ctx);
    else {
        if (clock_gettime(CLOCK_MONOTONIC, &ts)) return s->error = RT_IO;
        now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }
    if (now >= s->deadline) return s->error = RT_TIMEOUT;
    return RT_OK;
}
static int entropy(void *ctx, unsigned char *p, size_t n)
{
    remote_tls *s = ctx;
    int fd, result;
    if (gate(s)) return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
    if (s->hooks.entropy) result = s->hooks.entropy(s->hooks.ctx, p, n);
    else {
        /* Use the vendored path directly: host getrandom(flags=0) can block.
         * One nonblocking read; even EINTR/short entropy is a hard failure. */
        fd = open(MBEDTLS_PLATFORM_DEV_RANDOM, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        result = fd < 0 ? -1 : (read(fd, p, n) == (ssize_t)n ? 0 : -1);
        if (fd >= 0) close(fd);
    }
    if (gate(s)) return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
    if (result) {
        s->error = RT_ENTROPY;
        return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
    }
    return 0;
}
static int io(remote_tls *s, unsigned char *p, size_t n, int writing)
{
    ssize_t r;
    if (gate(s)) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (writing)
        r = s->hooks.send ? s->hooks.send(s->hooks.ctx, s->fd, p, n) :
            send(s->fd, p, n, MSG_DONTWAIT | MSG_NOSIGNAL);
    else
        r = s->hooks.recv ? s->hooks.recv(s->hooks.ctx, s->fd, p, n) :
            recv(s->fd, p, n, MSG_DONTWAIT);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return writing ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_WANT_READ;
    if (r < 0) { s->error = RT_IO; return MBEDTLS_ERR_SSL_INTERNAL_ERROR; }
    if (!r) { s->error = RT_CLOSED; return MBEDTLS_ERR_SSL_CONN_EOF; }
    return (int)r;
}
static int tx(void *s, const unsigned char *p, size_t n)
{ return io(s, (unsigned char *)p, n, 1); }
static int rx(void *s, unsigned char *p, size_t n)
{ return io(s, p, n, 0); }
static int result(remote_tls *s, int r)
{
    if (gate(s)) return s->error;
    if (r == MBEDTLS_ERR_SSL_WANT_READ) return RT_READ;
    if (r == MBEDTLS_ERR_SSL_WANT_WRITE) return RT_WRITE;
    if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF)
        return s->error = RT_CLOSED;
    if (r == MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED)
        return s->error = RT_ENTROPY;
    if (r < 0) return s->error = RT_PROTOCOL;
    return r;
}
void remote_tls_free(remote_tls *s)
{
    if (!s) return;
    mbedtls_ssl_free(&s->ssl);
    mbedtls_ssl_config_free(&s->conf);
    mbedtls_ctr_drbg_free(&s->rng);
    free(s);
}
int remote_tls_open(remote_tls **out, int fd, enum remote_tls_policy policy,
                    const remote_tls_hooks *hooks, int64_t deadline)
{
    remote_tls *s;
    int r;
    if (!out) return RT_ARGUMENT;
    *out = NULL;
    if (fd < 0 || policy != RT_DEVICE_UNVERIFIED) return RT_ARGUMENT;
    s = calloc(1, sizeof(*s));
    if (!s) return RT_MEMORY;
    s->fd = fd; s->deadline = deadline;
    if (hooks) s->hooks = *hooks;
    mbedtls_ssl_init(&s->ssl);
    mbedtls_ssl_config_init(&s->conf);
    mbedtls_ctr_drbg_init(&s->rng);
    r = gate(s);
    if (!r) r = mbedtls_ctr_drbg_seed(&s->rng, entropy, s, NULL, 0);
    if (!r) r = mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (!r) {
        mbedtls_ssl_conf_min_tls_version(&s->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&s->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        /* Owner decision 53: 1.2.2 encrypts but does not authenticate
         * the receiver. Device authentication is parked. */
        mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_NONE);
        /* RAOP needs legacy crypto in the same image. Keep this TLS client
         * on the original AEAD-only cipher set rather than inheriting CBC. */
        static const int suites[] = {
            MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
            MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
            MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
            MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
            MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
            MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, 0
        };
        mbedtls_ssl_conf_ciphersuites(&s->conf, suites);
        mbedtls_ssl_conf_rng(&s->conf, mbedtls_ctr_drbg_random, &s->rng);
        r = mbedtls_ssl_setup(&s->ssl, &s->conf);
    }
    if (r || gate(s)) {
        r = s->error ? s->error : (r == MBEDTLS_ERR_SSL_ALLOC_FAILED ? RT_MEMORY : RT_PROTOCOL);
        remote_tls_free(s);
        return r;
    }
    mbedtls_ssl_set_bio(&s->ssl, s, tx, rx, NULL);
    *out = s;
    return RT_OK;
}
static int begin(remote_tls *s, int64_t deadline)
{
    if (!s) return RT_ARGUMENT;
    s->deadline = deadline;
    return gate(s);
}
int remote_tls_handshake(remote_tls *s, int64_t deadline)
{
    int r = begin(s, deadline);
    if (r) return r;
    if (s->closing) return RT_ARGUMENT;
    r = result(s, mbedtls_ssl_handshake(&s->ssl));
    if (!r) s->ready = 1;
    return r;
}
int remote_tls_read(remote_tls *s, void *p, size_t n, int64_t deadline)
{
    int r = begin(s, deadline);
    if (r) return r;
    if (s->closing || !s->ready || !p || !n) return RT_ARGUMENT;
    r = result(s, mbedtls_ssl_read(&s->ssl, p, n));
    return r ? r : (s->error = RT_CLOSED);
}
int remote_tls_write(remote_tls *s, const void *p, size_t n, int64_t deadline)
{
    int r = begin(s, deadline);
    if (r) return r;
    if (s->closing || !s->ready || !p || !n) return RT_ARGUMENT;
    if (n > MBEDTLS_SSL_OUT_CONTENT_LEN) n = MBEDTLS_SSL_OUT_CONTENT_LEN;
    s->pending_buf = p;
    s->pending_len = n;
    r = result(s, mbedtls_ssl_write(&s->ssl, p, n));
    s->pending_write = r == RT_READ || r == RT_WRITE;
    return r;
}
int remote_tls_close(remote_tls *s, int64_t deadline)
{
    int r = begin(s, deadline);
    if (r) return r;
    s->closing = 1;
    if (s->pending_write) {
        r = result(s, mbedtls_ssl_write(&s->ssl, s->pending_buf,
                                       s->pending_len));
        if (r < 0) return r;
        s->pending_write = 0;
    }
    r = result(s, mbedtls_ssl_close_notify(&s->ssl));
    if (!r) s->error = RT_CLOSED;
    return r;
}
