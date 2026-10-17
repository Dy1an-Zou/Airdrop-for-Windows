/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Ethernet framing of host IPv6 packets, with no adapter or radio involved. */
#include <stdio.h>
#include <string.h>

#include "host_frame.h"

#define IP6_HEADER_LEN 40
#define IP6_DST_OFFSET 24
#define FRAME_CAP 1518

static const uint8_t self[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
static const uint8_t mdns_group[16] = {0xff, 0x02, [15] = 0xfb};
/* fe80::5000:5eff:fe00:5301, the address of a peer with MAC 52:00:5e:00:53:01. */
static const uint8_t peer[16] = {0xfe, 0x80, 0,	   0,	 0,    0,    0,	   0,
				 0x50, 0x00, 0x5e, 0xff, 0xfe, 0x00, 0x53, 0x01};
static const uint8_t random_link_local[16] = {0xfe, 0x80, 0,	0,    0,    0,	  0,	0,
					      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
static const uint8_t global[16] = {0x20, 0x01, 0x0d, 0xb8, [15] = 0x01};

static int failures;

static void expect(int condition, const char *what) {
	printf("%s  %s\n", condition ? "ok  " : "FAIL", what);
	if (!condition)
		failures++;
}

static int frame_to(const uint8_t destination[16], int packet_len, uint8_t *frame, int frame_cap) {
	uint8_t packet[FRAME_CAP] = {0x60};
	packet[4] = (uint8_t) ((packet_len - IP6_HEADER_LEN) >> 8);
	packet[5] = (uint8_t) (packet_len - IP6_HEADER_LEN);
	for (int i = IP6_HEADER_LEN; i < packet_len; i++)
		packet[i] = (uint8_t) i;
	memcpy(packet + IP6_DST_OFFSET, destination, 16);
	return host_frame_from_ip6(packet, packet_len, self, frame, frame_cap);
}

int main(void) {
	uint8_t frame[FRAME_CAP];
	const uint8_t mdns_mac[6] = {0x33, 0x33, 0x00, 0x00, 0x00, 0xfb};
	const uint8_t peer_mac[6] = {0x52, 0x00, 0x5e, 0x00, 0x53, 0x01};

	int length = frame_to(mdns_group, 60, frame, sizeof(frame));
	expect(length == 60 + HOST_FRAME_HEADER_LEN, "the frame grows by its header");
	expect(memcmp(frame, mdns_mac, 6) == 0, "multicast maps to 33:33 and four bytes");
	expect(memcmp(frame + 6, self, 6) == 0, "the source is our own MAC");
	expect(frame[12] == 0x86 && frame[13] == 0xDD, "the ethertype is IPv6");
	expect(frame[HOST_FRAME_HEADER_LEN] == 0x60 && frame[HOST_FRAME_HEADER_LEN + 59] == 59,
	       "the packet follows unchanged");

	frame_to(peer, IP6_HEADER_LEN, frame, sizeof(frame));
	expect(memcmp(frame, peer_mac, 6) == 0, "a peer address maps back to its MAC");

	expect(frame_to(random_link_local, 60, frame, sizeof(frame)) < 0,
	       "a link-local address not built from a MAC is refused");
	expect(frame_to(global, 60, frame, sizeof(frame)) < 0, "a routable address is refused");
	expect(frame_to(peer, IP6_HEADER_LEN - 1, frame, sizeof(frame)) < 0,
	       "a truncated header is refused");
	expect(frame_to(peer, 60, frame, 60 + HOST_FRAME_HEADER_LEN - 1) < 0,
	       "a packet that does not fit is refused");
	const uint8_t ip4[IP6_HEADER_LEN] = {0x45};
	expect(host_frame_from_ip6(ip4, sizeof(ip4), self, frame, sizeof(frame)) < 0,
	       "an IPv4 packet is refused");

	length = frame_to(peer, 60, frame, sizeof(frame));
	int packet_len = 0;
	const uint8_t *packet = host_frame_ip6(frame, length, &packet_len);
	expect(packet == frame + HOST_FRAME_HEADER_LEN && packet_len == 60,
	       "the packet comes back out of its frame");
	expect(host_frame_ip6(frame, length + 68, &packet_len) && packet_len == 60,
	       "bytes after the packet are left out");
	expect(!host_frame_ip6(frame, length - 1, &packet_len),
	       "a frame that ends before its packet yields none");
	expect(!host_frame_ip6(frame, HOST_FRAME_HEADER_LEN, &packet_len),
	       "a bare header yields no packet");
	frame[12] = 0x08;
	frame[13] = 0x00;
	expect(!host_frame_ip6(frame, length, &packet_len),
	       "a frame of another protocol yields no packet");

	return failures ? 1 : 0;
}
