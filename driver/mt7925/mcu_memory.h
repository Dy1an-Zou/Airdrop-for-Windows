/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_MCU_MEMORY_H
#define MT7925_MCU_MEMORY_H
#include "data_memory.h"

/* System MTFL stages 100..139 identify the exact MCU bank allocation.
 * Runtime preparation retains the existing stage 5 diagnostic. */
#define MT_MCU_ALLOCATION_STAGE 100u
#define MT_MCU_PREPARE_STAGE 5u
typedef struct mt_mcu_memory {
    mt_data_block blocks[MT_MCU_BANK_COUNT];
    mt_rx_bank banks[MT_MCU_BANK_COUNT];
    uint32_t attempted;
} mt_mcu_memory;

/* Uses the same coherent-buffer allocator as data RX/TX. Never publishes DMA
 * or frees on failure: all acquired handles reach the existing PnP release
 * fence, including a failed bank allocation or runtime validation. */
static inline int mt_mcu_memory_prepare(mt_mcu_memory *m,mt_data_allocation *diagnostic,
    const mt_data_allocator *allocator,mt_mcu_runtime *runtime,const mt_pcie_io *io,
    mt_desc *descriptors,uint64_t descriptor_address,uint32_t descriptor_bytes) {
    uint32_t i;
    if (!m || !diagnostic || !allocator || !allocator->create || !allocator->release ||
        !runtime || !io || !descriptors || m->attempted) return MT_INVALID;
    for (i=0;i<MT_MCU_BANK_COUNT;++i) if (m->blocks[i].handle) return MT_INVALID;
    m->attempted=1;
    for (i=0;i<MT_MCU_BANK_COUNT;++i) {
        diagnostic->stage=MT_MCU_ALLOCATION_STAGE+i;
        diagnostic->bank_index=i;diagnostic->request_bytes=MT_MCU_BANK_BYTES;
        diagnostic->status=allocator->create(allocator->context,MT_MCU_BANK_BYTES,&m->blocks[i]);
        if (diagnostic->status) return diagnostic->status;
        m->banks[i].virtual_address=m->blocks[i].virtual_address;
        m->banks[i].address=m->blocks[i].address;
        m->banks[i].bytes=MT_MCU_BANK_BYTES;
    }
    diagnostic->stage=MT_MCU_PREPARE_STAGE;
    diagnostic->bank_index=0xffffffffu;diagnostic->request_bytes=0;
    diagnostic->status=mt_runtime_prepare_banked(runtime,io,descriptors,descriptor_address,
        descriptor_bytes,m->banks,MT_MCU_BANK_COUNT);
    return diagnostic->status;
}

/* Caller has revoked/zeroed runtime users and reached its original powered-off
 * release boundary. This helper does not stop DMA or perform MMIO. */
static inline void mt_mcu_memory_release(mt_mcu_memory *m,const mt_data_allocator *allocator) {
    uint32_t i;
    if (!m || !allocator || !allocator->release) return;
    for (i=MT_MCU_BANK_COUNT;i>0;--i) {
        mt_data_block *block=&m->blocks[i-1];
        if (block->handle) allocator->release(allocator->context,block->handle);
        block->handle=0;block->virtual_address=0;block->address=0;
        m->banks[i-1].virtual_address=0;m->banks[i-1].address=0;m->banks[i-1].bytes=0;
    }
    m->attempted=0;
}
#endif
