/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "radio_client.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "radio_link.h"

#define SLOT_COUNT 2048
#define CONNECT_TIMEOUT_MS 15000
#define CONNECT_RETRY_MS 100
#define HELLO_TIMEOUT_MS 10000

struct slot {
	int length;
	uint8_t frame[RADIO_CLIENT_MAX_FRAME];
};

struct radio_client {
	HANDLE pipe;
	HANDLE reader;
	HANDLE rx_event;    /* auto-reset: a frame was queued */
	HANDLE hello_event; /* manual-reset: the hello arrived */
	HANDLE read_done;
	HANDLE write_done;
	CRITICAL_SECTION queue_lock;
	CRITICAL_SECTION write_lock;
	struct slot *slots;
	int head, count;
	uint64_t dropped;
	uint32_t version;
	uint8_t channel;
	uint8_t width_mhz;
	volatile LONG closing;
	void (*on_disconnect)(void);
};

/* The pipe is overlapped so the reader thread never blocks a send. */
static BOOL finish(struct radio_client *client, OVERLAPPED *io, BOOL started, DWORD *transferred) {
	if (!started && GetLastError() != ERROR_IO_PENDING)
		return FALSE;
	return GetOverlappedResult(client->pipe, io, transferred, TRUE);
}

static BOOL read_exact(struct radio_client *client, void *data, DWORD length) {
	uint8_t *bytes = data;
	while (length > 0) {
		OVERLAPPED io = {.hEvent = client->read_done};
		DWORD got = 0;
		BOOL started = ReadFile(client->pipe, bytes, length, &got, &io);
		if (!finish(client, &io, started, &got) || got == 0)
			return FALSE;
		bytes += got;
		length -= got;
	}
	return TRUE;
}

static BOOL write_all(struct radio_client *client, const void *data, DWORD length) {
	const uint8_t *bytes = data;
	while (length > 0) {
		OVERLAPPED io = {.hEvent = client->write_done};
		DWORD put = 0;
		BOOL started = WriteFile(client->pipe, bytes, length, &put, &io);
		if (!finish(client, &io, started, &put))
			return FALSE;
		bytes += put;
		length -= put;
	}
	return TRUE;
}

static void queue_frame(struct radio_client *client, const uint8_t *frame, uint32_t length) {
	if (length > RADIO_CLIENT_MAX_FRAME)
		return;
	EnterCriticalSection(&client->queue_lock);
	if (client->count == SLOT_COUNT) {
		/* The loop fell behind: the oldest frame is the least useful one. */
		client->head = (client->head + 1) % SLOT_COUNT;
		client->count--;
		client->dropped++;
	}
	struct slot *slot = &client->slots[(client->head + client->count) % SLOT_COUNT];
	slot->length = (int) length;
	memcpy(slot->frame, frame, length);
	client->count++;
	LeaveCriticalSection(&client->queue_lock);
	SetEvent(client->rx_event);
}

static DWORD WINAPI reader_main(void *arg) {
	struct radio_client *client = arg;
	uint8_t *payload = malloc(RADIO_LINK_MAX_PAYLOAD);
	struct radio_link_header header;

	while (payload && read_exact(client, &header, sizeof(header))) {
		if (header.length > RADIO_LINK_MAX_PAYLOAD ||
		    (header.length && !read_exact(client, payload, header.length)))
			break;
		if (header.type == RADIO_LINK_RX) {
			queue_frame(client, payload, header.length);
		} else if (header.type == RADIO_LINK_HELLO &&
			   header.length == sizeof(struct radio_link_hello)) {
			const struct radio_link_hello *hello = (const void *) payload;
			client->version = hello->version;
			client->channel = hello->channel;
			client->width_mhz = hello->width_mhz;
			SetEvent(client->hello_event);
		}
	}
	free(payload);
	if (!client->closing) {
		log_error("radio daemon disconnected");
		if (client->on_disconnect)
			client->on_disconnect();
	}
	return 0;
}

/* The daemon may still be starting, or its single pipe instance may still be
 * tied to a client that just left, so keep trying for a while. */
static HANDLE connect_pipe(void) {
	ULONGLONG deadline = GetTickCount64() + CONNECT_TIMEOUT_MS;
	for (;;) {
		HANDLE pipe = CreateFileA(RADIO_LINK_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0,
					  NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
		if (pipe != INVALID_HANDLE_VALUE)
			return pipe;
		DWORD error = GetLastError();
		BOOL may_appear = error == ERROR_FILE_NOT_FOUND || error == ERROR_PIPE_BUSY;
		if (!may_appear || GetTickCount64() >= deadline) {
			SetLastError(error);
			return INVALID_HANDLE_VALUE;
		}
		Sleep(CONNECT_RETRY_MS);
	}
}

struct radio_client *radio_client_open(void (*on_disconnect)(void)) {
	HANDLE pipe = connect_pipe();
	if (pipe == INVALID_HANDLE_VALUE) {
		log_error("cannot reach the radio daemon at %s (error %lu)", RADIO_LINK_PIPE_NAME,
			  GetLastError());
		return NULL;
	}

	struct radio_client *client = calloc(1, sizeof(*client));
	client->pipe = pipe;
	client->on_disconnect = on_disconnect;
	client->slots = calloc(SLOT_COUNT, sizeof(*client->slots));
	client->rx_event = CreateEventA(NULL, FALSE, FALSE, NULL);
	client->hello_event = CreateEventA(NULL, TRUE, FALSE, NULL);
	client->read_done = CreateEventA(NULL, TRUE, FALSE, NULL);
	client->write_done = CreateEventA(NULL, TRUE, FALSE, NULL);
	InitializeCriticalSection(&client->queue_lock);
	InitializeCriticalSection(&client->write_lock);
	client->reader = CreateThread(NULL, 0, reader_main, client, 0, NULL);

	if (WaitForSingleObject(client->hello_event, HELLO_TIMEOUT_MS) != WAIT_OBJECT_0 ||
	    client->version != RADIO_LINK_VERSION) {
		log_error("radio daemon did not greet with protocol version %u",
			  RADIO_LINK_VERSION);
		radio_client_close(client);
		return NULL;
	}
	return client;
}

void radio_client_close(struct radio_client *client) {
	if (!client)
		return;
	InterlockedExchange(&client->closing, 1);
	CancelIoEx(client->pipe, NULL);
	WaitForSingleObject(client->reader, INFINITE);
	CloseHandle(client->reader);
	CloseHandle(client->pipe);
	CloseHandle(client->rx_event);
	CloseHandle(client->hello_event);
	CloseHandle(client->read_done);
	CloseHandle(client->write_done);
	DeleteCriticalSection(&client->queue_lock);
	DeleteCriticalSection(&client->write_lock);
	if (client->dropped)
		log_warn("%llu received frames were dropped unread",
			 (unsigned long long) client->dropped);
	free(client->slots);
	free(client);
}

void *radio_client_rx_event(const struct radio_client *client) {
	return client->rx_event;
}

uint8_t radio_client_channel(const struct radio_client *client) {
	return client->channel;
}

uint8_t radio_client_width_mhz(const struct radio_client *client) {
	return client->width_mhz;
}

int radio_client_pop(struct radio_client *client, uint8_t *frame) {
	int length = 0;
	EnterCriticalSection(&client->queue_lock);
	if (client->count > 0) {
		const struct slot *slot = &client->slots[client->head];
		length = slot->length;
		memcpy(frame, slot->frame, (size_t) length);
		client->head = (client->head + 1) % SLOT_COUNT;
		client->count--;
	}
	LeaveCriticalSection(&client->queue_lock);
	return length;
}

int radio_client_has_frame(struct radio_client *client) {
	EnterCriticalSection(&client->queue_lock);
	int waiting = client->count > 0;
	LeaveCriticalSection(&client->queue_lock);
	return waiting;
}

int radio_client_send(struct radio_client *client, uint16_t type, const uint8_t *payload,
		      uint32_t length) {
	struct radio_link_header header = {.type = type, .length = length};
	/* Header and frame in one write: one system call a frame, not two. */
	static uint8_t message[sizeof(struct radio_link_header) + RADIO_CLIENT_MAX_FRAME];
	BOOL ok;
	EnterCriticalSection(&client->write_lock);
	if (length <= RADIO_CLIENT_MAX_FRAME) {
		memcpy(message, &header, sizeof(header));
		if (length)
			memcpy(message + sizeof(header), payload, length);
		ok = write_all(client, message, (DWORD) sizeof(header) + length);
	} else {
		ok = write_all(client, &header, sizeof(header)) &&
		     write_all(client, payload, length);
	}
	LeaveCriticalSection(&client->write_lock);
	return ok ? 0 : -1;
}
