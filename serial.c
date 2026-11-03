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
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Serial panes: a pane backed by an opened tty device rather than a forked
 * child on a pty. The device fd is given to the pane's bufferevent like a
 * pty, so output goes through the normal input path (scrollback, copy mode,
 * pipe-pane, triggers all work).
 *
 * When the device goes away (EOF or error on read), the bufferevent is kept
 * (the pane's buffer offsets depend on it) but its fd is closed and
 * detached; a timer then tries to reopen the device, resolving the spec
 * again so a match on USB properties survives re-enumeration.
 *
 * A device spec is either a path (/dev/ttyUSB0, /dev/serial/by-id/...) or a
 * comma-separated list of USB properties matched against sysfs:
 * vid=0403,pid=6001,serial=A50285BI,if=2 (usb:VID:PID is a shorthand).
 */

#define SERIAL_RECONNECT_INTERVAL 500	/* milliseconds */

struct serial_match {
	char	*vid;
	char	*pid;
	char	*serial;
	int	 interface;	/* -1 for any */
};

static const struct {
	u_int	rate;
	speed_t	speed;
} serial_speeds[] = {
	{ 50, B50 }, { 75, B75 }, { 110, B110 }, { 134, B134 },
	{ 150, B150 }, { 200, B200 }, { 300, B300 }, { 600, B600 },
	{ 1200, B1200 }, { 1800, B1800 }, { 2400, B2400 }, { 4800, B4800 },
	{ 9600, B9600 }, { 19200, B19200 }, { 38400, B38400 },
#ifdef B57600
	{ 57600, B57600 },
#endif
#ifdef B115200
	{ 115200, B115200 },
#endif
#ifdef B230400
	{ 230400, B230400 },
#endif
#ifdef B460800
	{ 460800, B460800 },
#endif
#ifdef B500000
	{ 500000, B500000 },
#endif
#ifdef B576000
	{ 576000, B576000 },
#endif
#ifdef B921600
	{ 921600, B921600 },
#endif
#ifdef B1000000
	{ 1000000, B1000000 },
#endif
#ifdef B1152000
	{ 1152000, B1152000 },
#endif
#ifdef B1500000
	{ 1500000, B1500000 },
#endif
#ifdef B2000000
	{ 2000000, B2000000 },
#endif
#ifdef B2500000
	{ 2500000, B2500000 },
#endif
#ifdef B3000000
	{ 3000000, B3000000 },
#endif
#ifdef B3500000
	{ 3500000, B3500000 },
#endif
#ifdef B4000000
	{ 4000000, B4000000 },
#endif
};

static void	serial_pane_timer(int, short, void *);

/* Find the termios speed for a baud rate. */
int
serial_pane_speed(u_int baud, speed_t *speed)
{
	u_int	i;

	for (i = 0; i < nitems(serial_speeds); i++) {
		if (serial_speeds[i].rate == baud) {
			if (speed != NULL)
				*speed = serial_speeds[i].speed;
			return (0);
		}
	}
	return (-1);
}

/* Name of the state, for formats. */
const char *
serial_pane_state(struct window_pane_serial *ser)
{
	if (ser->state == SERIAL_CONNECTED)
		return ("connected");
	return ("disconnected");
}

static void
serial_match_free(struct serial_match *sm)
{
	free(sm->vid);
	free(sm->pid);
	free(sm->serial);
}

/* Parse a property spec. Returns 0 if it is a path. */
static int
serial_match_parse(const char *spec, struct serial_match *sm, char **cause)
{
	char		*copy, *next, *item, *value;
	const char	*errstr;

	memset(sm, 0, sizeof *sm);
	sm->interface = -1;

	if (*spec == '/' || *spec == '.')
		return (0);

	copy = next = xstrdup(spec);
	if (strncmp(copy, "usb:", 4) == 0) {
		/* usb:VID:PID[:SERIAL] */
		next = copy + 4;
		if ((item = strsep(&next, ":")) != NULL && *item != '\0')
			sm->vid = xstrdup(item);
		if ((item = strsep(&next, ":")) != NULL && *item != '\0')
			sm->pid = xstrdup(item);
		if (next != NULL && *next != '\0')
			sm->serial = xstrdup(next);
		free(copy);
		goto check;
	}
	while ((item = strsep(&next, ",")) != NULL) {
		if (*item == '\0')
			continue;
		value = strchr(item, '=');
		if (value == NULL) {
			xasprintf(cause, "bad device property: %s", item);
			goto fail;
		}
		*value++ = '\0';
		if (strcasecmp(item, "vid") == 0)
			sm->vid = xstrdup(value);
		else if (strcasecmp(item, "pid") == 0)
			sm->pid = xstrdup(value);
		else if (strcasecmp(item, "serial") == 0 ||
		    strcasecmp(item, "ID_SERIAL_SHORT") == 0)
			sm->serial = xstrdup(value);
		else if (strcasecmp(item, "if") == 0 ||
		    strcasecmp(item, "interface") == 0) {
			sm->interface = strtonum(value, 0, 255, &errstr);
			if (errstr != NULL) {
				xasprintf(cause, "bad interface: %s", value);
				goto fail;
			}
		} else {
			xasprintf(cause, "unknown device property: %s", item);
			goto fail;
		}
	}
	free(copy);

check:
	if (sm->vid == NULL && sm->pid == NULL && sm->serial == NULL) {
		xasprintf(cause, "no device properties: %s", spec);
		serial_match_free(sm);
		return (-1);
	}
	return (1);

fail:
	free(copy);
	serial_match_free(sm);
	return (-1);
}

/* Read a one-line sysfs attribute. */
static char *
serial_read_attr(const char *dir, const char *name)
{
	char	 path[PATH_MAX], line[256];
	FILE	*f;

	if (snprintf(path, sizeof path, "%s/%s", dir, name) >=
	    (int)sizeof path)
		return (NULL);
	if ((f = fopen(path, "r")) == NULL)
		return (NULL);
	if (fgets(line, sizeof line, f) == NULL) {
		fclose(f);
		return (NULL);
	}
	fclose(f);
	line[strcspn(line, "\r\n")] = '\0';
	return (xstrdup(line));
}

/* Does this tty match the USB properties? */
static int
serial_match_tty(const char *name, struct serial_match *sm)
{
	char	 path[PATH_MAX], real[PATH_MAX], *cp, *value;
	int	 i, interface = -1, found = 0, ok = 1;

	if (snprintf(path, sizeof path, "/sys/class/tty/%s/device",
	    name) >= (int)sizeof path)
		return (0);
	if (realpath(path, real) == NULL)
		return (0);

	/* Walk up to the USB interface and then the USB device. */
	for (i = 0; i < 6; i++) {
		if (interface == -1 &&
		    (value = serial_read_attr(real, "bInterfaceNumber")) !=
		    NULL) {
			interface = (int)strtol(value, NULL, 16);
			free(value);
		}
		if ((value = serial_read_attr(real, "idVendor")) != NULL) {
			found = 1;
			if (sm->vid != NULL && strcasecmp(value, sm->vid) != 0)
				ok = 0;
			free(value);
			if (sm->pid != NULL) {
				value = serial_read_attr(real, "idProduct");
				if (value == NULL ||
				    strcasecmp(value, sm->pid) != 0)
					ok = 0;
				free(value);
			}
			if (sm->serial != NULL) {
				value = serial_read_attr(real, "serial");
				if (value == NULL ||
				    strcmp(value, sm->serial) != 0)
					ok = 0;
				free(value);
			}
			break;
		}
		cp = strrchr(real, '/');
		if (cp == NULL || cp == real)
			break;
		*cp = '\0';
	}
	if (!found)
		return (0);
	if (sm->interface != -1 && sm->interface != interface)
		return (0);
	return (ok);
}

static int
serial_name_cmp(const void *a, const void *b)
{
	return (strcmp(*(char * const *)a, *(char * const *)b));
}

/* Resolve a device spec to a path. */
char *
serial_pane_resolve(const char *spec, char **cause)
{
	struct serial_match	 sm;
	DIR			*d;
	struct dirent		*de;
	char			**names = NULL, *path = NULL;
	u_int			 n = 0, i;

	switch (serial_match_parse(spec, &sm, cause)) {
	case -1:
		return (NULL);
	case 0:
		return (xstrdup(spec));
	}

	if ((d = opendir("/sys/class/tty")) == NULL) {
		xasprintf(cause, "cannot match device properties: %s",
		    strerror(errno));
		serial_match_free(&sm);
		return (NULL);
	}
	while ((de = readdir(d)) != NULL) {
		if (*de->d_name == '.')
			continue;
		if (serial_match_tty(de->d_name, &sm)) {
			names = xreallocarray(names, n + 1, sizeof *names);
			names[n++] = xstrdup(de->d_name);
		}
	}
	closedir(d);
	serial_match_free(&sm);

	if (n == 0) {
		xasprintf(cause, "no device matches %s", spec);
		return (NULL);
	}

	/* Sort so the choice is stable when several match. */
	qsort(names, n, sizeof *names, serial_name_cmp);
	xasprintf(&path, "/dev/%s", names[0]);
	if (n > 1)
		log_debug("%s: %u devices match %s, using %s", __func__, n,
		    spec, path);
	for (i = 0; i < n; i++)
		free(names[i]);
	free(names);
	return (path);
}

/* Create serial state with settings from the options. */
struct window_pane_serial *
serial_pane_create(const char *spec, struct options *oo)
{
	struct window_pane_serial	*ser;

	ser = xcalloc(1, sizeof *ser);
	ser->spec = xstrdup(spec);
	ser->baud = options_get_number(oo, "serial-baud");
	ser->flow = options_get_number(oo, "serial-flow");
	ser->parity = options_get_number(oo, "serial-parity");
	ser->data_bits = options_get_number(oo, "serial-data-bits");
	ser->stop_bits = options_get_number(oo, "serial-stop-bits");
	ser->state = SERIAL_DISCONNECTED;
	ser->line_start = 1;
	ser->log_fd = -1;
	return (ser);
}

/* Configure an open device. */
int
serial_pane_configure(struct window_pane_serial *ser, int fd, char **cause)
{
	struct termios	tio;
	speed_t		speed;

	if (serial_pane_speed(ser->baud, &speed) != 0) {
		xasprintf(cause, "unsupported baud rate: %u", ser->baud);
		return (-1);
	}
	if (tcgetattr(fd, &tio) != 0) {
		xasprintf(cause, "%s: %s", ser->device, strerror(errno));
		return (-1);
	}

	cfmakeraw(&tio);
	tio.c_cflag |= (CLOCAL|CREAD);
	tio.c_cflag &= ~(CSIZE|PARENB|PARODD|CSTOPB);
#ifdef CRTSCTS
	tio.c_cflag &= ~CRTSCTS;
#endif
	tio.c_iflag &= ~(IXON|IXOFF|IXANY);

	switch (ser->data_bits) {
	case 5:
		tio.c_cflag |= CS5;
		break;
	case 6:
		tio.c_cflag |= CS6;
		break;
	case 7:
		tio.c_cflag |= CS7;
		break;
	default:
		tio.c_cflag |= CS8;
		break;
	}
	if (ser->parity == SERIAL_PARITY_EVEN)
		tio.c_cflag |= PARENB;
	else if (ser->parity == SERIAL_PARITY_ODD)
		tio.c_cflag |= (PARENB|PARODD);
	if (ser->stop_bits == 2)
		tio.c_cflag |= CSTOPB;
	if (ser->flow == SERIAL_FLOW_RTSCTS) {
#ifdef CRTSCTS
		tio.c_cflag |= CRTSCTS;
#else
		xasprintf(cause, "RTS/CTS flow control not supported");
		return (-1);
#endif
	} else if (ser->flow == SERIAL_FLOW_XONXOFF)
		tio.c_iflag |= (IXON|IXOFF);
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;

	cfsetispeed(&tio, speed);
	cfsetospeed(&tio, speed);
	if (tcsetattr(fd, TCSANOW, &tio) != 0) {
		xasprintf(cause, "%s: %s", ser->device, strerror(errno));
		return (-1);
	}
	return (0);
}

/* Resolve, open, lock and configure the device. Returns the fd. */
int
serial_pane_open(struct window_pane_serial *ser, char **cause)
{
	char	*path;
	int	 fd;

	if ((path = serial_pane_resolve(ser->spec, cause)) == NULL)
		return (-1);
	free(ser->device);
	ser->device = path;

	fd = open(path, O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC);
	if (fd == -1) {
		if (errno == EACCES) {
			xasprintf(cause, "%s: permission denied (is the user "
			    "in the dialout or uucp group?)", path);
		} else if (errno == EBUSY) {
			xasprintf(cause, "%s: device busy", path);
		} else
			xasprintf(cause, "%s: %s", path, strerror(errno));
		return (-1);
	}
	if (!isatty(fd)) {
		if (errno == ENOTTY)
			xasprintf(cause, "%s: not a terminal device", path);
		else
			xasprintf(cause, "%s: %s", path, strerror(errno));
		close(fd);
		return (-1);
	}

	/* Same advisory lock as picocom, minicom (lockdev) and screen use. */
	if (flock(fd, LOCK_EX|LOCK_NB) != 0) {
		if (errno == EWOULDBLOCK) {
			xasprintf(cause, "%s: device busy (in use by another "
			    "program)", path);
		} else
			xasprintf(cause, "%s: %s", path, strerror(errno));
		close(fd);
		return (-1);
	}

	if (serial_pane_configure(ser, fd, cause) != 0) {
		close(fd);
		return (-1);
	}
	return (fd);
}

/* Write a status message into the pane, dimmed. */
static void
serial_pane_message(struct window_pane *wp, const char *fmt, ...)
{
	struct window_pane_serial	*ser = wp->serial;
	va_list				 ap;
	char				*msg, *line;

	va_start(ap, fmt);
	xvasprintf(&msg, fmt, ap);
	va_end(ap);

	xasprintf(&line, "%s\033[2m[tmux: %s]\033[22m\r\n",
	    ser->line_start ? "" : "\r\n", msg);
	input_parse_buffer(wp, line, strlen(line));
	ser->line_start = 1;
	free(line);
	free(msg);
}

/* Fire a serial pane hook. */
static void
serial_pane_fire(struct window_pane *wp, const char *name)
{
	struct event_payload	*ep;
	struct cmd_find_state	 fs;

	ep = event_payload_create();
	cmd_find_from_pane(&fs, wp, 0);
	event_payload_set_target(ep, &fs);
	event_payload_set_pane(ep, "pane", wp);
	event_payload_set_window(ep, "window", wp->window);
	event_payload_set_string(ep, "serial_device", "%s",
	    wp->serial->device != NULL ? wp->serial->device : "");
	events_fire(name, ep);
}

/* Attach an open fd to a pane that already has a bufferevent. */
static void
serial_pane_attach(struct window_pane *wp, int fd)
{
	struct window_pane_serial	*ser = wp->serial;

	wp->fd = fd;
	setblocking(fd, 0);
	bufferevent_setfd(wp->event, fd);
	evbuffer_drain(EVBUFFER_OUTPUT(wp->event),
	    EVBUFFER_LENGTH(EVBUFFER_OUTPUT(wp->event)));
	bufferevent_enable(wp->event, EV_READ|EV_WRITE);
	ser->state = SERIAL_CONNECTED;
}

/* Called once the pane is set up with its first fd. */
void
serial_pane_start(struct window_pane *wp)
{
	struct window_pane_serial	*ser = wp->serial;

	ser->wp = wp;
	ser->state = SERIAL_CONNECTED;
	evtimer_set(&ser->timer, serial_pane_timer, wp);
	serial_pane_log_open(wp);
}

/* Close the device, keeping the bufferevent. */
static void
serial_pane_close(struct window_pane *wp)
{
	struct window_pane_serial	*ser = wp->serial;

	bufferevent_disable(wp->event, EV_READ|EV_WRITE);
	bufferevent_setfd(wp->event, -1);
	if (wp->fd != -1) {
		close(wp->fd);
		wp->fd = -1;
	}
	ser->state = SERIAL_DISCONNECTED;
	server_redraw_window_borders(wp->window);
	server_status_window(wp->window);
}

static void
serial_pane_schedule(struct window_pane *wp)
{
	struct window_pane_serial	*ser = wp->serial;
	struct timeval			 tv = {
		.tv_sec = SERIAL_RECONNECT_INTERVAL / 1000,
		.tv_usec = (SERIAL_RECONNECT_INTERVAL % 1000) * 1000
	};

	evtimer_del(&ser->timer);
	evtimer_add(&ser->timer, &tv);
}

/* The device has gone (read error or EOF). */
void
serial_pane_disconnected(struct window_pane *wp)
{
	struct window_pane_serial	*ser = wp->serial;

	if (ser->state == SERIAL_DISCONNECTED) {
		/* Already closed (failed reconnect): keep trying if enabled. */
		if (options_get_number(wp->options, "serial-reconnect"))
			serial_pane_schedule(wp);
		return;
	}
	log_debug("%s: %%%u %s", __func__, wp->id, ser->device);

	serial_pane_close(wp);
	if (options_get_number(wp->options, "serial-reconnect")) {
		serial_pane_message(wp, "%s disconnected, waiting for it to "
		    "come back", ser->device);
		serial_pane_schedule(wp);
	} else
		serial_pane_message(wp, "%s disconnected", ser->device);
	serial_pane_fire(wp, "pane-serial-disconnected");
}

/* Try to reopen now. Returns 0 on success. */
int
serial_pane_reconnect(struct window_pane *wp, char **cause)
{
	struct window_pane_serial	*ser = wp->serial;
	int				 fd;

	evtimer_del(&ser->timer);
	if (ser->state == SERIAL_CONNECTED)
		serial_pane_close(wp);

	if ((fd = serial_pane_open(ser, cause)) == -1)
		return (-1);
	serial_pane_attach(wp, fd);
	serial_pane_message(wp, "connected to %s at %u", ser->device,
	    ser->baud);
	server_redraw_window_borders(wp->window);
	server_status_window(wp->window);
	serial_pane_fire(wp, "pane-serial-connected");
	return (0);
}

static void
serial_pane_timer(__unused int fd, __unused short events, void *data)
{
	struct window_pane	*wp = data;
	char			*cause = NULL;

	if (serial_pane_reconnect(wp, &cause) != 0) {
		log_debug("%s: %%%u: %s", __func__, wp->id, cause);
		free(cause);
		serial_pane_schedule(wp);
	}
}

/* Apply changed settings to a connected device. */
int
serial_pane_apply(struct window_pane *wp, char **cause)
{
	struct window_pane_serial	*ser = wp->serial;

	if (ser->state != SERIAL_CONNECTED || wp->fd == -1)
		return (0);
	return (serial_pane_configure(ser, wp->fd, cause));
}

/* Open a log file for the pane, if serial-log-dir is set. */
void
serial_pane_log_open(struct window_pane *wp)
{
	struct window_pane_serial	*ser = wp->serial;
	struct winlink			*wl;
	struct options			*oo;
	const char			*dir, *home, *name;
	char				*expanded, stamp[32], *path;
	struct tm			 tm;
	time_t				 t;

	if (ser->log_fd != -1) {
		close(ser->log_fd);
		ser->log_fd = -1;
	}

	/* The session the pane's window is in (any, if linked to several). */
	wl = TAILQ_FIRST(&wp->window->winlinks);
	if (wl != NULL && wl->session != NULL)
		oo = wl->session->options;
	else
		oo = global_s_options;
	dir = options_get_string(oo, "serial-log-dir");
	if (*dir == '\0')
		return;
	if (dir[0] == '~' && (dir[1] == '/' || dir[1] == '\0') &&
	    (home = find_home()) != NULL)
		xasprintf(&expanded, "%s%s", home, dir + 1);
	else
		expanded = xstrdup(dir);

	name = strrchr(ser->device, '/');
	name = (name != NULL) ? name + 1 : ser->device;
	t = time(NULL);
	localtime_r(&t, &tm);
	strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
	if (wp->boot != NULL && wp->boot->count != 0) {
		xasprintf(&path, "%s/%s-%s-boot%u.log", expanded, name, stamp,
		    wp->boot->count);
	} else
		xasprintf(&path, "%s/%s-%s.log", expanded, name, stamp);
	free(expanded);

	ser->log_fd = open(path, O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC, 0644);
	if (ser->log_fd == -1)
		log_debug("%s: %s: %s", __func__, path, strerror(errno));
	free(ser->log_path);
	ser->log_path = path;
}

/* Build the timestamp prefix for a line. */
static size_t
serial_pane_stamp(struct window_pane_serial *ser, int mode, char *buf,
    size_t size)
{
	struct timeval	 tv, delta;
	struct tm	 tm;
	char		 hms[16];

	gettimeofday(&tv, NULL);
	if (mode == SERIAL_TIMESTAMPS_DELTA) {
		if (!timerisset(&ser->last_line))
			timerclear(&delta);
		else
			timersub(&tv, &ser->last_line, &delta);
		ser->last_line = tv;
		return (snprintf(buf, size, "[+%ld.%03ld] ",
		    (long)delta.tv_sec, (long)delta.tv_usec / 1000));
	}
	ser->last_line = tv;
	localtime_r(&tv.tv_sec, &tm);
	strftime(hms, sizeof hms, "%H:%M:%S", &tm);
	return (snprintf(buf, size, "[%s.%03ld] ", hms,
	    (long)tv.tv_usec / 1000));
}

/*
 * Output from a serial device: add timestamps and write the log if
 * enabled, then parse it.
 */
void
serial_pane_input(struct window_pane *wp, const u_char *buf, size_t len)
{
	struct window_pane_serial	*ser = wp->serial;
	int				 mode;
	size_t				 i, start = 0, n;
	char				 stamp[64], screen[80];

	mode = options_get_number(wp->options, "serial-timestamps");
	if (mode == SERIAL_TIMESTAMPS_OFF) {
		if (ser->log_fd != -1 && write(ser->log_fd, buf, len) == -1)
			log_debug("%s: log write failed", __func__);
		if (len != 0)
			ser->line_start = (buf[len - 1] == '\n');
		input_parse_buffer(wp, buf, len);
		return;
	}

	for (i = 0; i < len; i++) {
		if (ser->line_start && buf[i] != '\r' && buf[i] != '\n') {
			if (i > start) {
				input_parse_buffer(wp, buf + start, i - start);
				if (ser->log_fd != -1)
					write(ser->log_fd, buf + start,
					    i - start);
			}
			start = i;
			n = serial_pane_stamp(ser, mode, stamp, sizeof stamp);
			snprintf(screen, sizeof screen, "\033[2m%s\033[22m",
			    stamp);
			input_parse_buffer(wp, screen, strlen(screen));
			if (ser->log_fd != -1)
				write(ser->log_fd, stamp, n);
			ser->line_start = 0;
		}
		if (buf[i] == '\n')
			ser->line_start = 1;
	}
	if (len > start) {
		input_parse_buffer(wp, buf + start, len - start);
		if (ser->log_fd != -1)
			write(ser->log_fd, buf + start, len - start);
	}
}

/* Free serial state when the pane is destroyed. */
void
serial_pane_free(struct window_pane_serial *ser)
{
	if (ser == NULL)
		return;
	if (event_initialized(&ser->timer))
		evtimer_del(&ser->timer);
	if (ser->log_fd != -1)
		close(ser->log_fd);
	free(ser->log_path);
	free(ser->spec);
	free(ser->device);
	free(ser);
}
