/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "schedule.h"

int winawdl_is_multicast_eaw(const struct awdl_state *, uint64_t);
int __wrap_log_log(int level, const char *file, int line, const char *fmt, ...) {
    (void)level; (void)file; (void)line; (void)fmt;
    return 0;
}
static int failures, checks;
static void expect(int ok, const char *why) {
    ++checks;
    if (!ok) { ++failures; printf("FAIL %s\n", why); }
}
int main(void) {
    struct awdl_state state;
    memset(&state, 0, sizeof(state));
    const uint64_t origin = 1000000, now = origin + 32 * 1024;
    awdl_sync_state_init(&state.sync, origin);
    state.channel.enc = AWDL_CHAN_ENC_OPCLASS;
    state.channel.master = CHAN_OPCLASS_6;
    awdl_chanseq_init_static(state.channel.sequence, &CHAN_OPCLASS_6);
    awdl_peer_state_init(&state.peers);
    state.sync.aw_counter = 0;
    expect(winawdl_is_multicast_eaw(&state, now), "no peer retains discovery fallback");
    const struct ether_addr addr = {{2, 3, 4, 5, 6, 7}};
    awdl_peer_add(state.peers.peers, &addr, origin, NULL, NULL);
    struct awdl_peer *peer = NULL;
    awdl_peer_get(state.peers.peers, &addr, &peer);
    peer->is_valid = 1;
    // Channel sequence captured from a real peer in transfer-r3/r4.
    const uint8_t sequence[16] = {36,0,149,0,0,0,0,0,6,0,149,0,0,0,0,0};
    for (int i = 0; i < 16; ++i) peer->sequence[i].opclass.chan_num = sequence[i];
    expect(!winawdl_is_multicast_eaw(&state, now), "slot0 peer on36 is not reachable on6");
    state.sync.aw_counter = 40;
    expect(!winawdl_is_multicast_eaw(&state, now), "slot10 peer on149 is not reachable on6");
    state.sync.aw_counter = 32;
    expect(winawdl_is_multicast_eaw(&state, now), "slot8 shares advertised channel6");
    expect(awdl_can_send_in(&state, now, AWDL_MULTICAST_GUARD_TU) == 0,
           "shared-window middle passes unchanged core guard");
    expect(awdl_can_send_in(&state, origin + 1024, AWDL_MULTICAST_GUARD_TU) > 0,
           "shared-window leading edge still requires guard");
    peer->is_valid = 0;
    expect(!winawdl_is_multicast_eaw(&state, now), "incomplete peer cannot enable extra slot");
    peer->is_valid = 1;
    state.peers.timeout = 1;
    expect(!winawdl_is_multicast_eaw(&state, now), "stale peer cannot enable extra slot");
    state.peers.timeout = 2000000;
    awdl_chanseq_init_static(state.channel.sequence, &CHAN_OPCLASS_44);
    state.channel.master = CHAN_OPCLASS_44;
    expect(!winawdl_is_multicast_eaw(&state, now), "other configurations retain upstream policy");
    state.sync.aw_counter = 40;
    expect(winawdl_is_multicast_eaw(&state, now), "legacy slot10 remains for other configurations");
    awdl_peers_free(state.peers.peers);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
