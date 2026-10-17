/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Client end of the radio daemon's pipe (radio/radio_link.h). A reader thread
 * queues received frames; everything else is called from the event loop. */
#pragma once
#include <stdint.h>

/* AWDL frames stay under 2.4 KiB; larger ones are not ours and are dropped. */
#define RADIO_CLIENT_MAX_FRAME 4096

struct radio_client;

/* NULL when the radio daemon is not running or speaks another version.
 * `on_disconnect` runs on the reader thread when the daemon goes away. */
struct radio_client *radio_client_open(void (*on_disconnect)(void));
void radio_client_close(struct radio_client *client);

/* Signalled whenever a frame is queued. A HANDLE, typed void* to keep
 * windows.h out of this header. */
void *radio_client_rx_event(const struct radio_client *client);
uint8_t radio_client_channel(const struct radio_client *client);
/* How wide a channel the radio receives on, in MHz. */
uint8_t radio_client_width_mhz(const struct radio_client *client);

/* Copies the oldest queued frame into `frame` (RADIO_CLIENT_MAX_FRAME bytes)
 * and returns its length, or 0 when the queue is empty. */
int radio_client_pop(struct radio_client *client, uint8_t *frame);
int radio_client_has_frame(struct radio_client *client);

/* `type` is a radio_link_type. Returns 0 on success. */
int radio_client_send(struct radio_client *client, uint16_t type, const uint8_t *payload,
		      uint32_t length);
