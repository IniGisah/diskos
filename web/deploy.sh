#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Deploy diskOS Web Server to FiiO Snowsky Disc
# Usage: DISKOS_IP="<device-ip>" DISKOS_PW="<pw>" ./web/deploy.sh [device-ip]

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
IP="${DISKOS_IP:-${1:-}}"

if [ -z "$IP" ]; then
    printf "Enter device IP (shown in Web UI / Debug Mode): "
    read -r IP
fi
[ -n "$IP" ] || { echo "Device IP is required" >&2; exit 1; }

PW="${DISKOS_PW:-${2:-}}"

if [ -z "$PW" ]; then
    printf "Enter device SSH password: "
    stty -echo
    read -r PW
    stty echo
    echo ""
fi

SSHPASS="sshpass -p $PW"
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR"

echo "==> Building diskos-webd..."
CROSS="${CROSS:-/home/luii/.local/toolchains/mipsel-linux-musl-cross/bin/mipsel-linux-musl-}"
make -C "$DIR/server" CROSS="$CROSS"

echo "==> Packing web assets..."
TARBALL="/tmp/diskos-web-assets.tar.gz"
tar -czf "$TARBALL" -C "$DIR" index.html style.css app.js

echo "==> Stopping running web daemon on device ($IP)..."
$SSHPASS ssh $SSH_OPTS root@"$IP" "/usr/data/bin/diskos-web.sh stop 2>/dev/null || true"

echo "==> Uploading diskos-webd binary..."
$SSHPASS ssh $SSH_OPTS root@"$IP" "cat > /tmp/diskos-webd.gz" < "$DIR/server/diskos-webd.gz"

echo "==> Uploading diskos-web.sh script..."
$SSHPASS ssh $SSH_OPTS root@"$IP" "cat > /usr/data/bin/diskos-web.sh && chmod 755 /usr/data/bin/diskos-web.sh" < "$DIR/../payload/diskos-web.sh"

echo "==> Uploading web assets..."
$SSHPASS ssh $SSH_OPTS root@"$IP" "cat > /tmp/diskos-web-assets.tar.gz" < "$TARBALL"
rm -f "$TARBALL"

echo "==> Installing and starting diskOS Web Server..."
$SSHPASS ssh $SSH_OPTS root@"$IP" '
mkdir -p /usr/data/bin /usr/data/web
gunzip -c /tmp/diskos-webd.gz > /usr/data/bin/diskos-webd
chmod 755 /usr/data/bin/diskos-webd
gunzip -c /tmp/diskos-web-assets.tar.gz | tar -x -C /usr/data/web/
rm -f /tmp/diskos-webd.gz /tmp/diskos-web-assets.tar.gz
/usr/data/bin/diskos-web.sh restart
/usr/data/bin/diskos-web.sh status
'

echo "==> Deployed successfully! Open http://$IP:8080/ in your browser."

