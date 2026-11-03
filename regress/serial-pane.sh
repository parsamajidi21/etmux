#!/bin/sh

# Serial panes (new-serial): a socat pty pair stands in for a board. One end
# is opened by tmux, the test writes to and reads from the other. Covers
# output and input through the pane, the pane_serial_* formats, serial-set,
# serial-break, wallclock and delta timestamps, logging, the advisory lock,
# disconnect when the pty pair goes away, automatic reconnect when it comes
# back, the pane-serial-* hooks, and refusing respawn-pane.

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
LANG=C.UTF-8
export TERM LC_ALL LANG

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
command -v socat >/dev/null 2>&1 || { echo "socat not found, skipped"; exit 0; }
OUT=$(mktemp -d)
TMUX_TMPDIR="$OUT"
export TMUX_TMPDIR
TMUX="$TEST_TMUX -LtestS$$ -f/dev/null"
SOCAT=

fail()
{
	echo "$*" >&2
	$TMUX kill-server 2>/dev/null || true
	[ -n "$SOCAT" ] && kill $SOCAT 2>/dev/null
	rm -rf "$OUT"
	exit 1
}

cleanup()
{
	$TMUX kill-server 2>/dev/null || true
	[ -n "$SOCAT" ] && kill $SOCAT 2>/dev/null
	rm -rf "$OUT"
}
trap cleanup EXIT

start_line()
{
	socat pty,raw,echo=0,link="$OUT/board" pty,raw,echo=0,link="$OUT/host" \
	    2>/dev/null &
	SOCAT=$!
	i=0
	while [ ! -e "$OUT/host" ] && [ $i -lt 50 ]; do
		sleep 0.1
		i=$((i + 1))
	done
	[ -e "$OUT/host" ] || fail "socat did not create the pty pair"
}

stop_line()
{
	kill $SOCAT 2>/dev/null
	wait $SOCAT 2>/dev/null
	SOCAT=
}

wait_for_fmt()
{
	target=$1
	fmt=$2
	expected=$3
	i=0

	while [ $i -lt 30 ]; do
		value=$($TMUX display -pt "$target" "$fmt" 2>/dev/null || true)
		[ "$value" = "$expected" ] && return 0
		i=$((i + 1))
		sleep 0.2
	done
	fail "expected $fmt to be '$expected' but got '$value'"
}

wait_for_text()
{
	target=$1
	text=$2
	i=0

	while [ $i -lt 30 ]; do
		$TMUX capture-pane -pt "$target" | grep -q -- "$text" && return 0
		i=$((i + 1))
		sleep 0.2
	done
	fail "pane $target does not show '$text'"
}

start_line
mkdir "$OUT/logs"
$TMUX new -d -s s -x 100 -y 30 || fail "new-session failed"
$TMUX set -g serial-log-dir "$OUT/logs"
$TMUX set-hook -g pane-serial-disconnected 'set -ga @events D'
$TMUX set-hook -g pane-serial-connected 'set -ga @events C'

# Bad devices fail without leaving a pane.
$TMUX new-serial -t s "$OUT/nonexistent" 2>/dev/null && \
	fail "new-serial on a missing device succeeded"
$TMUX new-serial -t s vid=dead,pid=beef 2>/dev/null && \
	fail "new-serial matched a nonexistent USB device"
[ "$($TMUX list-panes -t s | wc -l)" -eq 1 ] || fail "failed new-serial left a pane"

pane=$($TMUX new-serial -P -F '#{pane_id}' -B 57600 -t s "$OUT/host") || \
	fail "new-serial failed"
wait_for_fmt "$pane" '#{pane_serial} #{pane_serial_baud} #{pane_serial_state}' \
	'1 57600 connected'
[ "$($TMUX display -pt "$pane" '#{pane_serial_device}')" = "$OUT/host" ] || \
	fail "pane_serial_device is wrong"
[ "$($TMUX display -pt %0 '#{pane_serial}')" = "0" ] || \
	fail "pane_serial is not 0 for a normal pane"

# Second open of the same device is refused (advisory lock).
$TMUX new-serial -t s "$OUT/host" 2>/dev/null && \
	fail "device opened twice"

# Output from the board.
printf 'U-Boot SPL 2024.01\r\n' >"$OUT/board"
wait_for_text "$pane" 'U-Boot SPL 2024.01'

# Input to the board.
timeout 3 head -c 9 "$OUT/board" >"$OUT/typed" &
reader=$!
sleep 0.3
$TMUX send-keys -t "$pane" 'printenv' Enter
wait $reader
[ "$(cat "$OUT/typed")" = "$(printf 'printenv\r')" ] || \
	fail "keys did not reach the device"

# Line settings.
$TMUX serial-set -t "$pane" -b 115200 -p even -f rtscts || \
	fail "serial-set failed"
wait_for_fmt "$pane" '#{pane_serial_baud}' '115200'
$TMUX serial-set -t "$pane" -b 12345 2>/dev/null && \
	fail "serial-set accepted an unsupported baud rate"
[ "$($TMUX display -pt "$pane" '#{pane_serial_baud}')" = "115200" ] || \
	fail "failed serial-set changed the baud rate"
$TMUX serial-set -t "$pane" -p none -f none
$TMUX serial-break -t "$pane" || fail "serial-break failed"

# Timestamps.
$TMUX set -p -t "$pane" serial-timestamps wallclock
printf 'stamped\r\n' >"$OUT/board"
wait_for_text "$pane" '^\[[0-9][0-9]:[0-9][0-9]:[0-9][0-9]\.[0-9][0-9][0-9]\] stamped'
$TMUX set -p -t "$pane" serial-timestamps delta
printf 'delta1\r\ndelta2\r\n' >"$OUT/board"
wait_for_text "$pane" '^\[+[0-9]*\.[0-9][0-9][0-9]\] delta2'
$TMUX set -p -t "$pane" serial-timestamps off

# Logging.
log=$(ls "$OUT/logs" | head -1)
[ -n "$log" ] || fail "no log file written"
grep -q 'U-Boot SPL 2024.01' "$OUT/logs/$log" || fail "log is missing output"

# Unplug and replug.
stop_line
wait_for_fmt "$pane" '#{pane_serial_state}' 'disconnected'
wait_for_text "$pane" 'disconnected, waiting'
start_line
wait_for_fmt "$pane" '#{pane_serial_state}' 'connected'
printf 'after replug\r\n' >"$OUT/board"
wait_for_text "$pane" 'after replug'
[ "$($TMUX show -gv @events)" = "DC" ] || \
	fail "hooks fired '$($TMUX show -gv @events)' instead of DC"

# Without serial-reconnect the pane stays disconnected.
$TMUX set -p -t "$pane" serial-reconnect off
stop_line
wait_for_fmt "$pane" '#{pane_serial_state}' 'disconnected'
start_line
sleep 1
[ "$($TMUX display -pt "$pane" '#{pane_serial_state}')" = "disconnected" ] || \
	fail "reconnected with serial-reconnect off"
$TMUX serial-reconnect -t "$pane" || fail "serial-reconnect failed"
wait_for_fmt "$pane" '#{pane_serial_state}' 'connected'

$TMUX respawn-pane -k -t "$pane" 2>/dev/null && \
	fail "respawn-pane on a serial pane succeeded"
$TMUX kill-pane -t "$pane" || fail "kill-pane failed"

exit 0
