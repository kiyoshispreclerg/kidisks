#!/usr/bin/env python3
"""Fills empty msgstr entries of a .po from a {msgid: msgstr} JSON file.

Usage: tools/fill_po.py po/pt_BR.po translations.json

For bulk-translating new strings; entries already translated are kept.
Handles msgids split over several lines, as xgettext writes them."""
import json
import sys


def unquote(s):
    return json.loads(s)


def quote(s):
    # Same line splitting as msgmerge is not needed: msgfmt accepts one line.
    return json.dumps(s, ensure_ascii=False)


def main():
    path, table_path = sys.argv[1], sys.argv[2]
    table = json.load(open(table_path, encoding="utf-8"))
    lines = open(path, encoding="utf-8").read().split("\n")
    out, i, used = [], 0, set()
    while i < len(lines):
        line = lines[i]
        if line.startswith("msgid "):
            parts = [line[6:]]
            i += 1
            while i < len(lines) and lines[i].startswith('"'):
                parts.append(lines[i])
                i += 1
            msgid = "".join(unquote(p) for p in parts)
            out.append("msgid " + parts[0])
            out.extend(parts[1:])
            if i < len(lines) and lines[i].startswith("msgstr "):
                sparts = [lines[i][7:]]
                i += 1
                while i < len(lines) and lines[i].startswith('"'):
                    sparts.append(lines[i])
                    i += 1
                cur = "".join(unquote(p) for p in sparts)
                if msgid and not cur and msgid in table:
                    out.append("msgstr " + quote(table[msgid]))
                    used.add(msgid)
                else:
                    out.append("msgstr " + sparts[0])
                    out.extend(sparts[1:])
            continue
        out.append(line)
        i += 1
    open(path, "w", encoding="utf-8").write("\n".join(out))
    for k in table:
        if k not in used:
            print("not used:", k[:60], file=sys.stderr)


main()
