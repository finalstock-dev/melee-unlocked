// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_practice.h"

#include <cstdio>
#include <string>

using slippi::native_practice::Lifecycle;
using slippi::native_practice::MatchMode;
using slippi::native_practice::Phase;

static_assert((int)MatchMode::Ranked == 0, "Ranked must match Slippi's online-mode value");
static_assert((int)MatchMode::Unranked == 1, "Unranked must match Slippi's online-mode value");
static_assert((int)MatchMode::Direct == 2, "Direct must match Slippi's online-mode value");

#define CHECK(condition) do { \
  if (!(condition)) { \
    std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); \
    return 1; \
  } \
} while (false)

int main() {
  std::string code, error;
  CHECK(slippi::native_practice::normalize_direct_code(" abcd # 123 ", &code, &error));
  CHECK(code == "ABCD#123");
  CHECK(slippi::native_practice::normalize_direct_code("ABCDEFGH#123456789", &code, &error));
  CHECK(code.size() == 18);
  CHECK(!slippi::native_practice::normalize_direct_code("ABCDEFGHI#123456789", &code, &error));
  CHECK(!slippi::native_practice::normalize_direct_code("ABCD", &code, &error));
  CHECK(!slippi::native_practice::normalize_direct_code("AB#12X", &code, &error));
  CHECK(!slippi::native_practice::normalize_direct_code("AB##12", &code, &error));
  CHECK(slippi::native_practice::format_search_duration(0) == "0:00");
  CHECK(slippi::native_practice::format_search_duration(59) == "0:00");
  CHECK(slippi::native_practice::format_search_duration(60) == "0:01");
  CHECK(slippi::native_practice::format_search_duration(3660) == "1:01");
  CHECK(slippi::native_practice::format_search_duration(5400) == "1:30");
  CHECK(!slippi::native_practice::phase_forces_input_capture(Phase::Idle));
  CHECK(!slippi::native_practice::phase_forces_input_capture(Phase::Searching));
  CHECK(slippi::native_practice::phase_forces_input_capture(Phase::Handoff));
  CHECK(!slippi::native_practice::phase_forces_input_capture(Phase::OnlineFlow));
  CHECK(!slippi::native_practice::phase_forces_input_capture(Phase::InMatch));
  CHECK(slippi::native_practice::phase_forces_input_capture(Phase::Failure));
  CHECK(!slippi::native_practice::phase_forces_input_capture(Phase::ReturningToPractice));
  CHECK(slippi::native_practice::phase_shows_return_overlay(Phase::ReturningToPractice, false));
  CHECK(!slippi::native_practice::phase_shows_return_overlay(Phase::ReturningToPractice, true));
  CHECK(!slippi::native_practice::phase_shows_return_overlay(Phase::Failure, false));
  CHECK(!slippi::native_practice::phase_owns_online_mode(Phase::Searching));
  CHECK(slippi::native_practice::phase_owns_online_mode(Phase::Handoff));
  CHECK(!slippi::native_practice::phase_owns_online_mode(Phase::OnlineFlow));
  CHECK(!slippi::native_practice::phase_owns_online_mode(Phase::InMatch));
  CHECK(!slippi::native_practice::phase_owns_online_mode(Phase::Failure));
  CHECK(slippi::native_practice::matchmaking_tab_available(Phase::Idle, false, false, false));
  CHECK(!slippi::native_practice::matchmaking_tab_available(Phase::Idle, true, false, false));
  CHECK(!slippi::native_practice::matchmaking_tab_available(Phase::Idle, false, true, true));
  CHECK(!slippi::native_practice::matchmaking_tab_available(Phase::Idle, false, false, true));
  CHECK(slippi::native_practice::matchmaking_tab_available(Phase::Searching, false, false, true));

  Lifecycle lifecycle;
  CHECK(lifecycle.begin_search());
  CHECK(!lifecycle.begin_search());
  auto d = lifecycle.matchmaking_result(false, true, false, {});
  CHECK(!d.request_online_handoff && lifecycle.phase() == Phase::Searching);
  d = lifecycle.matchmaking_result(true, true, false, {});
  CHECK(d.request_online_handoff && lifecycle.phase() == Phase::Handoff && lifecycle.left_practice());
  lifecycle.observe_session(true, true, false);
  CHECK(lifecycle.phase() == Phase::OnlineFlow);
  lifecycle.observe_session(true, true, true);
  CHECK(lifecycle.phase() == Phase::InMatch && lifecycle.match_started());
  lifecycle.observe_session(true, false, false);
  CHECK(lifecycle.phase() == Phase::Idle);  // post-frame-1 cleanup does not synthesize a practice failure

  CHECK(lifecycle.begin_search());
  d = lifecycle.matchmaking_result(true, true, true, {});
  CHECK(d.request_online_handoff);
  d = lifecycle.observe_session(true, false, false);
  CHECK(d.cleanup_connection && lifecycle.phase() == Phase::Failure);
  d = lifecycle.acknowledge_failure();
  CHECK(d.request_practice_return && lifecycle.phase() == Phase::ReturningToPractice);
  lifecycle.practice_restored();
  CHECK(lifecycle.phase() == Phase::Idle);

  CHECK(lifecycle.begin_search());
  d = lifecycle.cancel();
  CHECK(d.cleanup_connection && lifecycle.phase() == Phase::Idle);
  CHECK(lifecycle.begin_search());  // a cancelled coordinator can immediately queue again
  d = lifecycle.cancel();
  CHECK(d.cleanup_connection && lifecycle.phase() == Phase::Idle);

  CHECK(lifecycle.begin_search());
  d = lifecycle.matchmaking_result(false, false, false, "server rejected the code");
  CHECK(d.cleanup_connection && lifecycle.phase() == Phase::Failure);
  CHECK(lifecycle.error() == "server rejected the code");
  d = lifecycle.acknowledge_failure();
  CHECK(!d.request_practice_return && lifecycle.phase() == Phase::Idle);
  return 0;
}
