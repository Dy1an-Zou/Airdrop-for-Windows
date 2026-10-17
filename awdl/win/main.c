/* SPDX-License-Identifier: GPL-3.0-or-later */
/* AWDL daemon for Windows: OWL's protocol core, driven by the radio daemon.
 * Usage: awdld [-c 6|44|149] [-n] [-r] [-s 1|2] [-l] [-j us] [-v] [-v].
 * -n: no host adapter. -r: no device identities in the log. -s: spatial
 * streams to announce, 2 unless told. -l: announce the long guard interval
 * only. -j: microseconds to wait for the next TCP segment of a burst before
 * the joined ones go to the host, 400 unless told and 0 for no joining. */
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "core.h"
#include "frame_fixup.h"
#include "host_if.h"
#include "log.h"
#include "radio_client.h"
#include "redact.h"
#include "win_internal.h"

#define STATUS_PERIOD_S 2.0
#define DEFAULT_CHANNEL 44
#define DEFAULT_STREAMS 2
/* Longer than most gaps between the frames of one aggregate, measured. */
#define DEFAULT_JOIN_WAIT_US 400

static int shown_level = LOG_INFO;
static int redact_log;
static struct redactor redactor;
/* The radio client's reader thread logs too. */
static SRWLOCK redactor_lock = SRWLOCK_INIT;

int __real_log_log(int level, const char *file, int line, const char *fmt, ...);

/* The linker sends every log_log call here, OWL's included (--wrap), so that
 * -r also covers the lines OWL formats itself. */
int __wrap_log_log(int level, const char *file, int line, const char *fmt, ...) {
	if (level > shown_level)
		return 0;
	va_list args, measured;
	va_start(args, fmt);
	va_copy(measured, args);
	int length = vsnprintf(NULL, 0, fmt, measured);
	va_end(measured);
	if (length < 0) {
		va_end(args);
		return 0;
	}
	char text[length + 1];
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	if (!redact_log)
		return __real_log_log(level, file, line, "%s", text);

	/* Redacting never doubles a line's length. */
	char shown[2 * sizeof(text)];
	AcquireSRWLockExclusive(&redactor_lock);
	redact_line(&redactor, text, shown, sizeof(shown));
	ReleaseSRWLockExclusive(&redactor_lock);
	return __real_log_log(level, file, line, "%s", shown);
}

static int channel_from_number(int number, struct awdl_chan *chan) {
	switch (number) {
		case 6:
			*chan = CHAN_OPCLASS_6;
			return 0;
		case 44:
			*chan = CHAN_OPCLASS_44;
			return 0;
		case 149:
			*chan = CHAN_OPCLASS_149;
			return 0;
		default:
			return -1;
	}
}

static void print_status(struct ev_loop *loop, ev_timer *timer, int revents) {
	(void) loop;
	(void) revents;
	struct daemon_state *state = timer->data;
	const struct awdl_stats *stats = &state->awdl_state.stats;
	char tree[256] = "";
	char joined[48] = "";
	awdl_election_tree_print(&state->awdl_state.election, tree, sizeof(tree));
	if (winawdl_segments_joined())
		snprintf(joined, sizeof(joined), " (%llu joined)", winawdl_segments_joined());
	log_info("peers %d | rx action %llu, data %llu%s | tx action %llu, data %llu | "
		 "sync tree %s",
		 awdl_peers_length(state->awdl_state.peers.peers), stats->rx_action, stats->rx_data,
		 joined, stats->tx_action, stats->tx_data, tree);
}

int main(int argc, char *argv[]) {
	int channel = DEFAULT_CHANNEL;
	int log_level = LOG_INFO;
	const char *host_name = HOST_IF_NAME;
	int streams = DEFAULT_STREAMS;
	int short_guard_interval = 1;
	int option;
	winawdl_join_segments(DEFAULT_JOIN_WAIT_US);
	while ((option = getopt(argc, argv, "c:nrs:lj:v")) != -1) {
		if (option == 'c') {
			channel = atoi(optarg);
		} else if (option == 'n') {
			host_name = "";
		} else if (option == 'r') {
			redact_log = 1;
		} else if (option == 's') {
			streams = atoi(optarg);
		} else if (option == 'l') {
			short_guard_interval = 0;
		} else if (option == 'j') {
			winawdl_join_segments(atoi(optarg));
		} else if (option == 'v') {
			if (log_level < LOG_TRACE)
				log_level++;
		} else {
			fprintf(stderr,
				"usage: %s [-c 6|44|149] [-n] [-r] [-s 1|2] [-l] [-j us] [-v] "
				"[-v]\n",
				argv[0]);
			return EXIT_FAILURE;
		}
	}
	log_set_level(log_level);
	shown_level = log_level;

	struct awdl_chan chan;
	if (channel_from_number(channel, &chan) < 0) {
		log_error("unsupported channel %d (use 6, 44 or 149)", channel);
		return EXIT_FAILURE;
	}

	static struct daemon_state state;
	if (awdl_init(&state, "radio", host_name, chan, NULL) < 0) {
		log_error("could not initialize: is winawdl_radio running?");
		return EXIT_FAILURE;
	}
	if (radio_client_channel(winawdl_radio()) != channel) {
		log_error("the radio daemon is on channel %d, not %d",
			  radio_client_channel(winawdl_radio()), channel);
		awdl_free(&state);
		return EXIT_FAILURE;
	}
	frame_fixup_announce(streams == 2, short_guard_interval,
			     radio_client_width_mhz(winawdl_radio()));
	/* The radio's RSSI scale is not calibrated, so do not gate peers on it. */
	state.awdl_state.filter_rssi = 0;
	log_info("AWDL address %s on channel %d", ether_ntoa(&state.io.if_ether_addr), channel);
	if (state.io.host_ifindex)
		log_info("host interface %s, index %d", state.io.host_ifname,
			 state.io.host_ifindex);

	struct ev_loop *loop = EV_DEFAULT;
	awdl_schedule(loop, &state);

	ev_timer status;
	status.data = &state;
	ev_timer_init(&status, print_status, STATUS_PERIOD_S, STATUS_PERIOD_S);
	ev_timer_start(loop, &status);

	ev_run(loop, 0);

	awdl_free(&state);
	return EXIT_SUCCESS;
}
