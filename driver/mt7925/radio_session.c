/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "radio_session.h"
#include "mac_init.h"
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void snapshot_rx(mt_radio_session *s,uint32_t index) {
    static const uint32_t offsets[3]={0x21400u,0x21404u,0xe800u};
    mt_radio_rx_snapshot *snapshot=&s->rx_snapshot[index];
    mt_mcu_runtime *r=s->runtime;
    uint32_t i,value;
    int error;
    snapshot->phase=index+1;snapshot->valid_mask=0;snapshot->first_error=0;
    for(i=0;i<3;++i) {
        snapshot->values[i]=0;value=0xffffffffu;
        error=r->active && r->hardware.read ?
            r->hardware.read(r->hardware.context,offsets[i],&value) : MT_INVALID;
        if (!error && value==0xffffffffu) error=MT_FW_PROTOCOL;
        if (!error) {snapshot->values[i]=value;snapshot->valid_mask|=1u<<i;}
        else if (!snapshot->first_error) snapshot->first_error=error;
    }
}
/* SKU initialization currently ends at channel165 (20MHz). Wider geometry
 * must also keep every constituent 20MHz channel inside that range. Actual
 * regulatory coverage and supported channel spacing are checked downstream. */
static int sku_channel(uint32_t primary,uint32_t center,uint32_t width) {
    uint32_t delta;
    if (width!=20 && width!=40 && width!=80) return 0;
    delta=width/10-2;
    if (!primary || !center || center>165 || center<=delta) return 0;
    if (primary<=14) return primary<=13 && center+delta<=13 && width!=80;
    return primary<=165 && center+delta<=165;
}
int mt_radio_config_valid(const mt_radio_config *c) {
    return c && !c->reserved[0] && !c->reserved[1] && c->environment<=2 && c->fcs<=1 &&
        c->cap_half_dbm>=-127 && c->cap_half_dbm<=126 &&
        sku_channel(c->primary,c->center,c->width) &&
        ((c->alpha2[0]=='0' && c->alpha2[1]=='0') ||
         (c->alpha2[0]>='A' && c->alpha2[0]<='Z' && c->alpha2[1]>='A' && c->alpha2[1]<='Z'));
}
int mt_radio_stop(mt_radio_session *s) {
    if (!s || !s->attempted || !s->chip || !s->runtime) return MT_INVALID;
    if (s->stopped) return s->stop_error;
    s->ready=0;s->channel.monitor=0;s->channel.configured=0;
    /* Hard stop is also valid after a failed MCU command: do not depend on
     * another firmware ACK to revoke DMA access. */
    s->stopped=1;
    s->stop_error=mt_chip_stop(s->chip);
    return s->stop_error;
}
static int fail(mt_radio_session *s,int error) {
    if (!s->error) s->error=error;
    (void)mt_radio_stop(s);
    return error;
}
static int live(mt_radio_session *s) {
    mt_mcu_runtime *r;
    int error;
    if (!s || !s->ready || s->stopped || !s->runtime || !s->chip) return MT_INVALID;
    r=s->runtime;
    error=r->mcu.fault ? r->mcu.fault : r->rx.fault ? r->rx.fault :
        r->data_rx.fault ? r->data_rx.fault : r->data_tx ? r->data_tx->fault : MT_INVALID;
    if (!error && (!r->active || !r->tx_initialized || !r->data_tx->active ||
        !s->channel.configured || !s->channel.monitor)) error=MT_INVALID;
    /* A platform disarm/removal means MMIO may already be inaccessible.
     * Revoke here, leave the platform lifecycle path to fence DMA. */
    if (error && !r->active) {s->ready=0;if (!s->error) s->error=error;return error;}
    return error ? fail(s,error) : MT_OK;
}
int mt_radio_start(mt_radio_session *s,mt_chip *chip,mt_mcu_runtime *r,
    const uint8_t *bundle,uint32_t size,const mt_radio_config *c,
    const mt_platform *platform,uint8_t *scratch,uint32_t capacity) {
    mt_fw_preflight check;
    mt_tx_policy validation;
    const uint8_t *ram;
    uint32_t ps,rs;
    int error;
    if (!s || s->attempted || !chip || chip->attempted || !r || !r->prepared ||
        !r->data_prepared || !r->data_tx || !r->data_tx->prepared || r->active ||
        r->used || r->sealed || !mt_radio_config_valid(c) || !scratch || capacity<MT_CLC_SCRATCH)
        return MT_INVALID;
    error=mt_fw_preflight_bundle(bundle,size,&check);
    if (!error) error=mt_platform_policy(platform,0,c->cap_half_dbm,&validation);
    if (error) return error;
    ps=le32(bundle+8);rs=le32(bundle+12);ram=bundle+16+ps;
    s->runtime=r;s->chip=chip;s->attempted=1;s->fcs=c->fcs;
    s->stage=1;error=mt_chip_start(chip,r);
    if (error) return fail(s,error);
    s->stage=2;error=mt_fw_boot(bundle+16,ps,ram,rs,&r->firmware,&s->firmware);
    if (error) return fail(s,error);
    s->stage=3;error=mt_pcie_query_nic(&r->mcu,&s->nic);
    if (!error && (!(s->nic.present&8u) || !(s->nic.chip_low&32u) || !(s->nic.bands&3u))) error=MT_INVALID;
    if (error) return fail(s,error);
    s->stage=4;error=mt_pcie_query_hw_type(&r->mcu,&s->hardware);
    if (error) return fail(s,error);
    s->stage=5;error=mt_pcie_query_regdomain(&r->mcu,ram,rs,s->hardware.encapsulation,
        c->alpha2,scratch,capacity,&s->domain);
    if (error) return fail(s,error);
    s->stage=6;error=mt_platform_policy(platform,s->domain.dfs_region,c->cap_half_dbm,&s->power);
    if (error) return fail(s,error);
    s->stage=7;error=mt_clc_power_apply(r,ram,rs,s->hardware.encapsulation,c->alpha2,
        c->environment,scratch,capacity,&s->clc);
    if (error) return fail(s,error);
    s->stage=8;error=mt_mac_initialize(r);
    if (error) return fail(s,error);
    s->stage=9;error=mt_tx_initialize(r,&s->power,s->nic.bands&3u,c->alpha2,s->nic.mac,&s->tx);
    if (error) return fail(s,error);
    s->stage=10;error=mt_channel_prepare(&s->channel,r,&s->domain,s->nic.bands&3u,
        s->clc.channel_config,c->alpha2,c->environment);
    if (!error) error=mt_channel_apply(&s->channel);
    if (error) return fail(s,error);
    s->stage=11;error=mt_channel_monitor(&s->channel,c->primary,c->center,c->width);
    if (!error) snapshot_rx(s,0);
    /* Fixed readback only after the existing RF/channel configuration succeeds.
     * Invalid/missing data follows the same STOP path as other MCU failures. */
    if (!error) error=mt_pcie_query_txpower(&r->mcu,&s->txpower);
    if (!error) error=mt_raw_tx_activate(r->data_tx);
    if (error) return fail(s,error);
    s->stage=12;s->ready=1;
    return MT_OK;
}
int mt_radio_service(mt_radio_session *s,uint32_t budget,uint32_t *consumed) {
    int error;
    if (consumed) *consumed=0;
    if (!consumed || !budget || budget>64) return MT_INVALID;
    error=live(s);
    if (error) return error;
    error=mt_runtime_service(s->runtime,budget,consumed);
    return error && error!=MT_PENDING ? fail(s,error) : error;
}
int mt_radio_channel(mt_radio_session *s,uint32_t primary,uint32_t center,uint32_t width) {
    int error=live(s);
    if (error) return error;
    if (!sku_channel(primary,center,width)) return MT_INVALID;
    if (s->runtime->data_tx->inflight) return MT_PENDING;
    error=mt_channel_monitor(&s->channel,primary,center,width);
    return s->channel.fault ? fail(s,error) : error;
}
int mt_radio_set_address(mt_radio_session *s,const uint8_t mac[6]) {
    /* Pinned mt76_connac_mcu_uni_add_dev and mt7925_mcu_del_dev use this
     * OMAC update, including an already active legacy BSS. Preserve its
     * index0/link0 and MT7925 band0xff, changing only the own address.
     */
    uint8_t request[16]={0,255,0,0,0,0,12,0,1,0,0,0,0,0,0,0};
    uint32_t i,any=0,status=0xffffffffu;
    int error;
    if (!mac || (mac[0]&1u)) return MT_INVALID;
    for (i=0;i<6;++i) any|=mac[i];
    if (!any) return MT_INVALID;
    error=live(s);
    if (error) return error;
    if (s->runtime->data_tx->inflight) return MT_PENDING;
    for (i=0;i<6;++i) request[10+i]=mac[i];
    error=s->runtime->firmware.send(s->runtime->firmware.context,MT_MCU_UNI|1u,
        request,16,1,&status);
    if (!error && status) error=MT_FW_PROTOCOL;
    if (!error) snapshot_rx(s,1);
    return error ? fail(s,error) : MT_OK;
}
int mt_radio_receive_record(mt_radio_session *s,uint8_t *scratch,uint32_t sc,
    uint8_t *out,uint32_t capacity,uint32_t *length) {
    uint32_t consumed;
    int error;
    if (length) *length=0;
    if (!scratch || sc<MT_DATA_RX_CAPACITY || !out || !length || capacity<MT_DATA_RX_CAPACITY+22u)
        return MT_INVALID;
    error=mt_radio_service(s,16,&consumed);
    if (error && error!=MT_PENDING) return error;
    error=mt_runtime_radio_receive(s->runtime,scratch,sc,s->fcs,out,capacity,length);
    if (s->runtime->mcu.fault || s->runtime->rx.fault || s->runtime->data_rx.fault)
        return fail(s,error ? error : MT_FW_PROTOCOL);
    return error;
}
int mt_radio_transmit(mt_radio_session *s,const uint8_t *frame,uint32_t size,
    uint32_t rate,uint32_t no_ack,uint32_t *token) {
    int error=live(s);
    if (error) return error;
    if (!mt_channel_permits_tx(&s->channel)) return MT_INVALID;
    error=mt_raw_tx_submit(s->runtime->data_tx,frame,size,MT_MONITOR_WCID,rate,no_ack,token);
    return s->runtime->data_tx->fault ? fail(s,error) : error;
}
