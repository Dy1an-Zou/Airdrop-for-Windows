/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_MCU_RUNTIME_H
#define MT7925_MCU_RUNTIME_H
#include "pcie_mcu.h"
#include "rx_ring.h"
#include "raw_rx.h"
#include "raw_tx.h"

#define MT_MCU_ENTRIES 256u
#define MT_FWDL_ENTRIES 128u
#define MT_RX_ENTRIES 512u
#define MT_SLOT_BYTES 16384u
#define MT_RX_BYTES 8192u
#define MT_TX_POOL_BYTES ((MT_MCU_ENTRIES + MT_FWDL_ENTRIES) * MT_SLOT_BYTES)
#define MT_POOL_BYTES (MT_TX_POOL_BYTES + MT_RX_ENTRIES * MT_RX_BYTES)
#define MT_DESCRIPTOR_BYTES ((MT_MCU_ENTRIES + MT_FWDL_ENTRIES + MT_RX_ENTRIES) * 16u)
#define MT_MCU_BANK_BYTES 262144u
#define MT_MCU_COMMAND_BANKS 16u
#define MT_MCU_DOWNLOAD_BANKS 8u
#define MT_MCU_RX_BANKS 16u
#define MT_MCU_BANK_COUNT (MT_MCU_COMMAND_BANKS + MT_MCU_DOWNLOAD_BANKS + MT_MCU_RX_BANKS)
#define MT_DATA_RX_ENTRIES 1536u
#define MT_DATA_RX_STRIDE 16384u
#define MT_DATA_RX_CAPACITY 16383u
#define MT_DATA_RX_POOL_BYTES (MT_DATA_RX_ENTRIES * MT_DATA_RX_STRIDE)
#define MT_DATA_RX_DESCRIPTOR_BYTES (MT_DATA_RX_ENTRIES * 16u)
#define MT_DATA_RX_BANK_SLOTS 16u
#define MT_DATA_RX_BANK_BYTES (MT_DATA_RX_BANK_SLOTS * MT_DATA_RX_STRIDE)
#define MT_DATA_RX_BANK_COUNT (MT_DATA_RX_ENTRIES / MT_DATA_RX_BANK_SLOTS)
#define MT_DATA_TX_ENTRIES 2048u
#define MT_DATA_TX_TOKENS 256u
#define MT_DATA_TX_DESCRIPTOR_BYTES (MT_DATA_TX_ENTRIES * 16u)
#define MT_DATA_TX_POOL_BYTES (MT_DATA_TX_TOKENS * MT_TX_SLOT_BYTES)

/* Stable, nonpaged storage owned by the serialized platform caller. Not movable
 * after prepare: callbacks refer back to this object. No function stops DMA or
 * makes freeing a DMA allocation safe; the platform must do that separately. */
typedef struct mt_mcu_runtime {
    mt_pcie_io hardware;
    mt_ring command, download;
    mt_rx_ring rx;
    mt_pcie_mcu mcu;
    mt_fw_transport firmware;
    mt_rx_io rx_io;
    uint8_t *rx_pool;
    uint64_t descriptor_address;
    uint32_t prepared, active, used;
    uint8_t response[MT_RX_BYTES];
    mt_rx_ring data_rx;
    mt_rx_io data_rx_io;
    uint8_t *data_rx_pool;
    uint64_t data_descriptor_address;
    uint32_t data_prepared, sealed;
    uint32_t mac_attempted, mac_initialized, mac_step, mac_index;
    uint32_t clc_attempted, power_configured;
    mt_raw_tx *data_tx;
    uint64_t tx_descriptor_address;
    uint32_t tx_init_attempted, tx_initialized;
    uint32_t mac_reg, mac_expected, mac_observed, mac_mask;
    int32_t mac_io_error;
    uint32_t event_path, event_size;
    int32_t event_result;
    uint8_t event_prefix[128];
    uint32_t tx_dma_index, mcu_events, data_events, tx_free_events;
    mt_rx_stats rx_stats;
    const mt_rx_bank *mcu_banks;
    uint32_t mcu_bank_count;
} mt_mcu_runtime;

/* Software-only, for a zero-initialized object and disjoint coherent buffers.
 * hardware.receive is ignored: RX is always handled by the native ring here. */
MT_API int mt_runtime_prepare(mt_mcu_runtime *, const mt_pcie_io *,
    mt_desc *, uint64_t, uint32_t, uint8_t *, uint64_t, uint32_t);
/* Same MCU/FWDL/RX queues, backed by 40 independent 256 KiB allocations:
 * command banks0..15 (16 slots/bank), FWDL16..23 (16), RX24..39 (32).
 * The immutable bank table and memory remain owned by the platform until its
 * powered-off release fence. All addresses are validated before mutation. */
MT_API int mt_runtime_prepare_banked(mt_mcu_runtime *, const mt_pcie_io *,
    mt_desc *, uint64_t, uint32_t, const mt_rx_bank *, uint32_t);
/* Optional data RXQ2. Prepare BEFORE chip_start/attach, using separate coherent
 * allocations. No MMIO, no allocation or free here. The normal MCU diagnostic
 * never calls this. Full-sized buffers avoid splitting legal 802.11 MPDUs;
 * hardware fragments still fail closed until DMA shutdown and fresh prepare.
 * All buffers must remain alive through the same chip stop/OS removal fence. */
MT_API int mt_runtime_prepare_data_rx(mt_mcu_runtime *, mt_desc *, uint64_t,
    uint32_t, uint8_t *, uint64_t, uint32_t);
/* Same queue geometry, using 96 independently allocated 256 KiB banks.
 * The bank table is borrowed and must not move/change until DMA is stopped. */
MT_API int mt_runtime_prepare_data_rx_banked(mt_mcu_runtime *, mt_desc *, uint64_t,
    uint32_t, const mt_rx_bank *, uint32_t);
/* Optional data TX0, after data RX prepare and before chip start. Metadata arrays
 * have MT_DATA_TX_TOKENS / MT_DATA_TX_ENTRIES entries. No automatic activation:
 * the complete radio session must initialize WCID/power/channel before calling
 * mt_raw_tx_activate. Prepare/start/stop never send an 802.11 packet. */
MT_API int mt_runtime_prepare_data_tx(mt_mcu_runtime *,mt_raw_tx *,mt_desc *,uint64_t,
    uint32_t,uint8_t *,uint64_t,uint32_t,uint32_t *,uint32_t *);
MT_API int mt_runtime_tx_poll(mt_mcu_runtime *);
/* Idle MCU consumer: serialized with all command exchanges. Routes TX-free,
 * drains bounded unsolicited/stale MCU events; never consumes data RXQ2.
 * Packet count includes notifications. Caller schedules repeated service. */
MT_API int mt_runtime_service(mt_mcu_runtime *,uint32_t budget,uint32_t *consumed);
/* Serialized, passive-level data consumer. Explicit platform FCS policy.
 * Scratch >= MT_DATA_RX_CAPACITY and output >= that + 22 guarantee space;
 * small output may consume a packet, as documented by mt_radio_receive.
 * Disarm/stop revokes this path before any buffer or doorbell access. */
MT_API int mt_runtime_radio_receive(mt_mcu_runtime *, uint8_t *, uint32_t,
    uint32_t, uint8_t *, uint32_t, uint32_t *);
/* Read-only verification after platform ownership/reset/ring setup/DMA enable.
 * Checks the exact allocations and reset indices before permitting doorbells.
 * One activation per preparation; timeout/stop requires hardware reset and a
 * fresh preparation, not merely toggling active back on. */
MT_API int mt_runtime_attach(mt_mcu_runtime *);
MT_API void mt_runtime_disarm(mt_mcu_runtime *);
#endif
