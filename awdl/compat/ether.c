/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <netinet/ether.h>

#include <stdio.h>

char *ether_ntoa(const struct ether_addr *addr) {
	static char text[18];
	const uint8_t *o = addr->ether_addr_octet;
	snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", o[0], o[1], o[2], o[3], o[4],
		 o[5]);
	return text;
}

struct ether_addr *ether_aton(const char *text) {
	static struct ether_addr addr;
	unsigned int o[ETHER_ADDR_LEN];
	char trailing;
	if (sscanf(text, "%x:%x:%x:%x:%x:%x%c", &o[0], &o[1], &o[2], &o[3], &o[4], &o[5],
		   &trailing) != ETHER_ADDR_LEN)
		return NULL;
	for (int i = 0; i < ETHER_ADDR_LEN; i++) {
		if (o[i] > 0xFF)
			return NULL;
		addr.ether_addr_octet[i] = (uint8_t) o[i];
	}
	return &addr;
}
