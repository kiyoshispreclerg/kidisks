#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Integration test on loop devices: never on a real disk.
#
# Builds a 64 MiB image with an MBR table and two partitions (ext4 and FAT),
# formatted inside the file (no root), attaches it with `udisksctl
# loop-setup` (polkit lets the session user do that), then drives kidisks
# with KIDISKS_ONLY_LOOP=1 so it refuses anything that isn't a loop.
#
# Usage: tests/integration/test-disks.sh [build/kidisks]
set -eu

KD=${1:-build/kidisks}
export KIDISKS_ONLY_LOOP=1 LC_ALL=C LANGUAGE=
PATH=$PATH:/usr/sbin:/sbin

for t in udisksctl sfdisk mke2fs mkfs.vfat; do
    command -v $t >/dev/null || { echo "SKIP: $t not installed"; exit 0; }
done

dir=$(mktemp -d "${TMPDIR:-/tmp}/kidisks-test.XXXXXX")
img=$dir/disk.img
loop=
fails=0

cleanup() {
    if [ -n "$loop" ]; then
        for p in "${loop}p1" "${loop}p2"; do
            udisksctl unmount --no-user-interaction -b "$p" >/dev/null 2>&1 || true
        done
        udisksctl loop-delete --no-user-interaction -b "$loop" >/dev/null 2>&1 || true
    fi
    rm -rf "$dir"
}
trap cleanup EXIT INT TERM

ok() { echo "ok   $1"; }
fail() { echo "FAIL $1"; fails=$((fails + 1)); }
check() { # description, command...
    d=$1; shift
    if "$@" >/dev/null 2>&1; then ok "$d"; else fail "$d"; fi
}

# 64 MiB: p1 ext4 24 MiB at 1 MiB, p2 FAT the rest.
truncate -s 64M "$img"
printf 'label: dos\nstart=2048, size=49152, type=83\nstart=51200, type=c\n' |
    sfdisk -q "$img"
mke2fs -q -F -t ext4 -L KDTEST1 -E offset=1048576 "$img" 24M
# The FAT size is in KiB: the 14336 sectors of p2.
mkfs.vfat -n KDTEST2 --offset 51200 "$img" 7168 >/dev/null 2>&1

out=$(udisksctl loop-setup --no-user-interaction -f "$img")
loop=$(echo "$out" | sed -n 's/.* as \(\/dev\/loop[0-9]*\)\.$/\1/p')
[ -n "$loop" ] || { echo "FAIL loop-setup: $out"; exit 1; }
name=${loop#/dev/}
echo "image on $loop"
# udev needs a moment to probe the partitions.
udevadm settle 2>/dev/null || sleep 1

list=$("$KD" --list --all --bytes)
line_disk=$(echo "$list" | grep "^$name ")
line_p1=$(echo "$list" | grep "^  ${name}p1 ")
line_p2=$(echo "$list" | grep "^  ${name}p2 ")
check "disk listed with its MBR" sh -c "echo '$line_disk' | grep -q ' 67108864  MBR'"
check "p1: start, size, ext4 and label" \
    sh -c "echo '$line_p1' | grep -q ' 25165824  @1048576  ext4  \"KDTEST1\"'"
check "p2: FAT with its label" sh -c "echo '$line_p2' | grep -q 'vfat  \"KDTEST2\"'"
check "loop hidden without --all" sh -c "! '$KD' --list | grep -q '^$name '"

where=$("$KD" --mount "${name}p1" 2>&1) && ok "mount p1 -> $where" || fail "mount p1: $where"
check "list shows the mountpoint and usage" \
    sh -c "'$KD' --list --all | grep '^  ${name}p1 ' | grep -q '$where.* used of '"
check "unmount p1" "$KD" --unmount "${name}p1"
check "list shows p1 unmounted" \
    sh -c "! '$KD' --list --all | grep '^  ${name}p1 ' | grep -q ' used of '"
check "unmounting again fails" sh -c "! '$KD' --unmount '${name}p1'"

# The guard: a name that isn't a loop is refused before reaching UDisks2.
# (A made-up name: even if the guard broke, nothing real would be touched.)
msg=$("$KD" --unmount sdzz9 2>&1 || true)
check "non-loop refused by KIDISKS_ONLY_LOOP" sh -c "echo '$msg' | grep -q KIDISKS_ONLY_LOOP"

if [ $fails -ne 0 ]; then
    echo "*** $fails disk test(s) FAILED"
    exit 1
fi
echo "*** all disk tests passed"
