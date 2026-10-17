/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_EFUSE_TYPE_H
#define MT7925_EFUSE_TYPE_H
#include "firmware.h"
typedef struct mt_hw_type {
    uint32_t encapsulation, version, valid, data_size, magic, type, event_id;
} mt_hw_type;
/* Fixed MT7925 EFUSE_CTRL/tag2 response to tag1 read, block 0xa70, byte 0xa71.
 * Copies diagnostics only after bounds, tag, address and valid checks. Does not
 * interpret undocumented version/size/magic/type fields as hardware capability.
 * Pinned mt7925_mcu_parse_response has no EFUSE-specific event ID: correlate the
 * serialized sequence, solicited bit, exact TLV and address instead of guessing
 * an event number. Successful ACK alone never provides a hardware type.
 */
MT_API int mt_efuse_type_response(uint8_t, const uint8_t *, uint32_t, mt_hw_type *);
#endif
