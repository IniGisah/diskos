/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Comprehensive host unit tests for TX logic, bt_prepare, bt_codec_observe,
 * RX logic (connection reconciliation, D-Bus parsing, AVRCP plan, stack heal),
 * and AVRCP line framing. */
#include "bt_tx_logic.h"
#include "bt_rx_logic.h"
#include "bt_prepare.h"
#include "bt_codec_observe.h"
#include "bt_avrcp_proto.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Production helpers implemented in ui/bt.c using bt_tx_logic.h */
int bt_codec_pick(int choice, unsigned avail, int avail_known) {
    return bt_tx_codec_pick(choice, avail, avail_known);
}

int bt_prepare_codec_frame(const char *mac, int choice, char *out, int capacity) {
    return bt_tx_codec_frame_value(mac, choice, out, capacity);
}

/* ---- TX Logic Tests ---- */

static void test_tx_mac_and_info_prop(void) {
    assert(bt_tx_mac_valid("AA:BB:CC:11:22:33") == 1);
    assert(bt_tx_mac_valid("aa:bb:cc:11:22:ff") == 1);
    assert(bt_tx_mac_valid("AA:BB:CC:11:22:3") == 0);
    assert(bt_tx_mac_valid("AA-BB-CC-11-22-33") == 0);
    assert(bt_tx_mac_valid("GG:BB:CC:11:22:33") == 0);
    assert(bt_tx_mac_valid(NULL) == 0);

    /* Device named "Connected: yes" must NOT spoof line-anchored Connected: no */
    const char *info =
        "Device AA:BB:CC:11:22:33 (public)\n"
        "\tName: Evil Connected: yes\n"
        "\tAlias: Fake\n"
        "\tConnected: no\n";
    char val[64] = {0};
    assert(bt_tx_info_prop(info, "Connected:", val, sizeof(val)) == 1);
    assert(strcmp(val, "no") == 0);
}

static void test_tx_codec_frame_and_pick(void) {
    char f[48] = {0};
    assert(bt_tx_codec_frame_value("AA:BB:CC:DD:EE:FF", 4, f, sizeof(f)) == 0);
    assert(strcmp(f, "06b3001D0004AA:BB:CC:DD:EE:FF") == 0);
    /* aptX (5) / aptX-HD (6) must clamp cleanly to SBC (0) for stock 06b3 */
    assert(bt_tx_codec_frame_value("AA:BB:CC:DD:EE:FF", 5, f, sizeof(f)) == 0);
    assert(strcmp(f, "06b3001D0000AA:BB:CC:DD:EE:FF") == 0);
    assert(bt_tx_codec_frame_value("AA:BB:CC:DD:EE:FF", 6, f, sizeof(f)) == 0);
    assert(strcmp(f, "06b3001D0000AA:BB:CC:DD:EE:FF") == 0);
    assert(bt_tx_codec_frame_value("bad", 0, f, sizeof(f)) == -1);

    int known = 0;
    unsigned avail = bt_tx_codec_parse_avail(
        "Available codecs: SBC AAC aptX-HD aptX LDAC\nDelay: 120ms\n", &known);
    assert(known == 1);
    assert((avail & (BTC_AAC | BTC_APTX_HD | BTC_APTX | BTC_LDAC)) ==
           (BTC_AAC | BTC_APTX_HD | BTC_APTX | BTC_LDAC));

    /* Multi-line wrapping before next key */
    avail = bt_tx_codec_parse_avail(
        "Available codecs:\n  SBC\n  AAC\nFormat: S16_LE\n", &known);
    assert(known == 1 && avail == BTC_AAC);

    /* Fallbacks */
    assert(bt_tx_codec_pick(4, BTC_AAC, 1) == 1);      /* LDAC -> AAC */
    assert(bt_tx_codec_pick(4, 0, 1) == 0);            /* LDAC -> SBC */
    assert(bt_tx_codec_pick(6, BTC_APTX, 1) == 5);     /* aptX-HD -> aptX */
    assert(bt_tx_codec_pick(6, BTC_AAC, 1) == 1);      /* aptX-HD -> AAC */
    assert(bt_tx_codec_pick(3, BTC_LDAC, 0) == 0);     /* unknown -> SBC */
}

static int deny_match(const char *mac, void *ctx) {
    const char *blocked = (const char *)ctx;
    return strcasecmp(mac, blocked) != 0;
}

static void test_tx_source_pcms_and_link_check(void) {
    /* Accepts both /a2dpsrc/ and /a2dp-source/, ignores /a2dpsnk/, obeys filter */
    const char *pcms =
        "/org/bluealsa/hci0/dev_11_22_33_44_55_66/a2dpsnk/source\n"
        "/org/bluealsa/hci0/dev_AA_BB_CC_11_22_33/a2dp-source/sink\n"
        "/org/bluealsa/hci0/dev_DD_EE_FF_00_11_22/a2dpsrc/sink\n";
    char first[20] = {0};
    int found = 0, routed_found = 0;
    bt_tx_parse_source_pcms(pcms, "DD:EE:FF:00:11:22", deny_match,
                            "AA:BB:CC:11:22:33", first, sizeof(first),
                            &found, &routed_found);
    assert(found == 1);
    assert(routed_found == 1);
    assert(strcmp(first, "DD:EE:FF:00:11:22") == 0);

    const char *sink_only = "/org/bluealsa/hci0/dev_11_22_33_44_55_66/a2dpsnk/source\n";
    bt_tx_parse_source_pcms(sink_only, "11:22:33:44:55:66", NULL, NULL,
                            first, sizeof(first), &found, &routed_found);
    assert(found == 0 && routed_found == 0 && first[0] == '\0');

    /* Link check combined info + hcitool con */
    const char *disc_buf =
        "Device AA:BB:CC:11:22:33 (public)\n\tConnected: no\n"
        "DISKOS_INFO_DONE\n< ACL AA:BB:CC:11:22:33 handle 12\nDISKOS_HCI_DONE\n";
    assert(bt_tx_parse_link_check(disc_buf, "AA:BB:CC:11:22:33") == 1);

    const char *conn_buf =
        "Device AA:BB:CC:11:22:33 (public)\n\tConnected: yes\n"
        "DISKOS_INFO_DONE\nDISKOS_HCI_DONE\n";
    assert(bt_tx_parse_link_check(conn_buf, "AA:BB:CC:11:22:33") == 0);

    /* Missing markers returns unknown (-1) */
    assert(bt_tx_parse_link_check("partial output", "AA:BB:CC:11:22:33") == -1);
}

static void test_tx_managed_objects(void) {
    const char *objs =
        "  object path \"/org/bluez/hci0/dev_11_22_33_44_55_66\"\n"
        "    string \"org.bluez.Device1\"\n"
        "      dict entry( string \"Address\" variant string \"11:22:33:44:55:66\" )\n"
        "      dict entry( string \"Name\" variant string \"Keychron K2\" )\n"
        "      dict entry( string \"Icon\" variant string \"input-keyboard\" )\n"
        "      dict entry( string \"Connected\" variant boolean false )\n"
        "  object path \"/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF\"\n"
        "    string \"org.bluez.Device1\"\n"
        "      dict entry( string \"Address\" variant string \"AA:BB:CC:DD:EE:FF\" )\n"
        "      dict entry( string \"Name\" variant string \"Sony WH-1000XM5\" )\n"
        "      dict entry( string \"Icon\" variant string \"audio-headphones\" )\n"
        "      dict entry( string \"Connected\" variant boolean true )\n";
    bt_tx_scan_dev_t devs[8];
    int n = bt_tx_parse_managed_objects(objs, devs, 8);
    assert(n == 1);
    assert(strcmp(devs[0].mac, "AA:BB:CC:DD:EE:FF") == 0);
    assert(strcmp(devs[0].name, "Sony WH-1000XM5") == 0);
    assert(devs[0].connected == 1);
}

static void test_tx_row_classification_and_labels(void) {
    const char *mac = "AA:BB:CC:DD:EE:FF";
    /* Active connecting state overrides paired/avail */
    assert(bt_tx_classify_row(mac, 2, mac, 1, "") == BT_ROW_CONNECTING);
    /* Connected + matches routed -> BT_ROW_ROUTED */
    assert(bt_tx_classify_row(mac, 3, "", 0, "aa:bb:cc:dd:ee:ff") == BT_ROW_ROUTED);
    /* Connected + not routed -> BT_ROW_CONNECTED */
    assert(bt_tx_classify_row(mac, 3, "", 0, "11:22:33:44:55:66") == BT_ROW_CONNECTED);
    /* Paired disconnected -> BT_ROW_PAIRED */
    assert(bt_tx_classify_row(mac, 2, "", 0, "") == BT_ROW_PAIRED);
    /* Unpaired -> BT_ROW_AVAIL */
    assert(bt_tx_classify_row(mac, 0, "", 0, "") == BT_ROW_AVAIL);

    assert(strcmp(bt_tx_device_type_label("audio-headphones"), "Headphones") == 0);
    assert(strcmp(bt_tx_device_type_label("audio-speakers"), "Speaker") == 0);
    assert(strcmp(bt_tx_codec_short_name(4), "LDAC (High)") == 0);
    assert(strcmp(bt_tx_codec_short_name(6), "aptX HD") == 0);
}

/* ---- Codec Observe & bt_prepare State Machine Tests ---- */

typedef struct {
    char cmds[32][64];
    int ncmds;
    int release_seen;
    int probe_state; /* 0=pending, 1=done, -1=fail */
    bc_snapshot snap;
    unsigned avail;
    int known;
    int codec_apply_calls;
    char applied_pcm[128];
    char applied_codec[32];
    int codec_apply_result; /* 0=pending, 1=ok, -1=err */
    int cancel_probe_calls;
    int cancel_codec_calls;
    int fallback_toasted;
    int fb_choice, fb_selected;
    char routed_mac[20];
} sim_sink_t;

static int sim_send(void *ctx, const char *f, int release, unsigned report, unsigned frames) {
    (void)report; (void)frames;
    sim_sink_t *s = (sim_sink_t *)ctx;
    if (s->ncmds < 32) {
        snprintf(s->cmds[s->ncmds++], 64, "%s", f);
    }
    if (release) s->release_seen = 1;
    return 0;
}
static int sim_probe_start(void *ctx, uint64_t *id) { (void)ctx; *id = 42; return 0; }
static int sim_probe_read(void *ctx, uint64_t id, bc_snapshot *snap, int *known, unsigned *avail) {
    (void)id;
    sim_sink_t *s = (sim_sink_t *)ctx;
    if (s->probe_state <= 0) return s->probe_state;
    *snap = s->snap;
    *known = s->known;
    *avail = s->avail;
    return 1;
}
static void sim_probe_cancel(void *ctx, uint64_t id) {
    (void)id;
    ((sim_sink_t *)ctx)->cancel_probe_calls++;
}
static int sim_codec_start(void *ctx, const char *pcm, const char *cname, uint64_t *id) {
    sim_sink_t *s = (sim_sink_t *)ctx;
    s->codec_apply_calls++;
    snprintf(s->applied_pcm, sizeof(s->applied_pcm), "%s", pcm);
    snprintf(s->applied_codec, sizeof(s->applied_codec), "%s", cname);
    *id = 99;
    return 0;
}
static int sim_codec_read(void *ctx, uint64_t id) {
    (void)id;
    return ((sim_sink_t *)ctx)->codec_apply_result;
}
static void sim_codec_cancel(void *ctx, uint64_t id) {
    (void)id;
    ((sim_sink_t *)ctx)->cancel_codec_calls++;
}
static void sim_routed(void *ctx, const char *mac) {
    snprintf(((sim_sink_t *)ctx)->routed_mac, 20, "%s", mac);
}
static void sim_fallback(void *ctx, int choice, int selected) {
    sim_sink_t *s = (sim_sink_t *)ctx;
    s->fallback_toasted = 1;
    s->fb_choice = choice;
    s->fb_selected = selected;
}

static bp_sink make_sink(sim_sink_t *s) {
    bp_sink k = {
        .ctx = s,
        .send = sim_send,
        .probe_start = sim_probe_start,
        .probe_read = sim_probe_read,
        .probe_cancel = sim_probe_cancel,
        .codec_start = sim_codec_start,
        .codec_read = sim_codec_read,
        .codec_cancel = sim_codec_cancel,
        .routed = sim_routed,
        .toast_fallback = sim_fallback,
    };
    return k;
}

static void make_ready_input(bp_input *in) {
    memset(in, 0, sizeof(*in));
    in->qualified = 1;
    in->permitted = 1;
    in->report_serial = 1;
    in->current.player_generation = 7;
    in->current.have_track = 1;
    in->current.player_confirmed = 1;
    in->current.state = 1;
    in->current.position_ms = 12345;
    in->current.volume = 60;
    in->current.volume_seq = 1;
    in->current.path_seq = 2;
    in->current.pos_seq = 5;
    in->current.song_id_valid = 1;
    in->current.song_id_raw = 77;
    in->current.pos_id_valid = 1;
    in->current.pos_id_raw = 88;
    snprintf(in->current.path, sizeof(in->current.path), "/mnt/sd/track.flac");
    in->report = in->current;
}

static void test_codec_observe_alt_path(void) {
    const char *txt =
        "PCM=/org/bluealsa/hci0/dev_AA_BB_CC_11_22_33/a2dp-source/sink\n"
        "Device: /org/bluez/hci0/dev_AA_BB_CC_11_22_33\n"
        "Sequence: 3\n"
        "Mode: sink\n"
        "Transport: A2DP-source\n"
        "Selected codec: aptX-HD\n"
        "Channels: 2\n"
        "Sampling: 48000 Hz\n"
        "Format: S24_4LE\n";
    bc_snapshot s;
    assert(bc_parse(txt, strlen(txt), "AA:BB:CC:11:22:33", &s) == BC_OK);
    assert(s.codec == BC_CODEC_APTX_HD);
    assert(s.sequence == 3);
    assert(s.rate == 48000);
}

static void test_bp_prepare_fallback_and_sequence_bump(void) {
    /* Request LDAC (4) when headset only supports AAC; starts on SBC -> fallback AAC,
     * applies AAC off-thread, handles Sequence bump during BP_VERIFY -> completes BP_DONE! */
    bp_prepare p = {0};
    bp_input in;
    make_ready_input(&in);
    sim_sink_t sim = {0};
    bp_sink k = make_sink(&sim);

    assert(bp_begin(&p, 100, "aa:bb:cc:11:22:33", 4, 1, &in) == 0);
    assert(strcmp(p.mac, "AA:BB:CC:11:22:33") == 0);

    /* BP_PAUSE sends 0201 pause toggle, then waits for fresh report_serial */
    bp_tick(&p, 100, &in, &k);
    assert(p.phase == BP_PAUSE);
    in.report_serial = 2;
    bp_tick(&p, 110, &in, &k);
    assert(p.phase == BP_POSITION);

    /* BP_POSITION completes when pos_seq advances past saved pos_seq & path_seq */
    in.current.pos_seq = 6;
    in.report = in.current;
    bp_tick(&p, 120, &in, &k);
    assert(p.phase == BP_ROUTE);

    /* 6 route steps */
    for (int step = 0; step < 6; step++) bp_tick(&p, 130 + (uint32_t)step, &in, &k);
    assert(p.phase == BP_CAPS);
    assert(strcmp(sim.routed_mac, "AA:BB:CC:11:22:33") == 0);

    /* Probe starts on first tick, finishes on second tick: headset only supports AAC, currently SBC */
    bp_tick(&p, 200, &in, &k);
    assert(p.phase == BP_CAPS);
    sim.probe_state = 1;
    sim.known = 1;
    sim.avail = BTC_AAC;
    snprintf(sim.snap.pcm, sizeof(sim.snap.pcm),
             "/org/bluealsa/hci0/dev_AA_BB_CC_11_22_33/a2dpsrc/sink");
    snprintf(sim.snap.mac, sizeof(sim.snap.mac), "AA:BB:CC:11:22:33");
    sim.snap.codec = BC_CODEC_SBC;
    sim.snap.sequence = 1;
    bp_tick(&p, 205, &in, &k);
    assert(sim.fallback_toasted == 1 && sim.fb_choice == 4 && sim.fb_selected == 1);
    assert(p.selected == 1);
    assert(p.phase == BP_APPLY_CODEC);

    /* BP_APPLY_CODEC starts & finishes */
    bp_tick(&p, 210, &in, &k);
    assert(sim.codec_apply_calls == 1);
    assert(strcmp(sim.applied_codec, "AAC") == 0);
    sim.codec_apply_result = 1;
    bp_tick(&p, 220, &in, &k);
    assert(p.phase == BP_CODEC);

    /* BP_CODEC sends 06b3...0001<MAC> */
    bp_tick(&p, 230, &in, &k);
    assert(p.phase == BP_VERIFY);
    assert(strcmp(sim.cmds[sim.ncmds - 1], "06b3001D0001AA:BB:CC:11:22:33") == 0);

    /* Verify succeeds EVEN WITH endpoint_sequence bump from 1 to 2! */
    in.report_serial = 3;
    sim.snap.sequence = 2;
    sim.snap.codec = BC_CODEC_AAC;
    bp_tick(&p, 240, &in, &k); /* issues 02020008 + transitions to BP_VOLUME */
    if (p.phase == BP_VERIFY) bp_tick(&p, 250, &in, &k);
    assert(p.phase == BP_VOLUME);

    bp_tick(&p, 260, &in, &k);
    assert(p.phase == BP_SEEK);

    bp_tick(&p, 270, &in, &k);
    assert(p.phase == BP_SEEKED);
    in.current.pos_seq = 7;
    in.current.position_ms = 12300;
    in.report = in.current;
    bp_tick(&p, 280, &in, &k);
    assert(p.phase == BP_RELEASE);

    bp_tick(&p, 290, &in, &k);
    assert(p.phase == BP_DONE);
    assert(sim.release_seen == 1);
}

static void test_bp_prepare_reconfig_and_cancel(void) {
    bp_prepare p = {0};
    bp_input in;
    make_ready_input(&in);
    sim_sink_t sim = {0};
    bp_sink k = make_sink(&sim);

    /* reconfig_only = 1 skips BP_ROUTE -> goes directly to BP_CAPS */
    assert(bp_begin_ex(&p, 1000, "AA:BB:CC:11:22:33", 5, 0, 1, &in) == 0);
    in.report_serial = 2;
    bp_tick(&p, 1000, &in, &k);
    assert(p.phase == BP_POSITION);
    in.current.pos_seq = 6;
    in.report = in.current;
    bp_tick(&p, 1010, &in, &k);
    assert(p.phase == BP_CAPS);

    /* Probe starts on first tick, finishes on second tick with aptX available -> enters BP_APPLY_CODEC */
    bp_tick(&p, 1015, &in, &k);
    assert(p.phase == BP_CAPS);
    sim.probe_state = 1;
    sim.known = 1;
    sim.avail = BTC_APTX | BTC_AAC;
    snprintf(sim.snap.pcm, sizeof(sim.snap.pcm),
             "/org/bluealsa/hci0/dev_AA_BB_CC_11_22_33/a2dpsrc/sink");
    snprintf(sim.snap.mac, sizeof(sim.snap.mac), "AA:BB:CC:11:22:33");
    sim.snap.codec = BC_CODEC_AAC;
    bp_tick(&p, 1020, &in, &k);
    assert(p.phase == BP_APPLY_CODEC);
    bp_tick(&p, 1030, &in, &k);
    assert(strcmp(sim.applied_codec, "aptX") == 0);

    /* Cancel mid-APPLY_CODEC must invoke codec_cancel */
    bp_cancel(&p, &k);
    assert(p.phase == BP_CANCELLED);
    assert(sim.cancel_codec_calls == 1);
    assert(!bp_active(&p));
}

/* ---- RX Logic Tests ---- */

static void test_rx_dbus_parsing_edge_cases(void) {
    /* Escaped quotes, multi-line strings, nested Track dict, negative/uint64 numbers,
     * and decoy substring ("Title") inside another string value! */
    const char *reply =
        "method return time=1.23\n"
        "  array [\n"
        "    dict entry(\n"
        "      string \"Status\"\n"
        "      variant\n"
        "        string \"playing\"\n"
        "    )\n"
        "    dict entry(\n"
        "      string \"Position\"\n"
        "      variant int32 -50\n"
        "    )\n"
        "    dict entry(\n"
        "      string \"Track\"\n"
        "      variant array [\n"
        "        dict entry(\n"
        "          string \"Artist\"\n"
        "          variant string \"The \\\"Title\\\" Band\"\n"
        "        )\n"
        "        dict entry(\n"
        "          string \"Title\"\n"
        "          variant\n"
        "            string \"Line One\nLine \\\"Two\\\"\"\n"
        "        )\n"
        "        dict entry(\n"
        "          string \"Album\"\n"
        "          variant string \"Best\\\\Hits\"\n"
        "        )\n"
        "        dict entry(\n"
        "          string \"Duration\"\n"
        "          variant uint64 245000\n"
        "        )\n"
        "      ]\n"
        "    )\n"
        "  ]\n";

    bt_rx_mediaplayer_props_t mp;
    bt_rx_dbus_parse_mediaplayer_props(reply, &mp);
    assert(mp.has_status && strcmp(mp.status, "playing") == 0);
    assert(mp.has_artist && strcmp(mp.artist, "The \"Title\" Band") == 0);
    assert(mp.has_title && strcmp(mp.title, "Line One Line \"Two\"") == 0);
    assert(mp.has_album && strcmp(mp.album, "Best\\Hits") == 0);
    assert(mp.has_duration && mp.duration_ms == 245000);
    assert(mp.has_position && mp.position_ms == 0); /* negative int32 clamped cleanly */

    uint32_t pos = 0;
    assert(bt_rx_dbus_parse_variant_u32("method return\n  variant uint32 42190\n", &pos) == 1);
    assert(pos == 42190);
}

static int filter_only_phone(const char *mac, void *ctx) {
    (void)ctx;
    return strcasecmp(mac, "AA:BB:CC:00:11:22") == 0;
}

static void test_rx_reconcile_conn_truthfulness(void) {
    bt_rx_conn_info_t info;

    /* 1. Stale bt_state == 1 with NO sink PCM and NO ACL/BlueZ MUST stay disconnected! */
    bt_rx_reconcile_conn("", "", "", "", 1, 0, NULL, NULL, &info);
    assert(info.connected == 0);
    assert(info.ignored_stale_bt_state == 1);

    /* 2. Stale bt_state == 1 with TX /a2dpsrc/ PCM must NOT count as RX connected */
    bt_rx_reconcile_conn("/org/bluealsa/hci0/dev_11_22_33_44_55_66/a2dpsrc/sink\n",
                         "", "", "", 1, 0, NULL, NULL, &info);
    assert(info.connected == 0);
    assert(info.ignored_stale_bt_state == 1);

    /* 3. Active /a2dp-sink/ PCM overrides even force_drop_active */
    bt_rx_reconcile_conn("/org/bluealsa/hci0/dev_aa_bb_cc_00_11_22/a2dp-sink/source\n",
                         "", "AA:BB:CC:00:11:22", "My Pixel", 0, 1, NULL, NULL, &info);
    assert(info.connected == 1);
    assert(info.has_sink_pcm == 1);
    assert(strcmp(info.mac, "AA:BB:CC:00:11:22") == 0);
    assert(strcmp(info.name, "My Pixel") == 0);

    /* 4. ACL + BlueZ Connected (before PCM opens): connected when !force_drop_active */
    bt_rx_reconcile_conn("", "< ACL AA:BB:CC:00:11:22 handle 11 state 1\n",
                         "aa:bb:cc:00:11:22", "iPhone", 0, 0, filter_only_phone, NULL, &info);
    assert(info.connected == 1 && info.acl_active == 1 && info.bluez_connected == 1);
    assert(strcmp(info.mac, "AA:BB:CC:00:11:22") == 0);

    /* 5. Same ACL + BlueZ during force_drop window -> suppressed until PCM arrives */
    bt_rx_reconcile_conn("", "< ACL AA:BB:CC:00:11:22 handle 11 state 1\n",
                         "AA:BB:CC:00:11:22", "iPhone", 1, 1, filter_only_phone, NULL, &info);
    assert(info.connected == 0);

    /* 6. Non-audio-source ACL filtered out */
    bt_rx_reconcile_conn("", "< ACL 99:88:77:66:55:44 handle 12 state 1\n",
                         "99:88:77:66:55:44", "Earbuds", 1, 0, filter_only_phone, NULL, &info);
    assert(info.connected == 0);
}

static void test_rx_bluez_objects_avrcp_and_heal(void) {
    /* Must skip /player0 sub-object and disconnected device */
    const char *objs =
        "  object path \"/org/bluez/hci0/dev_11_22_33_44_55_66\"\n"
        "    dict entry( string \"Connected\" variant boolean false )\n"
        "    dict entry( string \"Name\" variant string \"Old Phone\" )\n"
        "  object path \"/org/bluez/hci0/dev_AA_BB_CC_00_11_22/player0\"\n"
        "    dict entry( string \"Connected\" variant boolean true )\n"
        "  object path \"/org/bluez/hci0/dev_AA_BB_CC_00_11_22\"\n"
        "    dict entry( string \"Connected\" variant boolean true )\n"
        "    dict entry( string \"Alias\" variant string \"Lui Phone\" )\n";
    char mac[18] = {0}, name[64] = {0};
    assert(bt_rx_extract_bluez_connected(objs, filter_only_phone, NULL,
                                         mac, sizeof(mac), name, sizeof(name)) == 1);
    assert(strcmp(mac, "AA:BB:CC:00:11:22") == 0);
    assert(strcmp(name, "Lui Phone") == 0);

    /* Single-path AVRCP planning */
    bt_rx_avrcp_plan_t p1 = bt_rx_plan_avrcp(BT_RX_AVRCP_ACT_PLAY_PAUSE, 1, "AA_BB_CC_00_11_22", 1);
    assert(p1.use_dbus == 1 && strcmp(p1.ctrl_method, "Pause") == 0 && strcmp(p1.ipc_cmd, "0201000C0000") == 0);
    bt_rx_avrcp_plan_t p2 = bt_rx_plan_avrcp(BT_RX_AVRCP_ACT_NEXT, 0, "", 0);
    assert(p2.use_dbus == 0 && strcmp(p2.ipc_cmd, "0201000C0001") == 0);

    /* Heal startup grace (15s) & cooldown (20s) */
    assert(bt_rx_should_heal(14999, 1000, 0, 15000, 20000, 0, 0) == 0);
    assert(bt_rx_should_heal(16000, 1000, 0, 15000, 20000, 0, 1) == 0); /* bt_enabling */
    assert(bt_rx_should_heal(16000, 1000, 0, 15000, 20000, 1, 0) == 0); /* connected */
    assert(bt_rx_should_heal(16000, 1000, 0, 15000, 20000, 0, 0) == 1);
    assert(bt_rx_should_heal(35999, 1000, 16000, 15000, 20000, 0, 0) == 0);
    assert(bt_rx_should_heal(36000, 1000, 16000, 15000, 20000, 0, 0) == 1);

    /* Codec fallback label sanitization */
    char clbl[32];
    assert(bt_rx_clean_codec_label("  AUTO\n", clbl, sizeof(clbl)) == 0);
    assert(bt_rx_clean_codec_label("  LDAC \r\n", clbl, sizeof(clbl)) == 1);
    assert(strcmp(clbl, "LDAC") == 0);
}

/* ---- Line Buffer Framing Tests ---- */

typedef struct {
    char lines[8][128];
    int n;
} cap_lines_t;

static void on_line_cap(const char *line, void *ctx) {
    cap_lines_t *c = (cap_lines_t *)ctx;
    if (c->n < 8) snprintf(c->lines[c->n++], 128, "%s", line);
}

static void test_avrcp_linebuf_incremental_and_resync(void) {
    avrcp_linebuf_t lb;
    avrcp_linebuf_init(&lb);
    cap_lines_t cap = {0};

    /* 1-byte split + CRLF */
    const char *s1 = "TITLE:Hello\r\nNEXT\n";
    for (size_t i = 0; s1[i]; i++) {
        avrcp_linebuf_feed(&lb, &s1[i], 1, on_line_cap, &cap);
    }
    assert(cap.n == 2);
    assert(strcmp(cap.lines[0], "TITLE:Hello") == 0);
    assert(strcmp(cap.lines[1], "NEXT") == 0);

    /* Oversized unterminated garbage (> AVRCP_LINEBUF_CAP) followed by valid frame */
    char junk[AVRCP_LINEBUF_CAP + 200];
    memset(junk, 'X', sizeof(junk));
    avrcp_linebuf_feed(&lb, junk, sizeof(junk), on_line_cap, &cap);
    const char *rec = "\nPREV\n";
    avrcp_linebuf_feed(&lb, rec, strlen(rec), on_line_cap, &cap);
    assert(cap.n == 3);
    assert(strcmp(cap.lines[2], "PREV") == 0);

    char san[32];
    avrcp_sanitize_field(san, sizeof(san), "A\r\nB");
    assert(strcmp(san, "A  B") == 0);
}

static void test_bp_prepare_paused_position_and_unknown_caps(void) {
    bp_prepare p = {0};
    bp_input in;
    make_ready_input(&in);
    sim_sink_t sim = {0};
    bp_sink k = make_sink(&sim);

    /* Paused stream (playing=0), reconfig_only=1, cold-boot player_generation=0 */
    in.current.player_generation = 0;
    in.report.player_generation = 0;
    assert(bp_begin_ex(&p, 100, "AA:BB:CC:11:22:33", 4, 0, 1, &in) == 0);
    in.report_serial = 2;
    bp_tick(&p, 110, &in, &k);
    assert(p.phase == BP_POSITION);

    /* Without pos_seq bumping first (paused track), BP_POSITION must actively query (02020008) */
    int cmds_before = sim.ncmds;
    bp_tick(&p, 120, &in, &k);
    assert(sim.ncmds > cmds_before);
    assert(strcmp(sim.cmds[sim.ncmds - 1], "02020008") == 0);

    in.current.pos_seq = 6;
    in.report = in.current;
    bp_tick(&p, 130, &in, &k);
    assert(p.phase == BP_CAPS);

    /* Probe finishes with known == 0 -> must NOT stall waiting for known == 1; falls back to SBC */
    bp_tick(&p, 140, &in, &k);
    sim.probe_state = 1;
    sim.known = 0;
    sim.avail = 0;
    snprintf(sim.snap.pcm, sizeof(sim.snap.pcm),
             "/org/bluealsa/hci0/dev_AA_BB_CC_11_22_33/a2dpsrc/sink");
    snprintf(sim.snap.mac, sizeof(sim.snap.mac), "AA:BB:CC:11:22:33");
    sim.snap.codec = BC_CODEC_SBC;
    sim.snap.sequence = 1;
    bp_tick(&p, 150, &in, &k);
    assert(p.phase != BP_CAPS);
    assert(p.selected == 0);
    assert(sim.fallback_toasted == 1 && sim.fb_choice == 4 && sim.fb_selected == 0);

    /* Paused track where a103 does not emit a new tick past 500 ms must settle from valid pos_seq,
     * and BP_PAUSE / BP_VERIFY must not stall if mq_player stays silent on 02020008 while paused. */
    memset(&p, 0, sizeof(p));
    memset(&sim, 0, sizeof(sim));
    make_ready_input(&in);
    assert(bp_begin_ex(&p, 1000, "AA:BB:CC:11:22:33", 1, 0, 1, &in) == 0);
    bp_tick(&p, 1010, &in, &k);
    assert(p.phase == BP_POSITION);
    bp_tick(&p, 1200, &in, &k);
    assert(p.phase == BP_POSITION);
    bp_tick(&p, 1520, &in, &k);
    assert(p.phase == BP_CAPS);
    assert(p.position == 12345);
    bp_tick(&p, 1530, &in, &k);
    sim.probe_state = 1;
    sim.known = 1;
    sim.avail = BTC_AAC;
    snprintf(sim.snap.pcm, sizeof(sim.snap.pcm),
             "/org/bluealsa/hci0/dev_AA_BB_CC_11_22_33/a2dpsrc/sink");
    snprintf(sim.snap.mac, sizeof(sim.snap.mac), "AA:BB:CC:11:22:33");
    sim.snap.codec = BC_CODEC_AAC;
    sim.snap.sequence = 1;
    bp_tick(&p, 1540, &in, &k);
    assert(p.phase == BP_CODEC);
    bp_tick(&p, 1550, &in, &k);
    assert(p.phase == BP_VERIFY);
    bp_tick(&p, 1560, &in, &k); /* issues 02020008 + starts probe */
    bp_tick(&p, 3100, &in, &k); /* report_serial still unchanged after 1500 ms -> settles */
    assert(p.phase == BP_VOLUME);
    bp_tick(&p, 3110, &in, &k);
    assert(p.phase == BP_SEEK);
    bp_tick(&p, 3120, &in, &k);
    assert(p.phase == BP_SEEKED);
    bp_tick(&p, 5700, &in, &k); /* no a103 bump on seek while paused past 2500 ms -> BP_DONE */
    assert(p.phase == BP_DONE);

    /* Playing track (playing=1) where mq_player stops a103 immediately upon pause -> settles past 1200 ms */
    memset(&p, 0, sizeof(p));
    memset(&sim, 0, sizeof(sim));
    make_ready_input(&in);
    assert(bp_begin_ex(&p, 6000, "AA:BB:CC:11:22:33", 1, 1, 1, &in) == 0);
    bp_tick(&p, 6010, &in, &k);
    in.report_serial = 2;
    bp_tick(&p, 6020, &in, &k);
    assert(p.phase == BP_POSITION);
    bp_tick(&p, 7000, &in, &k);
    assert(p.phase == BP_POSITION);
    bp_tick(&p, 7250, &in, &k);
    assert(p.phase == BP_CAPS);
    assert(p.position == 12345);
}

static void test_rx_large_device_adapter_and_player_discovery(void) {
    static char objs[8192];
    size_t pos = 0;
    pos += (size_t)snprintf(objs + pos, sizeof(objs) - pos,
        "  object path \"/org/bluez/hci0\"\n"
        "    interface \"org.bluez.Adapter1\"\n"
        "      dict entry( string \"Alias\" variant string \"Snowsky Disc\" )\n"
        "      dict entry( string \"Powered\" variant boolean true )\n"
        "      dict entry( string \"Discoverable\" variant boolean false )\n"
        "      dict entry( string \"Pairable\" variant boolean true )\n"
        "  object path \"/org/bluez/hci0/dev_AA_BB_CC_00_11_22\"\n"
        "    interface \"org.bluez.Device1\"\n"
        "      dict entry( string \"UUIDs\" variant array [\n");
    for (int i = 0; i < 32; i++) {
        pos += (size_t)snprintf(objs + pos, sizeof(objs) - pos,
            "        string \"000011%02x-0000-1000-8000-00805f9b34fb\"\n", i);
    }
    snprintf(objs + pos, sizeof(objs) - pos,
        "      ] )\n"
        "      dict entry( string \"Name\" variant string \"PhoneRaw\" )\n"
        "      dict entry( string \"Alias\" variant string \"Pixel 8\" )\n"
        "      dict entry( string \"Connected\" variant boolean true )\n"
        "    interface \"org.bluez.MediaControl1\"\n"
        "      dict entry( string \"Player\" variant object path \"/org/bluez/hci0/dev_AA_BB_CC_00_11_22/player2\" )\n"
        "  object path \"/org/bluez/hci0/dev_AA_BB_CC_00_11_22/player2\"\n"
        "    interface \"org.bluez.MediaPlayer1\"\n"
        "      dict entry( string \"Status\" variant string \"playing\" )\n"
        "      dict entry( string \"Position\" variant uint32 45000 )\n"
        "      dict entry( string \"Track\" variant array [\n"
        "        dict entry( string \"Title\" variant string \"Night Drive\" )\n"
        "        dict entry( string \"Artist\" variant string \"Synth\" )\n"
        "        dict entry( string \"Duration\" variant uint32 180000 )\n"
        "      ] )\n");

    char mac[18] = {0}, name[64] = {0};
    assert(bt_rx_extract_bluez_connected(objs, filter_only_phone, NULL,
                                         mac, sizeof(mac), name, sizeof(name)) == 1);
    assert(strcmp(mac, "AA:BB:CC:00:11:22") == 0);
    assert(strcmp(name, "Pixel 8") == 0);

    bt_rx_adapter_state_t ad = {0};
    assert(bt_rx_extract_adapter_state(objs, &ad) == 1);
    assert(ad.powered == 1 && ad.discoverable == 0 && ad.pairable == 1);
    assert(strcmp(ad.alias, "Snowsky Disc") == 0);

    char ppath[96] = {0};
    assert(bt_rx_extract_player_path(objs, "AA:BB:CC:00:11:22", ppath, sizeof(ppath)) == 1);
    assert(strcmp(ppath, "/org/bluez/hci0/dev_AA_BB_CC_00_11_22/player2") == 0);

    bt_rx_mediaplayer_props_t mp;
    memset(&mp, 0, sizeof(mp));
    assert(bt_rx_extract_mediaplayer_from_objs(objs, ppath, &mp) == 1);
    assert(mp.has_status == 1 && strcmp(mp.status, "playing") == 0);
    assert(mp.has_position == 1 && mp.position_ms == 45000);
    assert(mp.has_duration == 1 && mp.duration_ms == 180000);
    assert(mp.has_title == 1 && strcmp(mp.title, "Night Drive") == 0);
    assert(mp.has_artist == 1 && strcmp(mp.artist, "Synth") == 0);

    assert(bt_rx_path_safe("/org/bluealsa/hci0/dev_11_22_33_44_55_66/a2dpsnk/source") == 1);
    assert(bt_rx_path_safe("/org/bluez/hci0/dev_11_22_33_44_55_66/player2") == 1);
    assert(bt_rx_path_safe("/bad;rm -rf") == 0);
    assert(bt_rx_path_safe("relative/path") == 0);
}

static void test_tx_pairing_tws_and_codec_choice_map(void) {
    /* Pairing TWS without Icon property yet: Audio/Video CoD (0x240404) & multi-line variant formatting */
    const char *objs =
        "  object path \"/org/bluez/hci0/dev_AA_11_22_33_44_55\"\n"
        "    string \"org.bluez.Device1\"\n"
        "      dict entry( string \"Address\" variant\n"
        "        string \"AA:11:22:33:44:55\" )\n"
        "      dict entry( string \"Alias\" variant string \"MOONDROP TWS\" )\n"
        "      dict entry( string \"Class\" variant uint32 0x240404 )\n"
        "      dict entry( string \"Adapter\" variant object path \"/org/bluez/hci0\" )\n"
        "      dict entry( string \"Paired\" variant boolean false )\n"
        "      dict entry( string \"Connected\" variant boolean false )\n"
        "  object path \"/org/bluez/hci0/dev_BB_11_22_33_44_55\"\n"
        "    string \"org.bluez.Device1\"\n"
        "      dict entry( string \"Address\" variant string \"BB:11:22:33:44:55\" )\n"
        "      dict entry( string \"Alias\" variant string \"Pixel Phone\" )\n"
        "      dict entry( string \"Icon\" variant string \"phone\" )\n"
        "      dict entry( string \"Paired\" variant boolean true )\n"
        "  object path \"/org/bluez/hci0/dev_CC_11_22_33_44_55\"\n"
        "    string \"org.bluez.Device1\"\n"
        "      dict entry( string \"Address\" variant string \"CC:11:22:33:44:55\" )\n"
        "      dict entry( string \"Name\" variant string \"Buds A2DP\" )\n"
        "      dict entry( string \"UUIDs\" variant array [\n"
        "        string \"0000110b-0000-1000-8000-00805f9b34fb\"\n"
        "      ] )\n";
    bt_tx_scan_dev_t res[8];
    memset(res, 0, sizeof(res));
    int n = bt_tx_parse_managed_objects(objs, res, 8);
    assert(n == 2);
    assert(strcmp(res[0].mac, "AA:11:22:33:44:55") == 0);
    assert(strcmp(res[0].name, "MOONDROP TWS") == 0);
    assert(strcmp(res[1].mac, "CC:11:22:33:44:55") == 0);
    assert(strcmp(res[1].name, "Buds A2DP") == 0);

    /* bluetoothctl info audio detection without Icon line */
    assert(bt_tx_info_is_audio("Device AA:11:22:33:44:55\n\tName: TWS\n\tClass: 0x240404\n") == 1);
    assert(bt_tx_info_is_audio("Device DD:11:22:33:44:55\n\tName: Keyboard\n\tIcon: input-keyboard\n") == 0);

    /* Selected codec name -> UI index mapping */
    assert(bt_tx_choice_from_codec_name("SBC", 4) == 0);
    assert(bt_tx_choice_from_codec_name("AAC", 4) == 1);
    assert(bt_tx_choice_from_codec_name("LDAC", 4) == 4);
    assert(bt_tx_choice_from_codec_name("LDAC", 2) == 2);
    assert(bt_tx_choice_from_codec_name("LDAC", 1) == 3);
    assert(bt_tx_choice_from_codec_name("aptX", 0) == 5);
    assert(bt_tx_choice_from_codec_name("aptX-HD", 0) == 6);
}

static void test_bp_verify_quiet_when_was_playing(void) {
    /* When was_playing=1 and mq_player stops answering 02020008 after BP_CODEC,
     * BP_VERIFY still advances to BP_VOLUME once verify_at + 1500ms passes. */
    bp_prepare p = {0};
    bp_input in;
    make_ready_input(&in);
    sim_sink_t sim = {0};
    bp_sink k = make_sink(&sim);

    assert(bp_begin(&p, 100, "AA:BB:CC:11:22:33", 4, 1, &in) == 0);
    bp_tick(&p, 100, &in, &k);
    in.report_serial = 2;
    bp_tick(&p, 110, &in, &k);
    assert(p.phase == BP_POSITION);
    in.current.pos_seq = 6;
    in.report = in.current;
    bp_tick(&p, 120, &in, &k);
    assert(p.phase == BP_ROUTE);
    for (int step = 0; step < 6; step++) bp_tick(&p, 130 + (uint32_t)step, &in, &k);
    assert(p.phase == BP_CAPS);

    bp_tick(&p, 200, &in, &k);
    sim.probe_state = 1;
    sim.known = 1;
    sim.avail = BTC_AAC;
    snprintf(sim.snap.pcm, sizeof(sim.snap.pcm),
             "/org/bluealsa/hci0/dev_AA_BB_CC_11_22_33/a2dpsrc/sink");
    snprintf(sim.snap.mac, sizeof(sim.snap.mac), "AA:BB:CC:11:22:33");
    sim.snap.codec = BC_CODEC_AAC;
    bp_tick(&p, 210, &in, &k);
    assert(p.phase == BP_CODEC);

    bp_tick(&p, 220, &in, &k);
    assert(p.phase == BP_VERIFY);

    /* Keep report_serial fixed (2) and simulate probe completing before/after 1500ms */
    bp_tick(&p, 230, &in, &k); /* issues 02020008 (queried=1) + starts probe */
    assert(p.phase == BP_VERIFY);
    bp_tick(&p, 1719, &in, &k); /* < 1500ms since verify_at=220 */
    assert(p.phase == BP_VERIFY);
    bp_tick(&p, 1720, &in, &k); /* starts fresh probe >= 1500ms */
    bp_tick(&p, 1725, &in, &k); /* probe completes -> BP_VOLUME */
    assert(p.phase == BP_VOLUME);
}

int main(void) {
    test_tx_mac_and_info_prop();
    test_tx_codec_frame_and_pick();
    test_tx_source_pcms_and_link_check();
    test_tx_managed_objects();
    test_tx_row_classification_and_labels();
    test_tx_pairing_tws_and_codec_choice_map();
    test_codec_observe_alt_path();
    test_bp_prepare_fallback_and_sequence_bump();
    test_bp_prepare_reconfig_and_cancel();
    test_bp_prepare_paused_position_and_unknown_caps();
    test_bp_verify_quiet_when_was_playing();
    test_rx_dbus_parsing_edge_cases();
    test_rx_reconcile_conn_truthfulness();
    test_rx_bluez_objects_avrcp_and_heal();
    test_rx_large_device_adapter_and_player_discovery();
    test_avrcp_linebuf_incremental_and_resync();
    printf("bt_test: all assertions passed\n");
    return 0;
}
