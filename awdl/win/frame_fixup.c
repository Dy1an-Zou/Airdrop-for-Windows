/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "frame_fixup.h"

#include <string.h>

#define RADIOTAP_MIN_LEN 8
#define RADIOTAP_PRESENT_FLAGS (1u << 1)
#define RADIOTAP_PRESENT_TSFT (1u << 0)
#define RADIOTAP_FLAGS_OFFSET 8
#define RADIOTAP_FLAG_FCS 0x10

#define IEEE80211_HEADER_LEN 24
#define FRAME_CONTROL_ACTION 0xD0
#define FCS_LEN 4

/* Category 127, Apple's OUI, type 8: an AWDL action frame. */
static const uint8_t awdl_action[] = {0x7F, 0x00, 0x17, 0xF2, 0x08};
#define AWDL_FIXED_LEN 16
#define TLV_HEADER_LEN 3
#define TLV_HT_CAPABILITIES 7
#define HT_CAPABILITIES_OFFSET 2
/* Supported channel width, short guard interval at 40 MHz, CCK at 40 MHz. */
#define HT_CAPABILITIES_40MHZ 0x1042
#define HT_CAPABILITIES_SHORT_GI_20MHZ 0x0020
/* After the capabilities come the A-MPDU parameters, then one byte of
 * supported rates per spatial stream. */
#define HT_SECOND_STREAM_RATES_OFFSET 6
#define HT_ALL_RATES 0xFF

/* The AWDL TLV that carries an 802.11 element, here VHT Capabilities:
 * element 191, twelve bytes. */
#define TLV_IEEE80211_CONTAINER 17
#define VHT_CAPABILITIES_ELEMENT 191
#define VHT_CAPABILITIES_LEN 12
#define VHT_TLV_LEN (TLV_HEADER_LEN + 2 + VHT_CAPABILITIES_LEN)
/* A-MPDUs of up to 1 MiB, the most the field can say: the block-ack window
 * is what bounds them. */
#define VHT_INFO_MAX_AMPDU 0x03800000u
#define VHT_INFO_SHORT_GI_80MHZ 0x00000020u
/* Two bits per stream: 2 is MCS 0 to 9, 3 is no such stream. */
#define VHT_RATES_ONE_STREAM 0xFFFEu
#define VHT_RATES_TWO_STREAMS 0xFFFAu

static int announce_two_streams;
static int announce_short_gi;
static int announce_width_mhz = 20;

void frame_fixup_announce(int two_streams, int short_guard_interval, int width_mhz) {
	announce_two_streams = two_streams;
	announce_short_gi = short_guard_interval;
	announce_width_mhz = width_mhz;
}

static unsigned read_le16(const uint8_t *bytes) {
	return bytes[0] | bytes[1] << 8;
}

/* Returns where the TLVs of an AWDL action frame start, or -1 for any other
 * frame. `*fcs_len` is how many checksum bytes follow the frame. */
static int tlv_offset(const uint8_t *frame, int length, int *fcs_len) {
	if (length < RADIOTAP_MIN_LEN)
		return -1;
	int radiotap_len = (int) read_le16(frame + 2);
	unsigned present = read_le16(frame + 4) | read_le16(frame + 6) << 16;
	int start = radiotap_len + IEEE80211_HEADER_LEN + AWDL_FIXED_LEN;
	if (radiotap_len < RADIOTAP_MIN_LEN || length < start ||
	    frame[radiotap_len] != FRAME_CONTROL_ACTION ||
	    memcmp(frame + radiotap_len + IEEE80211_HEADER_LEN, awdl_action, sizeof(awdl_action)) !=
		    0)
		return -1;
	/* The flags field comes first unless a timestamp precedes it. */
	int flagged = (present & RADIOTAP_PRESENT_FLAGS) && !(present & RADIOTAP_PRESENT_TSFT) &&
		      radiotap_len > RADIOTAP_FLAGS_OFFSET &&
		      (frame[RADIOTAP_FLAGS_OFFSET] & RADIOTAP_FLAG_FCS);
	*fcs_len = flagged ? FCS_LEN : 0;
	return length - *fcs_len < start ? -1 : start;
}

/* Walks the TLVs from `offset` and returns where the walk stops: at `end`, or
 * where fewer bytes are left than the next TLV needs. */
static int walk_tlvs(const uint8_t *frame, int offset, int end, int wanted, int *found) {
	while (end - offset >= TLV_HEADER_LEN) {
		int tlv_len = (int) read_le16(frame + offset + 1);
		if (tlv_len > end - offset - TLV_HEADER_LEN)
			break;
		if (found && frame[offset] == wanted)
			*found = offset;
		offset += TLV_HEADER_LEN + tlv_len;
	}
	return offset;
}

int awdl_action_tlvs(const uint8_t *frame, int length, int *end) {
	int fcs_len;
	int offset = tlv_offset(frame, length, &fcs_len);
	if (offset >= 0)
		*end = length - fcs_len;
	return offset;
}

int frame_fixup_received(uint8_t *frame, int length) {
	int fcs_len;
	int offset = tlv_offset(frame, length, &fcs_len);
	if (offset < 0)
		return length;
	int end = length - fcs_len;
	int stop = walk_tlvs(frame, offset, end, 0, NULL);
	int left = end - stop;
	if (left == 0 || left >= TLV_HEADER_LEN)
		return length;
	for (int i = stop; i < end; i++)
		if (frame[i] != 0)
			return length;
	memmove(frame + stop, frame + end, (size_t) fcs_len);
	return length - left;
}

static void write_le16(uint8_t *bytes, unsigned value) {
	bytes[0] = (uint8_t) value;
	bytes[1] = (uint8_t) (value >> 8);
}

/* Writes the TLV that says which VHT frames this station receives. */
static void write_vht_tlv(uint8_t *tlv) {
	unsigned info = VHT_INFO_MAX_AMPDU | (announce_short_gi ? VHT_INFO_SHORT_GI_80MHZ : 0);
	unsigned rates = announce_two_streams ? VHT_RATES_TWO_STREAMS : VHT_RATES_ONE_STREAM;
	memset(tlv, 0, VHT_TLV_LEN);
	tlv[0] = TLV_IEEE80211_CONTAINER;
	write_le16(tlv + 1, 2 + VHT_CAPABILITIES_LEN);
	tlv[3] = VHT_CAPABILITIES_ELEMENT;
	tlv[4] = VHT_CAPABILITIES_LEN;
	write_le16(tlv + 5, info & 0xFFFF);
	write_le16(tlv + 7, info >> 16);
	/* Received rates, then sent ones, each followed by an unused limit. */
	write_le16(tlv + 9, rates);
	write_le16(tlv + 13, rates);
}

int frame_fixup_sent(uint8_t *frame, int length, int capacity) {
	int fcs_len;
	int offset = tlv_offset(frame, length, &fcs_len);
	if (offset < 0)
		return length;
	int found = -1;
	int end = walk_tlvs(frame, offset, length - fcs_len, TLV_HT_CAPABILITIES, &found);
	if (found < 0)
		return length;
	uint8_t *value = frame + found + TLV_HEADER_LEN;
	int value_len = (int) read_le16(frame + found + 1);
	if (value_len < HT_CAPABILITIES_OFFSET + 2)
		return length;
	uint8_t *caps = value + HT_CAPABILITIES_OFFSET;
	unsigned announced = read_le16(caps);
	if (announce_width_mhz < 40)
		announced &= ~HT_CAPABILITIES_40MHZ;
	if (announce_short_gi)
		announced |= HT_CAPABILITIES_SHORT_GI_20MHZ;
	write_le16(caps, announced);
	if (announce_two_streams && value_len > HT_SECOND_STREAM_RATES_OFFSET)
		value[HT_SECOND_STREAM_RATES_OFFSET] = HT_ALL_RATES;

	/* Only a frame that ends on a TLV, as OWL's do, can take one more. */
	if (announce_width_mhz < 80 || fcs_len || end != length || length + VHT_TLV_LEN > capacity)
		return length;
	write_vht_tlv(frame + length);
	return length + VHT_TLV_LEN;
}
