/* SPDX-License-Identifier: GPL-3.0-or-later */
/* AirDrop records in an AWDL action frame become a multicast DNS response. */
#include <stdio.h>
#include <string.h>

#include "service_bridge.h"

#define RADIOTAP_LEN 10
#define BODY_OFFSET (RADIOTAP_LEN + 24)
#define FCS_LEN 4
#define IP6_HEADER_LEN 40
#define UDP_HEADER_LEN 8
#define DNS_OFFSET (IP6_HEADER_LEN + UDP_HEADER_LEN)

static int failures;

static void expect(int condition, const char *what) {
	printf("%s  %s\n", condition ? "ok  " : "FAIL", what);
	if (!condition)
		failures++;
}

/* Radiotap header as the radio daemon writes it, an action frame from
 * 52:00:00:AA:BB:CC and the AWDL fixed fields. Returns the length so far. */
static int start_frame(uint8_t *frame) {
	static const uint8_t head[] = {
		0,    0, RADIOTAP_LEN, 0, 0x22, 0, 0, 0, 0x10, 0xD8, /* radiotap */
		0xD0, 0,					     /* action frame */
	};
	static const uint8_t sender[] = {0x52, 0x00, 0x00, 0xAA, 0xBB, 0xCC};
	static const uint8_t awdl[] = {0x7F, 0x00, 0x17, 0xF2, 8, 0x10, 3, 0,
				       0,    0,	   0,	 0,    0, 0,	0, 0};
	memset(frame, 0, BODY_OFFSET);
	memcpy(frame, head, sizeof(head));
	memcpy(frame + RADIOTAP_LEN + 10, sender, sizeof(sender));
	memcpy(frame + BODY_OFFSET, awdl, sizeof(awdl));
	return BODY_OFFSET + sizeof(awdl);
}

/* Appends a service response TLV: `name` and `data` in the frame's form. */
static int add_response(uint8_t *frame, int length, const char *name, int name_len, int type,
			const char *data, int data_len) {
	int body = 2 + name_len + 1 + 4 + data_len;
	uint8_t *at = frame + length;
	*at++ = 2;
	*at++ = (uint8_t) body;
	*at++ = (uint8_t) (body >> 8);
	*at++ = (uint8_t) (name_len + 1);
	*at++ = 0;
	memcpy(at, name, (size_t) name_len);
	at += name_len;
	*at++ = (uint8_t) type;
	*at++ = (uint8_t) data_len;
	*at++ = 0;
	*at++ = 0;
	*at++ = 0;
	memcpy(at, data, (size_t) data_len);
	return length + 3 + body;
}

static int end_frame(uint8_t *frame, int length) {
	memset(frame + length, 0xEE, FCS_LEN);
	return length + FCS_LEN;
}

static int contains(const uint8_t *haystack, int length, const char *needle, int needle_len) {
	for (int i = 0; i + needle_len <= length; i++)
		if (memcmp(haystack + i, needle, (size_t) needle_len) == 0)
			return 1;
	return 0;
}

static unsigned udp_sum(const uint8_t *packet, int length) {
	unsigned long sum = (unsigned long) (length - IP6_HEADER_LEN) + 17;
	for (int i = 8; i + 1 < length; i += 2)
		sum += (unsigned long) (packet[i] << 8 | packet[i + 1]);
	if (length & 1)
		sum += (unsigned long) packet[length - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (unsigned) sum;
}

#define INSTANCE                                                                                   \
	"\x0c"                                                                                     \
	"0a1b2c3d4e5f"
#define INSTANCE_LEN 13
#define AIRDROP_ENDING "\xC0\x07"
#define OWN_NAME_ENDING "\xC0\x00"
#define FULL_TYPE "\x08_airdrop\x04_tcp\x05local"
#define FULL_TYPE_LEN ((int) sizeof(FULL_TYPE) - 1)

int main(void) {
	static const char host[] = "\x04host\xC0\x0C";
	static const char service[] = "\x00\x00\x00\x00\x22\x42\x04host\xC0\x0C";
	static const char text[] = "\x09"
				   "flags=507";
	static const char source[] = {(char) 0xFE, (char) 0x80, 0,	     0,
				      0,	   0,		0,	     0,
				      0x50,	   0x00,	0,	     (char) 0xFF,
				      (char) 0xFE, (char) 0xAA, (char) 0xBB, (char) 0xCC};
	uint8_t frame[1024];
	uint8_t packet[SERVICE_BRIDGE_MAX_PACKET];

	int length = start_frame(frame);
	length = add_response(frame, length, AIRDROP_ENDING, 2, 12, INSTANCE OWN_NAME_ENDING,
			      INSTANCE_LEN + 2);
	length = add_response(frame, length, INSTANCE AIRDROP_ENDING, INSTANCE_LEN + 2, 33, service,
			      sizeof(service) - 1);
	length = add_response(frame, length, INSTANCE AIRDROP_ENDING, INSTANCE_LEN + 2, 16, text,
			      sizeof(text) - 1);
	length = add_response(frame, length, "\x05other\xC0\x0A", 8, 16, text, sizeof(text) - 1);
	length = end_frame(frame, length);
	int packet_len = service_bridge_packet(frame, length, packet);
	const uint8_t *dns = packet + DNS_OFFSET;
	int dns_len = packet_len - DNS_OFFSET;

	expect(packet_len > DNS_OFFSET + 12, "AirDrop records make a packet");
	expect(packet[0] == 0x60 && packet[6] == 17 && packet[7] == 255 &&
		       (packet[4] << 8 | packet[5]) == packet_len - IP6_HEADER_LEN,
	       "it is a UDP packet that stays on the link");
	expect(memcmp(packet + 8, source, sizeof(source)) == 0,
	       "it comes from the address the sender derives from its MAC");
	expect(packet[24] == 0xFF && packet[25] == 0x02 && packet[39] == 0xFB &&
		       packet[40] == 0x14 && packet[41] == 0xE9 && packet[42] == 0x14 &&
		       packet[43] == 0xE9,
	       "it goes to the multicast DNS group and port");
	expect(udp_sum(packet, packet_len) == 0xFFFF, "its checksum holds");
	expect(dns[2] == 0x84 && dns[7] == 4,
	       "a response with four records: the foreign one is left out");
	expect(contains(dns, dns_len, INSTANCE FULL_TYPE, INSTANCE_LEN + FULL_TYPE_LEN + 1),
	       "names are written out in full, the pointer's target too");
	expect(contains(dns, dns_len, "\x22\x42\x04host\x05local", 13),
	       "the service record keeps its port and names its host");
	expect(contains(dns, dns_len, source, sizeof(source)),
	       "the host gets the sender's address");
	expect(contains(dns, dns_len, text, sizeof(text) - 1), "the text record is carried over");
	expect(!contains(dns, dns_len, "other", 5), "nothing of the foreign record is in it");

	length = start_frame(frame);
	length = add_response(frame, length, "\x05other\xC0\x0A", 8, 16, text, sizeof(text) - 1);
	length = end_frame(frame, length);
	expect(service_bridge_packet(frame, length, packet) == 0,
	       "a frame without AirDrop records makes none");

	length = start_frame(frame);
	length = add_response(frame, length, INSTANCE "\xC0\x63", INSTANCE_LEN + 2, 16, text,
			      sizeof(text) - 1);
	length = add_response(frame, length, INSTANCE AIRDROP_ENDING, INSTANCE_LEN + 2, 33, host,
			      sizeof(host) - 1);
	length = end_frame(frame, length);
	expect(service_bridge_packet(frame, length, packet) == 0,
	       "an unknown ending and a service record cut short are skipped");

	length = start_frame(frame);
	length = add_response(frame, length, AIRDROP_ENDING, 2, 12, INSTANCE OWN_NAME_ENDING,
			      INSTANCE_LEN + 2);
	length = end_frame(frame, length);
	frame[RADIOTAP_LEN] = 0x80;
	expect(service_bridge_packet(frame, length, packet) == 0,
	       "another kind of frame makes none");

	return failures ? 1 : 0;
}
