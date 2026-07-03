# nm-socks5

A NetworkManager VPN plugin that adds a **"SOCKS5 Proxy"** entry to the
connection editor ("Choose a Connection Type" list) and routes **all**
traffic of the machine through a SOCKS5 proxy — including optional
username/password authentication and optional DNS-through-the-proxy.

Built with [tun2socks](https://github.com/xjasonlyu/tun2socks) and designed
for **Qubes OS** service VMs (e.g. `sys-vpn`), but it works on any Linux
with NetworkManager.

> **Status: beta.** The code is complete but young — please report issues.

## How it works

SOCKS5 is not a VPN protocol, so NetworkManager cannot speak it natively.
This plugin bridges the gap the same way commercial clients do:

```
apps / downstream qubes
        │
        ▼ (default route)
   TUN device nm-socks5          ← created by tun2socks
        │
        ▼
     tun2socks                   ← speaks SOCKS5 (+ auth) to your proxy
        │
        ▼ (host route via uplink, added by NetworkManager)
   SOCKS5 server ──────► internet
```

* NetworkManager starts `nm-socks5-service` (Python, D-Bus).
* The service launches `tun2socks`, which creates a TUN device and forwards
  every TCP/UDP flow entering it to the SOCKS5 server.
* The service hands NetworkManager the IP configuration; NM sets the
  default route through the TUN device and pins a host route to the proxy
  server via the physical uplink (no routing loop).
* If **"Route DNS through the proxy"** is enabled, the configured DNS
  server (default `1.1.1.1`) is pushed into the connection, so DNS queries
  travel through the tunnel too. *Note: DNS is UDP, so your SOCKS5 server
  must support UDP ASSOCIATE (shadowsocks, xray, sing-box, dante, 3proxy do;
  `ssh -D` does not).*

## Installation

Run **inside the VM whose NetworkManager should get the plugin**:

```bash
sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/CHANGEME/nm-socks5/main/install.sh)"
```

The installer detects the environment:

| Environment | Detection | Layout |
|---|---|---|
| Qubes **TemplateVM** / StandaloneVM, normal Linux | `qubes-vm-persistence = full` (or no Qubes) | `/usr/lib/nm-socks5`, `/usr/libexec`, `/usr/lib/NetworkManager/VPN` |
| Qubes **AppVM** (e.g. `sys-vpn`) | `qubes-vm-persistence = rw-only` | `/usr/local/...` (persistent) + `qubes-bind-dirs` for the two files that must live in `/etc` |

So yes — you can run the one-liner **directly in `sys-vpn`** and it survives
reboots, no file shuffling between VMs needed. (In a TemplateVM, which has
no direct network, add `--proxy http://127.0.0.1:8082` to the `curl`.)

It installs build dependencies (gcc, libnm-devel, gtk3-devel, PyGObject),
compiles the small GTK editor plugin, and downloads the static `tun2socks`
binary from its upstream GitHub release.

Uninstall with `sudo bash uninstall.sh` from a source checkout.

## Usage

**GUI:** right-click the nm-applet icon → *Edit Connections…* → **+** →
choose **"SOCKS5 Proxy (tun2socks)"**. Fill in server, port, optionally
username/password, and tick *Route DNS through the proxy* if desired.
Activate the connection from the applet's VPN menu.

**CLI:**

```bash
nmcli connection add type vpn vpn-type socks5 con-name my-proxy \
    vpn.data "server=203.0.113.7,port=1080,username=me,dns-through=yes,dns-server=1.1.1.1" \
    vpn.secrets "password=secret"
nmcli connection up my-proxy
```

Recognized `vpn.data` keys: `server`, `port` (default 1080), `username`,
`dns-through` (`yes`/`no`, default `yes`), `dns-server` (comma-separated
IPv4 list), plus advanced overrides `tun-ip`, `tun-peer`, `mtu`.
The secret key is `password`.

## Qubes notes

* In `sys-vpn` (a NetVM), traffic of all downstream qubes is forwarded
  through the main routing table — so it goes through the proxy too.
  Qubes' DNAT-based DNS forwarding follows the DNS servers NetworkManager
  writes to `resolv.conf`, so downstream DNS also flows through the tunnel
  when *Route DNS through the proxy* is on.
* **This is not a kill switch.** If the proxy connection drops, traffic
  falls back to the normal uplink. Add `qvm-firewall`/nftables rules that
  only allow traffic to your proxy IP if you need leak protection.
* IPv6 is not handled (the plugin tells NM "no IPv6"); disable IPv6 on the
  uplink if you are worried about v6 leaks.

## Security notes

* The password is stored system-scoped in
  `/etc/NetworkManager/system-connections/` (root-only, like other NM VPN
  secrets) and is passed to tun2socks inside the proxy URL on its command
  line — visible to root-level process listings inside the VM. In a
  dedicated Qubes service VM this is usually acceptable; patches for a
  config-file based handover are welcome.
* MIT licensed. GPL-compatible; link/reuse freely.

## Troubleshooting

```bash
journalctl -u NetworkManager -f          # plugin logs appear here (nm-socks5: ...)
nmcli connection up my-proxy             # verbose error on failure
ls -l /etc/NetworkManager/VPN/ /usr/lib/NetworkManager/VPN/   # .name file present?
busctl status org.freedesktop.NetworkManager.socks5           # D-Bus policy ok?
```

* Entry missing in the "+" list → restart `nm-connection-editor`; check that
  the `.so` path inside the `.name` file exists and is root-owned.
* `failed to register D-Bus service` → D-Bus policy file missing or dbus
  not reloaded; rerun the installer.
* Connection sticks at "connecting" then fails → check whether `tun2socks`
  can reach the proxy (`journalctl` shows its stderr), and that
  username/password are correct.
* DNS not resolving with *Route DNS through the proxy* on → your SOCKS5
  server probably lacks UDP support; untick the option or switch servers.

## Building from source

```bash
make            # needs: pkg-config, libnm-devel, gtk3-devel
sudo ./install.sh
```

Repository layout:

```
properties/nm-socks5-editor.c   GTK3 editor plugin (the dialog in nm-connection-editor)
service/nm-socks5-service.py    NM VPN service daemon (spawns tun2socks)
data/nm-socks5-service.name.in  NetworkManager VPN service registration
data/nm-socks5-dbus.conf        D-Bus policy
install.sh / uninstall.sh       Qubes-aware installer
```

---

## Schnellstart (Deutsch)

In der VM ausführen, deren NetworkManager das Plugin bekommen soll
(z. B. direkt in `sys-vpn` — überlebt dort Neustarts dank bind-dirs):

```bash
sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/CHANGEME/nm-socks5/main/install.sh)"
```

Danach: Rechtsklick auf das Netzwerk-Icon → *Edit Connections…* → **+** →
**"SOCKS5 Proxy (tun2socks)"** auswählen, Server/Port/Benutzername/Passwort
eintragen, optional *Route DNS through the proxy* aktivieren — fertig.
