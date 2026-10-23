/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cast_wire.h"
#include <pb_decode.h>
#include <pb_encode.h>
#include <string.h>

static int result(cast_wire *w, int r)
{
    if (r != RT_READ && r != RT_WRITE) w->error = r;
    return r;
}
void cast_wire_init(cast_wire *w, remote_tls *tls)
{
    memset(w, 0, sizeof *w);
    w->tls = tls;
}
int cast_wire_read(cast_wire *w, int64_t deadline)
{
    if (!w || !w->tls) return RT_ARGUMENT;
    if (w->error) return w->error;
    while (w->prefix_used < 4) {
        int n = remote_tls_read(w->tls, w->prefix+w->prefix_used,
                                4-w->prefix_used, deadline);
        if (n <= 0) return result(w, n == RT_CLOSED && w->prefix_used ? RT_PROTOCOL : n);
        w->prefix_used += (size_t)n;
    }
    if (!w->rx_len) {
        const unsigned char *p = w->prefix;
        uint32_t n = (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
        if (!n || n > CW_MAX_FRAME) return result(w, RT_PROTOCOL);
        w->rx_len = n;
    }
    while (w->rx_used < w->rx_len) {
        int n = remote_tls_read(w->tls, w->rx+w->rx_used, w->rx_len-w->rx_used, deadline);
        if (n <= 0) return result(w, n == RT_CLOSED ? RT_PROTOCOL : n);
        w->rx_used += (size_t)n;
    }
    pb_istream_t stream = pb_istream_from_buffer(w->rx, w->rx_len);
    openscreen_cast_proto_CastMessage *m = &w->message;
    if (!pb_decode(&stream, openscreen_cast_proto_CastMessage_fields, m) ||
        m->protocol_version != openscreen_cast_proto_CastMessage_ProtocolVersion_CASTV2_1_0 ||
        !m->source_id[0] || !m->destination_id[0] || !m->namespace[0] ||
        m->payload_type != openscreen_cast_proto_CastMessage_PayloadType_STRING ||
        !m->has_payload_utf8 || m->has_payload_binary ||
        (m->has_continued && m->continued) || m->has_remaining_length)
        return result(w, RT_PROTOCOL);
    w->prefix_used = w->rx_used = w->rx_len = 0;
    return CW_MESSAGE;
}
static int bounded(const char *s, size_t cap, int required)
{
    return s && (!required || s[0]) && strnlen(s, cap) < cap;
}
int cast_wire_queue(cast_wire *w, const char *source, const char *destination,
                    const char *ns, const char *payload)
{
    if (!w || !w->tls) return RT_ARGUMENT;
    if (w->error) return w->error;
    if (w->tx_len || !bounded(source, 128, 1) || !bounded(destination, 128, 1) ||
        !bounded(ns, 128, 1) || !bounded(payload, 16384, 0)) return RT_ARGUMENT;
    /* Keep the borrowed receive message intact while encoding independently. */
    openscreen_cast_proto_CastMessage m = openscreen_cast_proto_CastMessage_init_zero;
    strcpy(m.source_id, source); strcpy(m.destination_id, destination);
    strcpy(m.namespace, ns); strcpy(m.payload_utf8, payload); m.has_payload_utf8 = true;
    pb_ostream_t stream = pb_ostream_from_buffer(w->tx+4, CW_MAX_FRAME);
    if (!pb_encode(&stream, openscreen_cast_proto_CastMessage_fields, &m)) return RT_ARGUMENT;
    uint32_t n = (uint32_t)stream.bytes_written;
    w->tx[0] = n>>24; w->tx[1] = n>>16; w->tx[2] = n>>8; w->tx[3] = n;
    w->tx_len = n+4; w->tx_used = 0;
    return RT_OK;
}
int cast_wire_flush(cast_wire *w, int64_t deadline)
{
    if (!w || !w->tls) return RT_ARGUMENT;
    if (w->error) return w->error;
    while (w->tx_used < w->tx_len) {
        int n = remote_tls_write(w->tls, w->tx+w->tx_used, w->tx_len-w->tx_used, deadline);
        if (n <= 0) return result(w, n);
        w->tx_used += (size_t)n;
    }
    w->tx_used = w->tx_len = 0;
    return RT_OK;
}
