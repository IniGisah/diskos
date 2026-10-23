/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_CAST_WIRE_H
#define DISKOS_CAST_WIRE_H
#include "remote_tls.h"
#include <cast_channel.pb.h>
/* 32 KiB accommodates a 16 KiB status string plus routing/unknown fields.
 * Fixed buffers: no allocation based on an untrusted prefix, no resync. */
#define CW_MAX_FRAME 32768u
#define CW_MESSAGE 1
/* Single owner; allocate outside small thread stacks. TLS remains caller-owned.
 * message is borrowed until the next read. No JSON or session interpretation. */
typedef struct {
    remote_tls *tls;
    openscreen_cast_proto_CastMessage message;
    unsigned char rx[CW_MAX_FRAME], tx[CW_MAX_FRAME+4], prefix[4];
    size_t prefix_used, rx_used, rx_len, tx_used, tx_len;
    int error;
} cast_wire;
void cast_wire_init(cast_wire *, remote_tls *);
/* One message per call; coalesced frames remain in TLS for subsequent calls.
 * CW_MESSAGE, RT_READ/RT_WRITE, or terminal RT_* error. Truncated EOF is
 * RT_PROTOCOL. All terminal errors latch across read/queue/flush; reconnect
 * requires fresh state and a fresh TLS connection. Caller owns deadlines. */
int cast_wire_read(cast_wire *, int64_t deadline);
/* Copy bounded NUL-terminated strings; source/destination/namespace 1..127
 * bytes, payload 0..16383 bytes. One queued frame; RT_ARGUMENT if busy/invalid.
 * Queue needs 33 KiB of temporary stack and does no I/O. Flush preserves the same buffer/length on TLS retries;
 * RT_OK means fully sent. Do not change deadline while a flush is pending. */
int cast_wire_queue(cast_wire *, const char *source, const char *destination,
                    const char *ns, const char *payload);
int cast_wire_flush(cast_wire *, int64_t deadline);
#endif
