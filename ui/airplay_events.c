/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_events.h"
#include "airplay_crypto.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
static const char *text(plist_t d, const char *key){
    plist_t p = plist_dict_get_item(d, key); uint64_t n;
    return p && plist_get_node_type(p) == PLIST_STRING ? plist_get_string_ptr(p, &n) : NULL;
}
static int number(plist_t d, const char *key, double *out){
    plist_t p = plist_dict_get_item(d, key);
    if(!p) return -1;
    if(plist_get_node_type(p) == PLIST_REAL) plist_get_real_val(p, out);
    else if(plist_get_node_type(p) == PLIST_UINT){ uint64_t v; plist_get_uint_val(p, &v); *out = (double)v; }
    else return -1;
    return isfinite(*out) ? 0 : -1;
}
int aps_event_command(const void *body, size_t n, aps_message *m){
    plist_t p = aps_plist_read(body, n); if(!p) return -1;
    memset(m, 0, sizeof *m); m->type = APS_COMMAND;
    const char *type = text(p, "type"); int result = 0;
    if(!type){ result = -1; goto end; }
    if(!strcmp(type, "sendMediaRemoteCommand")){
        uint64_t code; const char *name = text(p, "value");
        int have_code = !aps_plist_uint(p, "modernMediaRemoteCommand", &code);
        if(!have_code){
            /* Some receivers encode the command number as decimal text. */
            const char *s = text(p, "modernMediaRemoteCommand");
            if(s && *s){ code = 0; have_code = 1;
                for(; *s; s++) if(*s < '0' || *s > '9' || code > 1000){ have_code = 0; break; } else code = code*10 + (unsigned)(*s-'0');
            }
        }
        if(!have_code && name){
            const char *names[] = {"play","paus","plps","stop","nitm","pitm"};
            for(unsigned i = 0; i < 6; i++) if(!strcmp(name, names[i])){ code = i; have_code = 1; break; }
        }
        if(!have_code){ result = -1; goto end; }
        result = 1;
        switch(code){
        case 0: case 1: case 2: m->command = PB_CMD_TOGGLE; m->play = code == 2 ? -1 : code == 0; break;
        case 3: m->command = APS_REMOTE_STOP; break;
        case 4: m->command = PB_CMD_NEXT; break;
        case 5: m->command = PB_CMD_PREV; break;
        case 24: {
            double seconds; plist_t params = plist_dict_get_item(p, "params");
            if(!params || number(params, "kMRMediaRemoteOptionPlaybackPosition", &seconds) || seconds < 0 || seconds > INT32_MAX/1000){ result = -1; break; }
            m->command = PB_CMD_SEEK; m->position_ms = (int64_t)llround(seconds*1000); break;
        }
        default: result = 0; break;
        }
    }
end:
    plist_free(p); return result;
}
static int uint_text(const char *s, unsigned *v){
    *v = 0; if(!*s) return -1;
    for(; *s; s++){
        if(*s < '0' || *s > '9' || *v > (UINT32_MAX-(unsigned)(*s-'0'))/10) return -1;
        *v = *v*10 + (unsigned)(*s-'0');
    }
    return 0;
}
static int event_request(aps_events *e, aps_rtsp *r, aps_remote *remote){
    size_t h = 0;
    for(size_t i = 0; i + 3 < e->used; i++) if(!memcmp(e->input+i, "\r\n\r\n", 4)){ h = i+4; break; }
    if(!h) return e->used >= 4096 ? -1 : 0;
    if(h >= 4096) return -1;
    aps_reply reply = {0}; memcpy(reply.headers, e->input, h);
    for(size_t i = 0; i < h; i++) if(!e->input[i] || (e->input[i] < 32 && e->input[i] != '\r' && e->input[i] != '\n' && e->input[i] != '\t')) return -1;
    if(strncmp(reply.headers, "POST /command RTSP/1.0\r\n", 24)) return -1;
    char value[128], server[128] = ""; unsigned length = 0, seq = 0;
    if(aps_rtsp_header(&reply, "Content-Length", value, sizeof value) != 1 || uint_text(value, &length) || length > sizeof e->input-h) return -1;
    if(aps_rtsp_header(&reply, "Transfer-Encoding", value, sizeof value) != 0) return -1;
    int have_seq = aps_rtsp_header(&reply, "CSeq", value, sizeof value);
    if(have_seq < 0 || (have_seq && uint_text(value, &seq))) return -1;
    if(aps_rtsp_header(&reply, "Server", server, sizeof server) < 0) return -1;
    if(e->used < h + length) return 0;
    aps_message command; int got = aps_event_command(e->input+h, length, &command);
    if(got < 0) return -1;
    if(!have_seq || !e->have_seq || seq > e->last_seq){
        if(got){ if(remote->n == 8) return -1; remote->commands[remote->n++] = command; }
        if(have_seq){ e->have_seq = 1; e->last_seq = seq; }
    } else if(seq != e->last_seq) return -1;
    char response[320];
    char cseq[48] = ""; if(have_seq) snprintf(cseq, sizeof cseq, "CSeq: %u\r\n", seq);
    int n = snprintf(response, sizeof response, "RTSP/1.0 200 OK\r\n%s%s%s%s\r\n", cseq,
        server[0] ? "Server: " : "", server, server[0] ? "\r\n" : "");
    if(n < 0 || (size_t)n >= sizeof response) return -1;
    n = aps_record_seal(r->write_key, &r->tx_counter, response, (size_t)n, e->output);
    if(n < 0) return -1;
    e->length = (size_t)n; e->written = 0;
    e->used -= h+length; memmove(e->input, e->input+h+length, e->used);
    e->deadline = aps_now_ms()+5000; return 1;
}
int aps_events_service(aps_events *e, aps_rtsp *r, aps_remote *remote){
    if(r->fd < 0) return 0;
    if(!r->secure || r->dead) return -1;
    for(unsigned step = 0; step < 8; step++){
        if(e->length){
            if(aps_now_ms() >= e->deadline) return -1;
            ssize_t sent = send(r->fd, e->output+e->written, e->length-e->written, MSG_DONTWAIT | MSG_NOSIGNAL);
            if(sent < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
            if(sent <= 0) return -1;
            e->written += (size_t)sent;
            if(e->written < e->length) return 0;
            e->length = e->written = 0; if(!e->used && !r->raw_n) e->deadline = 0;
        }
        int parsed = event_request(e, r, remote); if(parsed < 0) return -1;
        if(parsed) continue;
        if(e->deadline && aps_now_ms() >= e->deadline) return -1;
        size_t available = r->raw_n;
        if(available >= 2){
            size_t length = r->raw[0] | (size_t)r->raw[1]<<8;
            if(!length || length > 1024) return -1;
            if(available >= length+18){
                uint8_t plain[1024];
                int n = aps_record_open(r->read_key, &r->rx_counter, r->raw, length+18, plain);
                if(n < 0 || (size_t)n > sizeof e->input-e->used) return -1;
                memcpy(e->input+e->used, plain, (size_t)n); e->used += (size_t)n;
                r->raw_n -= length+18; memmove(r->raw, r->raw+length+18, r->raw_n);
                continue;
            }
        }
        ssize_t n = recv(r->fd, r->raw+r->raw_n, sizeof r->raw-r->raw_n, MSG_DONTWAIT);
        if(n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        if(n <= 0) return -1;
        if(!e->deadline) e->deadline = aps_now_ms()+5000;
        r->raw_n += (size_t)n;
    }
    return 0;
}
int aps_events_advertise(aps_rtsp *r, aps_reply *reply){
    plist_t p = plist_new_dict(), params = plist_new_dict(), commands = plist_new_array();
    aps_plist_text(p, "type", "updateMRSupportedCommands");
    const unsigned codes[] = {0,1,2,3,4,5,24};
    for(unsigned i = 0; i < sizeof codes/sizeof codes[0]; i++){
        plist_t info = plist_new_dict(); aps_plist_int(info, "kCommandInfoCommandKey", codes[i]);
        plist_dict_set_item(info, "kCommandInfoEnabledKey", plist_new_bool(1));
        if(codes[i] == 24){
            plist_t options = plist_new_dict();
            plist_dict_set_item(options, "kMRMediaRemoteCommandInfoCanBeControlledByScrubbingKey", plist_new_bool(1));
            plist_dict_set_item(options, "kMRMediaRemoteCommandInfoSupportsReferencePosition", plist_new_bool(0));
            plist_dict_set_item(info, "kCommandInfoOptionsKey", options);
        }
        char *bytes = NULL; uint32_t n = 0; plist_to_bin(info, &bytes, &n); plist_free(info);
        if(!bytes || !n){ free(bytes); plist_free(p); plist_free(params); plist_free(commands); return -1; }
        plist_array_append_item(commands, plist_new_data(bytes, n)); free(bytes);
    }
    plist_dict_set_item(params, "mrSupportedCommandsFromSender", commands); plist_dict_set_item(p, "params", params);
    int rc = aps_plist_request_path(r, "POST", "/command", p, reply, 1000); plist_free(p);
    if(rc) return -1;
    if(reply->status == 200) return 0;
    /* Some audio receivers reject MediaRemote advertisements. Keep their
     * authenticated audio session; transport and authentication errors remain
     * fatal. Never report an unsupported advertisement as accepted. */
    if(reply->status == 400 || reply->status == 404 || reply->status == 405 || reply->status == 501){
        fprintf(stderr, "AirPlay supported commands unavailable: status=%d\n", reply->status);
        return 1;
    }
    return -1;
}
