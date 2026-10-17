/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "radio_device.h"
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void put32(uint8_t *p,uint32_t n) {
    p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);p[2]=(uint8_t)(n>>16);p[3]=(uint8_t)(n>>24);
}
int mt_radio_device_close(mt_radio_device *d,uint32_t available) {
    if (!d || !d->runtime || !d->chip) return MT_INVALID;
    if (!available) {
        d->session.ready=0;
        d->session.channel.monitor=0;d->session.channel.configured=0;
        mt_runtime_disarm(d->runtime);
        return MT_OK; /* Does not claim stop/DMA quiescence. */
    }
    return d->session.attempted ? mt_radio_stop(&d->session) : MT_OK;
}
static void reply(mt_radio_device *d,int result,uint32_t token,uint32_t bytes,uint8_t *out) {
    mt_radio_session *s=&d->session;
    uint32_t i,ready=s->ready && d->runtime->active && !s->stopped && !s->error;
    for (i=0;i<MT_RADIO_REPLY_BYTES;++i) out[i]=0;
    put32(out,MT_RADIO_REPLY_BYTES);put32(out+4,1);put32(out+8,(uint32_t)result);
    put32(out+12,(uint32_t)s->error);put32(out+16,(uint32_t)s->stop_error);
    put32(out+20,ready);put32(out+24,d->start_attempted);put32(out+28,s->stopped);
    put32(out+32,s->stage);put32(out+36,d->chip->published);put32(out+40,d->chip->quiescent);
    put32(out+44,s->channel.primary);put32(out+48,s->channel.center);put32(out+52,s->channel.width);
    put32(out+56,d->runtime->data_tx ? d->runtime->data_tx->inflight : 0);
    put32(out+60,s->nic.bands);put32(out+64,s->hardware.encapsulation);
    for (i=0;i<6;++i) out[68+i]=s->nic.mac[i];
    put32(out+76,(uint32_t)d->platform.error);put32(out+80,d->platform.dynamic.state);
    put32(out+84,d->platform.geographic.state);put32(out+88,token);put32(out+92,bytes);
}
int mt_radio_request(mt_radio_device *d,uint32_t op,const uint8_t *in,uint32_t size,
    uint8_t *out,uint32_t capacity,uint32_t *length) {
    mt_radio_config c;
    mt_fw_preflight preflight;
    uint32_t bytes=0,token=0xffffffffu,i;
    int result=MT_INVALID;
    if (length) *length=0;
    if (!d || !d->runtime || !d->chip || !out || !length || capacity<MT_RADIO_REPLY_BYTES ||
        (size && !in) || op<MT_RADIO_START || op>MT_RADIO_DIAGNOSTICS) return MT_INVALID;
    if (op==MT_RADIO_DIAGNOSTICS) {
        const mt_pcie_mcu *m=&d->runtime->mcu;
        if (size || capacity<MT_RADIO_DIAGNOSTIC_BYTES) return MT_INVALID;
        put32(out,MT_RADIO_DIAGNOSTIC_BYTES);put32(out+4,12);
        put32(out+8,m->last_command);put32(out+12,m->stage);
        put32(out+16,m->sequence);put32(out+20,m->response_count);
        put32(out+24,m->response_size);put32(out+28,(uint32_t)m->response_result);
        for (i=0;i<128;++i) out[32+i]=m->response_prefix[i];
        put32(out+160,m->first_size);put32(out+164,(uint32_t)m->first_result);
        for (i=0;i<128;++i) out[168+i]=m->first_prefix[i];
        put32(out+296,d->runtime->mac_step);put32(out+300,d->runtime->mac_index);
        put32(out+304,d->runtime->mac_reg);put32(out+308,d->runtime->mac_expected);
        put32(out+312,d->runtime->mac_observed);put32(out+316,d->runtime->mac_mask);
        put32(out+320,(uint32_t)d->runtime->mac_io_error);
        put32(out+324,d->runtime->event_path);put32(out+328,d->runtime->event_size);
        put32(out+332,(uint32_t)d->runtime->event_result);
        for(i=0;i<128;++i)out[336+i]=d->runtime->event_prefix[i];
        put32(out+464,(uint32_t)m->fault);put32(out+468,(uint32_t)d->runtime->rx.fault);
        put32(out+472,(uint32_t)d->runtime->data_rx.fault);
        put32(out+476,(uint32_t)(d->runtime->data_tx?d->runtime->data_tx->fault:0));
        put32(out+480,d->runtime->data_tx?d->runtime->data_tx->ring.head:0);
        put32(out+484,d->runtime->data_tx?d->runtime->data_tx->ring.tail:0);
        put32(out+488,d->runtime->data_tx?d->runtime->data_tx->ring.queued:0);
        put32(out+492,d->runtime->data_tx?d->runtime->data_tx->inflight:0);
        put32(out+496,d->runtime->tx_dma_index);put32(out+500,d->runtime->mcu_events);
        put32(out+504,d->runtime->data_events);put32(out+508,d->runtime->tx_free_events);
        for(i=0;i<4;++i)put32(out+512+i*4,d->runtime->data_tx?d->runtime->data_tx->status_count[i]:0);
        put32(out+528,d->runtime->data_tx?d->runtime->data_tx->status_attempts:0);
        for(i=0;i<MT_RX_OUTCOMES;++i) {
            put32(out+532+i*4,d->runtime->rx_stats.outcomes[i]);
            put32(out+568+i*4,d->runtime->rx_stats.to_local[i]);
        }
        for(i=0;i<6;++i)put32(out+604+i*4,d->runtime->rx_stats.local_error_bits[i]);
        for(i=0;i<8;++i)put32(out+628+i*4,d->runtime->rx_stats.first_local_error[i]);
        put32(out+660,d->allocation.stage);put32(out+664,d->allocation.bank_index);
        put32(out+668,d->allocation.request_bytes);put32(out+672,(uint32_t)d->allocation.status);
        put32(out+676,d->runtime->rx_stats.short_candidates);
        put32(out+680,d->runtime->rx_stats.short_malformed);
        put32(out+684,d->runtime->rx_stats.short_offset);
        put32(out+688,d->runtime->rx_stats.short_frame_bytes);
        for(i=0;i<3;++i)put32(out+692+i*4,d->runtime->rx_stats.short_rxd[i]);
        put32(out+704,d->runtime->rx_stats.short_header_bytes);
        for(i=0;i<16;++i)out[708+i]=d->runtime->rx_stats.short_header[i];
        for(i=0;i<2;++i) {
            const mt_radio_rx_snapshot *snapshot=&d->session.rx_snapshot[i];
            uint8_t *p=out+724+i*24;
            put32(p,snapshot->phase);put32(p+4,snapshot->valid_mask);
            put32(p+8,(uint32_t)snapshot->first_error);
            put32(p+12,snapshot->values[0]);put32(p+16,snapshot->values[1]);
            put32(p+20,snapshot->values[2]);
        }
        put32(out+772,d->session.txpower.attempted);put32(out+776,d->session.txpower.valid);
        put32(out+780,(uint32_t)d->session.txpower.error);put32(out+784,d->session.txpower.sequence);
        put32(out+788,d->session.txpower.reason);put32(out+792,d->session.txpower.response_size);
        for(i=0;i<MT_TXPOWER_RAW_CAPACITY;++i)out[796+i]=d->session.txpower.raw[i];
        for(i=1692;i<MT_RADIO_DIAGNOSTIC_BYTES;++i)out[i]=0;
        if (d->runtime->data_tx) {
            const mt_tx_status_diag *t=&d->runtime->data_tx->txs;
            put32(out+1692,t->requests);put32(out+1696,t->envelopes);
            put32(out+1700,t->records);put32(out+1704,t->matched);
            put32(out+1708,t->duplicates);put32(out+1712,t->unmatched);
            put32(out+1716,t->malformed);put32(out+1720,t->unsupported);
            for(i=0;i<MT_TXS_LIMIT;++i) {
                const mt_tx_status_sample *sample=&t->samples[i];
                uint8_t *p=out+1724+i*80u;
                uint32_t j;
                put32(p,sample->pid);put32(p+4,sample->fc);put32(p+8,sample->sequence);
                put32(p+12,sample->wcid);put32(p+16,sample->rate_table);
                for(j=0;j<6;++j)p[20+j]=sample->receiver[j];
                put32(p+28,sample->matched);
                for(j=0;j<MT_TXS_BYTES;++j)p[32+j]=sample->raw[j];
            }
            put32(out+2364,t->first_size);put32(out+2368,t->first_bytes);
            for(i=0;i<64;++i)out[2372+i]=t->first_raw[i];
        }
        *length=MT_RADIO_DIAGNOSTIC_BYTES;return MT_OK;
    }
    if (op==MT_RADIO_START) {
        if (size<56 || size>40u+16u+2u*MT_FW_MAX_SIZE || get32(in)!=1 ||
            get32(in+4)!=size-40 || get32(in+36)) return MT_INVALID;
        c.alpha2[0]=in[8];c.alpha2[1]=in[9];c.reserved[0]=in[10];c.reserved[1]=in[11];
        c.environment=get32(in+12);c.primary=get32(in+16);c.center=get32(in+20);
        c.width=get32(in+24);c.fcs=get32(in+28);c.cap_half_dbm=(int32_t)get32(in+32);
        if (!mt_radio_config_valid(&c) || mt_fw_preflight_bundle(in+40,size-40,&preflight)) return MT_INVALID;
        if (!d->start_attempted && !d->chip->attempted && d->read_platform && d->prepare_data &&
            d->scratch && d->scratch_capacity>=MT_DATA_RX_CAPACITY) {
            d->start_attempted=1;
            result=d->read_platform(d->context,&d->platform);
            if (!result) result=d->prepare_data(d->context);
            if (!result) result=mt_radio_start(&d->session,d->chip,d->runtime,in+40,size-40,
                &c,&d->platform,d->scratch,d->scratch_capacity);
            if (result && !d->session.error) d->session.error=result;
        }
    } else if (op==MT_RADIO_CHANNEL) {
        if (size!=16 || get32(in)!=1) return MT_INVALID;
        result=mt_radio_channel(&d->session,get32(in+4),get32(in+8),get32(in+12));
    } else if (op==MT_RADIO_ADDRESS) {
        if (size!=12 || get32(in)!=1 || in[10] || in[11]) return MT_INVALID;
        result=mt_radio_set_address(&d->session,in+4);
    } else if (op==MT_RADIO_TX) {
        if (size<16 || size>16+MT_TX_PAYLOAD_MAX || get32(in)!=1 || get32(in+12)!=size-16) return MT_INVALID;
        result=mt_radio_transmit(&d->session,in+16,size-16,get32(in+4),get32(in+8),&token);
    } else {
        if (size) return MT_INVALID;
        if (op==MT_RADIO_STATUS) result=MT_OK;
        if (op==MT_RADIO_STOP) result=mt_radio_device_close(d,1);
        if (op==MT_RADIO_POLL) {
            if (capacity<MT_RADIO_POLL_BYTES) return MT_INVALID;
            result=mt_radio_receive_record(&d->session,d->scratch,d->scratch_capacity,
                out+MT_RADIO_REPLY_BYTES,capacity-MT_RADIO_REPLY_BYTES,&bytes);
        }
    }
    /* Avoid exposing an old token if submission failed after publication. */
    if (result) token=0xffffffffu;
    reply(d,result,token,bytes,out);
    *length=MT_RADIO_REPLY_BYTES+bytes;
    /* Only initialized reply/record bytes are exposed, never unused capacity. */
    for (i=74;i<76;++i) out[i]=0;
    return MT_OK;
}
