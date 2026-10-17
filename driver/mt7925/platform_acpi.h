/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_PLATFORM_ACPI_H
#define MT7925_PLATFORM_ACPI_H
#include "tx_init.h"
#define MT_ACPI_TABLE_BYTES 104u
#define MT_ACPI_WIRE_BYTES 1024u
#define MT_ACPI_OUTPUT_SIGNATURE 0x426f6541u
#define MT_ACPI_NAME_NOT_FOUND 0xc0000034u
typedef struct mt_acpi_table {
    uint32_t state, ntstatus, size;
    uint8_t data[MT_ACPI_TABLE_BYTES];
} mt_acpi_table;
typedef struct mt_platform {
    uint32_t size, version;
    int32_t error;
    uint32_t reserved;
    mt_acpi_table dynamic, geographic;
} mt_platform;
typedef struct mt_acpi_io {
    void *context;
    /* Actual NTSTATUS, not a Boolean. Exact output byte count, never capacity.
     * Synchronous callback consumes fixed four-byte names, no caller AML path.
     * Windows adapter sets a timeout; returned storage stays valid for call. */
    int32_t (*evaluate)(void *,const uint8_t name[4],uint8_t *,uint32_t,uint32_t *);
} mt_acpi_io;
/* Decode the V1 output argument array: MTDS/MTGS are packages of byte-valued
 * integers, flattened by ACPI.sys into Count ULONG arguments. No nested package,
 * buffer/string coercion, truncation of integers, stale output, or partial data.
 * Output cleared even on failure. Only OBJECT_NAME_NOT_FOUND means ABSENT.
 * Header/stride constants have WDK C_ASSERT checks in the Windows adapter. */
MT_API int mt_acpi_decode(uint32_t ntstatus,const uint8_t *,uint32_t,mt_acpi_table *);
/* Always collect both fixed methods for useful diagnostics. Successful transport
 * parsing is separate from SAR table semantics, checked by mt_platform_policy.
 * No MAC/DMA operations, no ACPI writes, no mutable global or dynamic allocation. */
MT_API int mt_platform_read(const mt_acpi_io *,mt_platform *);
MT_API int mt_platform_policy(const mt_platform *,uint32_t dfs_region,
    int32_t cap_half_dbm,mt_tx_policy *);
#endif
