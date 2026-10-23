/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cast_http.h"
#include "cast_art.h"
#include "artcache.h"
#include "sdio.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef CH_IDLE_MS
#define CH_IDLE_MS 20000 /* a client that moves no byte for this long is closed */
#endif
#ifndef CH_ART_WAIT_MS
#define CH_ART_WAIT_MS 15000 /* one pending image request; never delays audio */
#endif
#define CH_CLIENTS 4     /* bounded connections; one more gets 503 */
#define CH_HDR 2048      /* request bytes; longer gets 431 */
#define CH_CHUNK 16384   /* file bytes in flight per client */
#define CH_REL 512
#define CH_TICK_MS 250   /* poll cap: admission, idle and stop are re-checked at least this often */
#ifdef CH_TEST           /* host tests only; never defined by the device build */
const char *cast_http_test_urandom;
int (*cast_http_test_ioctl)(int, unsigned long, void *); /* host seam: stands in for the wlan0 address query */
#define URANDOM (cast_http_test_urandom ? cast_http_test_urandom : "/dev/urandom")
#define CH_LOOPBACK_OK 1
#else
#define URANDOM "/dev/urandom"
#define CH_LOOPBACK_OK 0
#endif
typedef struct {
    int fd, file, lease, ph; /* ph: 0 free, 1 request, 2 answer, 3 waiting for cover */
    int64_t left, pos, t; size_t in, ol, oo; /* left/pos: body bytes still to send / next file offset */
    unsigned char *mem; /* the cover PNG being sent (malloc, owned); then the body comes from here, not from file */
    cast_http *art_http; unsigned art_gen, cache_gen;
    int art_head, art_tried; int64_t art_deadline, art_next;
    char rq[CH_HDR], out[CH_CHUNK];
} cl;
struct cast_http {
    int lfd, wake[2]; uint16_t port; char root[256]; pthread_t th;
    pthread_mutex_t mu; pthread_cond_t cv; /* guards token, obj, dead, flush */
    char token[CH_TOKEN_LEN]; int dead, flush; unsigned seq;
    char art_id[CH_ID_LEN]; unsigned art_gen; /* the one cover image object (cast_art generation); 0 = none */
    struct { char id[CH_ID_LEN], rel[CH_REL], ct[64]; unsigned seq; } obj[CH_OBJECTS];
    _Atomic int stop; cl c[CH_CLIENTS];
};
static int64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static int rand_hex(char *out, int nbytes)
{
    unsigned char b[16]; int f = open(URANDOM, O_RDONLY | O_CLOEXEC), got = 0;
    if (f < 0) return -1;
    while (got < nbytes) { ssize_t n = read(f, b + got, nbytes - got); if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; } got += n; }
    close(f);
    if (got != nbytes) return -1;
    for (int i = 0; i < nbytes; i++) sprintf(out + 2 * i, "%02x", b[i]);
    return 0;
}
/* Library-relative path: no leading '/', no empty, "." or ".." component, no control byte. */
static int rel_ok(const char *r)
{
    size_t n = strlen(r), s = 0;
    if (!n || n >= CH_REL) return 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && ((unsigned char)r[i] < 0x20 || r[i] == 0x7f)) return 0;
        if (i == n || r[i] == '/') {
            size_t k = i - s;
            if (!k || (k == 1 && r[s] == '.') || (k == 2 && r[s] == '.' && r[s + 1] == '.')) return 0;
            s = i + 1;
        }
    }
    return 1;
}
/* Opens rel only beneath root: one openat per component, no symlink at any step, regular file only.
 * Every descriptor but the returned leaf is closed here; the caller holds the sd_io lease. */
static int open_rel(const char *root, const char *rel)
{
    char p[CH_REL]; struct stat a, b; int d = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (d < 0 || !rel_ok(rel)) { if (d >= 0) close(d); return -1; }
    snprintf(p, sizeof p, "%s", rel);
    for (char *q = p;;) {
        char *s = strchr(q, '/');
        if (s) {
            *s = 0; int n = openat(d, q, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            close(d); if (n < 0) return -1;
            d = n; q = s + 1; continue;
        }
        if (fstatat(d, q, &a, AT_SYMLINK_NOFOLLOW) || !S_ISREG(a.st_mode)) { close(d); return -1; }
        int f = openat(d, q, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC); /* NONBLOCK: never hang on a swapped-in FIFO */
        close(d);
        if (f < 0) return -1;
        if (fstat(f, &b) || !S_ISREG(b.st_mode) || a.st_ino != b.st_ino || a.st_dev != b.st_dev) { close(f); return -1; }
        return f;
    }
}
static void drop(cl *c)
{
    if (c->file >= 0) close(c->file);
    free(c->mem);
    if (c->lease) sd_io_end(); /* before the socket closes: a client that saw EOF can rely on the lease being gone */
    if (c->fd >= 0) { /* unread input would turn close into RST; the drain is bounded so a flooding peer cannot hold the worker */
        char b[512];
        for (size_t drained = 0; drained < CH_HDR;) {
            ssize_t n = recv(c->fd, b, sizeof b, MSG_DONTWAIT);
            if (n <= 0) break;
            drained += (size_t)n;
        }
        close(c->fd);
    }
    memset(c, 0, sizeof *c); c->fd = c->file = -1;
}
static const char *reason(int code)
{
    switch (code) { case 200: return "OK"; case 206: return "Partial Content"; case 204: return "No Content"; case 400: return "Bad Request"; case 401: return "Unauthorized";
    case 403: return "Forbidden"; case 404: return "Not Found"; case 405: return "Method Not Allowed";
    case 416: return "Range Not Satisfiable"; case 431: return "Request Header Fields Too Large"; default: return "Service Unavailable"; }
}
/* Header block into c->out. Bodyless answers carry no detail at all. cr: Content-Range value or NULL. */
static void answer_x(cl *c, int code, const char *ct, int64_t len, const char *cr)
{
    int ok = code == 200 || code == 206 || code == 416;
    int n = snprintf(c->out, sizeof c->out, "HTTP/1.1 %d %s\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Length: %lld\r\n",
                     code, reason(code), (long long)len);
    if (code == 401) n += snprintf(c->out + n, sizeof c->out - n, "WWW-Authenticate: Bearer\r\n");
    if (code == 405) n += snprintf(c->out + n, sizeof c->out - n, "Allow: GET, HEAD, OPTIONS\r\n");
    if (ok || code == 204) n += snprintf(c->out + n, sizeof c->out - n, "Access-Control-Allow-Origin: *\r\n");
    if (code == 204) n += snprintf(c->out + n, sizeof c->out - n, "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\nAccess-Control-Allow-Headers: Range\r\nAccess-Control-Max-Age: 600\r\n");
    if (ok) n += snprintf(c->out + n, sizeof c->out - n, "Accept-Ranges: bytes\r\n");
    if (cr) n += snprintf(c->out + n, sizeof c->out - n, "Content-Range: %s\r\n", cr);
    if (ct) n += snprintf(c->out + n, sizeof c->out - n, "Content-Type: %s\r\n", ct);
    n += snprintf(c->out + n, sizeof c->out - n, "\r\n");
    c->ol = n; c->oo = 0; c->ph = 2; c->left = 0;
}
static void answer(cl *c, int code, const char *ct, int64_t len) { answer_x(c, code, ct, len, 0); }
/* Count named headers case-insensitively; optionally copy the first value.
 * A value too long for v counts as two, so the range is ignored. */
static int range_find(const char *p, const char *end, const char *name, char v[96])
{
    int cnt = 0;
    size_t name_len = strlen(name);
    while (p < end) {
        const char *e = memmem(p, end - p, "\r\n", 2), *col;
        if (!e) e = end;
        col = memchr(p, ':', e - p);
        if (col && (size_t)(col - p) == name_len && !strncasecmp(p, name, name_len)) {
            const char *s = col + 1, *t = e;
            while (s < t && (*s == ' ' || *s == '\t')) s++;
            while (t > s && (t[-1] == ' ' || t[-1] == '\t')) t--;
            if (cnt++ == 0 && v) { if (t - s >= 96) cnt = 2; else { memcpy(v, s, t - s); v[t - s] = 0; } }
        }
        p = e < end ? e + 2 : end;
    }
    return cnt;
}
static int num(const char **s, int64_t *out) /* digits only; a value beyond int64 saturates (then it is simply past EOF) */
{
    const char *p = *s; int64_t v = 0;
    if (!isdigit((unsigned char)*p)) return 0;
    for (; isdigit((unsigned char)*p); p++) { int d = *p - '0'; v = v > (INT64_MAX - d) / 10 ? INT64_MAX : v * 10 + d; }
    *s = p; *out = v; return 1;
}
/* 0: serve the whole file with 200 (no Range, another unit, malformed, reversed, several ranges: a server may
 * ignore a Range it does not honour, and an ignored range never breaks a receiver; multipart is never built).
 * 1: one satisfiable range, clamped to [*a, *b]. 2: unsatisfiable (416). */
static int range_eval(const char *v, int64_t size, int64_t *a, int64_t *b)
{
    int64_t x = 0, y = 0; const char *p = v + 6;
    if (strncasecmp(v, "bytes=", 6)) return 0;
    int hx = num(&p, &x), hy;
    if (*p++ != '-') return 0;
    hy = num(&p, &y);
    if (*p || (!hx && !hy) || (hx && hy && y < x)) return 0;
    if (!hx) { if (!y || !size) return 2; *a = y >= size ? 0 : size - y; *b = size - 1; return 1; } /* suffix */
    if (x >= size) return 2;
    *a = x; *b = hy && y < size - 1 ? y : size - 1; return 1;
}
/* Keep the original GET/HEAD pending instead of returning a cache-miss 404.
 * No socket writes are requested while waiting. The normal poll loop still
 * serves audio, handles revocation and checks stop. No decoder is started. */
static void art_poll(cl *c){
    cast_http *h = c->art_http;
    pthread_mutex_lock(&h->mu);
    int stale = h->dead || h->flush || h->art_gen != c->art_gen;
    pthread_mutex_unlock(&h->mu);
    if(stale || atomic_load(&h->stop) || !sd_io_allowed()){ drop(c); return; }
    int64_t now = now_ms();
    if(now >= c->art_deadline){ answer(c, 404, 0, 0); return; }
    if(now < c->art_next) return;
    c->art_next = now + CH_TICK_MS;
    unsigned gen = artcache_gen();
    if(c->art_tried && gen == c->cache_gen) return;
    c->cache_gen = gen; c->art_tried = 1;
    unsigned char *png; size_t n;
    if(cast_art_fetch(c->art_gen, &png, &n)) return;
    answer(c, 200, "image/png", (int64_t)n);
    if(c->art_head) free(png);
    else { c->mem = png; c->left = (int64_t)n; c->pos = 0; }
    /* pump() rechecks the generation under mu before sending any bytes. */
}
static void request(cast_http *h, cl *c)
{
    char *e = memchr(c->rq, '\r', c->in), *tm = memmem(c->rq, c->in, "\r\n\r\n", 4);
    if (memchr(c->rq, 0, c->in) || !e || e + 1 >= c->rq + c->in || e[1] != '\n') { answer(c, 400, 0, 0); return; }
    *e = 0;
    char *t = strchr(c->rq, ' '), *v = t ? strchr(t + 1, ' ') : 0;
    if (!v || (strcmp(v + 1, "HTTP/1.0") &&
               strcmp(v + 1, "HTTP/1.1"))) {
        answer(c, 400, 0, 0);
        return;
    }
    *t++ = 0; *v = 0;
    int get = !strcmp(c->rq, "GET"), head = !strcmp(c->rq, "HEAD");
    if (!get && !head && strcmp(c->rq, "OPTIONS")) { answer(c, 405, 0, 0); return; }
    char tok[CH_TOKEN_LEN], rel[CH_REL], ct[64]; int found = 0, dead; unsigned ag = 0;
    pthread_mutex_lock(&h->mu); dead = h->dead; memcpy(tok, h->token, sizeof tok);
    const char *id = strlen(t) > 33 ? t + 34 : "";
    for (int i = 0; i < CH_OBJECTS && !dead; i++)
        if (h->obj[i].id[0] && !strcmp(h->obj[i].id, id)) { found = 1; strcpy(rel, h->obj[i].rel); strcpy(ct, h->obj[i].ct); }
    if (!found && !dead && h->art_gen && !strcmp(h->art_id, id)) { found = 1; ag = h->art_gen; strcpy(ct, "image/png"); }
    pthread_mutex_unlock(&h->mu);
    if (dead) { answer(c, 503, 0, 0); return; }
    size_t seg = t[0] == '/' ? strcspn(t + 1, "/?") : 0; /* first segment: the token, compared whole and in constant time */
    if (seg != CH_TOKEN_LEN - 1) { answer(c, 401, 0, 0); return; }
    unsigned diff = 0;
    for (int i = 0; i < CH_TOKEN_LEN - 1; i++) diff |= (unsigned char)(t[1 + i] ^ tok[i]);
    if (diff) { answer(c, 403, 0, 0); return; }
    if (t[33] != '/' || strlen(id) != CH_ID_LEN - 1 || !found) { answer(c, 404, 0, 0); return; }
    if (!get && !head) { answer(c, 204, 0, 0); return; }
    if (ag) { /* the cover: always the whole image (a Range is ignored), encoded here on the worker, never on the LVGL thread */
        c->art_http = h; c->art_gen = ag; c->art_head = head;
        c->ph = 3; c->art_deadline = now_ms() + CH_ART_WAIT_MS;
        art_poll(c);
        return;
    }
    if (!sd_io_begin()) { answer(c, 503, 0, 0); return; } /* admission closed: refuse */
    struct stat st; int f = open_rel(h->root, rel);
    if (f < 0 || fstat(f, &st)) { if (f >= 0) close(f); sd_io_end(); answer(c, 404, 0, 0); return; }
    c->file = f; c->lease = 1;
    int64_t size = st.st_size, a = 0, b = size - 1; char rv[96], cr[80]; int rc = 0;
    if (get && tm > e &&
        range_find(e + 2, tm + 2, "If-Range", NULL) == 0 &&
        range_find(e + 2, tm + 2, "Range", rv) == 1)
        rc = range_eval(rv, size, &a, &b);
    if (rc == 2) { snprintf(cr, sizeof cr, "bytes */%lld", (long long)size); answer_x(c, 416, 0, 0, cr); }
    else if (rc == 1) { snprintf(cr, sizeof cr, "bytes %lld-%lld/%lld", (long long)a, (long long)b, (long long)size); answer_x(c, 206, ct, b - a + 1, cr); }
    else answer_x(c, 200, ct, size, 0);
    if (rc != 2 && !head) { c->left = b - a + 1; c->pos = a; }
    else { close(c->file); c->file = -1; sd_io_end(); c->lease = 0; } /* no body: nothing was read beyond fstat */
}
/* One send then, when the buffer is empty, one refill. Done or failed: drop the client (always Connection: close). */
static void pump_bytes(cl *c)
{
    if (c->oo < c->ol) {
        ssize_t n = send(c->fd, c->out + c->oo, c->ol - c->oo, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) { if (errno != EAGAIN && errno != EINTR) drop(c); return; }
        c->oo += n; c->t = now_ms();
    }
    if (c->oo < c->ol) return;
    if ((c->file < 0 && !c->mem) || c->left <= 0) { drop(c); return; }
    size_t want = c->left < CH_CHUNK ? (size_t)c->left : CH_CHUNK;
    ssize_t r = c->mem ? (memcpy(c->out, c->mem + c->pos, want), (ssize_t)want) : pread(c->file, c->out, want, (off_t)c->pos);
    if (r < 0 && errno == EINTR) return;
    if (r <= 0) { drop(c); return; } /* file shrank or read error: end the transfer rather than lie */
    c->ol = r; c->oo = 0; c->left -= r; c->pos += r;
}
static void pump(cl *c)
{
    if (!c->art_gen) { pump_bytes(c); return; }
    cast_http *h = c->art_http;
    pthread_mutex_lock(&h->mu);
    if (h->dead || h->flush || c->art_gen != h->art_gen) drop(c);
    else pump_bytes(c); /* memory only; send is nonblocking */
    pthread_mutex_unlock(&h->mu);
}
static void feed(cast_http *h, cl *c)
{
    ssize_t n = recv(c->fd, c->rq + c->in, CH_HDR - c->in, MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { drop(c); return; }
    if (n < 0) return;
    c->in += n; c->t = now_ms();
    if (memmem(c->rq, c->in, "\r\n\r\n", 4)) request(h, c);
    else if (c->in == CH_HDR) answer(c, 431, 0, 0);
}
static void *worker(void *arg)
{
    cast_http *h = arg;
    while (!atomic_load(&h->stop)) {
        pthread_mutex_lock(&h->mu);
        if (h->flush) { for (int i = 0; i < CH_CLIENTS; i++) if (h->c[i].fd >= 0) drop(&h->c[i]); h->flush = 0; pthread_cond_broadcast(&h->cv); }
        pthread_mutex_unlock(&h->mu);
        struct pollfd p[2 + CH_CLIENTS]; int map[CH_CLIENTS], np = 2;
        p[0] = (struct pollfd){h->wake[0], POLLIN, 0}; p[1] = (struct pollfd){h->lfd, POLLIN, 0};
        int allowed = sd_io_allowed(); int64_t t = now_ms();
        for (int i = 0; i < CH_CLIENTS; i++) {
            cl *c = &h->c[i];
            if (c->fd < 0) continue;
            if ((c->lease && !allowed) || t - c->t > CH_IDLE_MS) { drop(c); continue; }
            if(c->ph == 3) art_poll(c);
            if(c->fd < 0) continue;
            map[np - 2] = i; p[np++] = (struct pollfd){c->fd,
                c->ph == 1 ? POLLIN : c->ph == 2 ? POLLOUT : 0, 0};
        }
        if (poll(p, np, CH_TICK_MS) < 0 && errno != EINTR) continue;
        if (p[0].revents) { char b[16]; while (read(h->wake[0], b, sizeof b) > 0) {} }
        for (int k = 2; k < np; k++) {
            cl *c = &h->c[map[k - 2]];
            if ((p[k].revents & (POLLERR | POLLNVAL)) ||
                (c->ph == 3 && (p[k].revents & POLLHUP))) drop(c);
            else if (c->ph == 1 && (p[k].revents & (POLLIN | POLLHUP))) feed(h, c);
            else if (c->ph == 2 && (p[k].revents & (POLLOUT | POLLHUP))) pump(c);
        }
        for (int accepted = 0, fd;
             accepted < CH_CLIENTS &&
             !atomic_load(&h->stop) &&
             (p[1].revents & POLLIN) &&
             (fd = accept4(h->lfd, 0, 0,
                           SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0;
             ++accepted) {
            int i = 0;
            while (i < CH_CLIENTS && h->c[i].fd >= 0) i++;
            if (i == CH_CLIENTS) {
                /* Pending artwork must not prevent a new audio connection. */
                for (int j = 0; j < CH_CLIENTS; j++) {
                    if (h->c[j].ph != 3) continue;
                    drop(&h->c[j]); i = j; break;
                }
            }
            if (i == CH_CLIENTS) { static const char r[] = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"; (void)!send(fd, r, sizeof r - 1, MSG_NOSIGNAL | MSG_DONTWAIT); close(fd); continue; }
            h->c[i].fd = fd; h->c[i].ph = 1; h->c[i].t = now_ms();
        }
    }
    return 0;
}
cast_http *cast_http_start(const char *ipv4, const char *sd_root)
{
    struct sockaddr_in a = {.sin_family = AF_INET}; socklen_t sl = sizeof a;
    if (!ipv4 || !sd_root || strlen(sd_root) >= sizeof(((cast_http *)0)->root) || inet_pton(AF_INET, ipv4, &a.sin_addr) != 1) return 0;
    uint32_t ip = ntohl(a.sin_addr.s_addr), top = ip >> 24;
    if (!ip || top >= 224 || (top == 127 && !CH_LOOPBACK_OK)) return 0; /* no wildcard, multicast or (device build) loopback */
    cast_http *h = calloc(1, sizeof *h);
    if (!h) return 0;
    h->lfd = h->wake[0] = h->wake[1] = -1;
    for (int i = 0; i < CH_CLIENTS; i++) h->c[i].fd = h->c[i].file = -1;
    snprintf(h->root, sizeof h->root, "%s", sd_root);
    pthread_mutex_init(&h->mu, 0); pthread_cond_init(&h->cv, 0);
    if (rand_hex(h->token, 16) || pipe2(h->wake, O_NONBLOCK | O_CLOEXEC)) goto bad;
    h->lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (h->lfd < 0) goto bad;
#ifdef CH_TEST
    if (!cast_http_test_ioctl) { if (ip != INADDR_LOOPBACK) goto bad; } else /* host only: exercise the wlan0 check below with an injected result */
#endif
    {
        struct ifreq ifr = {0};
        strcpy(ifr.ifr_name, "wlan0");
#ifdef CH_TEST
        if (cast_http_test_ioctl(h->lfd, SIOCGIFADDR, &ifr) ||
#else
        if (ioctl(h->lfd, SIOCGIFADDR, &ifr) ||
#endif
            ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr !=
                a.sin_addr.s_addr) goto bad;
    }
    if (bind(h->lfd, (struct sockaddr *)&a, sizeof a) ||
        listen(h->lfd, CH_CLIENTS) ||
        getsockname(h->lfd, (struct sockaddr *)&a, &sl)) goto bad;
    h->port = ntohs(a.sin_port);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 128 * 1024);
    int r = pthread_create(&h->th, &at, worker, h);
    pthread_attr_destroy(&at);
    if (!r) return h;
bad:
    if (h->lfd >= 0) close(h->lfd);
    for (int i = 0; i < 2; i++) if (h->wake[i] >= 0) close(h->wake[i]);
    free(h);
    return 0;
}
uint16_t cast_http_port(const cast_http *h) { return h->port; }
int cast_http_token(cast_http *h, char out[CH_TOKEN_LEN])
{
    pthread_mutex_lock(&h->mu); int dead = h->dead; if (!dead) memcpy(out, h->token, CH_TOKEN_LEN); pthread_mutex_unlock(&h->mu);
    return dead ? -1 : 0;
}
int cast_http_approve(cast_http *h, const char *rel, const char *ct, char id[CH_ID_LEN])
{
    if (!rel || !ct || !rel_ok(rel) || !*ct || strlen(ct) >= sizeof h->obj[0].ct || strspn(ct, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.+/-") != strlen(ct)) return -1;
    char nid[CH_ID_LEN]; int r = -1, v = 0;
    pthread_mutex_lock(&h->mu);
    if (!h->dead) {
        for (int i = 0; i < CH_OBJECTS; i++)
            if (h->obj[i].id[0] && !strcmp(h->obj[i].rel, rel) && !strcmp(h->obj[i].ct, ct)) { strcpy(id, h->obj[i].id); r = 0; }
        if (r) {
            for (int i = 0; i < CH_OBJECTS; i++) { /* a free slot, else the oldest */
                if (!h->obj[i].id[0]) { v = i; break; }
                if (h->obj[i].seq < h->obj[v].seq) v = i;
            }
            if (!rand_hex(nid, 8)) {
                strcpy(h->obj[v].id, nid); strcpy(h->obj[v].rel, rel); strcpy(h->obj[v].ct, ct); h->obj[v].seq = ++h->seq;
                strcpy(id, nid); r = 0;
            }
        }
    }
    pthread_mutex_unlock(&h->mu);
    return r;
}
int cast_http_approve_art(cast_http *h, unsigned gen, char id[CH_ID_LEN])
{
    char nid[CH_ID_LEN]; int r = -1;
    pthread_mutex_lock(&h->mu);
    if (!h->dead) {
        if (gen && h->art_gen == gen) { strcpy(id, h->art_id); r = 0; }
        else {
            h->art_id[0] = 0; h->art_gen = 0;
            if (gen && !rand_hex(nid, 8)) {
                strcpy(h->art_id, nid); h->art_gen = gen; strcpy(id, nid); r = 0;
            }
        }
    }
    pthread_mutex_unlock(&h->mu);
    return r;
}
int cast_http_revoke(cast_http *h)
{
    char t[CH_TOKEN_LEN]; int r = rand_hex(t, 16);
    pthread_mutex_lock(&h->mu);
    memset(h->obj, 0, sizeof h->obj);
    memset(h->art_id, 0, sizeof h->art_id); h->art_gen = 0;
    if (r || !memcmp(t, h->token, sizeof t)) { h->dead = 1; memset(h->token, 0, sizeof h->token); r = -1; } else memcpy(h->token, t, sizeof t);
    h->flush = 1; (void)!write(h->wake[1], "x", 1);
    while (h->flush) pthread_cond_wait(&h->cv, &h->mu); /* every client is closed when this returns */
    pthread_mutex_unlock(&h->mu);
    return r;
}
void cast_http_stop(cast_http *h)
{
    if (!h) return;
    atomic_store(&h->stop, 1); (void)!write(h->wake[1], "x", 1);
    pthread_join(h->th, 0);
    for (int i = 0; i < CH_CLIENTS; i++) if (h->c[i].fd >= 0) drop(&h->c[i]);
    close(h->lfd); close(h->wake[0]); close(h->wake[1]);
    free(h);
}
