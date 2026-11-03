#!/bin/sh
# Replay a boot log to a serial line at a realistic pace: each line is
# written with CR LF after a short delay (DELAY, default 0.02s).
# Usage: replay-boot.sh log-file device
DELAY=${DELAY:-0.02}
while IFS= read -r line; do
	printf '%s\r\n' "$line"
	sleep "$DELAY"
done <"$1" >"$2"
