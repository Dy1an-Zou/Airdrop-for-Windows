/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_DATA_MEMORY_H
#define MT7925_DATA_MEMORY_H
#include "mcu_runtime.h"

#define MT_DATA_BLOCKS (MT_DATA_RX_BANK_COUNT + 3u)
#define MT_DATA_TX_DESC_BLOCK (MT_DATA_RX_BANK_COUNT + 1u)
#define MT_DATA_TX_POOL_BLOCK (MT_DATA_RX_BANK_COUNT + 2u)
typedef struct mt_data_block {
    void *handle;
    uint8_t *virtual_address;
    uint64_t address;
} mt_data_block;
typedef struct mt_data_allocation {
    uint32_t stage, bank_index, request_bytes;
    int32_t status;
} mt_data_allocation;
typedef struct mt_data_memory {
    mt_data_block blocks[MT_DATA_BLOCKS];
    mt_rx_bank banks[MT_DATA_RX_BANK_COUNT];
    uint32_t attempted;
} mt_data_memory;
typedef struct mt_data_allocator {
    void *context;
    /* A successful create returns one owned handle and its coherent VA/PA.
     * Failure leaves the zero-initialized output untouched. No MMIO here. */
    int32_t (*create)(void *,uint32_t,mt_data_block *);
    void (*release)(void *,void *);
} mt_data_allocator;

/* Shared by the WDF adapter and the offline allocation-failure consumer.
 * Partial failure deliberately retains every successful allocation. Only the
 * caller's existing ReleaseHardware fence permits mt_data_memory_release. */
static inline int mt_data_memory_prepare(mt_data_memory *m,mt_data_allocation *diagnostic,
    const mt_data_allocator *allocator,mt_mcu_runtime *runtime,mt_raw_tx *tx,
    uint32_t *states,uint32_t *slots) {
    uint32_t i,bytes,stage,bank;
    int result;
    mt_data_block *rd,*td,*tp;
    if (!m || !diagnostic || !allocator || !allocator->create || !allocator->release ||
        !runtime || !tx || !states || !slots || m->attempted) return MT_INVALID;
    for (i=0;i<MT_DATA_BLOCKS;++i) if (m->blocks[i].handle) return MT_INVALID;
    m->attempted=1;
    for (i=0;i<MT_DATA_BLOCKS;++i) {
        bank=0xffffffffu;
        if (!i) {stage=1;bytes=MT_DATA_RX_DESCRIPTOR_BYTES;}
        else if (i<=MT_DATA_RX_BANK_COUNT) {stage=2;bank=i-1;bytes=MT_DATA_RX_BANK_BYTES;}
        else if (i==MT_DATA_TX_DESC_BLOCK) {stage=3;bytes=MT_DATA_TX_DESCRIPTOR_BYTES;}
        else {stage=4;bytes=MT_DATA_TX_POOL_BYTES;}
        diagnostic->stage=stage;diagnostic->bank_index=bank;diagnostic->request_bytes=bytes;
        diagnostic->status=allocator->create(allocator->context,bytes,&m->blocks[i]);
        if (diagnostic->status) return diagnostic->status;
        if (stage==2) {
            m->banks[bank].virtual_address=m->blocks[i].virtual_address;
            m->banks[bank].address=m->blocks[i].address;
            m->banks[bank].bytes=bytes;
        }
    }
    rd=&m->blocks[0];td=&m->blocks[MT_DATA_TX_DESC_BLOCK];tp=&m->blocks[MT_DATA_TX_POOL_BLOCK];
    result=mt_runtime_prepare_data_rx_banked(runtime,(mt_desc *)rd->virtual_address,
        rd->address,MT_DATA_RX_DESCRIPTOR_BYTES,m->banks,MT_DATA_RX_BANK_COUNT);
    if (!result) result=mt_runtime_prepare_data_tx(runtime,tx,(mt_desc *)td->virtual_address,
        td->address,MT_DATA_TX_DESCRIPTOR_BYTES,tp->virtual_address,tp->address,
        MT_DATA_TX_POOL_BYTES,states,slots);
    return result;
}

/* Not a STOP operation. The platform first revokes/zeros all runtime users,
 * then calls this only at its established powered-off PnP release boundary. */
static inline void mt_data_memory_release(mt_data_memory *m,const mt_data_allocator *allocator) {
    uint32_t i;
    if (!m || !allocator || !allocator->release) return;
    for (i=MT_DATA_BLOCKS;i>0;--i) {
        mt_data_block *block=&m->blocks[i-1];
        if (block->handle) allocator->release(allocator->context,block->handle);
        block->handle=0;block->virtual_address=0;block->address=0;
    }
    for (i=0;i<MT_DATA_RX_BANK_COUNT;++i) {
        m->banks[i].virtual_address=0;m->banks[i].address=0;m->banks[i].bytes=0;
    }
    m->attempted=0;
}
#endif
