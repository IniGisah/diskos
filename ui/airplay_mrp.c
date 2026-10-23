/* SPDX-License-Identifier: GPL-3.0-or-later */
/* AP2 type-130 framing and MRP field numbers: pyatv's MIT protocol schemas.
 * Use the existing nanopb wire primitives; no recursive or allocating protobuf parser. */
#include "airplay_mrp.h"
#include "airplay_crypto.h"
#include <pb_decode.h>
#include <pb_encode.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
typedef struct { unsigned tag; pb_wire_type_t wire; const uint8_t *data; size_t n; uint64_t v; } field;
typedef struct { field f[32]; unsigned n; } fields;
static int parse(const void *data, size_t n, fields *out){
    if(!n || n > 8192) return -1;
    pb_istream_t s = pb_istream_from_buffer(data, n); memset(out, 0, sizeof *out);
    while(s.bytes_left){
        if(out->n == 32) return -1;
        field *f = &out->f[out->n++]; bool eof;
        if(!pb_decode_tag(&s, &f->wire, &f->tag, &eof) || !f->tag) return -1;
        for(unsigned i = 0; i+1 < out->n; i++) if(out->f[i].tag == f->tag) return -1;
        if(f->wire == PB_WT_VARINT){ if(!pb_decode_varint(&s, &f->v)) return -1; }
        else if(f->wire == PB_WT_STRING){
            uint64_t len; if(!pb_decode_varint(&s, &len) || len > s.bytes_left) return -1;
            f->data = s.state; f->n = (size_t)len; if(!pb_read(&s, NULL, f->n)) return -1;
        } else if(f->wire == PB_WT_32BIT || f->wire == PB_WT_64BIT){
            f->n = f->wire == PB_WT_32BIT ? 4 : 8; f->data = s.state;
            if(!pb_read(&s, NULL, f->n)) return -1;
        } else return -1;
    }
    return 0;
}
static const field *get(const fields *p, unsigned tag, pb_wire_type_t wire){
    for(unsigned i = 0; i < p->n; i++) if(p->f[i].tag == tag) return p->f[i].wire == wire ? &p->f[i] : NULL;
    return NULL;
}
int aps_mrp_command(const void *data, size_t n, aps_message *m){
    fields p, inner; const field *type, *body, *v;
    if(parse(data, n, &p) || !(type = get(&p, 1, PB_WT_VARINT))) return -1;
    memset(m, 0, sizeof *m); m->type = APS_COMMAND;
    v = get(&p, 4, PB_WT_VARINT); if(v && v->v) return 0; /* receiver refusal, not a malformed command */
    unsigned ext = type->v == 1 ? 6 : type->v == 51 ? 55 : type->v == 52 ? 56 : type->v == 50 ? 54 : 0;
    if(!ext) return 0;
    if(!(body = get(&p, ext, PB_WT_STRING)) || parse(body->data, body->n, &inner)) return -1;
    if(type->v != 1){
        float level;
        if(!(v = get(&inner, 1, PB_WT_32BIT))) return -1;
        uint32_t bits = (uint32_t)v->data[0] | (uint32_t)v->data[1]<<8 | (uint32_t)v->data[2]<<16 | (uint32_t)v->data[3]<<24;
        memcpy(&level, &bits, 4);
        if(!isfinite(level) || level < 0 || level > 1) return -1;
        m->command = type->v == 51 ? PB_CMD_VOLUME : APS_VOLUME_OBSERVED;
        m->position_ms = (int64_t)lroundf(level*100); return 1;
    }
    if(!(v = get(&inner, 1, PB_WT_VARINT))) return -1;
    switch(v->v){
    case 1: case 2: case 3: m->command = PB_CMD_TOGGLE; m->play = v->v == 3 ? -1 : v->v == 1; break;
    case 4: m->command = APS_REMOTE_STOP; break;
    case 5: m->command = PB_CMD_NEXT; break;
    case 6: m->command = PB_CMD_PREV; break;
    case 45: {
        fields options;
        if(!(v = get(&inner, 2, PB_WT_STRING)) || parse(v->data, v->n, &options) || !(v = get(&options, 9, PB_WT_64BIT))) return -1;
        uint64_t bits = 0; for(unsigned i = 0; i < 8; i++) bits |= (uint64_t)v->data[i] << (i*8);
        double seconds; memcpy(&seconds, &bits, 8);
        if(!isfinite(seconds) || seconds < 0 || seconds > INT32_MAX/1000) return -1;
        m->command = PB_CMD_SEEK; m->position_ms = (int64_t)llround(seconds*1000); break;
    }
    default: return 0;
    }
    return 1;
}
static int put(pb_ostream_t *s, unsigned tag, uint64_t v){ return pb_encode_tag(s, PB_WT_VARINT, tag) && pb_encode_varint(s, v); }
static int bytes(pb_ostream_t *s, unsigned tag, const void *p, size_t n){ return pb_encode_tag(s, PB_WT_STRING, tag) && pb_encode_string(s, p, n); }
static uint32_t be32(const uint8_t *p){ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static int frame(aps_mrp *m, const char *kind, const uint8_t seq[8], plist_t payload){
    char *body = NULL; uint32_t n = 0;
    if(payload) plist_to_bin(payload, &body, &n);
    if((payload && !body) || m->length > sizeof m->output-32 || n > sizeof m->output-32-m->length){ free(body); return -1; }
    uint8_t *p = m->output+m->length; memset(p, 0, 32); uint32_t total = n+32;
    p[0] = total>>24; p[1] = total>>16; p[2] = total>>8; p[3] = total;
    memcpy(p+4, kind, 4); if(payload) memcpy(p+16, "comm", 4);
    if(seq) memcpy(p+20, seq, 8);
    else { for(unsigned i = 0; i < 8; i++) p[27-i] = (uint8_t)(m->sequence >> (8*i)); }
    if(n) memcpy(p+32, body, n);
    free(body); m->length += total;
    if(!m->deadline) m->deadline = aps_now_ms()+5000;
    return 0;
}
static int message(aps_mrp *m, unsigned type, unsigned ext, const void *body, size_t n, const field *identifier){
    uint8_t data[1024], packed[1034]; pb_ostream_t s = pb_ostream_from_buffer(data, sizeof data);
    if(!put(&s, 1, type) || !bytes(&s, ext, body, n)) return -1;
    if(identifier && (identifier->n > 128 || !bytes(&s, 2, identifier->data, identifier->n))) return -1;
    uint8_t random[16]; char unique[33];
    if(aps_random(random, sizeof random)) return -1;
    for(unsigned i = 0; i < 16; i++) snprintf(unique+2*i, 3, "%02x", random[i]);
    if(!bytes(&s, 85, unique, 32)) return -1;
    pb_ostream_t t = pb_ostream_from_buffer(packed, sizeof packed);
    if(!pb_encode_varint(&t, s.bytes_written) || !pb_write(&t, data, s.bytes_written)) return -1;
    plist_t p = plist_new_dict(), params = plist_new_dict();
    plist_dict_set_item(params, "data", plist_new_data((const char *)packed, t.bytes_written));
    plist_dict_set_item(p, "params", params); int rc = frame(m, "sync", NULL, p); plist_free(p); return rc;
}
static int real64(pb_ostream_t *s, unsigned tag, double value){ return pb_encode_tag(s, PB_WT_64BIT, tag) && pb_encode_fixed64(s, &value); }
static int state_message(aps_mrp *m, const pb_view *v, const field *id){
    uint8_t now[800], state[900]; pb_ostream_t s = pb_ostream_from_buffer(now, sizeof now);
    float rate = v->playing ? 1 : 0;
    if(!bytes(&s, 1, v->album, strlen(v->album)) || !bytes(&s, 2, v->artist, strlen(v->artist)) ||
       !bytes(&s, 9, v->title, strlen(v->title)) || !real64(&s, 3, v->duration_ms/1000.0) ||
       !real64(&s, 4, v->position_ms/1000.0) || !pb_encode_tag(&s, PB_WT_32BIT, 5) || !pb_encode_fixed32(&s, &rate)) return -1;
    pb_ostream_t t = pb_ostream_from_buffer(state, sizeof state);
    if(!bytes(&t, 1, now, s.bytes_written) || !put(&t, 6, v->playing ? 1 : 2)) return -1;
    return message(m, 4, 9, state, t.bytes_written, id);
}
int aps_mrp_state(aps_mrp *m, const pb_view *v){
    if(m->channel.fd < 0) return 0;
    int64_t now = aps_now_ms();
    if(m->have_state && now-m->state_at < 1000 && v->playing == m->state.playing && !strcmp(v->path, m->state.path) &&
       llabs((long long)v->position_ms-m->state.position_ms) < 1000) return 0;
    if(state_message(m, v, NULL)){ m->channel.dead = 1; return -1; }
    m->state = *v; m->have_state = 1; m->state_at = now; return 0;
}
static int payload(aps_mrp *m, const uint8_t *data, size_t n, aps_remote *r){
    plist_t p = aps_plist_read(data, n); if(!p) return -1;
    plist_t params = plist_dict_get_item(p, "params"), item = params ? plist_dict_get_item(params, "data") : NULL;
    uint64_t length = 0; const char *raw = item && plist_get_node_type(item) == PLIST_DATA ? plist_get_data_ptr(item, &length) : NULL;
    /* Some frames carry non-protobuf connection information. */
    if(!raw){ plist_free(p); return item ? -1 : 0; }
    pb_istream_t s = pb_istream_from_buffer((const uint8_t *)raw, (size_t)length);
    aps_message pending[8]; unsigned count = 0, seen = 0; int rc = -1;
    while(s.bytes_left){
        uint64_t len = s.bytes_left;
        if(++seen > 8) goto end;
        if(*(const uint8_t *)s.state != 8 && (!pb_decode_varint(&s, &len) || !len || len > s.bytes_left)) goto end;
        const uint8_t *start = s.state; aps_message command;
        int got = aps_mrp_command(start, (size_t)len, &command); if(got < 0) goto end;
        fields envelope;
        if(parse(start, (size_t)len, &envelope)) goto end;
        const field *type = get(&envelope, 1, PB_WT_VARINT), *id = get(&envelope, 2, PB_WT_STRING);
        int duplicate = 0;
        if(id){
            if(!id->n || id->n > 128 || memchr(id->data, 0, id->n)) goto end;
            for(unsigned i = 0; i < 8; i++) if(strlen(m->identifiers[i]) == id->n && !memcmp(m->identifiers[i], id->data, id->n)) duplicate = 1;
        }
        if(type && type->v == 1 && id){
            /* Acknowledges mailbox acceptance, not audible completion. */
            uint8_t result[] = {8, got ? 0 : 10, 16, got ? 0 : 2};
            if(message(m, 2, 7, result, sizeof result, id)) goto end;
        }
        if(type && type->v == 3 && m->have_state && state_message(m, &m->state, id)) goto end;
        if(got && !duplicate){
            pending[count++] = command;
            if(id){ char *saved = m->identifiers[m->next_identifier++ % 8]; memcpy(saved, id->data, id->n); saved[id->n] = 0; }
        }
        if(!pb_read(&s, NULL, (size_t)len)) goto end;
    }
    if(count > 8-r->n) goto end;
    for(unsigned i = 0; i < count; i++) r->commands[r->n++] = pending[i];
    rc = 0;
end: plist_free(p); return rc;
}
void aps_mrp_init(aps_mrp *m){ memset(m, 0, sizeof *m); strcpy(m->display_name,"Snowsky Disc"); aps_rtsp_init(&m->channel, -1, NULL, NULL); }
int aps_mrp_open(aps_mrp *m, aps_rtsp *rtsp, aps_reply *reply, const char *host,
                 const uint8_t secret[32], const char *identity, const char *session){
    uint64_t seed; uint8_t wk[32], rk[32]; char salt[64]; int rc = -1;
    if(!rtsp->secure || rtsp->dead || m->channel.fd >= 0 || aps_random(&seed, sizeof seed)) return -1;
    plist_t p = plist_new_dict(), streams = plist_new_array(), stream = plist_new_dict();
    aps_plist_int(stream, "type", 130); aps_plist_int(stream, "controlType", 2); aps_plist_int(stream, "seed", seed);
    aps_plist_text(stream, "channelID", session); aps_plist_text(stream, "clientUUID", identity);
    aps_plist_text(stream, "clientTypeUUID", "1910A70F-DBC0-4242-AF95-115DB30604E1");
    plist_dict_set_item(stream, "wantsDedicatedSocket", plist_new_bool(1));
    plist_array_append_item(streams, stream); plist_dict_set_item(p, "streams", streams);
    rtsp->diag_next = "SETUP-mrp";
    int request = aps_plist_request(rtsp, "SETUP", p, reply, 2000); plist_free(p);
    if(request) return -1;
    if(reply->status != 200) return 0; /* receiver keeps its audio-session controls */
    p = aps_plist_read(reply->body, reply->n); if(aps_rtsp_diag(rtsp, "mrp-reply", p ? 0 : -1, NULL)) return -1;
    streams = plist_dict_get_item(p, "streams"); uint16_t port = 0;
    if(streams && plist_get_node_type(streams) == PLIST_ARRAY && plist_array_get_size(streams) == 1){
        uint64_t type; stream = plist_array_get_item(streams, 0);
        if(aps_plist_uint(stream, "type", &type) || type != 130 || aps_plist_port(stream, "dataPort", &port)) port = 0;
    }
    plist_free(p); if(aps_rtsp_diag(rtsp, "mrp-port", port ? 0 : -1, NULL)) return -1;
    snprintf(salt, sizeof salt, "DataStream-Salt%llu", (unsigned long long)seed);
    if(aps_rtsp_diag(rtsp, "mrp-write-key", aps_kdf(secret, 32, salt, "DataStream-Output-Encryption-Key", wk), NULL) ||
       aps_rtsp_diag(rtsp, "mrp-read-key", aps_kdf(secret, 32, salt, "DataStream-Input-Encryption-Key", rk), NULL)) goto end;
    if(aps_rtsp_diag(rtsp, "mrp-connect", aps_rtsp_connect(&m->channel, host, port, 2000), NULL) ||
       aps_rtsp_diag(rtsp, "mrp-secure", aps_rtsp_secure(&m->channel, wk, rk), NULL)) goto end;
    m->sequence = UINT64_C(0x100000000) | (uint32_t)seed;
    uint8_t info[512]; pb_ostream_t s = pb_ostream_from_buffer(info, sizeof info);
    if(!bytes(&s, 1, identity, strlen(identity)) || !bytes(&s, 2, m->display_name, strlen(m->display_name)) ||
       !bytes(&s, 5, "org.diskos.player", 17) || !put(&s, 7, 1) || !put(&s, 8, 120) ||
       !put(&s, 21, 2) || !bytes(&s, 41, session, strlen(session)) || message(m, 15, 20, info, s.bytes_written, NULL)) goto end;
    const uint8_t connected[] = {8,2}, updates[] = {8,0,16,0,24,1,32,0,40,0};
    if(message(m, 38, 42, connected, sizeof connected, NULL) || message(m, 16, 21, updates, sizeof updates, NULL)) goto end;
    uint8_t supported[128], commands[140]; pb_ostream_t list = pb_ostream_from_buffer(supported, sizeof supported);
    const unsigned codes[] = {1,2,3,4,5,6,45};
    for(unsigned i = 0; i < sizeof codes/sizeof codes[0]; i++){
        uint8_t command[] = {8, codes[i], 16, 1, 128, 1, 1};
        if(!bytes(&list, 1, command, codes[i] == 45 ? sizeof command : 4)) goto end;
    }
    pb_ostream_t defaults = pb_ostream_from_buffer(commands, sizeof commands);
    if(!bytes(&defaults, 2, supported, list.bytes_written) || message(m, 72, 75, commands, defaults.bytes_written, NULL)) goto end;
    rc = 0;
end: aps_wipe(wk, sizeof wk); aps_wipe(rk, sizeof rk); return rc;
}
int aps_mrp_volume(aps_mrp *m, int percent){
    if(m->channel.fd < 0 || m->channel.dead || !m->channel.secure || percent < -1 || percent > 100) return -1;
    uint8_t body[5] = {13}; float level = percent/100.0f; uint32_t bits; memcpy(&bits, &level, 4);
    for(unsigned i = 0; i < 4; i++) body[i+1] = (uint8_t)(bits >> (8*i));
    return message(m, percent < 0 ? 49 : 51, percent < 0 ? 53 : 55, body, percent < 0 ? 0 : sizeof body, NULL);
}
int aps_mrp_service(aps_mrp *m, aps_remote *remote){
    aps_rtsp *r = &m->channel; if(r->fd < 0) return 0;
    if(!r->secure || r->dead) return -1;
    for(unsigned step = 0; step < 16; step++){
        if(m->deadline && aps_now_ms() >= m->deadline) return -1;
        if(m->wire_n){
            ssize_t n = send(r->fd, m->wire+m->wire_at, m->wire_n-m->wire_at, MSG_DONTWAIT | MSG_NOSIGNAL);
            if(n < 0 && (errno == EINTR || errno == EAGAIN)) return 0;
            if(n <= 0) return -1;
            m->wire_at += (size_t)n; if(m->wire_at < m->wire_n) return 0;
            m->wire_n = m->wire_at = 0; continue;
        }
        if(m->at < m->length){
            size_t n = m->length-m->at; if(n > 1024) n = 1024;
            int k = aps_record_seal(r->write_key, &r->tx_counter, m->output+m->at, n, m->wire);
            if(k < 0) return -1;
            m->at += n; m->wire_n = (size_t)k; continue;
        }
        m->length = m->at = 0;
        if(m->used >= 4){
            uint32_t n = be32(m->input); if(n < 32 || n > sizeof m->input) return -1;
            if(m->used >= n){
                const uint8_t *p = m->input;
                if(memcmp(p+8, "\0\0\0\0\0\0\0\0", 8) || memcmp(p+28, "\0\0\0\0", 4)) return -1;
                if(!memcmp(p+4, "sync", 4)){
                    if(memcmp(p+16, "comm", 4) || (n > 32 && payload(m, p+32, n-32, remote)) || frame(m, "rply", p+20, NULL)) return -1;
                } else if(memcmp(p+4, "rply", 4) || n != 32 || memcmp(p+16, "\0\0\0\0", 4)) return -1;
                m->used -= n; memmove(m->input, m->input+n, m->used); continue;
            }
        }
        if(r->raw_n >= 2){
            size_t n = r->raw[0] | (size_t)r->raw[1]<<8; if(!n || n > 1024) return -1;
            if(r->raw_n >= n+18){
                uint8_t plain[1024]; int k = aps_record_open(r->read_key, &r->rx_counter, r->raw, n+18, plain);
                if(k < 0 || (size_t)k > sizeof m->input-m->used) return -1;
                memcpy(m->input+m->used, plain, (size_t)k); m->used += (size_t)k;
                r->raw_n -= n+18; memmove(r->raw, r->raw+n+18, r->raw_n); continue;
            }
        }
        if(!m->used && !r->raw_n) m->deadline = 0;
        ssize_t n = recv(r->fd, r->raw+r->raw_n, sizeof r->raw-r->raw_n, MSG_DONTWAIT);
        if(n < 0 && (errno == EINTR || errno == EAGAIN)) return 0;
        if(n <= 0) return -1;
        if(!m->deadline) m->deadline = aps_now_ms()+5000;
        r->raw_n += (size_t)n;
    }
    return 0;
}
void aps_mrp_close(aps_mrp *m){ aps_rtsp_close(&m->channel); aps_wipe(m, sizeof *m); m->channel.fd = -1; }
