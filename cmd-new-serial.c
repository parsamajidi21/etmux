/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Parsa Majidi <majidii.parsa@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Open a serial device in a new pane (split from the target pane, or in a
 * new window with -w).
 */

#define NEW_SERIAL_TEMPLATE "#{session_name}:#{window_index}.#{pane_index}"

static enum cmd_retval	cmd_new_serial_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_new_serial_entry = {
	.name = "new-serial",
	.description = "Open a serial device in a new pane.",
	.alias = "serial",

	.args = { "bB:dfF:hl:n:Pt:vw", 1, 1, NULL },
	.usage = "[-bdfhPvw] [-B baud-rate] [-F format] [-l size] "
		 "[-n window-name] " CMD_TARGET_PANE_USAGE " device",

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = 0,
	.exec = cmd_new_serial_exec
};

static enum cmd_retval
cmd_new_serial_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args			*args = cmd_get_args(self);
	struct cmd_find_state		*current = cmdq_get_current(item);
	struct cmd_find_state		*target = cmdq_get_target(item);
	struct client			*tc = cmdq_get_target_client(item);
	struct session			*s = target->s;
	struct winlink			*wl = target->wl, *new_wl = NULL;
	struct window			*w = wl->window;
	struct window_pane		*wp = target->wp, *new_wp;
	struct spawn_context		 sc = { 0 };
	struct window_pane_serial	*ser;
	struct cmd_find_state		 fs;
	const char			*template, *name;
	char				*cause = NULL, *cp, *title;
	long long			 baud;
	int				 fd, flags = 0;

	ser = serial_pane_create(args_string(args, 0), s->options);
	if (args_has(args, 'B')) {
		baud = args_strtonum(args, 'B', 50, 16000000, &cause);
		if (cause != NULL) {
			cmdq_error(item, "baud rate %s", cause);
			goto fail;
		}
		ser->baud = baud;
	}
	if (serial_pane_speed(ser->baud, NULL) != 0) {
		cmdq_error(item, "unsupported baud rate: %u", ser->baud);
		goto fail;
	}

	/* Open first so a bad device does not leave an empty pane. */
	if ((fd = serial_pane_open(ser, &cause)) == -1) {
		cmdq_error(item, "%s", cause);
		goto fail;
	}

	if (args_has(args, 'd'))
		flags |= SPAWN_DETACHED;

	sc.item = item;
	sc.s = s;
	sc.tc = tc;
	sc.environ = environ_create();
	sc.idx = -1;
	sc.serial = ser;
	sc.serial_fd = fd;

	/* The device path is the "command", so automatic-rename shows it. */
	sc.argc = 1;
	sc.argv = xcalloc(2, sizeof *sc.argv);
	sc.argv[0] = xstrdup(ser->device);

	if (args_has(args, 'w')) {
		sc.name = args_get(args, 'n');
		sc.flags = flags;
		new_wl = spawn_window(&sc, &cause);
		if (new_wl == NULL)
			goto spawn_fail;
		new_wp = new_wl->window->active;
		wl = new_wl;
		w = new_wl->window;
	} else {
		window_unzoom(w, 1);
		flags |= SPAWN_SPLIT;
		if (args_has(args, 'h'))
			flags |= SPAWN_HORIZONTAL;
		if (args_has(args, 'b'))
			flags |= SPAWN_BEFORE;
		if (args_has(args, 'f'))
			flags |= SPAWN_FULLSIZE;
		sc.lc = layout_get_tiled_cell(item, args, w, wp, flags,
		    &cause);
		if (cause != NULL)
			goto spawn_fail;
		sc.wl = wl;
		sc.wp0 = wp;
		sc.flags = flags;
		if ((new_wp = spawn_pane(&sc, &cause)) == NULL)
			goto spawn_fail;
	}

	name = strrchr(ser->device, '/');
	name = (name != NULL) ? name + 1 : ser->device;
	xasprintf(&title, "%s @ %u", name, ser->baud);
	screen_set_title(&new_wp->base, title, 0);
	free(title);

	if (~flags & SPAWN_DETACHED)
		cmd_find_from_winlink_pane(current, wl, new_wp, 0);
	server_redraw_window(w);
	server_redraw_session(s);

	if (args_has(args, 'P')) {
		if ((template = args_get(args, 'F')) == NULL)
			template = NEW_SERIAL_TEMPLATE;
		cp = format_single(item, template, tc, s, wl, new_wp);
		cmdq_print(item, "%s", cp);
		free(cp);
	}

	cmd_find_from_winlink_pane(&fs, wl, new_wp, 0);
	cmdq_insert_hook(s, item, &fs, "after-new-serial");

	cmd_free_argv(sc.argc, sc.argv);
	environ_free(sc.environ);
	return (CMD_RETURN_NORMAL);

spawn_fail:
	cmdq_error(item, "create pane failed: %s", cause);
	free(cause);
	cmd_free_argv(sc.argc, sc.argv);
	environ_free(sc.environ);
	if (sc.serial != NULL) {
		/* Not taken over by a pane, so still ours to close. */
		close(fd);
		serial_pane_free(sc.serial);
	}
	return (CMD_RETURN_ERROR);

fail:
	free(cause);
	serial_pane_free(ser);
	return (CMD_RETURN_ERROR);
}
