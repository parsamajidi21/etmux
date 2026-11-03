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

#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Serial (UART) console support:
 *
 * - choose-serial shows a menu of the devices matching the serial-devices
 *   option and runs serial-command for the one chosen;
 * - choose-serial -d device does the same without a menu (used by the
 *   clickable status bar);
 * - choose-serial -B shows a menu to change the serial-baud option;
 * - the serial_device_count and serial_open_count formats count them;
 * - the serial_bar and serial_panel formats list the devices as clickable status line ranges.
 *
 * If a console is already open on a device, it is focused instead of a
 * second one being opened.
 */

/* Longest product name shown. */
#define SERIAL_PRODUCT_MAX 24

struct serial_device {
	char		*path;
	const char	*name;	/* points into path */
	char		*product;
};

static enum cmd_retval	cmd_choose_serial_exec(struct cmd *,
			    struct cmdq_item *);

const struct cmd_entry cmd_choose_serial_entry = {
	.name = "choose-serial",
	.description = "Choose a serial device and open a console on it.",
	.alias = NULL,

	.args = { "b:Bc:d:t:", 0, 1, NULL },
	.usage = "[-B] [-b baud-rate] [-c target-client] [-d device] "
		 CMD_TARGET_PANE_USAGE " [template]",

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK|CMD_CLIENT_CFLAG,
	.exec = cmd_choose_serial_exec
};

/* Look up the USB product name for a tty from sysfs (Linux only). */
static char *
serial_product(const char *name)
{
	char	 path[PATH_MAX], real[PATH_MAX], line[128], *cp;
	FILE	*f;
	int	 i;

	if (snprintf(path, sizeof path, "/sys/class/tty/%s/device",
	    name) >= (int)sizeof path)
		return (NULL);
	if (realpath(path, real) == NULL)
		return (NULL);

	/* Walk up a few levels to find the USB device's product file. */
	for (i = 0; i < 4; i++) {
		if (snprintf(path, sizeof path, "%s/product",
		    real) < (int)sizeof path &&
		    (f = fopen(path, "r")) != NULL) {
			cp = fgets(line, sizeof line, f);
			fclose(f);
			if (cp != NULL) {
				line[strcspn(line, "\r\n")] = '\0';
				line[SERIAL_PRODUCT_MAX] = '\0';
				if (*line != '\0')
					return (xstrdup(line));
			}
		}
		cp = strrchr(real, '/');
		if (cp == NULL || cp == real)
			break;
		*cp = '\0';
	}
	return (NULL);
}

/* Find all devices matching the serial-devices option. */
static u_int
serial_list(struct session *s, struct serial_device **out)
{
	struct serial_device	*list = NULL, *sd;
	u_int			 n = 0;
	char			*patterns, *copy, *pattern, *cp;
	glob_t			 g;
	size_t			 i;

	patterns = copy = xstrdup(options_get_string(s->options,
	    "serial-devices"));
	while ((pattern = strsep(&copy, " \t,")) != NULL) {
		if (*pattern == '\0')
			continue;
		memset(&g, 0, sizeof g);
		if (glob(pattern, 0, NULL, &g) != 0) {
			globfree(&g);
			continue;
		}
		for (i = 0; i < g.gl_pathc; i++) {
			list = xreallocarray(list, n + 1, sizeof *list);
			sd = &list[n++];
			sd->path = xstrdup(g.gl_pathv[i]);
			cp = strrchr(sd->path, '/');
			sd->name = (cp != NULL) ? cp + 1 : sd->path;
			sd->product = serial_product(sd->name);
		}
		globfree(&g);
	}
	free(patterns);

	*out = list;
	return (n);
}

static void
serial_free(struct serial_device *list, u_int n)
{
	u_int	i;

	for (i = 0; i < n; i++) {
		free(list[i].path);
		free(list[i].product);
	}
	free(list);
}

/* Find a live pane whose command line uses this device. */
static struct window_pane *
serial_find_pane(const char *path)
{
	struct window		*w;
	struct window_pane	*wp;
	const char		*found, *end;
	size_t			 len = strlen(path);
	int			 i;

	RB_FOREACH(w, windows, &windows) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (wp->serial != NULL && wp->serial->device != NULL &&
			    strcmp(wp->serial->device, path) == 0)
				return (wp);
			if (wp->fd == -1)
				continue;
			for (i = 0; i < wp->argc; i++) {
				found = wp->argv[i];
				while ((found = strstr(found, path)) != NULL) {
					end = found + len;
					if (*end == '\0' || *end == ' ' ||
					    *end == '\'' || *end == '"')
						return (wp);
					found = end;
				}
			}
		}
	}
	return (NULL);
}

/* Escape # so that text is not treated as a format. */
static char *
serial_escape(const char *s)
{
	char	*out, *cp;

	out = cp = xmalloc(strlen(s) * 2 + 1);
	for (; *s != '\0'; s++) {
		if (*s == '#')
			*cp++ = '#';
		*cp++ = *s;
	}
	*cp = '\0';
	return (out);
}

/* Replace %1 with the path, %2 with the name and %3 with the baud rate. */
static char *
serial_template(const char *template, const char *path, const char *name,
    const char *baud)
{
	const char	*s, *with;
	char		*buf = NULL;
	size_t		 len = 0, add;

	for (s = template; *s != '\0'; s++) {
		with = NULL;
		if (s[0] == '%' && s[1] == '1')
			with = path;
		else if (s[0] == '%' && s[1] == '2')
			with = name;
		else if (s[0] == '%' && s[1] == '3')
			with = baud;
		if (with != NULL) {
			add = strlen(with);
			s++;
		} else
			add = 1;
		buf = xrealloc(buf, len + add + 1);
		memcpy(buf + len, with != NULL ? with : s, add);
		len += add;
	}
	buf = xrealloc(buf, len + 1);
	buf[len] = '\0';
	return (buf);
}

/* Build the command for a device: focus its console or open a new one. */
static char *
serial_command(struct serial_device *sd, const char *template,
    const char *baud)
{
	struct window_pane	*wp;
	char			*cmd;

	wp = serial_find_pane(sd->path);
	if (wp != NULL) {
		xasprintf(&cmd, "switch-client -t %%%u", wp->id);
		return (cmd);
	}
	return (serial_template(template, sd->path, sd->name, baud));
}

/*
 * Status line markup for the serial_bar (horizontal) and serial_panel
 * (vertical, for the side status line) formats: one clickable range per
 * device. The range string is "@" followed by the device name.
 */
static char *
serial_markup(struct session *s, int vertical)
{
	struct serial_device	*list, *sd;
	u_int			 n, i;
	const char		*style, *active, *st, *dot;
	char			*buf = NULL, *product = NULL, *item;
	size_t			 len = 0, add;

	style = options_get_string(s->options, "serial-device-style");
	active = options_get_string(s->options, "serial-device-active-style");

	n = serial_list(s, &list);
	if (n == 0) {
		if (vertical)
			return (xstrdup("#[dim]   no devices#[default]#[nl]"));
		xasprintf(&buf, "#[%s dim] no UART devices #[default]", style);
		return (buf);
	}
	for (i = 0; i < n; i++) {
		sd = &list[i];
		if (serial_find_pane(sd->path) != NULL) {
			st = active;
			dot = "●";
		} else {
			st = style;
			dot = "○";
		}
		if (sd->product != NULL)
			product = serial_escape(sd->product);

		if (vertical) {
			/* Name on one line, product dimmed underneath. */
			if (product != NULL) {
				xasprintf(&item, "#[range=user|@%.14s]   #[%s]%s "
				    "%s#[default]#[norange]#[nl]"
				    "#[range=user|@%.14s]#[dim]     %s"
				    "#[default]#[norange]#[nl]",
				    sd->name, st, dot, sd->name, sd->name,
				    product);
			} else {
				xasprintf(&item, "#[range=user|@%.14s]   #[%s]%s "
				    "%s#[default]#[norange]#[nl]", sd->name, st,
				    dot, sd->name);
			}
		} else if (product != NULL) {
			xasprintf(&item, "%s#[range=user|@%.14s]#[%s] %s %s "
			    "#[dim]%s #[default]#[norange]", i == 0 ? "" : " ",
			    sd->name, st, dot, sd->name, product);
		} else {
			xasprintf(&item, "%s#[range=user|@%.14s]#[%s] %s %s "
			    "#[default]#[norange]", i == 0 ? "" : " ",
			    sd->name, st, dot, sd->name);
		}
		free(product);
		product = NULL;

		add = strlen(item);
		buf = xrealloc(buf, len + add + 1);
		memcpy(buf + len, item, add + 1);
		len += add;
		free(item);
	}
	serial_free(list, n);
	return (buf);
}

char *
serial_bar(struct session *s)
{
	return (serial_markup(s, 0));
}

char *
serial_panel(struct session *s)
{
	return (serial_markup(s, 1));
}

/* Centre a menu on the window and show it. */
static void
serial_show_menu(struct cmdq_item *item, struct menu *menu,
    struct client *tc, struct cmd_find_state *target, int choice)
{
	struct key_event	*event = cmdq_get_event(item);
	struct window		*w = target->w;
	enum box_lines		 lines;
	u_int			 sx, sy, px, py;
	int			 flags = 0;

	/*
	 * Opened by a mouse click (on the status line): pop up from the
	 * bottom-right corner, just above the bar, and accept mouse clicks.
	 * Otherwise centre it and use the keyboard.
	 */
	if (event->m.valid)
		w = tc->session->curw->window;
	lines = options_get_number(w->options, "menu-border-lines");
	menu_get_size(menu, lines, &sx, &sy);
	if (event->m.valid) {
		px = (w->sx > sx) ? w->sx - sx : 0;
		py = (w->sy > sy) ? w->sy - sy : 0;
	} else {
		px = (w->sx > sx) ? (w->sx - sx) / 2 : 0;
		py = (w->sy > sy) ? (w->sy - sy) / 2 : 0;
		flags |= MENU_NOMOUSE;
	}
	if (menu_display(menu, flags, choice, item, px, py, tc, lines,
	    NULL, NULL, NULL, target, NULL, NULL) != 0)
		menu_free(menu);
}

/* Count devices, and how many of them have an open console. */
u_int
serial_count(struct session *s, u_int *open)
{
	struct serial_device	*list;
	u_int			 n, i;

	n = serial_list(s, &list);
	if (open != NULL) {
		*open = 0;
		for (i = 0; i < n; i++) {
			if (serial_find_pane(list[i].path) != NULL)
				(*open)++;
		}
	}
	serial_free(list, n);
	return (n);
}

/* Menu of common baud rates. */
static void
serial_baud_menu(struct cmdq_item *item, struct client *tc,
    struct cmd_find_state *target, long long current)
{
	static const long long	 rates[] = { 9600, 19200, 38400, 57600,
				     115200, 230400, 460800, 921600 };
	struct menu		*menu;
	struct menu_item	 mi;
	char			*label, *command, key[2];
	u_int			 i, choice = 0;

	menu = menu_create("#[align=centre]Baud rate");
	for (i = 0; i < nitems(rates); i++) {
		if (rates[i] == current)
			choice = i;
		xasprintf(&label, "%s %lld", rates[i] == current ? "●" : " ",
		    rates[i]);
		xasprintf(&command, "set -g serial-baud %lld", rates[i]);
		key[0] = '1' + i;
		key[1] = '\0';
		mi.name = label;
		mi.command = command;
		mi.key = key_string_lookup_string(key);
		menu_add_item(menu, &mi, item, tc, target);
		free(label);
		free(command);
	}
	menu_add_item(menu, NULL, item, tc, target);
	mi.name = "  Custom...";
	mi.command = "command-prompt -p 'Baud rate:' "
	    "{ set -g serial-baud '%%' }";
	mi.key = key_string_lookup_string("c");
	menu_add_item(menu, &mi, item, tc, target);
	serial_show_menu(item, menu, tc, target, choice);
}

/* Run a command string after the current item. */
static enum cmd_retval
serial_run(struct cmdq_item *item, const char *command)
{
	char	*expanded, *error = NULL;

	expanded = format_single_from_target(item, command);
	if (cmd_parse_and_insert(expanded, NULL, item, cmdq_get_state(item),
	    &error) == CMD_PARSE_ERROR) {
		cmdq_error(item, "%s", error);
		free(error);
		free(expanded);
		return (CMD_RETURN_ERROR);
	}
	free(expanded);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
cmd_choose_serial_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct client		*tc = cmdq_get_target_client(item);
	struct session		*s = target->s;
	struct serial_device	*list, *sd;
	struct menu		*menu;
	struct menu_item	 mi;
	const char		*template;
	char			*label, *product, *command, *want, key[2];
	char			 baud[32];
	u_int			 n, i;
	size_t			 wantlen;
	enum cmd_retval		 retval;

	if (args_count(args) != 0)
		template = args_string(args, 0);
	else
		template = options_get_string(s->options, "serial-command");
	if (args_has(args, 'b'))
		strlcpy(baud, args_get(args, 'b'), sizeof baud);
	else {
		snprintf(baud, sizeof baud, "%lld",
		    options_get_number(s->options, "serial-baud"));
	}
	if (baud[strspn(baud, "0123456789")] != '\0' || *baud == '\0') {
		cmdq_error(item, "invalid baud rate: %s", baud);
		return (CMD_RETURN_ERROR);
	}

	if (args_has(args, 'B')) {
		if (tc == NULL || tc->session == NULL ||
		    (tc->flags & CLIENT_CONTROL)) {
			cmdq_error(item, "no client to display menu on");
			return (CMD_RETURN_ERROR);
		}
		serial_baud_menu(item, tc, target,
		    options_get_number(s->options, "serial-baud"));
		return (CMD_RETURN_NORMAL);
	}

	n = serial_list(s, &list);

	/* -d: open (or focus) the given device directly. */
	if (args_has(args, 'd')) {
		want = format_single_from_target(item, args_get(args, 'd'));
		wantlen = strlen(want);
		sd = NULL;
		for (i = 0; i < n; i++) {
			if (strcmp(want, list[i].path) == 0 ||
			    strcmp(want, list[i].name) == 0 ||
			    (wantlen == 14 &&
			    strncmp(want, list[i].name, 14) == 0)) {
				sd = &list[i];
				break;
			}
		}
		if (sd == NULL) {
			cmdq_error(item, "serial device not found: %s", want);
			free(want);
			serial_free(list, n);
			return (CMD_RETURN_ERROR);
		}
		free(want);
		command = serial_command(sd, template, baud);
		retval = serial_run(item, command);
		free(command);
		serial_free(list, n);
		return (retval);
	}

	if (tc == NULL || tc->session == NULL ||
	    (tc->flags & CLIENT_CONTROL)) {
		serial_free(list, n);
		cmdq_error(item, "no client to display menu on");
		return (CMD_RETURN_ERROR);
	}
	if (n == 0) {
		status_message_set(tc, -1, 1, 0, 0,
		    "No serial devices found (serial-devices is \"%s\")",
		    options_get_string(s->options, "serial-devices"));
		return (CMD_RETURN_NORMAL);
	}

	xasprintf(&label, "#[align=centre]Serial devices @ %s baud", baud);
	menu = menu_create(label);
	free(label);
	for (i = 0; i < n; i++) {
		sd = &list[i];
		if (sd->product != NULL) {
			product = serial_escape(sd->product);
			xasprintf(&label, "%s %-10s %s",
			    serial_find_pane(sd->path) ? "●" : "○", sd->name,
			    product);
			free(product);
		} else {
			xasprintf(&label, "%s %s",
			    serial_find_pane(sd->path) ? "●" : "○", sd->name);
		}
		command = serial_command(sd, template, baud);

		mi.name = label;
		mi.command = command;
		if (i < 10) {
			key[0] = (i == 9) ? '0' : '1' + i;
			key[1] = '\0';
			mi.key = key_string_lookup_string(key);
		} else
			mi.key = KEYC_NONE;
		menu_add_item(menu, &mi, item, tc, target);

		free(label);
		free(command);
	}
	serial_free(list, n);

	serial_show_menu(item, menu, tc, target, 0);
	return (CMD_RETURN_NORMAL);
}
