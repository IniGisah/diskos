#!/usr/bin/env bash
# diskos-deploy.sh - push locally built diskOS binaries (mq_ui, diskos-webd, scripts, web assets)
# to the device and hot-reload them, safely.
#
# This is for iterating on UI and Web UI changes. A hand-deployed binary reverts to the flashed build on the
# next reboot (that is intentional). To bake a build in permanently, flash it with the installer.
#
# Usage:
#   DISKOS_IP=<device-ip> DISKOS_PW=<password> ./tools/diskos-deploy.sh [path/to/mq_ui]
#
# If DISKOS_IP is unset, prompts interactively.
# If DISKOS_PW is unset, prompts interactively.
#
# Requires: sshpass, ssh, md5sum. Default binary path: auto-detected (ui/mq_ui, ./mq_ui, payload/mq_ui)
set -euo pipefail

if [ -z "${DISKOS_IP:-}" ]; then
  read -rp "Enter device IP (shown in Debug Mode / Web UI): " DISKOS_IP
fi
IP="${DISKOS_IP:?set DISKOS_IP to the device IP}"

if [ -z "${DISKOS_PW:-}" ]; then
  read -rsp "Enter device SSH password: " DISKOS_PW
  echo ""
fi
export SSHPASS="$DISKOS_PW"

BIN="${1:-}"
if [ -z "$BIN" ]; then
  if [ -f "ui/mq_ui" ]; then BIN="ui/mq_ui"
  elif [ -f "./mq_ui" ]; then BIN="./mq_ui"
  elif [ -f "payload/mq_ui" ]; then BIN="payload/mq_ui"
  else echo "binary not found: please build ui/mq_ui or pass path" >&2; exit 1
  fi
fi
[ -f "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 1; }

BIN_SIZE="$(wc -c < "$BIN" | tr -d ' ')"
BIN_SIZE_MB="$(awk -v s="$BIN_SIZE" 'BEGIN { printf "%.2f", s / 1048576 }')"
MD5="$(md5sum "$BIN" | cut -d' ' -f1)"

# Locate Web UI components if available
WEBD_BIN=""
if [ -f "web/server/diskos-webd" ]; then WEBD_BIN="web/server/diskos-webd"
elif [ -f "payload/diskos-webd" ]; then WEBD_BIN="payload/diskos-webd"
fi

WEBSH_BIN=""
if [ -f "payload/diskos-web.sh" ]; then WEBSH_BIN="payload/diskos-web.sh"
elif [ -f "web/diskos-web.sh" ]; then WEBSH_BIN="web/diskos-web.sh"
fi

WEB_DIR=""
if [ -d "web" ] && [ -f "web/index.html" ]; then WEB_DIR="web"
fi

# password via SSHPASS (sshpass -e), not the command line, so it is not visible in the process list.
# -o LogLevel=ERROR suppresses non-fatal warnings (e.g. OpenSSH post-quantum key exchange warnings).
SSH=(sshpass -e ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=8 -o LogLevel=ERROR)

echo ">> Target:      root@$IP"
echo ">> Binary:      $BIN (${BIN_SIZE_MB} MB, md5: $MD5)"
if [ -n "$WEBD_BIN" ]; then echo ">> Web daemon:  $WEBD_BIN"; fi
if [ -n "$WEBSH_BIN" ]; then echo ">> Web script:  $WEBSH_BIN"; fi
if [ -n "$WEB_DIR" ]; then echo ">> Web assets:  $WEB_DIR/"; fi

stream_with_progress() {
  local file="$1"
  local size="$2"
  if command -v pv >/dev/null 2>&1; then
    pv -N "   Upload" -p -t -e -r -b -s "$size" "$file"
    return
  fi

  if command -v python3 >/dev/null 2>&1; then
    python3 -c '
import sys, time, os
path = sys.argv[1]
size = int(sys.argv[2])
sent = 0
chunk = 65536
t0 = time.time()
with open(path, "rb") as f:
    while True:
        buf = f.read(chunk)
        if not buf:
            break
        sys.stdout.buffer.write(buf)
        sent += len(buf)
        now = time.time()
        elapsed = max(now - t0, 0.001)
        speed = sent / elapsed
        pct = (sent / size) * 100 if size else 100
        mb_sent = sent / 1048576
        mb_tot = size / 1048576
        bar_len = 25
        filled = int(bar_len * sent / size) if size else bar_len
        bar = "=" * filled + (">" if filled < bar_len else "=") + " " * max(0, bar_len - filled - 1)
        sys.stderr.write(f"\r   Upload: [{bar[:bar_len]}] {pct:5.1f}% {mb_sent:4.1f}/{mb_tot:4.1f}MB ({speed/1048576:.2f}MB/s)")
        sys.stderr.flush()
sys.stderr.write("\n")
' "$file" "$size"
    return
  fi

  cat "$file"
}

# Ensure destination directories exist on device
"${SSH[@]}" "root@$IP" "mkdir -p /usr/data/bin /usr/data/web"

# 1. Stream mq_ui to a staging path with progress bar and verify exact md5 before touching live binary
echo ">> [1/4] Uploading UI binary to device (/usr/data/mq_ui.new)..."
REMOTE_MD5="$(stream_with_progress "$BIN" "$BIN_SIZE" | "${SSH[@]}" "root@$IP" \
  'cat > /usr/data/mq_ui.new && chmod 755 /usr/data/mq_ui.new && md5sum /usr/data/mq_ui.new | cut -d" " -f1')"
REMOTE_MD5="$(echo "$REMOTE_MD5" | tr -d '\r\n ')"

if [ "$REMOTE_MD5" != "$MD5" ]; then
  echo "ERROR: md5 mismatch (local=$MD5, remote=$REMOTE_MD5) - aborting, live binary untouched" >&2
  exit 1
fi
echo "   Integrity verified (remote md5 matches local: $REMOTE_MD5)"

# 2. Deploy Web UI daemon and script if present
if [ -n "$WEBD_BIN" ]; then
  echo ">> [2/4] Uploading diskos-webd daemon (/usr/data/bin/diskos-webd)..."
  "${SSH[@]}" "root@$IP" 'cat > /usr/data/bin/diskos-webd && chmod 755 /usr/data/bin/diskos-webd' < "$WEBD_BIN"
fi
if [ -n "$WEBSH_BIN" ]; then
  "${SSH[@]}" "root@$IP" 'cat > /usr/data/bin/diskos-web.sh && chmod 755 /usr/data/bin/diskos-web.sh' < "$WEBSH_BIN"
fi

# 3. Deploy Web static assets if present
if [ -n "$WEB_DIR" ]; then
  echo ">> [3/4] Uploading Web UI assets (/usr/data/web/)..."
  tar -c -C "$WEB_DIR" index.html style.css app.js | "${SSH[@]}" "root@$IP" 'tar -x -C /usr/data/web/'
fi

# 4. Swap in and hot-reload, in a failure-aware order.
#    IMPORTANT: only ever touch mq_ui. NEVER kill mq_player - that frees the SD card and the
#    hardware MCU reboots the whole device (~10s).
#    The fiio_init watchdog does `pgrep -x mq_ui` (matches the full argv[0]); if we kill mq_ui
#    without immediately relaunching a DETACHED replacement it respawns the STOCK /usr/bin/mq_ui.
#    We relaunch with setsid </dev/null, then POLL until our build is confirmed alive, and only
#    THEN prune any stock instance. If our build never comes up (a bad binary), we do NOT prune -
#    the watchdog restores the stock UI as a working fallback.
echo ">> [4/4] Swapping in binary and hot-reloading UI & Web daemon..."
"${SSH[@]}" "root@$IP" 'sh -s' <<'REMOTE'
echo "   [1/6] Staging binary (/usr/data/mq_ui.new -> /usr/data/mq_ui)..."
mv -f /usr/data/mq_ui.new /usr/data/mq_ui || { echo "ERROR: could not swap in the new binary" >&2; exit 1; }

echo "   [2/6] Stopping previous mq_ui..."
killall -9 mq_ui 2>/dev/null

echo "   [3/6] Launching /usr/data/mq_ui..."
setsid /usr/data/mq_ui </dev/null >/usr/data/diskos_boot.log 2>&1 &

# poll up to ~10s. Sleep FIRST, then check, so a check is always the last action before we decide
echo "   [4/6] Waiting for /usr/data/mq_ui to initialize..."
ok=0
i=0
target_pid=""
while [ "$i" -lt 10 ]; do
  sleep 1
  for p in $(pidof mq_ui 2>/dev/null); do
    if [ "$(readlink /proc/$p/exe 2>/dev/null)" = /usr/data/mq_ui ]; then
      ok=1
      target_pid="$p"
      break
    fi
  done
  if [ "$ok" = 1 ]; then break; fi
  i=$((i + 1))
  echo "         Waiting for UI to come up... (${i}s/10s)"
done
if [ "$ok" != 1 ]; then
  echo "ERROR: /usr/data/mq_ui did not come up after 10s - NOT pruning; the watchdog will restore stock" >&2
  exit 1
fi
echo "         /usr/data/mq_ui is running (PID: $target_pid)"

# our build is confirmed running -> prune ONLY a genuine stock instance (exe is EXACTLY /usr/bin/mq_ui)
echo "   [5/6] Checking process state & pruning competing stock UI..."
pruned=0
for p in $(pidof mq_ui 2>/dev/null); do
  if [ "$(readlink /proc/$p/exe 2>/dev/null)" = /usr/bin/mq_ui ]; then
    kill -9 "$p" 2>/dev/null
    pruned=$((pruned + 1))
  fi
done
if [ "$pruned" -gt 0 ]; then
  echo "         Pruned $pruned competing stock instance(s)"
fi

# verify: no stock instance survived the prune
stock=0
for p in $(pidof mq_ui 2>/dev/null); do
  if [ "$(readlink /proc/$p/exe 2>/dev/null)" = /usr/bin/mq_ui ]; then stock=1; fi
done
if [ "$stock" = 1 ]; then
  echo "ERROR: a stock mq_ui is still running after prune (check the device)" >&2
  exit 1
fi

echo "   [6/6] Restarting diskOS Web Server daemon..."
if [ -x /usr/data/bin/diskos-web.sh ]; then
  /bin/sh /usr/data/bin/diskos-web.sh restart >/dev/null 2>&1 || true
fi

player_pid="$(pidof mq_player 2>/dev/null || true)"
if [ -n "$player_pid" ]; then
  echo ">> Deployed successfully: /usr/data/mq_ui running (PID $target_pid); mq_player alive (PID $player_pid)"
else
  echo ">> Deployed: /usr/data/mq_ui running (PID $target_pid), but mq_player is NOT running (check the device)" >&2
fi
REMOTE
