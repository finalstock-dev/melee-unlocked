// Pure state/validation logic for the native practice matchmaking coordinator.
// Kept independent of the Windows renderer and Slippi sockets so it can be unit tested anywhere.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace slippi::native_practice {

enum class Phase : uint8_t {
  Idle,
  Searching,
  Handoff,
  OnlineFlow,
  InMatch,
  Failure,
  ReturningToPractice,
};

struct Decision {
  bool cleanup_connection = false;
  bool request_online_handoff = false;
  bool request_practice_return = false;
};

// Owns only lifecycle decisions. Socket work and guest-memory scene changes are performed by the
// simulation-thread coordinator after it receives a Decision.
class Lifecycle {
 public:
  Phase phase() const { return phase_; }
  bool left_practice() const { return left_practice_; }
  bool match_started() const { return match_started_; }
  const std::string& error() const { return error_; }

  bool begin_search();
  Decision cancel();
  Decision matchmaking_result(bool connection_success, bool local_ready, bool remote_ready,
                              const std::string& error);
  Decision observe_session(bool online_scene, bool session_active, bool online_match);
  Decision fail(const std::string& error, bool cleanup_connection);
  Decision acknowledge_failure();
  void practice_restored();
  void reset();

 private:
  Phase phase_ = Phase::Idle;
  bool left_practice_ = false;
  bool match_started_ = false;
  std::string error_;
};

// Slippi connect codes are ASCII NAME#digits. Whitespace from a copied code is ignored and the
// player name is upper-cased. Returns false with a user-facing reason when the input is invalid.
bool normalize_direct_code(const std::string& input, std::string* normalized, std::string* error);
// Formats a 60 Hz search-tick count for the render snapshot without using wall-clock time.
std::string format_search_duration(uint32_t ticks);
// Only transition/failure phases that must suppress guest input force global capture. Returning to
// Training lands on its character-select scene, where the player must be able to continue.
bool phase_forces_input_capture(Phase phase);
// The return notice is useful only while Melee is leaving the online scene. Once Training's
// character select is active it must disappear, even though field restoration remains pending
// until the Training match minor settles.
bool phase_shows_return_overlay(Phase phase, bool in_practice);
// The guest's online-mode byte normally belongs to the online menu. A native-practice handoff
// enters that major scene without passing through the title-menu option that initializes it, so
// the coordinator owns the byte only until the online scene has taken over.
bool phase_owns_online_mode(Phase phase);
const char* phase_name(Phase phase);

}  // namespace slippi::native_practice
