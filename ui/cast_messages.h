/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_CAST_MESSAGES_H
#define DISKOS_CAST_MESSAGES_H
#include <stddef.h>
#include <stdint.h>
/* Byte bounds exclude NUL. No truncation; absent/null fields have no presence bit.
 * JSON <=16383 bytes, 512 tokens, depth <=16, decoded strings <=1023 bytes,
 * keys <=127, numbers <=63 bytes. Up to four applications/media entries. */
#define CM_TEXT 1024
#define CM_ID 128
#define CM_ENTRIES 4
#define CM_JSON 16384
#define CM_APP "CC1AD845"
typedef enum { CM_CONNECT, CM_CLOSE, CM_PING, CM_PONG, CM_GET_STATUS,
    CM_LAUNCH, CM_LOAD, CM_PLAY, CM_PAUSE, CM_SEEK, CM_STOP, CM_SET_VOLUME } cm_kind;
typedef struct {
    cm_kind kind; int receiver, request_id, media_session_id, muted;
    double time, level;
    const char *sender_name; /* copied active display identity; CONNECT only */
    const char *content_id, *content_type, *stream_type, *title, *artist, *album, *image;
} cm_command;
typedef struct { unsigned present; double level; int muted; } cm_volume;
enum { CM_LEVEL=1, CM_MUTED=2, CM_MEDIA_ID=4, CM_TIME=8 };
typedef struct { char app_id[CM_ID], session_id[CM_ID], transport_id[CM_ID], display_name[CM_ID]; int idle; } cm_app; /* idle: isIdleScreen:true (e.g. Backdrop) */
typedef struct {
    unsigned present; int media_session_id; double current_time;
    char player_state[CM_ID], idle_reason[CM_ID], content_id[CM_TEXT]; cm_volume volume;
} cm_media;
typedef struct {
    char type[CM_ID]; int request_id, has_status, has_apps; size_t app_count, media_count; /* presence of non-null status/applications */
    cm_app apps[CM_ENTRIES]; cm_media media[CM_ENTRIES]; cm_volume volume;
} cm_status;
/* Failure leaves the caller's output unchanged, for both encode and parse. */
/* 1 success, 0 invalid/oversize. Output only usable on success. caller-owned buffers.
 * Positive request/media IDs <=INT32_MAX; unsolicited/absent request ID is zero.
 * Time is finite 0..INT32_MAX seconds; volume finite 0..1; muted exactly 0/1.
 * LOAD uses autoplay:false, music metadata and zero/one image. No allocation/I/O. */
int cm_encode(char *, size_t, const cm_command *);
const char *cm_namespace(const cm_command *);
/* Validated top-level decoded type, bounded by CM_ID; failure leaves out unchanged. */
int cm_type(const char *, size_t, char *out, size_t cap);
/* Receiver-namespace STOP of one application session (not the media-session CM_STOP). request_id >0;
 * session_id non-empty, <CM_ID bytes. Same failure rule as cm_encode. */
int cm_encode_stop(char *, size_t, int request_id, const char *session_id);
/* expected_request >=0: accept exact match OR unsolicited status (zero), never
 * unsolicited errors. All entries retained; caller must validate ownership even
 * on unsolicited status. No selection of a convenient first entry. */
int cm_parse(const char *, size_t, int expected_request, cm_status *);
/* Pure identity predicate for slice 4: generation and every supplied identity
 * must match exactly. NULL app/content or zero media ID skips that check. */
int cm_identity(uint64_t generation, uint64_t expected_generation,
    const cm_app *, const cm_media *, const cm_app *expected_app,
    int expected_media_id, const char *owned_content_id);
#endif
