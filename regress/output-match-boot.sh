#!/bin/sh

# Output triggers and boot detection. output-match patterns fire
# pane-output-match with hook_match_name/hook_match_line (escape sequences
# stripped, rate limited); boot-stages on a serial pane (socat pty pair)
# replaying fixture boot logs: stage and count formats, the pane-boot-stage
# hook, new boot on an earlier or "!" stage, autoboot-interrupt, boot-report
# and one serial log per boot.

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
LANG=C.UTF-8
export TERM LC_ALL LANG

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
command -v socat >/dev/null 2>&1 || { echo "socat not found, skipped"; exit 0; }
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d)
TMUX_TMPDIR="$OUT"
export TMUX_TMPDIR
TMUX="$TEST_TMUX -LtestB$$ -f/dev/null"
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

wait_for()
{
	option=$1
	expected=$2
	i=0

	while [ $i -lt 30 ]; do
		value=$($TMUX show -gqv "$option" 2>/dev/null || true)
		[ "$value" = "$expected" ] && return 0
		i=$((i + 1))
		sleep 0.2
	done
	fail "expected $option to be '$expected' but got '$value'"
}

replay()
{
	DELAY=0.005 sh "$HERE/fixtures/replay-boot.sh" "$HERE/fixtures/$1" "$OUT/board"
}

$TMUX new -d -s s -x 120 -y 30 'cat' || fail "new-session failed"

# Triggers on a normal pane. The line is coloured: matching must see the
# text without escape sequences.
$TMUX set -g output-match[0] 'panic:Kernel panic - not syncing'
$TMUX set -ga output-match 'oops:Internal error: Oops'
$TMUX set-hook -g pane-output-match \
	'set -gaF @m "#{hook_match_name}=#{hook_match_line};"'
$TMUX new-window -d -t s:1 \
	"printf '\\033[31mKernel panic - not syncing: VFS\\033[0m\\r\\n'; sleep 30"
wait_for @m 'panic=Kernel panic - not syncing: VFS;'

# Rate limit: a flood of matches fires at most 10 hooks a second.
$TMUX set -g @m ''
$TMUX new-window -d -t s:2 \
	"i=0; while [ \$i -lt 50 ]; do echo Internal error: Oops \$i; i=\$((i+1)); done; sleep 30"
sleep 1
n=$($TMUX show -gv @m | tr ';' '\n' | grep -c '^oops=')
[ "$n" -ge 1 ] && [ "$n" -le 10 ] || fail "rate limit let $n matches through"

# Boot detection is off for normal panes by default.
[ "$($TMUX display -pt s '#{pane_boot_count}')" = "0" ] || \
	fail "boot detected on a normal pane"
$TMUX set -gu output-match
$TMUX set-hook -gu pane-output-match

# Serial pane replaying boots.
socat pty,raw,echo=0,link="$OUT/board" pty,raw,echo=0,link="$OUT/host" \
	2>/dev/null &
SOCAT=$!
sleep 0.5
mkdir "$OUT/logs"
$TMUX set -g serial-log-dir "$OUT/logs"
$TMUX set-hook -g pane-boot-stage \
	'set -gaF @st "#{hook_boot_stage}#{hook_boot_count} "'
pane=$($TMUX new-serial -P -F '#{pane_id}' -t s "$OUT/host") || \
	fail "new-serial failed"
$TMUX set -p -t "$pane" autoboot-interrupt ' '
timeout 5 cat "$OUT/board" >"$OUT/typed" 2>/dev/null &

replay boot-imx8-linux.log
wait_for @st 'spl1 u-boot1 kernel1 init1 login1 '
[ "$($TMUX display -pt "$pane" '#{pane_boot_stage} #{pane_boot_count}')" = \
	"login 1" ] || fail "wrong stage after first boot"
[ -n "$($TMUX display -pt "$pane" '#{pane_boot_elapsed}')" ] || \
	fail "pane_boot_elapsed is empty"
sleep 0.3
[ "$(head -c 1 "$OUT/typed")" = " " ] || fail "autoboot interrupt not sent"

# Same boot again: SPL is marked "!" so it starts boot 2.
replay boot-imx8-linux.log
# Android boot without the login stage.
replay boot-android.log
wait_for @st 'spl1 u-boot1 kernel1 init1 login1 spl2 u-boot2 kernel2 init2 login2 spl3 u-boot3 kernel3 init3 login3 '

# An earlier stage (kernel after login) starts a new boot too.
printf '[    0.000000] Booting Linux on physical CPU 0x0\r\n' >"$OUT/board"
wait_for @st 'spl1 u-boot1 kernel1 init1 login1 spl2 u-boot2 kernel2 init2 login2 spl3 u-boot3 kernel3 init3 login3 kernel4 '

[ "$($TMUX boot-report -t "$pane" -N 10 | wc -l)" -eq 4 ] || \
	fail "boot-report did not list 4 boots"
$TMUX boot-report -t "$pane" | grep -q '^boot 1 .* spl [0-9.]*s u-boot [0-9.]*s kernel [0-9.]*s init [0-9.]*s login, total' || \
	fail "boot-report line is wrong"
$TMUX boot-report -t %0 2>/dev/null && fail "boot-report on a pane with no boots"

[ "$(ls "$OUT/logs" | wc -l)" -eq 4 ] || fail "expected one log per boot"

exit 0
