/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_CHIP_CONTROL_H
#define MT7925_CHIP_CONTROL_H
#include "mcu_runtime.h"

/* Caller serializes start, MCU traffic and stop at passive level. MMIO must
 * remain mapped/powered for these calls. This does not free any allocation.
 * quiescent is meaningful only for this generation's published buffers; an
 * unsuccessful stop requires retaining them until the OS powers down/removes
 * the device. Merely disarming the software transport is not a DMA fence. */
typedef struct mt_chip {
    mt_mcu_runtime *runtime;
    uint32_t attempted, published, quiescent;
    int32_t start_error, stop_error;
} mt_chip;
MT_API int mt_chip_start(mt_chip *, mt_mcu_runtime *);
MT_API int mt_chip_stop(mt_chip *);
/* One-shot firmware diagnostic. Validates the complete immutable bundle before
 * ownership/reset. Always stops after a start attempt, including boot failure.
 * firmware.ready is historical (boot observed), not a running radio flag. */
typedef struct mt_chip_result {
    uint32_t size, version;
    mt_fw_result firmware;
    int32_t primary_error, stop_error;
    uint32_t published, quiescent;
    /* ABI v2: snapshot BEFORE stop, best-effort read validity is explicit.
     * regs: LPCTL, FW state, DMA cfg, MCU CPU/DMA, FWDL CPU/DMA,
     * RX CPU/DMA, PCIe MAC PM. Snapshot errors never replace boot/stop errors. */
    uint32_t stage, last_command, last_state, snapshot_valid;
    uint32_t snapshot[10];
} mt_chip_result;
MT_API int mt_chip_boot_bundle(mt_chip *, mt_mcu_runtime *,
    const uint8_t *, uint32_t, mt_chip_result *);
#endif
