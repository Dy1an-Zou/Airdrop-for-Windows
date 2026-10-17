/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2023 MediaTek Inc.
 * SAR layouts/limits: mt792x_acpi_sar.c/h, mac80211.c; SKU: mt7925/mcu.c/h;
 * monitor context: mt76_connac_mcu_uni_add_dev and mt7925/main.c.
 * Acquisition status, finite cap, complete preflight and lifecycle gates are
 * Windows port additions. No ACPI evaluation or device installation here.
 */
#include "tx_init.h"
static void put16(uint8_t *p,uint32_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void zero(uint8_t *p,uint32_t n) {uint32_t i;for(i=0;i<n;++i)p[i]=0;}
static int fits(const uint8_t *p,uint32_t n,uint32_t h,uint32_t row,uint32_t min) {
    uint32_t count;
    if (n<h || (n-h)%row) return 0;
    count=(n-h)/row;
    return count>=min && count<=8 && p[h-1]==count;
}
static int table(uint32_t state,const uint8_t *p,uint32_t n,uint32_t geo,
                 uint32_t *header,uint32_t *stride) {
    uint32_t i,j,bands;
    *header=0;*stride=0;
    if (state==MT_ACPI_ABSENT) return !p && !n ? MT_OK : MT_INVALID;
    if (state!=MT_ACPI_DATA || !p) return MT_INVALID;
    if (fits(p,n,6,geo?5u:6u,geo?3u:1u)) {*header=6;*stride=geo?5u:6u;}
    else if (fits(p,n,7,geo?7u:12u,geo?3u:1u)) {*header=7;*stride=geo?7u:12u;}
    else return MT_INVALID;
    /* 0xff is explicitly no limit. Other out-of-range signed power encodings
     * are not guessed or wrapped; an OEM table must be investigated first. */
    bands=geo?(*stride-1)/2:*stride-1;
    for(i=*header;i<n;i+=*stride) for(j=0;j<bands;++j) {
        uint8_t v=p[i+1+j*(geo?2u:1u)];
        if(v>127 && v!=255) return MT_INVALID;
    }
    return MT_OK;
}
int mt_tx_policy_prepare(mt_tx_policy *out,uint32_t ds,const uint8_t *d,uint32_t dn,
    uint32_t gs,const uint8_t *g,uint32_t gn,uint32_t region,int32_t cap) {
    uint32_t dh=0,dr=0,gh=0,gr=0,i,geo_row;
    int error;
    if (!out) return MT_INVALID;
    out->prepared=0;out->dfs_region=0;
    for(i=0;i<5;++i) out->half_dbm[i]=0;
    if (region>3 || cap < -127 || cap>126) return MT_INVALID;
    error=table(ds,d,dn,0,&dh,&dr);
    if (!error) error=table(gs,g,gn,1,&gh,&gr);
    if (error) return error;
    geo_row=region==1?0u:region==2?1u:2u;
    for(i=0;i<5;++i) {
        int32_t value=127;
        if(dh && d[4]) {
            value=d[dh+1+i]==255?127:d[dh+1+i];
            if(gh) {
                uint32_t at=gh+geo_row*gr+1+(i?2u:0u);
                if(g[at]!=255) {
                    value+=g[at+1];
                    if(value>127)value=127;
                    if(value>g[at])value=g[at];
                }
            }
        }
        out->half_dbm[i]=value<cap?value:cap;
    }
    out->dfs_region=region;out->prepared=1;return MT_OK;
}
static int live(const mt_mcu_runtime *r) {
    return r && r->active && r->mcu.last_state==3 && !r->mcu.fault &&
        !r->rx.fault && !r->data_rx.fault && r->mac_initialized &&
        r->power_configured && r->data_prepared &&
        (!r->data_tx || (!r->data_tx->active && !r->data_tx->fault));
}
static int command(mt_mcu_runtime *r,uint32_t id,const uint8_t *p,uint32_t n,int wait) {
    uint32_t status=0xffffffffu;
    int error;
    if(!live(r))return MT_INVALID;
    error=r->firmware.send(r->firmware.context,MT_MCU_UNI|id,p,n,wait,&status);
    if(!error && wait && status)error=MT_FW_PROTOCOL;
    return error;
}
static void sku(uint8_t *out,uint32_t band,int32_t value) {
    uint32_t i,row;
    uint8_t v=(uint8_t)value;
    for(i=0;i<449;++i)out[i]=127;
    if(band==1)for(i=0;i<4;++i)out[i]=v;
    for(i=4;i<12;++i)out[i]=v;
    for(i=44;i<61;++i)out[i]=v;
    /* Each 12-byte VHT slot contains ten rates and two reserved bytes. */
    for(row=0;row<4;++row)for(i=0;i<10;++i)out[61+row*12+i]=v;
    for(i=109;i<449;++i)out[i]=v;
}
static int chip_command(mt_mcu_runtime *r,const uint8_t *text,uint32_t size) {
    uint8_t request[336];
    uint32_t i;
    if(!size || size>320)return MT_INVALID;
    zero(request,336);put16(request+4,2);put16(request+6,332);put16(request+12,size);
    for(i=0;i<size;++i)request[16+i]=text[i];
    return command(r,0x0e,request,336,0);
}
int mt_tx_initialize(mt_mcu_runtime *r,const mt_tx_policy *p,uint32_t bands,
    const uint8_t alpha[2],const uint8_t mac[6],mt_tx_init_result *out) {
    static const uint8_t list5[46]={36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,
        100,102,104,106,108,110,112,114,116,118,120,122,124,126,128,132,134,136,
        138,140,142,144,149,151,153,155,157,159,161,165,167};
    static const uint8_t full_power[14]="KeepFullPwr 1";
    static const uint8_t thermal_g[]="ThermalProtGband 0 100 90 80 30 1 1 115 105 5";
    static const uint8_t thermal_a[]="ThermalProtAband 1 100 90 80 30 1 1 115 105 5";
    uint8_t request[1402]; /* 52-byte header + three (channel + 449-byte SKU). */
    uint32_t i,band,start,count,j,n,total,ch,freq,range,any=0;
    int error=MT_INVALID;
    if(!out)return MT_INVALID;
    out->full_power_sent=0;out->device_enabled=0;out->bss_enabled=0;out->sku_batches=0;out->thermal_sent=0;out->error=0;
    if(!live(r) || r->tx_init_attempted || !p || p->prepared!=1 || p->dfs_region>3 ||
        !bands || (bands&~3u) || !alpha || !mac || (mac[0]&1u) ||
        !((alpha[0]=='0' && alpha[1]=='0') ||
        (alpha[0]>='A' && alpha[0]<='Z' && alpha[1]>='A' && alpha[1]<='Z')))goto done;
    for(i=0;i<6;++i)any|=mac[i];
    if(!any)goto done;
    for(i=0;i<5;++i)if(p->half_dbm[i] < -127 || p->half_dbm[i]>126)goto done;
    r->tx_init_attempted=1;r->tx_initialized=0;
    error=chip_command(r,full_power,sizeof(full_power));
    if(error)goto fault;
    out->full_power_sent=1;
    error=chip_command(r,thermal_g,sizeof(thermal_g));
    if(error)goto fault;
    out->thermal_sent=1;
    error=chip_command(r,thermal_a,sizeof(thermal_a));
    if(error)goto fault;
    out->thermal_sent=2;
    zero(request,16);request[1]=255;put16(request+6,12);request[8]=1;
    for(i=0;i<6;++i)request[10+i]=mac[i];
    error=command(r,1,request,16,1);
    if(error)goto fault;
    out->device_enabled=1;
    zero(request,36);put16(request+6,32);request[8]=1;request[11]=255;
    request[12]=2;request[14]=1;request[16]=1;
    put16(request+24,MT_MONITOR_WCID);put16(request+30,MT_MONITOR_WCID);
    error=command(r,2,request,36,1);
    if(error)goto fault;
    out->bss_enabled=1;
    for(band=1;band<=2;++band) {
        if(!(bands&(1u<<(band-1))))continue;
        total=band==1?14u:46u;
        for(start=0;start<total;start+=count) {
            count=total-start;if(count>3)count=3;
            n=52+count*450;zero(request,52);put16(request+4,1);put16(request+6,n-4);
            request[12]=(uint8_t)count;request[13]=(uint8_t)band;
            request[14]=(uint8_t)(start+count==total);
            request[16]=alpha[0];request[17]=alpha[1];
            for(j=0;j<count;++j) {
                ch=band==1?start+j+1:list5[start+j];
                freq=5000+ch*5;
                range=band==1?0u:freq<5350?1u:freq<5470?2u:freq<5725?3u:4u;
                request[52+j*450]=(uint8_t)ch;
                sku(request+53+j*450,band,p->half_dbm[range]);
            }
            error=command(r,0x2c,request,n,1);
            if(error)goto fault;
            ++out->sku_batches;
        }
    }
    r->tx_initialized=1;goto done;
fault:
    r->mcu.fault=error;
    if(r->data_tx)mt_raw_tx_disarm(r->data_tx);
done:
    out->error=error;return error;
}
