# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# Off-device host tests for diskOS pure-logic modules (arch#5 harness).
# Native x86-64 build with sanitizers + CFG_* paths pointed at a temp dir. Does NOT touch the
# device build (separate dir, separate Makefile). Run: make -C tests run
CC ?= cc
CFLAGS := -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
TDIR := /tmp/diskos_cfgtest
DEFS := -DCFG_PATH='"$(TDIR)/diskos.conf"' -DCFG_DIR='"$(TDIR)"' \
        -DLEGACY_CONF='"$(TDIR)/nolegacy1"' -DLEGACY_SWIPE='"$(TDIR)/nolegacy2"'

config_test: ../config.c config_test.c ../config.h
	$(CC) $(CFLAGS) $(DEFS) -I.. ../config.c config_test.c -o $@

playstate_test: ../playstate.c playstate_test.c ../playstate.h
	$(CC) $(CFLAGS) -I.. ../playstate.c playstate_test.c -o $@

wifi_test: wifi_test.c ../wifi_logic.h
	$(CC) $(CFLAGS) -I.. wifi_test.c -o $@

bt_test: bt_test.c ../bt_prepare.c ../bt_prepare.h ../bt_codec_observe.c ../bt_codec_observe.h ../bt_tx_logic.h ../bt_rx_logic.h ../bt_avrcp_proto.h
	$(CC) $(CFLAGS) -Ihost_stubs -I.. ../bt_prepare.c ../bt_codec_observe.c bt_test.c -o $@

DBUS_CFLAGS := $(shell pkg-config --cflags dbus-1)
DBUS_LIBS   := $(shell pkg-config --libs dbus-1)

avrcp_test: avrcp_test.c ../bt_avrcp.c ../bt_avrcp.h ../bt_avrcp_proto.h ../../src/avrcp/diskos_avrcpd.c
	$(CC) $(CFLAGS) -DDISKOS_HOST_TEST -Ihost_stubs -I.. $(DBUS_CFLAGS) ../bt_avrcp.c avrcp_test.c $(DBUS_LIBS) -o $@

.PHONY: run clean
run: config_test playstate_test wifi_test bt_test avrcp_test
	./config_test
	@echo "--- playstate ---"
	./playstate_test
	@echo "--- wifi ---"
	./wifi_test
	@echo "--- bt ---"
	./bt_test
	@echo "--- avrcp ---"
	./avrcp_test

clean:
	rm -f config_test playstate_test wifi_test bt_test avrcp_test
