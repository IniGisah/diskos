#!/bin/sh
# Build the minimal static FreeType library for the stock TrueType Collection.
# FreeType is used under the FreeType License in freetype-2.13.3/docs/FTL.TXT.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
src="$here/freetype-2.13.3"
cfg="$here/ftconfig"

if [ "$#" -ne 1 ] || { [ "$1" != host ] && [ "$1" != mipsel ]; }; then
    echo "usage: $0 host|mipsel" >&2
    exit 2
fi
target=$1
out="$here/out/$target"
if [ "$target" = mipsel ]; then
    cross=${CROSS:-mipsel-linux-musl-}
    cc="${cross}gcc"
    ar="${cross}ar"
else
    cc=gcc
    ar=ar
fi
mkdir -p "$out"

# Each FreeType component is its upstream single-object amalgamation. The
# bytecode interpreter supplies hinting; autofit is not used by this TTC.
sources='base/ftbase base/ftinit base/ftsystem base/ftdebug base/ftmm base/ftbitmap truetype/truetype sfnt/sfnt smooth/smooth'
objects=''
for component in $sources; do
    name=${component##*/}
    object="$out/$name.o"
    "$cc" -Os -pipe -std=gnu11 -ffunction-sections -fdata-sections \
        -ffp-contract=off -fno-ident \
        "-ffile-prefix-map=$here/=third_party/" \
        -DFT2_BUILD_LIBRARY \
        '-DFT_CONFIG_OPTIONS_H=<ftoption.h>' \
        '-DFT_CONFIG_MODULES_H=<ftmodule.h>' \
        -I"$cfg" -I"$src/include" -I"$src/src/${component%/*}" \
        -c "$src/src/$component.c" -o "$object"
    objects="$objects $object"
done
# Replace the archive atomically; old member lists cannot survive a rebuild.
archive="$out/libfreetype.a"
tmp="$out/libfreetype.a.tmp"
rm -f "$tmp"
# Shell splitting here is intentional: object paths are fixed by this tree.
# shellcheck disable=SC2086
"$ar" crD "$tmp" $objects
mv -f "$tmp" "$archive"
stat -c '%n %s bytes' "$archive"
