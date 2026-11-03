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
	.description = "Open a file in a new pane with the editor.",
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

/* Markup for the file_panel format. */
char *
file_panel(struct window_pane *wp, struct session *s)
{
	struct options		*oo;
	struct file_panel_entry	*list;
	const char		*style, *dir_style, *code_style;
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
		if (list[i].dir) {
			xasprintf(&item, "   #[%s]%s/#[default]#[nl]",
			    dir_style, name);
		} else if (file_panel_openable(list[i].name, oo)) {
			xasprintf(&item, "#[range=user|f%u]   #[%s]%s"
			    "#[default]#[norange]#[nl]", i, code_style, name);
		} else {
			xasprintf(&item, "   #[%s]%s#[default]#[nl]", style,
			    name);
		}
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

/* Replace %1 (path), %2 (directory) and %3 (editor), already escaped. */
static char *
file_panel_template(const char *template, const char *path, const char *dir,
    const char *editor)
{
	const char	*s, *with;
	char		*buf = NULL;
	size_t		 len = 0, add;

	for (s = template; *s != '\0'; s++) {
		with = NULL;
		if (s[0] == '%' && s[1] == '1')
			with = path;
		else if (s[0] == '%' && s[1] == '2')
			with = dir;
		else if (s[0] == '%' && s[1] == '3')
			with = editor;
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

static enum cmd_retval
cmd_open_file_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct session		*s = target->s;
	struct file_panel_entry	*list;
	char			*dir, *path = NULL, *cmd, *error, *value;
	const char		*errstr;
	char			*epath, *edir, *editor, *slash;
	u_int			 n, idx;

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
		if (idx >= n || list[idx].dir) {
			cmdq_error(item, "no file %u in %s", idx, dir);
			file_panel_free(list, n);
			free(dir);
			return (CMD_RETURN_ERROR);
		}
		xasprintf(&path, "%s/%s", dir, list[idx].name);
		file_panel_free(list, n);
	} else if (args_count(args) == 1) {
		path = format_single_from_target(item, args_string(args, 0));
		if (*path != '/' &&
		    (dir = file_panel_dir(target->wp, s)) != NULL) {
			xasprintf(&cmd, "%s/%s", dir, path);
			free(dir);
			free(path);
			path = cmd;
		}
		dir = xstrdup(path);
		if ((slash = strrchr(dir, '/')) != NULL && slash != dir)
			*slash = '\0';
	} else {
		cmdq_error(item, "no file given");
		return (CMD_RETURN_ERROR);
	}

	epath = args_escape(path);
	edir = args_escape(dir);
	editor = file_panel_editor();
	cmd = file_panel_template(options_get_string(s->options,
	    "file-open-command"), epath, edir, editor);
	free(epath);
	free(edir);
	free(editor);
	free(path);
	free(dir);

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
