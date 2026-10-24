#!/bin/sh
# Checks an installed kidisks: the version matches, every library the
# binary needs is there, the installed files exist, --list and --scan run,
# and the window opens on a virtual X screen and exits after the first map.
# Read-only: it never mounts or changes a disk.
#
# Usage: tools/smoke-test.sh      (from the repository root)
# Needs xvfb-run and fonts, and a non-root user (kidisks refuses root). Touches nothing of the user's: HOME lives in a
# temporary folder.
set -eu

[ "$(id -u)" -ne 0 ] || { echo "kidisks refuses to run as root: use a plain user" >&2; exit 2; }
[ -f config.mk ] || { echo "run it from the repository root" >&2; exit 2; }
version=$(sed -n 's/^VERSION *= *//p' config.mk)

work=$(mktemp -d /tmp/kd-smoke.XXXXXX)
trap 'rm -rf "$work"' EXIT
export HOME=$work NO_AT_BRIDGE=1
unset XDG_DATA_HOME XDG_CONFIG_HOME XDG_RUNTIME_DIR

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

echo "== version"
got=$(kidisks --version 2>&1) || fail "--version exited with an error: $got"
[ "$got" = "kidisks $version" ] || fail "--version: \"$got\", expected \"kidisks $version\""

echo "== libraries"
missing=$(ldd "$(command -v kidisks)" | grep "not found" || true)
[ -z "$missing" ] || fail "$missing"

echo "== installed files"
prefix=$(dirname "$(dirname "$(command -v kidisks)")")
for f in "$prefix/share/applications/kidisks.desktop" \
	"$prefix/share/icons/hicolor/scalable/apps/kidisks.svg" \
	"$prefix/share/locale/pt_BR/LC_MESSAGES/kidisks.mo"; do
	[ -f "$f" ] || fail "missing $f"
done

echo "== disk model (no disks may be present in a container)"
kidisks --list > "$work/list.log" 2>&1 || { cat "$work/list.log" >&2; fail "--list failed"; }

echo "== scan"
kidisks --scan /usr/share -b --top 0 > "$work/scan.log" 2>&1 || { cat "$work/scan.log" >&2; fail "--scan failed"; }

echo "== window opens"
status=0
xvfb-run -a -s "-screen 0 1200x800x24" \
	timeout 60 kidisks -v --exit-after-map > "$work/window.log" 2>&1 || status=$?
if [ "$status" -ne 0 ]; then
	cat "$work/window.log" >&2
	fail "kidisks --exit-after-map exited with status $status"
fi

echo "OK"
