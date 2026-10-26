#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$here/vendor-build-common.sh"
src="$here/nanopb-0.4.9.1"
for file in "$src/pb_common.c" "$src/pb_encode.c" "$src/pb_decode.c" "$here/cast/cast_channel.pb.c"; do
    name=${file##*/}
    "$cc" -Os -std=c99 -ffunction-sections -fdata-sections -fno-ident \
        "-ffile-prefix-map=$root/=" -I"$src" -I"$here/cast" \
        -c "$file" -o "$work/${name%.c}.o"
done
"$ar" crD "$work/libnanopb.a" "$work/"*.o
mv "$work/libnanopb.a" "$out/libnanopb.a"
stat -c '%n %s bytes' "$out/libnanopb.a"
