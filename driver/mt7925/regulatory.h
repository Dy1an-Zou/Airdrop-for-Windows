/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_REGULATORY_H
#define MT7925_REGULATORY_H
#include "firmware.h"
#define MT_REGD_MAX_RULES 128u
#define MT_REGD_REQUEST_HEADER 88u
typedef struct mt_reg_rule {
    uint32_t start_mhz, end_mhz, bandwidth_mhz, eirp, flags;
} mt_reg_rule;
typedef struct mt_regdomain {
    uint32_t count, dfs_region;
    mt_reg_rule rules[MT_REGD_MAX_RULES];
} mt_regdomain;
/* Build SET_POWER_LIMIT/tag6 from a trusted immutable RAM image. The signed
 * country rules are opaque and must be decoded/validated by firmware, never
 * used as host-side frequencies. No hardware callbacks. Returns bytes or error.
 * Buffers must be disjoint; invalid input leaves output unchanged. */
MT_API int mt_regd_request(const uint8_t *, uint32_t, uint32_t hw_encap,
    const uint8_t alpha2[2], uint8_t *, uint32_t);
/* Parses decoded rules from one solicited MCU response; ACK-only waits. Exact
 * tag/length/count validation; output unchanged on failure or unrelated event.
 * This is firmware information, not a channel-selection or TX permission API. */
MT_API int mt_regd_response(uint8_t, const uint8_t *, uint32_t, mt_regdomain *);
#endif
