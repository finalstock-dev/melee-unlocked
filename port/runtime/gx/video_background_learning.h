// Pure scene and candidate scoring used by the menu-video learner and its native tests.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace gx::video_bg::learning {

constexpr unsigned kDecisionFrames = 90;
constexpr unsigned kTimeoutFrames = 360;
constexpr unsigned kSavedTargetTimeoutFrames = 180;
constexpr unsigned kMinimumPersistentFrames = 20;
constexpr unsigned kMaximumCandidates = 256;

// Kept pure so the immediate vanilla fallback can be regression-tested without a GPU backend.
constexpr bool should_route_video(bool enabled, int active_slot, bool has_video) {
  return enabled && active_slot >= 0 && active_slot < 2 && has_video;
}

// Offline CSS/SSS uses minor 0/1. Slippi online uses 0/4; minor 1 is a transitional scene.
constexpr int scene_slot(uint8_t major, uint8_t minor) {
  const bool match_mode = major == 0x02 || major == 0x03 || major == 0x04 ||
                          major == 0x05 || major == 0x0f ||
                          (major >= 0x10 && major <= 0x13) ||
                          major == 0x1b || major == 0x1c;
  if ((match_mode || major == 0x08) && minor == 0) return 0;
  if ((match_mode && minor == 1) || (major == 0x08 && minor == 4)) return 1;
  return -1;
}

// Persistence prevents animated icons from winning merely because they are drawn often. Area then
// separates a backdrop or backdrop tile from cursors and labels without assuming a square texture
// or a 192-pixel minimum dimension.
constexpr uint64_t candidate_score(uint32_t width, uint32_t height,
                                   unsigned distinct_frames, unsigned observations) {
  if (!width || !height || width > 4096 || height > 4096) return 0;
  const uint64_t area = (uint64_t)width * height;
  if (area < 1024) return 0;
  const uint64_t persistence = std::min(distinct_frames, kTimeoutFrames);
  const uint64_t repeat_bonus = std::min(observations, distinct_frames * 4u);
  return area * (persistence * 8u + repeat_bonus);
}

constexpr bool confident(uint32_t width, uint32_t height, unsigned distinct_frames,
                         unsigned learning_frames, unsigned observations) {
  if (distinct_frames < kMinimumPersistentFrames || !learning_frames) return false;
  if (distinct_frames * 2 < learning_frames) return false;
  return candidate_score(width, height, distinct_frames, observations) != 0;
}

}  // namespace gx::video_bg::learning
