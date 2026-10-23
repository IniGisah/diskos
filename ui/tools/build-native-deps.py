#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the existing offline recipes with explicit source and output directories.

Sources may be a read-only shared tree. No configure, fetch or install step.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tarfile

def build(target, library, app, third_party, output):
    app = Path(app).resolve()
    tp = Path(third_party).resolve()
    output = Path(output).resolve()
    if target not in ('host', 'mipsel') or library not in (
            'freetype', 'mbedtls', 'monocypher', 'libplist', 'speexdsp'):
        raise ValueError('unknown native dependency or target')
    out = output / target
    out.mkdir(parents=True, exist_ok=True)
    common = (tp / 'vendor-build-common.sh').read_text()
    common = common.replace('out="$here/out/$1"', 'out=' + shlex.quote(str(out)))
    recipe = (tp / f'build-{library}.sh').read_text()
    if library == 'mbedtls':
        recipe = recipe.replace('diskos_mbedtls_config.h', 'airplay_mbedtls_config.h')
        recipe = recipe.replace('-I"$src/include"', '-I"$src/include" -I' + shlex.quote(str(app)))
    if library == 'freetype' and not (tp / 'freetype-2.13.3/src').exists():
        unpack = output / 'src'
        if not (unpack / 'freetype-2.13.3/src').exists():
            unpack.mkdir(parents=True, exist_ok=True)
            with tarfile.open(tp / 'freetype-2.13.3.tar.xz') as archive:
                archive.extractall(unpack, filter='data')
        recipe = recipe.replace('src="$here/freetype-2.13.3"',
                                'src=' + shlex.quote(str(unpack / 'freetype-2.13.3')))
    recipe = recipe.replace('here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)',
                            'here=' + shlex.quote(str(tp)))
    recipe = recipe.replace('. "$here/vendor-build-common.sh"', common)
    recipe = recipe.replace('out="$here/out/$target"', 'out=' + shlex.quote(str(out)))
    cross = os.environ.get('CROSS')
    if not cross:
        cross = 'mipsel-linux-musl-'
    recipe = recipe.replace('cross="$root/toolchain/build/mipsel-n2008-musl-cross/bin/mipsel-linux-musl-"',
                            'cross=' + shlex.quote(cross))
    subprocess.run(['sh', '-s', '--', target], input=recipe, text=True, check=True,
                   env={**os.environ, 'CROSS': cross})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=('host', 'mipsel'))
    parser.add_argument('library', choices=('freetype', 'mbedtls', 'monocypher', 'libplist', 'speexdsp'))
    parser.add_argument('--app', required=True)
    parser.add_argument('--third-party', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    build(args.target, args.library, args.app, args.third_party, args.output)
