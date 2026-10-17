/* SPDX-License-Identifier: GPL-3.0-or-later */
/* <endian.h> for OWL on Windows, where every supported host is little-endian. */
#pragma once
#include <stdint.h>

#define htole16(x) ((uint16_t) (x))
#define htole32(x) ((uint32_t) (x))
#define htole64(x) ((uint64_t) (x))
#define le16toh(x) ((uint16_t) (x))
#define le32toh(x) ((uint32_t) (x))
#define le64toh(x) ((uint64_t) (x))

#define htobe16(x) __builtin_bswap16(x)
#define htobe32(x) __builtin_bswap32(x)
#define htobe64(x) __builtin_bswap64(x)
#define be16toh(x) __builtin_bswap16(x)
#define be32toh(x) __builtin_bswap32(x)
#define be64toh(x) __builtin_bswap64(x)
