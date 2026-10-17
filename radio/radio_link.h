/* SPDX-License-Identifier: MIT */
/* Wire protocol between the radio daemon and its client: a header, then
 * `length` payload bytes, little-endian. MIT, so both sides can include it. */
#pragma once
#include <stdint.h>

#define RADIO_LINK_PIPE_NAME "\\\\.\\pipe\\winawdl-radio"
#define RADIO_LINK_VERSION 3u
#define RADIO_LINK_MAX_PAYLOAD 16384u

enum radio_link_type {
  /* radio -> client, sent once on connect: struct radio_link_hello */
  RADIO_LINK_HELLO = 1,
  /* radio -> client: radiotap header, then an 802.11 frame */
  RADIO_LINK_RX = 2,
  /* client -> radio: radiotap header, then an 802.11 frame without FCS */
  RADIO_LINK_TX = 3,
  /* client -> radio: one byte, the channel number */
  RADIO_LINK_SET_CHANNEL = 4,
  /* client -> radio: six bytes. Only frames whose third address matches are
   * forwarded; all zeros forwards everything. */
  RADIO_LINK_SET_BSSID_FILTER = 5,
  /* client -> radio: six bytes, the address the client's frames come from */
  RADIO_LINK_SET_ADDRESS = 6,
};

struct radio_link_header {
  uint16_t type;
  uint16_t reserved;
  uint32_t length;
};

struct radio_link_hello {
  uint32_t version;
  uint8_t channel;
  uint8_t width_mhz; /* what the radio receives: 20, 40 or 80 */
  uint8_t reserved[2];
};
