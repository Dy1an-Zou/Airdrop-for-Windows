/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_RADIO_DEVICE_H
#define MT7925_RADIO_DEVICE_H
#include "radio_session.h"
#include "data_memory.h"
#define MT_RADIO_START 0x805u
#define MT_RADIO_STATUS 0x806u
#define MT_RADIO_CHANNEL 0x807u
#define MT_RADIO_TX 0x808u
#define MT_RADIO_POLL 0x809u
#define MT_RADIO_STOP 0x80au
#define MT_RADIO_ADDRESS 0x80bu
#define MT_RADIO_DIAGNOSTICS 0x80cu
#define MT_RADIO_DIAGNOSTIC_BYTES 2436u
/* Diagnostics v12 preserves prior field offsets (header size/version change).
 * At 724 and748, six LE32 words:
 * phase, valid_mask, first_error(signed), RFCR0, RFCR1, MDP_DCR0.
 * At772: attempted,valid,error,sequence,reason,response_size (six LE32),
 * then896 raw response bytes. Full pinned TXPOWER event is893 bytes.
 * Both are cached by the session; querying diagnostics performs no MMIO.
 * At1692: TXS requests,envelopes,records,matched,duplicates,unmatched,
 * malformed,unsupported (eight LE32). At1724: eight80-byte samples:
 * PID,FC,sequence,WCID,rate_table (LE32), RA6,zero2,matched(LE32),raw48.
 * Only a first format0/HOST PID/WCID/sequence match has matched1/raw48.
 * Unused samples and unmatched raw bytes are zero. TXS never releases DMA.
 * At2364/2368: first type0 envelope size/copied bytes (LE32), then64 raw
 * bytes at2372. Cached before validation, zero padded, never overwritten.
 */
#define MT_RADIO_REPLY_BYTES 96u
#define MT_RADIO_POLL_BYTES (MT_RADIO_REPLY_BYTES+MT_DATA_RX_CAPACITY+22u)
typedef struct mt_radio_device {
    mt_radio_session session;
    mt_platform platform;
    mt_mcu_runtime *runtime;
    mt_chip *chip;
    void *context;
    /* Kernel adapter only: acquire platform policy and optional coherent
     * buffers. Never accept user-supplied ACPI tables or DMA addresses. */
    int (*read_platform)(void *,mt_platform *);
    int (*prepare_data)(void *);
    uint8_t *scratch;
    uint32_t scratch_capacity, start_attempted;
    mt_data_allocation allocation;
} mt_radio_device;
/* Fixed LE wire protocol, no pointers/padding exposed. Input/output may alias;
 * all input is consumed before reply fields overwrite the buffered request.
 * Result is envelope validation; operation result lives at reply offset8.
 * START: version,bundle_bytes,28-byte config,reserved,complete firmware bundle.
 * CHANNEL: version,primary,center,width. TX: version,rate,no_ack,size,MPDU.
 * ADDRESS: version,MAC6,zero2. STATUS/POLL/STOP: empty.
 * POLL returns at most one full radio-link RX record.
 * Caller serializes requests, cleanup and PnP; STATUS requires no MMIO.
 */
MT_API int mt_radio_request(mt_radio_device *,uint32_t,const uint8_t *,uint32_t,
    uint8_t *,uint32_t,uint32_t *);
/* Last handle/D0Exit: stop while powered. Surprise removal: revoke without
 * touching MMIO. Both keep all allocations owned by the platform. */
MT_API int mt_radio_device_close(mt_radio_device *,uint32_t hardware_available);
#endif
