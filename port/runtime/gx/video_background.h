// Looping menu video backgrounds. Presentation only: decoded frames never enter guest memory.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gx::video_bg {

// A published Media Foundation frame. RGB32 is BGRA in memory, matching the DXGI format used by
// both renderer backends. The shared_ptr keeps a worker-owned frame immutable during GPU upload.
struct Frame {
  uint32_t width = 0, height = 0;
  uint64_t serial = 0;
  std::vector<uint8_t> bgra;
};

struct ObservedTexture {
  std::string name;
  uint32_t width = 0, height = 0;
  unsigned observations = 0, distinct_frames = 0;
  uint64_t score = 0;
};

// Called once before a captured GX frame is submitted. CSS is minor scene 0 and SSS is minor 1
// for modes which lead into a match; Slippi online's CSS is major 08, minor 0.
void begin_frame(uint8_t scene_major, uint8_t scene_minor);

// True only while CSS/SSS is active and that slot has an MP4. It lets get_texture avoid even
// generating Dolphin texture names when no video can be displayed.
bool wants_texture_names();

// Observes the textures used by the active menu and returns its video frame once `base` is the
// learned background target. `slot` is 0 for CSS and 1 for SSS and selects a backend-owned dynamic
// texture. Before a target is learned this returns null and the normal guest texture is drawn.
std::shared_ptr<const Frame> lookup(const std::string& base, uint32_t width, uint32_t height,
                                    int* slot);

// Returns the decoded frame for the active CSS screen without tying it to guest texture geometry.
// The backend inserts it between Melee's hardcoded animated backdrop and textured foreground UI.
std::shared_ptr<const Frame> fullscreen_frame(int* slot);

// Test-build controls used by the native settings panel.
void set_enabled(bool enabled);
bool enabled();
void open_folder();
void relearn(int slot);  // 0 CSS, 1 SSS
std::string status(int slot);
std::string target(int slot);
std::vector<ObservedTexture> observed_textures(int slot);
bool choose_target(int slot, const std::string& name);
void report_backend_failure(int slot, const std::string& message);

}  // namespace gx::video_bg
