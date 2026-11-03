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
#include <termios.h>

#include "tmux.h"

/*
 * Commands acting on a serial pane: send a break, change line settings,
 * reconnect.
 */

static enum cmd_retval	cmd_serial_break_exec(struct cmd *,
			    struct cmdq_item *);
static enum cmd_retval	cmd_serial_set_exec(struct cmd *, struct cmdq_item *);
static enum cmd_retval	cmd_serial_reconnect_exec(struct cmd *,
			    struct cmdq_item *);

const struct cmd_entry cmd_serial_break_entry = {
	.name = "serial-break",
	.description = "Send a break on a serial pane's line.",
	.alias = NULL,

	.args = { "t:", 0, 0, NULL },
	.usage = CMD_TARGET_PANE_USAGE,

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_serial_break_exec
};

const struct cmd_entry cmd_serial_set_entry = {
	.name = "serial-set",
	.description = "Change a serial pane's line settings.",
	.alias = NULL,

	.args = { "b:D:f:p:s:t:", 0, 0, NULL },
	.usage = "[-b baud-rate] [-D data-bits] [-f none|rtscts|xonxoff] "
		 "[-p none|even|odd] [-s stop-bits] " CMD_TARGET_PANE_USAGE,

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_serial_set_exec
};

const struct cmd_entry cmd_serial_reconnect_entry = {
	.name = "serial-reconnect",
	.description = "Close and reopen a serial pane's device.",
	.alias = NULL,

	.args = { "t:", 0, 0, NULL },
	.usage = CMD_TARGET_PANE_USAGE,

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_serial_reconnect_exec
};

static struct window_pane_serial *
cmd_serial_get(struct cmdq_item *item)
{
	struct window_pane	*wp = cmdq_get_target(item)->wp;

	if (wp->serial == NULL) {
		cmdq_error(item, "pane %%%u is not a serial pane", wp->id);
		return (NULL);
	}
	return (wp->serial);
}

static enum cmd_retval
cmd_serial_break_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	struct window_pane		*wp = cmdq_get_target(item)->wp;
	struct window_pane_serial	*ser;

	if ((ser = cmd_serial_get(item)) == NULL)
		return (CMD_RETURN_ERROR);
	if (ser->state != SERIAL_CONNECTED || wp->fd == -1) {
		cmdq_error(item, "%s is disconnected", ser->device);
		return (CMD_RETURN_ERROR);
	}
	if (tcsendbreak(wp->fd, 0) != 0) {
		cmdq_error(item, "%s: %s", ser->device, strerror(errno));
		return (CMD_RETURN_ERROR);
	}
	return (CMD_RETURN_NORMAL);
}

static int
cmd_serial_choice(struct cmdq_item *item, const char *option,
    const char *value, int *out)
{
	const struct options_table_entry	*oe;
	char					*cause = NULL;
	int					 n;

	oe = options_search(option);
	n = options_find_choice(oe, value, &cause);
	if (n == -1) {
		cmdq_error(item, "%s %s", option, cause);
		free(cause);
		return (-1);
	}
	*out = n;
	return (0);
}

static enum cmd_retval
cmd_serial_set_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args			*args = cmd_get_args(self);
	struct window_pane		*wp = cmdq_get_target(item)->wp;
	struct window_pane_serial	*ser, old;
	char				*cause = NULL, *title;
	const char			*name;
	long long			 n;
	int				 choice;

	if ((ser = cmd_serial_get(item)) == NULL)
		return (CMD_RETURN_ERROR);
	memcpy(&old, ser, sizeof old);

	if (args_has(args, 'b')) {
		n = args_strtonum(args, 'b', 50, 16000000, &cause);
		if (cause != NULL || serial_pane_speed(n, NULL) != 0) {
			cmdq_error(item, "unsupported baud rate: %s",
			    args_get(args, 'b'));
			free(cause);
			return (CMD_RETURN_ERROR);
		}
		ser->baud = n;
	}
	if (args_has(args, 'D')) {
		n = args_strtonum(args, 'D', 5, 8, &cause);
		if (cause != NULL) {
			cmdq_error(item, "data bits %s", cause);
			free(cause);
			goto fail;
		}
		ser->data_bits = n;
	}
	if (args_has(args, 's')) {
		n = args_strtonum(args, 's', 1, 2, &cause);
		if (cause != NULL) {
			cmdq_error(item, "stop bits %s", cause);
			free(cause);
			goto fail;
		}
		ser->stop_bits = n;
	}
	if (args_has(args, 'f')) {
		if (cmd_serial_choice(item, "serial-flow", args_get(args, 'f'),
		    &choice) != 0)
			goto fail;
		ser->flow = choice;
	}
	if (args_has(args, 'p')) {
		if (cmd_serial_choice(item, "serial-parity",
		    args_get(args, 'p'), &choice) != 0)
			goto fail;
		ser->parity = choice;
	}

	if (serial_pane_apply(wp, &cause) != 0) {
		cmdq_error(item, "%s", cause);
		free(cause);
		goto fail;
	}

	name = strrchr(ser->device, '/');
	name = (name != NULL) ? name + 1 : ser->device;
	xasprintf(&title, "%s @ %u", name, ser->baud);
	screen_set_title(&wp->base, title, 0);
	free(title);
	server_redraw_window_borders(wp->window);
	server_status_window(wp->window);
	return (CMD_RETURN_NORMAL);

fail:
	ser->baud = old.baud;
	ser->data_bits = old.data_bits;
	ser->stop_bits = old.stop_bits;
	ser->flow = old.flow;
	ser->parity = old.parity;
	return (CMD_RETURN_ERROR);
}

static enum cmd_retval
cmd_serial_reconnect_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	struct window_pane	*wp = cmdq_get_target(item)->wp;
	char			*cause = NULL;

	if (cmd_serial_get(item) == NULL)
		return (CMD_RETURN_ERROR);
	if (serial_pane_reconnect(wp, &cause) != 0) {
		cmdq_error(item, "%s", cause);
		free(cause);
		/* Keep trying in the background if enabled. */
		serial_pane_disconnected(wp);
		return (CMD_RETURN_ERROR);
	}
	return (CMD_RETURN_NORMAL);
}
