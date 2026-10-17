/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "posix_win.h"
#include <windows.h>
#include <stdint.h>
#include <string.h>

int winawdl_clock_gettime(int clock, struct timespec *out) {
    LARGE_INTEGER ticks, frequency;
    if (clock != 1 || !out || !QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&ticks) || frequency.QuadPart <= 0 || ticks.QuadPart < 0)
        return -1;
    out->tv_sec = (time_t)(ticks.QuadPart / frequency.QuadPart);
    out->tv_nsec = (long)((double)(ticks.QuadPart % frequency.QuadPart) *
                          1000000000.0 / (double)frequency.QuadPart);
    return 0;
}

/* Short-option surface used by awdld. No argv mutation or GNU permutation.
 * optind=0 resets the parser for a new invocation (also exercised offline). */
char *winawdl_optarg;
int winawdl_optind = 1;
int winawdl_getopt(int argc, char *const argv[], const char *options) {
    static const char *next;
    const char *found;
    int option;
    winawdl_optarg = NULL;
    if (!winawdl_optind) {winawdl_optind = 1; next = NULL;}
    if (!next || !*next) {
        if (winawdl_optind >= argc || argv[winawdl_optind][0] != '-' ||
            !argv[winawdl_optind][1]) return -1;
        if (!strcmp(argv[winawdl_optind], "--")) {++winawdl_optind;return -1;}
        next = argv[winawdl_optind++] + 1;
    }
    option = (unsigned char)*next++;
    found = strchr(options, option);
    if (!found || option == ':') return '?';
    if (found[1] == ':') {
        if (*next) {winawdl_optarg = (char *)next;next = NULL;}
        else if (winawdl_optind < argc) {winawdl_optarg = argv[winawdl_optind++];next = NULL;}
        else return '?';
    }
    return option;
}
