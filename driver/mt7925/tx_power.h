/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_TX_POWER_H
#define MT7925_TX_POWER_H
#include "firmware.h"
/* Pinned mt76 mt7925_txpwr: 417 signed rate entries, each with TWO BANDS.
 * Firmware targets, not measured RF power. No antenna-chain interpretation. */
#define MT_TXPOWER_RATE_COUNT 417u
#define MT_TXPOWER_TABLE_BYTES (MT_TXPOWER_RATE_COUNT*2u)
#define MT_TXPOWER_EVENT_BYTES (44u+12u+MT_TXPOWER_TABLE_BYTES+3u)
#define MT_TXPOWER_RAW_CAPACITY 896u
/* Response profile captured on the approved MT7925 firmware in r32.
 * EID 0x2a/category 5 also have MediaTek vendor-source definitions. Tag 5
 * and TLV length 841 are the observed firmware contract, not inferred from
 * the request's tag 7/category 2 or a generic TLV length convention. */
#define MT_TXPOWER_RESPONSE_EID 0x2au
#define MT_TXPOWER_RESPONSE_TAG 5u
#define MT_TXPOWER_RESPONSE_CATEGORY 5u
#define MT_TXPOWER_RESPONSE_TLV_BYTES 841u
enum mt_txpower_reason {
    MT_TXPOWER_NONE, MT_TXPOWER_WAITING, MT_TXPOWER_SIZE,
    MT_TXPOWER_PACKET_TYPE, MT_TXPOWER_DECLARED, MT_TXPOWER_SEQUENCE,
    MT_TXPOWER_UNSOLICITED, MT_TXPOWER_ACK, MT_TXPOWER_ACK_ERROR,
    MT_TXPOWER_TAG, MT_TXPOWER_TLV_LENGTH, MT_TXPOWER_CATEGORY,
    MT_TXPOWER_BAND, MT_TXPOWER_FORMAT, MT_TXPOWER_VALID,
    MT_TXPOWER_EVENT_ID
};
typedef struct mt_txpower_result {
    uint32_t attempted, valid;
    int32_t error;
    uint32_t sequence, reason, response_size;
    uint8_t raw[MT_TXPOWER_RAW_CAPACITY];
} mt_txpower_result;
/* Pure bounded parser. Success requires the complete observed fixed response.
 * An ACK alone and stale/unsolicited events never establish a power table. */
MT_API int mt_txpower_response(uint8_t sequence,const uint8_t *,uint32_t,
    uint32_t *reason);
#endif
