/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Takes the identities of nearby devices out of log lines, for a recording or
 * a shared screen. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define REDACT_MAX_DEVICES 256

/* The devices numbered so far, in order of first appearance. */
struct redactor {
	uint8_t seen[REDACT_MAX_DEVICES][6];
	int count;
};

/* Copies a line with each MAC address replaced by mac-N, and each link-local
 * address and peer name by (hidden). */
void redact_line(struct redactor *redactor, const char *line, char *out, size_t cap);
