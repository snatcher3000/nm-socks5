#!/usr/bin/env bash
# nm-socks5 installer - Qubes OS aware.
#
# Usage (inside the VM where NetworkManager runs, e.g. sys-vpn, or inside
# the template it is based on):
#
#   sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/CHANGEME/nm-socks5/main/install.sh)"
#
# In a Qubes TemplateVM (no direct network) prefix the curl with the update
# proxy:  curl -fsSL --proxy http://127.0.0.1:8082 ...
#
# Install modes, detected via qubesdb-read /qubes-vm-persistence:
#   full     (TemplateVM / StandaloneVM / non-Qubes): install under /usr
#   rw-only  (AppVM like sys-vpn): install under /usr/local (persistent)
#            and register the two files that must live in /etc via
#            qubes-bind-dirs so they survive reboots.

set -euo pipefail

REPO_TARBALL="${NM_SOCKS5_TARBALL:-https://github.com/CHANGEME/nm-socks5/archive/refs/heads/main.tar.gz}"
TUN2SOCKS_VERSION="${TUN2SOCKS_VERSION:-v2.5.2}"
QUBES_PROXY="http://127.0.0.1:8082"

msg()  { echo "[nm-socks5] $*"; }
fail() { echo "[nm-socks5] ERROR: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root: sudo bash install.sh"

fetch() { # fetch URL OUTFILE - try direct, fall back to the Qubes update proxy
    local url="$1" out="$2"
    if curl -fsSL --connect-timeout 15 -o "$out" "$url" 2>/dev/null; then
        return 0
    fi
    msg "direct download failed, retrying via Qubes update proxy ..."
    curl -fsSL --proxy "$QUBES_PROXY" -o "$out" "$url" \
        || fail "could not download $url"
}

# --------------------------------------------------------------------------
# Bootstrap: when run via `bash -c "$(curl ...)"` there is no source tree
# next to us - download the repo tarball and re-exec from there.
# --------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-/nonexistent}")" 2>/dev/null && pwd || echo /nonexistent)"
if [ ! -f "$SCRIPT_DIR/service/nm-socks5-service.py" ]; then
    command -v curl >/dev/null || fail "curl is required for bootstrap"
    msg "fetching source tree from $REPO_TARBALL"
    WORKDIR="$(mktemp -d)"
    fetch "$REPO_TARBALL" "$WORKDIR/src.tar.gz"
    tar -xzf "$WORKDIR/src.tar.gz" -C "$WORKDIR"
    SRCDIR="$(find "$WORKDIR" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
    [ -n "$SRCDIR" ] || fail "unexpected tarball layout"
    exec bash "$SRCDIR/install.sh" "$@"
fi
cd "$SCRIPT_DIR"

# --------------------------------------------------------------------------
# Dependencies
# --------------------------------------------------------------------------
msg "installing build/runtime dependencies"
if command -v dnf >/dev/null; then
    dnf install -y gcc make pkgconf-pkg-config NetworkManager-libnm-devel \
        gtk3-devel python3-gobject unzip curl
elif command -v apt-get >/dev/null; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y build-essential pkg-config libnm-dev libgtk-3-dev \
        python3-gi gir1.2-nm-1.0 unzip curl ca-certificates
else
    fail "no supported package manager found (dnf/apt-get)"
fi

# --------------------------------------------------------------------------
# Build the editor plugin
# --------------------------------------------------------------------------
msg "building libnm-vpn-plugin-socks5.so"
make clean >/dev/null 2>&1 || true
make

# --------------------------------------------------------------------------
# Choose layout
# --------------------------------------------------------------------------
PERSISTENCE="full"
if command -v qubesdb-read >/dev/null; then
    PERSISTENCE="$(qubesdb-read /qubes-vm-persistence 2>/dev/null || echo full)"
fi

if [ "$PERSISTENCE" = "rw-only" ]; then
    msg "Qubes AppVM detected: installing to /usr/local + bind-dirs"
    LIB_DIR="/usr/local/lib/nm-socks5"
    PROGRAM="/usr/local/libexec/nm-socks5-service"
else
    msg "TemplateVM/StandaloneVM/regular system: installing to /usr"
    LIB_DIR="/usr/lib/nm-socks5"
    PROGRAM="/usr/libexec/nm-socks5-service"
fi
PLUGIN_SO="$LIB_DIR/libnm-vpn-plugin-socks5.so"

# --------------------------------------------------------------------------
# tun2socks (single static Go binary, from upstream GitHub releases)
# --------------------------------------------------------------------------
if [ ! -x "$LIB_DIR/tun2socks" ]; then
    case "$(uname -m)" in
        x86_64)  T2S_ARCH="amd64" ;;
        aarch64) T2S_ARCH="arm64" ;;
        *) fail "unsupported architecture: $(uname -m)" ;;
    esac
    T2S_URL="https://github.com/xjasonlyu/tun2socks/releases/download/${TUN2SOCKS_VERSION}/tun2socks-linux-${T2S_ARCH}.zip"
    msg "downloading tun2socks ${TUN2SOCKS_VERSION} (${T2S_ARCH})"
    T2S_TMP="$(mktemp -d)"
    fetch "$T2S_URL" "$T2S_TMP/t2s.zip"
    unzip -q -o "$T2S_TMP/t2s.zip" -d "$T2S_TMP"
    install -D -m 755 "$T2S_TMP/tun2socks-linux-${T2S_ARCH}" "$LIB_DIR/tun2socks"
    rm -rf "$T2S_TMP"
else
    msg "tun2socks already present at $LIB_DIR/tun2socks, keeping it"
fi

# --------------------------------------------------------------------------
# Install plugin, service, .name file, D-Bus policy
# --------------------------------------------------------------------------
install -D -m 644 libnm-vpn-plugin-socks5.so "$PLUGIN_SO"
install -D -m 755 service/nm-socks5-service.py "$PROGRAM"

NAME_CONTENT="$(sed -e "s|@PROGRAM@|$PROGRAM|" -e "s|@PLUGIN@|$PLUGIN_SO|" \
    data/nm-socks5-service.name.in)"

if [ "$PERSISTENCE" = "rw-only" ]; then
    # Real files live under /rw (persistent); bind-dirs mounts them over /etc.
    install -d /rw/bind-dirs/etc/NetworkManager/VPN \
               /rw/bind-dirs/etc/dbus-1/system.d \
               /rw/config/qubes-bind-dirs.d \
               /etc/NetworkManager/VPN
    echo "$NAME_CONTENT" > /rw/bind-dirs/etc/NetworkManager/VPN/nm-socks5-service.name
    install -m 644 data/nm-socks5-dbus.conf \
        /rw/bind-dirs/etc/dbus-1/system.d/nm-socks5-service.conf
    cat > /rw/config/qubes-bind-dirs.d/50-nm-socks5.conf <<'EOF'
binds+=( '/etc/NetworkManager/VPN/nm-socks5-service.name' )
binds+=( '/etc/dbus-1/system.d/nm-socks5-service.conf' )
EOF
    # bind-dirs needs existing mount targets
    touch /etc/NetworkManager/VPN/nm-socks5-service.name \
          /etc/dbus-1/system.d/nm-socks5-service.conf
    if [ -x /usr/lib/qubes/init/bind-dirs.sh ]; then
        /usr/lib/qubes/init/bind-dirs.sh || true
    fi
    # Fallback if bind-dirs did not mount (e.g. older Qubes)
    for f in /etc/NetworkManager/VPN/nm-socks5-service.name \
             /etc/dbus-1/system.d/nm-socks5-service.conf; do
        if ! mountpoint -q "$f"; then
            mount --bind "/rw/bind-dirs$f" "$f"
        fi
    done
else
    install -d /usr/lib/NetworkManager/VPN /usr/share/dbus-1/system.d
    echo "$NAME_CONTENT" > /usr/lib/NetworkManager/VPN/nm-socks5-service.name
    chmod 644 /usr/lib/NetworkManager/VPN/nm-socks5-service.name
    install -m 644 data/nm-socks5-dbus.conf \
        /usr/share/dbus-1/system.d/nm-socks5-service.conf
fi

# --------------------------------------------------------------------------
# Activate
# --------------------------------------------------------------------------
msg "reloading D-Bus policy"
systemctl reload dbus.service 2>/dev/null \
    || systemctl reload dbus-broker.service 2>/dev/null || true

if systemctl is-active --quiet NetworkManager; then
    msg "restarting NetworkManager (connectivity drops for a moment)"
    systemctl restart NetworkManager
fi

msg "done."
if [ "$PERSISTENCE" = "rw-only" ]; then
    msg "Installed persistently in this AppVM (survives reboots via bind-dirs)."
else
    msg "If this is a TemplateVM: shut it down, then restart sys-vpn."
fi
msg 'Create a connection: right-click nm-applet -> Edit Connections -> + -> "SOCKS5 Proxy (tun2socks)"'
msg 'Or via CLI: nmcli connection add type vpn vpn-type socks5 con-name my-proxy \'
msg '            vpn.data "server=1.2.3.4,port=1080,username=me,dns-through=yes,dns-server=1.1.1.1" \'
msg '            vpn.secrets "password=secret"'
