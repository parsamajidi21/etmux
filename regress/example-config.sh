#!/bin/sh

# embedded-modern.tmux.conf loads without errors and still sets up the
# side panel (TABS, DEVICES, FILES), the bottom bar and the click bindings.

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
LANG=C.UTF-8
export TERM LC_ALL LANG

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d)
TMUX_TMPDIR="$OUT"
export TMUX_TMPDIR
TMUX="$TEST_TMUX -LtestE$$ -f/dev/null"

fail()
{
	echo "$*" >&2
	$TMUX kill-server 2>/dev/null || true
	rm -rf "$OUT"
	exit 1
}
trap '$TMUX kill-server 2>/dev/null; rm -rf "$OUT"' EXIT

$TMUX new -d -s s || fail "new-session failed"
err=$($TMUX source-file "$HERE/../embedded-modern.tmux.conf" 2>&1) || \
	fail "config failed: $err"
[ -z "$err" ] || fail "config errors: $err"

[ "$($TMUX show -gv side-status)" = "left" ] || fail "side panel not on"
[ "$($TMUX show -gv side-status-width)" = "26" ] || fail "side panel width"
fmt=$($TMUX show -gv side-status-format)
for part in TABS DEVICES FILES serial_panel file_panel newsession; do
	case "$fmt" in
	*"$part"*) ;;
	*) fail "side-status-format has no $part" ;;
	esac
done
[ "$($TMUX show -gv status)" = "on" ] || fail "bottom bar not on"
$TMUX show -gv 'status-format[0]' | grep -q 'range=user|baud' || \
	fail "bottom bar has no baud chip"
for range in 'f\[0-9\]' newsession baud menu panel; do
	$TMUX list-keys -T root MouseDown1Status | grep -q "$range" || \
		fail "click binding does not handle $range"
done
$TMUX list-keys -T prefix b | grep -q toggle-side-status || \
	fail "prefix b does not toggle the panel"

exit 0
