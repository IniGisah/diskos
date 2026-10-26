#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
root=$(CDPATH= cd -- "$here/../.." && pwd)
case ${1:-} in
    host) cc=${CC:-gcc}; ar=${AR:-ar} ;;
    mipsel)
        cross=${CROSS:-mipsel-linux-musl-}
        if [ -z "${CROSS:-}" ] &&
           [ -x "$root/toolchain/build/mipsel-n2008-musl-cross/bin/mipsel-linux-musl-gcc" ]; then
            cross="$root/toolchain/build/mipsel-n2008-musl-cross/bin/mipsel-linux-musl-"
        fi
        cc="${cross}gcc"; ar="${cross}ar" ;;
    *) echo "usage: $0 host|mipsel" >&2; exit 2 ;;
esac
out="$here/out/$1"
mkdir -p "$out"
work=$(mktemp -d "${TMPDIR:-/tmp}/diskos-vendor.XXXXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
