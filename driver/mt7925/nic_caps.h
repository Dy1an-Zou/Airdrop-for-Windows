/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_NIC_CAPS_H
#define MT7925_NIC_CAPS_H
#include "firmware.h"
/* present bits: MAC, PHY, 6GHz, chip capability, EML. bands bits: 2.4/5/6GHz.
 * PHY and a valid unicast MAC are mandatory. Other absent TLVs remain absent,
 * not evidence that firmware explicitly denied a capability. */
typedef struct mt_nic_caps {
    uint32_t present, bands, nss, max_bandwidth;
    uint32_t chip_low, chip_high, eml;
    uint8_t mac[6], reserved[2];
} mt_nic_caps;
/* A complete NIC-capability payload after the 44-byte MCU RX header.
 * Unknown TLVs are skipped; malformed lengths/counts and duplicate known TLVs
 * fail atomically. Output is written only after the whole payload is valid. */
MT_API int mt_nic_parse(const uint8_t *, uint32_t, mt_nic_caps *);
/* Match UNI NIC_CAPAB event 0x43 and command sequence. Unsolicited, stale and
 * unrelated events do not mutate output. A command-result ACK is not capability
 * data: successful ACKs keep waiting, negative CHIP_CONFIG ACKs fail. */
MT_API int mt_nic_response(uint8_t, const uint8_t *, uint32_t, mt_nic_caps *);
#endif
