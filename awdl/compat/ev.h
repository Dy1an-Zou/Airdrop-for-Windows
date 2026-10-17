/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The subset of libev that OWL's core.c uses, implemented in win/ev_win.c on
 * Windows wait handles and one high-resolution timer. */
#pragma once

#define EV_READ 0x01
#define EV_TIMER 0x100
#define EV_SIGNAL 0x400

struct ev_loop;

/* Every watcher starts with these members, so a pointer to any watcher can be
 * used as a pointer to struct ev_watcher. */
#define EV_WATCHER(type)                                                                           \
	int active;                                                                                \
	void *data;                                                                                \
	void (*cb)(struct ev_loop * loop, struct type * watcher, int revents);

typedef struct ev_watcher {
	EV_WATCHER(ev_watcher)
} ev_watcher;

typedef struct ev_timer {
	EV_WATCHER(ev_timer)
	double after;
	double repeat;
	unsigned long long due_us;
} ev_timer;

typedef struct ev_io {
	EV_WATCHER(ev_io)
	int fd;
	int events;
} ev_io;

typedef struct ev_signal {
	EV_WATCHER(ev_signal)
	int signum;
} ev_signal;

#define EV_DEFAULT ev_default_loop()
struct ev_loop *ev_default_loop(void);

/* Runs until ev_win_stop() or Ctrl+C. */
void ev_run(struct ev_loop *loop, int flags);
/* Queues the watcher's callback to run on the next loop iteration. */
void ev_feed_event(struct ev_loop *loop, void *watcher, int revents);

#define ev_timer_init(w, callback, after_, repeat_)                                                \
	do {                                                                                       \
		(w)->active = 0;                                                                   \
		(w)->cb = (callback);                                                              \
		ev_timer_set((w), (after_), (repeat_));                                            \
	} while (0)
#define ev_timer_set(w, after_, repeat_)                                                           \
	do {                                                                                       \
		(w)->after = (after_);                                                             \
		(w)->repeat = (repeat_);                                                           \
	} while (0)
void ev_timer_start(struct ev_loop *loop, ev_timer *timer);
void ev_timer_stop(struct ev_loop *loop, ev_timer *timer);
/* Restarts a repeating timer from now; stops a one-shot one. */
void ev_timer_again(struct ev_loop *loop, ev_timer *timer);

#define ev_io_init(w, callback, fd_, events_)                                                      \
	do {                                                                                       \
		(w)->active = 0;                                                                   \
		(w)->cb = (callback);                                                              \
		(w)->fd = (fd_);                                                                   \
		(w)->events = (events_);                                                           \
	} while (0)
void ev_io_start(struct ev_loop *loop, ev_io *io);

#define ev_signal_init(w, callback, signum_)                                                       \
	do {                                                                                       \
		(w)->active = 0;                                                                   \
		(w)->cb = (callback);                                                              \
		(w)->signum = (signum_);                                                           \
	} while (0)
/* Signals are never delivered on Windows; this only records the watcher. */
void ev_signal_start(struct ev_loop *loop, ev_signal *signal);

/* Windows side: an ev_io "file descriptor" stands for a waitable handle. */
int ev_win_fd_for_handle(void *handle);
void ev_win_stop(struct ev_loop *loop);
