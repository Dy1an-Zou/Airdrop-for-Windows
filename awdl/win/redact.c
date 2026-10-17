/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "redact.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define HIDDEN "(hidden)"
#define LINK_LOCAL "fe80:"

static int is_address_char(char c) {
	return isxdigit((unsigned char) c) || c == ':';
}

/* Returns the length of the MAC address that starts the text, or 0. */
static size_t parse_mac(const char *text, uint8_t mac[6]) {
	const char *cur = text;
	for (int i = 0; i < 6; i++) {
		int digits = 0;
		unsigned value = 0;
		for (; digits < 2 && isxdigit((unsigned char) *cur); digits++, cur++)
			value = value * 16 +
				(isdigit((unsigned char) *cur)
					 ? (unsigned) (*cur - '0')
					 : (unsigned) (tolower((unsigned char) *cur) - 'a' + 10));
		if (digits == 0)
			return 0;
		mac[i] = (uint8_t) value;
		if (i < 5 && *cur++ != ':')
			return 0;
	}
	return is_address_char(*cur) ? 0 : (size_t) (cur - text);
}

/* Returns the number of a device, or 0 once the table is full. */
static int device_number(struct redactor *redactor, const uint8_t mac[6]) {
	for (int i = 0; i < redactor->count; i++)
		if (memcmp(redactor->seen[i], mac, 6) == 0)
			return i + 1;
	if (redactor->count >= REDACT_MAX_DEVICES)
		return 0;
	memcpy(redactor->seen[redactor->count], mac, 6);
	return ++redactor->count;
}

/* OWL ends its sync tree with "(met N, ctr N)", which is not a name. */
static int is_tree_counters(const char *text, size_t length) {
	unsigned metric, counter;
	int used = 0;
	return sscanf(text, "met %u, ctr %u%n", &metric, &counter, &used) == 2 &&
	       (size_t) used == length;
}

static size_t append(char *out, size_t used, size_t cap, const char *text) {
	int written = snprintf(out + used, cap - used, "%s", text);
	return used + (size_t) written < cap ? used + (size_t) written : cap - 1;
}

void redact_line(struct redactor *redactor, const char *line, char *out, size_t cap) {
	if (cap == 0)
		return;
	size_t used = 0;
	const char *cur = line;
	while (*cur && used + 1 < cap) {
		int starts_token =
			cur == line || !(isalnum((unsigned char) cur[-1]) || cur[-1] == ':');
		uint8_t mac[6];
		size_t length;
		if (starts_token && strncasecmp(cur, LINK_LOCAL, strlen(LINK_LOCAL)) == 0) {
			while (is_address_char(*cur))
				cur++;
			if (*cur == '%')
				for (cur++; isdigit((unsigned char) *cur); cur++)
					;
			used = append(out, used, cap, HIDDEN);
		} else if (starts_token && (length = parse_mac(cur, mac)) > 0) {
			char label[16];
			int number = device_number(redactor, mac);
			if (number)
				snprintf(label, sizeof(label), "mac-%d", number);
			else
				snprintf(label, sizeof(label), "mac-?");
			used = append(out, used, cap, label);
			cur += length;
			/* OWL prints a peer's name in brackets after its address. */
			const char *close = strrchr(cur, ')');
			if (cur[0] == ' ' && cur[1] == '(' && close && close > cur + 2 &&
			    !is_tree_counters(cur + 2, (size_t) (close - (cur + 2)))) {
				used = append(out, used, cap, " " HIDDEN);
				cur = close + 1;
			}
		} else {
			out[used++] = *cur++;
		}
	}
	out[used] = '\0';
}
