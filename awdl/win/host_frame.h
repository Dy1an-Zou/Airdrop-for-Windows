/* SPDX-License-Identifier: GPL-3.0-or-later */
/* OWL's core exchanges Ethernet frames with the host, while the Windows
 * adapter carries bare IPv6 packets. These convert between the two. */
#pragma once
#include <stdint.h>

#define HOST_FRAME_HEADER_LEN 14

/* Wraps an IPv6 packet in an Ethernet header, taking the destination MAC from
 * its address. Returns the length, or -1 if that fails or it is too big. */
int host_frame_from_ip6(const uint8_t *packet, int packet_len, const uint8_t self[6],
			uint8_t *frame, int frame_cap);

/* Returns the IPv6 packet inside an Ethernet frame, as long as its own header
 * says and without what follows it. NULL when the frame carries another
 * protocol or less than that. */
const uint8_t *host_frame_ip6(const uint8_t *frame, int frame_len, int *packet_len);
