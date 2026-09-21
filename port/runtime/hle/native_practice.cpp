// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_practice.h"

#include "host.h"
#include "slippi_online.h"
#include "slippi_playback.h"

#include <array>
#include <deque>
#include <mutex>
#include <utility>

namespace slippi::native_practice {
namespace {

constexpr uint32_t kSceneState = 0x80479D30;
constexpr uint32_t kMinorSceneControl = 0x80479D58;
constexpr uint8_t kOnlineMajor = 8;
constexpr uint8_t kTrainingMajor = 0x1C;
constexpr uint32_t kOnlineMode = 0x804DAFA0;
constexpr uint32_t kEventBackup = 0x8045A6C0 + 0x532;
constexpr uint32_t kPlayerSlots = 0x80453080;
constexpr uint32_t kPlayerStride = 0xE90;
constexpr uint32_t kMatchStage = 0x8046B6A0 + 0x24C8 + 0x0E;
constexpr uint32_t kSearchTimeoutTicks = 60 * 90;

enum class CommandKind { StartDirect, Cancel, AcknowledgeFailure };
struct Command { CommandKind kind; std::string text; };

struct SavedPlayer {
  bool active = false;
  uint32_t state = 0;
  uint32_t character = 0;
  uint32_t player_kind = 3;
  uint8_t costume = 0;
  uint8_t controller = 0;
  uint8_t cpu_level = 0;
  uint16_t damage = 0;
  uint16_t initial_damage = 0;
};

struct PracticeConfig {
  bool valid = false;
  int local_slot = 0;
  int controller_port = 0;
  uint16_t stage = 0;
  std::array<SavedPlayer, 4> players{};
};

std::mutex g_mutex;
std::deque<Command> g_commands;
Snapshot g_snapshot;
Lifecycle g_lifecycle;
PracticeConfig g_practice;
std::string g_connect_code;
std::string g_opponent;
int g_matchmaking_state = 0;
uint32_t g_search_ticks = 0;
uint32_t g_generation = 0;
uint32_t g_return_ticks = 0;
Phase g_logged_phase = Phase::Idle;

bool is_training() { return host::rd8(kSceneState) == kTrainingMajor; }
bool is_online_scene() { return host::rd8(kSceneState) == kOnlineMajor; }

PracticeConfig capture_practice() {
  PracticeConfig out;
  out.valid = true;
  out.stage = host::rd16(kMatchStage);
  bool found_human = false;
  for (int i = 0; i < 4; ++i) {
    const uint32_t base = kPlayerSlots + (uint32_t)i * kPlayerStride;
    SavedPlayer& p = out.players[i];
    p.state = host::rd32(base);
    p.character = host::rd32(base + 4);
    p.player_kind = host::rd32(base + 8);
    p.active = p.state == 2 || p.state == 3;
    p.costume = host::rd8(base + 0x44);
    p.controller = host::rd8(base + 0x48);
    p.cpu_level = host::rd8(base + 0x49);
    p.damage = host::rd16(base + 0x5C);
    p.initial_damage = host::rd16(base + 0x5E);
    if (!found_human && p.active && p.player_kind == 0) {
      found_human = true;
      out.local_slot = i;
      out.controller_port = p.controller < 4 ? p.controller : 0;
    }
  }
  return out;
}

void write_event_backup() {
  if (!g_practice.valid) return;
  const SavedPlayer& p = g_practice.players[g_practice.local_slot];
  host::wr8(kEventBackup + 0, (uint8_t)p.character);
  host::wr8(kEventBackup + 1, p.costume);
  host::wr8(kEventBackup + 4, (uint8_t)g_practice.controller_port);
}

void restore_practice_fields() {
  if (!g_practice.valid) return;
  host::wr16(kMatchStage, g_practice.stage);
  for (int i = 0; i < 4; ++i) {
    const SavedPlayer& p = g_practice.players[i];
    if (!p.active) continue;
    const uint32_t base = kPlayerSlots + (uint32_t)i * kPlayerStride;
    host::wr32(base + 4, p.character);
    host::wr32(base + 8, p.player_kind);
    host::wr8(base + 0x44, p.costume);
    host::wr8(base + 0x48, p.controller);
    host::wr8(base + 0x49, p.cpu_level);
    host::wr16(base + 0x5C, p.damage);
    host::wr16(base + 0x5E, p.initial_damage);
  }
}

void request_major(uint8_t major) {
  // Native equivalents of Scene_SetNextMajor, Scene_ExitMajor and Scene_ExitMinor. The last write
  // makes the current minor run its decide path on the next guest frame; writing only the major
  // pending flag would leave an endless Training minor alive.
  host::wr8(kSceneState + 1, major);
  host::wr8(kSceneState + 12, 1);
  host::wr32(kMinorSceneControl + 12, 1);
}

void run_decision(const Decision& d) {
  if (g_lifecycle.phase() == Phase::Failure && g_logged_phase != Phase::Failure) {
    ++g_generation;
    host::log("native practice: pre-match failure: %s", g_lifecycle.error().c_str());
  }
  if (d.cleanup_connection) slippi::online::native_cleanup_match();
  if (d.request_online_handoff) {
    write_event_backup();
    host::wr8(kOnlineMode, 2);  // Matchmaking::DIRECT, consumed by the normal online scene.
    request_major(kOnlineMajor);
    host::log("native practice: match found; handing off to normal online flow");
  }
  if (d.request_practice_return) {
    write_event_backup();
    // Seed the guest fields before Training's major-scene preparation reads them, then apply the
    // same narrow set again once the Training match minor has settled.
    restore_practice_fields();
    request_major(kTrainingMajor);
    g_return_ticks = 0;
    host::log("native practice: returning to Training after pre-match disconnect");
  }
}

void fail(const std::string& reason, bool cleanup) {
  run_decision(g_lifecycle.fail(reason, cleanup));
}

void process_command(Command command) {
  switch (command.kind) {
    case CommandKind::StartDirect: {
      if (g_lifecycle.phase() != Phase::Idle) return;
      std::string code, error;
      if (!is_training()) { fail("Start Direct from the Training scene", false); return; }
      if (!normalize_direct_code(command.text, &code, &error)) { fail(error, false); return; }
      if (slippi::online::session_mode() >= 0) { fail("An online session is already active", false); return; }
      g_practice = capture_practice();
      const SavedPlayer& player = g_practice.players[g_practice.local_slot];
      g_connect_code = code;
      g_opponent.clear();
      g_matchmaking_state = 0;
      g_search_ticks = 0;
      if (!g_lifecycle.begin_search()) return;
      host::wr8(kOnlineMode, 2);
      if (!slippi::online::native_start_match(2, code, (uint8_t)player.character,
                                               player.costume, &error)) {
        fail(error.empty() ? "Unable to start Direct search" : error, true);
        return;
      }
      ++g_generation;
      host::log("native practice: Direct search started for %s (Training port %d)",
                code.c_str(), g_practice.controller_port + 1);
      break;
    }
    case CommandKind::Cancel: {
      Decision d = g_lifecycle.cancel();
      if (!d.cleanup_connection) return;
      run_decision(d);
      g_connect_code.clear();
      g_opponent.clear();
      g_matchmaking_state = 0;
      g_search_ticks = 0;
      ++g_generation;
      host::log("native practice: search cancelled and connection cleaned up");
      break;
    }
    case CommandKind::AcknowledgeFailure:
      if (g_lifecycle.phase() != Phase::Failure) return;
      run_decision(g_lifecycle.acknowledge_failure());
      if (g_lifecycle.phase() == Phase::Idle) {
        g_connect_code.clear();
        g_opponent.clear();
        g_matchmaking_state = 0;
        g_search_ticks = 0;
      }
      ++g_generation;
      break;
  }
}

void publish_snapshot() {
  Snapshot out;
  out.phase = g_lifecycle.phase();
  out.in_practice = is_training();
  out.tab_available = !slippi::playback::enabled() && !slippi::online::is_online_match() &&
                      (out.phase == Phase::Idle || out.phase == Phase::Searching);
  out.cosmetic_profile_locked = out.phase == Phase::Searching || out.phase == Phase::Handoff ||
                                out.phase == Phase::OnlineFlow || out.phase == Phase::InMatch;
  out.controller_port = g_practice.valid ? g_practice.controller_port : 0;
  out.matchmaking_state = g_matchmaking_state;
  out.generation = g_generation;
  out.search_ticks = g_search_ticks;
  out.connect_code = g_connect_code;
  out.opponent = g_opponent;
  out.detail = g_lifecycle.error();
  switch (out.phase) {
    case Phase::Idle: out.status = out.in_practice ? "Practice" : "Ready"; break;
    case Phase::Searching: out.status = "Searching for " + g_connect_code; break;
    case Phase::Handoff: out.status = "Match found"; break;
    case Phase::OnlineFlow: out.status = "Connecting..."; break;
    case Phase::InMatch: out.status = "In match"; break;
    case Phase::Failure: out.status = "Disconnected"; break;
    case Phase::ReturningToPractice: out.status = "Returning to practice..."; break;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_snapshot = std::move(out);
}

}  // namespace

Snapshot snapshot() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_snapshot;
}

void submit_start_direct(const std::string& connect_code) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_commands.push_back({CommandKind::StartDirect, connect_code});
}

void submit_cancel() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_commands.push_back({CommandKind::Cancel, {}});
}

void submit_acknowledge_failure() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_commands.push_back({CommandKind::AcknowledgeFailure, {}});
}

void tick() {
  std::deque<Command> commands;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    commands.swap(g_commands);
  }
  for (Command& command : commands) process_command(std::move(command));

  if (g_lifecycle.phase() == Phase::Searching) {
    ++g_search_ticks;
    if (g_search_ticks > kSearchTimeoutTicks) {
      fail("The Direct search timed out", true);
    } else {
      const slippi::online::NativeMatchPoll poll = slippi::online::native_poll_match();
      g_matchmaking_state = poll.process_state;
      if (!poll.opponent.empty()) g_opponent = poll.opponent;
      if (poll.error.empty() && slippi::online::session_mode() < 0)
        fail("The connection ended before the match started", true);
      else
        run_decision(g_lifecycle.matchmaking_result(poll.connection_success, poll.local_ready,
                                                    poll.remote_ready, poll.error));
    }
  } else if (g_lifecycle.phase() == Phase::Handoff ||
             g_lifecycle.phase() == Phase::OnlineFlow ||
             g_lifecycle.phase() == Phase::InMatch) {
    run_decision(g_lifecycle.observe_session(is_online_scene(),
                                             slippi::online::session_mode() >= 0,
                                             slippi::online::is_online_match()));
  } else if (g_lifecycle.phase() == Phase::ReturningToPractice) {
    if (is_training() && host::rd8(kSceneState + 3) == 2) ++g_return_ticks;
    else g_return_ticks = 0;
    if (g_return_ticks >= 3) {
      restore_practice_fields();
      g_lifecycle.practice_restored();
      g_connect_code.clear();
      g_opponent.clear();
      g_matchmaking_state = 0;
      ++g_generation;
      host::log("native practice: Training configuration fields restored");
    }
  }

  if (g_lifecycle.phase() != g_logged_phase) {
    g_logged_phase = g_lifecycle.phase();
    host::log("native practice: phase %s", phase_name(g_logged_phase));
  }
  publish_snapshot();
}

void shutdown() {
  if (g_lifecycle.phase() != Phase::Idle && g_lifecycle.phase() != Phase::InMatch)
    slippi::online::native_cleanup_match();
  g_lifecycle.reset();
  g_practice = {};
  g_connect_code.clear();
  g_opponent.clear();
  g_matchmaking_state = 0;
  g_search_ticks = 0;
  publish_snapshot();
}

bool cosmetic_profile_locked() { return snapshot().cosmetic_profile_locked; }

}  // namespace slippi::native_practice
