# etmux

**etmux** is a fork of [tmux](https://github.com/tmux/tmux) for embedded
and BSP developers. It adds the following to tmux:
- serial consoles as real tmux panes
- boot detection
- alerts on kernel panics and similar
- jumping from compiler errors to your editor
- a clickable side panel
- a modern look

It is still tmux. With your existing `~/.tmux.conf`, it behaves exactly
like upstream tmux. Every new feature is opt-in.

```
┌ TABS ──────────────┬──────────────────────────┬──────────────────────────┐
│  main              │ $ make -j8               │ ⚡ ttyUSB0 @ 115200       │
│   ▎ 1 bash         │ ...                      │  u-boot 3s (boot 2)      │
│  + New session     │ drivers/uart.c:142:17:   │ U-Boot 2023.04 ...       │
│                    │   error: 'baud' undecl.. │ Hit any key to stop ...  │
│ DEVICES            │                          │ => printenv              │
│  ● ttyUSB0  FT232R │                          │                          │
│  ○ ttyACM0  STLink │                          │                          │
│ FILES  myproject   │                          │                          │
│  src/              │                          │                          │
│  main.c            │                          │                          │
│  board.dts         │                          │                          │
└────────────────────┴──────────────────────────┴──────────────────────────┘
  ≡  main  bash │ myproject                       115200 baud ▾    23:45
```

## Features

### Serial consoles (native, no picocom needed)
- **`new-serial /dev/ttyUSB0`** opens a UART directly in a pane.
  Scrollback, copy mode, search and `pipe-pane` work as in any other
  pane, and what you type goes to the board.
- **Find adapters by USB identity**, not by a name that changes when
  you plug them in again: `new-serial vid=0403,pid=6001,serial=A50285BI`
  or `new-serial usb:0483`.
- **Auto-reconnect.** Unplug the adapter or power-cycle the board: the
  pane says *disconnected*, and reconnects by itself when the device
  comes back.
- **Line settings:**
  - `serial-set -b 921600` changes the baud rate live; `-p` sets parity,
    `-f` flow control, `-D` data bits and `-s` stop bits.
  - `serial-break` sends a break; `serial-reconnect` closes and reopens
    the device.
- **Per-line timestamps:** `set -p serial-timestamps wallclock` (or
  `delta`).
- **Logs:** `set -g serial-log-dir ~/serial-logs` writes one log file
  per boot.

### Boot detection and alerts
- **Boot stages:** follows SPL → U-Boot → kernel → init → login in
  serial panes. The current stage and the time since reset are
  available to the status line and the pane border
  (`#{pane_boot_stage}`, `#{pane_boot_elapsed}`, `#{pane_boot_count}`).
- **`boot-report`** shows how long each stage took over the last boots.
- **Stop autoboot automatically:** `set -g autoboot-interrupt " "`
  presses a key when U-Boot says *Hit any key to stop autoboot*.
- **Alerts on output:** `output-match` patterns raise an alert on things
  like `Kernel panic`, `Oops`, `BUG:`, `avc: denied` or crashed services
  (the `pane-output-match` hook).

### Jump from errors to your editor
In copy mode, put the cursor on an error such as `drivers/uart.c:142:17:`
and press **`Ctrl+o`** (vi keys) or **`Alt+o`** (emacs keys). The file
opens at that line in VS Code by default.
- **Recognised formats:** gcc/clang, ld, dtc, kernel warnings,
  ninja/soong, bitbake and Python tracebacks.
- **Over SSH:** if the pane is an `ssh` session, the file opens through
  VS Code Remote.
- **Container and Yocto paths** can be rewritten to your local checkout
  with `open-location-rewrite`.

### Side panel (Tabs, Devices, Files)
- **TABS:** your sessions and windows. Click to switch; *+ New session*
  asks for a name.
- **DEVICES:** detected `/dev/ttyUSB*` and `/dev/ttyACM*` adapters with
  their USB product name. Click one to open its console, or to jump to
  it if it is already open.
- **FILES:** the current pane's directory. Click a file to open it next
  to your terminal: source files in your editor, binaries in a viewer
  (`readelf` for ELF, a hex dump otherwise), directories as a shell.
- **Show or hide it:** **`Ctrl+b b`** slides the panel in and out.

### Modern look
- **Themes:** `source-theme dark | light | auto | classic`. `auto`
  follows your terminal's light or dark background; `classic` returns
  to the stock tmux look.
- **Semantic colours:** use them in any style, e.g. `fg=@accent`,
  `bg=@bg`, `@ok`, `@warn`, `@error`. Change the palette with
  `dark-theme-*` and `light-theme-*` and everything follows.
- **Rounded borders, pane titles and dimmed inactive panes.** These use
  true colour, with automatic fallback to 256 or 16 colours.

## Install

### 1. Dependencies

Debian / Ubuntu:

```sh
sudo apt install git build-essential autoconf automake pkg-config bison \
    libevent-dev libncurses-dev
```

Fedora:

```sh
sudo dnf install git gcc make autoconf automake pkgconf bison \
    libevent-devel ncurses-devel
```

Optional:
- `socat` is used by the serial tests.
- `xxd` or `hexyl` and `readelf` are used to view binary files.
- A [Nerd Font](https://www.nerdfonts.com/) in your terminal gives the
  icons in the example config; everything works without one.

### 2. Build

```sh
git clone https://github.com/parsamajidi21/etmux.git
cd etmux
git checkout embedded-serial    # the branch with the etmux features
sh autogen.sh
./configure
make -j"$(nproc)"
```

### 3. Try it without installing

This runs the freshly built binary with the example configuration on a
private server. Your installed tmux and its sessions are not touched:

```sh
./try-theme.sh
```

Run it from a plain terminal, not inside another tmux. Quit with
`Ctrl+b d` (detach) or `Ctrl+b :kill-server`.

### 4. Install

Either system-wide, to `/usr/local`, which is usually ahead of the distro
tmux in `PATH`:

```sh
sudo make install
hash -r
tmux -V            # should print: tmux next-3.9
```

Or just for your user, with no sudo:

```sh
./configure --prefix="$HOME/.local" && make -j"$(nproc)" && make install
# make sure ~/.local/bin comes first in PATH
```

**Restart the tmux server after installing.** An already-running server
keeps running the old binary until it is restarted:

```sh
tmux kill-server     # closes your current tmux sessions
tmux
```

### 5. Use the example configuration (optional)

```sh
cp embedded-modern.tmux.conf ~/.tmux.conf
# or keep your own config and add at the end of it:
#   source-file /path/to/etmux/embedded-modern.tmux.conf
```

### Serial port permissions

To open `/dev/ttyUSB*` and `/dev/ttyACM*` without sudo, add yourself to
the `dialout` group (`uucp` on Arch), then log out and back in:

```sh
sudo usermod -aG dialout "$USER"
```

## Quick reference

These keys are from `embedded-modern.tmux.conf`; the prefix is `Ctrl+b`.

| Key | Action |
| --- | --- |
| `Alt+Space` or `Ctrl+b P` | Command palette (serial, files, splits, themes...) |
| `Ctrl+b b` | Show or hide the side panel |
| `Ctrl+b S` | Menu of serial devices |
| `Ctrl+b B` | Change the baud rate |
| `Ctrl+b C-c` | New session, asking for its name |
| `Ctrl+b \|` and `Ctrl+b -` | Split right or down, in the current directory |
| `Ctrl+o` or `Alt+o` in copy mode | Open the `file:line` under the cursor |
| Mouse | Click sessions, windows, devices and files in the panel, and the baud and panel buttons on the bottom bar |

Commands, which you can type at `Ctrl+b :`:

```
new-serial [-h|-v|-w] [-B baud] DEVICE   open a serial console
serial-set -b 921600                     change baud (also -p -f -D -s)
serial-break / serial-reconnect          send a break / reopen the device
boot-report [-N 10]                      boot stage timings
choose-serial                            menu of serial devices
open-file PATH                           open a file next to the terminal
source-theme dark|light|auto|classic     switch theme
toggle-side-status                       slide the side panel in or out
```

Useful options:

```tmux
set -g serial-baud 921600                     # default baud for new panes
set -g serial-log-dir ~/serial-logs           # one log per boot
set -g autoboot-interrupt " "                 # stop U-Boot autoboot
set -g output-match[0] "panic:Kernel panic"   # alert on a pattern
set -g editor "code -w"                       # editor for open-file
set -g open-location-rewrite[0] "^/workdir/=>/home/me/yocto/"
set -g file-open-width 50                     # width of the file column
set -g side-status right                      # panel on the right
```

The full documentation is in the man page: `man ./tmux.1` before
installing, `man tmux` after. Look for the **SERIAL CONSOLES** section and
the options above. [DESIGN.md](DESIGN.md) explains how each feature works
inside tmux.

## Tests

```sh
cd regress
sh serial-pane.sh          # serial panes over a socat pty pair
sh output-match-boot.sh    # triggers and boot detection (replays boot logs)
sh open-location.sh        # file:line extraction for each tool
sh source-theme.sh         # themes
sh file-panel.sh           # side panel files
sh example-config.sh       # the example configuration
make                       # the whole upstream suite
```

## Status

etmux is a work in progress on the `embedded-serial` branch. Not done yet:
- gaps and padding between panes
- instant hot-plug reconnect via libudev (it polls every 500 ms today)
- non-standard baud rates
- target, SSH, ADB and build-progress integration

## Licence

Same as tmux: ISC. See [COPYING](COPYING). etmux is based on
[tmux](https://github.com/tmux/tmux) by Nicholas Marriott and contributors;
the original tmux README is in [README.tmux.md](README.tmux.md).
