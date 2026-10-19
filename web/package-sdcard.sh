#!/bin/sh
set -e

# Package diskOS Web UI for MicroSD card deployment (Path B)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DIST_DIR="$ROOT_DIR/dist"
PKG_DIR="$DIST_DIR/diskos"

echo "Building diskOS Web UI assets..."
cd "$ROOT_DIR/web"
tar -czf "$ROOT_DIR/payload/diskos-web-assets.tar.gz" index.html style.css app.js

echo "Staging files in $PKG_DIR..."
rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR"

# Copy binary, script, and assets
cp -f "$ROOT_DIR/payload/diskos-webd" "$PKG_DIR/diskos-webd"
chmod 755 "$PKG_DIR/diskos-webd"
cp -f "$ROOT_DIR/payload/diskos-web.sh" "$PKG_DIR/diskos-web.sh"
chmod 755 "$PKG_DIR/diskos-web.sh"
cp -f "$ROOT_DIR/payload/diskos-web-assets.tar.gz" "$PKG_DIR/diskos-web-assets.tar.gz"

# Create install.sh helper
cat << 'EOF' > "$PKG_DIR/install.sh"
#!/bin/sh
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
echo "Installing diskOS Web UI..."
mkdir -p /usr/data/bin /usr/data/web
cp -f "$DIR/diskos-webd" /usr/data/bin/
chmod 755 /usr/data/bin/diskos-webd
cp -f "$DIR/diskos-web.sh" /usr/data/bin/
chmod 755 /usr/data/bin/diskos-web.sh
if [ -f "$DIR/diskos-web-assets.tar.gz" ]; then
    gunzip -c "$DIR/diskos-web-assets.tar.gz" | tar -x -C /usr/data/web
elif [ -f "$DIR/web.tar.gz" ]; then
    gunzip -c "$DIR/web.tar.gz" | tar -x -C /usr/data/web
elif [ -d "$DIR/web" ]; then
    cp -rf "$DIR/web/"* /usr/data/web/
fi
echo "diskOS Web UI installed successfully to /usr/data!"
echo "You can manage it in Apps -> Web Server on the device screen,"
echo "or run: /usr/data/bin/diskos-web.sh {start|stop|status}"
EOF
chmod 755 "$PKG_DIR/install.sh"

# Create README.txt
cat << 'EOF' > "$PKG_DIR/README.txt"
diskOS Web UI - MicroSD Card Installation Package
=================================================

Installation Instructions:
--------------------------
Method 1 (Automatic via Device Screen):
1. Copy this entire "diskos" folder to the root of your MicroSD card.
2. Insert the MicroSD card into your FiiO Snowsky Disc player.
3. Turn on the device and open Apps -> Web Server.
4. The screen will display "Status: SD READY".
5. Tap "Install & Start". diskOS will automatically install the server and launch it!

Method 2 (Terminal / SSH / Serial):
1. Insert the MicroSD card into the player.
2. Run: sh /tmp/sdcard/diskos/install.sh
3. Start the server with: /usr/data/bin/diskos-web.sh start

Accessing Web UI:
-----------------
Make sure your player is connected to the same Wi-Fi network as your computer or phone.
Open your browser and navigate to:
    http://<device-ip>:8080/
(The device IP is displayed on the Web Server screen in Settings).
Enter your PIN/password (default is 1234, configurable on-device or via diskos.conf).
EOF

cd "$DIST_DIR"
rm -f diskos-web-sdcard.zip diskos-web-sdcard.tar.gz
python3 -c "import zipfile, os; z = zipfile.ZipFile('diskos-web-sdcard.zip', 'w', zipfile.ZIP_DEFLATED); [z.write(os.path.join(r, f), os.path.relpath(os.path.join(r, f), '.')) for r, d, fs in os.walk('diskos') for f in fs]; z.close()"
tar -czf diskos-web-sdcard.tar.gz diskos/

echo "Package created:"
echo " - $DIST_DIR/diskos-web-sdcard.zip"
echo " - $DIST_DIR/diskos-web-sdcard.tar.gz"
