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

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Find a "file:line[:column]" location in a line of text (compiler, linker,
 * dtc, kernel, ninja/soong, bitbake and Python output), rewrite the path and
 * work out the remote host if the pane is an SSH session. Used by the
 * open-location copy mode command.
 */

#define LOCATION_PATH "[A-Za-z0-9_.+@%~/-]+"

/* Patterns, most specific first. */
static const struct {
	const char	*pattern;
	int		 file;		/* subexpression for the file */
	int		 line;		/* subexpression for the line, 0 = none */
	int		 column;	/* subexpression for the column, 0 = none */
	int		 any_name;	/* accept a file without '/' or '.' */
} location_patterns[] = {
	/* Python traceback: File "x.py", line 12, in f */
	{ "File \"([^\"]+)\", line ([0-9]+)", 1, 2, 0, 1 },

	/* file:line:col, file:line, and dtc's file:line.col */
	{ "(" LOCATION_PATH "):([0-9]+)([:.]([0-9]+))?", 1, 2, 4, 0 },

	/* ld: file.o:(.text+0x10) or file.c:(.text...) */
	{ "(" LOCATION_PATH "):\\(\\.", 1, 0, 0, 0 },

	/* Bare absolute path, e.g. bitbake "Logfile of failure stored in". */
	{ "(/" LOCATION_PATH ")", 1, 0, 0, 0 }
};

/* Names accepted without a '/' or '.' in them. */
static const char *location_names[] = {
	"Makefile", "Kconfig", "Kbuild", "Dockerfile", "Android.bp", NULL
};

static regex_t	location_regex[nitems(location_patterns)];
static int	location_compiled;

static void
location_compile(void)
{
	u_int	i;

	if (location_compiled)
		return;
	for (i = 0; i < nitems(location_patterns); i++) {
		if (regcomp(&location_regex[i], location_patterns[i].pattern,
		    REG_EXTENDED) != 0)
			fatalx("bad location pattern %u", i);
	}
	location_compiled = 1;
}

/* Is this plausibly a file name? */
static int
location_plausible(const char *file, int any_name)
{
	const char	*base, **name;

	if (any_name)
		return (1);
	if (strchr(file, '/') != NULL) {
		/* Not just slashes and dots. */
		return (strspn(file, "/.") != strlen(file));
	}
	base = file;
	for (name = location_names; *name != NULL; name++) {
		if (strcmp(base, *name) == 0)
			return (1);
	}
	/* Needs an extension that is not all digits (e.g. a version). */
	base = strrchr(file, '.');
	if (base == NULL || base == file || base[1] == '\0')
		return (0);
	return (strspn(base + 1, "0123456789") != strlen(base + 1));
}

static char *
location_sub(const char *text, regmatch_t *m, int n)
{
	if (n == 0 || m[n].rm_so == -1)
		return (NULL);
	return (xstrndup(text + m[n].rm_so, m[n].rm_eo - m[n].rm_so));
}

/*
 * Find a location in text. If several are found, prefer the one covering
 * offset cx (the cursor); otherwise use the first. Returns 0 if found.
 */
int
location_find(const char *text, u_int cx, struct location *loc)
{
	regmatch_t	 m[8];
	u_int		 i;
	size_t		 off;
	char		*file, *line, *column;
	int		 found = 0, covers;

	location_compile();
	memset(loc, 0, sizeof *loc);

	for (i = 0; i < nitems(location_patterns); i++) {
		off = 0;
		while (text[off] != '\0' &&
		    regexec(&location_regex[i], text + off, nitems(m), m,
		    off == 0 ? 0 : REG_NOTBOL) == 0) {
			file = location_sub(text + off, m,
			    location_patterns[i].file);
			if (file == NULL ||
			    !location_plausible(file,
			    location_patterns[i].any_name)) {
				free(file);
				off += (m[0].rm_eo > 0) ? m[0].rm_eo : 1;
				continue;
			}
			line = location_sub(text + off, m,
			    location_patterns[i].line);
			column = location_sub(text + off, m,
			    location_patterns[i].column);
			covers = (cx >= off + m[0].rm_so &&
			    cx < off + m[0].rm_eo);

			if (!found || covers) {
				location_free(loc);
				loc->file = file;
				loc->line = (line != NULL) ? atoi(line) : 1;
				loc->column = (column != NULL) ? atoi(column) :
				    0;
				found = 1;
			} else
				free(file);
			free(line);
			free(column);
			if (covers)
				return (0);
			off += (m[0].rm_eo > 0) ? m[0].rm_eo : 1;
		}
		if (found)
			return (0);
	}
	return (found ? 0 : -1);
}

void
location_free(struct location *loc)
{
	free(loc->file);
	loc->file = NULL;
}

/*
 * Apply the open-location-rewrite rules ("regex=>replacement") to a path,
 * in order.
 */
char *
location_rewrite(const char *path, struct options *oo)
{
	struct options_entry		*o;
	struct options_array_item	*a;
	const char			*rule, *arrow;
	char				*pattern, *out, *new;

	out = xstrdup(path);
	o = options_get(oo, "open-location-rewrite");
	if (o == NULL)
		return (out);
	a = options_array_first(o);
	while (a != NULL) {
		rule = options_array_item_value(a)->string;
		arrow = strstr(rule, "=>");
		if (arrow != NULL && arrow != rule) {
			pattern = xstrndup(rule, arrow - rule);
			new = regsub(pattern, arrow + 2, out, REG_EXTENDED);
			free(pattern);
			if (new != NULL) {
				free(out);
				out = new;
			}
		}
		a = options_array_next(a);
	}
	return (out);
}

/* Get the host of the ssh command running in a pane, if any. */
char *
location_ssh_host(struct window_pane *wp)
{
#ifdef __linux__
	static const char	*with_arg = "BbcDEeFIiJLlmOoPpQRSWw";
	char			*path, buf[4096], *host = NULL, *cp, *at;
	FILE			*f;
	size_t			 len, i;
	pid_t			 pgrp;
	int			 skip = 0, first = 1;

	if (wp->fd == -1 || wp->serial != NULL)
		return (NULL);
	if ((pgrp = tcgetpgrp(wp->fd)) == -1)
		return (NULL);
	xasprintf(&path, "/proc/%lld/cmdline", (long long)pgrp);
	f = fopen(path, "r");
	free(path);
	if (f == NULL)
		return (NULL);
	len = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[len] = '\0';

	/* Arguments are NUL separated; the first is the program. */
	for (i = 0; i < len; i += strlen(buf + i) + 1) {
		cp = buf + i;
		if (first) {
			first = 0;
			at = strrchr(cp, '/');
			if (strcmp(at != NULL ? at + 1 : cp, "ssh") != 0)
				return (NULL);
			continue;
		}
		if (skip) {
			skip = 0;
			continue;
		}
		if (*cp == '-') {
			/* Options taking a value, unless attached (-p22). */
			if (cp[1] != '\0' && cp[2] == '\0' &&
			    strchr(with_arg, cp[1]) != NULL)
				skip = 1;
			continue;
		}
		host = xstrdup(cp);
		break;
	}
	return (host);
#else
	(void)wp;
	return (NULL);
#endif
}
