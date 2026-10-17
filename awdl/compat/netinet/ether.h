/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The parts of <netinet/ether.h> OWL uses; implemented in ether.c. */
#pragma once
#include <net/ethernet.h>

/* Returns a static buffer, like the libc original. */
char *ether_ntoa(const struct ether_addr *addr);
/* Returns a static buffer, or NULL when `text` is not six hex octets. */
struct ether_addr *ether_aton(const char *text);
