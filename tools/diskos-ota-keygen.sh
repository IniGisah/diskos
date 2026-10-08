#!/usr/bin/env bash
# diskos-ota-keygen.sh - generate ECDSA P-256 root and leaf keypairs for custom diskOS OTA signing.
#
# Keys are stored in the repo's signing/ directory
#
# Usage:
#   ./tools/diskos-ota-keygen.sh [--force]
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SIGN_DIR="$REPO_ROOT/signing"

FORCE=0
if [ "${1:-}" = "--force" ] || [ "${1:-}" = "-f" ]; then
    FORCE=1
fi

if ! command -v openssl >/dev/null 2>&1; then
    echo "ERROR: openssl is required but not installed." >&2
    exit 1
fi

if [ -f "$SIGN_DIR/root_priv.pem" ] && [ "$FORCE" -eq 0 ]; then
    echo "Keys already exist in $SIGN_DIR."
    echo "To regenerate (WARNING: will invalidate existing signed bundles), pass --force:"
    echo "  $0 --force"
    exit 0
fi

mkdir -p "$SIGN_DIR"

echo ">> Generating ECDSA P-256 root keypair (trust anchor)..."
openssl ecparam -name prime256v1 -genkey -noout -out "$SIGN_DIR/root_priv.pem"
openssl ec -in "$SIGN_DIR/root_priv.pem" -pubout -out "$SIGN_DIR/root_pub.pem" 2>/dev/null
chmod 600 "$SIGN_DIR/root_priv.pem"

echo ">> Generating ECDSA P-256 leaf keypair (update signer)..."
openssl ecparam -name prime256v1 -genkey -noout -out "$SIGN_DIR/leaf_priv.pem"
openssl ec -in "$SIGN_DIR/leaf_priv.pem" -pubout -out "$SIGN_DIR/leaf_pub.pem" 2>/dev/null
chmod 600 "$SIGN_DIR/leaf_priv.pem"

echo ""
echo "=== Key generation complete! ==="
echo "Files created in $SIGN_DIR/ (ignored by .gitignore):"
echo "  - root_priv.pem : Private root key (KEEP SECRET, stay on your PC)"
echo "  - root_pub.pem  : Public root key (bake this into the device rootfs)"
echo "  - leaf_priv.pem : Private leaf signing key"
echo "  - leaf_pub.pem  : Public leaf key"
echo ""
echo "Next step (Flash once with your root key):"
echo "  ./diskos-installer install \\"
echo "    --firmware SNOWSKY_DISC_update_*.zip \\"
echo "    --ota-key signing/root_pub.pem \\"
echo "    --variant public"
echo ""
echo "After that initial flash, use ./tools/diskos-ota-push.sh to deploy permanent updates over Wi-Fi."
