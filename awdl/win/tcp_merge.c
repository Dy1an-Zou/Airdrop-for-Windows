/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tcp_merge.h"

#include <string.h>

#define IP6_HEADER_LEN 40
#define IP6_PAYLOAD_LEN_OFFSET 4
#define IP6_NEXT_HEADER_OFFSET 6
#define IP6_ADDRESSES_OFFSET 8
#define IP6_ADDRESSES_LEN 32
#define IP6_NEXT_HEADER_TCP 6

#define TCP_MIN_HEADER_LEN 20
#define TCP_PORTS_LEN 4
#define TCP_SEQUENCE_OFFSET 4
#define TCP_DATA_OFFSET_OFFSET 12
#define TCP_FLAGS_OFFSET 13
#define TCP_CHECKSUM_OFFSET 16
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_PSH 0x08
#define TCP_OPTION_END 0
#define TCP_OPTION_NOP 1
#define TCP_OPTION_TIMESTAMPS 8

static uint32_t read_be32(const uint8_t *bytes) {
	return (uint32_t) bytes[0] << 24 | (uint32_t) bytes[1] << 16 | (uint32_t) bytes[2] << 8 |
	       bytes[3];
}

static uint32_t sum_be16(const uint8_t *bytes, int length, uint32_t sum) {
	for (int i = 0; i + 1 < length; i += 2)
		sum += (uint32_t) (bytes[i] << 8 | bytes[i + 1]);
	if (length & 1)
		sum += (uint32_t) bytes[length - 1] << 8;
	return sum;
}

/* The checksum of the TCP segment in an IPv6 packet whose own checksum field
 * is counted as it stands: 0 for a segment that carries a correct one. */
static uint16_t tcp_checksum(const uint8_t *packet, int length) {
	int tcp_len = length - IP6_HEADER_LEN;
	uint32_t sum = sum_be16(packet + IP6_ADDRESSES_OFFSET, IP6_ADDRESSES_LEN, 0);
	sum += (uint32_t) tcp_len + IP6_NEXT_HEADER_TCP;
	sum = sum_be16(packet + IP6_HEADER_LEN, tcp_len, sum);
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (uint16_t) ~sum;
}

/* Returns the TCP header length of a segment that may be merged, 0 for any
 * other packet. */
static int mergeable_header_len(const uint8_t *packet, int length) {
	if (length < IP6_HEADER_LEN + TCP_MIN_HEADER_LEN || packet[0] >> 4 != 6 ||
	    packet[IP6_NEXT_HEADER_OFFSET] != IP6_NEXT_HEADER_TCP)
		return 0;
	const uint8_t *tcp = packet + IP6_HEADER_LEN;
	int payload_len = packet[IP6_PAYLOAD_LEN_OFFSET] << 8 | packet[IP6_PAYLOAD_LEN_OFFSET + 1];
	int header_len = (tcp[TCP_DATA_OFFSET_OFFSET] >> 4) * 4;
	if (header_len < TCP_MIN_HEADER_LEN || payload_len <= header_len ||
	    length != IP6_HEADER_LEN + payload_len ||
	    (tcp[TCP_FLAGS_OFFSET] | TCP_FLAG_PSH) != (TCP_FLAG_ACK | TCP_FLAG_PSH))
		return 0;
	for (int at = TCP_MIN_HEADER_LEN; at < header_len;) {
		uint8_t kind = tcp[at];
		if (kind == TCP_OPTION_END)
			break;
		if (kind == TCP_OPTION_NOP) {
			at++;
			continue;
		}
		if (kind != TCP_OPTION_TIMESTAMPS || at + 1 >= header_len || tcp[at + 1] < 2)
			return 0;
		at += tcp[at + 1];
	}
	return tcp_checksum(packet, length) == 0 ? header_len : 0;
}

int tcp_merge_offer(struct tcp_merge *merge, const uint8_t *packet, int length) {
	int header_len = mergeable_header_len(packet, length);
	if (header_len == 0)
		return 0;
	const uint8_t *tcp = packet + IP6_HEADER_LEN;
	int data_len = length - IP6_HEADER_LEN - header_len;
	if (merge->length == 0) {
		memcpy(merge->packet, packet, (size_t) length);
		merge->length = length;
		merge->segments = 1;
		merge->closed = tcp[TCP_FLAGS_OFFSET] & TCP_FLAG_PSH;
		return 1;
	}

	uint8_t *held_tcp = merge->packet + IP6_HEADER_LEN;
	int held_data_len = merge->length - IP6_HEADER_LEN - header_len;
	/* The same connection and traffic class, the same header layout, and
	 * data that starts where the held data ends. */
	if (merge->closed || merge->segments >= TCP_MERGE_MAX_SEGMENTS ||
	    merge->length + data_len > TCP_MERGE_MAX_PACKET ||
	    (held_tcp[TCP_DATA_OFFSET_OFFSET] >> 4) * 4 != header_len ||
	    memcmp(merge->packet, packet, IP6_PAYLOAD_LEN_OFFSET) != 0 ||
	    memcmp(merge->packet + IP6_NEXT_HEADER_OFFSET, packet + IP6_NEXT_HEADER_OFFSET,
		   IP6_HEADER_LEN - IP6_NEXT_HEADER_OFFSET) != 0 ||
	    memcmp(held_tcp, tcp, TCP_PORTS_LEN) != 0 ||
	    read_be32(tcp + TCP_SEQUENCE_OFFSET) !=
		    read_be32(held_tcp + TCP_SEQUENCE_OFFSET) + (uint32_t) held_data_len)
		return 0;

	/* The newest header speaks for all of it, but the data starts where the
	 * first segment did. */
	uint8_t first_sequence[4];
	memcpy(first_sequence, held_tcp + TCP_SEQUENCE_OFFSET, sizeof(first_sequence));
	memcpy(held_tcp, tcp, (size_t) header_len);
	memcpy(held_tcp + TCP_SEQUENCE_OFFSET, first_sequence, sizeof(first_sequence));
	memcpy(merge->packet + merge->length, tcp + header_len, (size_t) data_len);
	merge->length += data_len;
	merge->segments++;
	merge->closed = tcp[TCP_FLAGS_OFFSET] & TCP_FLAG_PSH;
	return 1;
}

int tcp_merge_take(struct tcp_merge *merge, uint8_t *out) {
	int length = merge->length;
	if (length == 0)
		return 0;
	uint8_t *tcp = merge->packet + IP6_HEADER_LEN;
	int payload_len = length - IP6_HEADER_LEN;
	merge->packet[IP6_PAYLOAD_LEN_OFFSET] = (uint8_t) (payload_len >> 8);
	merge->packet[IP6_PAYLOAD_LEN_OFFSET + 1] = (uint8_t) payload_len;
	tcp[TCP_CHECKSUM_OFFSET] = 0;
	tcp[TCP_CHECKSUM_OFFSET + 1] = 0;
	uint16_t checksum = tcp_checksum(merge->packet, length);
	tcp[TCP_CHECKSUM_OFFSET] = (uint8_t) (checksum >> 8);
	tcp[TCP_CHECKSUM_OFFSET + 1] = (uint8_t) checksum;
	memcpy(out, merge->packet, (size_t) length);
	merge->length = 0;
	merge->segments = 0;
	merge->closed = 0;
	return length;
}
