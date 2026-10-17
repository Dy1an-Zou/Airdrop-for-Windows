/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2020, 2023 MediaTek Inc.
 * Register sequences adapted from mt76 mt792x_core.c, mt792x_dma.c and
 * mt7925/pci.c. Bounded error propagation, polling-only MCU rings and lifecycle
 * state are Windows port additions. See SOURCES.md for the pinned revision.
 */
#include "chip_control.h"

#define LPCTL 0xe0010u
#define WFSYS_RESET 0xf0140u
#define L1_REMAP 0x155024u
#define EMI_WINDOW 0x131100u
#define DMA_CFG 0xd4208u
#define DMA_ENABLE 5u
#define DMA_BUSY 10u
#define DMA_DISABLE_MASK 0x18208005u
#define DMA_SETUP 0x5020b870u

static int rd(mt_chip *c, uint32_t reg, uint32_t *value) {
    mt_pcie_io *io = &c->runtime->hardware;
    int error = io->read(io->context, reg, value);
    if (error) return error;
    return *value == 0xffffffffu ? MT_FW_PROTOCOL : MT_OK;
}
static int wr(mt_chip *c, uint32_t reg, uint32_t value) {
    mt_pcie_io *io = &c->runtime->hardware;
    io->barrier(io->context);
    return io->write(io->context, reg, value);
}
static int rmw(mt_chip *c, uint32_t reg, uint32_t clear, uint32_t set) {
    uint32_t value;
    int error = rd(c, reg, &value);
    return error ? error : wr(c, reg, (value & ~clear) | set);
}
static int equal(mt_chip *c, uint32_t reg, uint32_t mask, uint32_t expected) {
    uint32_t value;
    int error = rd(c, reg, &value);
    return error ? error : (value & mask) == expected ? MT_OK : MT_FW_PROTOCOL;
}
static int poll(mt_chip *c, uint32_t reg, uint32_t mask,
                uint32_t expected, uint32_t timeout) {
    mt_pcie_io *io = &c->runtime->hardware;
    uint64_t start = io->milliseconds(io->context);
    uint32_t value;
    for (;;) {
        int error = rd(c, reg, &value);
        if (error) return error;
        if ((value & mask) == expected) return MT_OK;
        if (io->milliseconds(io->context) - start >= timeout) return MT_FW_TIMEOUT;
        io->pause(io->context);
    }
}
static void delay(mt_chip *c, uint32_t ms) {
    mt_pcie_io *io = &c->runtime->hardware;
    uint64_t start = io->milliseconds(io->context);
    while (io->milliseconds(io->context) - start < ms) io->pause(io->context);
}
static int ownership(mt_chip *c, int firmware) {
    unsigned i;
    int error;
    for (i = 0; i < 10; ++i) {
        error = wr(c, LPCTL, firmware ? 1u : 2u);
        if (error) return error;
        if (!firmware) delay(c, 3); /* Also covers an ASPM-enabled device. */
        error = poll(c, LPCTL, 4, firmware ? 4u : 0u, 50);
        if (error != MT_FW_TIMEOUT) return error;
    }
    return MT_FW_TIMEOUT;
}
static int emi_protection(mt_chip *c) {
    uint32_t saved, mapped;
    int error, restore;
    error = rd(c, L1_REMAP, &saved);
    if (error) return error;
    mapped = (saved & 0xffffu) | 0x18010000u;
    /* Even a failed write can have reached the device. Always restore the
     * previous mapping, including zero, before leaving this operation. */
    error = wr(c, L1_REMAP, mapped);
    if (!error) error = equal(c, L1_REMAP, 0xffffffffu, mapped);
    if (!error) error = rmw(c, EMI_WINDOW, 0, 2);
    if (!error) error = equal(c, EMI_WINDOW, 2, 2);
    restore = wr(c, L1_REMAP, saved);
    if (!restore) restore = equal(c, L1_REMAP, 0xffffffffu, saved);
    return error ? error : restore;
}
static int reset_wfsys(mt_chip *c) {
    int error = rmw(c, WFSYS_RESET, 1, 0);
    if (error) return error;
    error = equal(c, WFSYS_RESET, 1, 0);
    if (error) return error;
    delay(c, 50);
    error = rmw(c, WFSYS_RESET, 0, 1);
    if (!error) error = poll(c, WFSYS_RESET, 0x11, 0x11, 500);
    return error;
}
static int mask_interrupts(mt_chip *c) {
    int error = wr(c, 0xd4204u, 0);
    if (!error) error = wr(c, 0x10188u, 0);
    if (!error) error = wr(c, 0xd41f4u, 0);
    if (!error) error = equal(c, 0xd4204u, 0xffffffffu, 0);
    if (!error) error = equal(c, 0x10188u, 0xffffffffu, 0);
    if (!error) error = equal(c, 0xd41f4u, 0xffffffffu, 0);
    return error;
}
static int disable_dma(mt_chip *c) {
    int error = rmw(c, DMA_CFG, DMA_DISABLE_MASK, 0);
    /* An ignored disable write must not be accepted merely because BUSY=0. */
    if (!error) error = poll(c, DMA_CFG, DMA_ENABLE | DMA_BUSY, 0, 100);
    if (!error) error = rmw(c, 0xd42b0u, 0x40u, 0);
    if (!error) error = rmw(c, 0xd6004u, 0, 0x10000000u);
    if (!error) error = rmw(c, 0xd4100u, 0x30u, 0);
    if (!error) error = rmw(c, 0xd4100u, 0, 0x30u);
    return error;
}
static int ring(mt_chip *c, uint32_t reg, uint32_t address,
                uint32_t count, uint32_t cpu) {
    int error = wr(c, reg + 4, 0); /* No partially configured live ring. */
    if (!error) error = wr(c, reg, address);
    if (!error) error = wr(c, reg + 8, cpu);
    if (!error) error = wr(c, reg + 4, count);
    if (!error) error = equal(c, reg, 0xffffffffu, address);
    if (!error) error = equal(c, reg+4, 0xffffffffu, count);
    if (!error) error = equal(c, reg+8, 0xffffffffu, cpu);
    return error;
}
static int clear_rings(mt_chip *c) {
    unsigned i;
    int error;
    /* MT7925 has TX0..16 and RX0..3. Clear inherited vendor allocations,
     * including data/event queues that this MCU-only stage does not populate. */
    for (i = 0; i < 17; ++i) {
        error = ring(c, 0xd4300u + i*16u, 0, 0, 0);
        if (error) return error;
    }
    for (i = 0; i < 4; ++i) {
        error = ring(c, 0xd4500u + i*16u, 0, 0, 0);
        if (error) return error;
    }
    return MT_OK;
}
static int setup_dma(mt_chip *c) {
    static const uint32_t prefetch[][2] = {
        {0xd4680u,0x00000004u},{0xd4684u,0x00400004u},
        {0xd4688u,0x00800004u},{0xd468cu,0x00c00004u},
        {0xd4600u,0x01000010u},{0xd4604u,0x02000010u},
        {0xd4608u,0x03000010u},{0xd460cu,0x04000010u},
        {0xd463cu,0x05000004u},{0xd4640u,0x05400004u}
    };
    uint32_t base = (uint32_t)c->runtime->descriptor_address;
    unsigned i;
    int error = clear_rings(c);
    for (i = 0; !error && i < sizeof(prefetch)/sizeof(prefetch[0]); ++i)
        error = wr(c, prefetch[i][0], prefetch[i][1]);
    if (!error) error = wr(c, 0xd420cu, 0xffffffffu);
    if (!error) error = wr(c, 0xd4280u, 0xffffffffu);
    if (!error) error = wr(c, 0xd42f0u, 0);
    if (error) return error;
    /* Mark before the FIRST address write, not after successful enable. */
    c->published = 1; c->quiescent = 0;
    error = ring(c, 0xd43f0u, base, MT_MCU_ENTRIES, 0);
    if (!error) error = ring(c, 0xd4400u, base + MT_MCU_ENTRIES*16u, MT_FWDL_ENTRIES, 0);
    if (!error) error = ring(c, 0xd4500u,
        base + (MT_MCU_ENTRIES+MT_FWDL_ENTRIES)*16u, MT_RX_ENTRIES, MT_RX_ENTRIES-1u);
    if (!error && c->runtime->data_prepared) error = ring(c, 0xd4520u,
        (uint32_t)c->runtime->data_descriptor_address,
        MT_DATA_RX_ENTRIES, MT_DATA_RX_ENTRIES-1u);
    if (!error && c->runtime->data_tx) error = ring(c,0xd4300u,
        (uint32_t)c->runtime->tx_descriptor_address,MT_DATA_TX_ENTRIES,0);
    /* Explicitly clear inherited 36-bit addressing and unwanted RX omission. */
    if (!error) error = rmw(c, DMA_CFG, 0x0c000035u, DMA_SETUP);
    if (!error) error = rmw(c, 0xd42b4u, 0, 0x10000000u);
    if (!error) error = rmw(c, 0xd4298u, 0, 0x0f00u);
    if (!error) error = rmw(c, 0xd429cu, 0, 0x7f00u);
    if (!error) error = rmw(c, 0x2120u, 0, 2);
    if (!error) error = rmw(c, DMA_CFG, 0, DMA_ENABLE);
    if (!error) error = mt_runtime_attach(c->runtime);
    return error;
}
int mt_chip_start(mt_chip *c, mt_mcu_runtime *runtime) {
    int error;
    if (!c || c->attempted || !runtime || !runtime->prepared || runtime->active ||
        runtime->used || runtime->mcu.fault || runtime->rx.fault ||
        (runtime->data_prepared && runtime->data_rx.fault) ||
        (runtime->data_tx && (!runtime->data_tx->prepared || runtime->data_tx->active ||
                             runtime->data_tx->used || runtime->data_tx->fault))) return MT_INVALID;
    runtime->sealed = 1; /* Configuration is immutable from the first HW access. */
    c->runtime = runtime; c->attempted = 1;
    c->published = 0; c->quiescent = 1; c->stop_error = 0;
    /* Same ownership handshake as the MT7925 PCI probe, then cold WF reset. */
    error = ownership(c, 1);
    if (!error) error = ownership(c, 0);
    if (!error) error = emi_protection(c);
    if (!error) error = reset_wfsys(c);
    if (!error) error = mask_interrupts(c);
    if (!error) error = disable_dma(c);
    if (!error) error = setup_dma(c);
    /* mt7925e_mcu_init performs a second ownership handoff AFTER DMA setup.
     * This is a device BAR register, not the OS-owned PCI config header. */
    if (!error) error = ownership(c, 1);
    if (!error) error = ownership(c, 0);
    if (!error) error = rmw(c, 0x10194u, 0, 0x100u);
    if (!error) error = equal(c, 0x10194u, 0x100u, 0x100u);
    c->start_error = error;
    return error;
}
int mt_chip_stop(mt_chip *c) {
    int error;
    if (!c || !c->runtime || !c->attempted) return MT_INVALID;
    mt_runtime_disarm(c->runtime);
    if (!c->published || c->quiescent) return MT_OK;
    error = ownership(c, 0);
    if (!error) error = mask_interrupts(c);
    if (!error) error = disable_dma(c);
    /* A stuck engine gets one WF subsystem reset, never a shared-bus reset. */
    if (error == MT_FW_TIMEOUT) {
        error = reset_wfsys(c);
        if (!error) error = ownership(c, 0);
        if (!error) error = mask_interrupts(c);
        if (!error) error = disable_dma(c);
    }
    if (!error) error = clear_rings(c);
    if (!error) error = equal(c, DMA_CFG, DMA_ENABLE | DMA_BUSY, 0);
    if (!error) c->quiescent = 1;
    c->stop_error = error;
    return error;
}
int mt_chip_boot_bundle(mt_chip *c, mt_mcu_runtime *runtime,
                       const uint8_t *data, uint32_t size, mt_chip_result *out) {
    mt_fw_preflight preflight;
    static const uint32_t registers[10] = {0xe0010u,0xe00f0u,0xd4208u,
        0xd43f8u,0xd43fcu,0xd4408u,0xd440cu,0xd4508u,0xd450cu,0x10194u};
    uint32_t ps, rs, i, value;
    int error;
    if (!c || c->attempted || !runtime || !out) return MT_INVALID;
    error = mt_fw_preflight_bundle(data, size, &preflight);
    if (error) return error;
    ps = (uint32_t)data[8] | (uint32_t)data[9]<<8 |
         (uint32_t)data[10]<<16 | (uint32_t)data[11]<<24;
    rs = size - 16 - ps;
    out->size = sizeof(*out); out->version = 2;
    out->firmware.patch_present = 0; out->firmware.regions_sent = 0;
    out->firmware.bytes_sent = 0; out->firmware.start_acked = 0;
    out->firmware.ready = 0; out->firmware.primary_error = 0;
    out->firmware.release_error = 0;
    error = mt_chip_start(c, runtime);
    if (!error) error = mt_fw_boot(data+16, ps, data+16+ps, rs,
                                  &runtime->firmware, &out->firmware);
    out->primary_error = error;
    out->stage = runtime->mcu.stage;
    out->last_command = runtime->mcu.last_command;
    out->last_state = runtime->mcu.last_state;
    out->snapshot_valid = 0;
    for (i = 0; i < 10; ++i) {
        out->snapshot[i] = 0xffffffffu;
        if (c->attempted && !rd(c, registers[i], &value)) {
            out->snapshot[i] = value;
            out->snapshot_valid |= 1u << i;
        }
    }
    out->stop_error = c->attempted ? mt_chip_stop(c) : MT_OK;
    out->published = c->published; out->quiescent = c->quiescent;
    return error ? error : out->stop_error;
}
