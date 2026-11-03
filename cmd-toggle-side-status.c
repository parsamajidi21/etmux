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

#include "tmux.h"

/*
 * Show or hide the side status line, sliding it in or out by stepping
 * side-status-width (unless -n is given or side-status-animation is off).
 */

#define TOGGLE_SIDE_STEPS	8
#define TOGGLE_SIDE_INTERVAL	16	/* milliseconds */

static enum cmd_retval	cmd_toggle_side_status_exec(struct cmd *,
			    struct cmdq_item *);

const struct cmd_entry cmd_toggle_side_status_entry = {
	.name = "toggle-side-status",
	.description = "Show or hide the side status line.",
	.alias = NULL,

	.args = { "nt:", 0, 0, NULL },
	.usage = "[-n] " CMD_TARGET_SESSION_USAGE,

	.target = { 't', CMD_FIND_SESSION, CMD_FIND_CANFAIL },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_toggle_side_status_exec
};

struct toggle_side_status {
	struct event	 timer;
	struct options	*oo;
	struct session	*s;

	int		 show;
	int		 side;		/* side to show on (1 left, 2 right) */
	u_int		 width;		/* full width */
	u_int		 step;
};

/* Only one animation at a time. */
static struct toggle_side_status *toggle_side_status_running;

/* Side used when showing again; remembered across hides. */
static int toggle_side_status_last_side = 2;

static void
toggle_side_status_set(struct options *oo, const char *name, long long n)
{
	options_set_number(oo, name, n);
	options_push_changes(name);
}

static void
toggle_side_status_done(struct toggle_side_status *ts)
{
	evtimer_del(&ts->timer);
	session_remove_ref(ts->s, __func__);
	free(ts);
	toggle_side_status_running = NULL;
}

static void
toggle_side_status_timer(__unused int fd, __unused short events, void *arg)
{
	struct toggle_side_status	*ts = arg;
	struct timeval			 tv = { .tv_usec =
					     TOGGLE_SIDE_INTERVAL * 1000 };
	u_int				 w, t, inv;

	if (!session_alive(ts->s)) {
		toggle_side_status_done(ts);
		return;
	}

	ts->step++;
	if (ts->step >= TOGGLE_SIDE_STEPS) {
		if (ts->show)
			toggle_side_status_set(ts->oo, "side-status-width",
			    ts->width);
		else {
			toggle_side_status_set(ts->oo, "side-status", 0);
			toggle_side_status_set(ts->oo, "side-status-width",
			    ts->width);
		}
		toggle_side_status_done(ts);
		return;
	}

	/* Ease out: fast at first, slowing down at the end. */
	t = ts->step;
	inv = TOGGLE_SIDE_STEPS - t;
	w = ts->width - (ts->width * inv * inv) /
	    (TOGGLE_SIDE_STEPS * TOGGLE_SIDE_STEPS);
	if (!ts->show)
		w = ts->width - w;
	if (w < 1)
		w = 1;
	toggle_side_status_set(ts->oo, "side-status-width", w);

	evtimer_add(&ts->timer, &tv);
}

static enum cmd_retval
cmd_toggle_side_status_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args			*args = cmd_get_args(self);
	struct session			*s = cmdq_get_target(item)->s;
	struct options			*oo;
	struct toggle_side_status	*ts;
	struct timeval			 tv = { .tv_usec =
					     TOGGLE_SIDE_INTERVAL * 1000 };
	int				 side;
	u_int				 width;

	if (toggle_side_status_running != NULL)
		return (CMD_RETURN_NORMAL);

	/* Change the options where they are set: the session or global. */
	if (s != NULL && options_get_only(s->options, "side-status") != NULL)
		oo = s->options;
	else
		oo = global_s_options;
	side = options_get_number(oo, "side-status");
	width = options_get_number(oo, "side-status-width");

	if (args_has(args, 'n') ||
	    !options_get_number(global_s_options, "side-status-animation") ||
	    width < 3 || s == NULL) {
		if (side != 0) {
			toggle_side_status_last_side = side;
			toggle_side_status_set(oo, "side-status", 0);
		} else {
			toggle_side_status_set(oo, "side-status",
			    toggle_side_status_last_side);
		}
		return (CMD_RETURN_NORMAL);
	}

	ts = xcalloc(1, sizeof *ts);
	ts->oo = oo;
	ts->s = s;
	session_add_ref(s, __func__);
	ts->width = width;
	if (side != 0) {
		/* Hide: shrink, then turn off. */
		ts->show = 0;
		toggle_side_status_last_side = side;
	} else {
		/* Show: turn on at the narrowest width, then grow. */
		ts->show = 1;
		toggle_side_status_set(oo, "side-status-width", 1);
		toggle_side_status_set(oo, "side-status",
		    toggle_side_status_last_side);
	}
	toggle_side_status_running = ts;

	evtimer_set(&ts->timer, toggle_side_status_timer, ts);
	evtimer_add(&ts->timer, &tv);
	return (CMD_RETURN_NORMAL);
}
