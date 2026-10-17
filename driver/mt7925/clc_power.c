/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2023 MediaTek Inc.
 * Power payload/selection adapted from mt7925/mcu.c __mt7925_mcu_set_clc.
 * Bounds preflight and one-shot runtime lifecycle are Windows port additions.
 */
#include "clc_power.h"
typedef struct power_view {
    const uint8_t *block;
    uint32_t rules_begin, rules_end, fallback;
} power_view;
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void put16(uint8_t *p, uint32_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static int match(const uint8_t *rule, const uint8_t *alpha) {
    return rule[0]==alpha[0] && rule[1]==alpha[1];
}
static int validate(power_view *v, const uint8_t *alpha, uint32_t capacity) {
    const uint8_t *b=v->block, *data=b+16;
    uint32_t size=le32(b)-16, segments=b[8], begin=segments*16, end;
    uint32_t i, exact=0, world=0;
    if ((b[5]!=1 && b[5]!=2) || !segments || begin>size) return MT_INVALID;
    end=le32(data+4);
    if (end<begin || end>size || (end-begin)%8 || end==begin) return MT_INVALID;
    for (i=0;i<segments;++i) {
        uint32_t offset=le32(data+16*i+4), length=le32(data+16*i+8);
        if (offset<end || offset>size || !length || length>size-offset ||
            length>MT_CLC_SCRATCH-MT_CLC_HEADER || length>capacity-MT_CLC_HEADER)
            return MT_INVALID;
    }
    for (i=begin;i<end;i+=8) {
        const uint8_t *rule=data+i;
        if (!rule[4] || rule[4]>segments) return MT_INVALID;
        if (match(rule,alpha)) ++exact;
        if (rule[0]=='0' && rule[1]=='0') ++world;
    }
    if (!exact && !world) return MT_INVALID;
    v->rules_begin=begin;v->rules_end=end;v->fallback=!exact;
    return MT_OK;
}
static int locate(const uint8_t *ram, uint32_t size, uint32_t hw,
                  const uint8_t *alpha, uint32_t capacity, power_view views[2]) {
    mt_fw_plan plan;
    uint32_t table, i, found=0;
    int error=mt_fw_validate(ram,size,MT_FW_RAM,&plan);
    if (error) return error;
    table=size-36-plan.count*40;
    for (i=0;i<plan.count;++i) {
        const mt_fw_region *region=&plan.regions[i];
        uint32_t offset=0;
        if (!region->skip || ram[table+i*40+25]!=2) continue;
        if (found++) return MT_INVALID;
        while (offset<region->length) {
            const uint8_t *b=ram+region->offset+offset;
            uint32_t length;
            if (region->length-offset<16) return MT_INVALID;
            length=le32(b);
            if (length<16 || length>region->length-offset || b[4]>=4) return MT_INVALID;
            /* Indices 2/3 have different headers and are not power tables. */
            if (b[4]<2 && (b[7]&3u)==hw) {
                power_view *v=&views[b[4]];
                if (v->block) return MT_INVALID;
                v->block=b;
                error=validate(v,alpha,capacity);
                if (error) return error;
            }
            offset+=length;
        }
    }
    return found && views[0].block && views[1].block ? MT_OK : MT_INVALID;
}
int mt_clc_power_apply(mt_mcu_runtime *r, const uint8_t *ram, uint32_t size,
    uint32_t hw, const uint8_t alpha[2], uint32_t environment,
    uint8_t *scratch, uint32_t capacity, mt_clc_result *out) {
    power_view views[2];
    static const uint8_t world[2]={'0','0'};
    uint32_t idx,i,j,status,config=0xff;
    int error=MT_INVALID;
    if (!out) return MT_INVALID;
    out->selected_mask=0;out->fallback_mask=0;out->requests_sent=0;
    out->channel_config=0;out->primary_error=0;
    if (!r || !r->active || r->mcu.last_state!=3 || r->clc_attempted ||
        r->mcu.fault || r->rx.fault || (r->data_prepared && r->data_rx.fault) ||
        !scratch || capacity<MT_CLC_HEADER || !alpha || hw>3 || environment>2 ||
        !((alpha[0]=='0' && alpha[1]=='0') ||
          (alpha[0]>='A' && alpha[0]<='Z' && alpha[1]>='A' && alpha[1]<='Z'))) goto done;
    views[0].block=0;views[1].block=0;
    error=locate(ram,size,hw,alpha,capacity,views);
    if (error) goto done;
    r->clc_attempted=1;r->power_configured=0;
    out->selected_mask=3;
    for (idx=0;idx<2;++idx) {
        const power_view *v=&views[idx];
        const uint8_t *b=v->block,*data=b+16,*country=v->fallback?world:alpha;
        if (v->fallback) out->fallback_mask|=1u<<idx;
        for (i=v->rules_begin;i<v->rules_end;i+=8) {
            const uint8_t *rule=data+i,*segment;
            uint32_t offset,length;
            if (!match(rule,country)) continue;
            if (!r->active) {error=MT_INVALID;goto fault;}
            segment=data+(rule[4]-1u)*16;
            offset=le32(segment+4);length=le32(segment+8);
            for (j=0;j<MT_CLC_HEADER;++j) scratch[j]=0;
            put16(scratch+4,3);put16(scratch+6,MT_CLC_HEADER+length-4);
            scratch[8]=b[5];put16(scratch+10,length);scratch[12]=(uint8_t)idx;
            scratch[13]=(uint8_t)(v->fallback?1:environment);
            scratch[16]=rule[0];scratch[17]=rule[1];scratch[18]=rule[2];scratch[19]=rule[3];
            for (j=0;j<length;++j) scratch[MT_CLC_HEADER+j]=data[offset+j];
            status=0xffffffffu;
            error=r->firmware.send(r->firmware.context,MT_MCU_UNI|0x2cu,
                                   scratch,MT_CLC_HEADER+length,1,&status);
            if (!error && status) error=MT_FW_PROTOCOL;
            if (error) goto fault;
            ++out->requests_sent;
            config=b[5]==1?0xff:rule[5];
        }
    }
    out->channel_config=config;r->power_configured=1;
    goto done;
fault:
    r->mcu.fault=error;
done:
    out->primary_error=error;
    return error;
}
