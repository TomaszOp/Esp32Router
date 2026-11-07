#!/usr/bin/env python3
"""Firmware upload through the web panel (POST /api/ota) – the same as System → Firmware in the panel.

Called by "pio run -e esp32-s3-ota -t upload" (platformio.ini) or by hand:
  python tools/ota_upload.py [--host 192.168.4.1[:80]] [--port 80] .pio/build/<env>/firmware.bin
Panel password: the ESP32ROUTER_WEB_PASS variable, then SECRET_WEB_PASS from include/Secrets.h
(factory password), finally a prompt in the terminal. Standard library only.
Needs access to the panel: the computer in the AP network (or global.json mgmt_from_sta = true).
"""
import argparse
import getpass
import http.client
import json
import os
import re
import sys
import time

CSRF = {"X-Requested-With": "esp32router"}
CHUNK = 4096


def request(host, port, method, path, body=None, headers=None, timeout=10):
    """One HTTP request with the anti-CSRF header; returns (status, Set-Cookie, body)."""
    c = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        c.request(method, path, body=body, headers={**CSRF, **(headers or {})})
        r = c.getresponse()
        return r.status, r.getheader("Set-Cookie") or "", r.read()
    finally:
        c.close()


def error_text(body):
    """Error message from a JSON response body (or the start of a non-JSON body)."""
    try:
        d = json.loads(body or b"{}")
        return d.get("error") or d.get("output") or ""
    except ValueError:
        return body.decode(errors="replace")[:200]


def secrets_password(project_dir):
    """SECRET_WEB_PASS from include/Secrets.h, or None."""
    path = os.path.join(project_dir, "include", "Secrets.h")
    try:
        with open(path, encoding="utf-8") as f:
            m = re.search(r'^\s*#define\s+SECRET_WEB_PASS\s+"([^"]*)"', f.read(), re.M)
    except OSError:
        return None
    return m.group(1) if m else None


def password_candidates(project_dir):
    """Yields (password, source) to try: environment variable, Secrets.h, terminal prompt."""
    env = os.environ.get("ESP32ROUTER_WEB_PASS")
    if env:
        yield env, "ESP32ROUTER_WEB_PASS"
        return
    sec = secrets_password(project_dir)
    if sec:
        yield sec, "Secrets.h (haslo fabryczne - po zmianie w panelu ustaw ESP32ROUTER_WEB_PASS)"
    if sys.stdin.isatty():
        yield getpass.getpass("Haslo panelu: "), "terminal"


def login(host, port, project_dir):
    """Logs in with the candidate passwords; returns the "sid=..." cookie or exits with a message."""
    for password, source in password_candidates(project_dir):
        st, cookie, body = request(host, port, "POST", "/api/login", json.dumps({"password": password}).encode(),
                                   {"Content-Type": "application/json"})
        if st == 200 and cookie.startswith("sid="):
            print("Zalogowano (haslo: %s)" % source)
            return cookie.split(";")[0]
        if st == 429:
            sys.exit("Logowanie zablokowane po 5 blednych probach: %s" % error_text(body))
        print("Haslo z %s nie pasuje (%d: %s)" % (source, st, error_text(body)))
    sys.exit("Brak poprawnego hasla - ustaw ESP32ROUTER_WEB_PASS")


def upload(host, port, cookie, image):
    """Sends the image in 4 KB chunks with progress every 10%; returns (status, body, seconds)."""
    c = http.client.HTTPConnection(host, port, timeout=60)
    c.putrequest("POST", "/api/ota")
    for k, v in {**CSRF, "Cookie": cookie, "Content-Type": "application/octet-stream",
                 "Content-Length": str(len(image))}.items():
        c.putheader(k, v)
    c.endheaders()
    t0 = time.time()
    shown = -1
    for off in range(0, len(image), CHUNK):
        c.send(image[off:off + CHUNK])
        pct = 100 * min(off + CHUNK, len(image)) // len(image)
        if pct // 10 != shown:  # every 10% (pio shows each line separately)
            shown = pct // 10
            print("  %3d%%  %d / %d B" % (pct, min(off + CHUNK, len(image)), len(image)), flush=True)
    r = c.getresponse()
    body = r.read()
    c.close()
    return r.status, body, time.time() - t0


def main():
    """Checks the panel, logs in, uploads, then waits for the restart and verifies the version."""
    p = argparse.ArgumentParser(description="OTA Esp32Router przez panel WWW")
    p.add_argument("--host", default="192.168.4.1", help="adres, także adres:port")
    p.add_argument("--port", type=int, default=80)
    p.add_argument("firmware")
    a = p.parse_args()
    if ":" in a.host:
        a.host, port = a.host.rsplit(":", 1)
        a.port = int(port)
    project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    with open(a.firmware, "rb") as f:
        image = f.read()
    print("OTA: %s (%d B) -> http://%s:%d" % (a.firmware, len(image), a.host, a.port))

    try:
        st, _, body = request(a.host, a.port, "GET", "/api/session")
    except OSError as e:
        sys.exit("Brak polaczenia z %s: %s (komputer w sieci AP?)" % (a.host, e))
    if st != 200:
        sys.exit("Panel odpowiada %d: %s (dostep tylko od strony AP?)" % (st, error_text(body)))

    cookie = login(a.host, a.port, project_dir)
    st, _, body = request(a.host, a.port, "GET", "/api/session", headers={"Cookie": cookie})
    old = json.loads(body).get("version", "?") if st == 200 else "?"  # version only after login
    print("Wysylanie (wersja na urzadzeniu: %s)" % old)
    try:
        st, body, secs = upload(a.host, a.port, cookie, image)
    except OSError as e:
        sys.exit("Przerwane polaczenie podczas wysylania: %s" % e)
    if st != 200:
        sys.exit("Obraz odrzucony (%d): %s" % (st, error_text(body)))
    d = json.loads(body)
    new = d.get("version", "?")
    print("Zapisano %s w %s w %.1f s - urzadzenie sie restartuje" % (new, d.get("partition"), secs))

    # The new firmware confirms its start itself; if it does not come up, the bootloader returns to the previous one.
    # After the restart sessions are invalid, and /api/session gives the version only after login.

    time.sleep(4)
    deadline = time.time() + 60
    cookie = None
    while time.time() < deadline:
        try:
            st, _, body = request(a.host, a.port, "GET", "/api/session", timeout=3,
                                  headers={"Cookie": cookie} if cookie else None)
            if st == 200 and "version" not in json.loads(body):
                cookie = login(a.host, a.port, project_dir)
                continue
            if st == 200:
                now = json.loads(body).get("version", "?")
                if now == new:
                    print("Urzadzenie dziala, wersja %s" % now)
                    return
                sys.exit("Urzadzenie wrocilo z wersja %s zamiast %s (rollback?)" % (now, new))
        except OSError:
            pass
        time.sleep(2)
    print("Urzadzenie nie odpowiada po 60 s - polacz sie ponownie z siecia AP i sprawdz wersje w panelu")


if __name__ == "__main__":
    main()
