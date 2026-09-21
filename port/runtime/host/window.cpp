// Win32 window, message pump, keyboard + XInput controller mapping to GameCube pads.
// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#include <hidsdi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>
#include <atomic>
#include "host.h"
#include "window.h"
#include "input_bindings.h"
#include "hid_pad.h"
#include "playstation_pad.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "xinput9_1_0.lib")

namespace host {

namespace {
HWND g_hwnd = nullptr;
bool g_keys[256];
std::mutex g_keys_mutex;
bool g_closed = false;
int g_client_w = 1280, g_client_h = 960;
ResizeCallback g_on_resize;
std::atomic<bool> g_fullscreen_toggle{false};
std::atomic<bool> g_settings_toggle{false};
std::atomic<bool> g_practice_toggle{false};
std::atomic<bool> g_escape_press{false};
MessageCallback g_on_message;
std::atomic<bool> g_ui_capture{false};
std::mutex g_ui_pad_mutex;
PadState g_ui_pad{};
PadState g_ui_pads[4]{};
bool g_ui_has_pad = false;
bool g_ui_gamecube = false;
void raw_input(HRAWINPUT raw);
void ds4_init_defaults();

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  // No mouse pointer over the game while it has focus and the settings panel is closed. The pointer
  // has nothing to do there, and after alt-tabbing back from a music player it sat in the middle of
  // the screen for the rest of the match. It comes back the moment the panel opens or focus leaves.
  // Before the ImGui handler, which would otherwise set the arrow itself.
  if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT && !g_ui_capture.load() && GetForegroundWindow() == h) { SetCursor(nullptr); return TRUE; }
  // Reserve Tab before ImGui or the game's keyboard bindings see it. The edge reaches the practice
  // UI whether a text field is active or the overlay is closed.
  if (m == WM_KEYDOWN && w == VK_TAB) {
    if (!(l & (1 << 30))) g_practice_toggle.store(true);
    // Tab is reserved for native matchmaking. Never let a binding on the same keyboard key reach
    // Melee for the frame that opens or closes the overlay.
    std::lock_guard<std::mutex> lock(g_keys_mutex); g_keys[VK_TAB] = false; return 0;
  }
  if (m == WM_KEYUP && w == VK_TAB) {
    std::lock_guard<std::mutex> lock(g_keys_mutex); g_keys[VK_TAB] = false; return 0;
  }
  if (g_on_message && g_on_message(h, m, w, l)) return 1;
  switch (m) {
    case WM_CLOSE: g_closed = true; request_exit(0); return 0;
    case WM_APP + 7:   // kCursorRefresh, see window_input_capture
      if (GetForegroundWindow() == h) SetCursor(w ? LoadCursor(nullptr, IDC_ARROW) : nullptr);
      return 0;
    case WM_INPUT: raw_input((HRAWINPUT)l); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_SYSKEYDOWN:
      if (w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30))) { g_fullscreen_toggle.store(true); return 0; }   // Alt+Enter, first press only
      break;
    case WM_SYSCHAR: if (w == VK_RETURN) return 0; break;   // no beep for Alt+Enter
    case WM_KEYDOWN: {
      // F1 opens and closes the settings panel. Taken here, from the first key-down of a press
      // (bit 30 is set on auto-repeats), rather than from ImGui's key state: the panel only runs on
      // frames that are drawn, so presses made during a load queued up and were replayed afterwards,
      // and ImGui's repeat turned a held key into the panel flickering open and shut.
      if (w == VK_F1 && !(l & (1 << 30))) g_settings_toggle.store(true);
      if (w == VK_ESCAPE && !(l & (1 << 30))) g_escape_press.store(true);   // the in-game menu, same rules as F1
      std::lock_guard<std::mutex> lock(g_keys_mutex); if (w < 256) g_keys[w] = true; return 0;
    }
    case WM_KEYUP: { std::lock_guard<std::mutex> lock(g_keys_mutex); if (w < 256) g_keys[w] = false; return 0; }
    case WM_SIZE:
      if (w != SIZE_MINIMIZED) {
        g_client_w = LOWORD(l); g_client_h = HIWORD(l);
        if (g_on_resize && g_client_w > 0 && g_client_h > 0) g_on_resize(g_client_w, g_client_h);
      }
      return 0;
    case WM_KILLFOCUS: { std::lock_guard<std::mutex> lock(g_keys_mutex); std::memset(g_keys, 0, sizeof g_keys); return 0; }
  }
  return DefWindowProcW(h, m, w, l);
}
}  // namespace

void* window_create(int w, int h, const wchar_t* title, bool visible) {
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.hInstance = inst; wc.lpfnWndProc = wnd_proc; wc.lpszClassName = L"MeleePortWindow"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  // Resource id 1 is the application icon compiled in from app/melee_unlocked.rc. hIcon is the large
  // one (Alt-Tab, the window menu) and hIconSm the 16px one in the title bar; without these the
  // window shows Windows' default application icon even though Explorer shows ours.
  wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
  wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
  wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);   // black, not white, before the first present
  RegisterClassExW(&wc);
  ds4_init_defaults();
  // Usage 0x05 is Game Pad, which is what a DualShock calls itself. A Switch Pro Controller calls
  // itself usage 0x04, Joystick, so registering only 0x05 means its reports never arrive at all.
  // Anything else on 0x04 is filtered out by device id below, as it already was on 0x05.
  RAWINPUTDEVICE rid[2]{};
  rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x05; rid[0].dwFlags = RIDEV_INPUTSINK;
  rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x04; rid[1].dwFlags = RIDEV_INPUTSINK;
  RECT r{0, 0, w, h};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  g_hwnd = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  if (!g_hwnd) die("cannot create native window");
  rid[0].hwndTarget = g_hwnd; rid[1].hwndTarget = g_hwnd;
  RegisterRawInputDevices(rid, 2, sizeof rid[0]);
  g_closed = false;
  g_client_w = w; g_client_h = h;
  if (visible) ShowWindow(g_hwnd, SW_SHOW);
  return g_hwnd;
}

namespace {
bool g_fullscreen = false;
}

void window_set_fullscreen(bool enabled) {
  static WINDOWPLACEMENT saved{sizeof(WINDOWPLACEMENT)};
  bool& fullscreen = g_fullscreen;
  if (!g_hwnd || enabled == fullscreen) return;
  if (enabled) {
    GetWindowPlacement(g_hwnd, &saved);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &info)) return;
    // Keep WS_VISIBLE: replacing the style with a bare WS_POPUP hid the window, so the desktop
    // compositor stopped showing our frames (black screen with a stale frame of the old window).
    SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(g_hwnd, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                 info.rcMonitor.right-info.rcMonitor.left, info.rcMonitor.bottom-info.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  } else {
    SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetWindowPlacement(g_hwnd, &saved);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  }
  fullscreen = enabled;
}

bool window_is_fullscreen() { return g_fullscreen; }

namespace {
std::atomic<uint64_t> g_pending_client_size{0};   // (w << 32) | h, 0 = nothing requested

// SetWindowPos on a window owned by another thread blocks until that thread pumps, so the request
// is applied from window_pump instead: PeekMessage only ever succeeds on the owning thread.
void apply_client_size(int w, int h) {
  if (!g_hwnd || g_fullscreen || w < 320 || h < 240) return;
  MONITORINFO info{sizeof(info)};
  if (GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
    // A client area larger than the monitor's work area leaves the title bar off screen with no way
    // to drag it back, so cap it at what the desktop can actually show.
    RECT frame{0, 0, w, h};
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    long max_w = info.rcWork.right - info.rcWork.left - ((frame.right - frame.left) - w);
    long max_h = info.rcWork.bottom - info.rcWork.top - ((frame.bottom - frame.top) - h);
    if (w > max_w) w = (int)max_w;
    if (h > max_h) h = (int)max_h;
  }
  if (w == g_client_w && h == g_client_h) return;
  RECT r{0, 0, w, h};
  AdjustWindowRect(&r, (DWORD)GetWindowLongPtrW(g_hwnd, GWL_STYLE), FALSE);
  SetWindowPos(g_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  // AdjustWindowRect uses the system DPI, which is not this window's DPI on a scaled display, so
  // measure what the client area actually became and correct the frame by the difference. Windows
  // also caps a resizable window at its maximum tracking size, and that cap is not ours to beat:
  // one correction, not a loop.
  RECT client{};
  if (!GetClientRect(g_hwnd, &client)) return;
  int got_w = client.right, got_h = client.bottom;
  if (got_w == w && got_h == h) return;
  RECT frame{};
  if (!GetWindowRect(g_hwnd, &frame)) return;
  SetWindowPos(g_hwnd, nullptr, 0, 0, (frame.right - frame.left) + (w - got_w), (frame.bottom - frame.top) + (h - got_h),
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
}  // namespace

// The resolution picker in the PC settings panel. WM_SIZE then reaches the resize callback, which
// recreates the swapchain buffers and the EFB, so the new size applies without a restart.
void window_set_client_size(int w, int h) {
  if (w < 320 || h < 240) return;
  g_pending_client_size.store(((uint64_t)(uint32_t)w << 32) | (uint32_t)h);
}

static double query_refresh_rate();

// The settings panel asks for this on every frame it draws the Video tab (the default frame rate
// follows the monitor), and the answer comes from the display driver through the kernel, which some
// drivers make slow. A monitor's rate does not change from one frame to the next: ask once a second.
double window_refresh_rate() {
  static std::atomic<double> cached{0.0};
  static std::atomic<int64_t> cached_at{0};
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
  const double value = cached.load(std::memory_order_relaxed);
  if (value > 0.0 && now.QuadPart - cached_at.load(std::memory_order_relaxed) < freq.QuadPart) return value;
  const double fresh = query_refresh_rate();
  cached.store(fresh, std::memory_order_relaxed);
  cached_at.store(now.QuadPart, std::memory_order_relaxed);
  return fresh;
}

static double query_refresh_rate() {
  MONITORINFOEXW monitor{}; monitor.cbSize = sizeof(monitor);
  if (!GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return 60.0;
  // DisplayConfig retains rational rates (e.g. 60000/1001) that DEVMODE rounds.
  for (int attempt = 0; attempt < 3; ++attempt) {
    UINT32 paths_count = 0, modes_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths_count, &modes_count) != ERROR_SUCCESS) break;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(paths_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modes_count);
    LONG status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths_count, paths.data(), &modes_count, modes.data(), nullptr);
    if (status == ERROR_INSUFFICIENT_BUFFER) continue;
    if (status != ERROR_SUCCESS) break;
    for (UINT32 i = 0; i < paths_count; ++i) {
      const auto& path = paths[i];
      DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
      source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      source.header.size = sizeof(source); source.header.adapterId = path.sourceInfo.adapterId;
      source.header.id = path.sourceInfo.id;
      if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || wcscmp(source.viewGdiDeviceName, monitor.szDevice)) continue;
      const auto rate = path.targetInfo.refreshRate;
      if (rate.Numerator && rate.Denominator) return double(rate.Numerator) / rate.Denominator;
    }
    break;
  }
  DEVMODEW mode{}; mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) return mode.dmDisplayFrequency;
  return 60.0;
}

void window_set_message_callback(MessageCallback cb) { g_on_message = std::move(cb); }
// The pointer is hidden over the game and shown while a menu has input. Windows only asks for the
// cursor again when the mouse moves, so opening the settings or the Esc menu with the mouse still
// left it invisible; a change now tells the window thread to set it straight away.
constexpr UINT kCursorRefresh = WM_APP + 7;
void window_input_capture(bool capture) {
  if (g_ui_capture.exchange(capture) != capture && g_hwnd) PostMessageW(g_hwnd, kCursorRefresh, capture ? 1 : 0, 0);
}
bool window_ui_gamecube_pad(PadState& pad) { std::lock_guard<std::mutex> lock(g_ui_pad_mutex); pad = g_ui_pad; return g_ui_gamecube; }
bool window_ui_pads(PadState pads[4]) {
  std::lock_guard<std::mutex> lock(g_ui_pad_mutex);
  for (int i = 0; i < 4; ++i) pads[i] = g_ui_pads[i];
  return g_ui_has_pad;
}
void window_set_resize_callback(ResizeCallback cb) { g_on_resize = std::move(cb); }
bool window_take_fullscreen_toggle() { return g_fullscreen_toggle.exchange(false); }
// Several presses while nothing was being drawn count as one: the player pressed F1 again because
// nothing seemed to happen, and wants the panel, not an even number of toggles.
bool window_take_settings_toggle() { return g_settings_toggle.exchange(false); }
bool window_take_practice_toggle() { return g_practice_toggle.exchange(false); }
bool window_take_escape() { return g_escape_press.exchange(false); }

void window_destroy() { if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; } }

void window_pump() {
  if (uint64_t want = g_pending_client_size.exchange(0))
    apply_client_size((int)(want >> 32), (int)(want & 0xFFFFFFFFu));
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

void window_set_title(const wchar_t* title) { if (g_hwnd) SetWindowTextW(g_hwnd, title); }
bool window_closed() { return g_closed; }
void window_client_size(int* w, int* h) { *w = g_client_w; *h = g_client_h; }

// GameCube button bits (PADStatus.button)
enum : uint16_t {
  PAD_LEFT = 0x0001, PAD_RIGHT = 0x0002, PAD_DOWN = 0x0004, PAD_UP = 0x0008, PAD_Z = 0x0010, PAD_R = 0x0020, PAD_L = 0x0040,
  PAD_A = 0x0100, PAD_B = 0x0200, PAD_X = 0x0400, PAD_Y = 0x0800, PAD_START = 0x1000,
};

KeyBindings g_key_bindings = default_key_bindings();
std::array<PadBindings, 4> g_pad_bindings = default_pad_bindings();
std::array<GCBindings, 4> g_gc_bindings = default_gc_bindings();
std::array<PadBindings, 4> g_ds4_bindings = {};
// Unlike the DS4 table above, this one takes its defaults here rather than in window_create, so a
// binding read out of port-settings.ini (which is loaded before the window exists) is not written
// over by the defaults a moment later.
std::array<PadBindings, 4> g_swpro_bindings = default_swpro_bindings();
std::array<HidBindings, 4> g_hid_bindings = default_hid_bindings();
std::array<Deadzone, (size_t)PadFamily::Count> g_deadzones{};
std::array<PortSource, 4> g_port_sources = default_port_sources();
std::array<std::string, 4> g_port_device_names;
// The device that actually fed each port on the last poll. Usually its port source; but a port whose
// source is missing (say "GC Adapter 1" while a program like Delfinovin holds the adapter and offers
// it as an Xbox pad) falls back to the first free pad, and rumble has to go where the input came from.
std::array<PortSource, 4> g_port_feeding{};
// The last state the game actually read, for the on-screen controller overlay. Taken here rather
// than polled again by the renderer, so the overlay shows what the game saw and polling the devices
// stays on one thread at one rate.
std::mutex g_last_pads_mutex;
PadState g_last_pads[4]{};

namespace {
std::atomic<bool> g_capturing{false};
bool g_capture_key_baseline[256]{};
unsigned short g_capture_pad_baseline[4]{};
PadState g_capture_gc_baseline[4]{};
uint16_t g_capture_ds4_baseline[4]{};
uint16_t g_capture_swpro_baseline[4]{};
uint32_t g_capture_hid_baseline[4]{};
HANDLE g_ds4_devices[4]{};
uint16_t g_ds4_buttons[4]{};
PadState g_ds4_pads[4]{};
std::mutex g_ds4_mutex;

constexpr uint16_t ds4_action_mask(BindAction action) {
  switch (action) {
    case BindAction::A: return DS4_CROSS;
    case BindAction::B: return DS4_CIRCLE;
    case BindAction::X: return DS4_SQUARE;
    case BindAction::Y: return DS4_TRIANGLE;
    case BindAction::Start: return DS4_OPTIONS;
    case BindAction::L: return DS4_L1;
    case BindAction::R: return DS4_R1;
    case BindAction::Z: return DS4_R2;
    case BindAction::DUp: return DS4_DPAD_UP;
    case BindAction::DDown: return DS4_DPAD_DOWN;
    case BindAction::DLeft: return DS4_DPAD_LEFT;
    case BindAction::DRight: return DS4_DPAD_RIGHT;
    default: return 0;
  }
}

void ds4_init_defaults() {
  for (auto& bindings : g_ds4_bindings)
    for (int i = 0; i < (int)BindAction::Count; ++i) bindings.mask[i] = ds4_action_mask((BindAction)i);
}

int ds4_slot(HANDLE device) {
  for (int i = 0; i < 4; ++i) if (g_ds4_devices[i] == device) return i;
  for (int i = 0; i < 4; ++i) if (!g_ds4_devices[i]) { g_ds4_devices[i] = device; return i; }
  return -1;
}

bool ds4_device(HANDLE device, bool* dualsense) {
  RID_DEVICE_INFO info{}; info.cbSize = sizeof info;
  UINT size = sizeof info;
  if (GetRawInputDeviceInfoW(device, RIDI_DEVICEINFO, &info, &size) == (UINT)-1 || info.dwType != RIM_TYPEHID) return false;
  *dualsense = dualsense_pad(info.hid.dwVendorId, info.hid.dwProductId);
  return playstation_pad(info.hid.dwVendorId, info.hid.dwProductId);
}

// The decoding lives in playstation_pad.cpp, where it is tested byte by byte (port/tests).
void ds4_report(RAWINPUT* input, bool dualsense) {
  int slot = ds4_slot(input->header.hDevice);
  if (slot < 0 || !input->data.hid.dwSizeHid || !input->data.hid.dwCount) return;
  PadState pad{};
  uint16_t buttons = 0;
  if (!playstation_parse(input->data.hid.bRawData, input->data.hid.dwSizeHid, dualsense, pad, buttons)) return;
  std::lock_guard<std::mutex> lock(g_ds4_mutex); g_ds4_buttons[slot] = buttons; g_ds4_pads[slot] = pad;
}

// One WM_INPUT report, handed to whichever device path recognises it. Devices we do not know about
// (an Xbox pad, which is read through XInput instead, or a flight stick on usage 0x04) fall out of
// both and cost only the fetch.
void raw_input(HRAWINPUT raw) {
  UINT size = 0;
  if (GetRawInputData(raw, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1 || !size) return;
  std::vector<uint8_t> bytes(size);
  if (GetRawInputData(raw, RID_INPUT, bytes.data(), &size, sizeof(RAWINPUTHEADER)) != size) return;
  RAWINPUT* input = reinterpret_cast<RAWINPUT*>(bytes.data());
  if (input->header.dwType != RIM_TYPEHID) return;
  if (bool dualsense = false; ds4_device(input->header.hDevice, &dualsense)) { ds4_report(input, dualsense); return; }
  if (switchpro_raw_input(input->header.hDevice, input->data.hid.bRawData,
                          input->data.hid.dwSizeHid, input->data.hid.dwCount)) return;
  // Anything else that calls itself a gamepad: a B0XX, a Frame1, a vJoy feeder, a third-party pad.
  // It goes last so a device with a reader of its own is never taken by the generic path.
  hidpad_raw_input(input->header.hDevice, input->data.hid.bRawData,
                   input->data.hid.dwSizeHid, input->data.hid.dwCount);
}
}  // namespace

namespace { CaptureDevice g_capture_want = CaptureDevice::None; int g_capture_want_index = 0; }
void input_begin_capture(CaptureDevice want, int want_index) {
  g_capture_want = want; g_capture_want_index = want_index;
  input_begin_capture();
}
void input_begin_capture() {
  { std::lock_guard<std::mutex> lock(g_keys_mutex); std::memcpy(g_capture_key_baseline, g_keys, sizeof g_keys); }
  // gcadapter_poll fills four pads. It used to be handed the address of a single PadState, once per
  // iteration, so with an adapter connected it wrote three pads past the end of a stack variable
  // every time the rebinding UI opened, and every port ended up with port 1's baseline. Poll once,
  // into four.
  PadState gc[4]{};
  for (int i = 0; i < 4; ++i) gc[i].err = -1;
  gcadapter_poll(gc);
  for (int idx = 0; idx < 4; ++idx) {
    XINPUT_STATE xs{};
    g_capture_pad_baseline[idx] = (XInputGetState(idx, &xs) == ERROR_SUCCESS) ? xs.Gamepad.wButtons : 0;
    g_capture_gc_baseline[idx] = gc[idx];
  }
  { std::lock_guard<std::mutex> lock(g_ds4_mutex); for (int idx = 0; idx < 4; ++idx) g_capture_ds4_baseline[idx] = g_ds4_buttons[idx]; }
  {
    PadState swpro[4]; for (auto& s : swpro) { s = {}; s.err = -1; }
    uint16_t buttons[4]{};
    switchpro_poll(swpro, buttons);
    for (int idx = 0; idx < 4; ++idx) g_capture_swpro_baseline[idx] = buttons[idx];
  }
  {
    PadState hid[4]; for (auto& s : hid) { s = {}; s.err = -1; }
    uint32_t buttons[4]{};
    hidpad_poll(hid, buttons);
    for (int idx = 0; idx < 4; ++idx) g_capture_hid_baseline[idx] = buttons[idx];
  }
  g_capturing.store(true);
}

void input_cancel_capture() { g_capturing.store(false); }

bool input_poll_capture(CaptureDevice& device, int& value, int& device_index) {
  if (!g_capturing.load()) return false;
  {
    std::lock_guard<std::mutex> lock(g_keys_mutex);
    if (g_keys[VK_ESCAPE] && !g_capture_key_baseline[VK_ESCAPE]) {
      g_capturing.store(false); device = CaptureDevice::None; value = 0; device_index = 0; return true;
    }
    const bool want_keyboard = g_capture_want == CaptureDevice::None || g_capture_want == CaptureDevice::Keyboard;
    for (int vk = 0; vk < 256 && want_keyboard; ++vk) {
      if (vk == VK_ESCAPE) continue;
      if (g_keys[vk] && !g_capture_key_baseline[vk]) {
        g_capturing.store(false); device = CaptureDevice::Keyboard; value = vk; device_index = 0; return true;
      }
    }
  }
  // Only controllers of the kind being rebound are listened to (see input_begin_capture), but any
  // one of them: a player does not know which adapter socket or pad number theirs is, and the
  // settings panel moves to whichever one answered. `wanted` with no device named keeps the old
  // behaviour of taking whatever fires first.
  auto wanted = [](CaptureDevice kind, int) {
    return g_capture_want == CaptureDevice::None || g_capture_want == kind;
  };
  for (int idx = 0; idx < 4; ++idx) {
    if (!wanted(CaptureDevice::XInputPad, idx)) continue;
    XINPUT_STATE xs{};
    if (XInputGetState(idx, &xs) != ERROR_SUCCESS) continue;
    unsigned short newly = xs.Gamepad.wButtons & ~g_capture_pad_baseline[idx];
    if (newly) {
      unsigned short lowest = newly & (~(newly - 1));
      g_capturing.store(false); device = CaptureDevice::XInputPad; value = lowest; device_index = idx; return true;
    }
  }
  PadState gc[4];
  uint32_t mask = gcadapter_poll(gc);
  for (int idx = 0; idx < 4; ++idx) {
    if (!(mask & (1u << idx)) || !wanted(CaptureDevice::GCAdapter, idx)) continue;
    uint16_t newly = gc[idx].button & ~g_capture_gc_baseline[idx].button;
    // A trigger pressed most of the way counts, not only one clicked all the way in.
    constexpr int kTriggerPress = 140;
    if (gc[idx].trig_l >= kTriggerPress && g_capture_gc_baseline[idx].trig_l < kTriggerPress) newly |= PAD_L;
    if (gc[idx].trig_r >= kTriggerPress && g_capture_gc_baseline[idx].trig_r < kTriggerPress) newly |= PAD_R;
    if (newly) {
      unsigned short lowest = newly & (~(newly - 1));
      g_capturing.store(false); device = CaptureDevice::GCAdapter; value = lowest; device_index = idx; return true;
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_ds4_mutex);
    for (int idx = 0; idx < 4; ++idx) {
      if (!wanted(CaptureDevice::DS4Pad, idx)) continue;
      uint16_t newly = g_ds4_buttons[idx] & ~g_capture_ds4_baseline[idx];
      if (newly) {
        uint16_t lowest = newly & (uint16_t)(~(newly - 1));
        g_capturing.store(false); device = CaptureDevice::DS4Pad; value = lowest; device_index = idx; return true;
      }
    }
  }
  {
    PadState swpro[4]; for (auto& s : swpro) { s = {}; s.err = -1; }
    uint16_t buttons[4]{};
    uint32_t mask = switchpro_poll(swpro, buttons);
    for (int idx = 0; idx < 4; ++idx) {
      if (!(mask & (1u << idx)) || !wanted(CaptureDevice::SwitchPro, idx)) continue;
      uint16_t newly = buttons[idx] & ~g_capture_swpro_baseline[idx];
      if (newly) {
        uint16_t lowest = newly & (uint16_t)(~(newly - 1));
        g_capturing.store(false); device = CaptureDevice::SwitchPro; value = lowest; device_index = idx; return true;
      }
    }
  }
  {
    PadState hid[4]; for (auto& s : hid) { s = {}; s.err = -1; }
    uint32_t buttons[4]{};
    uint32_t mask = hidpad_poll(hid, buttons);
    for (int idx = 0; idx < 4; ++idx) {
      if (!(mask & (1u << idx)) || !wanted(CaptureDevice::HidPad, idx)) continue;
      // Bit 31 is left out: the capture result travels as an int, and a mask with the top bit set
      // would arrive negative. Thirty-one buttons is more than any box controller has.
      uint32_t newly = buttons[idx] & ~g_capture_hid_baseline[idx] & 0x7FFFFFFFu;
      if (newly) {
        uint32_t lowest = newly & ~(newly - 1);
        g_capturing.store(false); device = CaptureDevice::HidPad; value = (int)lowest; device_index = idx; return true;
      }
    }
  }
  return false;
}

namespace {
// tl/tr are the analog triggers. Melee shields from the analog value, not the digital L/R bit, so a
// script that only pressed L never actually shielded and shield behaviour could not be tested at all.
struct ScriptEntry { uint32_t frame; uint16_t buttons; int8_t sx, sy, cx, cy; uint8_t tl, tr; int port;  bool relative = false; };
std::vector<ScriptEntry> g_script;
uint32_t g_script_ports = 1;
// `@match` makes later entries relative to the retrace at which an online match reached frame 1
// (they stay silent until then); `@loop N` repeats the relative section every N frames.
static bool g_script_relative_section = false;
static uint32_t g_script_loop = 0;
static std::atomic<uint32_t> g_match_start_retrace{0};

// Turns a Switch controller's raw SWPRO_* bits into GameCube buttons through slot `idx`'s
// remappable table, and returns the same thing as BindAction bit indices for the settings panel.
// The L/R shoulders are digital on this pad, so a press bottoms the analog trigger out the way a
// digital press does on hardware: Melee shields from the trigger value, not from the L/R bit.
uint16_t hid_apply_bindings(int idx, uint32_t buttons, PadState& pad) {
  uint32_t actions = 0;
  for (int i = 0; i < (int)BindAction::Count; ++i) {
    if (!(g_hid_bindings[idx].mask[i] & buttons)) continue;
    pad.button |= kActionPadBit[i];
    actions |= (uint32_t)(1u << i);
  }
  // A box has no analog triggers: its L and R are buttons, and the game needs a full press to see
  // a shield. Light shield on such a device is a firmware feature, reported on an analog axis when
  // the device has one, which the axis path above has already put into trig_l/trig_r.
  if (pad.button & kActionPadBit[(size_t)BindAction::L] && !pad.trig_l) pad.trig_l = 255;
  if (pad.button & kActionPadBit[(size_t)BindAction::R] && !pad.trig_r) pad.trig_r = 255;
  apply_cstick_actions(actions, pad.sub_x, pad.sub_y);
  return actions;
}

uint16_t swpro_apply_bindings(int idx, uint16_t buttons, PadState& pad) {
  uint32_t actions = 0;
  for (int i = 0; i < (int)BindAction::Count; ++i) {
    if (!(g_swpro_bindings[idx].mask[i] & buttons)) continue;
    pad.button |= kActionPadBit[i];
    actions |= (uint32_t)(1u << i);
  }
  if (pad.button & kActionPadBit[(size_t)BindAction::L]) pad.trig_l = 255;
  if (pad.button & kActionPadBit[(size_t)BindAction::R]) pad.trig_r = 255;
  apply_cstick_actions(actions, pad.sub_x, pad.sub_y);
  return actions;
}

// A box whose layout is known gets it the first time it appears in a slot, as long as that slot
// still has the generic defaults (anything the player changed is kept). vJoy: the b0xx-ahk layout,
// which most B0XX-on-vJoy setups use. "Arduino Leonardo": a HayBox box in DInput mode.
void apply_known_box_layout(int idx) {
  static std::string seen[4];
  const std::string name = hidpad_name(idx);
  if (name == seen[idx]) return;
  seen[idx] = name;
  std::string lower = name;
  for (char& ch : lower) ch = (char)std::tolower((unsigned char)ch);
  const HidBindings generic = default_hid_bindings()[0];
  if (std::memcmp(&g_hid_bindings[idx], &generic, sizeof generic) != 0) return;
  if (lower.find("vjoy") != std::string::npos) {
    g_hid_bindings[idx] = vjoy_b0xx_bindings();
    host::log("hid pad %d: %s, using the B0XX (vJoy) layout", idx + 1, name.c_str());
  } else if (lower.find("leonardo") != std::string::npos || lower.find("haybox") != std::string::npos) {
    g_hid_bindings[idx] = haybox_dinput_bindings();
    host::log("hid pad %d: %s, using the B0XX-layout box (HayBox DInput) layout", idx + 1, name.c_str());
  }
}

// Every family's binding table the same way: `pressed(i)` says whether action i's binding is down.
// Returns the actions as BindAction bits for the settings panel.
template <class Pressed> uint32_t apply_actions(PadState& pad, Pressed pressed) {
  uint32_t actions = 0;
  for (int i = 0; i < (int)BindAction::Count; ++i) {
    if (!pressed(i)) continue;
    actions |= (uint32_t)(1u << i);
    pad.button |= kActionPadBit[i];
  }
  apply_cstick_actions(actions, pad.sub_x, pad.sub_y);
  return actions;
}

// The adapter reports GameCube buttons already; its table remaps them (the default is the identity).
// The analog triggers stay with the physical triggers; a digital L or R arriving from another button
// bottoms its trigger out, as a box's L/R do, since the game shields from the analog value.
uint16_t gc_apply_bindings(int idx, PadState& pad) {
  const uint16_t raw = pad.button;
  pad.button = 0;
  const uint32_t actions = apply_actions(pad, [&](int i) { const uint16_t m = g_gc_bindings[idx].mask[i]; return m && (raw & m); });
  if (pad.button & PAD_L && !pad.trig_l) pad.trig_l = 255;
  if (pad.button & PAD_R && !pad.trig_r) pad.trig_r = 255;
  return actions;
}

void apply_family_options(PadFamily family, PadState& pad) {
  const Deadzone& dz = g_deadzones[(size_t)family];
  apply_deadzone(dz, pad.stick_x, pad.stick_y, false);
  apply_deadzone(dz, pad.sub_x, pad.sub_y, true);
}
}  // namespace
void input_mark_match_start() { g_match_start_retrace.store(retrace_count()); }

bool input_load_script(const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) return false;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    ScriptEntry e{};
    char* p = line;
    if (*p == '#' || *p == '\n' || *p == '\r') continue;
    if (!strncmp(p, "@match", 6)) { g_script_relative_section = true; continue; }
    if (!strncmp(p, "@loop", 5)) { g_script_loop = (uint32_t)strtoul(p + 5, nullptr, 10); continue; }
    e.relative = g_script_relative_section;
    e.frame = (uint32_t)strtoul(p, &p, 10);
    while (*p) {
      while (*p == ' ' || *p == '\t') ++p;
      if (!*p || *p == '\n' || *p == '\r' || *p == '#') break;
      char tok[32]; int n = 0;
      while (*p && *p != ' ' && *p != '+' && *p != '\n' && *p != '\r' && n < 31) tok[n++] = *p++;
      tok[n] = 0;
      if (*p == '+') ++p;
      if (!strcmp(tok, "A")) e.buttons |= PAD_A; else if (!strcmp(tok, "B")) e.buttons |= PAD_B;
      else if (!strcmp(tok, "X")) e.buttons |= PAD_X; else if (!strcmp(tok, "Y")) e.buttons |= PAD_Y;
      else if (!strcmp(tok, "Z")) e.buttons |= PAD_Z; else if (!strcmp(tok, "L")) e.buttons |= PAD_L;
      else if (!strcmp(tok, "R")) e.buttons |= PAD_R; else if (!strcmp(tok, "START")) e.buttons |= PAD_START;
      else if (!strncmp(tok, "l=", 2)) e.tl = (uint8_t)std::min(255, std::max(0, atoi(tok + 2)));
      else if (!strncmp(tok, "r=", 2)) e.tr = (uint8_t)std::min(255, std::max(0, atoi(tok + 2)));
      else if (!strcmp(tok, "DU")) e.buttons |= PAD_UP; else if (!strcmp(tok, "DD")) e.buttons |= PAD_DOWN;
      else if (!strcmp(tok, "DL")) e.buttons |= PAD_LEFT; else if (!strcmp(tok, "DR")) e.buttons |= PAD_RIGHT;
      else if (!strncmp(tok, "sx=", 3)) e.sx = (int8_t)atoi(tok + 3); else if (!strncmp(tok, "sy=", 3)) e.sy = (int8_t)atoi(tok + 3);
      else if (!strncmp(tok, "cx=", 3)) e.cx = (int8_t)atoi(tok + 3); else if (!strncmp(tok, "cy=", 3)) e.cy = (int8_t)atoi(tok + 3);
      else if (!strncmp(tok, "p=", 2)) { e.port = atoi(tok + 2) - 1; if (e.port < 0 || e.port > 3) e.port = 0; g_script_ports |= 1u << e.port; }
    }
    g_script.push_back(e);
  }
  fclose(f);
  return !g_script.empty();
}

void input_poll(PadState out[4]) {
  struct UiSnapshot {
    PadState* pads; bool gamecube = false;
    ~UiSnapshot() {
      std::lock_guard<std::mutex> lock(g_ui_pad_mutex);
      g_ui_pad = pads[0];
      g_ui_gamecube = gamecube;
      g_ui_has_pad = false;
      for (int i = 0; i < 4; ++i) {
        g_ui_pads[i] = pads[i];
        if (pads[i].err == 0) g_ui_has_pad = true;
      }
      if (g_ui_capture.load()) {
        // UI capture is global, not player-one-only. Keeping ports 2-4 live made menu navigation
        // leak into Training and, during handoff, into the first online gameplay inputs.
        for (int i = 0; i < 4; ++i) {
          const int8_t err = pads[i].err;
          pads[i] = {};
          pads[i].err = err;
        }
      }
    }
  } ui{out};
  for (int i = 0; i < 4; ++i) { std::memset(&out[i], 0, sizeof out[i]); out[i].err = -1; }
  if (!g_script.empty()) {
    // Scripts drive port 1 by default; entries with p=N drive port N (a port with any entry counts as plugged in).
    uint32_t frame = retrace_count();
    uint32_t start = g_match_start_retrace.load();
    bool in_match = start && frame >= start;
    uint32_t rel = in_match ? frame - start : 0;
    if (in_match && g_script_loop) rel %= g_script_loop;
    for (int port = 0; port < 4; ++port) {
      if (port && !(g_script_ports & (1u << port))) continue;
      out[port].err = 0;
      const ScriptEntry* cur = nullptr;
      for (const ScriptEntry& e : g_script) {
        if (e.port != port) continue;
        if (e.relative) { if (in_match && e.frame <= rel) cur = &e; }
        else if (!in_match && e.frame <= frame) cur = &e;
      }
      if (cur) {
        PadState& q = out[port];
        q.button = cur->buttons; q.stick_x = cur->sx; q.stick_y = cur->sy; q.sub_x = cur->cx; q.sub_y = cur->cy;
        // A digital L/R press on hardware bottoms the trigger out, so mirror that when the script
        // did not ask for a specific analog value (light shield needs the explicit l=/r= token).
        q.trig_l = cur->tl ? cur->tl : (uint8_t)((cur->buttons & PAD_L) ? 255 : 0);
        q.trig_r = cur->tr ? cur->tr : (uint8_t)((cur->buttons & PAD_R) ? 255 : 0);
      }
    }
    // Scripted runs took this return before the overlay's copy was made below, so the controller
    // overlay stayed blank whenever a script was driving. It is the only way to see the overlay
    // without a controller in hand, so keep it fed here too.
    { std::lock_guard<std::mutex> lock(g_last_pads_mutex); for (int p = 0; p < 4; ++p) g_last_pads[p] = out[p]; }
    return;
  }
  // Poll every physical source unconditionally, then route each in-game port to
  // whichever device g_port_sources[port] assigns it to. This lets keyboard, an
  // Xbox pad, and the GC adapter all drive different ports at the same time.
  PadState gc[4];
  for (auto& s : gc) { s = {}; s.err = -1; }
  uint32_t gc_mask = gcadapter_poll(gc);
  ui.gamecube = gc_mask != 0;

  InputDebugSnapshot debug{};
  debug.gc_mask = gc_mask;

  // Whether the game window has focus: without it, input follows the Background input option.
  const bool focused = g_hwnd && GetForegroundWindow() == g_hwnd;
  PadState kb{}; kb.err = 0;
  uint32_t keyboard_actions = 0;
  {
    // Keyboard: every action, the control stick and C-stick included, comes from g_key_bindings.
    // The stick used to be hard wired to the arrow keys here while everything else was a setting,
    // which is the one thing players could not rebind; the arrows are now just its defaults.
    // Key messages only reach a focused window, so in the background the keys are read directly.
    std::lock_guard<std::mutex> lock(g_keys_mutex);
    auto key = [&](int vk) {
      if (focused) return g_keys[vk & 0xFF];
      return g_background_input && (GetAsyncKeyState(vk & 0xFF) & 0x8000) != 0;
    };
    keyboard_actions = apply_actions(kb, [&](int i) { const int vk = g_key_bindings.vk[i]; return vk && key(vk); });
    apply_stick_actions(keyboard_actions, kb.stick_x, kb.stick_y);
    if (kb.button & PAD_L) kb.trig_l = 255;
    if (kb.button & PAD_R) kb.trig_r = 255;
  }

  PadState xin[4];
  bool xin_connected[4] = {};
  for (int idx = 0; idx < 4; ++idx) {
    PadState& x = xin[idx]; x = {}; x.err = -1;
    XINPUT_STATE xs{};
    if (XInputGetState(idx, &xs) != ERROR_SUCCESS) continue;
    xin_connected[idx] = true;
    debug.xinput_connected[idx] = true;
    x.err = 0;
    auto& g = xs.Gamepad;
    auto axis = [](SHORT v) { int a = v / 258; return a > 127 ? 127 : a < -127 ? -127 : a; };
    // No deadzone and no trigger floor, as in Dolphin. These used Microsoft's recommended Xbox
    // deadzone, which zeroed both axes whenever both sat inside about 30 of Melee's 80 units: larger
    // than the game's own deadzone, and exactly where a box controller in XInput mode puts some of its
    // modifier coordinates, which then did nothing. The game already ignores a resting stick.
    const int sx = axis(g.sThumbLX), sy = axis(g.sThumbLY), cx = axis(g.sThumbRX), cy = axis(g.sThumbRY);
    x.stick_x = (int8_t)sx; x.stick_y = (int8_t)sy; x.sub_x = (int8_t)cx; x.sub_y = (int8_t)cy;
    debug.xinput_actions[idx] = apply_actions(x, [&](int i) { const unsigned short m = g_pad_bindings[idx].mask[i]; return m && (g.wButtons & m); });
    x.trig_l = g.bLeftTrigger; if (g.bLeftTrigger > 200) { x.button |= PAD_L; debug.xinput_actions[idx] |= 1u << (int)BindAction::L; }
    x.trig_r = g.bRightTrigger; if (g.bRightTrigger > 200) { x.button |= PAD_R; debug.xinput_actions[idx] |= 1u << (int)BindAction::R; }
  }

  PadState ds4[4]{};
  uint16_t ds4_buttons[4]{};
  bool ds4_connected[4]{};
  {
    std::lock_guard<std::mutex> lock(g_ds4_mutex);
    for (int idx = 0; idx < 4; ++idx) {
      ds4_buttons[idx] = g_ds4_buttons[idx]; ds4[idx] = g_ds4_pads[idx];
      ds4_connected[idx] = g_ds4_devices[idx] != nullptr;
      ds4[idx].err = ds4_connected[idx] ? 0 : -1;
      if (!ds4_connected[idx]) continue;
      debug.ds4_actions[idx] = apply_actions(ds4[idx], [&](int i) { return (g_ds4_bindings[idx].mask[i] & ds4_buttons[idx]) != 0; });
      debug.ds4_connected[idx] = true;
    }
  }

  PadState swpro[4];
  bool swpro_connected[4]{};
  {
    for (auto& s : swpro) { s = {}; s.err = -1; }
    uint16_t buttons[4]{};
    uint32_t swpro_mask = switchpro_poll(swpro, buttons);
    for (int idx = 0; idx < 4; ++idx) {
      if (!(swpro_mask & (1u << idx))) { swpro[idx] = {}; swpro[idx].err = -1; continue; }
      swpro_connected[idx] = true;
      debug.swpro_connected[idx] = true;
      debug.swpro_actions[idx] = swpro_apply_bindings(idx, buttons[idx], swpro[idx]);
    }
  }

  PadState hid[4];
  bool hid_connected[4]{};
  {
    for (auto& s : hid) { s = {}; s.err = -1; }
    uint32_t buttons[4]{};
    uint32_t hid_mask = hidpad_poll(hid, buttons);
    for (int idx = 0; idx < 4; ++idx) {
      if (!(hid_mask & (1u << idx))) { hid[idx] = {}; hid[idx].err = -1; continue; }
      apply_known_box_layout(idx);
      hid_connected[idx] = true;
      debug.hid_connected[idx] = true;
      debug.hid_actions[idx] = hid_apply_bindings(idx, buttons[idx], hid[idx]);
    }
  }

  for (int idx = 0; idx < 4; ++idx)
    if (gc_mask & (1u << idx)) debug.gc_actions[idx] = gc_apply_bindings(idx, gc[idx]);

  // Per-family options (deadzones). Before routing, so the port overlay and everything downstream
  // see what the game gets.
  for (int idx = 0; idx < 4; ++idx) {
    if (gc_mask & (1u << idx)) apply_family_options(PadFamily::GameCube, gc[idx]);
    if (xin_connected[idx]) apply_family_options(PadFamily::Xbox, xin[idx]);
    if (ds4_connected[idx]) apply_family_options(PadFamily::PlayStation, ds4[idx]);
    if (swpro_connected[idx]) apply_family_options(PadFamily::Switch, swpro[idx]);
    if (hid_connected[idx]) apply_family_options(PadFamily::Box, hid[idx]);
  }

  // 0.1.7 drove port 1 from the keyboard and the first pad together. The port-source table replaced
  // that with the keyboard alone, so a lone Xbox pad landed on port 2 and a DS4 on no port at all,
  // and players who had been port 1 reported their controller had stopped working. Keep the table,
  // but let the first unrouted pad also drive a port still on the default keyboard source.
  auto routed = [&](DeviceKind kind, int index) {
    for (int q = 0; q < 4; ++q) if (g_port_sources[q].kind == kind && g_port_sources[q].index == index) return true;
    return false;
  };
  auto keyboard_and_pad = [&](int port) {
    PadState result = kb;
    const PadState* pad = nullptr;
    PortSource feeding{DeviceKind::Keyboard, 0};
    for (int i = 0; i < 4 && !pad; ++i) if (xin_connected[i] && !routed(DeviceKind::XInputPad, i)) { pad = &xin[i]; feeding = {DeviceKind::XInputPad, i}; }
    for (int i = 0; i < 4 && !pad; ++i) if (ds4_connected[i] && !routed(DeviceKind::DS4Pad, i)) { pad = &ds4[i]; feeding = {DeviceKind::DS4Pad, i}; }
    for (int i = 0; i < 4 && !pad; ++i) if (swpro_connected[i] && !routed(DeviceKind::SwitchPro, i)) { pad = &swpro[i]; feeding = {DeviceKind::SwitchPro, i}; }
    for (int i = 0; i < 4 && !pad; ++i) if (hid_connected[i] && !routed(DeviceKind::HidPad, i)) { pad = &hid[i]; feeding = {DeviceKind::HidPad, i}; }
    g_port_feeding[port] = feeding;
    // The keyboard keeps working; the pad takes over whenever it is actually being used. "Used" means
    // past the game's own deadzone (0.2875 of 80), not merely non-zero: pad values carry no deadzone
    // now, and a worn stick resting a few units off centre would otherwise lock the keyboard out.
    auto moved = [](int8_t v) { return v > 23 || v < -23; };
    if (pad && (pad->button || moved(pad->stick_x) || moved(pad->stick_y) || moved(pad->sub_x) || moved(pad->sub_y) ||
                pad->trig_l > 20 || pad->trig_r > 20)) result = *pad;
    return result;
  };
  // A port given a named box follows that box to whatever HID slot it is in this time.
  for (int port = 0; port < 4; ++port) {
    PortSource& src = g_port_sources[port];
    const std::string& want = g_port_device_names[port];
    if (src.kind != DeviceKind::HidPad || want.empty()) continue;
    const bool here = src.index >= 0 && src.index < 4 && hid_connected[src.index] && port_device_key(hidpad_name(src.index)) == want;
    if (here) continue;
    for (int i = 0; i < 4; ++i) {
      if (!hid_connected[i] || port_device_key(hidpad_name(i)) != want) continue;
      log("controls: port %d follows %s to HID slot %d (it was listed as slot %d)", port + 1, want.c_str(), i + 1, src.index + 1);
      src.index = i;
      break;
    }
  }
  for (int port = 0; port < 4; ++port) {
    const PortSource& src = g_port_sources[port];
    g_port_feeding[port] = src;   // replaced below when the port falls back to another device
    switch (src.kind) {
      case DeviceKind::Keyboard: out[port] = keyboard_and_pad(port); break;
      case DeviceKind::XInputPad:
        if (src.index >= 0 && src.index < 4 && xin_connected[src.index]) out[port] = xin[src.index];
        break;
      case DeviceKind::DS4Pad:
        if (src.index >= 0 && src.index < 4 && ds4_connected[src.index]) out[port] = ds4[src.index];
        break;
      case DeviceKind::SwitchPro:
        if (src.index >= 0 && src.index < 4 && swpro_connected[src.index]) out[port] = swpro[src.index];
        break;
      case DeviceKind::HidPad:
        if (src.index >= 0 && src.index < 4 && hid_connected[src.index]) out[port] = hid[src.index];
        break;
      case DeviceKind::GCAdapter:
        if (src.index >= 0 && src.index < 4 && (gc_mask & (1u << src.index))) out[port] = gc[src.index];
        // Nothing in that adapter socket: port 1 falls back to the keyboard and the first unrouted
        // pad, so a player without an adapter is still player 1.
        else if (port == 0) out[port] = keyboard_and_pad(port);
        break;
      case DeviceKind::None: default: break;
    }
    debug.ports[port] = out[port];
  }
  // Background input off and another window in front: every port stays plugged in but neutral.
  if (!focused && !g_background_input)
    for (int port = 0; port < 4; ++port)
      if (out[port].err == 0) { const auto err = out[port].err; out[port] = {}; out[port].err = err; debug.ports[port] = out[port]; }
  { std::lock_guard<std::mutex> lock(g_last_pads_mutex); for (int port = 0; port < 4; ++port) g_last_pads[port] = out[port]; }

  debug.keyboard_actions = keyboard_actions;
  for (int idx = 0; idx < 4; ++idx) if (xin_connected[idx]) debug.xinput_connected[idx] = true;
  input_debug_snapshot(debug);
}

bool g_rumble_enabled = true;
bool g_background_input = true;

namespace {
// An Xbox pad's motors: both at full while the game asks for rumble, off otherwise. Only written
// when the state changes, since XInputSetState is a call into the driver.
void xinput_rumble(int index, bool on) {
  static bool state[4] = {};
  if (index < 0 || index > 3 || state[index] == on) return;
  state[index] = on;
  XINPUT_VIBRATION v{};
  v.wLeftMotorSpeed = v.wRightMotorSpeed = on ? 65535 : 0;
  XInputSetState((DWORD)index, &v);
}
// The first rumble each port asks for, and where it went: players report "rumble does not work"
// without saying which controller, and this line answers that from their log.
void log_first_rumble(int game_port, const char* where) {
  static bool logged[5] = {};
  const int i = game_port < 0 ? 4 : game_port;
  if (logged[i]) return;
  logged[i] = true;
  log("rumble: %s%d -> %s%s", game_port < 0 ? "local player" : "port ", game_port < 0 ? 0 : game_port + 1, where,
      g_rumble_enabled ? "" : " (switched off in Controls)");
}
}  // namespace

void input_rumble(int game_port, bool on) {
  if (game_port < 0 || game_port > 3) return;
  if (!g_rumble_enabled) on = false;
  const PortSource& src = g_port_feeding[game_port];   // where the input came from, fallback included
  const bool valid = src.index >= 0 && src.index < 4;
  if (src.kind == DeviceKind::GCAdapter && valid) { gcadapter_rumble(src.index, on); if (on) log_first_rumble(game_port, "GameCube adapter"); }
  else if (src.kind == DeviceKind::XInputPad && valid) { xinput_rumble(src.index, on); if (on) log_first_rumble(game_port, "Xbox controller"); }
  else if (on) log_first_rumble(game_port, "a controller without rumble support (keyboard, PlayStation, Switch or box)");
}

void input_rumble_local(bool on) {
  if (!g_rumble_enabled) on = false;
  // Online the match's slot is not the socket the controller is in, so the caller cannot name a
  // port: it only knows the rumble was meant for the local player. Rumble the one controller that
  // is actually feeding a port, not every device on the machine.
  //
  // This used to poll the adapter and XInput and buzz everything connected, which meant a player
  // on port 3 in a Direct match felt their port 1 controller go off as well, and anyone with a
  // second pad plugged in felt it on both. g_port_feeding already records which device feeds each
  // port, with the same fallback the offline path in input_rumble() uses, so the local player's
  // controller is the first port that has a real one behind it.
  for (int port = 0; port < 4; ++port) {
    const PortSource& src = g_port_feeding[port];
    const bool valid = src.index >= 0 && src.index < 4;
    if (!valid) continue;
    if (src.kind == DeviceKind::GCAdapter) {
      gcadapter_rumble(src.index, on);
      if (on) log_first_rumble(-1, "GameCube adapter");
      return;
    }
    if (src.kind == DeviceKind::XInputPad) {
      xinput_rumble(src.index, on);
      if (on) log_first_rumble(-1, "Xbox controller");
      return;
    }
  }
  if (on) log_first_rumble(-1, "a controller without rumble support (keyboard, PlayStation, Switch or box)");
}

void input_last_pads(PadState out[4]) {
  std::lock_guard<std::mutex> lock(g_last_pads_mutex);
  for (int port = 0; port < 4; ++port) out[port] = g_last_pads[port];
}

void input_debug_snapshot(InputDebugSnapshot& snapshot) {
  snapshot = {};
  PadState gc[4];
  for (auto& s : gc) { s = {}; s.err = -1; }
  uint32_t gc_mask = gcadapter_poll(gc);
  snapshot.gc_mask = gc_mask;

  PadState kb{}; kb.err = 0;
  {
    std::lock_guard<std::mutex> lock(g_keys_mutex);
    auto key = [](int vk) { return g_keys[vk & 0xFF]; };
    snapshot.keyboard_actions = apply_actions(kb, [&](int i) { const int vk = g_key_bindings.vk[i]; return vk && key(vk); });
    apply_stick_actions(snapshot.keyboard_actions, kb.stick_x, kb.stick_y);
    if (kb.button & PAD_L) kb.trig_l = 255;
    if (kb.button & PAD_R) kb.trig_r = 255;
  }

  PadState xin[4];
  for (int idx = 0; idx < 4; ++idx) {
    xin[idx] = {}; xin[idx].err = -1;
    XINPUT_STATE xs{};
    if (XInputGetState(idx, &xs) != ERROR_SUCCESS) continue;
    snapshot.xinput_connected[idx] = true;
    auto& g = xs.Gamepad;
    auto axis = [](SHORT v) { int a = v / 258; return a > 127 ? 127 : a < -127 ? -127 : a; };
    // Same reading as input_poll: no deadzone, no trigger floor.
    const int sx = axis(g.sThumbLX), sy = axis(g.sThumbLY), cx = axis(g.sThumbRX), cy = axis(g.sThumbRY);
    xin[idx].stick_x = (int8_t)sx; xin[idx].stick_y = (int8_t)sy; xin[idx].sub_x = (int8_t)cx; xin[idx].sub_y = (int8_t)cy;
    snapshot.xinput_actions[idx] = apply_actions(xin[idx], [&](int i) { const unsigned short m = g_pad_bindings[idx].mask[i]; return m && (g.wButtons & m); });
    xin[idx].trig_l = g.bLeftTrigger; if (g.bLeftTrigger > 200) { xin[idx].button |= PAD_L; snapshot.xinput_actions[idx] |= 1u << (int)BindAction::L; }
    xin[idx].trig_r = g.bRightTrigger; if (g.bRightTrigger > 200) { xin[idx].button |= PAD_R; snapshot.xinput_actions[idx] |= 1u << (int)BindAction::R; }
  }

  PadState ds4[4]{};
  uint16_t ds4_buttons[4]{};
  for (int idx = 0; idx < 4; ++idx) {
    std::lock_guard<std::mutex> lock(g_ds4_mutex);
    ds4_buttons[idx] = g_ds4_buttons[idx]; ds4[idx] = g_ds4_pads[idx];
    if (!g_ds4_devices[idx]) { ds4[idx].err = -1; continue; }
    snapshot.ds4_connected[idx] = true; ds4[idx].err = 0;
    snapshot.ds4_actions[idx] = apply_actions(ds4[idx], [&](int i) { return (g_ds4_bindings[idx].mask[i] & ds4_buttons[idx]) != 0; });
  }

  PadState swpro[4];
  {
    for (auto& s : swpro) { s = {}; s.err = -1; }
    uint16_t buttons[4]{};
    uint32_t swpro_mask = switchpro_poll(swpro, buttons);
    for (int idx = 0; idx < 4; ++idx) {
      if (!(swpro_mask & (1u << idx))) { swpro[idx] = {}; swpro[idx].err = -1; continue; }
      snapshot.swpro_connected[idx] = true;
      snapshot.swpro_buttons[idx] = buttons[idx];
      snapshot.swpro_actions[idx] = swpro_apply_bindings(idx, buttons[idx], swpro[idx]);
    }
  }

  // This function does not read back what input_poll computed: it rebuilds the whole snapshot from
  // the devices itself, which is why every family above is polled again here. A generic pad added
  // to input_poll alone therefore showed its name and its live axes, which are read straight from
  // the device, while reporting "not connected" and refusing to rebind, because those two come
  // from the snapshot.
  PadState hid[4];
  {
    for (auto& s : hid) { s = {}; s.err = -1; }
    uint32_t buttons[4]{};
    uint32_t hid_mask = hidpad_poll(hid, buttons);
    for (int idx = 0; idx < 4; ++idx) {
      if (!(hid_mask & (1u << idx))) { hid[idx] = {}; hid[idx].err = -1; continue; }
      snapshot.hid_connected[idx] = true;
      snapshot.hid_actions[idx] = hid_apply_bindings(idx, buttons[idx], hid[idx]);
    }
  }

  for (int idx = 0; idx < 4; ++idx) {
    snapshot.gc_actions[idx] = 0;
    if (!(gc_mask & (1u << idx))) continue;
    snapshot.gc_actions[idx] = gc_apply_bindings(idx, gc[idx]);
    snapshot.ports[idx] = gc[idx];
  }
  snapshot.keyboard_pad = kb;
  for (int idx = 0; idx < 4; ++idx) {
    snapshot.xinput_pad[idx] = xin[idx]; snapshot.ds4_pad[idx] = ds4[idx]; snapshot.swpro_pad[idx] = swpro[idx];
    snapshot.hid_pad[idx] = hid[idx];
    if (gc_mask & (1u << idx)) snapshot.gc_pad[idx] = gc[idx];
  }
  for (int port = 0; port < 4; ++port) {
    const PortSource& src = g_port_sources[port];
    switch (src.kind) {
      case DeviceKind::Keyboard: snapshot.ports[port] = kb; break;
      case DeviceKind::XInputPad:
        if (src.index >= 0 && src.index < 4 && snapshot.xinput_connected[src.index]) snapshot.ports[port] = xin[src.index];
        break;
      case DeviceKind::DS4Pad:
        if (src.index >= 0 && src.index < 4 && snapshot.ds4_connected[src.index]) snapshot.ports[port] = ds4[src.index];
        break;
      case DeviceKind::SwitchPro:
        if (src.index >= 0 && src.index < 4 && snapshot.swpro_connected[src.index]) snapshot.ports[port] = swpro[src.index];
        break;
      case DeviceKind::HidPad:
        if (src.index >= 0 && src.index < 4 && snapshot.hid_connected[src.index]) snapshot.ports[port] = hid[src.index];
        break;
      case DeviceKind::GCAdapter:
        if (src.index >= 0 && src.index < 4 && (gc_mask & (1u << src.index))) snapshot.ports[port] = gc[src.index];
        break;
      case DeviceKind::None: default: break;
    }
  }
}

}  // namespace host
