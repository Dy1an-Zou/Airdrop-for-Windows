/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_RAW_TX_H
#define MT7925_RAW_TX_H
#include "firmware.h"
#define MT_TXWI_BYTES 64u
#define MT_TX_PAYLOAD_MAX 4095u
#define MT_TX_SLOT_BYTES 4160u
#define MT_TX_TOKEN_LIMIT 8192u
#define MT_TXS_LIMIT 8u
#define MT_TXS_BYTES 48u
typedef struct mt_tx_status_sample {
    uint32_t pid, fc, sequence, wcid, rate_table;
    uint8_t receiver[6], reserved[2];
    uint32_t matched;
    uint8_t raw[MT_TXS_BYTES];
} mt_tx_status_sample;
typedef struct mt_tx_status_diag {
    uint32_t requests, envelopes, records, matched;
    uint32_t duplicates, unmatched, malformed, unsupported;
    mt_tx_status_sample samples[MT_TXS_LIMIT];
    uint32_t first_size, first_bytes;
    uint8_t first_raw[64];
} mt_tx_status_diag;
typedef struct mt_tx_io {
    void *context;
    void (*barrier)(void *);
    int (*publish)(void *,uint32_t);
} mt_tx_io;
typedef struct mt_raw_tx {
    mt_ring ring;
    mt_tx_io io;
    uint8_t *pool;
    uint32_t *states, *slots;
    uint64_t pool_address;
    uint32_t tokens, inflight, cursor, prepared, active, used;
    int32_t fault;
    uint32_t status_count[4], status_attempts;
    mt_tx_status_diag txs;
} mt_raw_tx;
/* Caller-serialized, stable nonpaged objects and disjoint coherent allocations.
 * states has tokens entries; slots has count entries. Software preparation only.
 * Platform validates these allocations against ALL other live DMA pools.
 * Activation requires platform-confirmed queue, MAC, power, WCID and channel
 * initialization. These routines alone do not establish radio readiness. */
MT_API int mt_raw_tx_prepare(mt_raw_tx *,mt_desc *,uint64_t,uint32_t count,
    uint8_t *,uint64_t,uint32_t pool_bytes,uint32_t *states,uint32_t tokens,
    uint32_t *slots,const mt_tx_io *);
MT_API int mt_raw_tx_activate(mt_raw_tx *);
MT_API void mt_raw_tx_disarm(mt_raw_tx *);
/* Raw unencrypted action/data MPDU, no radiotap or FCS. Rate is one of the
 * initialized legacy OFDM tables18..25; WCID must be initialized by platform.
 * Output is the exact PCIe TXD+HW TXP, not the USB packet format. */
MT_API int mt_tx_encode(const uint8_t *,uint32_t,uint64_t payload_address,
    uint32_t token,uint32_t wcid,uint32_t rate_table,uint32_t no_ack,
    uint8_t *,uint32_t capacity);
MT_API int mt_raw_tx_submit(mt_raw_tx *,const uint8_t *,uint32_t,
    uint32_t wcid,uint32_t rate_table,uint32_t no_ack,uint32_t *token);
/* Completion index must be read from this generation's data TX queue. The
 * platform read barrier precedes that read; this function also fences memory.
 * A token/payload is reusable only after BOTH DMA and TX-free notification.
 * Firmware release is not an over-air ACK/success indication. */
MT_API int mt_raw_tx_dma(mt_raw_tx *,uint32_t dma_index);
MT_API int mt_raw_tx_free(mt_raw_tx *,const uint8_t *,uint32_t);
/* PCIe packet-type0: 16-byte envelope and complete 48-byte TXS records.
 * Diagnostic only: never changes token/DMA ownership, readiness or faults.
 * PID3..10 are assigned once to the first eight ACK-requesting unicasts.
 * A matched format0/HOST record is retained verbatim; power stays raw8.
 * Late already-received records may be inspected after disarm without I/O. */
MT_API void mt_raw_tx_txs(mt_raw_tx *,const uint8_t *,uint32_t);
/* On any protocol/publication fault, allocations remain owned until platform
 * chip-stop or OS removal proves DMA quiescent. No timeout-based reclamation.
 * Firmware must emit each token release once: reused IDs have no wire epoch.
 */
#endif
