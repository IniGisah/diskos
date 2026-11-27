/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "bt_avrcp.h"
#include "bt_avrcp_proto.h"
#include "screens.h"
#include "config.h"
#include "power.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>

#ifndef AVRCP_SOCK_PATH
#define AVRCP_SOCK_PATH "/tmp/.diskos-avrcp.sock"
#endif

static int g_sock_fd = -1;
static uint64_t g_last_connect_try = 0;
static int g_last_synced_playing = -1;
static char g_last_synced_title[256] = "";
static char g_last_synced_artist[160] = "";
static char g_last_synced_album[160] = "";
static avrcp_linebuf_t g_rx_lb;

static uint64_t get_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int avrcp_allowed(void) {
    if (ui_get_source_mode() == 2) return 0;
    return bt_radio_cached() || cfg_get_int("bt_on", 0);
}

static void avrcp_reset_sync_cache(void) {
    g_last_synced_playing = -1;
    g_last_synced_title[0] = '\0';
    g_last_synced_artist[0] = '\0';
    g_last_synced_album[0] = '\0';
}

static void avrcp_close_sock(void) {
    if (g_sock_fd >= 0) {
        close(g_sock_fd);
        g_sock_fd = -1;
    }
    avrcp_linebuf_init(&g_rx_lb);
    avrcp_reset_sync_cache();
}

void bt_avrcp_suspend(void) {
    if (g_sock_fd >= 0 && (ui_get_source_mode() == 2 || access("/tmp/.diskos_bt_rx", F_OK) == 0)) {
        static const char msg[] = "SUSPEND\n";
        ssize_t w = write(g_sock_fd, msg, sizeof(msg) - 1);
        (void)w;
    }
    avrcp_close_sock();
}

static int proc_running_by_name(const char *target_name) {
    DIR *dir = opendir("/proc");
    if (!dir) return 0;
    struct dirent *de;
    int found = 0;
    while ((de = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[300], comm[64];
        snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof(comm), f)) {
            char *nl = strpbrk(comm, "\r\n");
            if (nl) *nl = '\0';
            if (strcmp(comm, target_name) == 0) {
                found = 1;
                fclose(f);
                break;
            }
        }
        fclose(f);
    }
    closedir(dir);
    return found;
}

static void spawn_avrcpd(void) {
    if (!avrcp_allowed()) return;
    if (proc_running_by_name("diskos-avrcpd")) return;

    const char *bin = NULL;
    if (access("/opt/diskos/bin/diskos-avrcpd", X_OK) == 0) bin = "/opt/diskos/bin/diskos-avrcpd";
    else if (access("/usr/data/diskos-avrcpd", X_OK) == 0) bin = "/usr/data/diskos-avrcpd";
    if (!bin) return;

    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            for (int fd = 3; fd < 256; fd++) close(fd);
            int in_fd = open("/dev/null", O_RDONLY);
            if (in_fd >= 0) { dup2(in_fd, STDIN_FILENO); if (in_fd > 2) close(in_fd); }
            int log_fd = open("/tmp/avrcpd.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (log_fd < 0) log_fd = open("/dev/null", O_WRONLY);
            if (log_fd >= 0) {
                dup2(log_fd, STDOUT_FILENO);
                dup2(log_fd, STDERR_FILENO);
                if (log_fd > 2) close(log_fd);
            }
            setpgid(0, 0);
            execl(bin, bin, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) {
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    }
}

static void try_connect(void) {
    if (!avrcp_allowed()) {
        bt_avrcp_suspend();
        return;
    }
    uint64_t now = get_now_ms();
    if (now - g_last_connect_try < 3000) return;
    g_last_connect_try = now;

    avrcp_close_sock();

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", AVRCP_SOCK_PATH);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            spawn_avrcpd();
            return;
        }
    }

    g_sock_fd = fd;
    avrcp_linebuf_init(&g_rx_lb);
    avrcp_reset_sync_cache();
}

void bt_avrcp_init(void) {
    if (!avrcp_allowed()) {
        bt_avrcp_suspend();
        return;
    }
    spawn_avrcpd();
    try_connect();
}

int bt_avrcp_fd(void) {
    if (!avrcp_allowed()) {
        bt_avrcp_suspend();
        return -1;
    }
    if (g_sock_fd < 0) try_connect();
    return g_sock_fd;
}

static void avrcp_handle_line(const char *line, void *ctx) {
    (void)ctx;
    if (!avrcp_allowed()) return;
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
}

void bt_avrcp_poll(void) {
    if (!avrcp_allowed()) {
        bt_avrcp_suspend();
        return;
    }
    if (g_sock_fd < 0) {
        try_connect();
        return;
    }

    for (;;) {
        char buf[256];
        ssize_t n = read(g_sock_fd, buf, sizeof(buf));
        if (n > 0) {
            avrcp_linebuf_feed(&g_rx_lb, buf, (size_t)n, avrcp_handle_line, NULL);
            if (!avrcp_allowed() || g_sock_fd < 0) return;
            continue;
        }
        if (n == 0) {
            avrcp_close_sock();
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        avrcp_close_sock();
        return;
    }
}

static int write_all_nonblock(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return (off == 0) ? 1 : -1;
        return -1;
    }
    return 0;
}

void bt_avrcp_sync_state(int playing, const char *title, const char *artist, const char *album) {
    if (!avrcp_allowed()) {
        bt_avrcp_suspend();
        return;
    }
    if (g_sock_fd < 0) {
        try_connect();
        if (g_sock_fd < 0) return;
    }

    char clean_title[256], clean_artist[160], clean_album[160];
    avrcp_sanitize_field(clean_title, sizeof(clean_title), title);
    avrcp_sanitize_field(clean_artist, sizeof(clean_artist), artist);
    avrcp_sanitize_field(clean_album, sizeof(clean_album), album);

    int playing_changed = (playing != g_last_synced_playing);
    int title_changed   = (strcmp(clean_title, g_last_synced_title) != 0);
    int artist_changed  = (strcmp(clean_artist, g_last_synced_artist) != 0);
    int album_changed   = (strcmp(clean_album, g_last_synced_album) != 0);

    if (!playing_changed && !title_changed && !artist_changed && !album_changed) return;

    char msg[1024];
    int len = snprintf(msg, sizeof(msg),
                       "MODE:TX\n"
                       "STATUS:%s\n"
                       "TITLE:%s\n"
                       "ARTIST:%s\n"
                       "ALBUM:%s\n",
                       playing ? "Playing" : "Paused",
                       clean_title,
                       clean_artist,
                       clean_album);
    if (len <= 0 || (size_t)len >= sizeof(msg)) return;

    int wrc = write_all_nonblock(g_sock_fd, msg, (size_t)len);
    if (wrc == 1) return; /* EAGAIN before any byte written; retry on next tick */
    if (wrc < 0) {
        avrcp_close_sock();
        return;
    }

    g_last_synced_playing = playing;
    snprintf(g_last_synced_title, sizeof(g_last_synced_title), "%s", clean_title);
    snprintf(g_last_synced_artist, sizeof(g_last_synced_artist), "%s", clean_artist);
    snprintf(g_last_synced_album, sizeof(g_last_synced_album), "%s", clean_album);
}

#ifdef DISKOS_HOST_TEST
void bt_avrcp_test_attach_fd(int fd) {
    avrcp_close_sock();
    g_sock_fd = fd;
    avrcp_linebuf_init(&g_rx_lb);
    avrcp_reset_sync_cache();
}
int bt_avrcp_test_get_fd(void) { return g_sock_fd; }
#endif
