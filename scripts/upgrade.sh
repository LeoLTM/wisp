#!/bin/bash
# Upgrade wispd to the latest release.
# Usage: curl -fsSL .../upgrade.sh | sudo bash
# Your config at /etc/wispd.conf is never modified.
set -euo pipefail

BINARY=/usr/local/bin/wispd
SERVICE_FILE=/etc/systemd/system/wispd.service
SERVICE=wispd
REPO="LeoLTM/wisp"
ARCHIVE=wisp-linux-armv6-release.tar.gz

if [[ $EUID -ne 0 ]]; then
    echo "Error: run as root (e.g. sudo bash upgrade.sh)" >&2
    exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "==> Downloading latest release..."
curl -fsSL "https://github.com/$REPO/releases/latest/download/$ARCHIVE" \
    -o "$TMP/$ARCHIVE"

echo "==> Extracting..."
tar -xzf "$TMP/$ARCHIVE" -C "$TMP"

OLD_SERVICE=sendspin-armv6
OLD_CONFIG=/etc/sendspin-armv6.conf
echo "==> Stopping service..."
if [[ -f /etc/systemd/system/$OLD_SERVICE.service ]]; then
    echo "==> Migrating legacy $OLD_SERVICE installation..."
    systemctl stop "$OLD_SERVICE" 2>/dev/null || true
    systemctl disable "$OLD_SERVICE" 2>/dev/null || true
    rm -f /etc/systemd/system/$OLD_SERVICE.service /usr/local/bin/sendspin-armv6
    systemctl daemon-reload
    systemctl reset-failed "$OLD_SERVICE" 2>/dev/null || true
    if [[ -f "$OLD_CONFIG" && ! -f /etc/wispd.conf ]]; then
        mv "$OLD_CONFIG" /etc/wispd.conf
    fi
    MIGRATED=true
fi
systemctl stop "$SERVICE" 2>/dev/null || true

echo "==> Installing binary..."
install -m 755 "$TMP/wispd" "$BINARY"

echo "==> Installing service file..."
install -m 644 "$TMP/wispd.service" "$SERVICE_FILE"
systemctl daemon-reload

echo "==> Starting service..."
systemctl reset-failed "$SERVICE" 2>/dev/null || true
[[ "${MIGRATED:-false}" == true ]] && systemctl enable "$SERVICE"
systemctl start "$SERVICE"

echo ""
echo "Done. Check status with:"
echo "  sudo systemctl status $SERVICE"
echo "  journalctl -u $SERVICE -f"
