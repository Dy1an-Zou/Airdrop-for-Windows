/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_RAW_RX_H
#define MT7925_RAW_RX_H
#include "rx_ring.h"
/* Diagnostic outcomes, counted only for consumed RX packets. Counters saturate;
 * U2M means RXD3 ADDR_TYPE==1, not proof of an authenticated peer. */
enum mt_rx_outcome { MT_RX_ACCEPT, MT_RX_OTHER, MT_RX_AIR_ERROR,
    MT_RX_TRANSLATED, MT_RX_CIPHER, MT_RX_PROTECTED, MT_RX_CONTROL,
    MT_RX_MALFORMED, MT_RX_CAPACITY, MT_RX_OUTCOMES };
typedef struct mt_rx_stats {
    uint32_t outcomes[MT_RX_OUTCOMES], to_local[MT_RX_OUTCOMES];
    uint32_t local_error_bits[6], first_local_error[8]; /* RXD only, no payload. */
    /* Bounded evidence from rejected raw U2M/HDR_TRANS_ERROR packets with no
     * RXD FCS error. Not an ACK assertion and never delivered to AWDL. Keep the
     * first complete FC/Duration/RA prefix; count later candidates/malformed
     * layouts without replacing it. frame_bytes excludes configured FCS. */
    uint32_t short_candidates, short_malformed, short_offset, short_frame_bytes;
    uint32_t short_rxd[3], short_header_bytes; /* RXD1/2/3 belong to this sample. */
    uint8_t short_header[16];
} mt_rx_stats;
MT_API int mt_rx_radio_record_measured(const uint8_t *, uint32_t, uint32_t,
    uint8_t *, uint32_t, uint32_t *, mt_rx_stats *);
/* Converts one completed MT7925 RXD packet to the existing radio_link.h v3
 * RX record: 8-byte link header + radiotap + raw 802.11 frame.
 * fcs_present is the platform's configured RX policy, never inferred from data.
 * Non-frame events, translated/encrypted frames and corrupt air frames are
 * dropped (MT_PENDING); malformed/truncated descriptors return MT_INVALID.
 * Caller supplies disjoint buffers. No MMIO, allocation or radio activation. */
MT_API int mt_rx_radio_record(const uint8_t *, uint32_t, uint32_t,
                              uint8_t *, uint32_t, uint32_t *);
/* Consume through the actual coherent-ring boundary before decoding. scratch
 * must hold one whole DMA packet. MT_FULL from record encoding consumes that
 * packet; use capacity >= ring capacity + 22 to avoid that drop. */
MT_API int mt_radio_receive(mt_rx_ring *, const uint8_t *, const mt_rx_io *,
    uint8_t *, uint32_t, uint32_t, uint8_t *, uint32_t, uint32_t *);
#endif
