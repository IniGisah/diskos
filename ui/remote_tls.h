/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_REMOTE_TLS_H
#define DISKOS_REMOTE_TLS_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef struct remote_tls remote_tls;
enum { RT_OK = 0, RT_READ = -1, RT_WRITE = -2, RT_TIMEOUT = -3,
       RT_CANCEL = -4, RT_ENTROPY = -5, RT_IO = -6, RT_PROTOCOL = -7,
       RT_CLOSED = -8, RT_MEMORY = -9, RT_ARGUMENT = -10 };
/* Owner decision 53: encrypted device connection, NO certificate/deviceauth
 * check in 1.2.2; device authentication is parked. Not for HTTPS. */
enum remote_tls_policy { RT_DEVICE_UNVERIFIED = 1 };
/* Optional test/platform hooks. I/O uses POSIX counts/errno, must never block.
 * Entropy must fill all bytes or fail. Callbacks run on the session owner thread. */
typedef struct {
    void *ctx;
    ssize_t (*send)(void *, int, const void *, size_t);
    ssize_t (*recv)(void *, int, void *, size_t);
    int (*entropy)(void *, unsigned char *, size_t);
    int64_t (*now_ms)(void *);
    int (*cancelled)(void *);
} remote_tls_hooks;
/* Caller owns fd (including after free); socket need not have O_NONBLOCK set.
 * Absolute CLOCK_MONOTONIC milliseconds; cancellation takes precedence.
 * Open seeds once; every failure leaves *out NULL. No implicit waits/retries. */
int remote_tls_open(remote_tls **out, int fd, enum remote_tls_policy policy,
                    const remote_tls_hooks *hooks, int64_t deadline);
/* Each step returns RT_READ/RT_WRITE when caller must wait for readiness.
 * Retry the SAME write buffer/length and deadline until progress/error.
 * Positive read/write counts are plaintext progress; zero-length I/O is invalid.
 * All errors except readiness/argument are terminal.
 * Close drains any pending write, then sends close_notify. Keep that write
 * buffer alive and unchanged until close finishes. Once closing starts,
 * only close/free are valid. Repeated completed close returns RT_CLOSED.
 * free always releases state, even after cancellation or failed handshake. */
int remote_tls_handshake(remote_tls *, int64_t deadline);
int remote_tls_read(remote_tls *, void *, size_t, int64_t deadline);
int remote_tls_write(remote_tls *, const void *, size_t, int64_t deadline);
int remote_tls_close(remote_tls *, int64_t deadline);
void remote_tls_free(remote_tls *);
#endif
