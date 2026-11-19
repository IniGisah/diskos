#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$here/vendor-build-common.sh"
src="$here/monocypher-4.0.2/src"
for file in "$src/monocypher.c" "$src/optional/monocypher-ed25519.c"; do
    name=${file##*/}
    "$cc" -Os -std=c99 -ffunction-sections -fdata-sections -fno-ident \
        "-ffile-prefix-map=$root/=" -I"$src" -I"$src/optional" \
        -c "$file" -o "$work/${name%.c}.o"
done
"$ar" crD "$work/libmonocypher.a" "$work/"*.o
mv "$work/libmonocypher.a" "$out/libmonocypher.a"
stat -c '%n %s bytes' "$out/libmonocypher.a"
