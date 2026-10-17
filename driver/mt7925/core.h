/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_CORE_H
#define MT7925_CORE_H
#include <stdint.h>
#ifdef MT_TEST_EXPORT
#define MT_API __declspec(dllexport)
#else
#define MT_API
#endif

#define MT_DMA_DONE 0x80000000u
#define MT_DMA_LAST0 0x40000000u
#define MT_MCU_UNI (1u << 17)
#define MT_MCU_QUERY (1u << 16)
#define MT_MCU_CE (1u << 18)
#define MT_MCU_WA (1u << 19)
#define MT_FW_SCATTER 0xeeu
#define MT_DMA_MAX_LENGTH 0x3fffu

typedef struct mt_desc { uint32_t buf0, ctrl, buf1, info; } mt_desc;
typedef struct mt_ring {
    mt_desc *descriptors;
    uint32_t count, head, tail, queued;
} mt_ring;
enum mt_result { MT_OK = 0, MT_INVALID = -1, MT_FULL = -2, MT_PENDING = -3 };

/* All functions require caller serialization. Buffers must be disjoint.
 * Caller supplies DMA-coherent memory, barriers and MMIO doorbells. These
 * routines never map memory, start DMA, or access the card themselves. */
MT_API int mt_dma_span32(uint64_t address, uint32_t size);
MT_API int mt_ring_init(mt_ring *ring, mt_desc *descriptors, uint32_t count);
MT_API int mt_ring_submit(mt_ring *ring, uint64_t address, uint32_t size,
                          uint32_t *index);
/* dma_index is the next descriptor reported by the hardware completion index.
 * It must come from this ring, after the caller's DMA read barrier. */
MT_API int mt_ring_reclaim(mt_ring *ring, uint32_t dma_index, uint32_t *index);
/* Encodes the PCIe MCU packet (no USB header/padding). Result is byte count
 * or negative error. Sequence is advanced only on a successfully built packet.
 * Firmware scatter is headerless and must use the separate FWDL queue. */
MT_API int mt_mcu_encode(uint32_t command, uint8_t *sequence,
                        const uint8_t *payload, uint32_t length,
                        uint8_t *output, uint32_t capacity);
#endif
