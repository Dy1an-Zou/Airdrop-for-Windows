/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_if.h"

#include <winsock2.h>

#include <windows.h>

#include <ws2tcpip.h>

#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ADAPTER_NAME L"" HOST_IF_NAME
#define TUNNEL_TYPE L"winawdl"
#define RING_BYTES 0x400000
/* OWL's limit: leaves room for the AWDL and 802.11 headers in one frame. */
#define HOST_MTU 1450
#define LINK_LOCAL_PREFIX_BITS 64
#define STACK_WAIT_MS 3000
#define STACK_POLL_MS 50

/* Fixed, so Windows sees the same network on every run, not a new one. */
static const GUID adapter_guid = {
	0x2be941ea, 0xa750, 0x450a, {0xba, 0x41, 0xfd, 0xd8, 0xdf, 0x08, 0x70, 0xb6}};

/* The part of Wintun 0.14's wintun.h used here. The DLL is loaded at run time
 * so the daemon still starts, without a host interface, when it is absent. */
typedef struct _WINTUN_ADAPTER *WINTUN_ADAPTER_HANDLE;
typedef struct _TUN_SESSION *WINTUN_SESSION_HANDLE;

struct wintun {
	WINTUN_ADAPTER_HANDLE(WINAPI *CreateAdapter)(LPCWSTR name, LPCWSTR type, const GUID *guid);
	VOID(WINAPI *CloseAdapter)(WINTUN_ADAPTER_HANDLE adapter);
	VOID(WINAPI *GetAdapterLUID)(WINTUN_ADAPTER_HANDLE adapter, NET_LUID *luid);
	WINTUN_SESSION_HANDLE(WINAPI *StartSession)(WINTUN_ADAPTER_HANDLE adapter, DWORD capacity);
	VOID(WINAPI *EndSession)(WINTUN_SESSION_HANDLE session);
	HANDLE(WINAPI *GetReadWaitEvent)(WINTUN_SESSION_HANDLE session);
	BYTE *(WINAPI *ReceivePacket)(WINTUN_SESSION_HANDLE session, DWORD *size);
	VOID(WINAPI *ReleaseReceivePacket)(WINTUN_SESSION_HANDLE session, const BYTE *packet);
	BYTE *(WINAPI *AllocateSendPacket)(WINTUN_SESSION_HANDLE session, DWORD size);
	VOID(WINAPI *SendPacket)(WINTUN_SESSION_HANDLE session, const BYTE *packet);
};

struct host_if {
	struct wintun api;
	HMODULE dll;
	WINTUN_ADAPTER_HANDLE adapter;
	WINTUN_SESSION_HANDLE session;
	NET_LUID luid;
	IN6_ADDR address;
	HANDLE address_watch;
};

#define RESOLVE(name) ((api->name = (void *) GetProcAddress(dll, "Wintun" #name)) != NULL)

/* Only the executable's own directory is searched: the daemon runs elevated,
 * so a wintun.dll picked up from anywhere else would run elevated too. */
static HMODULE load_wintun(struct wintun *api) {
	HMODULE dll = LoadLibraryExW(L"wintun.dll", NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
	if (!dll)
		return NULL;
	if (RESOLVE(CreateAdapter) && RESOLVE(CloseAdapter) && RESOLVE(GetAdapterLUID) &&
	    RESOLVE(StartSession) && RESOLVE(EndSession) && RESOLVE(GetReadWaitEvent) &&
	    RESOLVE(ReceivePacket) && RESOLVE(ReleaseReceivePacket) &&
	    RESOLVE(AllocateSendPacket) && RESOLVE(SendPacket))
		return dll;
	FreeLibrary(dll);
	SetLastError(ERROR_PROC_NOT_FOUND);
	return NULL;
}

/* The IPv6 stack attaches to a new adapter a moment after it is created. */
static DWORD wait_for_ip6(const NET_LUID *luid, MIB_IPINTERFACE_ROW *row) {
	for (int waited = 0;; waited += STACK_POLL_MS) {
		InitializeIpInterfaceEntry(row);
		row->Family = AF_INET6;
		row->InterfaceLuid = *luid;
		DWORD error = GetIpInterfaceEntry(row);
		if (error != ERROR_NOT_FOUND || waited >= STACK_WAIT_MS)
			return error;
		Sleep(STACK_POLL_MS);
	}
}

static DWORD configure_ip6(const struct host_if *host) {
	MIB_IPINTERFACE_ROW row;
	DWORD error = wait_for_ip6(&host->luid, &row);
	if (error != NO_ERROR)
		return error;
	row.NlMtu = HOST_MTU;
	/* AWDL has no routers, and its addresses follow from MACs, so router
	 * discovery and duplicate address detection have nobody to ask. */
	row.RouterDiscoveryBehavior = RouterDiscoveryDisabled;
	row.DadTransmits = 0;
	row.ManagedAddressConfigurationSupported = FALSE;
	row.OtherStatefulConfigurationSupported = FALSE;
	error = SetIpInterfaceEntry(&row);
	if (error != NO_ERROR)
		return error;

	MIB_UNICASTIPADDRESS_ROW address;
	InitializeUnicastIpAddressEntry(&address);
	address.InterfaceLuid = host->luid;
	address.Address.Ipv6.sin6_family = AF_INET6;
	address.Address.Ipv6.sin6_addr = host->address;
	address.OnLinkPrefixLength = LINK_LOCAL_PREFIX_BITS;
	address.DadState = IpDadStatePreferred;
	error = CreateUnicastIpAddressEntry(&address);
	return error == ERROR_OBJECT_ALREADY_EXISTS ? NO_ERROR : error;
}

/* Windows also assigns a random link-local address. Peers can only answer
 * the one derived from the AWDL MAC, so the other must never be a source. */
static void remove_other_link_local(const struct host_if *host) {
	MIB_UNICASTIPADDRESS_TABLE *table;
	if (GetUnicastIpAddressTable(AF_INET6, &table) != NO_ERROR)
		return;
	for (ULONG i = 0; i < table->NumEntries; i++) {
		MIB_UNICASTIPADDRESS_ROW *row = &table->Table[i];
		const IN6_ADDR *address = &row->Address.Ipv6.sin6_addr;
		if (row->InterfaceLuid.Value == host->luid.Value &&
		    IN6_IS_ADDR_LINKLOCAL(address) && !IN6_ADDR_EQUAL(address, &host->address))
			DeleteUnicastIpAddressEntry(row);
	}
	FreeMibTable(table);
}

static VOID CALLBACK on_address_change(PVOID context, PMIB_UNICASTIPADDRESS_ROW row,
				       MIB_NOTIFICATION_TYPE type) {
	(void) row;
	if (type != MibDeleteInstance)
		remove_other_link_local(context);
}

static struct host_if *fail(struct host_if *host, char *error, size_t error_len, const char *what,
			    DWORD code) {
	snprintf(error, error_len, "%s (error %lu)", what, code);
	host_if_close(host);
	return NULL;
}

struct host_if *host_if_open(const uint8_t address[16], char *error, size_t error_len) {
	struct host_if *host = calloc(1, sizeof(*host));
	if (!host)
		return fail(host, error, error_len, "out of memory", ERROR_OUTOFMEMORY);
	memcpy(&host->address, address, sizeof(host->address));

	host->dll = load_wintun(&host->api);
	if (!host->dll)
		return fail(host, error, error_len, "no usable wintun.dll beside the executable",
			    GetLastError());
	host->adapter = host->api.CreateAdapter(ADAPTER_NAME, TUNNEL_TYPE, &adapter_guid);
	if (!host->adapter)
		return fail(host, error, error_len,
			    "cannot create the adapter; Administrator rights are needed",
			    GetLastError());
	host->api.GetAdapterLUID(host->adapter, &host->luid);
	host->session = host->api.StartSession(host->adapter, RING_BYTES);
	if (!host->session)
		return fail(host, error, error_len, "cannot start the adapter", GetLastError());

	DWORD code = configure_ip6(host);
	if (code == NO_ERROR)
		code = NotifyUnicastIpAddressChange(AF_INET6, on_address_change, host, TRUE,
						    &host->address_watch);
	if (code != NO_ERROR)
		return fail(host, error, error_len, "cannot configure IPv6 on the adapter", code);

	/* Wintun raises the event only after a read has found its queue empty. */
	SetEvent(host->api.GetReadWaitEvent(host->session));
	return host;
}

void host_if_close(struct host_if *host) {
	if (!host)
		return;
	if (host->address_watch)
		CancelMibChangeNotify2(host->address_watch);
	if (host->session)
		host->api.EndSession(host->session);
	if (host->adapter)
		host->api.CloseAdapter(host->adapter);
	if (host->dll)
		FreeLibrary(host->dll);
	free(host);
}

void *host_if_read_event(const struct host_if *host) {
	return host->api.GetReadWaitEvent(host->session);
}

unsigned long host_if_index(const struct host_if *host) {
	NET_IFINDEX index = 0;
	ConvertInterfaceLuidToIndex(&host->luid, &index);
	return index;
}

int host_if_read(struct host_if *host, uint8_t *packet) {
	DWORD size = 0;
	BYTE *received = host->api.ReceivePacket(host->session, &size);
	if (!received)
		return 0;
	memcpy(packet, received, size);
	host->api.ReleaseReceivePacket(host->session, received);
	return (int) size;
}

int host_if_write(struct host_if *host, const uint8_t *packet, int len) {
	BYTE *slot = host->api.AllocateSendPacket(host->session, (DWORD) len);
	if (!slot)
		return -1;
	memcpy(slot, packet, (size_t) len);
	host->api.SendPacket(host->session, slot);
	return 0;
}
