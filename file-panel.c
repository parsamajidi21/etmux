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
#include <sys/stat.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

/*
 * A list of the files in a pane's current directory for the side status
 * line (the file_panel format), and the open-file command which opens one
 * of them. Each openable file is a clickable range named "f" and its index
 * in the list; open-file -i lists the directory again to find it, so the
 * range stays within the 15 characters a range name may have.
 */

struct file_panel_entry {
	char	*name;
	int	 dir;
};

static enum cmd_retval	cmd_open_file_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_open_file_entry = {
	.name = "open-file",
	.description = "Open a file or directory in a new window.",
	.alias = NULL,

	.args = { "i:t:", 0, 1, NULL },
	.usage = "[-i index] " CMD_TARGET_PANE_USAGE " [path]",

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = 0,
	.exec = cmd_open_file_exec
};

/* The directory a pane is in: its foreground process's, else the session's. */
char *
file_panel_dir(struct window_pane *wp, struct session *s)
{
	char	*cwd = NULL;

	if (wp != NULL && wp->fd != -1 && wp->serial == NULL)
		cwd = osdep_get_cwd(wp->fd);
	if (cwd != NULL)
		return (xstrdup(cwd));
	if (wp != NULL && wp->cwd != NULL)
		return (xstrdup(wp->cwd));
	if (s != NULL && s->cwd != NULL)
		return (xstrdup(s->cwd));
	return (NULL);
}

/* Does the file have one of the file-panel-extensions? */
static int
file_panel_openable(const char *name, struct options *oo)
{
	const char	*ext, *list;
	char		*copy, *next, *item;
	int		 found = 0;

	ext = strrchr(name, '.');
	if (ext == NULL || ext == name || ext[1] == '\0')
		return (0);
	ext++;

	list = options_get_string(oo, "file-panel-extensions");
	copy = next = xstrdup(list);
	while ((item = strsep(&next, " ,")) != NULL) {
		if (*item == '.')
			item++;
		if (*item != '\0' && strcasecmp(item, ext) == 0) {
			found = 1;
			break;
		}
	}
	free(copy);
	return (found);
}

static int
file_panel_cmp(const void *a, const void *b)
{
	const struct file_panel_entry	*fa = a, *fb = b;

	if (fa->dir != fb->dir)
		return (fb->dir - fa->dir);
	return (strcmp(fa->name, fb->name));
}

/* List a directory: no hidden files, directories first, then by name. */
static u_int
file_panel_list(const char *dir, struct file_panel_entry **out)
{
	struct file_panel_entry	*list = NULL;
	DIR			*d;
	struct dirent		*de;
	struct stat		 sb;
	char			*path;
	u_int			 n = 0;

	*out = NULL;
	if ((d = opendir(dir)) == NULL)
		return (0);
	while ((de = readdir(d)) != NULL) {
		if (*de->d_name == '.')
			continue;
		list = xreallocarray(list, n + 1, sizeof *list);
		list[n].name = xstrdup(de->d_name);
		xasprintf(&path, "%s/%s", dir, de->d_name);
		list[n].dir = (stat(path, &sb) == 0 && S_ISDIR(sb.st_mode));
		free(path);
		n++;
	}
	closedir(d);
	if (n != 0)
		qsort(list, n, sizeof *list, file_panel_cmp);
	*out = list;
	return (n);
}

static void
file_panel_free(struct file_panel_entry *list, u_int n)
{
	u_int	i;

	for (i = 0; i < n; i++)
		free(list[i].name);
	free(list);
}

static void
file_panel_append(char **buf, size_t *len, const char *add)
{
	size_t	n = strlen(add);

	*buf = xrealloc(*buf, *len + n + 1);
	memcpy(*buf + *len, add, n + 1);
	*len += n;
}

/* Escape # so a name is not taken as a format. */
static char *
file_panel_escape(const char *s)
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

/* Markup for the file_panel format: every entry is a clickable range. */
char *
file_panel(struct window_pane *wp, struct session *s)
{
	struct options		*oo;
	struct file_panel_entry	*list;
	const char		*style, *dir_style, *code_style, *st;
	char			*dir, *buf = NULL, *item, *name;
	size_t			 len = 0;
	u_int			 n, i, limit;

	if (s == NULL)
		return (NULL);
	oo = s->options;
	if ((dir = file_panel_dir(wp, s)) == NULL)
		return (NULL);

	style = options_get_string(oo, "file-panel-style");
	dir_style = options_get_string(oo, "file-panel-dir-style");
	code_style = options_get_string(oo, "file-panel-code-style");
	limit = options_get_number(oo, "file-panel-limit");

	n = file_panel_list(dir, &list);
	free(dir);
	file_panel_append(&buf, &len, "");
	for (i = 0; i < n && i < limit; i++) {
		name = file_panel_escape(list[i].name);
		if (list[i].dir)
			st = dir_style;
		else if (file_panel_openable(list[i].name, oo))
			st = code_style;
		else
			st = style;
		xasprintf(&item, "#[range=user|f%u]   #[%s]%s%s#[default]"
		    "#[norange]#[nl]", i, st, name, list[i].dir ? "/" : "");
		file_panel_append(&buf, &len, item);
		free(item);
		free(name);
	}
	if (n > limit) {
		xasprintf(&item, "   #[%s dim]%u more#[default]#[nl]", style,
		    n - limit);
		file_panel_append(&buf, &len, item);
		free(item);
	}
	file_panel_free(list, n);
	return (buf);
}

/*
 * Is the file binary? Look at the first 8 KiB: a NUL (as git decides), or
 * more than 1 in 20 bytes being control characters other than the usual
 * text ones (tab, newline, carriage return, form feed, backspace, escape)
 * or not part of valid UTF-8.
 */
static int
file_panel_binary(const char *path)
{
	u_char	 buf[8192];
	ssize_t	 n, i, j, bad = 0, more;
	int	 fd;

	if ((fd = open(path, O_RDONLY|O_NONBLOCK)) == -1)
		return (0);
	n = read(fd, buf, sizeof buf);
	close(fd);
	if (n <= 0)
		return (0);
	for (i = 0; i < n; i++) {
		if (buf[i] == '\0')
			return (1);
		if (buf[i] < 0x80) {
			if ((buf[i] < 0x20 &&
			    strchr("\t\n\r\f\b\033", buf[i]) == NULL) ||
			    buf[i] == 0x7f)
				bad++;
			continue;
		}
		if (buf[i] >= 0xc2 && buf[i] <= 0xdf)
			more = 1;
		else if (buf[i] >= 0xe0 && buf[i] <= 0xef)
			more = 2;
		else if (buf[i] >= 0xf0 && buf[i] <= 0xf4)
			more = 3;
		else {
			bad++;
			continue;
		}
		for (j = 1; j <= more && i + j < n; j++) {
			if ((buf[i + j] & 0xc0) != 0x80)
				break;
		}
		if (j <= more && i + j < n)
			bad++;
		else
			i += more;
	}
	return (bad * 20 > n);
}

/* Quote for sh(1): 'a'\''b'. */
static char *
file_panel_shell_quote(const char *s)
{
	char	*out, *cp;

	out = cp = xmalloc(strlen(s) * 4 + 3);
	*cp++ = '\'';
	for (; *s != '\0'; s++) {
		if (*s == '\'') {
			memcpy(cp, "'\\''", 4);
			cp += 4;
		} else
			*cp++ = *s;
	}
	*cp++ = '\'';
	*cp = '\0';
	return (out);
}

/* Replace %X in a template with the given strings. */
static char *
file_panel_template(const char *template, const char *keys,
    const char **values)
{
	const char	*s, *with, *k;
	char		*buf = NULL;
	size_t		 len = 0, add;

	for (s = template; *s != '\0'; s++) {
		with = NULL;
		if (s[0] == '%' && s[1] != '\0' &&
		    (k = strchr(keys, s[1])) != NULL)
			with = values[k - keys];
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

/* Each word of the editor command escaped as its own argument. */
static char *
file_panel_editor(void)
{
	char	*copy, *next, *word, *escaped, *out = xstrdup(""), *new;

	copy = next = xstrdup(options_get_string(global_options, "editor"));
	while ((word = strsep(&next, " ")) != NULL) {
		if (*word == '\0')
			continue;
		escaped = args_escape(word);
		xasprintf(&new, "%s%s%s", out, *out == '\0' ? "" : " ",
		    escaped);
		free(escaped);
		free(out);
		out = new;
	}
	free(copy);
	return (out);
}

/* The command to open a path: editor, viewer, or a shell for directories. */
static char *
file_panel_command(struct session *s, const char *path, int is_dir)
{
	const char	*keys = "1";
	const char	*values[1];
	char		*quoted, *shell, *escaped, *editor, *cmd;

	if (is_dir)
		return (xstrdup(""));
	if (file_panel_binary(path)) {
		quoted = file_panel_shell_quote(path);
		values[0] = quoted;
		shell = file_panel_template(options_get_string(s->options,
		    "file-view-command"), keys, values);
		free(quoted);
		cmd = args_escape(shell);
		free(shell);
		return (cmd);
	}
	editor = file_panel_editor();
	escaped = args_escape(path);
	xasprintf(&cmd, "%s %s", editor, escaped);
	free(editor);
	free(escaped);
	return (cmd);
}

/* Find a window in the session already showing this path. */
static struct winlink *
file_panel_find(struct session *s, const char *path)
{
	struct winlink		*wl;
	struct options_entry	*o;

	RB_FOREACH(wl, winlinks, &s->windows) {
		o = options_get_only(wl->window->options, "@open-file");
		if (o == NULL)
			continue;
		if (strcmp(options_get_string(wl->window->options,
		    "@open-file"), path) == 0)
			return (wl);
	}
	return (NULL);
}

static enum cmd_retval
cmd_open_file_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct session		*s = target->s;
	struct file_panel_entry	*list;
	struct winlink		*wl;
	struct stat		 sb;
	const char		*errstr, *name;
	const char		*values[4];
	char			*dir, *path = NULL, *cmd, *error, *value;
	char			*epath, *edir, *ename, *command, *slash, *label;
	u_int			 n, idx;
	int			 is_dir;

	if (args_has(args, 'i')) {
		/* Expanded, so a binding can pass #{mouse_status_range}. */
		value = format_single_from_target(item, args_get(args, 'i'));
		idx = strtonum(value, 0, UINT_MAX, &errstr);
		if (errstr != NULL) {
			cmdq_error(item, "index %s: %s", value, errstr);
			free(value);
			return (CMD_RETURN_ERROR);
		}
		free(value);
		if ((dir = file_panel_dir(target->wp, s)) == NULL) {
			cmdq_error(item, "no directory");
			return (CMD_RETURN_ERROR);
		}
		n = file_panel_list(dir, &list);
		if (idx >= n) {
			cmdq_error(item, "no file %u in %s", idx, dir);
			file_panel_free(list, n);
			free(dir);
			return (CMD_RETURN_ERROR);
		}
		xasprintf(&path, "%s/%s", dir, list[idx].name);
		file_panel_free(list, n);
		free(dir);
	} else if (args_count(args) == 1) {
		path = format_single_from_target(item, args_string(args, 0));
		if (*path != '/' &&
		    (dir = file_panel_dir(target->wp, s)) != NULL) {
			xasprintf(&cmd, "%s/%s", dir, path);
			free(dir);
			free(path);
			path = cmd;
		}
	} else {
		cmdq_error(item, "no file given");
		return (CMD_RETURN_ERROR);
	}
	is_dir = (stat(path, &sb) == 0 && S_ISDIR(sb.st_mode));

	/* Already open: go to its window. */
	if ((wl = file_panel_find(s, path)) != NULL) {
		xasprintf(&cmd, "select-window -t @%u", wl->window->id);
		goto run;
	}

	if (is_dir)
		dir = xstrdup(path);
	else {
		dir = xstrdup(path);
		if ((slash = strrchr(dir, '/')) != NULL && slash != dir)
			*slash = '\0';
	}
	name = strrchr(path, '/');
	name = (name != NULL && name[1] != '\0') ? name + 1 : path;
	xasprintf(&label, "%s%s", name, is_dir ? "/" : "");

	epath = args_escape(path);
	edir = args_escape(dir);
	ename = args_escape(label);
	command = file_panel_command(s, path, is_dir);
	values[0] = epath;
	values[1] = edir;
	values[2] = ename;
	values[3] = command;
	cmd = file_panel_template(options_get_string(s->options,
	    "file-open-command"), "fdnc", values);
	free(epath);
	free(edir);
	free(ename);
	free(command);
	free(label);
	free(dir);

run:
	free(path);
	log_debug("%s: %s", __func__, cmd);
	error = NULL;
	if (cmd_parse_and_insert(cmd, NULL, item, cmdq_get_state(item),
	    &error) == CMD_PARSE_ERROR) {
		cmdq_error(item, "%s", error);
		free(error);
		free(cmd);
		return (CMD_RETURN_ERROR);
	}
	free(cmd);
	return (CMD_RETURN_NORMAL);
}
