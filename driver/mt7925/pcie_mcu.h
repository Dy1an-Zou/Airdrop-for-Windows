/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_PCIE_MCU_H
#define MT7925_PCIE_MCU_H
#include "firmware.h"
#include "nic_caps.h"
#include "rx_ring.h"
#include "tx_power.h"
/* Serialized MCU transport over already configured, coherent PCIe rings.
 * Platform owns power/DMA start/stop and RX replenishment. Binding is software
 * only; callbacks must reject access until ownership and DMA setup are verified.
 * Any timeout poisons the channel until a hardware reset: sequence numbers
 * alone cannot distinguish an old reply after their 4-bit wraparound. */
typedef struct mt_pcie_io {
    void *context;
    int (*read)(void *, uint32_t, uint32_t *);
    int (*write)(void *, uint32_t, uint32_t);
    void (*barrier)(void *);
    uint64_t (*milliseconds)(void *);
    void (*pause)(void *);
    /* Copy one complete MCU event, with its 32-byte RX descriptor, into out.
     * MT_PENDING = no event; other failures must stop the channel. */
    int (*receive)(void *, uint8_t *out, uint32_t capacity, uint32_t *length);
} mt_pcie_io;
typedef struct mt_pcie_queue {
    mt_ring *ring;
    uint8_t *packets;
    uint64_t address;
    uint32_t stride, registers;
    /* Optional independent coherent banks; packets/address are zero in this
     * mode. The caller retains immutable metadata and storage until DMA stops. */
    const mt_rx_bank *banks;
    uint32_t bank_count, slots_per_bank;
} mt_pcie_queue;
typedef struct mt_pcie_mcu {
    mt_pcie_io io;
    mt_pcie_queue command, download;
    uint8_t *response;
    uint32_t response_capacity;
    uint8_t sequence;
    int fault;
    /* Last operation is retained when poisoned; cleanup must not erase it.
     * stage: 0 idle, 1 TX space, 2 doorbell, 3 reply, 4 drain,
     * 5 ROM state, 6 running state. No hardware access to read these fields. */
    uint32_t stage, last_command, last_state;
    /* Last exchange only; bounded initialized copy, retained through stop. */
    uint32_t response_count, response_size;
    int32_t response_result;
    uint8_t response_prefix[128];
    uint32_t first_size;
    int32_t first_result;
    uint8_t first_prefix[128];
} mt_pcie_mcu;
/* Read-only validation shared with runtime before any descriptor is modified. */
MT_API int mt_pcie_validate_banks(const mt_rx_bank *, uint32_t bank_count,
    uint32_t bytes_per_bank);
MT_API int mt_pcie_bind(mt_pcie_mcu *, mt_fw_transport *);
/* Only after firmware boot; same serialized transport and timeout fault latch.
 * Sends the fixed CHIP_CONFIG/NIC_CAPA request, never arbitrary register access. */
MT_API int mt_pcie_query_nic(mt_pcie_mcu *, mt_nic_caps *);
/* One fixed tag7/category2/band0 target-table read, after RF configuration.
 * Caller owns zero-initialized result; attempted forbids a second query. */
MT_API int mt_pcie_query_txpower(mt_pcie_mcu *, mt_txpower_result *);
#include "efuse_type.h"
/* Fixed read-only EFUSE block query. No user-selected address or EFUSE write. */
MT_API int mt_pcie_query_hw_type(mt_pcie_mcu *, mt_hw_type *);
#include "regulatory.h"
/* Requires decoded NIC capability bit5 and EFUSE-selected variant at caller.
 * Scratch is disjoint/nonpaged, held through the synchronous exchange.
 * No channel/monitor activation; firmware decodes its own signed country data. */
MT_API int mt_pcie_query_regdomain(mt_pcie_mcu *, const uint8_t *, uint32_t,
    uint32_t hw_encap, const uint8_t alpha2[2], uint8_t *, uint32_t, mt_regdomain *);
#endif
