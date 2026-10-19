#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Deploy diskOS Web Server to FiiO Snowsky Disc
# Usage: DISKOS_IP="10.10.10.145" DISKOS_PW="<pw>" ./web/deploy.sh

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
IP="${DISKOS_IP:-10.10.10.145}"
PW="${DISKOS_PW}"

if [ -z "$PW" ]; then
    printf "Enter device SSH password: "
    stty -echo
    read -r PW
    stty echo
    echo ""
fi

SSHPASS="sshpass -p $PW"
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5"

echo "==> Building diskos-webd..."
CROSS="${CROSS:-/home/luii/.local/toolchains/mipsel-linux-musl-cross/bin/mipsel-linux-musl-}"
make -C "$DIR/server" CROSS="$CROSS"

echo "==> Packing web assets..."
TARBALL="/tmp/diskos-web-assets.tar.gz"
tar -czf "$TARBALL" -C "$DIR" index.html style.css app.js

echo "==> Stopping running web daemon on device ($IP)..."
$SSHPASS ssh $SSH_OPTS root@"$IP" "/usr/data/bin/diskos-web.sh stop || true"

echo "==> Uploading diskos-webd binary..."
$SSHPASS ssh $SSH_OPTS root@"$IP" "cat > /tmp/diskos-webd.gz" < "$DIR/server/diskos-webd.gz"

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
/usr/data/bin/diskos-web.sh start
/usr/data/bin/diskos-web.sh status
'

echo "==> Deployed successfully! Open http://$IP:8080/ in your browser."

