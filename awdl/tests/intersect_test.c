/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include "slot_policy.h"
#include "schedule.h"
#include "rx.h"
#include "wire.h"

int winawdl_is_multicast_eaw(const struct awdl_state *, uint64_t);
int __wrap_log_log(int level, const char *file, int line, const char *fmt, ...) {
    (void)level; (void)file; (void)line; (void)fmt; return 0;
}
static int checks, failures;
static void expect(int yes, const char *why) {
    ++checks; if (!yes) { ++failures; printf("FAIL %s\n", why); }
}
static void emit(struct awdl_state *s, uint8_t *out) {
    const enum awdl_action_type kinds[] = {AWDL_ACTION_PSF, AWDL_ACTION_MIF};
    for (int k=0;k<2;++k) {
        struct ieee80211_state wifi = {0};
        int len=winawdl_init_full_action_frame(out,s,&wifi,kinds[k]);
        expect(len>0,"real PSF/MIF serializer succeeds");
        int pos=out[2]+256*out[3]+24+sizeof(struct awdl_action);
        int explicit_found=0,sync_found=0;
        while (pos+3<=len) {
            int n=out[pos+1]+256*out[pos+2];
            if (pos+3+n>len) break;
            int seq_pos=-1;
            if(out[pos]==AWDL_CHAN_SEQ_TLV) {seq_pos=pos+3; ++explicit_found;}
            if(out[pos]==AWDL_SYNCHRONIZATON_PARAMETERS_TLV) {
                seq_pos=pos+sizeof(struct awdl_sync_params_tlv); ++sync_found;
            }
            if(seq_pos>=0) {
                int valid=seq_pos+38<=pos+3+n;
                expect(valid,"wire channel sequence fits owning TLV");
                if(valid && s->channel.master.opclass.chan_num==6) {
                    expect(out[seq_pos]==15 && out[seq_pos+1]==3 && out[seq_pos+3]==3,
                           "wire sequence retains 16 OPCLASS slots and original phase period");
                    for(int i=0;i<16;++i)
                        expect(out[seq_pos+6+2*i]==6 && out[seq_pos+7+2*i]==81,
                               "actual PSF/MIF Sync and ChannelSequence advertise local fixed6 availability");
                }
            }
            pos+=3+n;
        }
        expect(pos==len && explicit_found==1 && sync_found==1,
               "complete transmitted PSF/MIF contains exactly one Sync and ChannelSequence TLV");
    }
}
int main(void) {
    struct awdl_state state;
    struct ether_addr self = {{2,1,2,3,4,5}}, other = {{2,6,7,8,9,10}};
    uint64_t now = clock_time_us();
    awdl_init_state(&state, "test", &self, CHAN_OPCLASS_6, now);
    awdl_peer_add(state.peers.peers, &other, now, NULL, NULL);
    struct awdl_peer *p = NULL;
    awdl_peer_get(state.peers.peers, &other, &p);
    p->is_valid = 1;
    state.election.sync_addr = other;
    uint8_t seq[38] = {15, AWDL_CHAN_ENC_OPCLASS, 0, 3, 255, 255};
    const uint8_t channels[16] = {36,0,149,0,0,0,0,0,6,0,149,0,0,0,0,0};
    for (int i=0; i<16; ++i) { seq[6+2*i]=channels[i]; seq[7+2*i]=channels[i]?0x51:0; }
    const struct buf *v=buf_new_const(seq, sizeof(seq));
    expect(awdl_handle_chanseq_tlv(p,v,&state)==RX_OK,"real RX channel parser accepts captured sequence");
    buf_free(v);
    uint8_t out[4096], encoded[64];
    emit(&state,out);
    expect(awdl_init_chanseq(encoded,&state)==38,"serialized sequence length remains 38");
    for (int i=0;i<16;++i)
        expect(encoded[6+2*i]==6 && encoded[7+2*i]==0x51,
               "fixed radio advertises channel6 in every slot, including primary slot0");
    expect(state.channel.master.opclass.chan_num==6,"radio master remains channel6");
    state.sync.last_update=now; state.sync.aw_counter=32;
    expect(winawdl_is_multicast_eaw(&state,now+32*1024),"intersection retains slot8 multicast");
    state.sync.aw_counter=0;
    expect(!winawdl_is_multicast_eaw(&state,now+32*1024),"intersection cannot send slot0 multicast");
    /* Expired or unelected sequences may not keep shaping our advertisement. */
    p->last_update=0;
    emit(&state,out);
    expect(state.channel.sequence[0].opclass.chan_num==6,"expired master restores fixed discovery sequence");
    p->last_update=clock_time_us(); p->is_valid=0;
    emit(&state,out);
    expect(state.channel.sequence[8].opclass.chan_num==6 && state.channel.sequence[0].opclass.chan_num==6,"invalid peer excluded");
    p->is_valid=1; state.election.sync_addr=self;
    emit(&state,out);
    expect(state.channel.sequence[0].opclass.chan_num==6,"non-master excluded");
    state.election.sync_addr=other;
    state.channel.master=CHAN_OPCLASS_44;
    awdl_chanseq_init_static(state.channel.sequence,&CHAN_OPCLASS_44);
    emit(&state,out);
    expect(state.channel.sequence[0].opclass.chan_num==44,"other radio configurations unchanged");
    state.channel.master=CHAN_OPCLASS_6;
    awdl_chanseq_init_static(state.channel.sequence,&CHAN_OPCLASS_6);
    seq[22]=149; /* no channel6 overlap */
    v=buf_new_const(seq,sizeof(seq));
    expect(awdl_handle_chanseq_tlv(p,v,&state)==RX_OK,"zero-overlap RX parsed");buf_free(v);
    emit(&state,out);
    expect(state.channel.sequence[0].opclass.chan_num==6,"no overlap keeps existing advertisement");
    /* Failed parse must not replace a previously accepted sequence. */
    seq[22]=6; v=buf_new_const(seq,sizeof(seq));
    expect(awdl_handle_chanseq_tlv(p,v,&state)==RX_OK,"restore valid peer sequence");buf_free(v);
    v=buf_new_const(seq,37);
    expect(awdl_handle_chanseq_tlv(p,v,&state)==RX_TOO_SHORT,"truncated sequence rejected");buf_free(v);
    emit(&state,out);
    expect(state.channel.sequence[0].opclass.chan_num==6 && state.channel.sequence[8].opclass.chan_num==6,"truncation preserves peer input without changing local fixed-channel availability");
    /* Legacy bytes are not interpreted as OPCLASS by this bounded candidate. */
    seq[1]=AWDL_CHAN_ENC_LEGACY;
    v=buf_new_const(seq,sizeof(seq));
    expect(awdl_handle_chanseq_tlv(p,v,&state)==RX_OK,"legacy parser result retained");buf_free(v);
    emit(&state,out);
    expect(state.channel.sequence[0].opclass.chan_num==6,"unsupported peer encoding restores discovery fallback");
    state.sync.last_update=now; state.sync.aw_counter=32;
    expect(!awdl_same_channel_as_peer(&state,now+32*1024,p),"peer LEGACY bytes decoded using peer encoding");
    awdl_peers_free(state.peers.peers);
    printf("%d checks, %d failures\n", checks, failures);
    return failures?1:0;
}
