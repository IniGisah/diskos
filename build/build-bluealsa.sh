#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 diskOS contributors
#
# Build recipe documentation for payload/bluealsa-custom
# Upstream source: BlueALSA v4.0.0 (https://github.com/arkq/bluez-alsa/archive/v4.0.0.tar.gz)
# License: MIT (Arkadiusz Bokowy)
#
# bluealsa-custom is built against the device's MIPS32r2 sysroot (glibc, bluez, fdk-aac, sbc)
# and links dynamically against the Disc's stock shared libraries:
#   - libaptx.so, libaptxHD.so (Qualcomm aptX / aptX-HD)
#   - libldacBT_enc.so.2, libldacBT_abr.so.2 (Sony LDAC encode & decode)
#
# Note: diskOS does not redistribute proprietary vendor libraries or rootfs.
# To compile, point SYSROOT to an extracted stock FiiO V2.57 firmware rootfs.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT="$HERE/.."
OUT="$ROOT/payload/bluealsa-custom"
CROSS=${CROSS:-mips-linux-gnu-}
SYSROOT=${SYSROOT:-"$ROOT/scratch/v257_rootfs"}

if [ ! -d "$SYSROOT" ]; then
    echo "Usage: SYSROOT=/path/to/extracted/stock_rootfs $0" >&2
    echo "SYSROOT directory not found: $SYSROOT" >&2
    exit 1
fi

echo "Building bluealsa-custom using SYSROOT=$SYSROOT and CROSS=$CROSS"
# 1. Obtain upstream BlueALSA 4.0.0:
#    git clone --depth 1 --branch v4.0.0 https://github.com/arkq/bluez-alsa.git
# 2. Run autoreconf -fiv
# 3. Configure:
#    ./configure --host="${CROSS%-}" --prefix=/usr \
#      CFLAGS="-EL -mips32r2 -mnan=2008 -O2 -I$SYSROOT/usr/include" \
#      LDFLAGS="-L$SYSROOT/usr/lib -L$SYSROOT/lib -Wl,-rpath-link,$SYSROOT/usr/lib -Wl,-rpath-link,$SYSROOT/lib" \
#      LIBS="$SYSROOT/lib/libblkid.so.1.1.0" \
#      --enable-aac --enable-aptx --enable-aptx-hd --enable-ldac --enable-cli \
#      --disable-manpages --disable-test --disable-systemd
# 4. Enable LDAC decoding in config.h (#define HAVE_LDAC_DECODE 1)
# 5. Compile with make -j$(nproc)
# 6. Strip: ${CROSS}strip src/bluealsa -o "$OUT"

