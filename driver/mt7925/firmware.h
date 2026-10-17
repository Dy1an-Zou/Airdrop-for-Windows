/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_FIRMWARE_H
#define MT7925_FIRMWARE_H
#include "core.h"
#define MT_FW_PATCH 1u
#define MT_FW_RAM 2u
#define MT_FW_MAX_SIZE (8u * 1024u * 1024u)
#define MT_FW_MAX_REGIONS 32u
#define MT_FW_CHUNK 4096u
#define MT_FW_PROTOCOL (-4)
#define MT_FW_TIMEOUT (-5)
typedef struct mt_fw_region {
    uint32_t offset, length, address, mode, skip, override_address;
} mt_fw_region;
typedef struct mt_fw_plan {
    uint32_t kind, count, entry;
    mt_fw_region regions[MT_FW_MAX_REGIONS];
} mt_fw_plan;
typedef struct mt_fw_result {
    uint32_t patch_present, regions_sent, bytes_sent, start_acked, ready;
    int32_t primary_error, release_error;
} mt_fw_result;
/* send must encode with mt_mcu_encode, route 0xee to FWDL, and match sequence
 * numbers for awaited replies. It returns transport status separately from
 * firmware status. drain waits for FWDL DMA completion. wait_state waits with
 * a deadline for ROM-download (0) or RAM-ready (1). No callback may fake success.
 * Images must be immutable for the call and selected from a trusted manifest. */
typedef struct mt_fw_transport {
    void *context;
    int (*send)(void *, uint32_t, const uint8_t *, uint32_t, int, uint32_t *);
    int (*drain)(void *);
    int (*wait_state)(void *, uint32_t);
} mt_fw_transport;
MT_API int mt_fw_validate(const uint8_t *, uint32_t, uint32_t, mt_fw_plan *);
/* Buffered IOCTL frame: LE32 magic 0x3546574d, version 1, patch size, RAM size,
 * then those exact two images. No extra trailing bytes or kernel pointers. */
typedef struct mt_fw_preflight {
    uint32_t size, version, patch_regions, ram_regions, download_regions;
    uint32_t download_bytes, entry, reserved;
} mt_fw_preflight;
MT_API int mt_fw_preflight_bundle(const uint8_t *, uint32_t, mt_fw_preflight *);
MT_API int mt_fw_boot(const uint8_t *patch, uint32_t patch_size,
                      const uint8_t *ram, uint32_t ram_size,
                      const mt_fw_transport *, mt_fw_result *);
/* Decode a response for one outstanding command. Out-of-sequence/event packets
 * return MT_PENDING without changing status. timeout/reset is the caller's job. */
MT_API int mt_mcu_response(uint32_t command, uint8_t sequence,
                           const uint8_t *packet, uint32_t size, uint32_t *status);
#endif
