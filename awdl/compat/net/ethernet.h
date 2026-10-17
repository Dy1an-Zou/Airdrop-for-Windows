/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The parts of <net/ethernet.h> OWL uses. */
#pragma once
#include <stdint.h>

#define ETHER_ADDR_LEN 6
#define ETHER_MAX_LEN 1518

struct ether_addr {
	uint8_t ether_addr_octet[ETHER_ADDR_LEN];
} __attribute__((__packed__));
