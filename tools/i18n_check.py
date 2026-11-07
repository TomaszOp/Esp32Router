#!/usr/bin/env python3
"""Check of the firmware texts in two languages (doc/dev/jezyki.md).

- In every tr("polski", "english") both versions have the same printf specifiers (%s, %d, %lu...).
- Text literals in console output calls (print/printf) outside tr() are reported as "bez tr()"
  (codes and names such as "WPA2" or "block" are skipped; translate the rest or add it to KODY).

Exit code 1 on mismatched formats or texts without tr(). Standard library only.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIT = r'"(?:[^"\\]|\\.)*"'
# tr( "a" "b" , "c" "d" ) – concatenated literals over several lines (also with macros, e.g. IPSTR).
TR = re.compile(r"\btr\(\s*((?:(?:%s|IPSTR)\s*)+),\s*((?:(?:%s|IPSTR)\s*)+)\)" % (LIT, LIT))
SPEC = re.compile(r"%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t)?[diouxXeEfgGcsp%]")
PRINT = re.compile(r"\b(?:\w+(?:->|\.))?printf?\(")
# Literals in print/printf that are not text to translate (codes, names, data formats).
KODY = {"WPA2", "block", "allow", "NXDOMAIN", "router", "true/false"}
# Whole literals without words to translate (labels shared by both languages).
WSPOLNE = {"[%s.json]\\r\\n", "Host:    %s\\r\\n", "NAT:     %s\\r\\n", "Uptime:  %lud %02lu:%02lu:%02lu\\r\\n",
           ", LittleFS %u / %u KB", "  %-16s prio %2u  %6u B%s\\r\\n"}


def text(group):
    """Concatenated content of the string literals in a matched group (without quotes)."""
    return "".join(m[1:-1] for m in re.findall(LIT, group))


def call_args(src, start):
    """Call arguments from the position after '(' up to the matching ')'."""
    depth, j = 1, start
    while depth:
        c = src[j]
        if c == '"':
            j += 1
            while src[j] != '"':
                j += 2 if src[j] == "\\" else 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        j += 1
    return src[start:j - 1]


def without_tr(args):
    """Arguments with the tr(...) parts removed."""
    out, i = [], 0
    for m in re.finditer(r"\btr\(", args):
        if m.start() < i:
            continue
        out.append(args[i:m.start()])
        i = m.end() + len(call_args(args, m.end())) + 1
    out.append(args[i:])
    return "".join(out)


def is_text(s):
    """Whether a literal contains words to translate (letters besides printf specifiers, not a code)."""
    s = s.replace("\\r", " ").replace("\\n", " ")
    s = SPEC.sub(" ", s)
    return re.search(r"[A-Za-z]{3,}", s) is not None and s.strip() not in KODY


def main():
    """Checks all sources in src/, prints problems and a summary, exits with 1 on errors."""

    pairs = bad = untranslated = 0
    files = sorted(glob.glob(os.path.join(ROOT, "src", "**", "*.cpp"), recursive=True) +
                   glob.glob(os.path.join(ROOT, "src", "**", "*.h"), recursive=True))
    for path in files:
        if os.sep + "generated" + os.sep in path:
            continue
        rel = os.path.relpath(path, ROOT)
        with open(path, encoding="utf-8") as f:
            src = f.read()
        for m in TR.finditer(src):
            pairs += 1
            pl, en = text(m.group(1)), text(m.group(2))
            if SPEC.findall(pl) != SPEC.findall(en):
                bad += 1
                print("%s:%d: rozne formaty: %r / %r" % (rel, src.count("\n", 0, m.start()) + 1, pl, en))
        for m in PRINT.finditer(src):
            line_start = src.rfind("\n", 0, m.start()) + 1
            if "LOG_" in src[line_start:m.start()]:
                continue
            for lit in re.findall(LIT, without_tr(call_args(src, m.end()))):
                if lit[1:-1] not in WSPOLNE and is_text(lit[1:-1]):
                    untranslated += 1
                    print("%s:%d: bez tr(): %s" % (rel, src.count("\n", 0, m.start()) + 1, lit))
    print("i18n_check: %d par tr(), niezgodnych formatow %d, tekstow bez tr() %d" % (pairs, bad, untranslated))
    sys.exit(1 if bad or untranslated else 0)


if __name__ == "__main__":
    main()
