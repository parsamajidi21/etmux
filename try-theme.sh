#!/bin/sh
# Run the freshly built ./tmux with embedded-modern.tmux.conf on a private server
# ("-L theme-test"), so your installed tmux and its sessions are untouched.
cd "$(dirname "$0")" || exit 1
[ -x ./tmux ] || { echo "build first: make" >&2; exit 1; }
unset TMUX
exec ./tmux -L theme-test -f ./embedded-modern.tmux.conf new-session -A -s main
