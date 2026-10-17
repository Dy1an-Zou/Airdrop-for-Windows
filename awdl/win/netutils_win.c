/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Windows replacement for OWL's daemon/netutils.c: the radio daemon owns the
 * card, so what nl80211 did is a pipe message or nothing. */
#include "netutils.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <errno.h>
#include <string.h>

#include "log.h"
#include "radio_client.h"
#include "radio_link.h"
#include "win_internal.h"

int netutils_init() {
	return 0;
}

void netutils_cleanup() {
}

int set_monitor_mode(int ifindex) {
	(void) ifindex;
	return 0;
}

int is_channel_available(int ifindex, int channel, bool *is_available) {
	(void) ifindex;
	(void) channel;
	*is_available = true;
	return 0;
}

int set_channel(int ifindex, int channel) {
	(void) ifindex;
	uint8_t number = (uint8_t) channel;
	struct radio_client *radio = winawdl_radio();
	if (!radio)
		return -ENODEV;
	return radio_client_send(radio, RADIO_LINK_SET_CHANNEL, &number, 1);
}

int link_up(int ifindex) {
	(void) ifindex;
	return 0;
}

int link_down(int ifindex) {
	(void) ifindex;
	return 0;
}

int link_ether_addr_get(const char *ifname, struct ether_addr *addr) {
	(void) ifname;
	(void) addr;
	return -ENOTSUP;
}

int get_hostname(char *name, size_t len) {
	DWORD size = (DWORD) len;
	return GetComputerNameA(name, &size) ? 0 : -EIO;
}

/* The host adapter carries bare IP packets, so Windows never resolves a
 * neighbor on it: peer MACs are read back out of the destination address. */
int neighbor_add(int ifindex, const struct ether_addr *eth, const struct in6_addr *in6) {
	(void) ifindex;
	(void) eth;
	(void) in6;
	return 0;
}

int neighbor_remove(int ifindex, const struct in6_addr *in6) {
	(void) ifindex;
	(void) in6;
	return 0;
}

int neighbor_add_rfc4291(int ifindex, const struct ether_addr *eth) {
	if (ifindex == 0)
		return 0;
	struct in6_addr in6;
	rfc4291_addr(eth, &in6);
	const uint8_t *id = &in6.s6_addr[8];
	log_info("peer %s is reachable at fe80::%x:%x:%x:%x%%%d", ether_ntoa(eth),
		 id[0] << 8 | id[1], id[2] << 8 | id[3], id[4] << 8 | id[5], id[6] << 8 | id[7],
		 ifindex);
	return 0;
}

int neighbor_remove_rfc4291(int ifindex, const struct ether_addr *eth) {
	(void) ifindex;
	(void) eth;
	return 0;
}

/* Link-local address from a MAC: fe80::/64 plus the modified EUI-64. */
void rfc4291_addr(const struct ether_addr *eth, struct in6_addr *in6) {
	const uint8_t *mac = eth->ether_addr_octet;
	uint8_t *out = in6->s6_addr;
	memset(out, 0, 16);
	out[0] = 0xfe;
	out[1] = 0x80;
	out[8] = mac[0] ^ 0x02;
	out[9] = mac[1];
	out[10] = mac[2];
	out[11] = 0xff;
	out[12] = 0xfe;
	out[13] = mac[3];
	out[14] = mac[4];
	out[15] = mac[5];
}
