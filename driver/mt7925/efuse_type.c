/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2023 MediaTek Inc.
 * Layout from pinned mt7925/mcu.c mt7925_mcu_read_eeprom. Valid-field gate
 * follows shared UNI EFUSE access convention in mt7996_mcu_get_eeprom.
 * Strict lengths, atomic output and ACK/data separation are port additions.
 */
#include "efuse_type.h"
static uint32_t le16(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8;}
static uint32_t le32(const uint8_t *p) {return le16(p)|le16(p+2)<<16;}
int mt_efuse_type_response(uint8_t sequence, const uint8_t *p, uint32_t size, mt_hw_type *out) {
    const uint8_t *data;
    uint32_t declared;
    if (!p || !out || !sequence || sequence>15 || size<40) return MT_INVALID;
    declared=le16(p+32);
    if (declared<8 || declared>size-32) return MT_INVALID;
    if (p[37]!=sequence || (p[38]&4u)) return MT_PENDING;
    if (p[36]==1) {
        if (declared<20) return MT_INVALID;
        if (p[44]!=0x2d) return MT_PENDING;
        return le32(p+48)?MT_FW_PROTOCOL:MT_PENDING;
    }
    if (declared<20) return MT_INVALID;
    data=p+44;
    /* MT7925 pinned firmware replies with tag2 to the tag1 read request.
     * Confirmed by the complete solicited 124-byte reply in radio trial r2.
     * Request tags are not response tags; keep all length/address/valid gates. */
    if (le16(data+4)!=2) return MT_PENDING;
    if (declared!=92 || le16(data+6)!=76) return MT_INVALID;
    if (le32(data+12)!=0xa70u) return MT_PENDING;
    if (!le32(data+16)) return MT_FW_PROTOCOL;
    out->encapsulation=data[49]&3u;
    out->version=le32(data+8);out->valid=le32(data+16);
    out->data_size=le32(data+20);out->magic=le32(data+24);out->type=le32(data+28);
    out->event_id=p[36];
    return MT_OK;
}
