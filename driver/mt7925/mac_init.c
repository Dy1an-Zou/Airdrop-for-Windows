/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2023 MediaTek Inc.
 * mt7925/init.c, mt792x_mac.c, mt7925/mac.c and mcu.c sequences.
 * BAR offsets below are translated with pinned mt7925/pci.c's MT7925 map,
 * not the MT7928 map or arbitrary caller-selected register addresses.
 * Error propagation, readback and lifecycle gates are Windows port additions.
 */
#include "mac_init.h"

static int read_reg(mt_mcu_runtime *r, uint32_t reg, uint32_t *out) {
    int error;
    if (!r->active) return MT_INVALID;
    r->mac_reg=reg;
    error=r->hardware.read(r->hardware.context,reg,out);
    r->mac_io_error=error;
    if (error) return error;
    r->mac_observed=*out;
    return *out==0xffffffffu ? MT_FW_PROTOCOL : MT_OK;
}
static int write_reg(mt_mcu_runtime *r, uint32_t reg, uint32_t value) {
    int error;
    if (!r->active) return MT_INVALID;
    r->mac_reg=reg;r->mac_expected=value;
    r->hardware.barrier(r->hardware.context);
    error=r->hardware.write(r->hardware.context,reg,value);
    r->mac_io_error=error;return error;
}
static int rmw(mt_mcu_runtime *r, uint32_t reg, uint32_t clear, uint32_t set) {
    uint32_t value, expected;
    r->mac_mask=clear|set;
    int error=read_reg(r,reg,&value);
    if (error) return error;
    expected=(value&~clear)|set;
    error=write_reg(r,reg,expected);
    if (!error) error=read_reg(r,reg,&value);
    if (!error && (value&(clear|set))!=(expected&(clear|set))) error=MT_FW_PROTOCOL;
    return error;
}
static int wtbl_idle(mt_mcu_runtime *r) {
    uint64_t start=r->hardware.milliseconds(r->hardware.context);
    uint32_t value;
    for (;;) {
        int error=read_reg(r,0x34380u,&value);
        if (error) return error;
        if (!(value&0x80000000u)) return MT_OK;
        /* Upstream mt76_poll(...,5000) is 5000 microseconds, not seconds. */
        if (r->hardware.milliseconds(r->hardware.context)-start>=5) return MT_FW_TIMEOUT;
        r->hardware.pause(r->hardware.context);
    }
}
static int enable_rx_time(mt_mcu_runtime *r, uint32_t reg) {
    uint32_t value;
    int error;
    /* Pinned mt792x_mac_init_band uses mt76_set for these MIB counters.
     * r6 observed a successful bit30 write to 0x217c4 read back as zero.
     * Do not impose stable-register equality on a counter's enable bit;
     * retain the upstream write, posted-write flush and I/O/all-ones checks.
     * This does not establish that time reporting is enabled on hardware.
     */
    r->mac_mask=0x40000000u;
    error=read_reg(r,reg,&value);
    if (!error) error=write_reg(r,reg,value|0x40000000u);
    if (!error) error=read_reg(r,reg,&value);
    return error;
}
static int init_band(mt_mcu_runtime *r, uint32_t band) {
    uint32_t offset=band ? 0x80000u : 0;
    int error=rmw(r,0x210f4u+offset,0x3fu,0x6003fu);
    if (!error) error=enable_rx_time(r,0x217c4u+offset);
    if (!error) error=enable_rx_time(r,0x21780u+offset);
    if (!error) error=rmw(r,0x24804u+offset,0,0x300u);
    if (!error) error=rmw(r,0x21e00u+offset,0x80fff8u,0x3000u);
    if (!error) error=rmw(r,0x23408u+offset,0xc3000000u,0x03000000u);
    return error;
}
int mt_mac_initialize(mt_mcu_runtime *r) {
    /* UNI EFUSE_CTRL tag BUFFER_MODE=2, mode EFUSE=0, WHOLE=1, len=0. */
    static const uint8_t efuse[12]={0,0,0,0,2,0,8,0,0,1,0,0};
    static const uint16_t rates[12]={0,1,2,3,75,79,74,78,73,77,72,76};
    uint32_t status=0, value, i;
    int error;
    if (!r || !r->active || r->mcu.last_state!=3 || r->mac_attempted ||
        r->mcu.fault || r->rx.fault || (r->data_prepared && r->data_rx.fault)) return MT_INVALID;
    r->mac_attempted=1; r->mac_initialized=0; r->mac_step=1; r->mac_index=0;
    error=r->firmware.send(r->firmware.context,MT_MCU_UNI|0x2du,efuse,12,1,&status);
    if (!error && status) error=MT_FW_PROTOCOL;
    if (error) goto done;
    r->mac_step=2;
    error=rmw(r,0xe804u,0xfff8u,0x3000u);
    /* Raw 802.11 input is needed by AWDL; explicitly leave header translation off. */
    if (!error) error=rmw(r,0xe800u,0x80000u,0x8000u);
    if (error) goto done;
    r->mac_step=3;
    error=wtbl_idle(r);
    for (i=0;!error && i<20;++i) {
        r->mac_index=i;
        error=read_reg(r,0x34380u,&value);
        if (!error) error=write_reg(r,0x34380u,(value&~0xfffu)|i|0x4000u);
        if (!error) error=wtbl_idle(r);
    }
    if (error) goto done;
    r->mac_step=4;
    for (i=0;!error && i<2;++i) {r->mac_index=i;error=init_band(r,i);}
    if (error) goto done;
    r->mac_step=5;
    for (i=0;!error && i<12;++i) {
        r->mac_index=i;
        error=write_reg(r,0x343b8u,rates[i]);
        if (!error) error=write_reg(r,0x343bcu,0x40u);
        if (!error) error=write_reg(r,0x343b0u,0x80010000u|(14u+i));
        /* Flush posted writes; EXEC is a hardware command, not a stable field. */
        if (!error) error=read_reg(r,0x343b0u,&value);
    }
    if (!error) {r->mac_initialized=1;r->mac_step=6;}
done:
    if (error) r->mcu.fault=error;
    return error;
}
