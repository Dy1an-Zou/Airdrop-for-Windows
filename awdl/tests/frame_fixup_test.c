/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The two corrections made to AWDL action frames on their way through. */
#include <stdio.h>
#include <string.h>

#include "frame_fixup.h"

#define RADIOTAP_LEN 10
#define BODY_OFFSET (RADIOTAP_LEN + 24)
#define FCS_LEN 4

static int failures;

static void expect(int condition, const char *what) {
	printf("%s  %s\n", condition ? "ok  " : "FAIL", what);
	if (!condition)
		failures++;
}

/* Radiotap header as the radio daemon writes it, an action frame header, the
 * AWDL fixed fields and one version TLV. Returns the length so far. */
static int start_frame(uint8_t *frame, int with_fcs_flag) {
	static const uint8_t head[] = {
		0,    0, RADIOTAP_LEN, 0, 0x22, 0, 0, 0, 0x10, 0xD8, /* radiotap */
		0xD0, 0,					     /* action frame */
	};
	static const uint8_t awdl[] = {0x7F, 0x00, 0x17, 0xF2, 8, 0x10, 3, 0,
				       0,    0,	   0,	 0,    0, 0,	0, 0};
	static const uint8_t version_tlv[] = {21, 2, 0, 0xA2, 0x02};
	memset(frame, 0, BODY_OFFSET);
	memcpy(frame, head, sizeof(head));
	if (!with_fcs_flag)
		frame[8] = 0;
	memcpy(frame + BODY_OFFSET, awdl, sizeof(awdl));
	memcpy(frame + BODY_OFFSET + sizeof(awdl), version_tlv, sizeof(version_tlv));
	return BODY_OFFSET + sizeof(awdl) + sizeof(version_tlv);
}

static int add(uint8_t *frame, int length, const uint8_t *bytes, int count) {
	memcpy(frame + length, bytes, (size_t) count);
	return length + count;
}

int main(void) {
	static const uint8_t fcs[FCS_LEN] = {0xDE, 0xAD, 0xBE, 0xEF};
	static const uint8_t zeros[8] = {0};
	static const uint8_t ht_tlv[] = {7, 8, 0, 0, 0, 0xCE, 0x11, 0x1B, 0xFF, 0, 0};
	uint8_t frame[256], before[256];

	int plain = add(frame, start_frame(frame, 1), fcs, FCS_LEN);
	memcpy(before, frame, sizeof(frame));
	expect(frame_fixup_received(frame, plain) == plain &&
		       memcmp(before, frame, (size_t) plain) == 0,
	       "a frame that ends on a TLV is left alone");

	/* Eight zero bytes read as two empty TLVs and two bytes over. */
	int tlvs_end = start_frame(frame, 1);
	int padded = add(frame, add(frame, tlvs_end, zeros, 8), fcs, FCS_LEN);
	int fixed = frame_fixup_received(frame, padded);
	expect(fixed == padded - 2, "zero bytes too few for a TLV are dropped");
	expect(memcmp(frame + fixed - FCS_LEN, fcs, FCS_LEN) == 0,
	       "the checksum still ends the frame");

	padded = add(frame, start_frame(frame, 0), zeros, 2);
	expect(frame_fixup_received(frame, padded) == padded - 2,
	       "a frame without a checksum is cut at its end");

	static const uint8_t stray[] = {0x00, 0x01};
	padded = add(frame, add(frame, start_frame(frame, 1), stray, 2), fcs, FCS_LEN);
	expect(frame_fixup_received(frame, padded) == padded,
	       "leftover bytes that are not zero are kept for OWL to refuse");

	static const uint8_t overrun[] = {21, 40, 0, 1, 2};
	padded = add(frame, add(frame, start_frame(frame, 1), overrun, 5), fcs, FCS_LEN);
	expect(frame_fixup_received(frame, padded) == padded,
	       "a TLV longer than the frame is kept as it is");

	padded = add(frame, add(frame, start_frame(frame, 1), zeros, 2), fcs, FCS_LEN);
	frame[RADIOTAP_LEN] = 0x88;
	expect(frame_fixup_received(frame, padded) == padded, "a data frame is left alone");
	expect(frame_fixup_received(frame, 12) == 12, "a short frame is left alone");

	int sent = add(frame, start_frame(frame, 0), ht_tlv, sizeof(ht_tlv));
	int caps = sent - 6;
	expect(frame_fixup_sent(frame, sent, sizeof(frame)) == sent, "a frame keeps its length");
	expect(frame[caps] == 0x8C && frame[caps + 1] == 0x01,
	       "an outgoing frame no longer claims 40 MHz reception");
	expect(frame[caps + 2] == 0x1B && frame[caps + 3] == 0xFF && frame[caps + 4] == 0,
	       "the rest of its capabilities stay");

	sent = add(frame, start_frame(frame, 0), ht_tlv, sizeof(ht_tlv));
	frame_fixup_announce(1, 1, 20);
	frame_fixup_sent(frame, sent, sizeof(frame));
	expect(frame[caps] == 0xAC && frame[caps + 1] == 0x01,
	       "the short guard interval at 20 MHz can be announced");
	expect(frame[caps + 3] == 0xFF && frame[caps + 4] == 0xFF && frame[caps + 5] == 0,
	       "and so can the rates of a second stream");

	sent = add(frame, start_frame(frame, 0), ht_tlv, sizeof(ht_tlv));
	frame_fixup_announce(0, 0, 40);
	expect(frame_fixup_sent(frame, sent, sizeof(frame)) == sent && frame[caps] == 0xCE &&
		       frame[caps + 1] == 0x11,
	       "a radio tuned to 40 MHz keeps OWL's 40 MHz capabilities");

	static const uint8_t vht_two_streams[] = {17,	14,   0, 191, 12,   0x20, 0x00, 0x80, 0x03,
						  0xFA, 0xFF, 0, 0,   0xFA, 0xFF, 0,	0};
	sent = add(frame, start_frame(frame, 0), ht_tlv, sizeof(ht_tlv));
	frame_fixup_announce(1, 1, 80);
	int grown = frame_fixup_sent(frame, sent, sizeof(frame));
	expect(grown == sent + (int) sizeof(vht_two_streams) &&
		       memcmp(frame + sent, vht_two_streams, sizeof(vht_two_streams)) == 0,
	       "at 80 MHz the frame gains VHT capabilities for two streams");
	expect(frame[caps] == 0xEE && frame[caps + 1] == 0x11, "and keeps the 40 MHz ones");

	sent = add(frame, start_frame(frame, 0), ht_tlv, sizeof(ht_tlv));
	frame_fixup_announce(0, 0, 80);
	grown = frame_fixup_sent(frame, sent, sizeof(frame));
	expect(grown == sent + 17 && frame[sent + 5] == 0x00 && frame[sent + 9] == 0xFE &&
		       frame[sent + 13] == 0xFE,
	       "one stream and the long guard interval are announced as such");
	sent = add(frame, start_frame(frame, 0), ht_tlv, sizeof(ht_tlv));
	expect(frame_fixup_sent(frame, sent, sent + 16) == sent,
	       "a frame with no room left is sent as it is");
	sent = start_frame(frame, 0);
	expect(frame_fixup_sent(frame, sent, sizeof(frame)) == sent,
	       "a frame without HT capabilities gains none for VHT");
	frame_fixup_announce(0, 0, 20);

	return failures ? 1 : 0;
}
