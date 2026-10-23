/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_REMOTE_H
#define DISKOS_AIRPLAY_REMOTE_H
#include "airplay_ipc.h"
#include <sys/types.h>
#include <netinet/in.h>
typedef struct { int fd; size_t used, reply_at; int answered; int64_t deadline; char request[2048]; } aps_remote_client;
typedef struct {
    int listener; pid_t publisher; struct in_addr peer;
    char id[17], token[11]; aps_remote_client clients[2];
    aps_message commands[8]; unsigned n;
} aps_remote;
void aps_remote_init(aps_remote *);
int aps_remote_start(aps_remote *, const char *local_ipv4, const char *peer_ipv4);
int aps_remote_service(aps_remote *);
int aps_remote_take(aps_remote *, aps_message *);
void aps_remote_close(aps_remote *);
/* Pure authenticated DACP request parser. Token is never logged. No prefix
 * matching, duplicate token headers, request body or pipelined extra requests. */
int aps_remote_parse(const char *request, size_t, const char *token, aps_message *);
#endif
