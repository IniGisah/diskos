/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_CAST_HTTP_H
#define DISKOS_CAST_HTTP_H
#include <stdint.h>
#define CH_ID_LEN 17     /* object id: 16 hex chars + NUL */
#define CH_TOKEN_LEN 33  /* session token: 32 hex chars (128 random bits) + NUL */
#define CH_OBJECTS 4     /* current track + a few retained for transitions */
typedef struct cast_http cast_http;
/* Cast media server: authorization, file admission, HEAD and byte ranges. One worker thread, nonblocking, bounded clients/headers/idle.
 * Listens ONLY on the given unicast IPv4 address (no wildcard) on a private port. The
 * device build accepts only the address currently assigned to wlan0 (so no USB gadget
 * or loopback listener can be created); the host test build accepts 127.0.0.1 only.
 * Requests must carry exactly "HTTP/1.0" or "HTTP/1.1" (anything else is 400), are
 * terminated by CRLF CRLF (an LF-only request is never parsed: no answer, closed at the
 * idle timeout or 431 at the header limit) and only the first request on a connection is
 * served (Connection: close; pipelined bytes are discarded).
 * Serves GET /<token>/<object-id> for approved objects: the library-relative path is
 * opened only beneath sd_root with directory descriptors, never following symlinks,
 * and every open file holds an sd_io lease (HEAD releases it before answering: it only fstats).
 * GET and HEAD send Accept-Ranges: bytes. One "bytes=a-b", "bytes=a-" or "bytes=-n" gets 206 with
 * Content-Range and an exact Content-Length (64-bit offsets, end clamped to EOF, a suffix longer than the
 * file is the whole file); an unsatisfiable one (start at or past EOF, "-0", any range on an empty file) gets
 * 416 with the unsatisfied-range form of Content-Range (bytes, a star, a slash, the size).
 * A Range that is malformed, reversed, of another unit, given more than once or listing several ranges is ignored: 200 with the whole file, never multipart. A file that
 * shrinks during a transfer ends the response early (the connection is closed, never padded).
 * Returns NULL if the address is bad, /dev/urandom fails, or a socket cannot be set up. */
cast_http *cast_http_start(const char *ipv4, const char *sd_root);
uint16_t cast_http_port(const cast_http *);
/* Copies the current session token. */
int cast_http_token(cast_http *, char out[CH_TOKEN_LEN]);
/* Approve rel (library-relative: no leading '/', no empty, "." or ".." parts) with a
 * Content-Type of [A-Za-z0-9.+/-]. Same rel+type returns the existing id. When all
 * CH_OBJECTS slots are used the oldest is retired (running transfers continue).
 * 0 on success. -1 on a bad argument or an entropy failure; after an entropy failure the
 * service stays alive (existing objects and the token are unchanged, nothing is added).
 * Only revoke() turns an entropy failure into a dead service. */
int cast_http_approve(cast_http *, const char *rel, const char *ctype, char id[CH_ID_LEN]);
/* Approve the cover image of the cast_art generation gen (nonzero): GET /<token>/<id> then answers 200 image/png (always the whole image,
 * a Range is ignored; HEAD gives the length), 404 when that generation is no longer current or the cover cannot be encoded. One image at
 * a time: a new generation replaces the previous one's id, the same generation returns the same id. Generation 0 retires the image, and a
 * failed replacement also retires it (a transfer already running for a retired generation is closed). 0 on success. */
int cast_http_approve_art(cast_http *, unsigned gen, char id[CH_ID_LEN]);
/* New token, all objects forgotten, every client closed. -1 (and every later
 * request is refused 503) if entropy fails. The old token never works again. */
int cast_http_revoke(cast_http *);
/* Closes every client and the listener, joins the worker, frees.
 * Threading contract: token(), approve() and revoke() may be called from any thread while
 * the worker runs and may overlap each other. stop() must be externally exclusive: no
 * API call may overlap it or follow it (a concurrent revoke() could be left waiting for
 * the worker and then touch freed memory). */
void cast_http_stop(cast_http *);
#endif
