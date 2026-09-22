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
// OFST_R13_ONLINE_MODE is -0x5060 from Melee's r13 base (0x804DB6A0).
constexpr uint32_t kOnlineMode = 0x804D6640;
// OFST_R13_ISWINNER (-0x5037) and OFST_R13_CHOSESTAGE (-0x5036). Normal Direct
// initializes these when Start begins a search; the native search begins outside that CSS path.
constexpr uint32_t kDirectIsWinner = 0x804D6669;
constexpr uint32_t kDirectChoseStage = 0x804D666A;
constexpr uint32_t kEventBackup = 0x8045A6C0 + 0x532;
constexpr uint32_t kPlayerSlots = 0x80453080;
constexpr uint32_t kPlayerStride = 0xE90;
constexpr uint32_t kMatchStage = 0x8046B6A0 + 0x24C8 + 0x0E;
constexpr uint32_t kSearchTimeoutTicks = 60 * 90;

enum class CommandKind { StartRanked, StartUnranked, StartDirect, Cancel, AcknowledgeFailure };
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
MatchMode g_match_mode = MatchMode::None;
int g_matchmaking_state = 0;
uint32_t g_search_ticks = 0;
uint32_t g_generation = 0;
uint32_t g_return_ticks = 0;
uint8_t g_origin_major = 1;
bool g_restore_training = false;
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
    if (mode_needs_direct_first_match_reset((uint8_t)g_match_mode)) {
      host::wr8(kDirectIsWinner, 0xFF);  // ISWINNER_NULL: first match, not a loser rematch.
      host::wr8(kDirectChoseStage, 0);
      host::log("native practice: initialized Direct first-match CSS state");
    }
    host::wr8(kOnlineMode, (uint8_t)g_match_mode);  // Consumed by the normal online scene.
    request_major(kOnlineMajor);
    host::log("native practice: %s match found; handing off to normal online flow",
              match_mode_name(g_match_mode));
  }
  if (d.request_practice_return) {
    if (g_restore_training) {
      write_event_backup();
      // Seed the guest fields before Training's major-scene preparation reads them, then apply the
      // same narrow set again once the Training match minor has settled.
      restore_practice_fields();
    }
    request_major(g_origin_major);
    g_return_ticks = 0;
    host::log("native practice: returning to offline scene %02X after pre-match disconnect",
              g_origin_major);
  }
}

void fail(const std::string& reason, bool cleanup) {
  run_decision(g_lifecycle.fail(reason, cleanup));
}

void clear_search_metadata() {
  g_match_mode = MatchMode::None;
  g_connect_code.clear();
  g_opponent.clear();
  g_matchmaking_state = 0;
  g_search_ticks = 0;
}

void start_search(MatchMode mode, const std::string& connect_code) {
  if (g_lifecycle.phase() != Phase::Idle) return;
  if (slippi::playback::enabled()) { fail("Matchmaking is unavailable during replay playback", false); return; }
  if (is_online_scene()) { fail("Matchmaking is already using the online scene", false); return; }
  if (slippi::online::session_mode() >= 0) { fail("An online session is already active", false); return; }

  g_origin_major = host::rd8(kSceneState);
  g_restore_training = is_training();
  g_practice = capture_practice();
  SavedPlayer& player = g_practice.players[g_practice.local_slot];
  // Menus do not always have an active player slot. Use Fox/color 0 as a safe CSS seed in that
  // case; an active offline fighter or Training selection is carried across when available.
  if (!player.active || player.character >= 26) {
    player.character = 2;
    player.costume = 0;
    player.controller = 0;
    g_practice.controller_port = 0;
  }
  g_match_mode = mode;
  g_connect_code = connect_code;
  g_opponent.clear();
  g_matchmaking_state = 0;
  g_search_ticks = 0;
  if (!g_lifecycle.begin_search()) return;

  const int online_mode = (int)mode;
  host::wr8(kOnlineMode, (uint8_t)online_mode);
  std::string error;
  if (!slippi::online::native_start_match(online_mode, connect_code, (uint8_t)player.character,
                                           player.costume, &error)) {
    fail(error.empty() ? "Unable to start matchmaking" : error, true);
    return;
  }
  ++g_generation;
  host::log("native practice: %s search started%s%s (offline scene %02X, port %d)",
            match_mode_name(mode), connect_code.empty() ? "" : " for ", connect_code.c_str(),
            g_origin_major, g_practice.controller_port + 1);
}

void process_command(Command command) {
  switch (command.kind) {
    case CommandKind::StartRanked:
      start_search(MatchMode::Ranked, {});
      break;
    case CommandKind::StartUnranked:
      start_search(MatchMode::Unranked, {});
      break;
    case CommandKind::StartDirect: {
      if (g_lifecycle.phase() != Phase::Idle) return;
      std::string code, error;
      if (!normalize_direct_code(command.text, &code, &error)) { fail(error, false); return; }
      start_search(MatchMode::Direct, code);
      break;
    }
    case CommandKind::Cancel: {
      Decision d = g_lifecycle.cancel();
      if (!d.cleanup_connection) return;
      run_decision(d);
      clear_search_metadata();
      ++g_generation;
      host::log("native practice: search cancelled and connection cleaned up");
      break;
    }
    case CommandKind::AcknowledgeFailure:
      if (g_lifecycle.phase() != Phase::Failure) return;
      run_decision(g_lifecycle.acknowledge_failure());
      if (g_lifecycle.phase() == Phase::Idle) clear_search_metadata();
      ++g_generation;
      break;
  }
}

void publish_snapshot() {
  Snapshot out;
  out.phase = g_lifecycle.phase();
  out.mode = g_match_mode;
  out.in_practice = is_training();
  const bool playback = slippi::playback::enabled();
  const bool online_match = slippi::online::is_online_match() || is_online_scene();
  const bool session_active = slippi::online::session_mode() >= 0;
  out.tab_available = matchmaking_tab_available(out.phase, playback, online_match, session_active);
  out.can_start = out.phase == Phase::Idle && !playback && !online_match && !session_active;
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
    case Phase::Searching:
      out.status = std::string("Searching ") + match_mode_name(g_match_mode);
      if (!g_connect_code.empty()) out.status += " for " + g_connect_code;
      break;
    case Phase::Handoff: out.status = "Match found"; break;
    case Phase::OnlineFlow: out.status = "Connecting..."; break;
    case Phase::InMatch: out.status = "In match"; break;
    case Phase::Failure: out.status = "Disconnected"; break;
    case Phase::ReturningToPractice: out.status = "Returning..."; break;
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

void submit_start_ranked() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_commands.push_back({CommandKind::StartRanked, {}});
}

void submit_start_unranked() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_commands.push_back({CommandKind::StartUnranked, {}});
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

  // Entering major 8 directly skips the title-menu option handler that normally owns this byte.
  // Keep the native selection authoritative during the scene transition, then leave every later
  // CSS decision to Slippi's normal online flow.
  if (phase_owns_online_mode(g_lifecycle.phase()) && g_match_mode != MatchMode::None) {
    const uint8_t expected = (uint8_t)g_match_mode;
    const uint8_t actual = host::rd8(kOnlineMode);
    if (actual != expected) {
      host::log("native practice: correcting guest online mode %u -> %u during handoff",
                actual, expected);
      host::wr8(kOnlineMode, expected);
    }
  }

  if (g_lifecycle.phase() == Phase::Searching) {
    ++g_search_ticks;
    if (g_search_ticks > kSearchTimeoutTicks) {
      fail(std::string("The ") + match_mode_name(g_match_mode) + " search timed out", true);
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
    const Phase before = g_lifecycle.phase();
    run_decision(g_lifecycle.observe_session(is_online_scene(),
                                             slippi::online::session_mode() >= 0,
                                             slippi::online::is_online_match()));
    if (before == Phase::InMatch && g_lifecycle.phase() == Phase::Idle)
      clear_search_metadata();
  } else if (g_lifecycle.phase() == Phase::ReturningToPractice) {
    const bool origin_ready = host::rd8(kSceneState) == g_origin_major &&
                              (!g_restore_training || host::rd8(kSceneState + 3) == 2);
    if (origin_ready) ++g_return_ticks;
    else g_return_ticks = 0;
    if (g_return_ticks >= 3) {
      if (g_restore_training) restore_practice_fields();
      g_lifecycle.practice_restored();
      clear_search_metadata();
      ++g_generation;
      host::log("native practice: offline scene %02X restored%s", g_origin_major,
                g_restore_training ? " with Training configuration" : "");
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
  g_restore_training = false;
  clear_search_metadata();
  publish_snapshot();
}

bool cosmetic_profile_locked() { return snapshot().cosmetic_profile_locked; }

const char* match_mode_name(MatchMode mode) {
  switch (mode) {
    case MatchMode::Ranked: return "Ranked";
    case MatchMode::Unranked: return "Unranked";
    case MatchMode::Direct: return "Direct";
    case MatchMode::None: return "matchmaking";
  }
  return "matchmaking";
}

}  // namespace slippi::native_practice
