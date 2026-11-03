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
#include <sys/time.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tmux.h"

/*
 * Show how long each boot stage took for the last boots of a pane.
 */

static enum cmd_retval	cmd_boot_report_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_boot_report_entry = {
	.name = "boot-report",
	.description = "Show boot stage durations for a pane.",
	.alias = NULL,

	.args = { "N:t:", 0, 0, NULL },
	.usage = "[-N count] " CMD_TARGET_PANE_USAGE,

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_boot_report_exec
};

static void
cmd_boot_report_append(char **buf, const char *fmt, ...)
{
	va_list	 ap;
	char	*add, *new;

	va_start(ap, fmt);
	xvasprintf(&add, fmt, ap);
	va_end(ap);
	xasprintf(&new, "%s%s", *buf, add);
	free(*buf);
	free(add);
	*buf = new;
}

static enum cmd_retval
cmd_boot_report_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct window_pane	*wp = cmdq_get_target(item)->wp;
	struct window_pane_boot	*wb = wp->boot;
	struct boot_record	*br;
	struct timeval		 d;
	struct tm		 tm;
	char			*line, when[16], *cause = NULL;
	u_int			 count = 5, n, i, j;

	if (args_has(args, 'N')) {
		count = args_strtonum(args, 'N', 1, BOOT_HISTORY, &cause);
		if (cause != NULL) {
			cmdq_error(item, "count %s", cause);
			free(cause);
			return (CMD_RETURN_ERROR);
		}
	}
	if (wb == NULL || wb->count == 0) {
		cmdq_error(item, "no boots seen in pane %%%u", wp->id);
		return (CMD_RETURN_ERROR);
	}

	n = (count < wb->nhistory) ? count : wb->nhistory;
	for (i = 0; i < n; i++) {
		/* Oldest first. */
		br = &wb->history[(wb->current + BOOT_HISTORY - (n - 1 - i)) %
		    BOOT_HISTORY];
		localtime_r(&br->start.tv_sec, &tm);
		strftime(when, sizeof when, "%H:%M:%S", &tm);
		xasprintf(&line, "boot %u (%s):", br->number, when);

		/* Each stage lasts until the next one starts. */
		for (j = 0; j < br->nstages; j++) {
			if (j + 1 == br->nstages) {
				cmd_boot_report_append(&line, " %s",
				    br->stages[j].name);
				break;
			}
			timersub(&br->stages[j + 1].at, &br->stages[j].at, &d);
			cmd_boot_report_append(&line, " %s %ld.%03lds",
			    br->stages[j].name, (long)d.tv_sec,
			    (long)d.tv_usec / 1000);
		}
		if (br->nstages != 0) {
			timersub(&br->stages[br->nstages - 1].at, &br->start,
			    &d);
			cmd_boot_report_append(&line, ", total %ld.%03lds",
			    (long)d.tv_sec, (long)d.tv_usec / 1000);
		}
		cmdq_print(item, "%s", line);
		free(line);
	}
	return (CMD_RETURN_NORMAL);
}
