/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2023 MediaTek Inc.
 * Wire layout from mt7925/pci_mac.c, mac.c and mt76_connac_mac.c.
 * Fixed pools, atomic notification validation and two-fence lifetime are port
 * additions. See SOURCES.md for the pinned upstream revision.
 */
#include "raw_tx.h"
static uint32_t get16(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8;}
static uint32_t get32(const uint8_t *p) {return get16(p)|get16(p+2)<<16;}
static void put16(uint8_t *p,uint32_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v) {put16(p,v);put16(p+2,v>>16);}
static int fail(mt_raw_tx *q,int error) {q->fault=error;q->active=0;return error;}
static int live(const mt_raw_tx *q) {return q && q->prepared && q->active && !q->fault;}
int mt_tx_encode(const uint8_t *frame,uint32_t size,uint64_t address,
    uint32_t token,uint32_t wcid,uint32_t rate,uint32_t no_ack,uint8_t *out,uint32_t cap) {
    uint32_t fc,type,sub,header=24,tid=0,multicast,i,seq;
    if (!frame || !out || size<24 || size>MT_TX_PAYLOAD_MAX || cap<MT_TXWI_BYTES ||
        !mt_dma_span32(address,size) || token>=MT_TX_TOKEN_LIMIT || wcid>=4096 ||
        rate<18 || rate>25 || no_ack>1) return MT_INVALID;
    fc=get16(frame);type=(fc>>2)&3u;sub=(fc>>4)&15u;
    /* AWDL sends vendor action or plain/QoS data. Reject encrypted, fragmented,
     * null/control frames and unsupported header forms rather than guessing. */
    if ((fc&0x4403u) || (get16(frame+22)&15u) ||
        !((type==0 && sub==13) || (type==2 && (sub==0 || sub==8)))) return MT_INVALID;
    if (type==0 && ((fc&0x8300u) || size<25)) return MT_INVALID;
    if (type==2) {
        if ((fc&0x300u)==0x300u) header+=6;
        if (sub==8) {
            if (size<header+2) return MT_INVALID;
            tid=frame[header]&15u;
            if (frame[header]&0x80u) return MT_INVALID; /* no injected A-MSDU */
            header+=2;if (fc&0x8000u) header+=4;
        } else if (fc&0x8000u) return MT_INVALID;
    }
    if (size<header) return MT_INVALID;
    seq=get16(frame+22)>>4;multicast=frame[4]&1u;
    for (i=0;i<MT_TXWI_BYTES;++i) out[i]=0;
    /* CT format0: ordinary DATA uses WMM0 AC_BE (AC01); others retain ALTX0. */
    put32(out,((type==2u && sub==0u)?0x02000000u:0x20000000u)|(size+32u));
    put32(out+4,0x80008000u|(header/2u)<<16|tid<<21|wcid);
    put32(out+8,type<<4|sub);
    put32(out+12,0x90007800u|seq<<16|multicast<<4|(no_ack||multicast?1u:0u));
    put32(out+20,1); /* submit may request bounded TXS; TX-free owns lifetime */
    /* Match mt7925_mac_write_txwi for monitor/non-MLO BSS:
     * DIS_MAT preserves the injected AWDL addresses instead of enabling
     * hardware MAC address translation. DAS + one MSDU remain unchanged. */
    put32(out+24,rate<<16|0x1cu);
    put16(out+32,token|0x8000u);
    put32(out+40,(uint32_t)address);put16(out+44,size|0x8000u);
    return MT_TXWI_BYTES;
}
int mt_raw_tx_prepare(mt_raw_tx *q,mt_desc *desc,uint64_t da,uint32_t count,
    uint8_t *pool,uint64_t pa,uint32_t bytes,uint32_t *states,uint32_t tokens,
    uint32_t *slots,const mt_tx_io *io) {
    uint32_t i,needed;
    int error;
    if (!q || q->prepared || !desc || !pool || !states || !slots || !io ||
        !io->barrier || !io->publish || count<4 || count>2048 || !tokens ||
        tokens>MT_TX_TOKEN_LIMIT || (da&15u) || (pa&3u)) return MT_INVALID;
    needed=tokens*MT_TX_SLOT_BYTES;
    if (bytes<needed || !mt_dma_span32(da,count*16u) || !mt_dma_span32(pa,needed) ||
        (da<pa+needed && pa<da+count*16u)) return MT_INVALID;
    error=mt_ring_init(&q->ring,desc,count);if (error) return error;
    q->io.context=io->context;q->io.barrier=io->barrier;q->io.publish=io->publish;
    q->pool=pool;q->pool_address=pa;q->states=states;q->slots=slots;q->tokens=tokens;
    for (i=0;i<tokens;++i) states[i]=0;
    for (i=0;i<count;++i) slots[i]=0xffffffffu;
    q->inflight=0;q->cursor=0;q->active=0;q->used=0;q->fault=0;q->prepared=1;
    for (i=0;i<4;++i) q->status_count[i]=0;
    q->status_attempts=0;
    for (i=0;i<sizeof(q->txs);++i) ((uint8_t *)&q->txs)[i]=0;
    return MT_OK;
}
int mt_raw_tx_activate(mt_raw_tx *q) {
    if (!q || !q->prepared || q->used || q->fault) return MT_INVALID;
    q->used=1;q->active=1;return MT_OK;
}
void mt_raw_tx_disarm(mt_raw_tx *q) {if (q) {q->active=0;q->used=1;}}
int mt_raw_tx_submit(mt_raw_tx *q,const uint8_t *frame,uint32_t size,
    uint32_t wcid,uint32_t rate,uint32_t no_ack,uint32_t *token) {
    uint8_t header[MT_TXWI_BYTES],*buffer;
    uint32_t id,i,index=0,pid=0,nonzero=0;
    uint64_t address;
    int error;
    if (!live(q) || !token) return MT_INVALID;
    if (q->ring.queued>=q->ring.count-2u || q->inflight>=q->tokens) return MT_FULL;
    for (i=0;i<q->tokens;++i) if (!q->states[(q->cursor+i)%q->tokens]) break;
    if (i==q->tokens) return fail(q,MT_FW_PROTOCOL);
    id=(q->cursor+i)%q->tokens;address=q->pool_address+(uint64_t)id*MT_TX_SLOT_BYTES;
    error=mt_tx_encode(frame,size,address+MT_TXWI_BYTES,id,wcid,rate,no_ack,header,sizeof(header));
    if (error<0) return error;
    for (i=0;i<6;++i) nonzero|=frame[4+i];
    if (!no_ack && !(frame[4]&1u) && nonzero && q->txs.requests<MT_TXS_LIMIT) {
        pid=3u+q->txs.requests;
        put32(header+20,0x400u|pid); /* HOST, format0; MCU status remains clear */
    }
    buffer=q->pool+id*MT_TX_SLOT_BYTES;
    for (i=0;i<MT_TXWI_BYTES;++i) buffer[i]=header[i];
    for (i=0;i<size;++i) buffer[MT_TXWI_BYTES+i]=frame[i];
    error=mt_ring_submit(&q->ring,address,MT_TXWI_BYTES,&index);
    if (error) return fail(q,error);
    q->states[id]=1;q->slots[index]=id;++q->inflight;q->cursor=(id+1u)%q->tokens;
    if (pid) {
        mt_tx_status_sample *sample=&q->txs.samples[q->txs.requests++];
        sample->pid=pid;sample->fc=get16(frame);sample->sequence=get16(frame+22)>>4;
        sample->wcid=wcid;sample->rate_table=rate;
        for (i=0;i<6;++i) sample->receiver[i]=frame[4+i];
    }
    q->io.barrier(q->io.context); /* payload, TXP and descriptor precede doorbell */
    error=q->io.publish(q->io.context,q->ring.head);
    if (error) return fail(q,error); /* publication may already have reached DMA */
    *token=id;return MT_OK;
}
static void count_txs(uint32_t *value) {if (*value!=0xffffffffu) ++*value;}
void mt_raw_tx_txs(mt_raw_tx *q,const uint8_t *data,uint32_t size) {
    uint32_t offset,word0,word1,word2,pid,i;
    mt_tx_status_sample *sample;
    if (!q || !q->prepared || !data || size<4 || (data[3]>>3)!=0) return;
    count_txs(&q->txs.envelopes);
    /* Preserve the first observed envelope even when later validation fails.
     * This bounded prefix is evidence, never a matched status or ownership. */
    if (!q->txs.first_size) {
        q->txs.first_size=size;
        q->txs.first_bytes=size<64u?size:64u;
        for (i=0;i<q->txs.first_bytes;++i) q->txs.first_raw[i]=data[i];
    }
    /* Validate the whole PCIe batch before counting records or storing any.
     * Do not confuse the other skb path's 8-byte header/32-byte entries. */
    if (size<16u+MT_TXS_BYTES || (get32(data)&0xffffu)!=size ||
        (size-16u)%MT_TXS_BYTES) {count_txs(&q->txs.malformed);return;}
    for (offset=16;offset<size;offset+=MT_TXS_BYTES) {
        const uint8_t *record=data+offset;
        count_txs(&q->txs.records);
        word0=get32(record);word1=get32(record+4);word2=get32(record+8);
        if ((word0>>23)&3u) {count_txs(&q->txs.unsupported);continue;}
        pid=get32(record+12)>>24;
        if (!(word0&0x8000u) || pid<3u || pid-3u>=q->txs.requests) {
            count_txs(&q->txs.unmatched);continue;
        }
        sample=&q->txs.samples[pid-3u];
        if (sample->pid!=pid || sample->sequence!=(word1>>20) ||
            sample->wcid!=((word2>>16)&0xfffu)) {
            count_txs(&q->txs.unmatched);continue;
        }
        if (sample->matched) {count_txs(&q->txs.duplicates);continue;}
        for (i=0;i<MT_TXS_BYTES;++i) sample->raw[i]=record[i];
        sample->matched=1;++q->txs.matched;
    }
}
static void release_if_complete(mt_raw_tx *q,uint32_t id) {
    if (q->states[id]==7u) {q->states[id]=0;--q->inflight;}
}
int mt_raw_tx_dma(mt_raw_tx *q,uint32_t dma_index) {
    uint32_t count,i,index,id;
    int error;
    if (!live(q)) return MT_INVALID;
    if (dma_index>=q->ring.count) return fail(q,MT_FW_PROTOCOL);
    q->io.barrier(q->io.context);
    count=(dma_index+q->ring.count-q->ring.tail)%q->ring.count;
    if (count>q->ring.queued) return fail(q,MT_FW_PROTOCOL);
    /* Validate the complete batch before changing queue or token ownership. */
    for (i=0;i<count;++i) {
        id=q->slots[(q->ring.tail+i)%q->ring.count];
        if (id>=q->tokens || !(q->states[id]&1u) || (q->states[id]&2u)) return fail(q,MT_FW_PROTOCOL);
    }
    for (i=0;i<count;++i) {
        error=mt_ring_reclaim(&q->ring,dma_index,&index);if (error) return fail(q,error);
        id=q->slots[index];q->slots[index]=0xffffffffu;q->states[id]|=2u;
        release_if_complete(q,id);
    }
    return count?MT_OK:MT_PENDING;
}
int mt_raw_tx_free(mt_raw_tx *q,const uint8_t *data,uint32_t size) {
    uint16_t ids[1023];
    uint32_t declared,total,n=0,offset,info,i,id,j;
    if (!live(q)) return MT_INVALID;
    if (!data || size<8) return fail(q,MT_FW_PROTOCOL);
    info=get32(data);declared=info&0xffffu;total=(info>>16)&0x3ffu;
    if ((info>>27)!=6 || declared<8 || declared>size || (declared&3u) ||
        ((get32(data+4)>>16)&15u)<4 || total>q->tokens) return fail(q,MT_FW_PROTOCOL);
    for (offset=8;offset<declared;offset+=4) {
        info=get32(data+offset);
        if (info&0xc0000000u) continue; /* WCID group or statistics record */
        for (i=0;i<2;++i) {
            id=(info>>(15*i))&0x7fffu;if (id==0x7fffu) continue;
            if (n>=total || id>=q->tokens || !(q->states[id]&1u) || (q->states[id]&4u))
                return fail(q,MT_FW_PROTOCOL);
            for (j=0;j<n;++j) if (ids[j]==id) return fail(q,MT_FW_PROTOCOL);
            ids[n++]=(uint16_t)id;
        }
    }
    if (n!=total) return fail(q,MT_FW_PROTOCOL);
    /* Preserve firmware TX-free statistics only after validating the complete
     * notification. Status zero is not recipient ACK proof for multicast. */
    for (offset=8;offset<declared;offset+=4) {
        info=get32(data+offset);
        if (info&0x80000000u) continue;
        if (info&0x40000000u) {
            ++q->status_count[(info>>28)&3u];
            q->status_attempts+=(info>>24)&15u;
        }
    }
    for (i=0;i<n;++i) {q->states[ids[i]]|=4u;release_if_complete(q,ids[i]);}
    return MT_OK;
}
