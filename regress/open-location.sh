#!/bin/sh

# open-location in copy mode: extraction of file:line[:column] from compiler
# (gcc, clang), linker, dtc, kernel, ninja, soong, bitbake, Python and make
# output (regress/fixtures/locations.txt: name, line, expected
# file|line|column or empty for no location), the cursor choosing between
# two locations on a line, open-location-rewrite, relative paths joined to
# the pane directory and the remote command with @remote-host.

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
TMUX="$TEST_TMUX -LtestL$$ -f/dev/null"

fail()
{
	echo "$*" >&2
	$TMUX kill-server 2>/dev/null || true
	rm -rf "$OUT"
	exit 1
}

cleanup()
{
	$TMUX kill-server 2>/dev/null || true
	rm -rf "$OUT"
}
trap cleanup EXIT

# Show a line in a fresh window, put the copy mode cursor on it at column
# $2 and run open-location. Prints what the command was given.
open_at()
{
	printf '%s\n' "$1" >"$OUT/line"
	rm -f "$OUT/got"
	$TMUX kill-window -t s:9 2>/dev/null
	$TMUX new-window -d -t s:9 -c "$OUT" "cat '$OUT/line'; sleep 60"
	sleep 0.2
	$TMUX copy-mode -t s:9
	$TMUX send -t s:9 -X history-top
	$TMUX send -t s:9 -X start-of-line
	i=0
	while [ $i -lt "$2" ]; do
		$TMUX send -t s:9 -X cursor-right
		i=$((i + 1))
	done
	$TMUX send -t s:9 -X open-location
	i=0
	while [ ! -s "$OUT/got" ] && [ $i -lt 20 ]; do
		sleep 0.1
		i=$((i + 1))
	done
	cat "$OUT/got" 2>/dev/null
}

$TMUX new -d -s s -x 200 -y 10 || fail "new-session failed"
$TMUX set -g open-location-command \
	"printf '%s|%s|%s' #{q:location_file} '#{location_line}' '#{location_column}' >'$OUT/got'"
$TMUX set -g open-location-remote-command \
	"printf 'remote %s %s' '#{location_remote_host}' #{q:location_file} >'$OUT/got'"

TAB=$(printf '\t')
while IFS="$TAB" read -r name line expected; do
	got=$(open_at "$line" 0)
	if [ -n "$expected" ]; then
		case "$expected" in
		/*)	want=$expected ;;
		*)	want="$OUT/$expected" ;;
		esac
	else
		want=
	fi
	[ "$got" = "$want" ] || fail "$name: expected '$want' got '$got'"
done <"$HERE/fixtures/locations.txt"

# Two locations on one line: the cursor picks.
line='a.c:1:2: note: see b.c:30:4'
[ "$(open_at "$line" 0)" = "$OUT/a.c|1|2" ] || fail "cursor at start"
[ "$(open_at "$line" 22)" = "$OUT/b.c|30|4" ] || fail "cursor on second"

# Rewrite rules (a Yocto container workdir to a local tree).
$TMUX set -g open-location-rewrite[0] '^/workdir/=>/home/me/yocto/'
[ "$(open_at '/workdir/build/foo.c:5: error' 0)" = \
	"/home/me/yocto/build/foo.c|5|" ] || fail "rewrite not applied"
$TMUX set -gu open-location-rewrite

# Remote command when @remote-host is set; relative paths stay relative.
$TMUX set -g @remote-host imx8-board
[ "$(open_at 'src/x.c:3: error' 0)" = "remote imx8-board src/x.c" ] || \
	fail "remote command not used"

# A pane running ssh (Linux): the host comes from its arguments.
$TMUX set -gu @remote-host
if [ -d /proc/self ] && command -v bash >/dev/null 2>&1; then
	$TMUX kill-window -t s:8 2>/dev/null
	$TMUX new-window -d -t s:8 "printf 'drivers/x.c:7: error\\n'; exec bash -c \"exec -a ssh sh -c 'sleep 60' -p 2222 root@imx8-board\""
	sleep 0.5
	rm -f "$OUT/got"
	$TMUX copy-mode -t s:8
	$TMUX send -t s:8 -X history-top
	$TMUX send -t s:8 -X open-location
	sleep 0.5
	[ "$(cat "$OUT/got" 2>/dev/null)" = "remote root@imx8-board drivers/x.c" ] || \
		fail "ssh host not detected: '$(cat "$OUT/got" 2>/dev/null)'"
fi

exit 0
