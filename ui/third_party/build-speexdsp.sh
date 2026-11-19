#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$here/vendor-build-common.sh"
src="$here/speexdsp-1.2.1"
"$cc" -Os -std=gnu99 -ffunction-sections -fdata-sections -fno-ident \
    "-ffile-prefix-map=$root/=" -DHAVE_CONFIG_H -DRANDOM_PREFIX=diskos \
    -I"$here/speexconfig" -I"$src/include" -I"$src/libspeexdsp" \
    -c "$src/libspeexdsp/resample.c" -o "$work/resample.o"
"$ar" crD "$work/libspeexdsp.a" "$work/resample.o"
mv "$work/libspeexdsp.a" "$out/libspeexdsp.a"
stat -c '%n %s bytes' "$out/libspeexdsp.a"
