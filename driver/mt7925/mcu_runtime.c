/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Ring addresses/counts follow mt7925/pci.c and mt792x_regs.h (MediaTek).
 * Windows fixed-pool integration and access gating are port additions.
 */
#include "mcu_runtime.h"

/* Preserve the first runtime failure through STOP, without further MMIO.
 * Paths: MCU receive1, data receive2, TX DMA poll3. Payload is a bounded prefix.
 */
static void event_fault(mt_mcu_runtime *r,uint32_t path,const uint8_t *p,uint32_t size,int error) {
    uint32_t i;
    if (!error || error==MT_PENDING || error==1 || r->event_path) return;
    r->event_path=path;r->event_size=size;r->event_result=error;
    for(i=0;i<sizeof(r->event_prefix);++i)r->event_prefix[i]=p && i<size?p[i]:0;
}

static int read_register(void *context, uint32_t reg, uint32_t *value) {
    mt_mcu_runtime *r = context;
    if (!r->active) return MT_INVALID;
    return r->hardware.read(r->hardware.context, reg, value);
}
static int write_register(void *context, uint32_t reg, uint32_t value) {
    mt_mcu_runtime *r = context;
    if (!r->active) return MT_INVALID;
    /* Fixed MCU doorbells and, only when explicitly prepared, data RXQ2. */
    if ((reg != 0xd43f8u || value >= MT_MCU_ENTRIES) &&
        (reg != 0xd4408u || value >= MT_FWDL_ENTRIES) &&
        (reg != 0xd4508u || value >= MT_RX_ENTRIES) &&
        (reg != 0xd4528u || !r->data_prepared || value >= MT_DATA_RX_ENTRIES)) return MT_INVALID;
    return r->hardware.write(r->hardware.context, reg, value);
}
static void barrier(void *context) {
    mt_mcu_runtime *r = context;
    r->hardware.barrier(r->hardware.context);
}
static uint64_t milliseconds(void *context) {
    mt_mcu_runtime *r = context;
    return r->hardware.milliseconds(r->hardware.context);
}
static void pause_poll(void *context) {
    mt_mcu_runtime *r = context;
    r->hardware.pause(r->hardware.context);
}
static int publish(void *context, uint32_t index) {
    return write_register(context, 0xd4508u, index);
}
static int publish_data(void *context, uint32_t index) {
    return write_register(context, 0xd4528u, index);
}
static int publish_tx(void *context,uint32_t index) {
    mt_mcu_runtime *r=context;
    int error;
    if (!r->active || !r->data_tx || !r->data_tx->active || r->mcu.fault ||
        r->rx.fault || r->data_rx.fault || !r->mac_initialized ||
        !r->power_configured || !r->tx_initialized || index>=MT_DATA_TX_ENTRIES) return MT_INVALID;
    error=r->hardware.write(r->hardware.context,0xd4308u,index);
    if (error) r->mcu.fault=error;
    return error;
}
int mt_runtime_tx_poll(mt_mcu_runtime *r) {
    uint32_t index=0xffffffffu;
    int error;
    if (!r || !r->active || !r->data_tx || !r->data_tx->active) return MT_INVALID;
    if (r->mcu.fault) return r->mcu.fault;
    if (r->rx.fault) return r->rx.fault;
    if (r->data_rx.fault) return r->data_rx.fault;
    r->hardware.barrier(r->hardware.context);
    error=r->hardware.read(r->hardware.context,0xd430cu,&index);
    r->tx_dma_index=index;
    if (!error) error=mt_raw_tx_dma(r->data_tx,index);
    if (error && error!=MT_PENDING) {
        event_fault(r,3,0,0,error);
        r->mcu.fault=error;mt_raw_tx_disarm(r->data_tx);
    }
    return error;
}
/* Return1 for consumed TXS/TX-free, zero for other packets. Both receive
 * queues route these events, including during synchronous MCU commands.
 * TXS is observation only and never supplies either token ownership fence. */
static int tx_event(mt_mcu_runtime *r,const uint8_t *p,uint32_t size) {
    int error;
    if (size>=4 && (p[3]>>3)==0) {
        if (r->data_tx) mt_raw_tx_txs(r->data_tx,p,size);
        return 1;
    }
    if (size<4 || (p[3]>>3)!=6) return 0;
    ++r->tx_free_events;
    if (!r->data_tx) return 1;
    if (!r->data_tx->active) {
        if (r->data_tx->inflight) {r->mcu.fault=MT_FW_PROTOCOL;return MT_FW_PROTOCOL;}
        return 1;
    }
    error=mt_runtime_tx_poll(r);
    if (error && error!=MT_PENDING) return error;
    error=mt_raw_tx_free(r->data_tx,p,size);
    if (error) {r->mcu.fault=error;return error;}
    return 1;
}
static int receive(void *context, uint8_t *output, uint32_t capacity, uint32_t *size) {
    mt_mcu_runtime *r = context;
    int error;
    if (size) *size = 0;
    if (!r->active) return MT_INVALID;
    error=mt_rx_receive(&r->rx, r->rx_pool, output, capacity, size, &r->rx_io);
    if (!error) {
        ++r->mcu_events;
        error=tx_event(r,output,*size);
        event_fault(r,1,output,*size,error);
        if (error) {*size=0;return error==1?MT_PENDING:error;}
    }
    return error;
}

int mt_runtime_service(mt_mcu_runtime *r,uint32_t budget,uint32_t *consumed) {
    uint32_t size=0,n;
    int error;
    if (consumed) *consumed=0;
    if (!r || !r->active || !consumed || !budget || budget>64) return MT_INVALID;
    if (r->mcu.fault) return r->mcu.fault;
    if (r->rx.fault) return r->rx.fault;
    if (r->data_tx && r->data_tx->active) {
        error=mt_runtime_tx_poll(r);
        if (error && error!=MT_PENDING) return error;
    }
    for (n=0;n<budget;++n) {
        error=mt_rx_receive(&r->rx,r->rx_pool,r->response,MT_RX_BYTES,&size,&r->rx_io);
        if (error==MT_PENDING) break;
        if (!error) {
            ++r->mcu_events;
            ++*consumed;
            error=tx_event(r,r->response,size);
            if (error==1) continue;
            /* No outstanding command here. Validate the MCU envelope before
             * discarding unsolicited/stale payloads, without fabricating ACKs. */
            /* MT7925 RX events carry packet type0x3800, as observed in r2-r7;
             * 0xe000 was a synthetic fixture, not this chip's receive envelope. */
            if (!error && (size<44 || r->response[34]!=0 || r->response[35]!=0x38 ||
                (uint32_t)r->response[32]+((uint32_t)r->response[33]<<8)!=size-32))
                error=MT_FW_PROTOCOL;
        }
        if (error) {event_fault(r,1,r->response,size,error);r->mcu.fault=error;return error;}
    }
    return *consumed ? MT_OK : MT_PENDING;
}

int mt_runtime_prepare(mt_mcu_runtime *r, const mt_pcie_io *io,
    mt_desc *desc, uint64_t desc_address, uint32_t desc_bytes,
    uint8_t *packets, uint64_t packet_address, uint32_t packet_bytes) {
    int error;
    if (!r || r->prepared || !io || !io->read || !io->write || !io->barrier ||
        !io->milliseconds || !io->pause || !desc || !packets ||
        desc_bytes < MT_DESCRIPTOR_BYTES || packet_bytes < MT_POOL_BYTES ||
        (desc_address & 15u) || !mt_dma_span32(desc_address, MT_DESCRIPTOR_BYTES) ||
        !mt_dma_span32(packet_address, MT_POOL_BYTES) ||
        (desc_address < packet_address + MT_POOL_BYTES &&
         packet_address < desc_address + MT_DESCRIPTOR_BYTES)) return MT_INVALID;
    /* Every fallible size/address/callback check precedes descriptor changes. */
    r->hardware.context = io->context;
    r->hardware.read = io->read; r->hardware.write = io->write;
    r->hardware.barrier = io->barrier; r->hardware.milliseconds = io->milliseconds;
    r->hardware.pause = io->pause; r->hardware.receive = 0;
    r->active = 0; r->used = 0;
    r->descriptor_address = desc_address;
    r->mcu_banks = 0; r->mcu_bank_count = 0;
    r->rx_pool = packets + MT_TX_POOL_BYTES;
    error = mt_ring_init(&r->command, desc, MT_MCU_ENTRIES);
    if (error) return error;
    error = mt_ring_init(&r->download, desc + MT_MCU_ENTRIES, MT_FWDL_ENTRIES);
    if (error) return error;
    error = mt_rx_init(&r->rx, desc + MT_MCU_ENTRIES + MT_FWDL_ENTRIES,
        packet_address + MT_TX_POOL_BYTES, MT_RX_ENTRIES, MT_RX_BYTES, MT_RX_BYTES);
    if (error) return error;
    r->rx_io.context = r; r->rx_io.barrier = barrier; r->rx_io.publish = publish;
    r->mcu.io.context = r;
    r->mcu.io.read = read_register; r->mcu.io.write = write_register;
    r->mcu.io.barrier = barrier; r->mcu.io.milliseconds = milliseconds;
    r->mcu.io.pause = pause_poll; r->mcu.io.receive = receive;
    r->mcu.command.ring = &r->command;
    r->mcu.command.packets = packets;
    r->mcu.command.address = packet_address;
    r->mcu.command.banks = 0; r->mcu.command.bank_count = 0; r->mcu.command.slots_per_bank = 0;
    r->mcu.command.stride = MT_SLOT_BYTES;
    r->mcu.command.registers = 0xd43f0u;
    r->mcu.download.ring = &r->download;
    r->mcu.download.packets = packets + MT_MCU_ENTRIES * MT_SLOT_BYTES;
    r->mcu.download.address = packet_address + MT_MCU_ENTRIES * MT_SLOT_BYTES;
    r->mcu.download.banks = 0; r->mcu.download.bank_count = 0; r->mcu.download.slots_per_bank = 0;
    r->mcu.download.stride = MT_SLOT_BYTES;
    r->mcu.download.registers = 0xd4400u;
    r->mcu.response = r->response;
    r->mcu.response_capacity = MT_RX_BYTES;
    r->mcu.fault = 0;
    error = mt_pcie_bind(&r->mcu, &r->firmware);
    if (!error) r->prepared = 1;
    return error;
}

static int overlap(uint64_t a, uint32_t as, uint64_t b, uint32_t bs) {
    return a < b + bs && b < a + as;
}
int mt_runtime_prepare_banked(mt_mcu_runtime *r, const mt_pcie_io *io,
    mt_desc *desc, uint64_t desc_address, uint32_t desc_bytes,
    const mt_rx_bank *banks, uint32_t bank_count) {
    uint32_t i;
    int error;
    uintptr_t va = (uintptr_t)desc;
    if (!r || r->prepared || !io || !io->read || !io->write || !io->barrier ||
        !io->milliseconds || !io->pause || !desc || !banks ||
        bank_count != MT_MCU_BANK_COUNT || desc_bytes < MT_DESCRIPTOR_BYTES ||
        (desc_address & 15u) || !mt_dma_span32(desc_address, MT_DESCRIPTOR_BYTES) ||
        va > UINTPTR_MAX - MT_DESCRIPTOR_BYTES ||
        mt_pcie_validate_banks(banks, bank_count, MT_MCU_BANK_BYTES)) return MT_INVALID;
    if (overlap(va, MT_DESCRIPTOR_BYTES, (uintptr_t)r, (uint32_t)sizeof(*r)) ||
        overlap(va, MT_DESCRIPTOR_BYTES, (uintptr_t)banks, bank_count*(uint32_t)sizeof(*banks)) ||
        overlap((uintptr_t)r, (uint32_t)sizeof(*r),
                (uintptr_t)banks, bank_count*(uint32_t)sizeof(*banks))) return MT_INVALID;
    /* Validate the complete table, including RX banks and cross-queue aliases,
     * before touching either descriptor memory or the runtime object. */
    for (i=0; i<bank_count; ++i) {
        if (overlap(desc_address, MT_DESCRIPTOR_BYTES, banks[i].address, banks[i].bytes) ||
            overlap(va, MT_DESCRIPTOR_BYTES, (uintptr_t)banks[i].virtual_address, banks[i].bytes) ||
            overlap((uintptr_t)r, (uint32_t)sizeof(*r),
                    (uintptr_t)banks[i].virtual_address, banks[i].bytes))
            return MT_INVALID;
    }
    r->hardware.context = io->context;
    r->hardware.read = io->read; r->hardware.write = io->write;
    r->hardware.barrier = io->barrier; r->hardware.milliseconds = io->milliseconds;
    r->hardware.pause = io->pause; r->hardware.receive = 0;
    r->active = 0; r->used = 0;
    r->descriptor_address = desc_address; r->rx_pool = 0;
    r->mcu_banks = banks; r->mcu_bank_count = bank_count;
    error = mt_ring_init(&r->command, desc, MT_MCU_ENTRIES);
    if (error) return error;
    error = mt_ring_init(&r->download, desc + MT_MCU_ENTRIES, MT_FWDL_ENTRIES);
    if (error) return error;
    error = mt_rx_init_banked(&r->rx, desc + MT_MCU_ENTRIES + MT_FWDL_ENTRIES,
        banks + MT_MCU_COMMAND_BANKS + MT_MCU_DOWNLOAD_BANKS, MT_MCU_RX_BANKS,
        MT_MCU_BANK_BYTES / MT_RX_BYTES, MT_RX_ENTRIES, MT_RX_BYTES, MT_RX_BYTES);
    if (error) return error;
    r->rx_io.context = r; r->rx_io.barrier = barrier; r->rx_io.publish = publish;
    r->mcu.io.context = r;
    r->mcu.io.read = read_register; r->mcu.io.write = write_register;
    r->mcu.io.barrier = barrier; r->mcu.io.milliseconds = milliseconds;
    r->mcu.io.pause = pause_poll; r->mcu.io.receive = receive;
    r->mcu.command.ring = &r->command;
    r->mcu.command.packets = 0; r->mcu.command.address = 0;
    r->mcu.command.banks = banks; r->mcu.command.bank_count = MT_MCU_COMMAND_BANKS;
    r->mcu.command.slots_per_bank = MT_MCU_BANK_BYTES / MT_SLOT_BYTES;
    r->mcu.command.stride = MT_SLOT_BYTES; r->mcu.command.registers = 0xd43f0u;
    r->mcu.download.ring = &r->download;
    r->mcu.download.packets = 0; r->mcu.download.address = 0;
    r->mcu.download.banks = banks + MT_MCU_COMMAND_BANKS;
    r->mcu.download.bank_count = MT_MCU_DOWNLOAD_BANKS;
    r->mcu.download.slots_per_bank = MT_MCU_BANK_BYTES / MT_SLOT_BYTES;
    r->mcu.download.stride = MT_SLOT_BYTES; r->mcu.download.registers = 0xd4400u;
    r->mcu.response = r->response; r->mcu.response_capacity = MT_RX_BYTES;
    r->mcu.fault = 0;
    error = mt_pcie_bind(&r->mcu, &r->firmware);
    if (!error) r->prepared = 1;
    return error;
}

/* Checks the complete MCU allocation, not a fictitious span from bank0.
 * Metadata was validated at prepare and is immutable throughout DMA lifetime. */
static int mcu_overlap(const mt_mcu_runtime *r, const void *memory, uint64_t address, uint32_t bytes) {
    uint32_t i;
    uintptr_t va = (uintptr_t)memory;
    if (!va || va > UINTPTR_MAX - bytes ||
        overlap(address, bytes, r->descriptor_address, MT_DESCRIPTOR_BYTES) ||
        overlap(va, bytes, (uintptr_t)r->command.descriptors, MT_DESCRIPTOR_BYTES)) return 1;
    if (r->mcu_banks) {
        for (i=0; i<r->mcu_bank_count; ++i) {
            const mt_rx_bank *bank = &r->mcu_banks[i];
            if (overlap(address, bytes, bank->address, bank->bytes) ||
                overlap(va, bytes, (uintptr_t)bank->virtual_address, bank->bytes)) return 1;
        }
        return 0;
    }
    return overlap(address, bytes, r->mcu.command.address, MT_POOL_BYTES) ||
        overlap(va, bytes, (uintptr_t)r->mcu.command.packets, MT_POOL_BYTES);
}
int mt_runtime_prepare_data_rx(mt_mcu_runtime *r, mt_desc *desc,
    uint64_t desc_address, uint32_t desc_bytes, uint8_t *packets,
    uint64_t packet_address, uint32_t packet_bytes) {
    int error;
    if (!r || !r->prepared || r->sealed || r->active || r->used ||
        r->data_prepared || r->mcu.fault || r->rx.fault || !desc || !packets ||
        desc_bytes < MT_DATA_RX_DESCRIPTOR_BYTES || packet_bytes < MT_DATA_RX_POOL_BYTES ||
        (desc_address & 15u) || !mt_dma_span32(desc_address, MT_DATA_RX_DESCRIPTOR_BYTES) ||
        !mt_dma_span32(packet_address, MT_DATA_RX_POOL_BYTES)) return MT_INVALID;
    if (overlap(desc_address, MT_DATA_RX_DESCRIPTOR_BYTES, packet_address, MT_DATA_RX_POOL_BYTES) ||
        mcu_overlap(r, desc, desc_address, MT_DATA_RX_DESCRIPTOR_BYTES) ||
        mcu_overlap(r, packets, packet_address, MT_DATA_RX_POOL_BYTES)) return MT_INVALID;
    error = mt_rx_init(&r->data_rx, desc, packet_address, MT_DATA_RX_ENTRIES,
                       MT_DATA_RX_STRIDE, MT_DATA_RX_CAPACITY);
    if (error) return error;
    r->data_rx_pool = packets; r->data_descriptor_address = desc_address;
    r->data_rx_io.context = r; r->data_rx_io.barrier = barrier;
    r->data_rx_io.publish = publish_data;
    r->data_prepared = 1;
    return MT_OK;
}
int mt_runtime_prepare_data_rx_banked(mt_mcu_runtime *r, mt_desc *desc,
    uint64_t desc_address, uint32_t desc_bytes, const mt_rx_bank *banks, uint32_t bank_count) {
    uint32_t i;
    int error;
    if (!r || !r->prepared || r->sealed || r->active || r->used || r->data_prepared ||
        r->mcu.fault || r->rx.fault || !desc || !banks || bank_count != MT_DATA_RX_BANK_COUNT ||
        desc_bytes < MT_DATA_RX_DESCRIPTOR_BYTES || (desc_address & 15u) ||
        !mt_dma_span32(desc_address, MT_DATA_RX_DESCRIPTOR_BYTES)) return MT_INVALID;
    if (mcu_overlap(r, desc, desc_address, MT_DATA_RX_DESCRIPTOR_BYTES)) return MT_INVALID;
    for (i = 0; i < bank_count; ++i) {
        if (!mt_dma_span32(banks[i].address, banks[i].bytes) ||
            overlap(banks[i].address, banks[i].bytes, desc_address, MT_DATA_RX_DESCRIPTOR_BYTES) ||
            mcu_overlap(r, banks[i].virtual_address, banks[i].address, banks[i].bytes)) return MT_INVALID;
    }
    error = mt_rx_init_banked(&r->data_rx, desc, banks, bank_count, MT_DATA_RX_BANK_SLOTS,
                             MT_DATA_RX_ENTRIES, MT_DATA_RX_STRIDE, MT_DATA_RX_CAPACITY);
    if (error) return error;
    r->data_rx_pool = 0; r->data_descriptor_address = desc_address;
    r->data_rx_io.context = r; r->data_rx_io.barrier = barrier;
    r->data_rx_io.publish = publish_data;
    r->data_prepared = 1;
    return MT_OK;
}
int mt_runtime_radio_receive(mt_mcu_runtime *r, uint8_t *scratch,
    uint32_t scratch_capacity, uint32_t fcs, uint8_t *out,
    uint32_t capacity, uint32_t *length) {
    uint32_t received=0;
    int error;
    if (length) *length = 0;
    if (!r || !r->active || !r->data_prepared || !out || !length || fcs>1) return MT_INVALID;
    if (r->mcu.fault) return r->mcu.fault;
    if (r->rx.fault) return r->rx.fault;
    error=mt_rx_receive(&r->data_rx,r->data_rx_pool,scratch,scratch_capacity,&received,&r->data_rx_io);
    if (error) {event_fault(r,2,0,0,error);return error;}
    ++r->data_events;
    error=tx_event(r,scratch,received);
    event_fault(r,2,scratch,received,error);
    if (error) return error==1?MT_PENDING:error;
    /* A malformed/unsupported air frame is dropped, not a runtime fault. */
    return mt_rx_radio_record_measured(scratch,received,fcs,out,capacity,length,&r->rx_stats);
}

int mt_runtime_prepare_data_tx(mt_mcu_runtime *r,mt_raw_tx *q,mt_desc *desc,uint64_t da,
    uint32_t db,uint8_t *pool,uint64_t pa,uint32_t pb,uint32_t *states,uint32_t *slots) {
    mt_tx_io io;
    uint32_t i;
    int error;
    if (!r || !r->prepared || !r->data_prepared || r->sealed || r->active || r->used ||
        r->data_tx || r->mcu.fault || r->rx.fault || r->data_rx.fault ||
        db<MT_DATA_TX_DESCRIPTOR_BYTES || pb<MT_DATA_TX_POOL_BYTES ||
        !mt_dma_span32(da,MT_DATA_TX_DESCRIPTOR_BYTES) || !mt_dma_span32(pa,MT_DATA_TX_POOL_BYTES)) return MT_INVALID;
    if (mcu_overlap(r,desc,da,MT_DATA_TX_DESCRIPTOR_BYTES) ||
        mcu_overlap(r,pool,pa,MT_DATA_TX_POOL_BYTES) ||
        overlap(da,MT_DATA_TX_DESCRIPTOR_BYTES,r->data_descriptor_address,MT_DATA_RX_DESCRIPTOR_BYTES) ||
        overlap(pa,MT_DATA_TX_POOL_BYTES,r->data_descriptor_address,MT_DATA_RX_DESCRIPTOR_BYTES)) return MT_INVALID;
    if (!r->data_rx.banks &&
        (overlap(da,MT_DATA_TX_DESCRIPTOR_BYTES,r->data_rx.address,MT_DATA_RX_POOL_BYTES) ||
         overlap(pa,MT_DATA_TX_POOL_BYTES,r->data_rx.address,MT_DATA_RX_POOL_BYTES))) return MT_INVALID;
    if (r->data_rx.banks) for (i=0;i<r->data_rx.bank_count;++i) {
        const mt_rx_bank *bank=&r->data_rx.banks[i];
        if (overlap(da,MT_DATA_TX_DESCRIPTOR_BYTES,bank->address,bank->bytes) ||
            overlap(pa,MT_DATA_TX_POOL_BYTES,bank->address,bank->bytes)) return MT_INVALID;
    }
    io.context=r;io.barrier=barrier;io.publish=publish_tx;
    error=mt_raw_tx_prepare(q,desc,da,MT_DATA_TX_ENTRIES,pool,pa,pb,states,MT_DATA_TX_TOKENS,slots,&io);
    if (!error) {r->data_tx=q;r->tx_descriptor_address=da;}
    return error;
}

static int match(mt_mcu_runtime *r, uint32_t reg, uint32_t mask, uint32_t expected) {
    uint32_t value;
    int error = r->hardware.read(r->hardware.context, reg, &value);
    if (error) return error;
    return value != 0xffffffffu && (value & mask) == expected ? MT_OK : MT_INVALID;
}
static int match_ring(mt_mcu_runtime *r, uint32_t reg, uint32_t address,
                      uint32_t count, uint32_t cpu) {
    int error = match(r, reg, 0xffffffffu, address);
    if (!error) error = match(r, reg+4, 0xffffffffu, count);
    if (!error) error = match(r, reg+8, 0xffffffffu, cpu);
    if (!error) error = match(r, reg+12, 0xffffffffu, 0);
    return error;
}
int mt_runtime_attach(mt_mcu_runtime *r) {
    int error;
    uint32_t base;
    if (!r || !r->prepared || r->active || r->used || r->mcu.fault || r->rx.fault ||
        (r->data_prepared && r->data_rx.fault) ||
        (r->data_tx && (!r->data_tx->prepared || r->data_tx->active || r->data_tx->used || r->data_tx->fault)))
        return MT_INVALID;
    r->sealed = 1; /* Even failed attachment cannot replace published buffers. */
    base = (uint32_t)r->descriptor_address;
    /* Driver owns the device; both engines enabled; 32-bit DMA addresses.
     * No pending firmware/IRQ consumer may race these checks. */
    error = match(r, 0xe0010u, 4u, 0);
    if (!error) error = match(r, 0xd4208u, 0x04000005u, 5u);
    if (!error) error = match_ring(r, 0xd43f0u, base, MT_MCU_ENTRIES, 0);
    if (!error) error = match_ring(r, 0xd4400u, base + MT_MCU_ENTRIES*16u, MT_FWDL_ENTRIES, 0);
    if (!error) error = match_ring(r, 0xd4500u,
        base + (MT_MCU_ENTRIES+MT_FWDL_ENTRIES)*16u, MT_RX_ENTRIES, MT_RX_ENTRIES-1u);
    if (!error && r->data_prepared) error = match_ring(r, 0xd4520u,
        (uint32_t)r->data_descriptor_address, MT_DATA_RX_ENTRIES, MT_DATA_RX_ENTRIES-1u);
    if (!error && r->data_tx) error = match_ring(r,0xd4300u,
        (uint32_t)r->tx_descriptor_address,MT_DATA_TX_ENTRIES,0);
    if (!error) {
        r->hardware.barrier(r->hardware.context);
        r->used = 1; r->active = 1;
    }
    return error;
}
void mt_runtime_disarm(mt_mcu_runtime *r) {
    if (r) {r->active = 0;r->tx_initialized=0;if (r->data_tx) mt_raw_tx_disarm(r->data_tx);}
}
