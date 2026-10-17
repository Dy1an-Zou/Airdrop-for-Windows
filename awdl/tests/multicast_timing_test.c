/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Run the actual OWL event callback, queue and serializer with deterministic
 * time and a recording I/O boundary. No radio, adapter or event loop opens. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define clock_time_us timing_clock_us
#define awdl_is_multicast_eaw winawdl_is_multicast_eaw
#define awdl_init_full_action_frame winawdl_init_full_action_frame
#include "../../upstream/owl/daemon/core.c"
#undef clock_time_us

static uint64_t test_now;
static int checks, failures, sent;
uint64_t timing_clock_us(void) { return test_now; }
int __wrap_log_log(int level, const char *file, int line, const char *fmt, ...) {
    (void)level; (void)file; (void)line; (void)fmt; return 0;
}
static void expect(int ok, const char *why) {
    ++checks; if (!ok) { ++failures; printf("FAIL %s\n", why); }
}
int wlan_send(const struct io_state *io, const uint8_t *buf, int len) {
    (void)io;
    /* Check that we really traversed the data serializer to the I/O edge. */
    expect(len >= 49 && buf[2] < len && buf[buf[2]] == 8,
           "real callback submits an 802.11 data frame");
    ++sent; return len;
}
int host_send(const struct io_state *io, const uint8_t *buf, int len) {
    (void)io; (void)buf; (void)len; return 0;
}
int host_recv(const struct io_state *io, uint8_t *buf, int *len) {
    (void)io; (void)buf; (void)len; return -EWOULDBLOCK;
}
int io_state_init(struct io_state *io, const char *wlan, const char *host,
                  const struct ether_addr *bssid) {
    (void)io; (void)wlan; (void)host; (void)bssid; return -1;
}
void io_state_free(struct io_state *io) { (void)io; }
int pcap_dispatch(pcap_t *p, int n, pcap_handler cb, unsigned char *user) {
    (void)p; (void)n; (void)cb; (void)user; return 0;
}
int netutils_init(void) { return -1; }
void netutils_cleanup(void) {}
int get_hostname(char *name, size_t len) { (void)name; (void)len; return -1; }
int neighbor_add_rfc4291(int index, const struct ether_addr *addr) {
    (void)index; (void)addr; return 0;
}
int neighbor_remove_rfc4291(int index, const struct ether_addr *addr) {
    (void)index; (void)addr; return 0;
}
int is_channel_available(int index, int channel, bool *available) {
    (void)index; (void)channel; *available = false; return -1;
}
int set_channel(int index, int channel) { (void)index; (void)channel; return -1; }
void ev_timer_start(struct ev_loop *loop, ev_timer *timer) { (void)loop; timer->active = 1; }
void ev_timer_stop(struct ev_loop *loop, ev_timer *timer) { (void)loop; timer->active = 0; }
void ev_timer_again(struct ev_loop *loop, ev_timer *timer) { (void)loop; (void)timer; }
void ev_feed_event(struct ev_loop *loop, void *watcher, int events) {
    (void)loop; (void)watcher; (void)events;
}
void ev_io_start(struct ev_loop *loop, ev_io *io) { (void)loop; (void)io; }
void ev_signal_start(struct ev_loop *loop, ev_signal *signal) { (void)loop; (void)signal; }

static void run_case(struct awdl_chan channel, unsigned slot, unsigned phase_us,
                     int should_send, double expected_wait, const char *why) {
    const uint64_t origin = 1000000;
    const struct ether_addr self = {{2,1,2,3,4,5}}, other = {{2,6,7,8,9,10}};
    struct daemon_state s = {0};
    test_now = origin + phase_us;
    awdl_init_state(&s.awdl_state, "timing-test", &self, channel, origin);
    s.awdl_state.sync.aw_counter = (uint16_t)(slot * 4);
    s.awdl_state.channel.current = channel;
    awdl_peer_add(s.awdl_state.peers.peers, &other, origin, NULL, NULL);
    struct awdl_peer *p = NULL;
    awdl_peer_get(s.awdl_state.peers.peers, &other, &p);
    p->is_valid = p->has_sequence = 1;
    /* Exact channel sequence seen in the sealed r13 capture. */
    const uint8_t channels[16] = {36,0,149,0,0,0,0,0,6,0,149,0,0,0,0,0};
    for (int i=0;i<16;++i) p->sequence[i].opclass.chan_num = channels[i];
    if (channel.opclass.chan_num == 6) {
        memset(s.awdl_state.channel.sequence, 0, sizeof(s.awdl_state.channel.sequence));
        s.awdl_state.channel.sequence[8] = CHAN_OPCLASS_6;
    }
    s.tx_queue_multicast = circular_buf_init(2);
    struct buf *frame = buf_new_owned(54);
    uint8_t packet[54] = {0x33,0x33,0,0,0,0xfb,2,1,2,3,4,5,0x86,0xdd,0x60};
    write_bytes(frame, 0, packet, sizeof(packet));
    circular_buf_put(s.tx_queue_multicast, frame);
    ev_timer timer = {0}; timer.data = &s;
    sent = 0;
    awdl_send_multicast(NULL, &timer, 0);
    expect(sent == should_send, why);
    expect(circular_buf_empty(s.tx_queue_multicast) == should_send,
           "queue changes only when the real scheduling callback sends");
    if (!should_send && expected_wait >= 0) {
        double error = timer.after - expected_wait;
        expect(timer.active && error > -0.0000001 && error < 0.0000001,
               "real timer rearm uses the selected guard");
    }
    void *pending;
    if (!circular_buf_get(s.tx_queue_multicast, &pending, 0)) buf_free(pending);
    circular_buf_free(s.tx_queue_multicast);
    awdl_peers_free(s.awdl_state.peers.peers);
}
int main(void) {
    run_case(CHAN_OPCLASS_6,8,0,0,3*0.001024,"channel6 leading edge waits three TU");
    run_case(CHAN_OPCLASS_6,8,2*1024,0,0.001024,"channel6 retains three-TU leading guard");
    run_case(CHAN_OPCLASS_6,8,3*1024,1,-1,"channel6 opens at three TU");
    run_case(CHAN_OPCLASS_6,8,8*1024,1,-1,"channel6 can announce during captured first AW");
    run_case(CHAN_OPCLASS_6,8,15*1024,1,-1,"channel6 does not exclude all of first AW");
    /* Six r13 Apple data submissions, placed relative to the iOS peer's
     * preceding sync TLV. Capture SHA256 4b7b5603d9572739e0dda3736f88c5022
     * 47f3b695362944a53a7dcea8603c06f. These are host-time estimates, not
     * calibrated RF times; all nevertheless lie inside the excluded AW. */
    const unsigned captured_phase_us[] = {13931,12583,12609,6019,7579,7688};
    for (unsigned i=0;i<sizeof(captured_phase_us)/sizeof(captured_phase_us[0]);++i)
        run_case(CHAN_OPCLASS_6,8,captured_phase_us[i],1,-1,
                 "real callback admits the captured first-AW phase");
    run_case(CHAN_OPCLASS_6,8,32*1024,1,-1,"channel6 still sends inside old window");
    run_case(CHAN_OPCLASS_6,8,63*1024,0,5*0.001024,"channel6 trailing guard retains queued frame");
    run_case(CHAN_OPCLASS_6,0,8*1024,0,64*0.001024,"channel6 must still overlap a peer slot");
    run_case(CHAN_OPCLASS_44,0,8*1024,0,8*0.001024,"channel44 keeps original sixteen-TU guard");
    run_case(CHAN_OPCLASS_44,0,16*1024,1,-1,"channel44 original window still opens");
    run_case(CHAN_OPCLASS_149,10,8*1024,0,8*0.001024,"channel149 keeps original sixteen-TU guard");
    run_case(CHAN_OPCLASS_149,10,63*1024,0,31*0.001024,"other channel trailing retry unchanged");
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
