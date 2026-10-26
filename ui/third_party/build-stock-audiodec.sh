#!/bin/sh
# SPDX-License-Identifier: MIT
# Offline, dynamic V2.57 helper. Never compile or redistribute FFmpeg sources.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
archive=${STOCK_AV_HEADERS:-"$root/vendor-src/ffmpeg/ffmpeg-4.2.2.tar.xz"}
sysroot=${STOCK_SYSROOT:-"$root/scratch/fixtures/v257"}
out=${STOCK_AUDIO_OUT:-"$root/scratch/native-deps/stock-audiodec"}
cc=${STOCK_CC:-mipsel-linux-gnu-gcc}
[ -f "$archive" ] && [ -r "$archive" ] || {
    echo "Missing or unreadable FFmpeg 4.2.2 header archive: $archive (set STOCK_AV_HEADERS)" >&2
    exit 1
}
expected=cb754255ab0ee2ea5f66f8850e1bd6ad5cac1cd855d0a2f4990fb8c668b0d29c
actual=$(sha256sum "$archive"); actual=${actual%% *}
[ "$actual" = "$expected" ] || {
    echo "FFmpeg 4.2.2 header archive SHA256 mismatch: $archive (expected $expected, got $actual)" >&2
    exit 1
}
[ "${1:-}" != --verify-only ] || exit 0
mkdir -p "$out"
# Only public headers are extracted. No configure or dependency installation.
mkdir -p "$out/headers/gnu"
tar -xf "$archive" -C "$out/headers" --wildcards 'ffmpeg-4.2.2/libavcodec/*.h' 'ffmpeg-4.2.2/libavformat/*.h' 'ffmpeg-4.2.2/libavutil/*.h'
printf '%s\n' '#define AV_HAVE_BIGENDIAN 0' '#define AV_HAVE_FAST_UNALIGNED 0' > "$out/headers/ffmpeg-4.2.2/libavutil/avconfig.h"
# Debian's o32 headers lack this empty ABI selection stub. No libc is supplied.
: > "$out/headers/gnu/stubs-o32_hard_2008.h"
app=${STOCK_APP:?set STOCK_APP to the application source directory}
# sysv hash is required by the stock 2.29 loader (modern MIPS XHASH faults at exit).
# No host crt/libc/libgcc and no unresolved-symbol override.
"$cc" -Os -std=gnu11 -Wall -Wextra -Werror -mips32r2 -mnan=2008 -mabs=2008 \
    -ffp-contract=off -fno-ident "-ffile-prefix-map=$root/=" \
    -I"$out/headers" -I"$out/headers/ffmpeg-4.2.2" -I"$app" \
    -U_TIME_BITS -D_TIME_BITS=32 -nostdlib -no-pie -Wl,--hash-style=sysv,-E,-e,__start \
    -Wl,--dynamic-linker=/lib/ld-linux-mipsn8.so.1 \
    -Wl,-rpath-link,"$sysroot/lib" -Wl,-rpath-link,"$sysroot/usr/lib" \
    "$app/tools/audio_stock_start.S" "$app/tools/diskos_audiodec.c" \
    "$sysroot/usr/lib/libavformat.so.58" "$sysroot/usr/lib/libavcodec.so.58" \
    "$sysroot/usr/lib/libavutil.so.56" "$sysroot/lib/libc.so.6" \
    -o "$out/diskos-audiodec"
"${STOCK_STRIP:-mipsel-linux-gnu-strip}" "$out/diskos-audiodec"
sha256sum "$out/diskos-audiodec"
stat -c '%n %s bytes' "$out/diskos-audiodec"
