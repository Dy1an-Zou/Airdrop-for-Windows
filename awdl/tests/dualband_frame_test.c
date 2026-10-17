/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise the actual serializer, slot-policy wrapper and send fixup against
 * pre-change complete-frame fixtures. Only the serializer clock is fixed. */
#include <stdio.h>
#include <string.h>
#define clock_time_us dualband_test_clock_us
#include "../../upstream/owl/src/tx.c"
#undef clock_time_us
#include "slot_policy.h"
#include "frame_fixup.h"
#include "dualband_baseline.h"

uint64_t dualband_test_clock_us(void) { return 1000000; }
int __wrap_log_log(int level, const char *file, int line, const char *fmt, ...) {
    (void)level; (void)file; (void)line; (void)fmt; return 0;
}
static int checks, failures;
static void expect(int yes, const char *why) {
    ++checks;
    if (!yes) { ++failures; printf("FAIL %s\n", why); }
}
static int find_tlv(const uint8_t *frame, int len, int kind, int *count) {
    int pos, found = -1;
    *count = 0;
    if (len < 4) return -1;
    pos = frame[2] + 256 * frame[3] + 40;
    if (pos > len) return -1;
    while (pos + 3 <= len) {
        int bytes = frame[pos + 1] + 256 * frame[pos + 2];
        if (bytes > len - pos - 3) return -1;
        if (frame[pos] == kind) { found = pos; ++*count; }
        pos += 3 + bytes;
    }
    return pos == len ? found : -1;
}
static void compare_baseline(const uint8_t *frame, int len,
                             const uint8_t *baseline, size_t bytes) {
    int count, data = find_tlv(baseline, (int)bytes, AWDL_DATA_PATH_STATE_TLV, &count);
    int valid = len == (int)bytes && count == 1 && data >= 0 && data + 5 <= len;
    expect(valid, "complete frame length and Data Path TLV match baseline");
    if (!valid) return;
    size_t flag = (size_t)data + 3;
    expect(baseline[flag] == 0x24 && baseline[flag + 1] == 0x8f &&
           frame[flag] == 0x04 && frame[flag + 1] == 0x8f,
           "Data Path flags change only from 0x8f24 to 0x8f04");
    int differences = 0, elsewhere = 0;
    for (size_t i = 0; i < bytes; ++i) {
        if (frame[i] != baseline[i]) {
            ++differences;
            if (i != flag) ++elsewhere;
        }
    }
    expect(differences == 1 && elsewhere == 0 &&
           (frame[flag] ^ baseline[flag]) == 0x20,
           "entire actual frame differs only in the Dualband bit");
}
static void malformed_unchanged(const uint8_t *frame, int len) {
    uint8_t bad[2048], before[2048];
    int first = frame[2] + 256 * frame[3] + 40;
    memcpy(bad, frame, (size_t)len);
    bad[first + 1] = 0xff;
    bad[first + 2] = 0x7f;
    memcpy(before, bad, (size_t)len);
    expect(frame_fixup_received(bad, len) == len &&
           !memcmp(bad, before, (size_t)len),
           "oversized TLV keeps existing receive-fixup pass-through behavior");
    expect(frame_fixup_sent(bad, len, sizeof(bad)) == len &&
           !memcmp(bad, before, (size_t)len),
           "oversized TLV keeps existing send-fixup pass-through behavior");
    memcpy(bad, frame, 12);
    memcpy(before, bad, 12);
    expect(frame_fixup_received(bad, 12) == 12 &&
           frame_fixup_sent(bad, 12, sizeof(bad)) == 12 && !memcmp(bad, before, 12),
           "short frames keep existing fixup pass-through behavior");
}
static void run_case(enum awdl_action_type type) {
    const struct ether_addr self = {{2,1,2,3,4,5}};
    struct awdl_state state = {0};
    struct ieee80211_state wifi = {0};
    uint8_t frame[2048] = {0};
    awdl_init_state(&state, "Xiatiao Windows", &self, CHAN_OPCLASS_6, 1000000);
    state.channel.current = CHAN_OPCLASS_6;
    wifi.sequence_number = 77;
    int len = winawdl_init_full_action_frame(frame, &state, &wifi, type);
    int valid = len > 49 && len < (int)sizeof(frame);
    expect(valid, "actual complete action builder succeeds");
    expect(wifi.sequence_number == 78, "serializer consumes exactly one Wi-Fi sequence");
    if (!valid) { awdl_peers_free(state.peers.peers); return; }
    int count;
    find_tlv(frame, len, 7, &count);
    expect(count == (type == AWDL_ACTION_MIF), "HT stays MIF-only; r31 PSF change is absent");
    find_tlv(frame, len, AWDL_ARPA_TLV, &count);
    expect(count == (type == AWDL_ACTION_MIF), "ARPA stays MIF-only");
    if (type == AWDL_ACTION_MIF)
        compare_baseline(frame, len, legacy_raw_3, sizeof(legacy_raw_3));
    else
        compare_baseline(frame, len, legacy_raw_0, sizeof(legacy_raw_0));
    frame_fixup_announce(0, 0, 20);
    int fixed = frame_fixup_sent(frame, len, sizeof(frame));
    expect(fixed == len, "20-MHz send fixup preserves frame length");
    if (type == AWDL_ACTION_MIF)
        compare_baseline(frame, fixed, legacy_sent_3, sizeof(legacy_sent_3));
    else
        compare_baseline(frame, fixed, legacy_sent_0, sizeof(legacy_sent_0));
    malformed_unchanged(frame, fixed);
    awdl_peers_free(state.peers.peers);
}
int main(void) {
    run_case(AWDL_ACTION_PSF);
    run_case(AWDL_ACTION_MIF);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
