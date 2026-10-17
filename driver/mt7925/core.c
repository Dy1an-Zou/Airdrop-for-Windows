/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * MCU wire fields and DMA descriptor layout derived from openwrt/mt76:
 * Copyright (C) 2020, 2023 MediaTek Inc.
 * DMA layout: Copyright (C) 2016 Felix Fietkau <nbd@nbd.name>.
 * See SOURCES.md for the exact revision and adaptation boundaries.
 */
#include "core.h"

static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, v); put16(p + 2, v >> 16);
}

int mt_dma_span32(uint64_t address, uint32_t size) {
    return size != 0 && address <= 0xffffffffULL &&
           (uint64_t)(size - 1u) <= 0xffffffffULL - address;
}

int mt_ring_init(mt_ring *r, mt_desc *d, uint32_t count) {
    uint32_t i;
    if (!r || !d || count < 4 || count > 4096) return MT_INVALID;
    for (i = 0; i < count; ++i) {
        d[i].buf0 = 0; d[i].buf1 = 0; d[i].info = 0;
        d[i].ctrl = MT_DMA_DONE;
    }
    r->descriptors = d; r->count = count;
    r->head = 0; r->tail = 0; r->queued = 0;
    return MT_OK;
}

static int valid_ring(const mt_ring *r) {
    return r && r->descriptors && r->count >= 4 && r->count <= 4096 &&
           r->head < r->count && r->tail < r->count &&
           r->queued <= r->count - 2 &&
           (r->tail + r->queued) % r->count == r->head;
}

int mt_ring_submit(mt_ring *r, uint64_t address, uint32_t size, uint32_t *index) {
    mt_desc *d;
    if (!valid_ring(r) || !index || size > MT_DMA_MAX_LENGTH ||
        !mt_dma_span32(address, size)) return MT_INVALID;
    /* Match the mt76 raw TX path's two-descriptor reserve. */
    if (r->queued >= r->count - 2) return MT_FULL;
    d = &r->descriptors[r->head];
    if (!(((volatile mt_desc *)d)->ctrl & MT_DMA_DONE)) return MT_PENDING;
    d->buf0 = (uint32_t)address; d->buf1 = 0; d->info = 0;
    d->ctrl = (size << 16) | MT_DMA_LAST0;
    *index = r->head;
    r->head = (r->head + 1) % r->count;
    ++r->queued;
    return MT_OK;
}

int mt_ring_reclaim(mt_ring *r, uint32_t dma_index, uint32_t *index) {
    uint32_t completed;
    if (!valid_ring(r) || !index || dma_index >= r->count) return MT_INVALID;
    completed = (dma_index + r->count - r->tail) % r->count;
    if (completed > r->queued) return MT_INVALID;
    if (!completed) return MT_PENDING;
    *index = r->tail;
    r->descriptors[r->tail].ctrl = MT_DMA_DONE;
    r->tail = (r->tail + 1) % r->count;
    --r->queued;
    return MT_OK;
}

int mt_mcu_encode(uint32_t cmd, uint8_t *sequence, const uint8_t *payload,
                  uint32_t length, uint8_t *out, uint32_t capacity) {
    uint32_t header, total, i, id = cmd & 0xffu, ext = (cmd >> 8) & 0xffu;
    uint8_t seq, option;
    if (!sequence || !out || (length && !payload) || (cmd & ~0x001fffffu))
        return MT_INVALID;
    header = cmd == MT_FW_SCATTER ? 0u : ((cmd & MT_MCU_UNI) ? 48u : 64u);
    if (length > MT_DMA_MAX_LENGTH - header || capacity < length + header ||
        (cmd == MT_FW_SCATTER && !length)) return MT_INVALID;
    total = length + header;
    seq = (uint8_t)((*sequence + 1u) & 15u);
    if (!seq) seq = 1;
    for (i = 0; i < header; ++i) out[i] = 0;
    if (header) {
        put32(out, 0x41000000u | total);
        put32(out + 4, 0x00004000u);
        put16(out + 32, total - 32);
        out[37] = 0xa0; out[39] = seq;
        if (cmd & MT_MCU_UNI) {
            put16(out + 34, id);
            option = (cmd & MT_MCU_QUERY) ? 3u : 7u;
            if (cmd == (MT_MCU_UNI | 7u) || cmd == (MT_MCU_UNI | 14u))
                option &= 0xfeu;
            if (id == 0x32 || id == 0x46) option = (cmd & MT_MCU_QUERY) ? 2u : 6u;
            out[43] = option;
        } else {
            put16(out + 34, 0x8000);
            out[36] = (uint8_t)id;
            out[38] = ext || (cmd & MT_MCU_CE) ? ((cmd & MT_MCU_QUERY) ? 0u : 1u) : 3u;
            out[41] = (uint8_t)ext;
            out[42] = cmd & MT_MCU_WA ? 2u : 0u;
            out[43] = ext ? 1u : 0u;
        }
    }
    for (i = 0; i < length; ++i) out[header + i] = payload[i];
    *sequence = seq;
    return (int)total;
}
