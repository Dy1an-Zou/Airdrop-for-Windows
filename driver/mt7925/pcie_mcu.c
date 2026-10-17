/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * PCIe MCU queue selection from mt7925/pci_mcu.c, MediaTek 2023.
 * Bounded polling and error propagation are Windows port additions.
 */
#include "pcie_mcu.h"
#define COMMAND_RING 0xd43f0u
#define DOWNLOAD_RING 0xd4400u
#define FIRMWARE_STATE 0xe00f0u
#define TIMEOUT_MS 3000u

static int overlap(uint64_t a, uint32_t an, uint64_t b, uint32_t bn) {
    return a <= b ? b-a < an : a-b < bn;
}
int mt_pcie_validate_banks(const mt_rx_bank *banks, uint32_t count, uint32_t required) {
    uint32_t i, j;
    uint64_t table = (uintptr_t)banks;
    if (!banks || !count || count > 4096 || !required ||
        table > UINTPTR_MAX - (uint64_t)count*sizeof(*banks)) return MT_INVALID;
    for (i=0; i<count; ++i) {
        uint64_t va = (uintptr_t)banks[i].virtual_address;
        uint32_t bytes = banks[i].bytes;
        if (!va || bytes < required || va > UINTPTR_MAX-bytes ||
            !mt_dma_span32(banks[i].address,bytes) ||
            overlap(va,bytes,table,count*(uint32_t)sizeof(*banks))) return MT_INVALID;
        for (j=0; j<i; ++j)
            if (overlap(va,bytes,(uintptr_t)banks[j].virtual_address,banks[j].bytes) ||
                overlap(banks[i].address,bytes,banks[j].address,banks[j].bytes)) return MT_INVALID;
    }
    return MT_OK;
}
static uint8_t *slot_buffer(const mt_pcie_queue *q, uint32_t slot, uint64_t *address) {
    if (q->banks) {
        const mt_rx_bank *bank = &q->banks[slot/q->slots_per_bank];
        uint32_t offset = (slot%q->slots_per_bank)*q->stride;
        *address = bank->address+offset;
        return bank->virtual_address+offset;
    }
    *address = q->address+slot*q->stride;
    return q->packets+slot*q->stride;
}
static int fail(mt_pcie_mcu *m, int error) {
    if (error) m->fault = error;
    return error;
}
static int reclaim(mt_pcie_mcu *m, mt_pcie_queue *q) {
    uint32_t index, slot;
    int error = m->io.read(m->io.context, q->registers+12, &index);
    if (error) return error;
    m->io.barrier(m->io.context);
    do { error = mt_ring_reclaim(q->ring, index, &slot); } while (!error);
    return error == MT_PENDING ? MT_OK : error;
}
static int expired(mt_pcie_mcu *m, uint64_t start, uint32_t limit) {
    return m->io.milliseconds(m->io.context) - start >= limit;
}
static int exchange(void *context, uint32_t cmd, const uint8_t *data, uint32_t size,
                int wait, uint32_t *reply, mt_nic_caps *caps, mt_hw_type *hw,
                mt_regdomain *regd, mt_txpower_result *power) {
    mt_pcie_mcu *m = context;
    mt_pcie_queue *q;
    uint64_t start, address;
    uint32_t slot, length, i;
    uint8_t sequence;
    uint8_t *packet;
    int error, encoded;
    if (m->fault) return m->fault;
    if (!reply || (wait && cmd == MT_FW_SCATTER)) return MT_INVALID;
    m->stage = 1; m->last_command = cmd;
    m->response_count=0; m->response_size=0; m->response_result=MT_PENDING;
    m->first_size=0;m->first_result=MT_PENDING;
    for (i=0;i<sizeof(m->response_prefix);++i) {m->response_prefix[i]=0;m->first_prefix[i]=0;}
    q = cmd == MT_FW_SCATTER ? &m->download : &m->command;
    start = m->io.milliseconds(m->io.context);
    for (;;) {
        error = reclaim(m,q);
        if (error) return fail(m,error);
        if (q->ring->queued < q->ring->count-2) break;
        if (expired(m,start,TIMEOUT_MS)) return fail(m,MT_FW_TIMEOUT);
        m->io.pause(m->io.context);
    }
    slot = q->ring->head; sequence = m->sequence;
    packet = slot_buffer(q,slot,&address);
    encoded = mt_mcu_encode(cmd, &sequence, data, size, packet, q->stride);
    if (encoded < 0) return encoded;
    error = mt_ring_submit(q->ring, address, (uint32_t)encoded, &slot);
    if (error) return fail(m,error);
    m->sequence = sequence;
    if (power) power->sequence=sequence;
    m->stage = 2;
    m->io.barrier(m->io.context); /* Packet and descriptor before doorbell. */
    error = m->io.write(m->io.context, q->registers+8, q->ring->head);
    if (error) return fail(m,error);
    if (!wait) return MT_OK;
    m->stage = 3;
    start = m->io.milliseconds(m->io.context);
    for (;;) {
        if (expired(m,start,TIMEOUT_MS)) return fail(m,MT_FW_TIMEOUT);
        length = 0;
        error = m->io.receive(m->io.context,m->response,m->response_capacity,&length);
        if (!error) {
            if (power) {
                power->response_size=length;
                for (i=0;i<MT_TXPOWER_RAW_CAPACITY;++i)
                    power->raw[i]=i<length && i<m->response_capacity ? m->response[i] : 0;
                if (length>m->response_capacity) power->reason=MT_TXPOWER_SIZE;
            }
            if (length > m->response_capacity) return fail(m,MT_INVALID);
            ++m->response_count; m->response_size=length;
            for (i=0;i<sizeof(m->response_prefix);++i)
                m->response_prefix[i]=i<length ? m->response[i] : 0;
            error = power ? mt_txpower_response(sequence,m->response,length,&power->reason) :
                    regd ? mt_regd_response(sequence,m->response,length,regd) :
                    hw ? mt_efuse_type_response(sequence,m->response,length,hw) :
                    caps ? mt_nic_response(sequence,m->response,length,caps) :
                           mt_mcu_response(cmd,sequence,m->response,length,reply);
            m->response_result=error;
            if (m->response_count==1) {
                m->first_size=length;m->first_result=error;
                for (i=0;i<sizeof(m->first_prefix);++i) m->first_prefix[i]=m->response_prefix[i];
            }
            if (!error) return MT_OK;
        }
        if (error != MT_PENDING) return fail(m,error);
        m->io.pause(m->io.context);
    }
}
static int send(void *context, uint32_t cmd, const uint8_t *data, uint32_t size,
                int wait, uint32_t *reply) {
    return exchange(context,cmd,data,size,wait,reply,0,0,0,0);
}
int mt_pcie_query_nic(mt_pcie_mcu *m, mt_nic_caps *caps) {
    static const uint8_t request[8]={0,0,0,0,3,0,4,0};
    uint32_t reply=0;
    if (!m || !caps || m->last_state!=3) return MT_INVALID;
    return exchange(m,MT_MCU_UNI|0x0eu,request,8,1,&reply,caps,0,0,0);
}
int mt_pcie_query_txpower(mt_pcie_mcu *m,mt_txpower_result *out) {
    /* Same request and UNI command option as mt7925_get_txpwr_info.
     * In particular this is NOT an arbitrary MT_MCU_QUERY command. */
    static const uint8_t request[12]={0,0,0,0,7,0,8,0,0,2,0,0};
    uint32_t reply=0;
    int error;
    if (!m || !out || out->attempted || m->last_state!=3) return MT_INVALID;
    out->attempted=1;out->valid=0;out->reason=MT_TXPOWER_WAITING;
    error=exchange(m,MT_MCU_UNI|0x2bu,request,12,1,&reply,0,0,0,out);
    out->error=error;out->valid=!error;
    return error;
}
int mt_pcie_query_hw_type(mt_pcie_mcu *m, mt_hw_type *hw) {
    static const uint8_t request[32]={0,0,0,0,1,0,28,0,0x70,0x0a,0,0};
    uint32_t reply=0;
    if (!m || !hw || m->last_state!=3) return MT_INVALID;
    return exchange(m,MT_MCU_UNI|MT_MCU_QUERY|0x2du,request,32,1,&reply,0,hw,0,0);
}
int mt_pcie_query_regdomain(mt_pcie_mcu *m,const uint8_t *ram,uint32_t size,
    uint32_t hw,const uint8_t alpha[2],uint8_t *scratch,uint32_t capacity,mt_regdomain *out) {
    uint32_t reply=0;
    int length;
    if (!m || !out || m->last_state!=3) return MT_INVALID;
    if (m->fault) return m->fault;
    length=mt_regd_request(ram,size,hw,alpha,scratch,capacity);
    if (length<0) return length;
    return exchange(m,MT_MCU_UNI|0x2cu,scratch,(uint32_t)length,1,&reply,0,0,out,0);
}
static int drain(void *context) {
    mt_pcie_mcu *m = context;
    uint64_t start = m->io.milliseconds(m->io.context);
    int error;
    if (m->fault) return m->fault;
    m->stage = 4;
    while (m->download.ring->queued) {
        error = reclaim(m,&m->download);
        if (error) return fail(m,error);
        if (!m->download.ring->queued) break;
        if (expired(m,start,TIMEOUT_MS)) return fail(m,MT_FW_TIMEOUT);
        m->io.pause(m->io.context);
    }
    return MT_OK;
}
static int wait_state(void *context, uint32_t ready) {
    mt_pcie_mcu *m = context;
    uint64_t start = m->io.milliseconds(m->io.context);
    uint32_t state, expected = ready ? 3u : 1u, mask = ready ? 3u : 7u;
    int error;
    if (m->fault) return m->fault;
    m->stage = ready ? 6u : 5u;
    for (;;) {
        error = m->io.read(m->io.context,FIRMWARE_STATE,&state);
        if (error) return fail(m,error);
        m->last_state = state;
        if (state == 0xffffffffu) return fail(m,MT_FW_PROTOCOL);
        if ((state & mask) == expected) return MT_OK;
        if (expired(m,start,ready ? 1500u : 1000u)) return fail(m,MT_FW_TIMEOUT);
        m->io.pause(m->io.context);
    }
}
static int valid_queue(const mt_pcie_queue *q, uint32_t registers) {
    uint32_t i, bytes, descriptor_bytes;
    uint64_t descriptors;
    if (!q->ring || !q->ring->descriptors || q->ring->count < 4 || q->ring->count > 4096 ||
        q->stride < MT_FW_CHUNK+64 || q->stride > 16384 || q->registers != registers ||
        q->ring->queued || q->ring->head != q->ring->tail || q->ring->head >= q->ring->count)
        return 0;
    bytes = q->ring->count*q->stride;
    descriptor_bytes = q->ring->count*(uint32_t)sizeof(mt_desc);
    descriptors = (uintptr_t)q->ring->descriptors;
    if (descriptors > UINTPTR_MAX-descriptor_bytes) return 0;
    if (!q->banks) {
        uint64_t va = (uintptr_t)q->packets;
        return !q->bank_count && !q->slots_per_bank && va && va <= UINTPTR_MAX-bytes &&
            mt_dma_span32(q->address,bytes) && !overlap(va,bytes,descriptors,descriptor_bytes);
    }
    if (q->packets || q->address || !q->slots_per_bank || q->slots_per_bank > q->ring->count ||
        q->ring->count%q->slots_per_bank || q->bank_count != q->ring->count/q->slots_per_bank ||
        mt_pcie_validate_banks(q->banks,q->bank_count,q->slots_per_bank*q->stride) != MT_OK ||
        overlap((uintptr_t)q->banks,q->bank_count*(uint32_t)sizeof(*q->banks),descriptors,descriptor_bytes))
        return 0;
    for (i=0; i<q->bank_count; ++i)
        if (overlap((uintptr_t)q->banks[i].virtual_address,q->banks[i].bytes,descriptors,descriptor_bytes))
            return 0;
    return 1;
}
static void queue_span(const mt_pcie_queue *q, uint32_t i, uint64_t *va, uint64_t *pa, uint32_t *bytes) {
    *va = (uintptr_t)(q->banks ? q->banks[i].virtual_address : q->packets);
    *pa = q->banks ? q->banks[i].address : q->address;
    *bytes = q->banks ? q->banks[i].bytes : q->ring->count*q->stride;
}
static int overlaps_metadata(const mt_pcie_queue *q, uint64_t va, uint32_t bytes) {
    return overlap(va,bytes,(uintptr_t)q->ring->descriptors,q->ring->count*(uint32_t)sizeof(mt_desc)) ||
        (q->banks && overlap(va,bytes,(uintptr_t)q->banks,q->bank_count*(uint32_t)sizeof(*q->banks)));
}
static int disjoint_queues(const mt_pcie_mcu *m) {
    const mt_pcie_queue *a=&m->command, *b=&m->download;
    uint32_t i,j,an,bn,ac=a->banks?a->bank_count:1,bc=b->banks?b->bank_count:1;
    uint64_t av,ap,bv,bp;
    if (overlaps_metadata(b,(uintptr_t)a->ring->descriptors,a->ring->count*(uint32_t)sizeof(mt_desc)) ||
        overlaps_metadata(a,(uintptr_t)b->ring->descriptors,b->ring->count*(uint32_t)sizeof(mt_desc)) ||
        overlaps_metadata(a,(uintptr_t)m->response,m->response_capacity) ||
        overlaps_metadata(b,(uintptr_t)m->response,m->response_capacity)) return 0;
    for (i=0; i<ac; ++i) {
        queue_span(a,i,&av,&ap,&an);
        if (overlaps_metadata(b,av,an) ||
            overlap(av,an,(uintptr_t)m->response,m->response_capacity)) return 0;
        for (j=0; j<bc; ++j) {
            queue_span(b,j,&bv,&bp,&bn);
            if (overlap(av,an,bv,bn) || overlap(ap,an,bp,bn)) return 0;
        }
    }
    for (j=0; j<bc; ++j) {
        queue_span(b,j,&bv,&bp,&bn);
        if (overlaps_metadata(a,bv,bn) ||
            overlap(bv,bn,(uintptr_t)m->response,m->response_capacity)) return 0;
    }
    return 1;
}
int mt_pcie_bind(mt_pcie_mcu *m, mt_fw_transport *t) {
    if (!m || !t || !m->io.read || !m->io.write || !m->io.barrier ||
        !m->io.milliseconds || !m->io.pause || !m->io.receive ||
        !m->response || m->response_capacity < 52 ||
        (uintptr_t)m->response > UINTPTR_MAX-m->response_capacity ||
        !valid_queue(&m->command,COMMAND_RING) || !valid_queue(&m->download,DOWNLOAD_RING) ||
        m->command.ring == m->download.ring || m->fault || !disjoint_queues(m)) return MT_INVALID;
    m->sequence = 0;
    m->stage = 0; m->last_command = 0; m->last_state = 0xffffffffu;
    t->context = m; t->send = send; t->drain = drain; t->wait_state = wait_state;
    return MT_OK;
}
