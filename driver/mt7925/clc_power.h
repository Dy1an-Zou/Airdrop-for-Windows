/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_CLC_POWER_H
#define MT7925_CLC_POWER_H
#include "mcu_runtime.h"
#define MT_CLC_HEADER 84u
#define MT_CLC_SCRATCH (MT_DMA_MAX_LENGTH - 48u)
typedef struct mt_clc_result {
    uint32_t selected_mask, fallback_mask, requests_sent, channel_config;
    int32_t primary_error;
} mt_clc_result;
/* Serialized power-table preparation for the pinned MT7925 firmware family.
 * RAM must be immutable/trusted and disjoint from scratch/result/runtime.
 * hw_encap comes from the device EFUSE query, never a guessed default.
 * alpha2 is "00" or uppercase ISO country, environment ANY=0/INDOOR=1/OUTDOOR=2.
 * Validates BOTH selected power blocks before sending any command. One attempt
 * per runtime activation; partial firmware application requires a fresh reset.
 * Does not implement CLC regulatory/BE rules, channel-domain or RF activation;
 * power_configured alone MUST NOT authorize transmission or radio_ready.
 */
MT_API int mt_clc_power_apply(mt_mcu_runtime *, const uint8_t *, uint32_t,
    uint32_t hw_encap, const uint8_t alpha2[2], uint32_t environment,
    uint8_t *scratch, uint32_t capacity, mt_clc_result *);
#endif
