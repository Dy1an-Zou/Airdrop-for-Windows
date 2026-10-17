/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_frame.h"

#include <string.h>

#define MAC_LEN 6
#define IP6_HEADER_LEN 40
#define IP6_PAYLOAD_LEN_OFFSET 4
#define IP6_DST_OFFSET 24
#define IP6_VERSION 6

static const uint8_t ethertype_ip6[2] = {0x86, 0xDD};
static const uint8_t link_local_prefix[8] = {0xfe, 0x80};

static int mac_for_destination(const uint8_t *ip6, uint8_t *mac) {
	if (ip6[0] == 0xff) {
		/* RFC 2464: 33:33 followed by the last four address bytes. */
		mac[0] = mac[1] = 0x33;
		memcpy(mac + 2, ip6 + 12, 4);
		return 0;
	}
	if (memcmp(ip6, link_local_prefix, sizeof(link_local_prefix)) != 0 || ip6[11] != 0xff ||
	    ip6[12] != 0xfe)
		return -1;
	mac[0] = ip6[8] ^ 0x02;
	mac[1] = ip6[9];
	mac[2] = ip6[10];
	memcpy(mac + 3, ip6 + 13, 3);
	return 0;
}

int host_frame_from_ip6(const uint8_t *packet, int packet_len, const uint8_t self[6],
			uint8_t *frame, int frame_cap) {
	if (packet_len < IP6_HEADER_LEN || packet[0] >> 4 != IP6_VERSION ||
	    packet_len > frame_cap - HOST_FRAME_HEADER_LEN)
		return -1;
	if (mac_for_destination(packet + IP6_DST_OFFSET, frame) < 0)
		return -1;
	memcpy(frame + MAC_LEN, self, MAC_LEN);
	memcpy(frame + 2 * MAC_LEN, ethertype_ip6, sizeof(ethertype_ip6));
	memcpy(frame + HOST_FRAME_HEADER_LEN, packet, (size_t) packet_len);
	return HOST_FRAME_HEADER_LEN + packet_len;
}

const uint8_t *host_frame_ip6(const uint8_t *frame, int frame_len, int *packet_len) {
	if (frame_len < HOST_FRAME_HEADER_LEN + IP6_HEADER_LEN ||
	    memcmp(frame + 2 * MAC_LEN, ethertype_ip6, sizeof(ethertype_ip6)) != 0)
		return NULL;
	const uint8_t *packet = frame + HOST_FRAME_HEADER_LEN;
	/* OWL hands over its whole buffer, whatever the packet's length. */
	int length = IP6_HEADER_LEN +
		     (packet[IP6_PAYLOAD_LEN_OFFSET] << 8 | packet[IP6_PAYLOAD_LEN_OFFSET + 1]);
	if (length > frame_len - HOST_FRAME_HEADER_LEN)
		return NULL;
	*packet_len = length;
	return packet;
}
