#!/usr/bin/env python3
"""Screenshots of the web panel (English) for README.md, made without a device.

Starts a small mock of the panel API with fictional data (documentation addresses, sample names),
serves the files from web/ and takes screenshots with headless Google Chrome / Chromium.
Output: doc/img/*.png (optimized with Pillow when installed).

  python tools/screenshots.py [--chrome /path/to/chrome]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "web")
OUT = os.path.join(ROOT, "doc", "img")
PORT = 8799

# Pages to capture: (file name, URL hash, logged in, window size).
SHOTS = [
    ("login.png", "", False, (1280, 360)),
    ("status.png", "#/stan", True, (1280, 830)),
    ("dhcp.png", "#/dhcp", True, (1280, 892)),
    ("firewall.png", "#/firewall", True, (1280, 1042)),
    ("ddns.png", "#/ddns", True, (1280, 1000)),
    ("status-mobile.png", "#/stan", True, (390, 844)),
]

state = {"auth": False}


def field(path, typ, value, hint="", live=False, src="plik", secret=False):
    f = {"path": path, "type": typ, "value": value, "source": src, "hint": hint, "live": live}
    if secret:
        f.update(secret=True, set=True)
    return f


CONFIG = {
    "dhcp": [field("pool_start", "string", "192.168.4.100", "IPv4 address"),
             field("pool_end", "string", "192.168.4.199", "IPv4 address"),
             field("lease_time", "number", 7200, "60-604800"),
             field("dns", "string", "auto", "auto or IPv4 address")],
    "ddns": [field("ip_source", "string", "public", "public/sta/provider", True),
             field("check_url", "string", "https://api.ipify.org", "http(s):// URL, response = bare IP address", True),
             field("check_interval_s", "number", 300, "300-21600", True),
             field("force_update_h", "number", 24, "0-720", True),
             field("retry_s", "number", 300, "60-3600", True)] +
            [f for n, (en, prov, host) in enumerate([(True, "duckdns", "myrouter"), (False, "noip", ""), (False, "duckdns", "")], 1)
             for f in (field("%d.enabled" % n, "bool", en, "true/false", True),
                       field("%d.provider" % n, "string", prov, "duckdns/dynu/noip/freedns/desec/custom", True),
                       field("%d.hostname" % n, "string", host, "host name, several separated by commas", True),
                       field("%d.username" % n, "string", "", "login (No-IP, Dynu, custom)", True),
                       field("%d.secret" % n, "string", "", "token or password", True, secret=(n == 1)),
                       field("%d.custom_url" % n, "string", "", "http(s)://... with {host} {ip} {user} {pass}", True))],
}

STATUS = {
    "device": {"hostname": "esp32router", "version": "0.2.0", "uptime": 93725, "time": "2026-11-07 14:05:12 CET",
               "time_synced": True},
    "sta": {"ssid": "HomeNetwork", "connected": True, "ip": "192.168.1.50", "mask": "255.255.255.0",
            "gateway": "192.168.1.1", "dns": "192.168.1.1", "rssi": -58, "channel": 6},
    "ap": {"ssid": "Esp32Router-A1B2", "open": False, "ip": "192.168.4.1", "mask": "255.255.255.0", "clients": 3,
           "max_clients": 8},
    "nat": True,
    "memory": {"sram_free": 182000, "sram_total": 301000, "sram_min": 151000, "psram_free": 15900000,
               "psram_total": 16777216},
    "services": [
        {"name": "ddns", "running": True, "autostart": True, "console": False, "details": " IP 203.0.113.25, accounts 1"},
        {"name": "dhcp", "running": True, "autostart": True, "console": False,
         "details": " pool 192.168.4.100-192.168.4.199, leases 3"},
        {"name": "dns", "running": True, "autostart": True, "console": False, "details": " cache 118/256, blocked 1520"},
        {"name": "firewall", "running": True, "autostart": True, "console": False, "details": " active rules 3 of 4"},
        {"name": "serial", "running": True, "autostart": True, "console": True, "details": ""},
        {"name": "telnet", "running": True, "autostart": True, "console": True, "details": " port 23, sessions 0/2"},
        {"name": "web", "running": True, "autostart": True, "console": False, "details": " port 80, sessions 1/4"},
    ],
    "ddns": {"running": True, "public_ip": "203.0.113.25", "accounts": [{"fatal": False}]},
    "unsaved": 0, "default_password": False,
    "firewall": {"running": True, "rules": 3, "blocked": 42, "wan_ping": False},
}

LEASES = {"pool_start": "192.168.4.100", "pool_end": "192.168.4.199", "running": True, "leases": [
    {"mac": "02:00:5e:10:00:01", "ip": "192.168.4.10", "host": "laptop", "remaining": 6900, "type": "rezerwacja"},
    {"mac": "02:00:5e:10:00:02", "ip": "192.168.4.101", "host": "phone", "remaining": 5400, "type": "dynamiczny"},
    {"mac": "02:00:5e:10:00:03", "ip": "192.168.4.50", "host": "printer", "remaining": 7100, "type": "rezerwacja"}]}
RESERVATIONS = {"max": 32, "reservations": [
    {"mac": "02:00:5e:10:00:01", "ip": "192.168.4.10", "name": "laptop"},
    {"mac": "02:00:5e:10:00:03", "ip": "192.168.4.50", "name": "printer"},
    {"mac": "02:00:5e:10:00:04", "ip": "192.168.4.60", "name": "camera"}]}
CLIENTS = {"clients": [
    {"mac": "02:00:5e:10:00:01", "ip": "192.168.4.10", "name": "laptop", "rssi": -45, "type": "rezerwacja"},
    {"mac": "02:00:5e:10:00:02", "ip": "192.168.4.101", "name": "phone", "rssi": -60, "type": "dynamiczny"},
    {"mac": "02:00:5e:10:00:03", "ip": "192.168.4.50", "name": "printer", "rssi": -52, "type": "rezerwacja"}]}
FIREWALL = {"running": True, "log_blocked": True, "wan_ping": False, "ping_dropped": 4, "pf_dropped": 2, "max_rules": 32,
            "rules": [
                {"name": "smtp", "enabled": True, "client": "all", "proto": "tcp", "ports": "25", "dest": "internet",
                 "dest_ip": "any", "action": "block", "hits": 17},
                {"name": "camera-offline", "enabled": True, "client": "camera", "ip": "192.168.4.60", "proto": "all",
                 "ports": "", "dest": "internet", "dest_ip": "any", "action": "block", "hits": 25},
                {"name": "force-dns", "enabled": True, "client": "all", "proto": "both", "ports": "53,853",
                 "dest": "internet", "dest_ip": "any", "action": "block", "hits": 0},
                {"name": "laptop-dns", "enabled": False, "client": "laptop", "ip": "192.168.4.10", "proto": "both",
                 "ports": "53", "dest": "internet", "dest_ip": "any", "action": "allow", "hits": 0}],
            "recent": [
                {"time": "14:03:51", "rule": "camera-offline", "src": "192.168.4.60", "dst": "198.51.100.20", "proto": "tcp",
                 "port": 443},
                {"time": "14:01:07", "rule": "smtp", "src": "192.168.4.101", "dst": "203.0.113.80", "proto": "tcp",
                 "port": 25}]}
DDNS = {"running": True, "ip_source": "public", "public_ip": "203.0.113.25", "check_error": "", "fallback": False,
        "last_check_s": 42, "next_check_s": 258, "accounts": [
            {"slot": 1, "enabled": True, "provider": "duckdns", "hostname": "myrouter", "secret_set": True, "empty": False,
             "problem": "", "result": "OK: OK", "fatal": False, "last_ok": "3 min ago", "last_ip": "203.0.113.25"},
            {"slot": 2, "enabled": False, "provider": "noip", "hostname": "", "secret_set": False, "empty": True,
             "problem": "", "result": "", "fatal": False, "last_ok": "never", "last_ip": ""},
            {"slot": 3, "enabled": False, "provider": "duckdns", "hostname": "", "secret_set": False, "empty": True,
             "problem": "", "result": "", "fatal": False, "last_ok": "never", "last_ip": ""}]}

API = {"/api/status": STATUS, "/api/dhcp/leases": LEASES, "/api/dhcp/reservations": RESERVATIONS,
       "/api/clients": CLIENTS, "/api/firewall": FIREWALL, "/api/ddns": DDNS}
MIME = {"js": "application/javascript", "css": "text/css", "html": "text/html; charset=utf-8", "json": "application/json"}


class Handler(BaseHTTPRequestHandler):
    """Mock of the panel API and static files from web/."""

    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        p = self.path.split("?")[0]
        if p == "/api/session":
            return self.send_json({"language": "en", "auth": state["auth"], "hostname": "esp32router",
                                   "version": "0.2.0", "default_password": False})
        if p in API:
            return self.send_json(API[p])
        if p.startswith("/api/config/"):
            return self.send_json({"module": p[12:], "fields": CONFIG.get(p[12:], [])})
        path = os.path.join(WEB, "index.html" if p == "/" else p.lstrip("/"))
        if os.path.isfile(path):
            body = open(path, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", MIME.get(path.rsplit(".", 1)[-1], "application/octet-stream"))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_json({"error": "not found"}, 404)

    def do_POST(self):
        self.send_json({"ok": True})

    def log_message(self, *args):
        pass


def find_chrome(given):
    for name in ([given] if given else []) + ["google-chrome", "chromium", "chromium-browser", "chrome"]:
        path = shutil.which(name) or (name if name and os.path.isfile(name) else None)
        if path:
            return path
    sys.exit("Chrome/Chromium not found – use --chrome")


def optimize(path):
    """Reduces the PNG with Pillow (palette of 256 colours) when available."""
    try:
        from PIL import Image
    except ImportError:
        return
    img = Image.open(path).convert("RGB")
    img.quantize(colors=256, method=Image.Quantize.MEDIANCUT).save(path, optimize=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--chrome", help="path to Chrome/Chromium")
    args = ap.parse_args()
    chrome = find_chrome(args.chrome)
    os.makedirs(OUT, exist_ok=True)
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for name, page, auth, (w, h) in SHOTS:
            state["auth"] = auth
            out = os.path.join(OUT, name)
            with tempfile.TemporaryDirectory() as profile:
                subprocess.run([chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
                                "--user-data-dir=" + profile, "--force-device-scale-factor=1",
                                "--window-size=%d,%d" % (w, h), "--virtual-time-budget=5000",
                                "--screenshot=" + out, "http://127.0.0.1:%d/%s" % (PORT, page)],
                               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            optimize(out)
            print("%-20s %6d B" % (name, os.path.getsize(out)))
    finally:
        server.shutdown()


if __name__ == "__main__":
    main()
