/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The libev subset from compat/ev.h. Its waits use a high-resolution timer:
 * AWDL slots are 16 ms and an ordinary Windows wait can overshoot by that. */
#include <ev.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>

#define MAX_TIMERS 16
#define MAX_IOS 8
#define MAX_PENDING 32
/* Handle "file descriptors" start above the three standard streams. */
#define FIRST_FD 3

struct pending_event {
	ev_watcher *watcher;
	int revents;
};

struct ev_loop {
	ev_timer *timers[MAX_TIMERS];
	ev_io *ios[MAX_IOS];
	struct pending_event pending[MAX_PENDING];
	int pending_count;
	HANDLE fd_handles[MAX_IOS];
	int fd_count;
	HANDLE wake_timer;
	HANDLE stop_event;
	volatile LONG stop;
	LARGE_INTEGER frequency;
};

static struct ev_loop default_loop;

static uint64_t now_us(const struct ev_loop *loop) {
	LARGE_INTEGER ticks;
	QueryPerformanceCounter(&ticks);
	return (uint64_t) (ticks.QuadPart / loop->frequency.QuadPart) * 1000000u +
	       (uint64_t) (ticks.QuadPart % loop->frequency.QuadPart) * 1000000u /
		       (uint64_t) loop->frequency.QuadPart;
}

static uint64_t seconds_to_us(double seconds) {
	return seconds > 0 ? (uint64_t) (seconds * 1e6) : 0;
}

static BOOL WINAPI on_console_ctrl(DWORD type) {
	(void) type;
	ev_win_stop(&default_loop);
	return TRUE;
}

struct ev_loop *ev_default_loop(void) {
	struct ev_loop *loop = &default_loop;
	if (!loop->stop_event) {
		QueryPerformanceFrequency(&loop->frequency);
		loop->stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
		loop->wake_timer = CreateWaitableTimerExA(
			NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		SetConsoleCtrlHandler(on_console_ctrl, TRUE);
	}
	return loop;
}

void ev_win_stop(struct ev_loop *loop) {
	InterlockedExchange(&loop->stop, 1);
	SetEvent(loop->stop_event);
}

int ev_win_fd_for_handle(void *handle) {
	struct ev_loop *loop = ev_default_loop();
	if (loop->fd_count == MAX_IOS)
		return -1;
	loop->fd_handles[loop->fd_count] = handle;
	return FIRST_FD + loop->fd_count++;
}

static HANDLE handle_for_fd(const struct ev_loop *loop, int fd) {
	int index = fd - FIRST_FD;
	return (index >= 0 && index < loop->fd_count) ? loop->fd_handles[index] : NULL;
}

void ev_feed_event(struct ev_loop *loop, void *watcher, int revents) {
	if (loop->pending_count < MAX_PENDING)
		loop->pending[loop->pending_count++] = (struct pending_event) {watcher, revents};
}

void ev_timer_start(struct ev_loop *loop, ev_timer *timer) {
	timer->due_us = now_us(loop) + seconds_to_us(timer->after);
	if (timer->active)
		return;
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (!loop->timers[i]) {
			loop->timers[i] = timer;
			timer->active = 1;
			return;
		}
	}
}

void ev_timer_stop(struct ev_loop *loop, ev_timer *timer) {
	for (int i = 0; i < MAX_TIMERS; i++)
		if (loop->timers[i] == timer)
			loop->timers[i] = NULL;
	timer->active = 0;
}

void ev_timer_again(struct ev_loop *loop, ev_timer *timer) {
	if (timer->repeat <= 0) {
		ev_timer_stop(loop, timer);
		return;
	}
	timer->after = timer->repeat;
	ev_timer_start(loop, timer);
}

void ev_io_start(struct ev_loop *loop, ev_io *io) {
	if (io->active || !handle_for_fd(loop, io->fd))
		return;
	for (int i = 0; i < MAX_IOS; i++) {
		if (!loop->ios[i]) {
			loop->ios[i] = io;
			io->active = 1;
			return;
		}
	}
}

void ev_signal_start(struct ev_loop *loop, ev_signal *signal) {
	(void) loop;
	signal->active = 1;
}

static void run_pending(struct ev_loop *loop) {
	/* Callbacks may feed new events; those wait for the next iteration. */
	struct pending_event batch[MAX_PENDING];
	int count = loop->pending_count;
	for (int i = 0; i < count; i++)
		batch[i] = loop->pending[i];
	loop->pending_count = 0;
	for (int i = 0; i < count; i++)
		batch[i].watcher->cb(loop, batch[i].watcher, batch[i].revents);
}

static void run_due_timers(struct ev_loop *loop) {
	uint64_t now = now_us(loop);
	for (int i = 0; i < MAX_TIMERS; i++) {
		ev_timer *timer = loop->timers[i];
		if (!timer || timer->due_us > now)
			continue;
		if (timer->repeat > 0) {
			/* Step from the deadline, not from now, so the period does not
			 * drift; resynchronize after a long stall. */
			timer->due_us += seconds_to_us(timer->repeat);
			if (timer->due_us <= now)
				timer->due_us = now + seconds_to_us(timer->repeat);
		} else {
			ev_timer_stop(loop, timer);
		}
		timer->cb(loop, timer, EV_TIMER);
	}
}

static void wait_for_work(struct ev_loop *loop) {
	HANDLE handles[2 + MAX_IOS];
	ev_io *watchers[2 + MAX_IOS] = {0};
	DWORD count = 0;
	handles[count++] = loop->stop_event;

	uint64_t earliest = UINT64_MAX;
	for (int i = 0; i < MAX_TIMERS; i++)
		if (loop->timers[i] && loop->timers[i]->due_us < earliest)
			earliest = loop->timers[i]->due_us;
	if (earliest != UINT64_MAX) {
		uint64_t now = now_us(loop);
		if (earliest <= now)
			return;
		LARGE_INTEGER due;
		due.QuadPart = -(LONGLONG) ((earliest - now) * 10); /* relative, 100 ns */
		SetWaitableTimer(loop->wake_timer, &due, 0, NULL, NULL, FALSE);
		handles[count++] = loop->wake_timer;
	}
	for (int i = 0; i < MAX_IOS; i++) {
		if (!loop->ios[i])
			continue;
		watchers[count] = loop->ios[i];
		handles[count++] = handle_for_fd(loop, loop->ios[i]->fd);
	}

	DWORD result = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
	DWORD index = result - WAIT_OBJECT_0;
	if (index < count && watchers[index])
		ev_feed_event(loop, watchers[index], EV_READ);
}

void ev_run(struct ev_loop *loop, int flags) {
	(void) flags;
	while (!loop->stop) {
		run_pending(loop);
		run_due_timers(loop);
		if (loop->pending_count == 0)
			wait_for_work(loop);
	}
}
