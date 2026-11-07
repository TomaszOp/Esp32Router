#!/usr/bin/env python3
"""Table of factory settings from include/Defaults.h (the single source of values and ranges) in doc/configuration.md.

Written between the <!-- defaults:begin --> and <!-- defaults:end --> markers – fields of the JSON
files (names from the POLA map below; Polish notes for the user documentation in UWAGI).
doc/dev/projekt.md is no longer updated (design document closed on 7.11.2026).
Without arguments updates the file, with --check only checks that the table is current (exit code 1 when not).
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "Defaults.h")
USER_DOC = os.path.join(ROOT, "doc", "configuration.md")
BEGIN, END = "<!-- defaults:begin -->", "<!-- defaults:end -->"           # Polish table
BEGIN_EN, END_EN = "<!-- defaults-en:begin -->", "<!-- defaults-en:end -->"  # English table

# Constant from Defaults.h -> field in the module's JSON file, when it does not follow from the name (lower-case name).
POLA = {
    ("global", "LOG_LEVEL"): "log.level", ("global", "LOG_OUTPUT"): "log.output",
    ("global", "NTP_ENABLED"): "time.ntp_enabled", ("global", "NTP_SERVER"): "time.ntp_server",
    ("global", "TIMEZONE"): "time.timezone",
    ("wifi", "STA_SSID"): "sta.ssid", ("wifi", "STA_PASS"): "sta.pass", ("wifi", "AP_SSID"): "ap.ssid",
    ("wifi", "AP_PASS"): "ap.pass", ("wifi", "AP_MAX_CLIENTS"): "ap.max_clients", ("wifi", "AP_IP"): "ap.ip",
    ("wifi", "AP_MASK"): "ap.mask",
    ("dhcp", "POOL_FIRST_HOST"): "pool_start", ("dhcp", "POOL_LAST_HOST"): "pool_end",
    ("dns", "BLOCK_ENABLED"): "block.enabled", ("dns", "BLOCK_RESPONSE"): "block.response",
    ("ddns", "ENABLED"): "<n>.enabled", ("ddns", "PROVIDER"): "<n>.provider", ("ddns", "HOSTNAME"): "<n>.hostname",
    ("ddns", "USERNAME"): "<n>.username", ("ddns", "SECRET"): "<n>.secret", ("ddns", "CUSTOM_URL"): "<n>.custom_url",
}


# Notes in the user table (Polish – the user documentation is Polish; comments in Defaults.h are English).
UWAGI = {
    ("global", "VERSION"): "wersja formatu global.json (nie ustawienie)",
    ("global", "LANGUAGE"): "pl|en",
    ("global", "CONSOLE_UTF8"): "konsola COM/Telnet w UTF-8 (false = polskie znaki jako ASCII)",
    ("wifi", "STA_SSID"): "puste = nie łącz",
    ("wifi", "AP_SSID"): '+ "-XXXX" (ostatnie 2 bajty MAC AP)',
    ("wifi", "AP_PASS"): "puste = sieć otwarta",
    ("dhcp", "POOL_FIRST_HOST"): "pula: <sieć AP>.100 – .199",
    ("telnet", "IDLE_TIMEOUT_S"): "0 = bez limitu",
    ("ddns", "ENABLED"): "konto; włącza użytkownik (Włączone) po wpisaniu danych",
}
# Note next to a range (upper bound given by the firmware).
UWAGI_ZAKRES = {
    ("wifi", "AP_MAX_CLIENTS"): "limit sterownika",
    ("telnet", "MAX_SESSIONS"): "rozmiar tablicy sesji",
    ("web", "MAX_SESSIONS"): "rozmiar tablicy sesji",
}


def field(ns, name):
    """JSON field path of a Defaults.h constant."""
    if name.startswith("AUTOSTART_"):
        return "services." + name[len("AUTOSTART_"):].lower()
    return POLA.get((ns, name), name.lower())


def parse():
    """Reads Defaults.h; returns rows (namespace, constant, value, range, comment) with macros resolved."""
    text = open(HEADER, encoding="utf-8").read()
    macros = {}  # DEFAULT_X -> (value, comment)
    for m in re.finditer(r'^#define (DEFAULT_\w+) ("(?:[^"\\]|\\.)*"|[^"/\n]+?)\s*(?://\s*(.*))?$', text, re.M):
        macros[m.group(1)] = (m.group(2).strip(), m.group(3) or "")
    rows = []
    for ns, body in re.findall(r"^namespace (?!defaults\b)(\w+) \{\n(.*?)^\}", text, re.M | re.S):
        consts = {}
        order = []
        for name, value, comment in re.findall(r"constexpr [\w:* ]+? (\w+) = ([^;]+);\s*(?://\s*(.*))?", body):
            consts[name] = (value.strip(), comment or "")
            order.append(name)
        for name in order:
            if name.endswith("_MIN") or name.endswith("_MAX"):
                continue
            value, comment = consts[name]
            if value in macros:
                value, mcomment = macros[value]
                comment = comment or mcomment
            value = re.sub(r"ESP_IP4TOADDR\((\d+), (\d+), (\d+), (\d+)\)", r"\1.\2.\3.\4", value)
            lo, hi = consts.get(name + "_MIN"), consts.get(name + "_MAX")
            rng = "%s – %s" % (lo[0], hi[0]) if lo and hi else ""
            rng = rng.replace("ESP_WIFI_MAX_CONN_NUM", "15")  # ESP32-S3 driver limit
            if hi and hi[1]:
                rng += " (%s)" % hi[1]
            rows.append((ns, name, value, rng, comment))
    return rows


def user_table(rows, lang):
    """Markdown table for the user (lang "pl" or "en"): file, field, default, range, notes.

    English notes come from the comments in Defaults.h, Polish ones from UWAGI / UWAGI_ZAKRES.
    """
    if lang == "pl":
        out = ["| Plik | Pole | Domyślnie | Zakres | Uwagi |", "|---|---|---|---|---|"]
    else:
        out = ["| File | Field | Default | Range | Notes |", "|---|---|---|---|---|"]
    for ns, name, value, rng, comment in rows:
        if lang == "pl":
            comment = UWAGI.get((ns, name), "")
            rng = re.sub(r" \(.*\)$", "", rng)
            if (ns, name) in UWAGI_ZAKRES:
                rng += " (%s)" % UWAGI_ZAKRES[(ns, name)]
        if (ns, name) == ("dhcp", "POOL_FIRST_HOST") or (ns, name) == ("dhcp", "POOL_LAST_HOST"):
            value = ("<sieć AP>.%s" if lang == "pl" else "<AP network>.%s") % value
        value = value.replace("|", "\\|")
        comment = comment.replace("|", "\\|").replace("<", "&lt;").replace(">", "&gt;")
        out.append("| %s.json | `%s` | `%s` | %s | %s |" % (ns, field(ns, name), value, rng, comment))
    return "\n".join(out)


def update(path, text, check, begin=BEGIN, end=END):
    """Replaces the table between the markers (or only checks it); returns a status text."""
    doc = open(path, encoding="utf-8").read()
    rel = os.path.relpath(path, ROOT)
    if begin not in doc or end not in doc:
        sys.exit("Brak znacznikow %s / %s w %s" % (begin, end, rel))
    new = doc[: doc.index(begin) + len(begin)] + "\n" + text + "\n" + doc[doc.index(end):]
    if check:
        if new != doc:
            sys.exit("Tabela ustawien fabrycznych w %s jest nieaktualna - uruchom tools/defaults_table.py" % rel)
        return "%s aktualna" % rel
    if new != doc:
        open(path, "w", encoding="utf-8").write(new)
    return rel


def main():
    """Updates or checks (--check) both tables (English and Polish) in doc/configuration.md."""
    rows = parse()
    check = "--check" in sys.argv
    done = [update(USER_DOC, user_table(rows, "en"), check, BEGIN_EN, END_EN),
            update(USER_DOC, user_table(rows, "pl"), check)]
    print("%s: %d wartosci" % (", ".join(done), len(rows)))

if __name__ == "__main__":
    main()
