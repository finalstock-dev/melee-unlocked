// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_practice_model.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace slippi::native_practice {

bool Lifecycle::begin_search() {
  if (phase_ != Phase::Idle) return false;
  phase_ = Phase::Searching;
  left_practice_ = false;
  match_started_ = false;
  error_.clear();
  return true;
}

Decision Lifecycle::cancel() {
  Decision out;
  if (phase_ != Phase::Searching) return out;
  out.cleanup_connection = true;
  reset();
  return out;
}

Decision Lifecycle::matchmaking_result(bool connection_success, bool local_ready, bool remote_ready,
                                       const std::string& error) {
  if (phase_ != Phase::Searching) return {};
  if (!error.empty()) return fail(error, true);
  if (!connection_success || !local_ready || !remote_ready) return {};

  phase_ = Phase::Handoff;
  left_practice_ = true;
  Decision out;
  out.request_online_handoff = true;
  return out;
}

Decision Lifecycle::observe_session(bool online_scene, bool session_active, bool online_match) {
  if (phase_ == Phase::ReturningToPractice) return {};
  if (phase_ != Phase::Searching && phase_ != Phase::Handoff && phase_ != Phase::OnlineFlow && phase_ != Phase::InMatch)
    return {};

  if (online_match) {
    match_started_ = true;
    phase_ = Phase::InMatch;
    return {};
  }
  if (online_scene && (phase_ == Phase::Handoff || phase_ == Phase::OnlineFlow)) phase_ = Phase::OnlineFlow;
  if (session_active) return {};

  // Losing the session before frame 1 is a pre-match failure. Once gameplay has started, normal
  // Slippi reporting/cleanup owns the exit and the practice coordinator simply becomes idle.
  if (match_started_) {
    reset();
    return {};
  }
  return fail("The connection ended before the match started", true);
}

Decision Lifecycle::fail(const std::string& error, bool cleanup_connection) {
  if (phase_ == Phase::InMatch) return {};
  phase_ = Phase::Failure;
  error_ = error.empty() ? "Unable to connect" : error;
  Decision out;
  out.cleanup_connection = cleanup_connection;
  return out;
}

Decision Lifecycle::acknowledge_failure() {
  if (phase_ != Phase::Failure) return {};
  error_.clear();
  if (left_practice_) {
    phase_ = Phase::ReturningToPractice;
    Decision out;
    out.request_practice_return = true;
    return out;
  }
  reset();
  return {};
}

void Lifecycle::practice_restored() {
  if (phase_ == Phase::ReturningToPractice) reset();
}

void Lifecycle::reset() {
  phase_ = Phase::Idle;
  left_practice_ = false;
  match_started_ = false;
  error_.clear();
}

bool normalize_direct_code(const std::string& input, std::string* normalized, std::string* error) {
  std::string out;
  out.reserve(input.size());
  for (unsigned char ch : input) {
    if (std::isspace(ch)) continue;
    if (ch >= 'a' && ch <= 'z') ch = (unsigned char)std::toupper(ch);
    if (!(std::isalnum(ch) || ch == '#')) {
      if (error) *error = "Use letters, numbers, and one #";
      return false;
    }
    out.push_back((char)ch);
  }

  const size_t hash = out.find('#');
  if (hash == std::string::npos || hash == 0 || hash + 1 == out.size() || out.find('#', hash + 1) != std::string::npos) {
    if (error) *error = "Enter a code like NAME#123";
    return false;
  }
  if (out.size() > 18) {
    if (error) *error = "Connect codes can be at most 18 characters";
    return false;
  }
  for (size_t i = hash + 1; i < out.size(); ++i) {
    if (!std::isdigit((unsigned char)out[i])) {
      if (error) *error = "The part after # must be numbers";
      return false;
    }
  }
  if (normalized) *normalized = out;
  if (error) error->clear();
  return true;
}

std::string format_search_duration(uint32_t ticks) {
  const uint32_t total_seconds = ticks / 60;
  const uint32_t minutes = total_seconds / 60;
  const uint32_t seconds = total_seconds % 60;
  char out[32];
  std::snprintf(out, sizeof out, "%u:%02u", minutes, seconds);
  return out;
}

bool phase_forces_input_capture(Phase phase) {
  return phase == Phase::Handoff || phase == Phase::Failure;
}

const char* phase_name(Phase phase) {
  switch (phase) {
    case Phase::Idle: return "Idle";
    case Phase::Searching: return "Searching";
    case Phase::Handoff: return "Handoff";
    case Phase::OnlineFlow: return "OnlineFlow";
    case Phase::InMatch: return "InMatch";
    case Phase::Failure: return "Failure";
    case Phase::ReturningToPractice: return "ReturningToPractice";
  }
  return "Unknown";
}

}  // namespace slippi::native_practice
