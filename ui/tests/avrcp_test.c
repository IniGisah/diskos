/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Integration test for ui/bt_avrcp.c <-> src/avrcp/diskos_avrcpd.c over real
 * Unix socketpair + dbus-1 MPRIS Metadata Get/GetAll handler checks. */

#define _GNU_SOURCE
#define AVRCP_RX_MARKER_PATH "/tmp/diskos_test_rx_marker"
#define DISKOS_AVRCPD_TEST 1

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- Stubs required by ui/bt_avrcp.c ---- */
static int g_stub_source_mode = 0;
static int g_stub_bt_cached = 1;
static int g_stub_playing = 0;
static int g_stub_power_notes = 0;
static char g_stub_ui_cmds[16][32];
static int g_stub_ui_ncmds = 0;

int ui_get_source_mode(void) { return g_stub_source_mode; }
int bt_radio_cached(void) { return g_stub_bt_cached; }
int cfg_get_int(const char *k, int def) { (void)k; return def; }
int ui_is_playing(void) { return g_stub_playing; }
void power_note_input(void) { g_stub_power_notes++; }
void ui_transport_command(const char *cmd) {
    if (g_stub_ui_ncmds < 16) snprintf(g_stub_ui_cmds[g_stub_ui_ncmds++], 32, "%s", cmd);
}

/* Include daemon implementation (guards main() under DISKOS_AVRCPD_TEST) */
#include "../../src/avrcp/diskos_avrcpd.c"

/* Test API declared in ui/bt_avrcp.c under DISKOS_HOST_TEST */
void bt_avrcp_sync_state(int playing, const char *title, const char *artist, const char *album);
void bt_avrcp_poll(void);
void bt_avrcp_suspend(void);
void bt_avrcp_test_attach_fd(int fd);
int  bt_avrcp_test_get_fd(void);

static void set_nonblock(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    if (f >= 0) fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static void drain_daemon_1byte(void) {
    /* Read currently pending socket bytes 1 byte at a time via handle_client_data! */
    set_nonblock(g_client_fd);
    handle_client_data(NULL);
}

static void test_socketpair_sync_and_commands(void) {
    unlink(AVRCP_RX_MARKER_PATH);
    g_stub_source_mode = 0;
    g_stub_bt_cached = 1;
    g_rx_suppressed = 0;
    g_test_mqueue_sent = 0;

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    set_nonblock(sv[0]);
    set_nonblock(sv[1]);

    bt_avrcp_test_attach_fd(sv[0]);
    g_client_fd = sv[1];
    avrcp_linebuf_init(&g_client_lb);

    /* Initial sync sets TX mode, Playing, title, artist, album */
    bt_avrcp_sync_state(1, "Song One\nRemix", "Artist A", "Album X");
    drain_daemon_1byte();
    assert(strcmp(g_playback_status, "Playing") == 0);
    assert(strcmp(g_track_title, "Song One Remix") == 0);
    assert(strcmp(g_track_artist, "Artist A") == 0);
    assert(strcmp(g_track_album, "Album X") == 0);

    /* Changing ONLY artist triggers metadata update */
    bt_avrcp_sync_state(1, "Song One\nRemix", "Artist B", "Album X");
    drain_daemon_1byte();
    assert(strcmp(g_track_artist, "Artist B") == 0);

    /* Changing ONLY album triggers metadata update */
    bt_avrcp_sync_state(1, "Song One\nRemix", "Artist B", "Album Y");
    drain_daemon_1byte();
    assert(strcmp(g_track_album, "Album Y") == 0);

    /* Daemon -> UI split writes & oversized junk resync */
    g_stub_playing = 1;
    g_stub_ui_ncmds = 0;
    assert(write(sv[1], "PA", 2) == 2);
    bt_avrcp_poll();
    assert(g_stub_ui_ncmds == 0); /* incomplete line buffered cleanly */
    assert(write(sv[1], "USE\r\nNEX", 8) == 8);
    bt_avrcp_poll();
    assert(g_stub_ui_ncmds == 1 && strcmp(g_stub_ui_cmds[0], "0201000C0000") == 0);
    assert(write(sv[1], "T\n", 2) == 2);
    bt_avrcp_poll();
    assert(g_stub_ui_ncmds == 2 && strcmp(g_stub_ui_cmds[1], "0201000C0001") == 0);

    char junk[AVRCP_LINEBUF_CAP + 64];
    memset(junk, 'Z', sizeof(junk));
    /* Write oversized junk (non-blocking loop) + \nPREV\n */
    size_t off = 0;
    while (off < sizeof(junk)) {
        ssize_t w = write(sv[1], junk + off, sizeof(junk) - off);
        if (w > 0) off += (size_t)w;
        else bt_avrcp_poll();
    }
    assert(write(sv[1], "\nPREV\n", 6) == 6);
    bt_avrcp_poll();
    assert(g_stub_ui_ncmds == 3 && strcmp(g_stub_ui_cmds[2], "0201000C0002") == 0);

    /* Mode 2 suspend suppresses both client writes and /player mqueue fallback */
    g_stub_source_mode = 2;
    bt_avrcp_sync_state(1, "Should Not Send", "Artist B", "Album Y");
    drain_daemon_1byte();
    assert(bt_avrcp_test_get_fd() == -1);
    assert(g_rx_suppressed == 1);
    assert(strcmp(g_track_title, "Song One Remix") == 0);

    /* Disconnect client so forward_cmd uses fallback path; must remain suppressed! */
    if (g_client_fd >= 0) { close(g_client_fd); g_client_fd = -1; }
    forward_cmd("PAUSE", "0201000C0000");
    assert(g_test_mqueue_sent == 0);

    /* Marker file gate suppresses fallback even when g_rx_suppressed == 0 */
    g_rx_suppressed = 0;
    int mfd = open(AVRCP_RX_MARKER_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(mfd >= 0);
    close(mfd);
    forward_cmd("TOGGLE", "0201000C0000");
    assert(g_test_mqueue_sent == 0);

    unlink(AVRCP_RX_MARKER_PATH);
    forward_cmd("NEXT", "0201000C0001");
    assert(g_test_mqueue_sent == 1 && strcmp(g_test_last_mq_cmd, "0201000C0001") == 0);
}

static void test_dbus_metadata_serialization(void) {
    snprintf(g_playback_status, sizeof(g_playback_status), "Playing");
    snprintf(g_track_title, sizeof(g_track_title), "D-Bus Title");
    snprintf(g_track_artist, sizeof(g_track_artist), "D-Bus Artist");
    snprintf(g_track_album, sizeof(g_track_album), "D-Bus Album");

    DBusMessage *req = dbus_message_new_method_call("org.test", PLAYER_PATH, PROPS_IFACE, "Get");
    assert(req != NULL);
    dbus_message_set_serial(req, 1);
    DBusMessage *msg = dbus_message_new_method_return(req);
    assert(msg != NULL);

    DBusMessageIter iter;
    dbus_message_iter_init_append(msg, &iter);
    append_metadata_variant(&iter);

    /* Parse back the serialized variant to confirm xesam:title/artist/album exist */
    DBusMessageIter riter, var, arr, entry;
    assert(dbus_message_iter_init(msg, &riter));
    assert(dbus_message_iter_get_arg_type(&riter) == DBUS_TYPE_VARIANT);
    dbus_message_iter_recurse(&riter, &var);
    assert(dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY);
    dbus_message_iter_recurse(&var, &arr);

    int saw_title = 0, saw_artist = 0, saw_album = 0;
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        dbus_message_iter_recurse(&arr, &entry);
        const char *k = NULL;
        dbus_message_iter_get_basic(&entry, &k);
        if (k && !strcmp(k, "xesam:title")) saw_title = 1;
        if (k && !strcmp(k, "xesam:artist")) saw_artist = 1;
        if (k && !strcmp(k, "xesam:album")) saw_album = 1;
        dbus_message_iter_next(&arr);
    }
    assert(saw_title && saw_artist && saw_album);
    dbus_message_unref(msg);
    dbus_message_unref(req);
}

int main(void) {
    test_socketpair_sync_and_commands();
    test_dbus_metadata_serialization();
    printf("avrcp_test: all assertions passed\n");
    return 0;
}
