/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_RX_RING_H
#define MT7925_RX_RING_H
#include "core.h"

/* One coherent allocation. address is its DMA logical address, not a CPU
 * physical address. Metadata and memory stay immutable/alive until DMA stops. */
typedef struct mt_rx_bank {
    uint8_t *virtual_address;
    uint64_t address;
    uint32_t bytes;
} mt_rx_bank;

/* One complete packet per descriptor (MCU events or data RX). Fragments/corruption
 * poison the ring until the platform stops DMA and resets it. The platform owns
 * coherent allocations, lifetime and initial hardware configuration. */
typedef struct mt_rx_ring {
    mt_desc *descriptors;
    uint64_t address;
    uint32_t count, stride, capacity, head, tail;
    int fault;
    const mt_rx_bank *banks;
    uint32_t bank_count, slots_per_bank;
} mt_rx_ring;
typedef struct mt_rx_io {
    void *context;
    void (*barrier)(void *);
    int (*publish)(void *, uint32_t);
} mt_rx_io;
MT_API int mt_rx_init(mt_rx_ring *, mt_desc *, uint64_t, uint32_t, uint32_t, uint32_t);
/* Fixed-size groups of slots, backed by independent coherent allocations.
 * Every slot stays inside one bank. Validates all banks before writing any
 * descriptor; banks must also be virtually and DMA-address disjoint. */
MT_API int mt_rx_init_banked(mt_rx_ring *, mt_desc *, const mt_rx_bank *,
    uint32_t bank_count, uint32_t slots_per_bank, uint32_t count,
    uint32_t stride, uint32_t capacity);
/* Initial hardware CPU index is head (the first unposted descriptor).
 * Receive copies from the fixed pool (pool may be NULL in banked mode)
 * before giving a different spare slot to DMA. Buffers must be disjoint. */
MT_API int mt_rx_receive(mt_rx_ring *, const uint8_t *, uint8_t *, uint32_t,
                         uint32_t *, const mt_rx_io *);
#endif
