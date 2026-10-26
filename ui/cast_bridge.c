/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cast_bridge.h"
#include "cast_art.h"
#include "cast_format.h"
#include "sdio.h"

#define CB_REL 512   /* cast_http.c CH_REL: the longest library-relative path the server will approve */

/* The wlan0 address by the same query cast_http_start() checks the listener address against (SIOCGIFADDR on "wlan0"). */
static int real_wlan_ip(char *out, size_t n){
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr); strcpy(ifr.ifr_name, "wlan0");
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if(fd < 0) return -1;
    int rc = ioctl(fd, SIOCGIFADDR, &ifr);
    close(fd);
    if(rc) return -1;
    return inet_ntop(AF_INET, &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr, out, (socklen_t)n) ? 0 : -1;
}
const cb_ops CB_REAL = { real_wlan_ip, cast_http_start, cast_http_stop, cast_session_load, cast_session_stop_media, cast_session_snapshot };

int cb_rel_path(const char *root, const char *path, char *rel, size_t n){
    if(!root || !path || !rel || !n) return -1;
    size_t rl = strlen(root);
    while(rl > 1 && root[rl - 1] == '/') rl--;
    if(rl < 2 || strncmp(path, root, rl) || path[rl] != '/') return -1;
    const char *r = path + rl + 1; size_t len = strlen(r), s = 0;
    if(!len || len >= CB_REL || len >= n) return -1;
    for(size_t i = 0; i <= len; i++){
        if(i < len && ((unsigned char)r[i] < 0x20 || r[i] == 0x7f)) return -1;
        if(i == len || r[i] == '/'){
            size_t k = i - s;
            if(!k || (k == 1 && r[s] == '.') || (k == 2 && r[s] == '.' && r[s + 1] == '.')) return -1;
            s = i + 1;
        }
    }
    memcpy(rel, r, len + 1);
    return 0;
}
int cb_url(char *out, size_t n, const char *ipv4, unsigned port, const char *token, const char *id){
    if(!out || !n || !ipv4 || !token || !id) return -1;
    int w = snprintf(out, n, "http://%s:%u/%s/%s", ipv4, port, token, id);
    return w > 0 && (size_t)w < n ? 0 : -1;
}
/* Open rel beneath root, no symlink at any step, regular file only (the same rule the server applies when it serves). */
static int open_rel(const char *root, const char *rel){
    char p[CB_REL]; struct stat st; int d = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(d < 0) return -1;
    snprintf(p, sizeof p, "%s", rel);
    for(char *q = p;;){
        char *s = strchr(q, '/');
        if(s){ *s = 0; int nd = openat(d, q, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); close(d); if(nd < 0) return -1; d = nd; q = s + 1; continue; }
        int f = openat(d, q, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        close(d);
        if(f < 0) return -1;
        if(fstat(f, &st) || !S_ISREG(st.st_mode)){ close(f); return -1; }
        return f;
    }
}
/* The Content-Type is what cast_format decides from the file's bytes (never its extension); only a clean OK verdict may be served.
 * is_cue / is_iso are library rows the handoff does not carry: an ISO is recognised by its bytes, a CUE sub-track has no file of its own
 * and is the picker's (slice 17) to exclude before it selects. */
static int classify(const char *root, const char *rel, const char **mime){
    cast_format_info fi; if(!sd_io_begin()) return -1;
    int fd = open_rel(root, rel);
    if(fd < 0){ sd_io_end(); return -1; }
    cast_format_classify_fd(fd, 0, 0, &fi);
    close(fd); sd_io_end();
    if(fi.verdict != CF_V_OK || !fi.mime || !fi.mime[0]) return -1;
    *mime = fi.mime;
    return 0;
}
void cb_init(cb *b, const cb_ops *ops, const char *sd_root){ memset(b, 0, sizeof *b); b->ops = ops; b->root = sd_root; }
void cb_attach(cb *b, cast_session *s){ b->sess = s; b->load_armed = b->stop_armed = 0; }

/* The shared body: approve, build the URL, send LOAD, return the session's load_seq. handoff: clear both edges just before the LOAD (the
 * handoff's own attempt); the controller's later LOADs (cb_serve_load_next) must not touch the edge state cb_events consumes. */
static int serve(cb *b, int handoff, const char *path, long position_ms, const char *title, const char *artist, const char *album, unsigned *seq){
    char rel[CB_REL], url[CB_URL_MAX], iurl[CB_URL_MAX], tok[CH_TOKEN_LEN], id[CH_ID_LEN], iid[CH_ID_LEN]; const char *mime = NULL; unsigned ag;
    cs_snapshot s0, s1; cs_load l;
    if(!b->sess || !b->ops || !b->root) return -1;
    if(cb_rel_path(b->root, path, rel, sizeof rel) || classify(b->root, rel, &mime)) return -1;
    if(!b->http){
        if(b->ops->wlan_ip(b->ip, sizeof b->ip)) return -1;
        b->http = b->ops->http_start(b->ip, b->root);
        if(!b->http) return -1;
    }
    if(cast_http_approve(b->http, rel, mime, id) || cast_http_token(b->http, tok) || cb_url(url, sizeof url, b->ip, cast_http_port(b->http), tok, id)) return -1;
    memset(&l, 0, sizeof l);
    l.content_id = url; l.content_type = mime; l.title = title; l.artist = artist; l.album = album;
    l.start = position_ms > 0 ? (double)position_ms / 1000.0 : 0.0;
    /* The cover of THIS track, if the cover cache has one; any failure only means no image. The PNG itself is made when the receiver asks. */
    ag = cast_art_arm(path);
    if(!cast_http_approve_art(b->http, ag, iid) && !cb_url(iurl, sizeof iurl, b->ip, cast_http_port(b->http), tok, iid)) l.image = iurl;
    if(handoff) b->load_armed = b->stop_armed = 0;
    b->ops->sess_snap(b->sess, &s0);
    if(b->ops->sess_load(b->sess, &l) < 0){
        l.title = l.artist = l.album = NULL;   /* unencodable tags must not veto the cast; a real refusal fails again below */
        if(b->ops->sess_load(b->sess, &l) < 0) return -1;
    }
    b->ops->sess_snap(b->sess, &s1);
    if(s1.load_seq == s0.load_seq) return -1;   /* accepted but not counted: cannot be told apart from the next attempt's result */
    *seq = s1.load_seq;
    return 0;
}
int cb_serve_load(cb *b, uint32_t attempt, const char *path, long position_ms, const char *title, const char *artist, const char *album){
    unsigned seq;
    if(serve(b, 1, path, position_ms, title, artist, album, &seq)) return -1;
    b->load_armed = 1; b->load_attempt = attempt; b->load_seq = seq;
    return 0;
}
int cb_serve_load_next(cb *b, const char *path, long position_ms, const char *title, const char *artist, const char *album, unsigned *load_seq){
    unsigned seq;
    if(!load_seq || serve(b, 0, path, position_ms, title, artist, album, &seq)) return -1;
    *load_seq = seq;
    return 0;
}
int cb_stop(cb *b, uint32_t attempt){
    cs_snapshot s0, s1;
    if(!b->sess || !b->ops) return -1;
    b->stop_armed = 0;
    b->ops->sess_snap(b->sess, &s0);
    if(b->ops->sess_stop(b->sess) < 0) return -1;
    b->ops->sess_snap(b->sess, &s1);
    if(s1.stop_seq == s0.stop_seq && !(s0.stop_state == CS_JOB_PENDING && s1.stop_state == CS_JOB_PENDING)) return -1;   /* a STOP already in flight is the one awaited */
    b->stop_armed = 1; b->stop_attempt = attempt; b->stop_seq = s1.stop_seq;
    return 0;
}
void cb_revoke(cb *b){ if(b->http) cast_http_revoke(b->http); }   /* a failed revoke leaves the service refusing every request */
void cb_release(cb *b){
    b->load_armed = b->stop_armed = 0;
    cast_art_clear();
    if(b->http){ b->ops->http_stop(b->http); b->http = NULL; }
}
int cb_events(cb *b, const cs_snapshot *s, uint32_t attempt, ho_evtype out[CB_EV_MAX]){
    int n = 0;
    if(s->state == CS_TAKEN_OVER) out[n++] = HO_EV_TAKEOVER;
    else if(s->state == CS_LOST || s->state == CS_FAILED || s->state == CS_CLOSED) out[n++] = HO_EV_REMOTE_LOST;
    else if(s->state == CS_READY) out[n++] = HO_EV_REMOTE_READY;
    if(b->load_armed && attempt == b->load_attempt && s->load_seq == b->load_seq && (s->load_state == CS_JOB_OK || s->load_state == CS_JOB_FAILED)){
        b->load_armed = 0; out[n++] = s->load_state == CS_JOB_OK ? HO_EV_LOAD_OK : HO_EV_LOAD_FAILED;
    }
    if(s->state == CS_READY && s->media_id) out[n++] = HO_EV_MEDIA;
    if(b->stop_armed && attempt == b->stop_attempt && s->stop_seq == b->stop_seq && (s->stop_state == CS_JOB_OK || s->stop_state == CS_JOB_FAILED)){
        b->stop_armed = 0; out[n++] = s->stop_state == CS_JOB_OK ? HO_EV_REMOTE_STOPPED : HO_EV_REMOTE_LOST;
    }
    return n;
}
