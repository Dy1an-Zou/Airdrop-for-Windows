/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "service_bridge.h"

#include <string.h>

#include "frame_fixup.h"

#define IEEE80211_SENDER_OFFSET 10
#define ETHER_ADDR_LEN 6
#define TLV_HEADER_LEN 3
#define TLV_SERVICE_RESPONSE 2
/* In a service response: the length of the name plus one for the record
 * type behind it, the name, the type, the length of the data, two bytes
 * that have only been seen zero, the data. */
#define RESPONSE_NAME_LEN_BYTES 2
#define RESPONSE_AFTER_TYPE_BYTES 4

#define DNS_TYPE_PTR 12
#define DNS_TYPE_TXT 16
#define DNS_TYPE_AAAA 28
#define DNS_TYPE_SRV 33
#define DNS_CLASS_IN 1
/* Set on records only one host can own. */
#define DNS_CACHE_FLUSH 0x8000
#define DNS_RESPONSE_AUTHORITATIVE 0x8400
#define DNS_HEADER_LEN 12
#define DNS_ANSWER_COUNT_OFFSET 6
#define DNS_MAX_NAME 255
#define DNS_MAX_LABEL 63
/* The frames repeat every second or so, so a record need not live long. */
#define RECORD_TTL_SECONDS 120
#define SRV_FIXED_BYTES 6

/* A name in a frame ends with a zero or with this byte and the number of a
 * well-known ending. */
#define NAME_ENDING_MARK 0xC0
/* Ending 0 is only used in a record's data: it stands for the record's own
 * name. */
static const char *const endings[] = {
	NULL,
	"_airplay._tcp.local",
	"_airplay._udp.local",
	"_airplay",
	"_raop._tcp.local",
	"_raop._udp.local",
	"_raop",
	"_airdrop._tcp.local",
	"_airdrop._udp.local",
	"_airdrop",
	"_tcp.local",
	"_udp.local",
	"local",
	"ip6.arpa",
	"ip4.arpa",
};
#define ENDING_COUNT ((int) (sizeof(endings) / sizeof(endings[0])))
static const uint8_t airdrop_type[] = "\x08_airdrop\x04_tcp\x05local";

#define IP6_HEADER_LEN 40
#define UDP_HEADER_LEN 8
#define IP6_ADDR_LEN 16
#define IP6_SOURCE_OFFSET 8
#define IP6_DESTINATION_OFFSET 24
#define IP6_NEXT_HEADER_UDP 17
#define IP6_HOP_LIMIT_LINK 255
#define MDNS_PORT 5353
#define UDP_CHECKSUM_OFFSET 6
static const uint8_t mdns_group[IP6_ADDR_LEN] = {0xFF, 0x02, 0, 0, 0, 0, 0, 0,
						 0,    0,    0, 0, 0, 0, 0, 0xFB};

struct name {
	uint8_t bytes[DNS_MAX_NAME];
	int length;
};

/* Where the DNS message is built; `failed` once something did not fit. */
struct message {
	uint8_t *bytes;
	int length;
	int capacity;
	int failed;
	int answers;
};

static unsigned read_le16(const uint8_t *bytes) {
	return bytes[0] | bytes[1] << 8;
}

static void put(struct message *out, const uint8_t *bytes, int count) {
	if (out->failed || count > out->capacity - out->length) {
		out->failed = 1;
		return;
	}
	memcpy(out->bytes + out->length, bytes, (size_t) count);
	out->length += count;
}

static void put_be16(struct message *out, unsigned value) {
	const uint8_t bytes[] = {(uint8_t) (value >> 8), (uint8_t) value};
	put(out, bytes, sizeof(bytes));
}

static int append(struct name *name, const uint8_t *bytes, int count) {
	if (count > DNS_MAX_NAME - name->length)
		return -1;
	memcpy(name->bytes + name->length, bytes, (size_t) count);
	name->length += count;
	return 0;
}

/* Appends "a.b.c" as labels with the closing zero. */
static int append_dotted(struct name *name, const char *dotted) {
	while (*dotted) {
		const char *dot = strchr(dotted, '.');
		int label_len = dot ? (int) (dot - dotted) : (int) strlen(dotted);
		const uint8_t prefix = (uint8_t) label_len;
		if (append(name, &prefix, 1) < 0 ||
		    append(name, (const uint8_t *) dotted, label_len) < 0)
			return -1;
		dotted += label_len + (dot ? 1 : 0);
	}
	return append(name, (const uint8_t *) "", 1);
}

/* Writes the name at `raw` in DNS form. `own` is what ending 0 stands for,
 * or NULL where that ending makes no sense. Returns how many bytes of `raw`
 * the name took, or -1 if it is not a whole valid name. */
static int expand_name(const uint8_t *raw, int raw_len, const struct name *own, struct name *out) {
	int offset = 0;
	out->length = 0;
	while (offset < raw_len) {
		int label_len = raw[offset];
		if (label_len == 0)
			return append(out, raw + offset, 1) < 0 ? -1 : offset + 1;
		if (label_len == NAME_ENDING_MARK) {
			if (offset + 1 >= raw_len)
				return -1;
			int ending = raw[offset + 1];
			if (ending == 0 && own)
				return append(out, own->bytes, own->length) < 0 ? -1 : offset + 2;
			if (ending == 0 || ending >= ENDING_COUNT)
				return -1;
			return append_dotted(out, endings[ending]) < 0 ? -1 : offset + 2;
		}
		if (label_len > DNS_MAX_LABEL || label_len >= raw_len - offset ||
		    append(out, raw + offset, 1 + label_len) < 0)
			return -1;
		offset += 1 + label_len;
	}
	return -1;
}

static int is_airdrop(const struct name *name) {
	int type_len = (int) sizeof(airdrop_type);
	return name->length >= type_len &&
	       memcmp(name->bytes + name->length - type_len, airdrop_type, (size_t) type_len) == 0;
}

static void put_record(struct message *out, const struct name *name, unsigned type, unsigned class,
		       const uint8_t *data, int data_len) {
	const uint8_t ttl[] = {0, 0, 0, RECORD_TTL_SECONDS};
	put(out, name->bytes, name->length);
	put_be16(out, type);
	put_be16(out, class);
	put(out, ttl, sizeof(ttl));
	put_be16(out, (unsigned) data_len);
	put(out, data, data_len);
	out->answers++;
}

/* Adds what one service response says, if it is about AirDrop. */
static void bridge_response(const uint8_t *response, int length,
			    const uint8_t address[IP6_ADDR_LEN], struct message *out) {
	if (length < RESPONSE_NAME_LEN_BYTES)
		return;
	int name_len = (int) read_le16(response) - 1;
	int type_offset = RESPONSE_NAME_LEN_BYTES + name_len;
	if (name_len < 1 || length < type_offset + 1 + RESPONSE_AFTER_TYPE_BYTES)
		return;
	int data_offset = type_offset + 1 + RESPONSE_AFTER_TYPE_BYTES;
	int data_len = (int) read_le16(response + type_offset + 1);
	const uint8_t *data = response + data_offset;
	struct name own, in_data;
	uint8_t rdata[SRV_FIXED_BYTES + DNS_MAX_NAME];
	if (data_len > length - data_offset ||
	    expand_name(response + RESPONSE_NAME_LEN_BYTES, name_len, NULL, &own) != name_len ||
	    !is_airdrop(&own))
		return;

	switch (response[type_offset]) {
		case DNS_TYPE_PTR:
			if (expand_name(data, data_len, &own, &in_data) == data_len)
				put_record(out, &own, DNS_TYPE_PTR, DNS_CLASS_IN, in_data.bytes,
					   in_data.length);
			break;
		case DNS_TYPE_TXT:
			put_record(out, &own, DNS_TYPE_TXT, DNS_CLASS_IN | DNS_CACHE_FLUSH, data,
				   data_len);
			break;
		case DNS_TYPE_SRV:
			if (data_len <= SRV_FIXED_BYTES ||
			    expand_name(data + SRV_FIXED_BYTES, data_len - SRV_FIXED_BYTES, NULL,
					&in_data) != data_len - SRV_FIXED_BYTES)
				break;
			memcpy(rdata, data, SRV_FIXED_BYTES);
			memcpy(rdata + SRV_FIXED_BYTES, in_data.bytes, (size_t) in_data.length);
			put_record(out, &own, DNS_TYPE_SRV, DNS_CLASS_IN | DNS_CACHE_FLUSH, rdata,
				   SRV_FIXED_BYTES + in_data.length);
			put_record(out, &in_data, DNS_TYPE_AAAA, DNS_CLASS_IN | DNS_CACHE_FLUSH,
				   address, IP6_ADDR_LEN);
			break;
		default:
			break;
	}
}

/* The link-local address an AWDL station derives from its MAC address. */
static void link_local(const uint8_t mac[ETHER_ADDR_LEN], uint8_t address[IP6_ADDR_LEN]) {
	const uint8_t derived[IP6_ADDR_LEN] = {
		0xFE,	       0x80,   0,      0,    0,	   0,	   0,	   0,
		mac[0] ^ 0x02, mac[1], mac[2], 0xFF, 0xFE, mac[3], mac[4], mac[5]};
	memcpy(address, derived, IP6_ADDR_LEN);
}

static uint32_t sum_be16(const uint8_t *bytes, int length, uint32_t sum) {
	for (int i = 0; i + 1 < length; i += 2)
		sum += (uint32_t) (bytes[i] << 8 | bytes[i + 1]);
	if (length & 1)
		sum += (uint32_t) bytes[length - 1] << 8;
	return sum;
}

/* The UDP checksum of an IPv6 packet whose checksum field is still zero. */
static unsigned udp_checksum(const uint8_t *packet, int udp_len) {
	uint32_t sum = sum_be16(packet + IP6_SOURCE_OFFSET, 2 * IP6_ADDR_LEN, 0);
	sum += (uint32_t) udp_len + IP6_NEXT_HEADER_UDP;
	sum = sum_be16(packet + IP6_HEADER_LEN, udp_len, sum);
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	/* A computed zero is sent as all ones: zero means no checksum. */
	return sum == 0xFFFF ? 0xFFFF : (unsigned) (~sum & 0xFFFF);
}

int service_bridge_packet(const uint8_t *frame, int length, uint8_t *packet) {
	int end;
	int offset = awdl_action_tlvs(frame, length, &end);
	if (offset < 0)
		return 0;
	int radiotap_len = (int) read_le16(frame + 2);
	uint8_t address[IP6_ADDR_LEN];
	link_local(frame + radiotap_len + IEEE80211_SENDER_OFFSET, address);

	uint8_t *dns = packet + IP6_HEADER_LEN + UDP_HEADER_LEN;
	struct message out = {.bytes = dns,
			      .length = DNS_HEADER_LEN,
			      .capacity =
				      SERVICE_BRIDGE_MAX_PACKET - IP6_HEADER_LEN - UDP_HEADER_LEN};
	while (end - offset >= TLV_HEADER_LEN) {
		int tlv_len = (int) read_le16(frame + offset + 1);
		if (tlv_len > end - offset - TLV_HEADER_LEN)
			break;
		if (frame[offset] == TLV_SERVICE_RESPONSE)
			bridge_response(frame + offset + TLV_HEADER_LEN, tlv_len, address, &out);
		offset += TLV_HEADER_LEN + tlv_len;
	}
	if (out.failed || out.answers == 0)
		return 0;
	memset(dns, 0, DNS_HEADER_LEN);
	dns[2] = DNS_RESPONSE_AUTHORITATIVE >> 8;
	dns[DNS_ANSWER_COUNT_OFFSET + 1] = (uint8_t) out.answers;

	int udp_len = UDP_HEADER_LEN + out.length;
	memset(packet, 0, IP6_HEADER_LEN + UDP_HEADER_LEN);
	packet[0] = 0x60;
	packet[4] = (uint8_t) (udp_len >> 8);
	packet[5] = (uint8_t) udp_len;
	packet[6] = IP6_NEXT_HEADER_UDP;
	packet[7] = IP6_HOP_LIMIT_LINK;
	memcpy(packet + IP6_SOURCE_OFFSET, address, IP6_ADDR_LEN);
	memcpy(packet + IP6_DESTINATION_OFFSET, mdns_group, IP6_ADDR_LEN);
	uint8_t *udp = packet + IP6_HEADER_LEN;
	udp[0] = udp[2] = MDNS_PORT >> 8;
	udp[1] = udp[3] = MDNS_PORT & 0xFF;
	udp[4] = (uint8_t) (udp_len >> 8);
	udp[5] = (uint8_t) udp_len;
	unsigned checksum = udp_checksum(packet, udp_len);
	udp[UDP_CHECKSUM_OFFSET] = (uint8_t) (checksum >> 8);
	udp[UDP_CHECKSUM_OFFSET + 1] = (uint8_t) checksum;
	return IP6_HEADER_LEN + udp_len;
}
