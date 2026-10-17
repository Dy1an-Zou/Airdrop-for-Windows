/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2025 MediaTek Inc.
 * Layout and opaque forwarding from pinned mt7925/regd.c/h.
 * Bounds validation, hardware-variant selection and atomic outputs are port
 * additions. No cryptographic verification is claimed by the host parser.
 */
#include "regulatory.h"
static uint32_t le16(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8;}
static uint32_t le32(const uint8_t *p) {return le16(p)|le16(p+2)<<16;}
static void put16(uint8_t *p,uint32_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v) {put16(p,v);put16(p+2,v>>16);}
int mt_regd_request(const uint8_t *ram, uint32_t size, uint32_t hw,
                     const uint8_t alpha[2], uint8_t *out, uint32_t capacity) {
    mt_fw_plan plan;
    const uint8_t *selected=0,*country=0;
    uint32_t i,table,regions=0,offset,length=0,count=0,total;
    if (!out || !alpha || hw>3 ||
        !((alpha[0]=='0' && alpha[1]=='0') ||
          (alpha[0]>='A' && alpha[0]<='Z' && alpha[1]>='A' && alpha[1]<='Z')) ||
        mt_fw_validate(ram,size,MT_FW_RAM,&plan)) return MT_INVALID;
    table=size-36-plan.count*40;
    for (i=0;i<plan.count;++i) {
        const mt_fw_region *r=&plan.regions[i];
        if (!r->skip || ram[table+i*40+25]!=2) continue;
        if (regions++) return MT_INVALID;
        offset=0;
        while (offset<r->length) {
            const uint8_t *b=ram+r->offset+offset;
            uint32_t n;
            if (r->length-offset<16) return MT_INVALID;
            n=le32(b);
            if (n<16 || n>r->length-offset || b[4]>=4) return MT_INVALID;
            /* Regulatory blocks use t2.type at byte 6, not t0.type at 7. */
            if (b[4]==3 && (b[6]&3u)==hw) {
                if (selected) return MT_INVALID;
                selected=b;
            }
            offset+=n;
        }
    }
    if (!selected || selected[5]!=1) return MT_INVALID;
    offset=16;total=le32(selected);
    while (total-offset>=16) {
        const uint8_t *cc=selected+offset;
        uint32_t n=le32(cc+4),bytes;
        if (n>MT_REGD_MAX_RULES) return MT_INVALID;
        bytes=36+n*32; /* CC header + rule header + rules + signature */
        if (bytes>total-offset) return MT_INVALID;
        if (cc[0]==alpha[0] && cc[1]==alpha[1]) {
            if (country || !n || cc[2]!=1 || cc[8]!=1 || cc[16]!=alpha[0] || cc[17]!=alpha[1])
                return MT_INVALID;
            country=cc;count=n;length=bytes-16;
        }
        offset+=bytes;
    }
    /* Upstream stops at an incomplete CC header. Pinned hw1 has an opaque
     * 8-byte trailer; never parse those bytes as a country or signature. */
    if (total-offset!=0 && total-offset!=8) return MT_INVALID;
    if (!country || capacity<MT_REGD_REQUEST_HEADER+length) return MT_INVALID;
    for (i=0;i<MT_REGD_REQUEST_HEADER;++i) out[i]=0;
    put16(out+4,6);put16(out+6,MT_REGD_REQUEST_HEADER+length-4);
    out[8]=country[2];out[9]=country[8];put32(out+12,length);
    out[16]=alpha[0];out[17]=alpha[1];put32(out+20,count);
    for (i=0;i<length;++i) out[MT_REGD_REQUEST_HEADER+i]=country[16+i];
    return (int)(MT_REGD_REQUEST_HEADER+length);
}
int mt_regd_response(uint8_t sequence,const uint8_t *p,uint32_t size,mt_regdomain *out) {
    const uint8_t *data;
    uint32_t declared,count,i,offset,bytes;
    if (!p || !out || !sequence || sequence>15 || size<40) return MT_INVALID;
    declared=le16(p+32);
    if (declared<8 || declared>size-32) return MT_INVALID;
    if (p[37]!=sequence || (p[38]&4u)) return MT_PENDING;
    if (p[36]==1) {
        if (declared<20) return MT_INVALID;
        if (p[44]!=0x2c) return MT_PENDING;
        return le32(p+48)?MT_FW_PROTOCOL:MT_PENDING;
    }
    if (declared<20) return MT_INVALID;
    data=p+44;
    /* Pinned firmware response observed in trial r4: tag0 and a length
     * covering only rule entries. The outbound signed-table request uses6. */
    if (le16(data+4)!=0) return MT_PENDING;
    if (declared<40) return MT_INVALID;
    count=le32(data+8);
    if (!count || count>MT_REGD_MAX_RULES || data[12]>3) return MT_INVALID;
    bytes=28+count*32;
    if (declared!=12+bytes || le16(data+6)!=count*32) return MT_INVALID;
    for (i=0;i<count;++i) {
        uint32_t start,end,bw;
        offset=28+i*32;start=le32(data+offset);end=le32(data+offset+4);bw=le32(data+offset+8);
        if (!start || start>=end || !bw || bw>320) return MT_INVALID;
    }
    /* No callbacks/failure after here; commit only fully validated data. */
    out->count=count;out->dfs_region=data[12];
    for (i=0;i<MT_REGD_MAX_RULES;++i) {
        mt_reg_rule *r=&out->rules[i];
        offset=28+i*32;
        r->start_mhz=i<count?le32(data+offset):0;
        r->end_mhz=i<count?le32(data+offset+4):0;
        r->bandwidth_mhz=i<count?le32(data+offset+8):0;
        r->eirp=i<count?le32(data+offset+12):0;
        r->flags=i<count?le32(data+offset+16):0;
    }
    return MT_OK;
}
