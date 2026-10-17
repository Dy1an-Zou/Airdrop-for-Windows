/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Apple devices publish their DNS-SD records inside AWDL action frames, not
 * in multicast DNS packets, which is where a program on Windows looks for
 * them. This makes the packet such a device would have sent. */
#pragma once
#include <stdint.h>

/* The largest packet service_bridge_packet writes. */
#define SERVICE_BRIDGE_MAX_PACKET 1280

/* Takes a received frame, radiotap header first. If it is an AWDL action
 * frame with AirDrop records, writes a multicast DNS response from its
 * sender as an IPv6 packet of at most SERVICE_BRIDGE_MAX_PACKET bytes and
 * returns the length; otherwise returns 0. The sender's address is added for
 * each host a record names, since the frame does not carry it. */
int service_bridge_packet(const uint8_t *frame, int length, uint8_t *packet);
