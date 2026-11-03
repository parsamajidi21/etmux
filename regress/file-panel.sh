#!/bin/sh

# file_panel format and open-file: listing order (directories first, no
# hidden files), clickable ranges only for file-panel-extensions, the limit,
# open-file -i and with a path (including a name with # in it), and
# refusing directories and bad indexes.

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
LANG=C.UTF-8
export TERM LC_ALL LANG

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
OUT=$(mktemp -d)
TMUX_TMPDIR="$OUT"
export TMUX_TMPDIR
TMUX="$TEST_TMUX -LtestF$$ -f/dev/null"

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

mkdir -p "$OUT/p/src"
for f in main.c lib.rs README .hidden '#odd.c'; do
	: >"$OUT/p/$f"
done

$TMUX new -d -s s -c "$OUT/p" || fail "new-session failed"
$TMUX set -g editor "tail -f"
sleep 0.3

panel=$($TMUX display -p -t s '#{file_panel}')
names=$(printf '%s' "$panel" | sed 's/#\[nl\]/\n/g; s/#\[[^]]*\]//g; s/^ *//')
[ "$(echo $names)" = "src/ ##odd.c README lib.rs main.c" ] || \
	fail "wrong listing: $(echo $names)"
printf '%s' "$panel" | grep -q 'range=user|f1\]' || fail "#odd.c not openable"
printf '%s' "$panel" | grep -q 'range=user|f2\]' && fail "README openable"
printf '%s' "$panel" | grep -q 'range=user|f0\]' && fail "directory openable"

$TMUX set -g file-panel-limit 2
$TMUX display -p -t s '#{file_panel}' | grep -q '3 more' || fail "limit"
$TMUX set -gu file-panel-limit

$TMUX open-file -t s -i 4 || fail "open-file -i failed"
sleep 0.3
$TMUX list-panes -t s -F '#{pane_start_command}' | grep -q 'main.c' || \
	fail "main.c not opened"
$TMUX open-file -t s:0.0 -i 1 || fail "open-file of #odd.c failed"
sleep 0.3
$TMUX list-panes -t s -F '#{pane_start_command}' | grep -q '#odd.c' || \
	fail "#odd.c not opened"
$TMUX open-file -t s:0.0 src/new.c || fail "open-file with a path failed"
$TMUX open-file -t s:0.0 -i 0 2>/dev/null && fail "opened a directory"
$TMUX open-file -t s:0.0 -i 99 2>/dev/null && fail "opened a bad index"

exit 0
