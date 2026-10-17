/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_CHANNEL_CONTROL_H
#define MT7925_CHANNEL_CONTROL_H
#include "mcu_runtime.h"
typedef struct mt_channel_control {
    mt_mcu_runtime *runtime;
    const mt_regdomain *domain;
    uint32_t bands, clc_config, environment;
    uint32_t prepared, attempted, configured, monitor;
    uint32_t primary, center, width;
    int32_t fault;
    uint8_t alpha2[2], reserved[2];
} mt_channel_control;
/* Firmware-decoded domain must stay immutable and alive with this object.
 * Serialized, nonpaged caller-owned objects, one control per runtime generation.
 * v3 radio link uses 2.4/5GHz numbers; it has no 6GHz band discriminator.
 * These APIs never implement raw TX or claim complete radio readiness.
 */
MT_API int mt_channel_prepare(mt_channel_control *,mt_mcu_runtime *,const mt_regdomain *,
    uint32_t bands,uint32_t clc_config,const uint8_t alpha2[2],uint32_t environment);
/* Fixed domain + RTS initialization, one attempt; requires MAC/power/data RX. */
MT_API int mt_channel_apply(mt_channel_control *);
/* Enable/configure sniffer and RX filter, or switch an enabled sniffer.
 * Width 20/40/80, explicit primary and center; all occupied spectrum is checked.
 * Restricted channels may be listened to; TX eligibility is checked separately.
 * Failure after command publication invalidates control until chip stop/reset.
 */
MT_API int mt_channel_monitor(mt_channel_control *,uint32_t primary,uint32_t center,uint32_t width);
MT_API int mt_channel_monitor_stop(mt_channel_control *);
/* Policy eligibility only: raw TX still requires its own initialization, SAR/
 * SKU power, queue and lifetime gates. DFS/no-IR/unknown restrictions deny. */
MT_API int mt_channel_permits_tx(const mt_channel_control *);
#endif
