/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <time.h>
int winawdl_clock_gettime(int clock, struct timespec *out);
extern char *winawdl_optarg;
extern int winawdl_optind;
int winawdl_getopt(int argc, char *const argv[], const char *options);
