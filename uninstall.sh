#!/usr/bin/env bash
# nm-socks5 uninstaller - removes both the /usr (template) and the
# /usr/local + bind-dirs (AppVM) layouts.

set -euo pipefail

msg() { echo "[nm-socks5] $*"; }
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }

# Unmount bind-dirs mounts if present
for f in /etc/NetworkManager/VPN/nm-socks5-service.name \
         /etc/dbus-1/system.d/nm-socks5-service.conf; do
    if mountpoint -q "$f" 2>/dev/null; then
        umount "$f" || true
    fi
    rm -f "$f"
done

rm -f  /rw/config/qubes-bind-dirs.d/50-nm-socks5.conf \
       /rw/bind-dirs/etc/NetworkManager/VPN/nm-socks5-service.name \
       /rw/bind-dirs/etc/dbus-1/system.d/nm-socks5-service.conf \
       /usr/lib/NetworkManager/VPN/nm-socks5-service.name \
       /usr/share/dbus-1/system.d/nm-socks5-service.conf \
       /usr/libexec/nm-socks5-service \
       /usr/local/libexec/nm-socks5-service
rm -rf /usr/lib/nm-socks5 /usr/local/lib/nm-socks5

systemctl reload dbus.service 2>/dev/null \
    || systemctl reload dbus-broker.service 2>/dev/null || true
if systemctl is-active --quiet NetworkManager; then
    systemctl restart NetworkManager
fi

msg "removed. Existing SOCKS5 connections can be deleted with:"
msg "  nmcli connection delete <name>"
