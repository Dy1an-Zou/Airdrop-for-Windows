/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Descriptor layout and the one-empty-slot convention follow mt76 dma.c/dma.h
 * (Felix Fietkau, 2016). Fixed-pool MCU receive and validation are port additions.
 */
#include "rx_ring.h"

static uint64_t slot_address(const mt_rx_ring *r, uint32_t slot) {
    if (r->banks)
        return r->banks[slot / r->slots_per_bank].address +
            (uint64_t)(slot % r->slots_per_bank) * r->stride;
    return r->address + (uint64_t)slot * r->stride;
}

static void prepare(mt_rx_ring *r, uint32_t slot) {
    mt_desc *d = &r->descriptors[slot];
    d->buf0 = (uint32_t)slot_address(r, slot);
    d->buf1 = 0; d->info = 0;
    d->ctrl = r->capacity << 16;
}

int mt_rx_init(mt_rx_ring *r, mt_desc *descriptors, uint64_t address,
               uint32_t count, uint32_t stride, uint32_t capacity) {
    uint32_t i;
    uint64_t bytes = (uint64_t)count * stride;
    if (!r || !descriptors || count < 3 || count > 4096 || !capacity ||
        capacity > MT_DMA_MAX_LENGTH || stride < capacity || bytes > 0xffffffffu ||
        !mt_dma_span32(address, (uint32_t)bytes)) return MT_INVALID;
    r->descriptors = descriptors; r->address = address;
    r->banks = 0; r->bank_count = 0; r->slots_per_bank = 0;
    r->count = count; r->stride = stride; r->capacity = capacity;
    r->head = count - 1; r->tail = 0; r->fault = 0;
    for (i = 0; i < count - 1; ++i) prepare(r, i);
    descriptors[count - 1].buf0 = 0; descriptors[count - 1].buf1 = 0;
    descriptors[count - 1].info = 0; descriptors[count - 1].ctrl = MT_DMA_DONE;
    return MT_OK;
}

int mt_rx_init_banked(mt_rx_ring *r, mt_desc *descriptors, const mt_rx_bank *banks,
    uint32_t bank_count, uint32_t slots_per_bank, uint32_t count,
    uint32_t stride, uint32_t capacity) {
    uint32_t i, j;
    uint64_t required = (uint64_t)slots_per_bank * stride;
    uintptr_t descriptors_va = (uintptr_t)descriptors;
    if (!r || !descriptors || !banks || count < 3 || count > 4096 ||
        !slots_per_bank || slots_per_bank > count || !bank_count ||
        bank_count != (count + slots_per_bank - 1) / slots_per_bank ||
        !capacity || capacity > MT_DMA_MAX_LENGTH || stride < capacity ||
        required > 0xffffffffu || descriptors_va > UINTPTR_MAX - (uint64_t)count * sizeof(mt_desc))
        return MT_INVALID;
    for (i = 0; i < bank_count; ++i) {
        uintptr_t va = (uintptr_t)banks[i].virtual_address;
        uint32_t bytes = banks[i].bytes;
        if (!va || bytes < required || va > UINTPTR_MAX - bytes ||
            !mt_dma_span32(banks[i].address, bytes) ||
            (va < descriptors_va + count * sizeof(mt_desc) && descriptors_va < va + bytes))
            return MT_INVALID;
        for (j = 0; j < i; ++j) {
            uintptr_t other = (uintptr_t)banks[j].virtual_address;
            if ((banks[i].address < banks[j].address + banks[j].bytes &&
                 banks[j].address < banks[i].address + bytes) ||
                (va < other + banks[j].bytes && other < va + bytes)) return MT_INVALID;
        }
    }
    r->descriptors = descriptors; r->address = 0;
    r->banks = banks; r->bank_count = bank_count; r->slots_per_bank = slots_per_bank;
    r->count = count; r->stride = stride; r->capacity = capacity;
    r->head = count - 1; r->tail = 0; r->fault = 0;
    for (i = 0; i < count - 1; ++i) prepare(r, i);
    descriptors[count - 1].buf0 = 0; descriptors[count - 1].buf1 = 0;
    descriptors[count - 1].info = 0; descriptors[count - 1].ctrl = MT_DMA_DONE;
    return MT_OK;
}

int mt_rx_receive(mt_rx_ring *r, const uint8_t *pool, uint8_t *output,
                  uint32_t capacity, uint32_t *length, const mt_rx_io *io) {
    mt_desc *d;
    const uint8_t *payload;
    uint32_t ctrl, size, i, spare;
    if (length) *length = 0;
    if (!r || !r->descriptors || (!r->banks && !pool) || !output || !length || !io ||
        !io->barrier || !io->publish || r->count < 3 || r->count > 4096 ||
        r->head >= r->count || r->tail >= r->count ||
        (r->banks && (!r->slots_per_bank || !r->bank_count ||
            (r->count - 1) / r->slots_per_bank >= r->bank_count))) return MT_INVALID;
    if (r->fault) return r->fault;
    d = &r->descriptors[r->tail];
    ctrl = ((volatile mt_desc *)d)->ctrl;
    if (!(ctrl & MT_DMA_DONE)) return MT_PENDING;
    io->barrier(io->context); /* Acquire the payload after observing DMA_DONE. */
    ctrl = ((volatile mt_desc *)d)->ctrl;
    size = (ctrl >> 16) & MT_DMA_MAX_LENGTH;
    if (!(ctrl & MT_DMA_DONE) || !(ctrl & MT_DMA_LAST0) || !size ||
        size > r->capacity || (ctrl & 0xffffu) || d->buf1 ||
        d->buf0 != (uint32_t)slot_address(r, r->tail)) {
        r->fault = MT_INVALID; return r->fault;
    }
    /* A small consumer buffer can be retried without returning ownership. */
    if (capacity < size) return MT_FULL;
    payload = r->banks ? r->banks[r->tail / r->slots_per_bank].virtual_address +
        (uint64_t)(r->tail % r->slots_per_bank) * r->stride :
        pool + (uint64_t)r->tail * r->stride;
    for (i = 0; i < size; ++i) output[i] = payload[i];
    spare = r->head;
    prepare(r, spare);
    io->barrier(io->context); /* Release the entire descriptor before doorbell. */
    if (io->publish(io->context, (spare + 1) % r->count) != MT_OK) {
        r->fault = MT_INVALID; return r->fault;
    }
    r->head = (spare + 1) % r->count;
    r->tail = (r->tail + 1) % r->count;
    *length = size;
    return MT_OK;
}
