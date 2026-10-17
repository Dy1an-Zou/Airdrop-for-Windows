/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "tx.h"

int winawdl_init_full_action_frame(uint8_t *, struct awdl_state *,
                                 struct ieee80211_state *, enum awdl_action_type);
int winawdl_multicast_guard(const struct awdl_state *);
