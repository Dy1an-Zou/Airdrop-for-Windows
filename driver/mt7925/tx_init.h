/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_TX_INIT_H
#define MT7925_TX_INIT_H
#include "mcu_runtime.h"

#define MT_ACPI_ABSENT 1u
#define MT_ACPI_DATA 2u
/* First monitor VIF: MT792x_WTBL_SIZE (20) - 1 - BSS index (0). */
#define MT_MONITOR_WCID 19u
typedef struct mt_tx_policy {
    uint32_t prepared, dfs_region;
    int32_t half_dbm[5];
} mt_tx_policy;
typedef struct mt_tx_init_result {
    uint32_t full_power_sent, device_enabled, bss_enabled, sku_batches, thermal_sent;
    int32_t error;
} mt_tx_init_result;

/* Software only. Platform must actually evaluate MTDS/MTGS before reporting
 * ABSENT; unknown/query failures are not absence. DATA is the flattened ACPI
 * integer package, including its four-byte name. Names/row IDs are opaque in
 * upstream; layout/count, every power byte and every row are validated here.
 * The caller supplies an explicit finite cap in half-dBm [-127,126]; zero is
 * 0 dBm, never "unlimited". Firmware CLC/EFUSE still impose device/regulatory
 * ceilings; the decoded regdomain.eirp field has no assumed host unit.
 * Only 2.4/5GHz is supported; v2's 6GHz entries are validated but not emitted.
 * A malformed present table denies initialization instead of silently falling
 * back. MTGS may be absent; disabled/absent MTDS ignores valid MTGS like mt76.
 * Output is invalidated first on every call. Serialized trusted kernel caller.
 */
MT_API int mt_tx_policy_prepare(mt_tx_policy *, uint32_t dyn_state,
    const uint8_t *dyn, uint32_t dyn_size, uint32_t geo_state,
    const uint8_t *geo, uint32_t geo_size, uint32_t dfs_region, int32_t cap_half_dbm);
/* One attempt per runtime generation. KeepFullPwr and both thermal-protection
 * command publications (no firmware ACK exists), monitor DEV/BSS context 0
 * with reserved WCID19, then all SKU batches via the real MCU transport.
 * Raw frames use this same monitor VIF context, as in mt792x_tx. MAC init
 * clears its WTBL counters; monitor setup does not invent a STA_REC command.
 * Does not activate the TX queue or claim RF readiness. Chip stop/reset is the
 * cleanup boundary after either success or partial failure. Call before monitor
 * enable/raw TX; channel policy and full radio-session gates remain required.
 */
MT_API int mt_tx_initialize(mt_mcu_runtime *, const mt_tx_policy *, uint32_t bands,
    const uint8_t alpha2[2], const uint8_t mac[6], mt_tx_init_result *);
#endif
