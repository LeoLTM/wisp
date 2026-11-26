#!/bin/bash
# Uninstall wispd and remove the daemon completely.
# Usage: sudo bash uninstall.sh
set -euo pipefail

BINARY=/usr/local/bin/wispd
CONFIG_FILE=/etc/wispd.conf
STATE_DIR=/var/lib/wispd
SERVICE_FILE=/etc/systemd/system/wispd.service
SERVICE=wispd

if [[ $EUID -ne 0 ]]; then
    echo "Error: run as root (e.g. sudo bash uninstall.sh)" >&2
    exit 1
fi

# Prompt to keep or remove the config file and the pairing state.
KEEP_CONFIG=true
if [[ -f "$CONFIG_FILE" || -d "$STATE_DIR" ]]; then
    echo -n "Keep config ($CONFIG_FILE) and pairing state ($STATE_DIR)? [Y/n] "
    read -r REPLY </dev/tty
    if [[ "$REPLY" =~ ^[Nn]$ ]]; then
        KEEP_CONFIG=false
    fi
fi

echo "==> Stopping and disabling service..."
systemctl stop "$SERVICE" 2>/dev/null || true
systemctl disable "$SERVICE" 2>/dev/null || true

echo "==> Removing service file..."
rm -f "$SERVICE_FILE"
systemctl daemon-reload
systemctl reset-failed "$SERVICE" 2>/dev/null || true

echo "==> Removing binary..."
rm -f "$BINARY"

if [[ "$KEEP_CONFIG" == false ]]; then
    echo "==> Removing config and pairing state..."
    rm -f "$CONFIG_FILE"
    rm -rf "$STATE_DIR"
else
    echo "==> Config kept at $CONFIG_FILE, pairing state kept in $STATE_DIR"
fi

echo ""
echo "Uninstall complete."
