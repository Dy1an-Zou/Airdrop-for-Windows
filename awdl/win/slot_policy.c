/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "schedule.h"
#include "slot_policy.h"

/* A channel-6 social visit may last only the first 16-TU AW of a 64-TU
 * slot. The legacy multicast guard excludes that whole visit. Keep the
 * shorter 3-TU guard on this fixed radio, including its trailing retry;
 * other channel configurations retain OWL's original multicast timing. */
int winawdl_multicast_guard(const struct awdl_state *state) {
    return state->channel.enc == AWDL_CHAN_ENC_OPCLASS &&
           awdl_chan_num(state->channel.master, AWDL_CHAN_ENC_OPCLASS) == 6
               ? AWDL_UNICAST_GUARD_TU : AWDL_MULTICAST_GUARD_TU;
}

/* This backend remains on channel 6. Advertise its own availability in
 * every slot; intersecting with a peer can erase slot 0, which Apple's
 * channel-update parser rejects. Peer overlap still gates actual TX below. */
static void advertise_fixed_channel(struct awdl_state *state) {
    if (state->channel.enc == AWDL_CHAN_ENC_OPCLASS &&
        awdl_chan_num(state->channel.master, AWDL_CHAN_ENC_OPCLASS) == 6)
        awdl_chanseq_init_static(state->channel.sequence, &state->channel.master);
}

int winawdl_init_full_action_frame(uint8_t *buf, struct awdl_state *state,
                                 struct ieee80211_state *wifi, enum awdl_action_type type) {
    advertise_fixed_channel(state);
    return awdl_init_full_action_frame(buf, state, wifi, type);
}

/* A fixed channel-6 backend cannot reach a peer's primary-channel multicast
 * slots. R3/R4 peer captures advertise channel 6 only in slot 8, while OWL's
 * legacy multicast predicate permits only slots 0/10. Use an overlapping
 * advertised availability window; core.c applies the selected guard.
 * This is an interoperability candidate, not proof that Apple accepts it. */
int winawdl_is_multicast_eaw(const struct awdl_state *state, uint64_t now) {
    if (state->channel.enc != AWDL_CHAN_ENC_OPCLASS ||
        awdl_chan_num(state->channel.master, AWDL_CHAN_ENC_OPCLASS) != 6)
        return awdl_is_multicast_eaw(state, now);

    int known = 0, overlaps = 0;
    struct awdl_peer *peer;
    awdl_peers_it_t it = awdl_peers_it_new(state->peers.peers);
    if (!it)
        return 0;
    while (awdl_peers_it_next(it, &peer) == PEERS_OK) {
        if (!peer->is_valid || now < peer->last_update ||
            now - peer->last_update >= state->peers.timeout)
            continue;
        known = 1;
        if (awdl_same_channel_as_peer(state, now, peer))
            overlaps = 1;
    }
    awdl_peers_it_free(it);
    return known ? overlaps : awdl_is_multicast_eaw(state, now);
}
