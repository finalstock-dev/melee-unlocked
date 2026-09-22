#include "video_background_learning.h"
#include <cstdio>

static int failures;
static void check(bool condition, const char* text) {
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", text); ++failures; }
}

int main() {
  check(gx::video_bg::learning::should_route_video(true, 0, true),
        "enabled CSS video routes through the replacement");
  check(!gx::video_bg::learning::should_route_video(false, 0, true),
        "disabling immediately routes the draw through vanilla");
  check(!gx::video_bg::learning::should_route_video(true, -1, true),
        "non-menu scenes retain vanilla");
  using namespace gx::video_bg::learning;
  check(!uses_texture_target(0), "CSS never substitutes the learned player-card atlas");
  check(uses_texture_target(1), "SSS keeps its genuine full-screen texture target");
  check(should_insert_fullscreen_layer(0, 217, true),
        "CSS inserts video between its hardcoded backdrop and textured foreground");
  check(!should_insert_fullscreen_layer(0, 7, true),
        "short setup runs do not trigger the CSS layer");
  check(!should_insert_fullscreen_layer(1, 217, true),
        "SSS continues through its texture-backed path");
  check(scene_slot(0x02, 0) == 0, "offline CSS scene");
  check(scene_slot(0x02, 1) == 1, "offline SSS scene");
  check(scene_slot(0x08, 0) == 0, "online CSS scene");
  check(scene_slot(0x08, 4) == 1, "online SSS scene");
  check(scene_slot(0x08, 1) == -1, "online transitional scene is not SSS");

  const uint64_t backdrop = candidate_score(320, 120, 60, 60);
  const uint64_t portrait = candidate_score(136, 188, 60, 60);
  const uint64_t label = candidate_score(512, 24, 60, 120);
  check(backdrop > portrait && backdrop > label,
        "a persistent short/tiled backdrop outranks portraits and labels");
  check(confident(320, 120, 45, 90, 45), "persistent short texture is eligible");
  check(!confident(640, 480, 4, 90, 40), "brief large splash is not eligible");
  check(candidate_score(16, 16, 90, 90) == 0, "tiny icons are ignored");

  if (!failures) std::puts("video background learning tests passed");
  return failures ? 1 : 0;
}
