/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Two places where OWL and the frames on the air disagree, corrected as AWDL
 * action frames pass between it and the radio. Both take a radiotap header
 * with the 802.11 frame behind it. */
#pragma once
#include <stdint.h>

/* Where the TLVs of an AWDL action frame start, with `*end` set to where they
 * stop; -1 for any other frame. */
int awdl_action_tlvs(const uint8_t *frame, int length, int *end);

/* An iPhone can end an action frame with zero padding, and OWL refuses the
 * frame when a byte or two of it is left over. Returns the length without
 * those bytes. */
int frame_fixup_received(uint8_t *frame, int length);

/* OWL announces that it receives 40 MHz frames, whatever the radio daemon
 * is tuned to: makes the capabilities of an outgoing frame say what
 * frame_fixup_announce was told. The frame may grow, up to `capacity` bytes;
 * returns its length. */
int frame_fixup_sent(uint8_t *frame, int length, int capacity);

/* What outgoing frames say this station can receive: OWL's single stream
 * with the long guard interval on 20 MHz, and each of these on top.
 * `width_mhz` is the channel the radio receives on; at 80 the frames also
 * carry VHT capabilities, which OWL does not send. */
void frame_fixup_announce(int two_streams, int short_guard_interval, int width_mhz);
