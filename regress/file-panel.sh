#!/bin/sh

# file_panel format and open-file: listing order (directories first, no
# hidden files), every entry clickable, the limit, each file opening in its
# own window (editor for text, file-view-command for binary, a shell for a
# directory, a name with # in it), opening again selecting the window, and
# refusing bad indexes.

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

$TMUX new -d -s s -x 300 -y 30 -c "$OUT/p" || fail "new-session failed"
$TMUX set -g editor "tail -f"
sleep 0.3

head -c 64 /dev/zero >"$OUT/p/blob.bin"
printf 'int x;\n' >"$OUT/p/main.c"

panel=$($TMUX display -p -t s '#{file_panel}')
names=$(printf '%s' "$panel" | sed 's/#\[nl\]/\n/g; s/#\[[^]]*\]//g; s/^ *//')
[ "$(echo $names)" = "src/ ##odd.c README blob.bin lib.rs main.c" ] || \
	fail "wrong listing: $(echo $names)"
for i in 0 1 2 3 4 5; do
	printf '%s' "$panel" | grep -q "range=user|f$i\]" || \
		fail "entry $i not clickable"
done

$TMUX set -g file-panel-limit 2
$TMUX display -p -t s '#{file_panel}' | grep -q '4 more' || fail "limit"
$TMUX set -gu file-panel-limit

# file-open-mode window: each opens in its own window named after it;
# opening again selects it.
$TMUX set -g file-open-mode window
$TMUX set -g file-open-terminal off
$TMUX set -g file-view-command 'echo VIEWER %1; sleep 60'
$TMUX open-file -t s -i 5 || fail "open-file -i failed"
$TMUX open-file -t s:0 -i 1 || fail "open-file of #odd.c failed"
$TMUX open-file -t s:0 -i 3 || fail "open-file of blob.bin failed"
$TMUX open-file -t s:0 -i 0 || fail "open-file of a directory failed"
sleep 0.5
[ "$($TMUX list-windows -t s -F '#{window_name}' | tr '\n' ' ')" = \
	"bash main.c #odd.c blob.bin src/ " ] || \
	[ "$($TMUX list-windows -t s -F '#{window_name}' | sed 1d | tr '\n' ' ')" = \
	"main.c #odd.c blob.bin src/ " ] || \
	fail "wrong windows: $($TMUX list-windows -t s -F '#{window_name}' | tr '\n' ' ')"
$TMUX list-windows -t s -F '#{pane_start_command}' | grep -q 'tail -f.*main.c' || \
	fail "main.c not opened with the editor"
w=$($TMUX list-windows -t s -F '#{window_index} #{window_name}' | \
	awk '$2 == "blob.bin" { print $1 }')
$TMUX capture-pane -p -t "s:$w" | grep -q '^VIEWER /' || \
	fail "blob.bin not opened with the viewer"
n=$($TMUX display -p -t s: "#{session_windows}")
$TMUX open-file -t s:0 -i 5
[ "$($TMUX display -p -t s: "#{session_windows} #{window_name}")" = "$n main.c" ] || \
	fail "opening main.c again did not select its window"
$TMUX open-file -t s:0 src/new.c || fail "open-file with a path failed"
$TMUX open-file -t s:0 -i 99 2>/dev/null && fail "opened a bad index"

# file-open-mode right (default): the terminal stays, the file is on its
# right, the previous file goes to its own window and comes back when
# opened again.
$TMUX kill-server
$TMUX new -d -s r -x 200 -y 30 -c "$OUT/p" || fail "new-session failed"
$TMUX set -g editor "tail -f"
term=$($TMUX display -p -t r: '#{pane_id}')
side()
{
	$TMUX list-panes -t r:0 -F '#{pane_id}:#{b:@open-file}' | tr '\n' ' '
}
$TMUX open-file -t "$term" main.c
p1=$($TMUX list-panes -t r:0 -F '#{pane_id} #{b:@open-file}' | \
	awk '$2 == "main.c" { print $1 }')
[ "$(side)" = "$term: $p1:main.c " ] || fail "main.c not beside: $(side)"
[ "$($TMUX display -p -t "$term" '#{pane_left}')" = "0" ] || \
	fail "terminal moved"
$TMUX open-file -t "$term" lib.rs
case "$(side)" in
"$term: %"*":lib.rs ") ;;
*) fail "lib.rs not beside: $(side)" ;;
esac
$TMUX list-windows -t r: -F '#{window_name}' | grep -qx main.c || \
	fail "main.c not moved to its own window"
$TMUX open-file -t "$term" main.c
[ "$(side)" = "$term: $p1:main.c " ] || fail "main.c not swapped back"
$TMUX list-windows -t r: -F '#{window_name}' | grep -qx lib.rs || \
	fail "lib.rs not moved to its own window"
n=$($TMUX list-panes -s -t r: | wc -l)
$TMUX open-file -t "$term" main.c
[ "$($TMUX list-panes -s -t r: | wc -l)" = "$n" ] || \
	fail "opening the shown file again made a pane"

exit 0
