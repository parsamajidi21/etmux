# Embedded and modern tmux: design notes

This fork adds serial consoles, boot awareness and a modern look to tmux.
Everything is opt-in: with an unchanged `~/.tmux.conf` tmux behaves as
upstream. This file has one section per feature: data structures,
lifecycle and failure modes.

## Map of the relevant internals

| Area | Where | Used by |
| --- | --- | --- |
| Pane creation | `spawn.c` `spawn_pane()`: allocates the pane, forks onto a pty with `fdforkpty()`, then `window_pane_set_event()` | A1: serial path skips the fork and uses an opened device fd |
| Pane I/O | `window.c` `window_pane_set_event()` (bufferevent on `wp->fd`), `window_pane_read_callback()` -> `input_parse_pane()`; `window_pane_error_callback()` on EOF/error | A1: disconnect handling; A2: output tap |
| Output parsing | `input.c` `input_parse_pane()` -> `input_parse_buffer()` (VT state machine, writes the screen) | A1: timestamps/log; A2: line matcher |
| Pane state | `tmux.h` `struct window_pane` (`fd`, `event`, `offset`, `pipe_*`, `base` screen) | A1: `serial` pointer |
| Options | `options-table.c` (table), `options.c` `options_push_changes()` (side effects of changes) | all |
| Formats | `format.c` `format_table[]` (sorted, `bsearch`), `format_cb_*` callbacks | A1/A2 formats |
| Hooks | `events.c` `events_fire()` with an `event_payload`; payload strings become `hook_*` formats (`events-payload.c`); hook names are `OPTIONS_TABLE_*_HOOK` entries | A1/A2 hooks |
| Status line | `status.c` (`status_redraw()`, `status_side_redraw()`), `format-draw.c` (styles, ranges) | side panel, B3 |
| Borders | `screen-redraw.c`, `window-border.c` (`pane-border-lines`, `pane-border-format`) | B1 |
| Menus/popups | `menu.c`, `cmd-display-menu.c` (`display-popup` is a floating modal pane) | choose-serial, palette |
| Copy mode | `window-copy.c` command table (`window_copy_cmd_table`) | A3 open-location |
| Jobs | `job.c` (`job_run()`), used by `run-shell` and `#()` | A3 editor launch |
| Theme colours | `colour.c` `colour_theme_table`, `server_client_update_theme_colours()`; light/dark from OSC 11 | B2 |

## A1. Native serial panes

**Commands:** `new-serial`, `serial-break`, `serial-set`,
`serial-reconnect`. `choose-serial` and its menu now open with
`new-serial` by default.

**Data structures:** `struct window_pane_serial` (`tmux.h`) is hung off
`wp->serial` (NULL for normal panes). It holds:
- the spec (a path or USB properties) and the resolved device path;
- the line settings: baud, flow, parity, data bits, stop bits;
- the state (`connected` or `disconnected`) and a reconnect timer;
- timestamp state (at start of line, time of the previous line);
- the log fd and path.

`struct spawn_context` gains `serial` and `serial_fd`.

**Lifecycle:**
1. `new-serial` creates the state from the session options (`serial-baud`,
   `serial-flow` and so on).
2. It resolves and opens the device *before* creating a pane, so a bad
   device leaves nothing behind. Opening is `O_RDWR|O_NOCTTY|O_NONBLOCK`,
   then `flock(LOCK_EX|LOCK_NB)` (the same convention as picocom and
   minicom), then raw termios.
3. `spawn_pane()` sees `sc->serial`, sets `wp->fd` to the device and jumps
   to the normal `complete:` path instead of forking. The pane gets the
   usual bufferevent, so output goes through `input_parse_pane()`. That is
   why scrollback, copy mode, search and `pipe-pane` work unchanged. Keys
   are written to the device by the normal key path.
4. `input_parse_pane()` calls `serial_pane_input()` for serial panes,
   instead of `input_parse_buffer()`. It writes the log and inserts
   timestamps at line starts. Non-serial panes pay one pointer test.
5. On EOF or a read error, `window_pane_error_callback()` calls
   `serial_pane_disconnected()` instead of marking the pane exited.
   - The **bufferevent is kept**: the pane's buffer offsets (`wp->offset`,
     `pipe_offset`, control-mode offsets) index into its input buffer.
   - Only the fd is closed and detached (`bufferevent_setfd(-1)`).
   - A dim `[tmux: ... disconnected]` line is written into the pane, and
     `pane-serial-disconnected` fires.
   - While `serial-reconnect` is on, a 500 ms timer resolves the spec again
     and reopens the device. On success it attaches the new fd, drops any
     output queued while disconnected, prints `[tmux: connected ...]` and
     fires `pane-serial-connected`.
6. `window_pane_destroy()` frees the state, which stops the timer and
   closes the log. `respawn-pane` is refused on serial panes:
   `serial-reconnect` is the equivalent.

**Device matching:** properties (`vid`, `pid`, `serial`/`ID_SERIAL_SHORT`,
`if`, or `usb:VID:PID`) are matched by walking
`/sys/class/tty/*/device` up to the USB interface (`bInterfaceNumber`)
and the USB device (`idVendor`, `idProduct`, `serial`). This needs no
libudev and works the same as udev's properties for USB serial. Matching
runs again on every reconnect, so re-enumeration (ttyUSB0 coming back as
ttyUSB1) is handled. If several devices match, the first by name is used,
so the choice is stable. The match is also logged.

**Failure modes:**

| Case | Behaviour |
| --- | --- |
| Device missing | `new-serial` fails with the path and `strerror`; no pane is created. |
| `EACCES` | The error says "permission denied (is the user in the dialout or uucp group?)". |
| `EBUSY`, or the lock is held by another program | The error says "device busy (in use by another program)". |
| Path is not a tty (for example a UART with no hardware) | `isatty()` fails and the error shows its errno (`Input/output error` for an empty ttyS). |
| Unsupported baud rate | Refused by `new-serial`/`serial-set`. Only rates with a `B*` constant are supported (see open questions). A failed `serial-set` restores the previous settings. |
| Disconnect mid escape sequence | The input parser state is kept, as for a pty that pauses. Our status line is written through the same parser, so it can be swallowed by a half-finished sequence. This is rare and cosmetic. Resetting the parser on disconnect would be the fix (open question). |
| Keys typed while disconnected | Dropped (`wp->fd == -1`). Queued output is drained on reconnect so a stale command is never sent to a board that just came back. |
| Server restart | Serial panes, like all panes, do not survive the server. |
| Window resize | `TIOCSWINSZ` is skipped for serial panes, since the device's window size means nothing. |

**Platform:** termios and `flock` are POSIX/BSD and build everywhere. USB
property matching uses Linux sysfs and fails with a clear error elsewhere;
paths work on all platforms.

**Tests:** `regress/serial-pane.sh` uses a `socat` pty pair. It skips if
`socat` is missing.

**Open questions:**
- Non-standard baud rates need `termios2`/`BOTHER` on Linux, which clashes
  with `<termios.h>`. Worth a small `osdep` helper?
- libudev monitoring would make reconnects instant. Polling every 500 ms is
  cheap and has no dependency, so it is the only implementation for now.
