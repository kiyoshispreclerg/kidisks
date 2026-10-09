#!/usr/bin/env python3
"""Compares `kidisks --list --all --bytes` with `lsblk -b` on this machine.

For every device lsblk shows (disks, partitions, dm/crypt), kidisks must
show the same size, start offset (from udevadm: this lsblk has no START), filesystem type, label and mountpoint.
Read-only: it only runs the two listing commands.

Usage: tools/check-lsblk.py [path/to/kidisks]"""
import os
import re
import shlex
import subprocess
import sys

exe = sys.argv[1] if len(sys.argv) > 1 else "build/kidisks"
env = dict(os.environ, LC_ALL="C", LANGUAGE="")
ours = subprocess.run([exe, "--list", "--all", "--bytes"], capture_output=True,
                      text=True, env=env, check=True).stdout.splitlines()
raw = subprocess.run(["lsblk", "-b", "-P", "-o",
                      "KNAME,TYPE,SIZE,FSTYPE,LABEL,MOUNTPOINT"],
                     capture_output=True, text=True, env=env, check=True).stdout


def part_start(kname):
    """Start in 512-byte sectors, from the partition entry (udev), which is
    independent from the sysfs file kidisks reads."""
    out = subprocess.run(["udevadm", "info", "-q", "property", "-n", kname],
                         capture_output=True, text=True, env=env).stdout
    m = re.search(r"^ID_PART_ENTRY_OFFSET=(\d+)$", out, re.M)
    return m.group(1) if m else ""


def our_line(kname):
    pat = re.compile(r"^\s*(-> )?" + re.escape(kname) + r"\b")
    for line in ours:
        if pat.match(line):
            return line
    return None


bad = checked = 0
for row in raw.splitlines():
    f = dict(kv.split("=", 1) for kv in shlex.split(row))
    if f["TYPE"] == "rom" or f["SIZE"] in ("", "0"):
        continue
    if f["TYPE"] == "part" and int(f["SIZE"]) <= 1024:
        continue  # extended: lsblk shows the 1 KiB stub, we the real extent
    line = our_line(f["KNAME"])
    problems = []
    if line is None:
        problems.append("missing")
    else:
        if (" %s " % f["SIZE"]) not in line + " ":
            problems.append("size %s" % f["SIZE"])
        start = part_start(f["KNAME"]) if f["TYPE"] == "part" else ""
        if start and ("@%d" % (int(start) * 512)) not in line:
            problems.append("start %s" % start)
        for key in ("FSTYPE", "MOUNTPOINT"):
            if f[key] and f[key] not in line:
                problems.append("%s %s" % (key.lower(), f[key]))
        if f["LABEL"] and ('"%s"' % f["LABEL"]) not in line:
            problems.append("label %s" % f["LABEL"])
    checked += 1
    if problems:
        bad += 1
        print("%s: %s" % (f["KNAME"], ", ".join(problems)))
        if line:
            print("    ours: " + line.strip())

print("%d devices checked, %d differ" % (checked, bad))
sys.exit(1 if bad else 0)
