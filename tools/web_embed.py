"""PlatformIO script (extra_scripts = pre:...): compresses the files of web/ with gzip and generates
src/generated/WebAssets.cpp with byte arrays and the file table.

The file is rewritten only when its content changes, so that no rebuild is forced. Also prints the
first-load size against the budget and checks the translation dictionaries of the panel.
"""
import gzip
import hashlib
import json
import os
import re
from html.parser import HTMLParser

try:
    Import("env")  # noqa: F821 - provided by PlatformIO
    PROJECT_DIR = env.subst("$PROJECT_DIR")  # noqa: F821
except NameError:
    PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

WEB_DIR = os.path.join(PROJECT_DIR, "web")
OUT_DIR = os.path.join(PROJECT_DIR, "src", "generated")
OUT_FILE = os.path.join(OUT_DIR, "WebAssets.cpp")
# Budget: first load (skeleton + Status page) after gzip (doc/dev/panel.md). The total is informational only.
FIRST_LOAD = ("/index.html", "/style.css", "/app.js", "/pages/stan.html", "/pages/stan.js")
FIRST_LOAD_BUDGET = 12 * 1024

MIME = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".json": "application/json",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
}


def project_version():
    """PROJECT_VER from CMakeLists.txt (part of the ETag)."""
    with open(os.path.join(PROJECT_DIR, "CMakeLists.txt"), encoding="utf-8") as f:
        m = re.search(r'PROJECT_VER\s+"([^"]+)"', f.read())
    return m.group(1) if m else "0"


# ---- language versions (doc/dev/jezyki.md) ----
# Dictionary keys = Polish texts: from HTML (text and the attributes data-label, data-help, data-button,
# data-options, placeholder, title, aria-label) and from JS (t('...'), tn('...')). Every dictionary
# web/i18n/<language>.json is compared with the keys; missing and unused keys are printed (missing key = Polish text in the panel).
I18N_DIR = os.path.join(WEB_DIR, "i18n")
I18N_ATTRS = ("data-label", "data-help", "data-button", "placeholder", "title", "aria-label")


def norm(text):
    """Text with whitespace collapsed to single spaces (dictionary key form)."""
    return re.sub(r"\s+", " ", text).strip()


def needs_translation(text):
    """Whether a text needs a dictionary entry."""
    # No lower-case letters or just an abbreviation/name (e.g. "NAT", "MAC", "Esp32Router") – no translation.
    return re.search(r"[a-ząćęłńóśźż]", text) is not None and text != "Esp32Router"


class _HtmlTexts(HTMLParser):
    """Collects translatable texts and attributes of an HTML file (skips script and style)."""
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.keys = set()
        self.skip = 0

    def handle_starttag(self, tag, attrs):
        if tag in ("script", "style"):
            self.skip += 1
        for name, value in attrs:
            if value is None:
                continue
            if name in I18N_ATTRS:
                self.keys.add(norm(value))
            elif name == "data-options":
                for opt in value.split(","):
                    self.keys.add(norm(opt.split(":", 1)[-1]))

    def handle_endtag(self, tag):
        if tag in ("script", "style"):
            self.skip -= 1

    def handle_data(self, data):
        if not self.skip and norm(data):
            self.keys.add(norm(data))


JS_KEY = re.compile(r"\btn?\(\s*'((?:[^'\\]|\\.)*)'")


def i18n_keys(files):
    """All dictionary keys used by the HTML and JS files of the panel."""
    keys = set()
    for path in files:
        if path.startswith(I18N_DIR):
            continue
        with open(path, encoding="utf-8") as f:
            text = f.read()
        if path.endswith(".html"):
            parser = _HtmlTexts()
            parser.feed(text)
            keys |= parser.keys
        elif path.endswith(".js"):
            for m in JS_KEY.finditer(text):
                keys.add(m.group(1).replace("\\'", "'").replace("\\n", "\n"))
    return {k for k in keys if needs_translation(k)}


def check_i18n(files):
    """Compares every web/i18n/*.json with the keys and prints missing, unused and {n}-mismatched entries."""
    keys = i18n_keys(files)
    if not os.path.isdir(I18N_DIR):
        return
    for name in sorted(os.listdir(I18N_DIR)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(I18N_DIR, name), encoding="utf-8") as f:
            try:
                d = json.load(f)
            except ValueError as e:
                print("web_embed: i18n/%s - BLAD JSON: %s" % (name, e))
                continue
        missing = sorted(keys - set(d))
        unused = sorted(set(d) - keys)
        bad = sorted(k for k, v in d.items() if re.findall(r"\{\d+\}", k) and
                     sorted(re.findall(r"\{\d+\}", k)) != sorted(re.findall(r"\{\d+\}", v)))
        print("web_embed: i18n/%s - %d tekstow, brak %d, nieuzywane %d, zle {n} %d" %
              (name, len(keys), len(missing), len(unused), len(bad)))
        for k in missing:
            print("web_embed:   brak: %s" % k)
        for k in unused:
            print("web_embed:   nieuzywany: %s" % k)
        for k in bad:
            print("web_embed:   inne {n}: %s" % k)


def generate():
    """Compresses web/, prints sizes and the budget, checks translations, writes WebAssets.cpp when changed."""

    version = project_version()
    files = []
    if os.path.isdir(WEB_DIR):
        for root, _, names in os.walk(WEB_DIR):
            for name in sorted(names):
                files.append(os.path.join(root, name))
    files.sort()

    lines = [
        "// Plik generowany przez tools/web_embed.py z katalogu web/ - nie edytowac.",
        '#include "WebAssets.h"',
        "",
    ]
    table = []
    total_raw = total_gz = first_gz = 0
    for i, path in enumerate(files):
        rel = "/" + os.path.relpath(path, WEB_DIR).replace(os.sep, "/")
        with open(path, "rb") as f:
            raw = f.read()
        gz = gzip.compress(raw, compresslevel=9, mtime=0)
        etag = '"%s-%s"' % (version, hashlib.sha1(gz).hexdigest()[:10])
        mime = MIME.get(os.path.splitext(path)[1].lower(), "application/octet-stream")
        total_raw += len(raw)
        total_gz += len(gz)
        if rel in FIRST_LOAD:
            first_gz += len(gz)
        print("web_embed: %-20s %7d B -> %6d B gz" % (rel, len(raw), len(gz)))

        body = ",".join(str(b) for b in gz)
        lines.append("static const uint8_t asset%d[] = {%s};" % (i, body))
        table.append('    { "%s", "%s", asset%d, sizeof(asset%d), %s },' % (rel, mime, i, i, '"\\"%s\\""' % etag.strip('"')))

    print("web_embed: razem %d B -> %d B gz (%d plikow)" % (total_raw, total_gz, len(files)))
    check_i18n(files)
    print("web_embed: pierwsze wejscie (szkielet + Stan) %d B gz, budzet %d B" % (first_gz, FIRST_LOAD_BUDGET))
    if first_gz > FIRST_LOAD_BUDGET:
        print("web_embed: UWAGA - pierwsze wejscie przekracza budzet (doc/dev/panel.md)")
    lines.append("")
    lines.append("const WebAsset WEB_ASSETS[] = {")
    lines.extend(table if table else ['    { "", "", nullptr, 0, "" },'])
    lines.append("};")
    lines.append("const size_t WEB_ASSET_COUNT = %d;" % len(files))
    content = "\n".join(lines) + "\n"

    os.makedirs(OUT_DIR, exist_ok=True)
    old = None
    if os.path.exists(OUT_FILE):
        with open(OUT_FILE, encoding="utf-8") as f:
            old = f.read()
    if old != content:
        with open(OUT_FILE, "w", encoding="utf-8") as f:
            f.write(content)


generate()
