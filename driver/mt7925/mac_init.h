/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_MAC_INIT_H
#define MT7925_MAC_INIT_H
#include "mcu_runtime.h"
/* After firmware/capability/CLC preparation, before monitor/channel setup.
 * Uses on-chip efuse calibration (no writes to efuse), clears WTBL counters,
 * configures two hardware bands and twelve basic rates. Serialized caller,
 * passive level, one attempt per DMA generation. Failure poisons MCU traffic
 * until chip_stop and a fresh preparation. Does NOT set radio_ready or free DMA.
 * mac_step: 1 efuse, 2 MDP, 3 WTBL, 4 band, 5 rates, 6 complete. */
MT_API int mt_mac_initialize(mt_mcu_runtime *);
#endif
