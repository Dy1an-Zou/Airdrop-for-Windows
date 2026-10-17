/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Joins consecutive TCP segments of one connection into a single IPv6 packet
 * before the host sees them, as network cards do on receive. Windows
 * acknowledges every second packet, and over AWDL each acknowledgement costs
 * the channel about as much as the data it answers. */
#pragma once
#include <stdint.h>

#define TCP_MERGE_MAX_PACKET 0xFFFF
/* No more segments than fit a block-ack window. */
#define TCP_MERGE_MAX_SEGMENTS 64

struct tcp_merge {
	uint8_t packet[TCP_MERGE_MAX_PACKET];
	int length;   /* 0: nothing held */
	int segments; /* how many segments the held packet is made of */
	int closed;   /* its last segment asked to be delivered at once */
};

/* Offers an IPv6 packet. Returns 1 when it was taken: it starts the held
 * packet or continues it. Returns 0 when it cannot be, and the caller sends
 * what is held, then this packet. Only data segments with a valid checksum,
 * no flag but ACK and PSH and no option but timestamps are taken. */
int tcp_merge_offer(struct tcp_merge *merge, const uint8_t *packet, int length);

/* Copies the held packet to `out` (TCP_MERGE_MAX_PACKET bytes) with its
 * lengths and checksum made good, and holds nothing afterwards. Returns its
 * length, 0 when nothing was held. */
int tcp_merge_take(struct tcp_merge *merge, uint8_t *out);
