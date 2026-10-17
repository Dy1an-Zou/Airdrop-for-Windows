/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The host side of AWDL on Windows: a Wintun adapter that carries IPv6 packets
 * between the Windows network stack and the daemon. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define HOST_IF_NAME "awdl0"
/* Wintun never hands over a larger packet. */
#define HOST_IF_MAX_PACKET 0xFFFF

struct host_if;

/* Creates the adapter with `address` as its only link-local address. On
 * failure returns NULL with the reason in `error`. */
struct host_if *host_if_open(const uint8_t address[16], char *error, size_t error_len);
/* Removes the adapter again. */
void host_if_close(struct host_if *host);

/* Signaled when host_if_read may have a packet. It fires again only after a
 * read has returned 0, so read until then before waiting. */
void *host_if_read_event(const struct host_if *host);
unsigned long host_if_index(const struct host_if *host);

/* Copies the next packet the host sent into `packet`, which must hold
 * HOST_IF_MAX_PACKET bytes. Returns its length, or 0 when none is waiting. */
int host_if_read(struct host_if *host, uint8_t *packet);
/* Hands a packet to the host. Returns -1 when the adapter's queue is full. */
int host_if_write(struct host_if *host, const uint8_t *packet, int len);
