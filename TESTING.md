# Test plan (fresh Fedora VM, non-Qubes)

End-to-end verification of nm-socks5 on a disposable Fedora VM (e.g.
VirtualBox). The code has **never run on a real system** — expect to find
and fix issues. Work through the steps in order; each later step depends
on the earlier ones.

**Known risk areas** (check these first when something breaks):

1. `service/nm-socks5-service.py` overrides the `do_need_secrets` vfunc.
   PyGObject's handling of the `(out)` string parameter of this libnm
   vfunc is the least certain part of the code. NetworkManager calls
   NeedSecrets *before every Connect*, so if this raises a TypeError the
   whole plugin is dead. Symptom appears in `journalctl -u NetworkManager`.
   Fallback if broken: try returning just `False`/`True`, or drop the
   override entirely and require the password to always be stored.
2. The `.name` file references the editor `.so` by **absolute path**
   (`plugin=/usr/lib/nm-socks5/...`). libnm verifies owner/permissions on
   absolutely-pathed plugins. If nm-connection-editor does not show the
   SOCKS5 entry, test with the `.so` copied to the default NM plugin dir
   (`pkg-config --variable=libdir libnm`/NetworkManager) and a bare
   filename in `plugin=` instead.
3. GVariant types in `_push_config()` must match what NM expects exactly
   (`u` for IPs in network byte order, `au` for DNS). A mismatch shows up
   as "VPN connection ... did not receive valid IP config" in the journal.

## 1. Build & install

```bash
sudo ./install.sh          # from this checkout; on non-Qubes takes the /usr layout
```

Verify afterwards:

```bash
ls -l /usr/lib/nm-socks5/                        # .so + tun2socks
ls -l /usr/libexec/nm-socks5-service             # executable
cat /usr/lib/NetworkManager/VPN/nm-socks5-service.name
ls -l /usr/share/dbus-1/system.d/nm-socks5-service.conf
python3 -c "import gi; gi.require_version('NM','1.0'); from gi.repository import NM; print(NM.VpnServicePlugin)"
python3 -m py_compile /usr/libexec/nm-socks5-service && echo "service syntax OK"
```

## 2. Plugin discovery

```bash
nmcli connection add type vpn vpn-type socks5 con-name test-socks5 \
    vpn.data "server=127.0.0.1,port=1080,username=testuser,dns-through=no" \
    vpn.secrets "password=testpass"
nmcli -f vpn connection show test-socks5
```

If `vpn-type socks5` is rejected, the `.name` file is not being picked up
(risk area 2).

## 3. Local SOCKS5 server with auth

microsocks is tiny and supports username/password (TCP only, no UDP):

```bash
sudo dnf install -y git gcc make
git clone https://github.com/rofl0r/microsocks && make -C microsocks
./microsocks/microsocks -u testuser -P testpass -p 1080 &
curl -s --socks5 testuser:testpass@127.0.0.1:1080 https://ifconfig.me && echo " <- proxy works"
```

## 4. Activate & verify routing

```bash
sudo journalctl -u NetworkManager -f &          # watch for "nm-socks5:" lines
nmcli connection up test-socks5
ip route                                        # default via nm-socks5 expected,
                                                # host route to proxy via uplink
ip addr show nm-socks5
curl -s https://ifconfig.me                     # must still work (now via tun -> proxy)
```

Note: with the proxy on 127.0.0.1 the "host route to the proxy" that NM
installs is unusual (loopback always wins anyway). If activation fails on
this, bind microsocks to the VM's LAN IP and use that as `server=` instead.

Negative test: wrong password must fail cleanly, not hang:

```bash
nmcli connection down test-socks5
nmcli connection modify test-socks5 vpn.secrets "password=WRONG"
nmcli connection up test-socks5   # expect failure + journal message
```

## 5. DNS-through-proxy (optional, needs UDP ASSOCIATE)

microsocks cannot do UDP. Use 3proxy/sing-box/xray, then:

```bash
nmcli connection modify test-socks5 vpn.data "server=...,port=1080,username=...,dns-through=yes,dns-server=1.1.1.1"
nmcli connection up test-socks5
resolvectl status nm-socks5      # 1.1.1.1 bound to the tun device
dig +short example.org           # must resolve
```

## 6. GUI (needs a desktop session)

`nm-connection-editor` → **+** → "SOCKS5 Proxy (tun2socks)" must appear;
create/edit a connection, re-open it, confirm all fields round-trip
(server, port, username, password, DNS checkbox + server).

## 7. Teardown / re-run

```bash
nmcli connection delete test-socks5
sudo ./uninstall.sh
```

Also test the public one-liner once local install works:

```bash
sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/snatcher3000/nm-socks5/main/install.sh)"
```

## Debugging tips

* Run the service by hand for full tracebacks:
  `sudo /usr/libexec/nm-socks5-service --debug` (then activate the
  connection from another terminal).
* `nmcli general logging level DEBUG domains VPN` for verbose NM logs.
* tun2socks stderr goes to the journal via the service.

## Reporting

Fix what you can, commit with clear messages, and record anything
Qubes-specific that cannot be tested here (AppVM bind-dirs layout,
qubesdb detection) as open items in the final summary.
