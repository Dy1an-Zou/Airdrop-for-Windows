/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The libpcap surface OWL's core.c uses, served by the radio daemon link in
 * win/io_win.c instead of a capture device. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

#define DLT_IEEE802_11_RADIO 127

struct pcap_pkthdr {
	struct timeval ts;
	uint32_t caplen;
	uint32_t len;
};

typedef struct pcap pcap_t;
typedef struct pcap_dumper pcap_dumper_t;
typedef void (*pcap_handler)(unsigned char *user, const struct pcap_pkthdr *hdr,
			     const unsigned char *bytes);

/* Delivers at most one received frame; returns how many were delivered. */
int pcap_dispatch(pcap_t *handle, int count, pcap_handler callback, unsigned char *user);

/* Frame dumps to a capture file are not implemented. core.c only reaches
 * these when a dump file is configured, which the Windows daemon never does. */
static inline pcap_t *pcap_open_dead(int linktype, int snaplen) {
	(void) linktype;
	(void) snaplen;
	return NULL;
}
static inline pcap_dumper_t *pcap_dump_open_append(pcap_t *handle, const char *path) {
	(void) handle;
	(void) path;
	return NULL;
}
static inline void pcap_dump(unsigned char *user, const struct pcap_pkthdr *hdr,
			     const unsigned char *bytes) {
	(void) user;
	(void) hdr;
	(void) bytes;
}
static inline void pcap_close(pcap_t *handle) {
	(void) handle;
}
static inline void pcap_dump_close(pcap_dumper_t *dumper) {
	(void) dumper;
}
