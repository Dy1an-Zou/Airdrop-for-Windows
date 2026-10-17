/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What -r leaves of the lines awdld logs. */
#include <stdio.h>
#include <string.h>

#include "redact.h"

static int failures;

static void expect_line(struct redactor *redactor, const char *line, const char *shown,
			const char *what) {
	char out[256];
	redact_line(redactor, line, out, sizeof(out));
	int same = strcmp(out, shown) == 0;
	printf("%s  %s\n", same ? "ok  " : "FAIL", what);
	if (!same) {
		printf("      got  %s\n      want %s\n", out, shown);
		failures++;
	}
}

int main(void) {
	struct redactor redactor = {0};

	expect_line(&redactor, "AWDL address be:dd:eb:cc:cc:72 on channel 6",
		    "AWDL address mac-1 on channel 6", "a MAC address becomes a number");
	expect_line(&redactor, "add peer 52:41:fc:0e:aa:43 (Someone's MacBook)",
		    "add peer mac-2 (hidden)", "a peer's name is hidden");
	expect_line(&redactor, "remove peer 52:41:fc:0e:aa:43 (odd) name (x)",
		    "remove peer mac-2 (hidden)", "a name is hidden up to the last bracket");
	expect_line(&redactor, "add peer 9e:9a:fc:3d:e8:52 ()", "add peer mac-3 ()",
		    "an empty name stays empty");
	expect_line(&redactor,
		    "peer 52:41:fc:0e:aa:43 is reachable at fe80::5041:fcff:fe0e:aa43%10",
		    "peer mac-2 is reachable at (hidden)",
		    "a link-local address is hidden with its scope");
	expect_line(&redactor,
		    "peers 1 | rx action 11, data 0 | sync tree be:dd:eb:cc:cc:72 -> "
		    "52:41:fc:0e:aa:43 --> 9e:9a:fc:3d:e8:52 (met 65, ctr 403)",
		    "peers 1 | rx action 11, data 0 | sync tree mac-1 -> mac-2 --> mac-3 "
		    "(met 65, ctr 403)",
		    "a device keeps its number and the tree keeps its counters");
	expect_line(&redactor, "peer 2:0:5e:0:53:1 left", "peer mac-4 left",
		    "a MAC address without leading zeros is recognised");
	expect_line(&redactor, "slot 12:34:56 of 16, channel 6:44:149",
		    "slot 12:34:56 of 16, channel 6:44:149", "shorter colon runs are left alone");
	expect_line(&redactor, "host interface awdl0, index 10", "host interface awdl0, index 10",
		    "a line without identities is unchanged");

	char small[12];
	redact_line(&redactor, "add peer 52:41:fc:0e:aa:43 (name)", small, sizeof(small));
	int cut = strcmp(small, "add peer ma") == 0;
	printf("%s  %s\n", cut ? "ok  " : "FAIL", "a short buffer cuts the line, not memory");
	failures += !cut;

	struct redactor full = {.count = REDACT_MAX_DEVICES};
	expect_line(&full, "add peer 52:41:fc:0e:aa:43 ()", "add peer mac-? ()",
		    "past the table's size a device gets no number");

	return failures ? 1 : 0;
}
