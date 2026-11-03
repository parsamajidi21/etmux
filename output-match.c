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

#include <regex.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Line matching on pane output, and boot detection built on it.
 *
 * Output is fed in as it is read, before parsing. Escape sequences and
 * carriage returns are stripped and complete lines are matched against the
 * output-match patterns (firing pane-output-match) and, if boot detection
 * is on for the pane, the boot-stages patterns.
 *
 * The compiled state is cached per pane and rebuilt when the generation
 * changes (any change to the options involved bumps it). A pane with
 * nothing to match has no state, so the cost on the output path is one
 * integer compare and one pointer test.
 */

/* Bumped when an option used here changes. Starts at 1, panes at 0. */
u_int	output_match_generation = 1;

/* At most this many pane-output-match hooks per pane per second. */
#define OUTPUT_MATCH_RATE 10

struct output_match_pattern {
	char		*name;
	regex_t		 re;
	int		 restart;	/* "!name": seen again = new boot */
};

struct window_pane_match {
	struct output_match_pattern	*patterns;
	u_int				 npatterns;
	struct output_match_pattern	*stages;
	u_int				 nstages;

	int				 has_autoboot;
	regex_t				 autoboot;
	char				*autoboot_keys;

	char				 line[OUTPUT_MATCH_LINE_MAX];
	size_t				 len;
	int				 esc;

	time_t				 rate_second;
	u_int				 rate_count;
	u_int				 rate_dropped;
};

enum {
	OUTPUT_MATCH_ESC_NONE,
	OUTPUT_MATCH_ESC_START,		/* ESC seen */
	OUTPUT_MATCH_ESC_CSI,		/* ESC [ */
	OUTPUT_MATCH_ESC_STRING,	/* ESC ] P ^ _ X: ends at BEL or ST */
	OUTPUT_MATCH_ESC_STRING_ESC	/* ESC inside a string */
};

/* Get boot state, allocating it on first use. */
static struct window_pane_boot *
output_match_get_boot(struct window_pane *wp)
{
	if (wp->boot == NULL) {
		wp->boot = xcalloc(1, sizeof *wp->boot);
		wp->boot->stage = -1;
	}
	return (wp->boot);
}

static void	output_match_autoboot(struct window_pane *, const char *);

static void
output_match_free_patterns(struct output_match_pattern *list, u_int n)
{
	u_int	i;

	for (i = 0; i < n; i++) {
		free(list[i].name);
		regfree(&list[i].re);
	}
	free(list);
}

/* Compile "name:regex" entries of an array option. */
static struct output_match_pattern *
output_match_compile(struct window_pane *wp, const char *option, u_int *n)
{
	struct options_entry		*o;
	struct options_array_item	*a;
	struct output_match_pattern	*list = NULL, *p;
	const char			*value, *sep;

	*n = 0;
	o = options_get(wp->options, option);
	if (o == NULL)
		return (NULL);
	a = options_array_first(o);
	while (a != NULL) {
		value = options_array_item_value(a)->string;
		sep = strchr(value, ':');
		if (sep == NULL || sep == value || sep[1] == '\0') {
			log_debug("%s: %s: bad entry: %s", __func__, option,
			    value);
			goto next;
		}
		list = xreallocarray(list, *n + 1, sizeof *list);
		p = &list[*n];
		if (regcomp(&p->re, sep + 1, REG_EXTENDED|REG_NOSUB) != 0) {
			log_debug("%s: %s: bad regex: %s", __func__, option,
			    sep + 1);
			goto next;
		}
		p->restart = (*value == '!');
		if (p->restart)
			value++;
		p->name = xstrndup(value, sep - value);
		(*n)++;
	next:
		a = options_array_next(a);
	}
	return (list);
}

/* Is boot detection on for this pane? */
static int
output_match_boot_enabled(struct window_pane *wp)
{
	switch (options_get_number(wp->options, "boot-detect")) {
	case 1:
		return (1);
	case 2:
		return (wp->serial != NULL);
	}
	return (0);
}

/* Free a pane's match state. */
void
output_match_free(struct window_pane *wp)
{
	struct window_pane_match	*wm = wp->match;

	if (wm == NULL)
		return;
	output_match_free_patterns(wm->patterns, wm->npatterns);
	output_match_free_patterns(wm->stages, wm->nstages);
	if (wm->has_autoboot)
		regfree(&wm->autoboot);
	free(wm->autoboot_keys);
	free(wm);
	wp->match = NULL;
}

/* Rebuild a pane's match state after an option change. */
void
output_match_refresh(struct window_pane *wp)
{
	struct window_pane_match	*wm;
	const char			*keys, *pattern;

	wp->match_generation = output_match_generation;
	output_match_free(wp);

	wm = xcalloc(1, sizeof *wm);
	wm->patterns = output_match_compile(wp, "output-match",
	    &wm->npatterns);
	if (output_match_boot_enabled(wp)) {
		wm->stages = output_match_compile(wp, "boot-stages",
		    &wm->nstages);
		keys = options_get_string(wp->options, "autoboot-interrupt");
		pattern = options_get_string(wp->options, "autoboot-pattern");
		if (*keys != '\0' && *pattern != '\0' &&
		    regcomp(&wm->autoboot, pattern,
		    REG_EXTENDED|REG_NOSUB) == 0) {
			wm->has_autoboot = 1;
			wm->autoboot_keys = xstrdup(keys);
		}
	}
	if (wm->npatterns == 0 && wm->nstages == 0 && !wm->has_autoboot) {
		free(wm->patterns);
		free(wm->stages);
		free(wm);
		return;
	}
	wp->match = wm;
}

static void
output_match_fire(struct window_pane *wp, const char *event,
    const char *name, const char *line)
{
	struct event_payload	*ep;
	struct cmd_find_state	 fs;

	ep = event_payload_create();
	cmd_find_from_pane(&fs, wp, 0);
	event_payload_set_target(ep, &fs);
	event_payload_set_pane(ep, "pane", wp);
	event_payload_set_window(ep, "window", wp->window);
	if (strcmp(event, "pane-boot-stage") == 0) {
		event_payload_set_string(ep, "boot_stage", "%s", name);
		event_payload_set_uint(ep, "boot_count", wp->boot->count);
	} else
		event_payload_set_string(ep, "match_name", "%s", name);
	event_payload_set_string(ep, "match_line", "%s", line);
	events_fire(event, ep);
}

/* Start a new boot record. */
static void
output_match_new_boot(struct window_pane *wp, struct timeval *now)
{
	struct window_pane_boot	*wb = wp->boot;
	struct boot_record	*br;
	u_int			 i;

	wb->count++;
	wb->start = *now;
	timerclear(&wb->end);
	wb->autoboot_sent = 0;

	wb->current = (wb->current + 1) % BOOT_HISTORY;
	br = &wb->history[wb->current];
	for (i = 0; i < br->nstages; i++)
		free(br->stages[i].name);
	memset(br, 0, sizeof *br);
	br->number = wb->count;
	br->start = *now;
	if (wb->nhistory < BOOT_HISTORY)
		wb->nhistory++;

	/* Start a new log file for each boot after the first. */
	if (wp->serial != NULL && wb->count > 1)
		serial_pane_log_open(wp);
}

/* A boot stage pattern matched. */
static void
output_match_stage(struct window_pane *wp, u_int i, const char *line)
{
	struct window_pane_match	*wm = wp->match;
	struct window_pane_boot		*wb = output_match_get_boot(wp);
	struct boot_record		*br;
	struct timeval			 now;
	int				 new_boot;

	/*
	 * An earlier stage than the current one means the board was reset.
	 * So does the same stage again if it is marked with "!" (bootloader
	 * banners); other stages may match several lines in one boot.
	 */
	if (wb->stage == -1)
		new_boot = 1;
	else if ((int)i < wb->stage)
		new_boot = 1;
	else if ((int)i == wb->stage)
		new_boot = wm->stages[i].restart;
	else
		new_boot = 0;
	if (!new_boot && (int)i == wb->stage)
		return;

	gettimeofday(&now, NULL);
	if (new_boot)
		output_match_new_boot(wp, &now);

	wb->stage = i;
	free(wb->stage_name);
	wb->stage_name = xstrdup(wm->stages[i].name);
	if (i == wm->nstages - 1)
		wb->end = now;

	br = &wb->history[wb->current];
	if (br->nstages < BOOT_STAGES_MAX) {
		br->stages[br->nstages].name = xstrdup(wm->stages[i].name);
		br->stages[br->nstages].at = now;
		br->nstages++;
	}

	server_redraw_window_borders(wp->window);
	server_status_window(wp->window);
	output_match_fire(wp, "pane-boot-stage", wm->stages[i].name, line);
}

/* Match a complete line. */
static void
output_match_line(struct window_pane *wp, const char *line)
{
	struct window_pane_match	*wm = wp->match;
	u_int				 i;
	time_t				 t;

	for (i = 0; i < wm->nstages; i++) {
		if (regexec(&wm->stages[i].re, line, 0, NULL, 0) == 0) {
			output_match_stage(wp, i, line);
			break;
		}
	}
	output_match_autoboot(wp, line);

	for (i = 0; i < wm->npatterns; i++) {
		if (regexec(&wm->patterns[i].re, line, 0, NULL, 0) != 0)
			continue;
		t = time(NULL);
		if (t != wm->rate_second) {
			if (wm->rate_dropped != 0) {
				log_debug("%s: %%%u: dropped %u matches",
				    __func__, wp->id, wm->rate_dropped);
			}
			wm->rate_second = t;
			wm->rate_count = 0;
			wm->rate_dropped = 0;
		}
		if (wm->rate_count++ >= OUTPUT_MATCH_RATE) {
			wm->rate_dropped++;
			continue;
		}
		output_match_fire(wp, "pane-output-match",
		    wm->patterns[i].name, line);
	}
}

/*
 * Check a line for the autoboot prompt. U-Boot leaves the prompt without a
 * newline while it counts down, so this is called for the incomplete line
 * after each read as well as for complete lines.
 */
static void
output_match_autoboot(struct window_pane *wp, const char *line)
{
	struct window_pane_match	*wm = wp->match;
	struct window_pane_boot		*wb;

	if (!wm->has_autoboot || *line == '\0')
		return;
	wb = output_match_get_boot(wp);
	if (wb->autoboot_sent)
		return;
	if (regexec(&wm->autoboot, line, 0, NULL, 0) != 0)
		return;
	wb->autoboot_sent = 1;
	if (wp->fd != -1 && wp->event != NULL) {
		bufferevent_write(wp->event, wm->autoboot_keys,
		    strlen(wm->autoboot_keys));
	}
	log_debug("%s: %%%u: sent autoboot interrupt", __func__, wp->id);
}

/* Feed pane output to the matcher. */
void
output_match_feed(struct window_pane *wp, const u_char *buf, size_t len)
{
	struct window_pane_match	*wm = wp->match;
	size_t				 i;
	u_char				 ch;

	for (i = 0; i < len; i++) {
		ch = buf[i];
		switch (wm->esc) {
		case OUTPUT_MATCH_ESC_START:
			if (ch == '[')
				wm->esc = OUTPUT_MATCH_ESC_CSI;
			else if (ch == ']' || ch == 'P' || ch == '^' ||
			    ch == '_' || ch == 'X')
				wm->esc = OUTPUT_MATCH_ESC_STRING;
			else
				wm->esc = OUTPUT_MATCH_ESC_NONE;
			continue;
		case OUTPUT_MATCH_ESC_CSI:
			if (ch >= 0x40 && ch <= 0x7e)
				wm->esc = OUTPUT_MATCH_ESC_NONE;
			continue;
		case OUTPUT_MATCH_ESC_STRING:
			if (ch == '\007')
				wm->esc = OUTPUT_MATCH_ESC_NONE;
			else if (ch == '\033')
				wm->esc = OUTPUT_MATCH_ESC_STRING_ESC;
			continue;
		case OUTPUT_MATCH_ESC_STRING_ESC:
			if (ch == '\\')
				wm->esc = OUTPUT_MATCH_ESC_NONE;
			else
				wm->esc = OUTPUT_MATCH_ESC_STRING;
			continue;
		}

		if (ch == '\033') {
			wm->esc = OUTPUT_MATCH_ESC_START;
			continue;
		}
		if (ch == '\n') {
			wm->line[wm->len] = '\0';
			output_match_line(wp, wm->line);
			if (wp->match != wm)	/* freed by a hook */
				return;
			wm->len = 0;
			continue;
		}
		if (ch == '\r' || ch == '\0')
			continue;
		if (ch == '\b') {
			if (wm->len > 0)
				wm->len--;
			continue;
		}
		if (wm->len < sizeof wm->line - 1)
			wm->line[wm->len++] = ch;
	}
	if (wm->len != 0) {
		wm->line[wm->len] = '\0';
		output_match_autoboot(wp, wm->line);
	}
}

/* Free boot history when the pane is destroyed. */
void
output_match_free_boot(struct window_pane *wp)
{
	struct window_pane_boot	*wb = wp->boot;
	u_int			 i, j;

	if (wb == NULL)
		return;
	for (i = 0; i < BOOT_HISTORY; i++) {
		for (j = 0; j < wb->history[i].nstages; j++)
			free(wb->history[i].stages[j].name);
	}
	free(wb->stage_name);
	free(wb);
	wp->boot = NULL;
}

/* Seconds since the current boot started (or its length, if finished). */
int
output_match_boot_elapsed(struct window_pane *wp, struct timeval *tv)
{
	struct window_pane_boot	*wb = wp->boot;
	struct timeval		 now;

	if (wb == NULL || wb->count == 0)
		return (-1);
	if (timerisset(&wb->end))
		timersub(&wb->end, &wb->start, tv);
	else {
		gettimeofday(&now, NULL);
		timersub(&now, &wb->start, tv);
	}
	return (0);
}
