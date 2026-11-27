/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* diskos-avrcpd: Lightweight D-Bus MPRIS Media Player 2 daemon for BlueZ AVRCP.
 * Bridges remote Bluetooth AVRCP controls (e.g. TWS earbud taps, car stereo buttons)
 * to diskOS UI and player transport commands, and publishes playback state and metadata. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <mqueue.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <time.h>
#include <dbus/dbus.h>
#include "../../ui/bt_avrcp_proto.h"

#ifndef SOCKET_PATH
#define SOCKET_PATH "/tmp/.diskos-avrcp.sock"
#endif
#ifndef AVRCP_RX_MARKER_PATH
#define AVRCP_RX_MARKER_PATH "/tmp/.diskos_bt_rx"
#endif

#define PLAYER_PATH "/org/mpris/MediaPlayer2"
#define HCI0_PATH   "/org/bluez/hci0"
#define MPRIS_IFACE "org.mpris.MediaPlayer2"
#define PLAYER_IFACE "org.mpris.MediaPlayer2.Player"
#define PROPS_IFACE "org.freedesktop.DBus.Properties"

static volatile sig_atomic_t g_running = 1;
static DBusConnection *g_conn = NULL;
static int g_registered = 0;
static int g_server_fd = -1;
static int g_client_fd = -1;
static int g_rx_suppressed = 0;
static avrcp_linebuf_t g_client_lb;

/* Playback State */
static char g_playback_status[32] = "Paused";
static char g_track_title[256] = "diskOS Track";
static char g_track_artist[256] = "diskOS Artist";
static char g_track_album[256] = "diskOS Album";

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int rx_mode_active(void) {
    return g_rx_suppressed || access(AVRCP_RX_MARKER_PATH, F_OK) == 0;
}

/* Direct fallback to /player message queue if mq_ui is not connected (suppressed in RX mode) */
#ifndef DISKOS_AVRCPD_TEST
static int send_player_cmd_mqueue(const char *cmd) {
    mqd_t q = mq_open("/player", O_WRONLY | O_NONBLOCK);
    if (q == (mqd_t)-1) {
        return -1;
    }
    int rc = mq_send(q, cmd, strlen(cmd), 0);
    mq_close(q);
    return rc;
}
#else
static int g_test_mqueue_sent = 0;
static char g_test_last_mq_cmd[32] = "";
static int send_player_cmd_mqueue(const char *cmd) {
    g_test_mqueue_sent++;
    snprintf(g_test_last_mq_cmd, sizeof(g_test_last_mq_cmd), "%s", cmd ? cmd : "");
    return 0;
}
#endif

static void forward_cmd(const char *cmd_name, const char *mq_cmd) {
    if (rx_mode_active()) {
        fprintf(stderr, "[diskos-avrcp] Suppressed AVRCP cmd in RX mode: %s\n", cmd_name);
        return;
    }
    if (g_client_fd >= 0) {
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "%s\n", cmd_name);
        ssize_t w = write(g_client_fd, buf, (size_t)n);
        if (w == n) {
            fprintf(stderr, "[diskos-avrcp] Forwarded to mq_ui: %s\n", cmd_name);
            return;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return;
        }
        /* Write failed or partial, client disconnected */
        close(g_client_fd);
        g_client_fd = -1;
        avrcp_linebuf_init(&g_client_lb);
    }
    /* Fallback directly to /player */
    if (mq_cmd && !rx_mode_active()) {
        fprintf(stderr, "[diskos-avrcp] Fallback mqueue cmd: %s\n", mq_cmd);
        send_player_cmd_mqueue(mq_cmd);
    }
}

static void append_metadata_variant(DBusMessageIter *parent_iter) {
    DBusMessageIter val, meta_dict, m_entry, m_val_iter, arr_iter;
    dbus_message_iter_open_container(parent_iter, DBUS_TYPE_VARIANT, "a{sv}", &val);
    dbus_message_iter_open_container(&val, DBUS_TYPE_ARRAY, "{sv}", &meta_dict);

    /* mpris:trackid */
    const char *m_key = "mpris:trackid";
    const char *m_val = "/org/diskos/track/0";
    dbus_message_iter_open_container(&meta_dict, DBUS_TYPE_DICT_ENTRY, NULL, &m_entry);
    dbus_message_iter_append_basic(&m_entry, DBUS_TYPE_STRING, &m_key);
    dbus_message_iter_open_container(&m_entry, DBUS_TYPE_VARIANT, "o", &m_val_iter);
    dbus_message_iter_append_basic(&m_val_iter, DBUS_TYPE_OBJECT_PATH, &m_val);
    dbus_message_iter_close_container(&m_entry, &m_val_iter);
    dbus_message_iter_close_container(&meta_dict, &m_entry);

    /* xesam:title */
    m_key = "xesam:title";
    m_val = g_track_title;
    dbus_message_iter_open_container(&meta_dict, DBUS_TYPE_DICT_ENTRY, NULL, &m_entry);
    dbus_message_iter_append_basic(&m_entry, DBUS_TYPE_STRING, &m_key);
    dbus_message_iter_open_container(&m_entry, DBUS_TYPE_VARIANT, "s", &m_val_iter);
    dbus_message_iter_append_basic(&m_val_iter, DBUS_TYPE_STRING, &m_val);
    dbus_message_iter_close_container(&m_entry, &m_val_iter);
    dbus_message_iter_close_container(&meta_dict, &m_entry);

    /* xesam:artist */
    m_key = "xesam:artist";
    const char *artist_arr[] = { g_track_artist };
    dbus_message_iter_open_container(&meta_dict, DBUS_TYPE_DICT_ENTRY, NULL, &m_entry);
    dbus_message_iter_append_basic(&m_entry, DBUS_TYPE_STRING, &m_key);
    dbus_message_iter_open_container(&m_entry, DBUS_TYPE_VARIANT, "as", &m_val_iter);
    dbus_message_iter_open_container(&m_val_iter, DBUS_TYPE_ARRAY, "s", &arr_iter);
    dbus_message_iter_append_basic(&arr_iter, DBUS_TYPE_STRING, &artist_arr[0]);
    dbus_message_iter_close_container(&m_val_iter, &arr_iter);
    dbus_message_iter_close_container(&m_entry, &m_val_iter);
    dbus_message_iter_close_container(&meta_dict, &m_entry);

    /* xesam:album */
    m_key = "xesam:album";
    m_val = g_track_album;
    dbus_message_iter_open_container(&meta_dict, DBUS_TYPE_DICT_ENTRY, NULL, &m_entry);
    dbus_message_iter_append_basic(&m_entry, DBUS_TYPE_STRING, &m_key);
    dbus_message_iter_open_container(&m_entry, DBUS_TYPE_VARIANT, "s", &m_val_iter);
    dbus_message_iter_append_basic(&m_val_iter, DBUS_TYPE_STRING, &m_val);
    dbus_message_iter_close_container(&m_entry, &m_val_iter);
    dbus_message_iter_close_container(&meta_dict, &m_entry);

    dbus_message_iter_close_container(&val, &meta_dict);
    dbus_message_iter_close_container(parent_iter, &val);
}

static void append_dict_metadata(DBusMessageIter *dict) {
    const char *key = "Metadata";
    DBusMessageIter entry;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    append_metadata_variant(&entry);
    dbus_message_iter_close_container(dict, &entry);
}

/* Emit D-Bus PropertiesChanged signal for org.mpris.MediaPlayer2.Player */
static void emit_properties_changed(DBusConnection *conn, int changed_status, int changed_meta) {
    if (!conn || !g_registered) return;

    DBusMessage *sig = dbus_message_new_signal(
        PLAYER_PATH,
        PROPS_IFACE,
        "PropertiesChanged"
    );
    if (!sig) return;

    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(sig, &iter);

    const char *iface = PLAYER_IFACE;
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

    if (changed_status) {
        const char *key = "PlaybackStatus";
        const char *val_str = g_playback_status;
        DBusMessageIter entry, val;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &val);
        dbus_message_iter_append_basic(&val, DBUS_TYPE_STRING, &val_str);
        dbus_message_iter_close_container(&entry, &val);
        dbus_message_iter_close_container(&dict, &entry);
    }

    if (changed_meta) {
        append_dict_metadata(&dict);
    }

    dbus_message_iter_close_container(&iter, &dict);

    DBusMessageIter inval_arr;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &inval_arr);
    dbus_message_iter_close_container(&iter, &inval_arr);

    dbus_connection_send(conn, sig, NULL);
    dbus_message_unref(sig);
    dbus_connection_flush(conn);
}

static void append_dict_string(DBusMessageIter *dict, const char *key, const char *val_str) {
    DBusMessageIter entry, val;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &val);
    dbus_message_iter_append_basic(&val, DBUS_TYPE_STRING, &val_str);
    dbus_message_iter_close_container(&entry, &val);
    dbus_message_iter_close_container(dict, &entry);
}

static void append_dict_bool(DBusMessageIter *dict, const char *key, dbus_bool_t b_val) {
    DBusMessageIter entry, val;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &val);
    dbus_message_iter_append_basic(&val, DBUS_TYPE_BOOLEAN, &b_val);
    dbus_message_iter_close_container(&entry, &val);
    dbus_message_iter_close_container(dict, &entry);
}

static DBusHandlerResult handle_get_all(DBusConnection *conn, DBusMessage *msg) {
    const char *iface = NULL;
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID)) {
        dbus_error_free(&err);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;

    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

    if (!strcmp(iface, MPRIS_IFACE) || !strcmp(iface, "")) {
        append_dict_string(&dict, "Identity", "diskOS");
        append_dict_bool(&dict, "CanQuit", FALSE);
        append_dict_bool(&dict, "CanRaise", FALSE);
        append_dict_bool(&dict, "HasTrackList", FALSE);
    }

    if (!strcmp(iface, PLAYER_IFACE) || !strcmp(iface, "")) {
        append_dict_string(&dict, "PlaybackStatus", g_playback_status);
        append_dict_metadata(&dict);
        append_dict_bool(&dict, "CanControl", TRUE);
        append_dict_bool(&dict, "CanPlay", TRUE);
        append_dict_bool(&dict, "CanPause", TRUE);
        append_dict_bool(&dict, "CanGoNext", TRUE);
        append_dict_bool(&dict, "CanGoPrevious", TRUE);
    }

    dbus_message_iter_close_container(&iter, &dict);
    if (conn) dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_get(DBusConnection *conn, DBusMessage *msg) {
    const char *iface = NULL, *prop = NULL;
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID)) {
        dbus_error_free(&err);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;

    DBusMessageIter iter, val;
    dbus_message_iter_init_append(reply, &iter);

    int match_player = (!iface || !iface[0] || !strcmp(iface, PLAYER_IFACE));
    int match_mpris  = (!iface || !iface[0] || !strcmp(iface, MPRIS_IFACE));

    if (match_player && !strcmp(prop, "PlaybackStatus")) {
        const char *v = g_playback_status;
        dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "s", &val);
        dbus_message_iter_append_basic(&val, DBUS_TYPE_STRING, &v);
        dbus_message_iter_close_container(&iter, &val);
    } else if (match_player && !strcmp(prop, "Metadata")) {
        append_metadata_variant(&iter);
    } else if (match_mpris && !strcmp(prop, "Identity")) {
        const char *v = "diskOS";
        dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "s", &val);
        dbus_message_iter_append_basic(&val, DBUS_TYPE_STRING, &v);
        dbus_message_iter_close_container(&iter, &val);
    } else if (match_mpris && (!strcmp(prop, "CanQuit") || !strcmp(prop, "CanRaise") || !strcmp(prop, "HasTrackList"))) {
        dbus_bool_t b = FALSE;
        dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "b", &val);
        dbus_message_iter_append_basic(&val, DBUS_TYPE_BOOLEAN, &b);
        dbus_message_iter_close_container(&iter, &val);
    } else if (match_player && (!strcmp(prop, "CanControl") || !strcmp(prop, "CanPlay") ||
               !strcmp(prop, "CanPause") || !strcmp(prop, "CanGoNext") ||
               !strcmp(prop, "CanGoPrevious"))) {
        dbus_bool_t b = TRUE;
        dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "b", &val);
        dbus_message_iter_append_basic(&val, DBUS_TYPE_BOOLEAN, &b);
        dbus_message_iter_close_container(&iter, &val);
    } else {
        dbus_message_unref(reply);
        DBusMessage *err_reply = dbus_message_new_error(msg, "org.freedesktop.DBus.Error.UnknownProperty", "Unknown property");
        if (err_reply) {
            if (conn) dbus_connection_send(conn, err_reply, NULL);
            dbus_message_unref(err_reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (conn) dbus_connection_send(conn, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult player_filter(DBusConnection *conn, DBusMessage *msg, void *user_data) {
    (void)user_data;
    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);

    if (!member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        const char *xml =
            "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
            "<node>\n"
            "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
            "    <method name=\"Introspect\">\n"
            "      <arg name=\"data\" direction=\"out\" type=\"s\"/>\n"
            "    </method>\n"
            "  </interface>\n"
            "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
            "    <method name=\"Get\">\n"
            "      <arg name=\"interface\" direction=\"in\" type=\"s\"/>\n"
            "      <arg name=\"property\" direction=\"in\" type=\"s\"/>\n"
            "      <arg name=\"value\" direction=\"out\" type=\"v\"/>\n"
            "    </method>\n"
            "    <method name=\"GetAll\">\n"
            "      <arg name=\"interface\" direction=\"in\" type=\"s\"/>\n"
            "      <arg name=\"properties\" direction=\"out\" type=\"a{sv}\"/>\n"
            "    </method>\n"
            "    <signal name=\"PropertiesChanged\">\n"
            "      <arg name=\"interface\" type=\"s\"/>\n"
            "      <arg name=\"changed_properties\" type=\"a{sv}\"/>\n"
            "      <arg name=\"invalidated_properties\" type=\"as\"/>\n"
            "    </signal>\n"
            "  </interface>\n"
            "  <interface name=\"org.mpris.MediaPlayer2\">\n"
            "    <property name=\"CanQuit\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanRaise\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"HasTrackList\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"Identity\" type=\"s\" access=\"read\"/>\n"
            "  </interface>\n"
            "  <interface name=\"org.mpris.MediaPlayer2.Player\">\n"
            "    <method name=\"Next\"/>\n"
            "    <method name=\"Previous\"/>\n"
            "    <method name=\"Pause\"/>\n"
            "    <method name=\"PlayPause\"/>\n"
            "    <method name=\"Stop\"/>\n"
            "    <method name=\"Play\"/>\n"
            "    <property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/>\n"
            "    <property name=\"CanControl\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanPlay\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanPause\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanGoNext\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/>\n"
            "  </interface>\n"
            "</node>\n";
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
            dbus_connection_send(conn, reply, NULL);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, PROPS_IFACE, "GetAll")) {
        return handle_get_all(conn, msg);
    }

    if (dbus_message_is_method_call(msg, PROPS_IFACE, "Get")) {
        return handle_get(conn, msg);
    }

    if (iface && !strcmp(iface, PLAYER_IFACE)) {
        fprintf(stderr, "[diskos-avrcp] Received method: %s\n", member);
        if (!strcmp(member, "Play")) {
            forward_cmd("PLAY", "0201000C0000");
        } else if (!strcmp(member, "Pause") || !strcmp(member, "Stop")) {
            forward_cmd("PAUSE", "0201000C0000");
        } else if (!strcmp(member, "PlayPause")) {
            forward_cmd("TOGGLE", "0201000C0000");
        } else if (!strcmp(member, "Next")) {
            forward_cmd("NEXT", "0201000C0001");
        } else if (!strcmp(member, "Previous")) {
            forward_cmd("PREV", "0201000C0002");
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(conn, reply, NULL);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int hci0_adapter_exists(DBusConnection *conn) {
    if (!conn) return 0;
    DBusError err;
    dbus_error_init(&err);

    DBusMessage *msg = dbus_message_new_method_call(
        "org.bluez",
        HCI0_PATH,
        PROPS_IFACE,
        "Get"
    );
    if (!msg) return 0;

    const char *iface = "org.bluez.Adapter1";
    const char *prop = "Address";
    dbus_message_append_args(msg,
        DBUS_TYPE_STRING, &iface,
        DBUS_TYPE_STRING, &prop,
        DBUS_TYPE_INVALID);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        dbus_error_free(&err);
        return 0;
    }
    int ok = (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN);
    dbus_message_unref(reply);
    return ok;
}

static int try_register_player(DBusConnection *conn) {
    if (!conn) return -1;

    if (!hci0_adapter_exists(conn)) {
        g_registered = 0;
        return -1;
    }

    DBusError err;
    dbus_error_init(&err);

    const char *player_path = PLAYER_PATH;
    DBusMessage *msg = dbus_message_new_method_call(
        "org.bluez",
        HCI0_PATH,
        "org.bluez.Media1",
        "RegisterPlayer"
    );
    if (!msg) return -1;

    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &player_path);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

    append_dict_string(&dict, "PlaybackStatus", g_playback_status);
    append_dict_metadata(&dict);
    append_dict_bool(&dict, "CanControl", TRUE);
    append_dict_bool(&dict, "CanPlay", TRUE);
    append_dict_bool(&dict, "CanPause", TRUE);
    append_dict_bool(&dict, "CanGoNext", TRUE);
    append_dict_bool(&dict, "CanGoPrevious", TRUE);

    dbus_message_iter_close_container(&iter, &dict);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 2000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        fprintf(stderr, "[diskos-avrcp] RegisterPlayer failed: %s: %s\n",
                err.name ? err.name : "", err.message ? err.message : "");
        dbus_error_free(&err);
        g_registered = 0;
        return -1;
    }

    dbus_message_unref(reply);
    fprintf(stderr, "[diskos-avrcp] RegisterPlayer SUCCESS on %s\n", HCI0_PATH);
    g_registered = 1;
    emit_properties_changed(conn, 1, 1);
    return 0;
}

static DBusHandlerResult signal_filter(DBusConnection *conn, DBusMessage *msg, void *user_data) {
    (void)user_data;
    if (dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name,
                                  DBUS_TYPE_STRING, &old_owner,
                                  DBUS_TYPE_STRING, &new_owner,
                                  DBUS_TYPE_INVALID)) {
            if (name && !strcmp(name, "org.bluez")) {
                fprintf(stderr, "[diskos-avrcp] BlueZ owner changed: %s -> %s\n",
                        old_owner ? old_owner : "", new_owner ? new_owner : "");
                if (new_owner && new_owner[0] != '\0') {
                    try_register_player(conn);
                } else {
                    g_registered = 0;
                }
            }
        }
    } else if (dbus_message_is_signal(msg, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded")) {
        const char *obj_path = NULL;
        DBusMessageIter iter;
        if (dbus_message_iter_init(msg, &iter) &&
            dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_get_basic(&iter, &obj_path);
            if (obj_path && !strcmp(obj_path, HCI0_PATH)) {
                fprintf(stderr, "[diskos-avrcp] hci0 InterfacesAdded -> registering player\n");
                try_register_player(conn);
            }
        }
    } else if (dbus_message_is_signal(msg, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved")) {
        const char *obj_path = NULL;
        DBusMessageIter iter;
        if (dbus_message_iter_init(msg, &iter) &&
            dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_get_basic(&iter, &obj_path);
            if (obj_path && !strcmp(obj_path, HCI0_PATH)) {
                fprintf(stderr, "[diskos-avrcp] hci0 InterfacesRemoved -> marking unregistered\n");
                g_registered = 0;
            }
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* UNIX domain socket server setup */
static int init_server_socket(void) {
    unlink(SOCKET_PATH);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    chmod(SOCKET_PATH, 0666);

    if (listen(fd, 2) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

typedef struct {
    int status_changed;
    int meta_changed;
} client_batch_t;

static void handle_client_line(const char *line, void *ctx) {
    client_batch_t *b = (client_batch_t *)ctx;
    if (!strcmp(line, "SUSPEND") || !strcmp(line, "MODE:RX")) {
        g_rx_suppressed = 1;
        return;
    }
    if (!strcmp(line, "MODE:TX")) {
        g_rx_suppressed = 0;
        return;
    }
    if (!strncmp(line, "STATUS:", 7)) {
        const char *st = line + 7;
        g_rx_suppressed = 0;
        if (strcmp(g_playback_status, st) != 0) {
            snprintf(g_playback_status, sizeof(g_playback_status), "%s", st);
            b->status_changed = 1;
            fprintf(stderr, "[diskos-avrcp] Status updated: %s\n", g_playback_status);
        }
    } else if (!strncmp(line, "TITLE:", 6)) {
        const char *t = line + 6;
        g_rx_suppressed = 0;
        if (strcmp(g_track_title, t) != 0) {
            snprintf(g_track_title, sizeof(g_track_title), "%s", t);
            b->meta_changed = 1;
        }
    } else if (!strncmp(line, "ARTIST:", 7)) {
        const char *a = line + 7;
        g_rx_suppressed = 0;
        if (strcmp(g_track_artist, a) != 0) {
            snprintf(g_track_artist, sizeof(g_track_artist), "%s", a);
            b->meta_changed = 1;
        }
    } else if (!strncmp(line, "ALBUM:", 6)) {
        const char *alb = line + 6;
        g_rx_suppressed = 0;
        if (strcmp(g_track_album, alb) != 0) {
            snprintf(g_track_album, sizeof(g_track_album), "%s", alb);
            b->meta_changed = 1;
        }
    }
}

static void handle_client_data(DBusConnection *conn) {
    client_batch_t batch = {0, 0};
    for (;;) {
        char buf[512];
        ssize_t n = read(g_client_fd, buf, sizeof(buf));
        if (n > 0) {
            avrcp_linebuf_feed(&g_client_lb, buf, (size_t)n, handle_client_line, &batch);
            continue;
        }
        if (n == 0) {
            close(g_client_fd);
            g_client_fd = -1;
            avrcp_linebuf_init(&g_client_lb);
            break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        close(g_client_fd);
        g_client_fd = -1;
        avrcp_linebuf_init(&g_client_lb);
        break;
    }

    if ((batch.status_changed || batch.meta_changed) && conn) {
        emit_properties_changed(conn, batch.status_changed, batch.meta_changed);
    }
}

#ifndef DISKOS_AVRCPD_TEST
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    avrcp_linebuf_init(&g_client_lb);
    fprintf(stderr, "[diskos-avrcp] Starting diskos-avrcpd...\n");

    g_server_fd = init_server_socket();
    if (g_server_fd < 0) {
        fprintf(stderr, "[diskos-avrcp] Failed to init server socket, continuing without IPC\n");
    }

    DBusError err;
    dbus_error_init(&err);

    g_conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    while (!g_conn && g_running) {
        fprintf(stderr, "[diskos-avrcp] Waiting for D-Bus system bus: %s\n",
                err.message ? err.message : "unknown");
        dbus_error_free(&err);
        sleep(2);
        g_conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    }

    if (!g_conn) {
        if (g_server_fd >= 0) {
            close(g_server_fd);
            unlink(SOCKET_PATH);
        }
        return 1;
    }

    dbus_connection_set_exit_on_disconnect(g_conn, FALSE);

    const char *player_path = PLAYER_PATH;
    DBusObjectPathVTable vtable = {
        .message_function = player_filter,
    };

    if (!dbus_connection_register_object_path(g_conn, player_path, &vtable, NULL)) {
        fprintf(stderr, "[diskos-avrcp] Failed to register object path %s\n", player_path);
    }

    dbus_bus_add_match(g_conn,
        "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
        NULL);
    dbus_bus_add_match(g_conn,
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',member='InterfacesAdded'",
        NULL);
    dbus_bus_add_match(g_conn,
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',member='InterfacesRemoved'",
        NULL);
    dbus_connection_add_filter(g_conn, signal_filter, NULL, NULL);

    try_register_player(g_conn);

    uint64_t last_reg_attempt = now_ms();

    while (g_running) {
        while (dbus_connection_get_dispatch_status(g_conn) == DBUS_DISPATCH_DATA_REMAINS) {
            dbus_connection_dispatch(g_conn);
        }

        if (!g_registered && (now_ms() - last_reg_attempt >= 3000)) {
            last_reg_attempt = now_ms();
            try_register_player(g_conn);
        }

        struct pollfd fds[4];
        int nfds = 0;

        int dbus_fd = -1;
        dbus_connection_get_unix_fd(g_conn, &dbus_fd);
        if (dbus_fd >= 0) {
            fds[nfds].fd = dbus_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        if (g_server_fd >= 0) {
            fds[nfds].fd = g_server_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        if (g_client_fd >= 0) {
            fds[nfds].fd = g_client_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        int poll_timeout = g_registered ? 1000 : 2000;
        int ret = poll(fds, nfds, poll_timeout);

        if (ret > 0) {
            for (int i = 0; i < nfds; i++) {
                if (fds[i].revents & (POLLIN | POLLERR | POLLHUP)) {
                    if (fds[i].fd == dbus_fd) {
                        dbus_connection_read_write_dispatch(g_conn, 0);
                    } else if (fds[i].fd == g_server_fd) {
                        int client = accept(g_server_fd, NULL, NULL);
                        if (client >= 0) {
                            if (g_client_fd >= 0) close(g_client_fd);
                            int flags = fcntl(client, F_GETFL, 0);
                            if (flags >= 0) fcntl(client, F_SETFL, flags | O_NONBLOCK);
                            fcntl(client, F_SETFD, FD_CLOEXEC);
                            g_client_fd = client;
                            avrcp_linebuf_init(&g_client_lb);
                            fprintf(stderr, "[diskos-avrcp] Client connected from mq_ui\n");
                        }
                    } else if (fds[i].fd == g_client_fd) {
                        handle_client_data(g_conn);
                    }
                }
            }
        } else if (ret == 0) {
            if (dbus_fd >= 0) {
                dbus_connection_read_write_dispatch(g_conn, 0);
            }
        }
    }

    fprintf(stderr, "[diskos-avrcp] Shutting down...\n");
    if (g_client_fd >= 0) close(g_client_fd);
    if (g_server_fd >= 0) {
        close(g_server_fd);
        unlink(SOCKET_PATH);
    }
    if (g_conn) {
        dbus_connection_unregister_object_path(g_conn, player_path);
        dbus_connection_close(g_conn);
        dbus_connection_unref(g_conn);
    }

    return 0;
}
#endif
