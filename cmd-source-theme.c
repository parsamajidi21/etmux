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

#include "tmux.h"

/*
 * Load a theme: one of the built-in themes, or a file (like source-file).
 *
 * The built-in modern themes only set styles in terms of the semantic
 * theme colours (@bg, @fg, @accent, @muted, @ok, @warn, @error), so the
 * palette options (dark-theme-* and light-theme-*) decide the actual
 * colours and "theme" decides between dark and light. classic sets every
 * option the modern themes touch back to its default.
 */

static enum cmd_retval	cmd_source_theme_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_source_theme_entry = {
	.name = "source-theme",
	.description = "Load a built-in theme or a theme file.",
	.alias = NULL,

	.args = { "l", 0, 1, NULL },
	.usage = "[-l] [dark|light|auto|classic|path]",

	.flags = 0,
	.exec = cmd_source_theme_exec
};

/* Options set by the modern themes, with their values. */
static const struct {
	const char	*name;
	const char	*value;
} cmd_source_theme_modern[] = {
	{ "status-style", "bg=@bg,fg=@fg" },
	{ "status-left", "#[bg=@accent,fg=@bg,bold] #S #[default] " },
	{ "status-left-length", "20" },
	{ "status-right", "#[fg=@muted]#{?pane_serial,#{pane_serial_device} "
	  "#{?#{==:#{pane_serial_state},connected},#[fg=@ok],#[fg=@error]}"
	  "#{pane_serial_state}#[fg=@muted] ,}%H:%M " },
	{ "status-right-length", "60" },
	{ "window-status-style", "fg=@muted" },
	{ "window-status-current-style", "bg=@accent,fg=@bg,bold" },
	{ "window-status-activity-style", "fg=@warn" },
	{ "window-status-bell-style", "fg=@error,bold" },
	{ "window-status-format", " #I #W " },
	{ "window-status-current-format", " #I #W " },
	{ "window-status-separator", "" },
	{ "message-style", "bg=@bg,fg=@accent" },
	{ "message-command-style", "bg=@bg,fg=@warn" },
	{ "mode-style", "bg=@accent,fg=@bg" },
	{ "copy-mode-match-style", "bg=@warn,fg=@bg" },
	{ "copy-mode-current-match-style", "bg=@error,fg=@bg" },
	{ "copy-mode-selection-style", "bg=@accent,fg=@bg" },
	{ "pane-border-lines", "rounded" },
	{ "pane-border-style", "fg=@muted" },
	{ "pane-active-border-style", "fg=@accent" },
	{ "menu-style", "bg=@bg,fg=@fg" },
	{ "menu-selected-style", "bg=@accent,fg=@bg,bold" },
	{ "menu-border-style", "fg=@accent" },
	{ "menu-border-lines", "rounded" },
	{ "side-status-style", "bg=@bg,fg=@fg" },
	{ "side-status-border-style", "fg=@muted" },
	{ "serial-device-style", "fg=@fg" },
	{ "serial-device-active-style", "fg=@ok,bold" },
	{ "window-style", "dim=25" },
	{ "window-active-style", "default" }
};

static enum cmd_retval
cmd_source_theme_set(struct cmdq_item *item, const char *theme)
{
	char		*cmds = NULL, *line, *escaped, *error = NULL;
	const char	*name;
	u_int		 i;
	int		 classic = (strcmp(theme, "classic") == 0);
	enum cmd_parse_status status;

	if (classic)
		xasprintf(&cmds, "set -gu theme\n");
	else
		xasprintf(&cmds, "set -g theme %s\n",
		    strcmp(theme, "auto") == 0 ? "detect" : theme);
	for (i = 0; i < nitems(cmd_source_theme_modern); i++) {
		name = cmd_source_theme_modern[i].name;
		if (classic)
			xasprintf(&line, "%sset -gu %s\n", cmds, name);
		else {
			escaped = args_escape(cmd_source_theme_modern[i].value);
			xasprintf(&line, "%sset -g %s %s\n", cmds, name,
			    escaped);
			free(escaped);
		}
		free(cmds);
		cmds = line;
	}

	status = cmd_parse_and_insert(cmds, NULL, item, cmdq_get_state(item),
	    &error);
	free(cmds);
	if (status == CMD_PARSE_ERROR) {
		cmdq_error(item, "%s", error);
		free(error);
		return (CMD_RETURN_ERROR);
	}
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
cmd_source_theme_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args	*args = cmd_get_args(self);
	const char	*theme;
	char		*cmd, *escaped, *error = NULL;
	enum cmd_parse_status status;

	if (args_has(args, 'l') || args_count(args) == 0) {
		cmdq_print(item, "dark");
		cmdq_print(item, "light");
		cmdq_print(item, "auto");
		cmdq_print(item, "classic");
		return (CMD_RETURN_NORMAL);
	}

	theme = args_string(args, 0);
	if (strcmp(theme, "dark") == 0 ||
	    strcmp(theme, "light") == 0 ||
	    strcmp(theme, "auto") == 0 ||
	    strcmp(theme, "classic") == 0)
		return (cmd_source_theme_set(item, theme));

	/* Anything else is a file. */
	escaped = args_escape(theme);
	xasprintf(&cmd, "source-file -- %s", escaped);
	free(escaped);
	status = cmd_parse_and_insert(cmd, NULL, item, cmdq_get_state(item),
	    &error);
	free(cmd);
	if (status == CMD_PARSE_ERROR) {
		cmdq_error(item, "%s", error);
		free(error);
		return (CMD_RETURN_ERROR);
	}
	return (CMD_RETURN_NORMAL);
}
