// D3D12 backend for captured GX frames.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <vector>
#include "gx_core.h"
#ifdef GX_DLSS5
#include "gx_dlss5.h"
#endif

namespace gx {
// Video memory the game is using and the adapter's own installed size, in GB (D3D12 only; false
// until the renderer has measured it). Refreshed about twice a second.
bool vram_usage(float* used_gb, float* total_gb);
// GPU-measured cost of the DLAA/DLSS pass and the DLSS 5 pass, in milliseconds; 0 for a pass that
// has not run since launch. See D3D12Backend::read_gpu_timers.
void gpu_pass_cost(float* dlaa_ms, float* neural_ms);

// Authored = predict ahead from the latest game frame (no delay); AuthoredInterpolate = exact
// in-betweens of the last two game frames (one frame of display delay, no overshoot).
enum class SubFrameMode { Off, Extrapolate, Interpolate, Authored, AuthoredInterpolate };

// Which graphics API renders the game. D3D12 is the default and the only one with DLSS; D3D11 is
// for machines whose driver cannot start D3D12. Switching takes effect at the next launch.
enum class RenderApi { D3D12, D3D11 };

// How the 640x480 image is fitted to the window. Presentation only: it changes nothing the game
// computes, so it cannot desync and two players in one match may pick different values.
//   Auto    73:60 normally, 16:9 with the Slippi widescreen code on (see presented_aspect).
//   Native  73:60, the aspect Melee's own camera asks for (Slippi Dolphin: "Force 73:60 (Melee)").
//   Stretch fills the window edge to edge with no bars, the "stretched res" some players prefer.
enum class AspectMode { Auto = 0, Native = 1, Force4_3 = 2, Force16_9 = 3, Stretch = 4 };

struct D3D12Options {
  RenderApi api = RenderApi::D3D12;   // --backend d3d11|d3d12, or "backend" in port-settings.ini
  // Presentation timeline (threaded renderer only). fps_cap 0 = uncapped. With a SubFrameMode other
  // than Off the renderer presents new sub-frames between 60 Hz simulation frames.
  double fps_cap = 60; // -1 follows the active monitor
  bool fullscreen = false;
  // DLSS Frame Generation (RTX 40+): needs an Upscaling mode; adds latency. 0 off, 1 2x, 2 3x, 3 4x
  // (Multi Frame Generation, RTX 50 only), 4 Dynamic (the driver picks the multiplier).
  int frame_generation_mode = 0;
  int reflex_mode = 0;            // NVIDIA Reflex: 0 off, 1 on, 2 on + boost (at least on whenever frame generation is)
  bool reflex_stats = false;      // show Reflex's measured render latency under the FPS counter
  bool reflex_flash = false;      // Reflex flash indicator (latency analyzer monitors, LDAT): flashes on the A button
  int dlss_mode = 0;              // gx::DlssMode: 0 native, 1 DLAA, 2 quality, 3 balanced, 4 performance, 5 ultra performance
  // EXPERIMENTAL DLSS 5 Neural Rendering over the DLSS/DLAA output (gx_dlss5.h). Needs D3D12, an
  // Upscaling mode other than Native, and NVIDIA's model on the machine; off by default.
#ifdef GX_DLSS5
  bool dlss5 = false;
  dlss5::Tuning dlss5_tuning;
#endif
  float dlss_jitter_sign = -1.0f; // calibrated 2026-09-11: -1 reconstructs sharp text, +1 blurs (see PORT_COMPLETION.md)
  bool pc_settings = false, settings_open = false, performance_overlay = false;
  bool show_vram = false;          // video memory in use and the budget Windows gives the game, under the FPS
  bool show_fps = false, show_ping = true;   // small top-left readouts: presented frame rate; netplay ping while online
  // The "Settings: F1" reminder in the corner. On for a new player, off for anyone who knows the
  // key and does not want it in a recording.
  bool settings_hint = true;
  // The small "Matchmaking: Tab" reminder. Matchmaking remains available when it is hidden.
  bool matchmaking_hint = true;
  bool input_overlay = false;     // on-screen controller display, for streaming
  int input_overlay_ports = 1;    // bitmask of the controller ports it shows (bit 0 = port 1)
  bool input_overlay_values = false;
  int input_overlay_stick = 5;            // stick knob size, 1 (small marker) to 10 (fills much of the gate)        // print each stick's value (Melee units) under it
  bool input_overlay_hide_border = false;  // draw the pads with no panel background or resize grip
  // Drop in-world translucent effects to buy frame rate on weak machines: 0 everything, 1 skips
  // effects that do not write depth (sparks, glow, smoke), 2 skips translucent world geometry too.
  // Purely presentational, so unlike a Gecko code it cannot desync and both players may differ.
  int effects_level = 0;
  // "Low spec": one switch that puts every setting which costs frames at its cheapest, for
  // integrated graphics and older laptops. Turning it off must give the player their own settings
  // back rather than a hardcoded default, so what they had is kept here while it is on. Both the
  // switch and the kept values are saved in port-settings.ini, so the state survives a restart.
  struct LowSpecPrevious {
    RenderApi api = RenderApi::D3D12;
    double fps_cap = 60;
    int efb_scale = 0, ssaa = 1, anisotropy = 16, effects_level = 0, dlss_mode = 0;
    SubFrameMode subframe = SubFrameMode::AuthoredInterpolate;
  };
  bool low_spec = false;
  LowSpecPrevious low_spec_previous;
  // Discord Rich Presence, off by default: it tells the player's Discord friends what they are
  // playing. The application id is Melee Unlocked's own, registered once for the whole game rather
  // than per player, and it is not a secret (Rich Presence needs no token). A player can override it
  // in the settings to point the presence at an application of their own.
  bool discord_presence = false;
  std::string discord_app_id = "1549608280949792790";
  // Gecko codes switched on by the player: Slippi's own switchable codes by id, and codes the
  // player supplied in the GeckoCodes folder. Both default to off, so a list holds what is on.
  // Widescreen is not in here: it has its own setting and its own control under Video.
  std::vector<std::string> gecko_enabled;
  std::vector<std::string> user_gecko_enabled;
  // Watches every presented frame for a single frame that differs from the one before and the one
  // after it while those two agree, which is what a one-frame visual glitch looks like and what no
  // human can catch with a screenshot key. Diagnostic only: it costs a small downsample and readback
  // per presented frame, so it is off unless --flicker-scan asks for it.
  bool flicker_scan = false;
  // Development: hold the sub-frame phase at this value and present once per simulation frame, so a
  // captured run is repeatable and can be compared picture for picture against another run.
  double pin_phase = -1;
  std::string settings_path = "port-settings.ini";
  std::string frame_times; // optional buffered CSV of CPU presentation timing
  SubFrameMode subframe = SubFrameMode::Off;
  int efb_scale = 0;          // internal resolution multiplier; 0 = auto (integer scale covering the window, like Dolphin "Auto (Window Size)")
  int window_w = 1280, window_h = 960;  // initial client size
  // The player picked a fixed window size ("window WxH" in the ini, or --window): the panel keeps
  // the window at it. Off = the window is whatever size it has been dragged to.
  bool window_pinned = false;
  AspectMode aspect = AspectMode::Auto;   // --aspect, or "aspect" in port-settings.ini
  bool vsync = false;
  bool widescreen = false;    // Slippi Widescreen 16:9 code on (present at 16:9 and tell the game)
  // EXPERIMENTAL: widen the frustum in the renderer rather than running the Gecko code, so the HUD
  // and the 2D layer keep their authored size and nothing is written to guest memory. Mutually
  // exclusive with `widescreen`: both together would widen twice. See gx_shader.h.
  bool true_widescreen = false;
  // Custom texture packs: replace game textures with PNGs from Load/Textures/GALE01, using
  // Dolphin's filenames and hashes so existing packs work unchanged. Off by default, and while it
  // is off no pack directory is opened or scanned. Display only, so it is safe online and the two
  // players may differ. dump_textures writes what the game drew, with the names a pack must use.
  bool custom_textures = false;
  bool dump_textures = false;
  // Decode every replacement when the game starts rather than the first time each texture appears,
  // as Dolphin's "Prefetch Custom Textures" does. A large pack costs about half a minute once here
  // instead of a stutter each time a new texture comes on screen.
  bool prefetch_textures = true;
  float sharpness = 0.0f;     // 0..1 contrast-adaptive sharpening in the present pass (works with or without DLSS)
  // Display adjustment in the present pass, over the whole picture including the HUD. 1.0 is neutral
  // (untouched) for all three; the shader skips the work entirely when nobody has moved them.
  float brightness = 1.0f, contrast = 1.0f, vibrance = 1.0f;
  int anisotropy = 16;        // texture anisotropic filtering 1..16
  int ssaa = 1;               // supersampling factor: 1 off, 2 = 4x SSAA (EFB rendered at 2x the chosen scale, box filtered)
  std::string capture_path;   // write a PPM of the presented image at capture_frame
  uint32_t capture_frame = 0;
  uint32_t capture_every = 0;  // if set, capture every N presented frames as <capture_path>_<frame>.ppm
  uint32_t capture_burst = 0;  // if set, capture this many consecutive presented frames from capture_frame
  uint64_t capture_sim_frame = 0;  // if set, the burst starts at the first presented frame whose simulation sequence >= this
  std::string shader_cache = "shadercache";   // directory for compiled shader blobs and the D3D12 pipeline library
  std::string dump_path;      // write a text dump of draw state + shaders at dump_frame
  uint32_t dump_frame = 0;
};

// Sub-frame animation only means anything when the display shows more frames than the simulation
// produces. At a cap of 60 or below there is one presented frame per 60 Hz tick either way, so
// re-posing buys no smoothness at all and costs three things: the solver's time, a frame of display
// delay in the Interpolate modes, and poses taken at an arbitrary fraction of a tick instead of the
// exact ones the game computed. That last one is visible: players running a 60 cap with sub-frame
// animation on reported stage geometry glitching on Yoshi's Story, Dream Land and Fountain of
// Dreams, and switching sub-frame animation off fixed it. So the two are not allowed to combine.
//
// A cap of 0 is uncapped and -1 follows the monitor, and both of those can exceed 60.
inline bool subframe_useful(double fps_cap) { return fps_cap <= 0.0 || fps_cap > 60.0; }

// Aspect the presented image is letterboxed to, for these options and this client size.
//
// Melee does not render 4:3. Every camera in the game asks C_MTXPerspective for aspect
// 1.2173333f (melee/src/melee/cm/camera.c, written 913.0f/750.0f in melee/src/melee/ty/toy.c),
// which the captured XF projection registers confirm at runtime: 4.51071/3.70743 = 1.216668 on
// the menus and 5.67128/4.65877 = 1.217334 in a match, both 73:60 to within a rounding error.
// The console agrees: the VI paints Melee's 640 framebuffer columns into 640 of the 720 BT.601
// samples of an NTSC active line, which is why Dolphin's VI derived aspect comes out at 1.2154
// and why Slippi Dolphin ships "Force 73:60 (Melee)" as its default (VideoConfig.cpp).
// The Slippi widescreen Gecko code multiplies that camera aspect by 320/219, and 73/60 * 320/219
// is exactly 16/9, so with the code on the correct presentation is 16:9.
//
// widenable_scene is frame_has_widenable_scene(frame) for the frame about to present (gx_core.h):
// whether build_projection's per-camera widen is actually reaching anything, i.e. a mode's
// character/stage select through its results screen. Auto/widescreen only claims 16:9 there.
// Letterboxing the bare menu shell (main menu, options, trophies, vs mode select) to 16:9 too would
// stretch the whole picture with nothing compensating: that screen is almost entirely the 2D layer,
// which is deliberately never widened (see build_projection's comment on why), so it has no 3D
// content to absorb the wider frame the way a match or a character select does. A caller with no
// frame yet (window just opened) passes true rather than default to a menu it has not seen.
inline float presented_aspect(const D3D12Options& options, int client_w, int client_h, bool widenable_scene = true) {
  switch (options.aspect) {
    case AspectMode::Native:    return 73.0f / 60.0f;
    case AspectMode::Force4_3:  return 4.0f / 3.0f;
    case AspectMode::Force16_9: return 16.0f / 9.0f;
    // No bars at all: claiming the window's own aspect makes the letterbox maths fill it exactly.
    case AspectMode::Stretch:   return (float)(client_w > 0 ? client_w : 1) / (float)(client_h > 0 ? client_h : 1);
    default:
      return (options.widescreen || options.true_widescreen) && widenable_scene ? 16.0f / 9.0f : 73.0f / 60.0f;
  }
}

// Ask the renderer to write the next `frames` presented frames to capture/blink_<n>.ppm. Bound to
// F2 so a defect that only appears in a real session can be caught by the person watching it:
// four scripted captures of the stage blinking reproduced nothing, because a headless run never
// falls behind and never sees the conditions it needs.
void request_frame_capture(unsigned frames);
// The pending count, shared by both backends so F2 behaves identically on each.
unsigned gx_capture_request();
void gx_capture_request_set(unsigned frames);

Backend* create_d3d12_backend(void* hwnd, int client_w, int client_h, const D3D12Options& options);
const D3D12Options& d3d12_options(Backend* backend);
void d3d12_resize(Backend* backend, int w, int h);
void d3d12_stats(Backend* backend, uint32_t* frames_presented, uint32_t* pipelines, uint32_t* textures);
// Per-section CPU cost of execute_draw since the last call (diagnostics), as a one-line summary.
std::string d3d12_profile_line();

}  // namespace gx
