/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2023 MediaTek Inc.
 * Commands from mt7925/mcu.c; channel numbers from mt76/mac80211.c.
 * Flag values correspond to Linux v6.18 nl80211.h/cfg80211.h. Range coverage,
 * lifecycle, conservative overlap and TX eligibility are Windows additions.
 */
#include "channel_control.h"
static const uint8_t channels[42]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,
    36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144,
    149,153,157,161,165,169,173,177};
static void put16(uint8_t *p,uint32_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v) {put16(p,v);put16(p+2,v>>16);}
static uint32_t frequency(uint32_t c) {return c==14?2484:c<14?2407+5*c:5000+5*c;}
static int live(const mt_channel_control *c) {
    const mt_mcu_runtime *r;
    if (!c || !c->prepared || c->fault || !c->runtime) return 0;
    r=c->runtime;
    return r->active && !r->mcu.fault && !r->rx.fault && !r->data_rx.fault &&
        r->mac_initialized && r->power_configured && r->data_prepared;
}
static int supported(const mt_channel_control *c,uint32_t ch) {
    uint32_t i;
    if (!(c->bands&(ch<=14?1u:2u))) return 0;
    if (ch>=169 && !(c->clc_config&1u)) return 0;
    for (i=0;i<42;++i) if (channels[i]==ch) return 1;
    return 0;
}
static int coverage(const mt_channel_control *c,uint32_t center,uint32_t width,uint32_t *flags) {
    uint32_t low=center-width/2,high=center+width/2,cursor=low,i,round,combined=0;
    const mt_regdomain *d=c->domain;
    for (i=0;i<d->count;++i) {
        const mt_reg_rule *r=&d->rules[i];
        if (r->start_mhz<=low && r->end_mhz>=high && r->bandwidth_mhz>=width) cursor=high;
    }
    for (round=0;round<d->count && cursor<high;++round) {
        uint32_t end=cursor;
        for (i=0;i<d->count;++i) {
            const mt_reg_rule *r=&d->rules[i];
            if (r->start_mhz<=cursor && r->end_mhz>end && (r->flags&0x800u)) end=r->end_mhz;
        }
        if (end==cursor) return 0;
        cursor=end;
    }
    if (cursor<high) return 0;
    /* All overlapping restrictions apply, even when another rule covers it. */
    for (i=0;i<d->count;++i) {
        const mt_reg_rule *r=&d->rules[i];
        if (r->start_mhz<high && r->end_mhz>low) combined|=r->flags;
    }
    *flags=combined;return 1;
}
static int geometry(const mt_channel_control *c,uint32_t primary,uint32_t center,uint32_t width,uint32_t *flags) {
    uint32_t i,base,sub;
    if (!supported(c,primary) || (width!=20 && width!=40 && width!=80)) return 0;
    if (width==20) {if (center!=primary) return 0;}
    else if (width==40) {
        if (primary==14 || (primary+2!=center && center+2!=primary) || center<3) return 0;
        if (!supported(c,center-2) || !supported(c,center+2) ||
            (primary<=14 && center+2>13)) return 0;
        if (primary>14) {
            for (i=14;i<42;i+=2) if (channels[i]+2u==center) break;
            if (i==42) return 0;
        }
    } else {
        if (primary<=14) return 0;
        for (i=14;i<42;i+=4) if (channels[i]+6u==center) break;
        if (i==42) return 0;
        base=channels[i];
        if (primary<base || primary>base+12 || (primary-base)%4) return 0;
        for (sub=0;sub<4;++sub) if (!supported(c,base+4*sub)) return 0;
    }
    if (!coverage(c,frequency(center),width,flags)) return 0;
    if (width==80 && (*flags&0x8000u)) return 0;
    if (width==40 && (*flags&(primary<center?0x4000u:0x2000u))) return 0;
    return 1;
}
static uint32_t channel_flags(uint32_t raw) {
    static const uint8_t from[19]={0,3,4,7,8,12,13,14,15,16,17,18,19,20,21,22,23,24,25};
    static const uint8_t to[19]={6,9,3,1,1,10,5,4,7,8,13,19,20,2,21,22,23,25,26};
    uint32_t i,result=0;
    for (i=0;i<19;++i) if (raw&(1u<<from[i])) result|=1u<<to[i];
    /* Unknown rule flags and PTP/PTMP-only modes have no supported TX policy. */
    if ((raw&~0x03fff9ffu) || (raw&0x60u)) result|=2u;
    return result;
}
static int command(mt_channel_control *c,uint32_t id,const uint8_t *data,uint32_t size) {
    uint32_t status=0xffffffffu;
    int error;
    if (!live(c)) return MT_INVALID;
    error=c->runtime->firmware.send(c->runtime->firmware.context,MT_MCU_UNI|id,data,size,1,&status);
    if (!error && status) error=MT_FW_PROTOCOL;
    if (error) {c->fault=error;c->monitor=0;c->configured=0;c->runtime->mcu.fault=error;}
    return error;
}
int mt_channel_prepare(mt_channel_control *c,mt_mcu_runtime *r,const mt_regdomain *d,
    uint32_t bands,uint32_t clc,const uint8_t alpha[2],uint32_t env) {
    uint32_t i;
    if (!c || c->prepared || !r || !d || !alpha || env>2 || !(bands&3u) || (bands&~7u) || clc>255 ||
        !d->count || d->count>MT_REGD_MAX_RULES || d->dfs_region>3 ||
        !((alpha[0]=='0' && alpha[1]=='0') ||
          (alpha[0]>='A' && alpha[0]<='Z' && alpha[1]>='A' && alpha[1]<='Z'))) return MT_INVALID;
    for (i=0;i<d->count;++i) {
        const mt_reg_rule *rule=&d->rules[i];
        if (!rule->start_mhz || rule->start_mhz>=rule->end_mhz || rule->end_mhz>10000 ||
            !rule->bandwidth_mhz || rule->bandwidth_mhz>320) return MT_INVALID;
    }
    c->runtime=r;c->domain=d;c->bands=bands;c->clc_config=clc;c->environment=env;
    c->attempted=0;c->configured=0;c->monitor=0;c->primary=0;c->center=0;c->width=0;c->fault=0;
    c->alpha2[0]=alpha[0];c->alpha2[1]=alpha[1];c->reserved[0]=0;c->reserved[1]=0;c->prepared=1;
    return MT_OK;
}
int mt_channel_apply(mt_channel_control *c) {
    uint8_t request[352];
    static const uint8_t rts[16]={0,0,0,0,8,0,12,0,0x2b,9,0,0,2,0,0,0};
    uint32_t i,j,count=0,n2=0,flags,raw,temp;
    int error;
    if (!live(c) || c->attempted) return MT_INVALID;
    for (i=0;i<16;++i) request[i]=0;
    request[0]=c->alpha2[0];request[1]=c->alpha2[1];request[5]=2; /* 5GHz <=80MHz */
    put16(request+8,2);
    for (i=0;i<42;++i) {
        uint32_t ch=channels[i];
        if (!geometry(c,ch,ch,20,&raw)) continue;
        flags=channel_flags(raw);
        if ((raw&4u) && c->environment!=2) flags|=2u;
        if ((raw&8u) && c->environment!=1) flags|=2u;
        if (!geometry(c,ch,ch+2,40,&temp)) flags|=16u;
        if (ch<3 || !geometry(c,ch,ch-2,40,&temp)) flags|=32u;
        j=i<14?0:14+(i-14)/4*4;
        if (i<14 || !geometry(c,ch,channels[j]+6,80,&temp)) flags|=128u;
        flags|=0x00182100u; /* no160/no320/noHE/noEHT in this radio-link path */
        put16(request+16+8*count,ch);put16(request+18+8*count,0);put32(request+20+8*count,flags);
        ++count;if (i<14) ++n2;
    }
    if (!count) return MT_INVALID;
    request[12]=(uint8_t)n2;request[13]=(uint8_t)(count-n2);put16(request+10,8+8*count);
    c->attempted=1;
    error=command(c,0x15,request,16+8*count);
    if (!error) error=command(c,8,rts,16);
    if (!error) c->configured=1;
    return error;
}
int mt_channel_monitor(mt_channel_control *c,uint32_t primary,uint32_t center,uint32_t width) {
    static const uint8_t enable[12]={0,0,0,0,0,0,8,0,1,0,0,0};
    uint8_t config[20],filter[72];
    uint32_t i,flags;
    int error;
    if (!live(c) || !c->configured || !geometry(c,primary,center,width,&flags)) return MT_INVALID;
    for (i=0;i<20;++i) config[i]=0;
    put16(config+4,1);put16(config+6,16);config[10]=(uint8_t)(primary<=14?1:2);
    config[11]=(uint8_t)(width==80);config[12]=(uint8_t)primary;
    config[13]=(uint8_t)(primary<center?1:primary>center?3:0);config[14]=(uint8_t)center;config[16]=1;
    if (c->monitor) error=command(c,0x24,config,20);
    else {
        error=command(c,0x24,enable,12);
        if (!error) error=command(c,0x24,config,20);
        for (i=0;i<72;++i) filter[i]=0;
        put16(filter+4,12);put16(filter+6,68);put32(filter+12,0x80000060u);
        if (!error) error=command(c,8,filter,72);
    }
    if (!error) {c->monitor=1;c->primary=primary;c->center=center;c->width=width;}
    return error;
}
int mt_channel_monitor_stop(mt_channel_control *c) {
    static const uint8_t disable[12]={0,0,0,0,0,0,8,0,0,0,0,0};
    int error;
    if (!live(c) || !c->monitor) return MT_INVALID;
    error=command(c,0x24,disable,12);
    if (!error) c->monitor=0;
    return error;
}
int mt_channel_permits_tx(const mt_channel_control *c) {
    uint32_t flags;
    if (!live(c) || !c->monitor || !c->configured ||
        !geometry(c,c->primary,c->center,c->width,&flags)) return 0;
    if ((flags&~0x03fff9ffu) || (flags&0x1f1u)) return 0; /* OFDM, DFS, PTP/PTMP, no-IR */
    if ((flags&4u) && c->environment!=2) return 0;
    if ((flags&8u) && c->environment!=1) return 0;
    return 1;
}
