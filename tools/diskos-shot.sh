#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# diskos-shot.sh - capture the device screen to a PNG over SSH.
#
# The panel is 360x360, 32bpp, BGRA byte order, and mounted rotated 180 degrees, so we read the raw
# framebuffer (/dev/fb0), reinterpret it as BGRA, and rotate 180. One frame is exactly 518400 bytes
# (360 * 360 * 4). A capture during a live UI redraw can show mild tearing; re-shoot if needed.
#
# Usage:
#   DISKOS_IP=<device-ip> DISKOS_PW=<debug-mode-password> ./diskos-shot.sh [--circle] [out.png]
#
# Pass --circle (or set DISKOS_SHOT_CIRCLE=1) to apply an anti-aliased circular alpha mask matching
# the physical round bezel. Default without flag is square RGB framebuffer.
#
# Get the IP and password from Debug Mode (Settings > System).
# Requires: sshpass, ssh, python3 with Pillow (pip install Pillow). Default output: diskos-shot.png
set -euo pipefail

IP="${DISKOS_IP:?set DISKOS_IP to the device IP (shown in Debug Mode)}"
export SSHPASS="${DISKOS_PW:?set DISKOS_PW to the Debug Mode SSH password}"

CIRCLE="${DISKOS_SHOT_CIRCLE:-0}"
OUT="diskos-shot.png"
for arg in "$@"; do
  case "$arg" in
    --circle|-c) CIRCLE=1 ;;
    --square|-s) CIRCLE=0 ;;
    *) OUT="$arg" ;;
  esac
done
RAW="$(mktemp)"
trap 'rm -f "$RAW"' EXIT

echo ">> Capturing screen (360x360 32bpp, 518.4 KB) from root@$IP..."

# password via SSHPASS (sshpass -e), not the command line
# -o LogLevel=ERROR suppresses non-fatal warnings (e.g. OpenSSH post-quantum key exchange warnings)
if command -v pv >/dev/null 2>&1; then
  sshpass -e ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=8 -o LogLevel=ERROR "root@$IP" \
    'dd if=/dev/fb0 bs=518400 count=1 2>/dev/null' | pv -N "   Capture" -p -t -r -b -s 518400 > "$RAW"
else
  sshpass -e ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=8 -o LogLevel=ERROR "root@$IP" \
    'dd if=/dev/fb0 bs=518400 count=1 2>/dev/null' > "$RAW"
fi

# portable byte count (stat -c is GNU-only); require a full frame
BYTES="$(wc -c < "$RAW" | tr -d ' ')"
[ "$BYTES" -ge 518400 ] || { echo "ERROR: capture failed (short read: $BYTES bytes, expected 518400)" >&2; exit 1; }

echo ">> Processing frame (BGRA -> RGB, 180° rotation, circle=${CIRCLE})..."
python3 - "$RAW" "$OUT" "$CIRCLE" <<'PY'
import sys
from PIL import Image, ImageDraw

raw = open(sys.argv[1], "rb").read()[:518400]
img = Image.frombuffer("RGBA", (360, 360), raw, "raw", "BGRA").rotate(180)

if sys.argv[3] == "1":
    scale = 4
    mask_large = Image.new("L", (360 * scale, 360 * scale), 0)
    draw = ImageDraw.Draw(mask_large)
    draw.ellipse((0, 0, 360 * scale - 1, 360 * scale - 1), fill=255)
    mask = mask_large.resize((360, 360), Image.Resampling.LANCZOS)
    img.putalpha(mask)
else:
    img = img.convert("RGB")

img.save(sys.argv[2], optimize=True)
PY
echo ">> Screenshot saved: $OUT"
