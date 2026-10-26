#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$here/vendor-build-common.sh"
src="$here/mbedtls-3.6.7"
for file in "$src"/library/*.c; do
    name=${file##*/}; name=${name%.c}
    case $name in
        x509*|pkcs7) lib=mbedx509 ;;
        ssl*|mps*|net_sockets|debug) lib=mbedtls ;;
        *) lib=mbedcrypto ;;
    esac
    mkdir -p "$work/$lib"
    "$cc" -Os -std=c99 -ffunction-sections -fdata-sections -fno-ident \
        "-ffile-prefix-map=$root/=" -I"$src/include" -I"$here/mbedconfig" \
        '-DMBEDTLS_CONFIG_FILE="diskos_mbedtls_config.h"' \
        -c "$file" -o "$work/$lib/$name.o"
done
for lib in mbedcrypto mbedx509 mbedtls; do
    "$ar" crD "$work/lib$lib.a" "$work/$lib/"*.o
    mv "$work/lib$lib.a" "$out/lib$lib.a"
    stat -c '%n %s bytes' "$out/lib$lib.a"
done
