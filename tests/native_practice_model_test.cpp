// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_practice_model.h"

#include <cstdio>
#include <string>

using slippi::native_practice::Lifecycle;
using slippi::native_practice::Phase;

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

  Lifecycle lifecycle;
  CHECK(lifecycle.begin_search());
  CHECK(!lifecycle.begin_search());
  auto d = lifecycle.matchmaking_result(false, true, false, {});
  CHECK(!d.request_online_handoff && lifecycle.phase() == Phase::Searching);
  d = lifecycle.matchmaking_result(true, true, true, {});
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
