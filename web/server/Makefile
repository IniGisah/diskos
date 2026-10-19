# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# Makefile for diskos-webd (MIPS32r2 static binary for Ingenic X2000)

CROSS ?= mipsel-linux-musl-
CC    := $(CROSS)gcc

CFLAGS  := -O2 -Wall -s -static -pthread -D_GNU_SOURCE
LDFLAGS := -static -pthread
LDLIBS  := -lpthread

TARGET := diskos-webd

all: $(TARGET) $(TARGET).gz

$(TARGET): diskos-webd.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

$(TARGET).gz: $(TARGET)
	gzip -9 -c $< > $@

clean:
	rm -f $(TARGET) $(TARGET).gz

.PHONY: all clean
