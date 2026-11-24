/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "bt_avrcp.h"
#include "screens.h"
#include "power.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#define AVRCP_SOCK_PATH "/tmp/.diskos-avrcp.sock"

static int g_sock_fd = -1;
static uint64_t g_last_connect_try = 0;
static int g_last_synced_playing = -1;
static char g_last_synced_title[256] = "";

static uint64_t get_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void spawn_avrcpd(void) {
    system("pidof diskos-avrcpd >/dev/null 2>&1 || ("
           "  if [ -x /opt/diskos/bin/diskos-avrcpd ]; then /opt/diskos/bin/diskos-avrcpd >/tmp/avrcpd.log 2>&1 & "
           "  elif [ -x /usr/data/diskos-avrcpd ]; then /usr/data/diskos-avrcpd >/tmp/avrcpd.log 2>&1 & fi"
           ")");
}

static void try_connect(void) {
    uint64_t now = get_now_ms();
    if (now - g_last_connect_try < 3000) return;
    g_last_connect_try = now;

    if (g_sock_fd >= 0) close(g_sock_fd);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, AVRCP_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            spawn_avrcpd();
            return;
        }
    }

    g_sock_fd = fd;
    g_last_synced_playing = -1; /* force sync on reconnect */
}

void bt_avrcp_init(void) {
    spawn_avrcpd();
    try_connect();
}

int bt_avrcp_fd(void) {
    if (g_sock_fd < 0) try_connect();
    return g_sock_fd;
}

void bt_avrcp_poll(void) {
    if (g_sock_fd < 0) {
        try_connect();
        return;
    }

    char buf[256];
    ssize_t n = read(g_sock_fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return;
        }
        /* Socket closed or error */
        close(g_sock_fd);
        g_sock_fd = -1;
        return;
    }

    buf[n] = '\0';
    char *line = strtok(buf, "\r\n");
    while (line) {
        if (!strcmp(line, "TOGGLE")) {
            power_note_input();
            ui_transport_command("0201000C0000");
        } else if (!strcmp(line, "PLAY")) {
            power_note_input();
            if (!ui_is_playing()) ui_transport_command("0201000C0000");
        } else if (!strcmp(line, "PAUSE")) {
            power_note_input();
            if (ui_is_playing()) ui_transport_command("0201000C0000");
        } else if (!strcmp(line, "NEXT")) {
            power_note_input();
            ui_transport_command("0201000C0001");
        } else if (!strcmp(line, "PREV")) {
            power_note_input();
            ui_transport_command("0201000C0002");
        }
        line = strtok(NULL, "\r\n");
    }
}

void bt_avrcp_sync_state(int playing, const char *title, const char *artist, const char *album) {
    if (g_sock_fd < 0) {
        try_connect();
        if (g_sock_fd < 0) return;
    }

    int playing_changed = (playing != g_last_synced_playing);
    int title_changed = (title && strcmp(title, g_last_synced_title) != 0);

    if (!playing_changed && !title_changed) return;

    char msg[1024];
    int len = snprintf(msg, sizeof(msg),
                       "STATUS:%s\n"
                       "TITLE:%s\n"
                       "ARTIST:%s\n"
                       "ALBUM:%s\n",
                       playing ? "Playing" : "Paused",
                       title ? title : "",
                       artist ? artist : "",
                       album ? album : "");

    ssize_t w = write(g_sock_fd, msg, len);
    if (w <= 0) {
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        close(g_sock_fd);
        g_sock_fd = -1;
        return;
    }

    g_last_synced_playing = playing;
    if (title) strncpy(g_last_synced_title, title, sizeof(g_last_synced_title) - 1);
}
