/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2023 MediaTek Inc.
 * RXD layout/group ordering follows pinned mt7925/mac.c and
 * mt76_connac3_mac.h; bounded radio-link serialization is a Windows addition.
 */
#include "raw_rx.h"
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void put16(uint8_t *p, uint32_t n) {p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8);}
static void put32(uint8_t *p, uint32_t n) {put16(p,n);put16(p+2,n>>16);}
/* Shared layout calculation only: callers keep their own admission policy. */
static int frame_offset(uint32_t d1,uint32_t d2,uint32_t total,uint32_t *result) {
    static const uint32_t bits[5]={19,16,17,18,20};
    uint32_t offset=32,i,n;
    if (total<32 || ((d1&(1u<<20)) && !(d1&(1u<<18)))) return MT_INVALID;
    for (i=0;i<5;++i) if (d1&(1u<<bits[i])) {
        n=i==4 ? 96u : 16u;
        if (n>total-offset) return MT_INVALID;
        offset+=n;
    }
    n=2*((d2>>13)&7u);
    if (n>total-offset) return MT_INVALID;
    *result=offset+n;return MT_OK;
}
static int decode(const uint8_t *p, uint32_t size, uint32_t fcs,
    uint8_t *out, uint32_t capacity, uint32_t *length, uint32_t *reason, uint32_t *own) {
    uint32_t d0,d1,d2,d3,total,type,offset,i,frame,fc,hdr,amsdu;
    uint32_t channel,frequency,channel_flags;
    *reason=MT_RX_MALFORMED;*own=0;
    if (length) *length=0;
    if (!p || !out || !length || size<4 || fcs>1) return MT_INVALID;
    d0=le32(p);total=d0&0xffffu;type=d0>>27;
    if (total<4 || total>size) return MT_INVALID;
    if (type!=2 && type!=8 && !((d0>>16 & 0x380fu)==0x3801u) &&
        !(type==7 && ((d0>>16)&15u)==1)) {*reason=MT_RX_OTHER;return MT_PENDING;}
    if (total<32) return MT_INVALID;
    d1=le32(p+4);d2=le32(p+8);d3=le32(p+12);amsdu=le32(p+16)&3u;
    *own=((d3>>16)&3u)==1;
    /* HDR_TRANS_ERROR is not an air error in raw mode (pinned mt7925/mac.c).
     * Actual translated frames remain rejected by the following policy gate. */
    if ((d1&0x06000000u) || (d2&0x01800000u) || (d3&0x01000000u)) {
        *reason=MT_RX_AIR_ERROR;return MT_PENDING;
    }
    /* AWDL consumes raw clear 802.11 frames; never pretend Ethernet/stripped
     * cipher payloads are those frames. Monitor setup will disable translation. */
    if (d2&0x80u) {*reason=MT_RX_TRANSLATED;return MT_PENDING;}
    if (d2&0x001f0000u) {*reason=MT_RX_CIPHER;return MT_PENDING;}
    if (frame_offset(d1,d2,total,&offset)) return MT_INVALID;
    frame=total-offset;
    if (frame<2+4*fcs) return MT_INVALID;
    fc=(uint32_t)p[offset]|(uint32_t)p[offset+1]<<8;
    if (fc&3u) return MT_INVALID;
    if (fc&0x4000u) {*reason=MT_RX_PROTECTED;return MT_PENDING;}
    type=(fc>>2)&3u;
    if (type==1 || type==3) {*reason=MT_RX_CONTROL;return MT_PENDING;}
    hdr=24;
    if (type==2) {
        if ((fc&0x300u)==0x300u) hdr+=6;
        if (fc&0x80u) {hdr+=2;if (fc&0x8000u) hdr+=4;}
    }
    if (frame<hdr+4*fcs+(amsdu ? 2u : 0u)) return MT_INVALID;
    if (amsdu && (type!=2 || !(fc&0x80u))) return MT_INVALID;
    channel=(d3>>8)&255u;
    if (!channel) return MT_INVALID;
    if (channel>180) {frequency=5950+5*((channel-181)*4+1);channel_flags=0;}
    else if (channel>14) {frequency=5000+5*channel;channel_flags=0x100;}
    else {frequency=channel==14 ? 2484u : 2407+5*channel;channel_flags=0x80;}
    if (amsdu) frame-=2;
    if (frame>16384u-14u) return MT_INVALID;
    if (capacity<frame+22) {*reason=MT_RX_CAPACITY;return MT_FULL;}
    /* radio_link RX=2; radiotap FLAGS + CHANNEL, aligned to 2 bytes.
     * No RSSI/rate/TSFT is fabricated when metadata is unavailable. */
    for (i=0;i<22;++i) out[i]=0;
    put16(out,2);put32(out+4,frame+14);put16(out+10,14);put32(out+12,10);
    out[16]=(uint8_t)(fcs ? 0x10 : 0);put16(out+18,frequency);put16(out+20,channel_flags);
    for (i=0;i<frame;++i) out[22+i]=p[offset+i+(amsdu && i>=hdr ? 2u : 0u)];
    *length=frame+22;
    *reason=MT_RX_ACCEPT;
    return MT_OK;
}
/* Called only after decode established a complete normal RXD and rejected U2M.
 * A good RXD FCS flag alone does not establish an ACK or peer identity. Preserve
 * actual bytes even when FC is not an ACK; never change the rejected result. */
static void sample_short(const uint8_t *p,uint32_t fcs,mt_rx_stats *stats) {
    uint32_t d1=le32(p+4),d2=le32(p+8),d3=le32(p+12),total=le32(p)&0xffffu;
    uint32_t offset=0,frame,i,n;
    if (!(d2&0x02000000u) || (d2&0x80u) || (d3&0x01000000u)) return;
    if (stats->short_candidates!=0xffffffffu) ++stats->short_candidates;
    if (frame_offset(d1,d2,total,&offset) || total-offset<10u+4u*fcs) {
        if (stats->short_malformed!=0xffffffffu) ++stats->short_malformed;
        return;
    }
    if (stats->short_header_bytes) return;
    frame=total-offset-4u*fcs;n=frame<16u ? frame : 16u;
    stats->short_offset=offset;stats->short_frame_bytes=frame;
    stats->short_rxd[0]=d1;stats->short_rxd[1]=d2;stats->short_rxd[2]=d3;
    for(i=0;i<16;++i) stats->short_header[i]=i<n ? p[offset+i] : 0;
    stats->short_header_bytes=n;
}
int mt_rx_radio_record_measured(const uint8_t *p,uint32_t size,uint32_t fcs,
    uint8_t *out,uint32_t capacity,uint32_t *length,mt_rx_stats *stats) {
    static const uint32_t words[6]={1,1,2,2,2,3},bits[6]={25,26,23,24,25,24};
    uint32_t reason,own,i;
    int result=decode(p,size,fcs,out,capacity,length,&reason,&own);
    if (stats) {
        if (stats->outcomes[reason]!=0xffffffffu) ++stats->outcomes[reason];
        if (own && stats->to_local[reason]!=0xffffffffu) ++stats->to_local[reason];
        /* own+AIR_ERROR is established only after a complete normal RXD. */
        if (own && reason==MT_RX_AIR_ERROR) {
            for(i=0;i<6;++i) if ((le32(p+4*words[i])&(1u<<bits[i])) &&
                stats->local_error_bits[i]!=0xffffffffu) ++stats->local_error_bits[i];
            if (!stats->first_local_error[0])
                for(i=0;i<8;++i) stats->first_local_error[i]=le32(p+4*i);
        }
        if (own && reason!=MT_RX_ACCEPT) sample_short(p,fcs,stats);
    }
    return result;
}
int mt_rx_radio_record(const uint8_t *p,uint32_t size,uint32_t fcs,
    uint8_t *out,uint32_t capacity,uint32_t *length) {
    return mt_rx_radio_record_measured(p,size,fcs,out,capacity,length,0);
}
int mt_radio_receive(mt_rx_ring *r, const uint8_t *pool, const mt_rx_io *io,
    uint8_t *scratch, uint32_t scratch_capacity, uint32_t fcs,
    uint8_t *out, uint32_t capacity, uint32_t *length) {
    uint32_t received=0;
    int error;
    if (length) *length=0;
    if (!out || !length || fcs>1) return MT_INVALID;
    error=mt_rx_receive(r,pool,scratch,scratch_capacity,&received,io);
    return error ? error : mt_rx_radio_record(scratch,received,fcs,out,capacity,length);
}
