#!/bin/sh

# Semantic theme colours and source-theme: @name and theme* colours in
# styles, the dark-theme-* and light-theme-* semantic options, the built-in
# themes, classic returning every option to its default, loading a theme
# file, and a client being drawn with the accent colour and redrawn when it
# changes.

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
LANG=C.UTF-8
export TERM LC_ALL LANG

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
OUT=$(mktemp -d)
TMUX_TMPDIR="$OUT"
export TMUX_TMPDIR
TMUX="$TEST_TMUX -LtestT$$ -f/dev/null"

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

$TMUX new -d -s s || fail "new-session failed"

# Colour names.
for c in @bg @fg @accent @muted @ok @warn @error @blue themeaccent \
    themeerror; do
	$TMUX set -g status-style "fg=$c" || fail "colour $c rejected"
done
$TMUX set -g status-style 'fg=@nonsense' 2>/dev/null && \
	fail "unknown @ colour accepted"
$TMUX set -gu status-style

for slot in bg fg accent muted ok warn error; do
	$TMUX show -gv dark-theme-$slot >/dev/null || \
		fail "no dark-theme-$slot"
	$TMUX show -gv light-theme-$slot >/dev/null || \
		fail "no light-theme-$slot"
done

# Built-in themes.
[ "$($TMUX source-theme -l | tr '\n' ' ')" = "dark light auto classic " ] || \
	fail "wrong theme list"
$TMUX show -g >"$OUT/g0"
$TMUX show -gw >"$OUT/w0"
$TMUX source-theme dark || fail "source-theme dark failed"
[ "$($TMUX show -gv theme)" = "dark" ] || fail "dark did not set theme"
[ "$($TMUX show -gwv pane-border-lines)" = "rounded" ] || \
	fail "dark did not set pane-border-lines"
$TMUX source-theme light
[ "$($TMUX show -gv theme)" = "light" ] || fail "light did not set theme"
$TMUX source-theme auto
[ "$($TMUX show -gv theme)" = "detect" ] || fail "auto did not set theme"
$TMUX source-theme classic || fail "source-theme classic failed"
$TMUX show -g >"$OUT/g1"
$TMUX show -gw >"$OUT/w1"
cmp -s "$OUT/g0" "$OUT/g1" || fail "classic did not restore session options"
cmp -s "$OUT/w0" "$OUT/w1" || fail "classic did not restore window options"

# Theme file.
printf 'set -g @from-theme-file yes\n' >"$OUT/my.theme"
$TMUX source-theme "$OUT/my.theme" || fail "theme file failed"
[ "$($TMUX show -gv @from-theme-file)" = "yes" ] || fail "theme file not run"
$TMUX source-theme "$OUT/missing.theme" 2>/dev/null && \
	fail "missing theme file accepted"

# A true colour client is drawn with the accent and redrawn on change.
if command -v script >/dev/null 2>&1; then
	$TMUX set -g dark-theme-accent '#123456'
	$TMUX set -g light-theme-accent '#123456'
	$TMUX set -g status-style 'bg=@accent'
	$TMUX set -as terminal-features ',*:RGB'
	script -qfc "stty rows 10 cols 60; TERM=xterm-256color $TMUX attach -t s" \
		"$OUT/screen" </dev/null >/dev/null 2>&1 &
	sleep 1.5
	grep -q "$(printf '\033')\[[0-9;]*48;2;18;52;86m" "$OUT/screen" || \
		fail "accent colour not drawn"
	$TMUX set -g dark-theme-accent '#654321'
	$TMUX set -g light-theme-accent '#654321'
	sleep 1
	grep -q "$(printf '\033')\[[0-9;]*48;2;101;67;33m" "$OUT/screen" || \
		fail "changed accent colour not drawn"
fi

exit 0
