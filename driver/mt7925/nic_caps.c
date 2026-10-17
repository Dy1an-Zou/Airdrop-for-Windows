/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2023 MediaTek Inc.
 * Wire definitions from pinned mt7925/mcu.c, mcu.h, mt76_connac_mcu.h.
 * Bounded atomic parsing and event filtering are Windows port additions.
 */
#include "nic_caps.h"
static uint32_t le16(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8;
}
static uint32_t le32(const uint8_t *p) {
    return le16(p) | le16(p+2)<<16;
}
int mt_nic_parse(const uint8_t *p, uint32_t size, mt_nic_caps *out) {
    mt_nic_caps parsed;
    uint32_t count, offset=4, i, j, tag, length, bit, any;
    if (!p || !out || size<4) return MT_INVALID;
    count=le16(p);
    if (count>(size-4)/4) return MT_INVALID;
    parsed.present=0; parsed.bands=0; parsed.nss=0; parsed.max_bandwidth=0;
    parsed.chip_low=0; parsed.chip_high=0; parsed.eml=0;
    for (j=0;j<6;++j) parsed.mac[j]=0;
    parsed.reserved[0]=0; parsed.reserved[1]=0;
    for (i=0;i<count;++i) {
        if (size-offset<4) return MT_INVALID;
        tag=le16(p+offset); length=le16(p+offset+2);
        if (length<4 || length>size-offset) return MT_INVALID;
        bit=tag==7 ? 1u : tag==8 ? 2u : tag==0x18 ? 4u :
            tag==0x20 ? 8u : tag==0x22 ? 16u : 0u;
        if (parsed.present&bit) return MT_INVALID;
        if (tag==7) {
            if (length<10 || (p[offset+4]&1)) return MT_INVALID;
            any=0;
            for (j=0;j<6;++j) {parsed.mac[j]=p[offset+4+j];any|=parsed.mac[j];}
            if (!any) return MT_INVALID;
        } else if (tag==8) {
            if (length<17) return MT_INVALID;
            parsed.nss=p[offset+8]; parsed.max_bandwidth=p[offset+7];
            if (!parsed.nss || parsed.nss>2) return MT_INVALID; /* MT7925 2x2 */
            parsed.bands|=p[offset+14]&3u;
        } else if (tag==0x18) {
            if (length<5) return MT_INVALID;
            if (p[offset+4]) parsed.bands|=4u;
        } else if (tag==0x20) {
            if (length<12) return MT_INVALID;
            parsed.chip_low=le32(p+offset+4); parsed.chip_high=le32(p+offset+8);
        } else if (tag==0x22) {
            if (length<16) return MT_INVALID;
            parsed.eml=le16(p+offset+8);
        }
        parsed.present|=bit;
        offset+=length;
    }
    if (offset!=size || (parsed.present&3u)!=3u) return MT_INVALID;
    /* No C runtime dependency in the freestanding transport test/driver. */
    for (j=0;j<sizeof(parsed);++j) ((uint8_t *)out)[j]=((const uint8_t *)&parsed)[j];
    return MT_OK;
}
int mt_nic_response(uint8_t sequence, const uint8_t *p, uint32_t size, mt_nic_caps *out) {
    uint32_t declared;
    if (!p || !out || !sequence || sequence>15 || size<40) return MT_INVALID;
    declared=le16(p+32);
    if (declared<8 || declared>size-32) return MT_INVALID;
    if (p[37]!=sequence || (p[38]&4u)) return MT_PENDING;
    if (p[36]==1) {
        if (declared<20) return MT_INVALID;
        if (p[44]!=0x0e) return MT_PENDING;
        return le32(p+48) ? MT_FW_PROTOCOL : MT_PENDING;
    }
    if (p[36]!=0x43) return MT_PENDING;
    if (declared<16) return MT_INVALID;
    return mt_nic_parse(p+44,declared-12,out);
}
