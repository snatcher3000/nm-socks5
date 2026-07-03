#!/usr/bin/python3
# SPDX-License-Identifier: MIT
"""nm-socks5-service - NetworkManager VPN service plugin for SOCKS5 proxies.

Launched by NetworkManager (see the .name file). Owns the D-Bus name
org.freedesktop.NetworkManager.socks5, starts tun2socks to create a TUN
device whose traffic is forwarded to the configured SOCKS5 server, and
hands the IP/DNS configuration back to NetworkManager, which then sets
up addressing, the default route and a host route to the proxy server
via the physical uplink (so tun2socks' own proxy connection does not
loop through the tunnel).

Connection data items (NMSettingVpn):
    server       SOCKS5 server hostname or IPv4 address (required)
    port         TCP port, default 1080
    username     optional
    dns-through  "yes"/"no", default "yes": push DNS through the tunnel
    dns-server   comma/space separated IPv4 list, default 1.1.1.1
    tun-ip       local TUN address override, default 10.255.255.2
    tun-peer     TUN point-to-point peer override, default 10.255.255.1
    mtu          MTU override, default 1500
Secrets:
    password     optional
"""

import os
import shutil
import socket
import struct
import subprocess
import sys
import urllib.parse

import gi
gi.require_version("NM", "1.0")
from gi.repository import GLib, NM  # noqa: E402

DBUS_SERVICE = "org.freedesktop.NetworkManager.socks5"
TUN_DEV = "nm-socks5"
DEFAULT_TUN_IP = "10.255.255.2"
DEFAULT_TUN_PEER = "10.255.255.1"
DEFAULT_PORT = 1080
DEFAULT_DNS = "1.1.1.1"
DEFAULT_MTU = 1500

TUN2SOCKS_CANDIDATES = [
    "/usr/local/lib/nm-socks5/tun2socks",
    "/usr/lib/nm-socks5/tun2socks",
    "/opt/nm-socks5/tun2socks",
]

DEVICE_WAIT_INTERVAL_MS = 200
DEVICE_WAIT_TRIES = 25  # 5 seconds


def log(msg):
    # NetworkManager forwards the plugin's stderr to the journal
    print("nm-socks5: %s" % msg, file=sys.stderr, flush=True)


def plugin_error(msg, code=NM.VpnPluginError.LAUNCH_FAILED):
    log("error: %s" % msg)
    return GLib.Error.new_literal(NM.vpn_plugin_error_quark(), msg, code)


def ip4_to_uint32(text):
    """Convert dotted-quad to the host-endian uint32 NetworkManager expects
    (the raw in_addr bytes reinterpreted as a native guint32)."""
    return struct.unpack("=I", socket.inet_aton(text))[0]


def find_tun2socks():
    for path in TUN2SOCKS_CANDIDATES:
        if os.access(path, os.X_OK):
            return path
    return shutil.which("tun2socks")


class Socks5Plugin(NM.VpnServicePlugin):
    def __init__(self, bus_name):
        super().__init__(service_name=bus_name, watch_peer=True)
        self._proc = None
        self._proc_watch_id = 0
        self._device_wait_id = 0
        self._pending = None  # config to push once the TUN device exists

    # ------------------------------------------------------------------ #
    # NMVpnServicePlugin virtual methods                                  #
    # ------------------------------------------------------------------ #

    def do_connect(self, connection):
        s_vpn = connection.get_setting_vpn()
        if s_vpn is None:
            raise plugin_error("connection lacks VPN settings",
                               NM.VpnPluginError.INVALID_CONNECTION)

        server = (s_vpn.get_data_item("server") or "").strip()
        if not server:
            raise plugin_error("no SOCKS5 server configured",
                               NM.VpnPluginError.BAD_ARGUMENTS)

        try:
            port = int(s_vpn.get_data_item("port") or DEFAULT_PORT)
            if not 1 <= port <= 65535:
                raise ValueError
        except ValueError:
            raise plugin_error("invalid port", NM.VpnPluginError.BAD_ARGUMENTS)

        username = s_vpn.get_data_item("username") or ""
        password = s_vpn.get_secret("password") or ""
        dns_through = (s_vpn.get_data_item("dns-through") or "yes") != "no"
        dns_servers = (s_vpn.get_data_item("dns-server") or DEFAULT_DNS)
        dns_servers = dns_servers.replace(",", " ").split()
        tun_ip = s_vpn.get_data_item("tun-ip") or DEFAULT_TUN_IP
        tun_peer = s_vpn.get_data_item("tun-peer") or DEFAULT_TUN_PEER
        try:
            mtu = int(s_vpn.get_data_item("mtu") or DEFAULT_MTU)
        except ValueError:
            mtu = DEFAULT_MTU

        # Resolve the proxy before the tunnel exists; NM will install a host
        # route to this IP via the physical uplink (the "gateway" config key).
        try:
            infos = socket.getaddrinfo(server, port,
                                       socket.AF_INET, socket.SOCK_STREAM)
            proxy_ip = infos[0][4][0]
        except (socket.gaierror, IndexError) as exc:
            raise plugin_error("cannot resolve %s: %s" % (server, exc),
                               NM.VpnPluginError.LAUNCH_FAILED)

        binary = find_tun2socks()
        if not binary:
            raise plugin_error(
                "tun2socks binary not found (looked in %s and PATH)"
                % ", ".join(TUN2SOCKS_CANDIDATES))

        if username:
            cred = "%s:%s@" % (urllib.parse.quote(username, safe=""),
                               urllib.parse.quote(password, safe=""))
        else:
            cred = ""
        proxy_url = "socks5://%s%s:%d" % (cred, proxy_ip, port)

        self._stop_proc()
        cmd = [binary,
               "-device", "tun://%s" % TUN_DEV,
               "-proxy", proxy_url,
               "-mtu", str(mtu),
               "-loglevel", "warning"]
        log("starting %s -> %s:%d (dns-through=%s)"
            % (os.path.basename(binary), proxy_ip, port, dns_through))
        try:
            self._proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL)
        except OSError as exc:
            raise plugin_error("failed to start tun2socks: %s" % exc)

        self._pending = {
            "proxy_ip": proxy_ip,
            "tun_ip": tun_ip,
            "tun_peer": tun_peer,
            "mtu": mtu,
            "dns": dns_servers if dns_through else [],
        }
        self._proc_watch_id = GLib.timeout_add(500, self._poll_proc)
        self._device_wait_id = GLib.timeout_add(
            DEVICE_WAIT_INTERVAL_MS, self._wait_device, DEVICE_WAIT_TRIES)
        return True

    def do_need_secrets(self, connection):
        s_vpn = connection.get_setting_vpn()
        if s_vpn and s_vpn.get_data_item("username") \
                and not s_vpn.get_secret("password"):
            return (True, "vpn")
        return (False, "")

    def do_disconnect(self):
        log("disconnect requested")
        self._teardown()
        return True

    # ------------------------------------------------------------------ #
    # Internals                                                           #
    # ------------------------------------------------------------------ #

    def _wait_device(self, tries_left):
        if self._proc is None or self._proc.poll() is not None:
            self._device_wait_id = 0
            return False  # _poll_proc reports the failure
        if not os.path.isdir("/sys/class/net/%s" % TUN_DEV):
            if tries_left <= 1:
                log("TUN device %s did not appear" % TUN_DEV)
                self._device_wait_id = 0
                self._fail()
                return False
            self._device_wait_id = GLib.timeout_add(
                DEVICE_WAIT_INTERVAL_MS, self._wait_device, tries_left - 1)
            return False
        self._device_wait_id = 0
        self._push_config()
        return False

    def _push_config(self):
        p = self._pending
        self._pending = None
        if p is None:
            return

        config = {
            "tundev": GLib.Variant("s", TUN_DEV),
            "gateway": GLib.Variant("u", ip4_to_uint32(p["proxy_ip"])),
            "mtu": GLib.Variant("u", p["mtu"]),
            "has-ip4": GLib.Variant("b", True),
            "has-ip6": GLib.Variant("b", False),
        }
        ip4 = {
            "address": GLib.Variant("u", ip4_to_uint32(p["tun_ip"])),
            "ptp": GLib.Variant("u", ip4_to_uint32(p["tun_peer"])),
            "prefix": GLib.Variant("u", 32),
            "never-default": GLib.Variant("b", False),
        }
        if p["dns"]:
            try:
                ip4["dns"] = GLib.Variant(
                    "au", [ip4_to_uint32(d) for d in p["dns"]])
            except OSError:
                log("ignoring invalid dns-server value %r" % (p["dns"],))

        log("tunnel up, sending IP config to NetworkManager")
        self.set_config(GLib.Variant("a{sv}", config))
        self.set_ip4_config(GLib.Variant("a{sv}", ip4))

    def _poll_proc(self):
        if self._proc is None:
            self._proc_watch_id = 0
            return False
        code = self._proc.poll()
        if code is None:
            return True
        log("tun2socks exited unexpectedly (code %s)" % code)
        self._proc = None
        self._proc_watch_id = 0
        self._fail()
        return False

    def _fail(self):
        self._teardown()
        self.failure(NM.VpnPluginFailure.CONNECT_FAILED)

    def _stop_proc(self):
        proc, self._proc = self._proc, None
        if proc is None or proc.poll() is not None:
            return
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    def _teardown(self):
        if self._proc_watch_id:
            GLib.source_remove(self._proc_watch_id)
            self._proc_watch_id = 0
        if self._device_wait_id:
            GLib.source_remove(self._device_wait_id)
            self._device_wait_id = 0
        self._pending = None
        self._stop_proc()


def main():
    # NetworkManager may pass --bus-name/--debug/--persist like it does for
    # the C plugins; accept and mostly ignore them.
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--bus-name", default=DBUS_SERVICE)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--persist", action="store_true")
    args, _unknown = parser.parse_known_args()

    if os.geteuid() != 0:
        log("must run as root (started by NetworkManager)")
        return 1

    try:
        plugin = Socks5Plugin(args.bus_name)
        plugin.init(None)  # GInitable: registers on the system bus
    except GLib.Error as exc:
        log("failed to register D-Bus service %s: %s"
            % (args.bus_name, exc.message))
        log("is the D-Bus policy file installed and dbus reloaded?")
        return 1

    loop = GLib.MainLoop()
    plugin.connect("quit", lambda _plugin: loop.quit())
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 2, loop.quit)   # SIGINT
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 15, loop.quit)  # SIGTERM

    log("service started on %s" % args.bus_name)
    loop.run()
    plugin._teardown()
    log("service stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
