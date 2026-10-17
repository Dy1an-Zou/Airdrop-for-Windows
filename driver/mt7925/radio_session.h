/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_RADIO_SESSION_H
#define MT7925_RADIO_SESSION_H
#include "chip_control.h"
#include "channel_control.h"
#include "clc_power.h"
#include "platform_acpi.h"

typedef struct mt_radio_config {
    uint8_t alpha2[2], reserved[2];
    uint32_t environment, primary, center, width, fcs;
    int32_t cap_half_dbm;
} mt_radio_config;
MT_API int mt_radio_config_valid(const mt_radio_config *);
/* Fixed read-only BAR snapshot: phase0=not attempted, phase1=after the initial
 * monitor/filter commands, phase2=after the last successful OMAC update.
 * valid_mask bits0..2 correspond to RFCR0(0x21400), RFCR1(0x21404),
 * MDP_DCR0(0xe800). Invalid values are zero; first_error is the first failed
 * read, including an all-ones result. Observation errors never change RF state.
 */
typedef struct mt_radio_rx_snapshot {
    uint32_t phase, valid_mask;
    int32_t first_error;
    uint32_t values[3];
} mt_radio_rx_snapshot;
typedef struct mt_radio_session {
    mt_mcu_runtime *runtime;
    mt_chip *chip;
    mt_channel_control channel;
    mt_regdomain domain;
    mt_nic_caps nic;
    mt_hw_type hardware;
    mt_clc_result clc;
    mt_tx_policy power;
    mt_tx_init_result tx;
    mt_fw_result firmware;
    uint32_t attempted, ready, stopped, stage, fcs;
    int32_t error, stop_error;
    mt_radio_rx_snapshot rx_snapshot[2];
    mt_txpower_result txpower;
} mt_radio_session;
/* Caller-owned zeroed, stable, nonpaged session. Serialized at passive level
 * with ALL runtime/chip/MCU access. Chip is the platform's lifecycle object,
 * not a private copy, so D0Exit/removal observes these published allocations.
 * Runtime has MCU, data RX and TX prepared but not started. Immutable trusted
 * bundle/config/platform and disjoint scratch >= MT_CLC_SCRATCH for this call.
 * One attempt per generation. Success leaves firmware/DMA running; every
 * post-start failure revokes I/O and attempts stop, retaining both errors.
 * No allocations are freed here, including after failed stop.
 */
MT_API int mt_radio_start(mt_radio_session *,mt_chip *,mt_mcu_runtime *,
    const uint8_t *,uint32_t,const mt_radio_config *,const mt_platform *,
    uint8_t *,uint32_t);
/* Stop is idempotent; failure is terminal until platform power removal. */
MT_API int mt_radio_stop(mt_radio_session *);
/* Service idle MCU events/TX completion without waiting for a command reply.
 * Bounded budget 1..64. Caller repeats/polls; no background thread is implied.
 * Does not consume data RX (and therefore never drops a pending AWDL frame).
 */
MT_API int mt_radio_service(mt_radio_session *,uint32_t budget,uint32_t *consumed);
/* A switch with outstanding TX returns PENDING, keeps old channel, sends no
 * channel command. Caller drains BOTH queues then retries. Invalid geometry
 * leaves the previous session usable; partial command failure stops it.
 */
MT_API int mt_radio_channel(mt_radio_session *,uint32_t,uint32_t,uint32_t);
/* Update OMAC0 through the fixed DEV_INFO_ACTIVE command. Wait for in-flight
 * TX to drain before changing source identity; no BSS/channel recreation.
 * Success is firmware command acknowledgement, not observed receiver ACK.
 * NIC capability MAC remains unchanged. Partial/failed update stops session.
 */
MT_API int mt_radio_set_address(mt_radio_session *,const uint8_t mac[6]);
/* Full-sized buffers required before consuming any frame. Invalid on-air
 * packets may return INVALID without poisoning a healthy DMA transport.
 */
MT_API int mt_radio_receive_record(mt_radio_session *,uint8_t *,uint32_t,
    uint8_t *,uint32_t,uint32_t *);
/* WCID fixed to initialized monitor BSS WCID19. Policy checked on every submit;
 * token acceptance is not an over-air ACK or peer receipt. */
MT_API int mt_radio_transmit(mt_radio_session *,const uint8_t *,uint32_t,
    uint32_t rate,uint32_t no_ack,uint32_t *token);
#endif
