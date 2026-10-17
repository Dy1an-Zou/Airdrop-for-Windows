/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Force-included into every OWL source on Windows: the libc gaps that are not
 * tied to a single header. */
#pragma once
#include <stdint.h>
#include <time.h>
#ifdef _MSC_VER
#include "../win/posix_win.h"
#define CLOCK_MONOTONIC 1
#define clock_gettime winawdl_clock_gettime
#ifndef PATH_MAX
#define PATH_MAX 260
#endif
#endif

/* core.c registers a statistics dump on SIGUSR1. Windows has no such signal
 * and the ev shim never raises it. */
#ifndef SIGUSR1
#define SIGUSR1 30
#endif

#ifndef localtime_r
static inline struct tm *owl_localtime_r(const time_t *time, struct tm *out) {
	return localtime_s(out, time) == 0 ? out : NULL;
}
#define localtime_r owl_localtime_r
#endif
