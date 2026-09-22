// Slippi Online command handling for the EXI device: matchmaking, netplay input exchange,
// rollback savestates, chat, match state. Port of the online half of Dolphin's CEXISlippi.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace slippi::online {

struct Config {
  std::string user_dir = "runtime/slippi/User/Slippi";   // user.json, direct-codes.json (Slippi Launcher layout)
  int delay = 2;                 // Slippi Online input delay (frames)
  int chat = 0;                  // 0 enabled, 1 direct only, 2 disabled
  bool show_local_rank = true, show_opponent_rank = true;
};
Config& config();

void init();
void shutdown();
// Handles one online command (cmd byte, payload after it); responses go to `read_queue`.
// Returns false for commands this module does not own.
bool handle(uint8_t cmd, const uint8_t* payload, uint32_t payload_len, std::vector<uint8_t>& read_queue);
// Counts rollback loads so the renderer can treat them as discontinuities.
uint64_t rollback_count();
bool is_online_match();
// The in-game slot the local player occupies in the running online match (0-3).
int local_player_slot();
// Most recent measured round trip to the opponent, in milliseconds; 0 when not connected.
int ping_ms();
// The online mode of the session that is running or being set up, as a Matchmaking::OnlinePlayMode
// value (RANKED 0, UNRANKED 1, DIRECT 2, TEAMS 3, PARTY 4). -1 when there is no online session at
// all, which is what "offline" means to the rest of the port. Covers matchmaking, the online
// character select screen and the match itself, so a feature can be gated on the mode before the
// match starts rather than only once it is running.
int session_mode();
// In-game player slot of the local player in an online match (0..3).
int local_player_index();
// True while the game is polling the online menus (mode select, the online character select screen,
// waiting for an opponent) and not running a match.
bool in_online_menus();

// Native practice uses the same Slippi matchmaking implementation as the game's online menus.
// These calls are simulation-thread only. native_poll_match is deliberately side-effecting and
// must be advanced no more than once per intended 60 Hz tick, just like CMD_GET_MATCH_STATE.
struct NativeMatchPoll {
  int process_state = 0;
  bool connection_success = false;
  bool local_ready = false;
  bool remote_ready = false;
  std::string error;
  std::string opponent;
};
bool native_start_match(int mode, const std::string& connect_code, uint8_t character,
                        uint8_t color, std::string* error);
NativeMatchPoll native_poll_match();
void native_cleanup_match();

}  // namespace slippi::online
