/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Layout: pinned mt76 467ae2e mt7925/mcu.h and mt7925/mt7925.h.
 * Strict response validation is a port addition; no firmware setters here. */
#include "tx_power.h"
static uint32_t le16(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8;}
static uint32_t le32(const uint8_t *p) {return le16(p)|le16(p+2)<<16;}
int mt_txpower_response(uint8_t sequence,const uint8_t *p,uint32_t size,
    uint32_t *reason) {
    uint32_t declared;
    const uint8_t *data;
    if (!reason) return MT_INVALID;
    *reason=MT_TXPOWER_SIZE;
    if (!p || !sequence || sequence>15 || size<40) return MT_INVALID;
    *reason=MT_TXPOWER_SEQUENCE;
    if (p[37]!=sequence) return MT_PENDING;
    *reason=MT_TXPOWER_UNSOLICITED;
    if (p[38]&4u) return MT_PENDING;
    *reason=MT_TXPOWER_PACKET_TYPE;
    if (le16(p+34)!=0x3800u) return MT_INVALID;
    declared=le16(p+32);*reason=MT_TXPOWER_DECLARED;
    if (declared<12 || declared!=size-32) return MT_INVALID;
    data=p+44;
    if (p[36]==1) {
        *reason=MT_TXPOWER_SIZE;
        if (size!=52) return MT_INVALID;
        *reason=MT_TXPOWER_ACK;
        if (data[0]!=0x2b) return MT_PENDING;
        if (!le32(data+4)) return MT_PENDING;
        *reason=MT_TXPOWER_ACK_ERROR;return MT_FW_PROTOCOL;
    }
    *reason=MT_TXPOWER_SIZE;
    if (size!=MT_TXPOWER_EVENT_BYTES) return MT_INVALID;
    *reason=MT_TXPOWER_EVENT_ID;
    if (p[36]!=MT_TXPOWER_RESPONSE_EID) return MT_INVALID;
    *reason=MT_TXPOWER_TAG;
    if (le16(data+4)!=MT_TXPOWER_RESPONSE_TAG) return MT_INVALID;
    *reason=MT_TXPOWER_TLV_LENGTH;
    if (le16(data+6)!=MT_TXPOWER_RESPONSE_TLV_BYTES) return MT_INVALID;
    *reason=MT_TXPOWER_CATEGORY;
    if (data[8]!=MT_TXPOWER_RESPONSE_CATEGORY) return MT_INVALID;
    *reason=MT_TXPOWER_BAND;
    if (data[9]!=0) return MT_INVALID;
    *reason=MT_TXPOWER_FORMAT;
    if (data[11]>1) return MT_INVALID;
    *reason=MT_TXPOWER_VALID;
    return MT_OK;
}
