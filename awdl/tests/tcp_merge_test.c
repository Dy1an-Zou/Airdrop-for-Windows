/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Joining TCP segments into one packet, and what is left apart. */
#include <stdio.h>
#include <string.h>

#include "tcp_merge.h"

#define IP6_HEADER_LEN 40
#define TCP_HEADER_LEN 32
#define DATA_OFFSET (IP6_HEADER_LEN + TCP_HEADER_LEN)
#define TCP_ACK 0x10
#define TCP_PSH 0x08
#define TCP_FIN 0x01
#define SEGMENT_DATA 1378

static int failures;

static void expect(int condition, const char *what) {
	printf("%s  %s\n", condition ? "ok  " : "FAIL", what);
	if (!condition)
		failures++;
}

/* The Internet checksum of a TCP segment over IPv6, worked out separately
 * from the code under test: 0 when the segment's own field is right. */
static uint16_t reference_checksum(const uint8_t *packet, int length) {
	uint32_t sum = (uint32_t) (length - IP6_HEADER_LEN) + 6;
	for (int i = 8; i < length; i += 2)
		sum += (uint32_t) packet[i] << 8 | (i + 1 < length ? packet[i + 1] : 0);
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (uint16_t) ~sum;
}

/* A data segment to port 8771 with timestamps; returns its length. Every
 * data byte is `fill`. */
static int segment(uint8_t *packet, uint32_t sequence, uint32_t ack, uint8_t flags, int data_len,
		   uint8_t fill) {
	int length = DATA_OFFSET + data_len;
	memset(packet, 0, (size_t) length);
	packet[0] = 0x60;
	packet[4] = (uint8_t) ((TCP_HEADER_LEN + data_len) >> 8);
	packet[5] = (uint8_t) (TCP_HEADER_LEN + data_len);
	packet[6] = 6;
	packet[7] = 64;
	packet[8] = 0xFE;
	packet[9] = 0x80;
	packet[23] = 1;
	packet[24] = 0xFE;
	packet[25] = 0x80;
	packet[39] = 2;
	uint8_t *tcp = packet + IP6_HEADER_LEN;
	tcp[0] = 0xC0;
	tcp[1] = 0x01;
	tcp[2] = 0x22;
	tcp[3] = 0x43;
	for (int i = 0; i < 4; i++) {
		tcp[4 + i] = (uint8_t) (sequence >> (24 - 8 * i));
		tcp[8 + i] = (uint8_t) (ack >> (24 - 8 * i));
	}
	tcp[12] = TCP_HEADER_LEN / 4 << 4;
	tcp[13] = flags;
	tcp[14] = 0x07;
	tcp[15] = (uint8_t) fill;
	tcp[20] = 1;
	tcp[21] = 1;
	tcp[22] = 8;
	tcp[23] = 10;
	tcp[27] = fill;
	memset(packet + DATA_OFFSET, fill, (size_t) data_len);
	uint16_t checksum = reference_checksum(packet, length);
	tcp[16] = (uint8_t) (checksum >> 8);
	tcp[17] = (uint8_t) checksum;
	return length;
}

static struct tcp_merge merge;
static uint8_t out[TCP_MERGE_MAX_PACKET];

int main(void) {
	uint8_t first[2048], second[2048], third[2048], other[2048];
	int first_len = segment(first, 1000, 500, TCP_ACK, SEGMENT_DATA, 0xA1);
	int second_len = segment(second, 1000 + SEGMENT_DATA, 500, TCP_ACK, SEGMENT_DATA, 0xB2);
	int third_len = segment(third, 1000 + 2 * SEGMENT_DATA, 501, TCP_ACK | TCP_PSH, 100, 0xC3);

	expect(tcp_merge_take(&merge, out) == 0, "nothing held gives nothing");
	expect(tcp_merge_offer(&merge, first, first_len) && merge.segments == 1,
	       "a data segment starts a packet");
	int alone = tcp_merge_take(&merge, out);
	expect(alone == first_len && memcmp(out, first, (size_t) first_len) == 0,
	       "taken alone it comes back as it was");

	expect(tcp_merge_offer(&merge, first, first_len) &&
		       tcp_merge_offer(&merge, second, second_len) &&
		       tcp_merge_offer(&merge, third, third_len) && merge.segments == 3,
	       "segments that follow one another are joined");
	int joined = tcp_merge_take(&merge, out);
	int data_len = 2 * SEGMENT_DATA + 100;
	expect(joined == DATA_OFFSET + data_len &&
		       (out[4] << 8 | out[5]) == TCP_HEADER_LEN + data_len,
	       "the joined packet has the length of all their data");
	expect(reference_checksum(out, joined) == 0, "and a checksum that holds");
	const uint8_t *tcp = out + IP6_HEADER_LEN;
	expect(tcp[4] == 0 && tcp[5] == 0 && tcp[6] == 0x03 && tcp[7] == 0xE8,
	       "its data starts where the first segment's did");
	expect(tcp[11] == 0xF5 && tcp[13] == (TCP_ACK | TCP_PSH) && tcp[15] == 0xC3 &&
		       tcp[27] == 0xC3,
	       "its acknowledgement, flags, window and timestamp are the last one's");
	expect(out[DATA_OFFSET] == 0xA1 && out[DATA_OFFSET + SEGMENT_DATA] == 0xB2 &&
		       out[DATA_OFFSET + 2 * SEGMENT_DATA] == 0xC3 && out[joined - 1] == 0xC3,
	       "the data is in order");
	expect(merge.length == 0, "nothing is held afterwards");

	tcp_merge_offer(&merge, first, first_len);
	expect(!tcp_merge_offer(&merge, third, third_len), "a segment after a gap is not joined");
	expect(!tcp_merge_offer(&merge, first, first_len), "nor is the same segment again");
	int other_len = segment(other, 1000 + SEGMENT_DATA, 500, TCP_ACK, SEGMENT_DATA, 0xD4);
	other[IP6_HEADER_LEN + 1] ^= 1;
	other[IP6_HEADER_LEN + 16] = 0;
	other[IP6_HEADER_LEN + 17] = 0;
	uint16_t checksum = reference_checksum(other, other_len);
	other[IP6_HEADER_LEN + 16] = (uint8_t) (checksum >> 8);
	other[IP6_HEADER_LEN + 17] = (uint8_t) checksum;
	expect(!tcp_merge_offer(&merge, other, other_len) && merge.segments == 1,
	       "nor is a segment of another connection");
	tcp_merge_take(&merge, out);

	tcp_merge_offer(&merge, third, third_len);
	int fourth_len = segment(other, 1000 + 2 * SEGMENT_DATA + 100, 501, TCP_ACK, 50, 0xE5);
	expect(!tcp_merge_offer(&merge, other, fourth_len),
	       "nothing is joined after a segment that asked for delivery");
	tcp_merge_take(&merge, out);

	int bare_len = segment(other, 1000, 500, TCP_ACK, 0, 0);
	expect(!tcp_merge_offer(&merge, other, bare_len),
	       "an acknowledgement without data is left");
	int fin_len = segment(other, 1000, 500, TCP_ACK | TCP_FIN, 10, 1);
	expect(!tcp_merge_offer(&merge, other, fin_len), "so is a segment that closes");
	second[DATA_OFFSET + 5] ^= 0xFF;
	expect(!tcp_merge_offer(&merge, second, second_len),
	       "so is a segment whose checksum is wrong");
	second[6] = 17;
	expect(!tcp_merge_offer(&merge, second, second_len) && merge.length == 0,
	       "and anything that is not TCP");

	uint32_t sequence = 7;
	int taken = 0;
	for (int i = 0; i <= TCP_MERGE_MAX_SEGMENTS; i++, sequence += 10) {
		int length = segment(other, sequence, 1, TCP_ACK, 10, (uint8_t) i);
		taken += tcp_merge_offer(&merge, other, length);
	}
	expect(taken == TCP_MERGE_MAX_SEGMENTS, "a packet stops growing at 64 segments");

	return failures ? 1 : 0;
}
