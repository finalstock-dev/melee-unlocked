// Native practice matchmaking coordinator. Renderers exchange snapshots/commands with it; only
// tick() touches Slippi networking or guest scene state, and tick() runs on the simulation thread.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "native_practice_model.h"

#include <cstdint>
#include <string>

namespace slippi::native_practice {

// Values intentionally match Slippi's OnlinePlayMode for the two native-practice modes.
enum class MatchMode : uint8_t { None = 0xFF, Unranked = 1, Direct = 2 };

struct Snapshot {
  Phase phase = Phase::Idle;
  MatchMode mode = MatchMode::None;
  bool in_practice = false;
  bool tab_available = false;
  bool cosmetic_profile_locked = false;
  int controller_port = 0;
  int matchmaking_state = 0;
  uint32_t generation = 0;
  uint32_t search_ticks = 0;
  std::string connect_code;
  std::string status;
  std::string detail;
  std::string opponent;
};

Snapshot snapshot();
void submit_start_unranked();
void submit_start_direct(const std::string& connect_code);
void submit_cancel();
void submit_acknowledge_failure();

// Called once per 60 Hz retrace, before the guest advances its frame.
void tick();
void shutdown();

// Narrow integration hook for cosmetic work: do not change the active cosmetic profile from the
// start of a search through online cleanup. No cosmetic implementation is required by this module.
bool cosmetic_profile_locked();
const char* match_mode_name(MatchMode mode);

}  // namespace slippi::native_practice
