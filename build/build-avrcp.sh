#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# Builds payload/diskos-avrcpd using the Ingenic GCC toolchain
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT="$HERE/.."
OUT="$ROOT/payload/diskos-avrcpd"
CROSS=${CROSS:-/home/luii/.local/toolchains/mips-ingenic-gcc720/mips-linux-gnu-ingenic-gcc7.2.0-glibc2.29-fp64-r5.1.0/bin/mips-linux-gnu-}

# Ensure device libdbus exists locally for linking
LIBDIR="/tmp/device_libs"
mkdir -p "$LIBDIR"
if [ ! -f "$LIBDIR/libdbus-1.so" ]; then
    if [ -f "$LIBDIR/libdbus-1.so.3" ]; then
        ln -sf libdbus-1.so.3 "$LIBDIR/libdbus-1.so"
    else
        echo "Error: $LIBDIR/libdbus-1.so not found" >&2
        exit 1
    fi
fi

echo "Compiling diskos-avrcpd..."
"${CROSS}gcc" -Os -mips32r2 -EL -Wall -Wextra -Wno-unused-parameter \
    -I/usr/include/dbus-1.0 -I/usr/lib/dbus-1.0/include \
    -L"$LIBDIR" -ldbus-1 -lrt \
    -o "$OUT" "$ROOT/src/avrcp/diskos_avrcpd.c"

"${CROSS}strip" "$OUT"
chmod 755 "$OUT"
echo "Built $OUT successfully (size: $(wc -c < "$OUT") bytes)"
sha256sum "$OUT"
