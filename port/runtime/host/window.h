// Host window services.
#pragma once
#include <functional>
#include <cstdint>

namespace host {
using MessageCallback = std::function<bool(void*, uint32_t, uintptr_t, intptr_t)>;
void window_set_message_callback(MessageCallback cb);
void window_input_capture(bool capture);
struct PadState;
bool window_ui_gamecube_pad(PadState& pad);
// Last routed state for every in-game port, captured before UI input suppression. This lets an
// overlay follow the controller port selected in Training instead of assuming port 1.
bool window_ui_pads(PadState pads[4]);
using ResizeCallback = std::function<void(int, int)>;
void* window_create(int w, int h, const wchar_t* title, bool visible = true);
void window_set_resize_callback(ResizeCallback cb);
void window_pump();
void window_set_fullscreen(bool enabled);
bool window_is_fullscreen();
// Resize the client area, as the resolution picker in the PC settings panel does. Ignored while
// the window is fullscreen (there the client area is the monitor) and clamped to the monitor.
void window_set_client_size(int w, int h);
bool window_take_fullscreen_toggle();   // true once per Alt+Enter press in the game window
bool window_take_escape();              // true once per Esc press, like the F1 toggle
bool window_take_settings_toggle();     // true once per F1 press (auto-repeat ignored), however late it is read
bool window_take_practice_toggle();     // true once per Tab press (auto-repeat ignored)
double window_refresh_rate();
void window_destroy();
void window_set_title(const wchar_t* title);
bool window_closed();
void window_client_size(int* w, int* h);
// Scripted input: text file with lines "FRAME BUTTON+BUTTON [sx=N] [sy=N] [cx=N] [cy=N]"; state holds
// until the next line. Buttons: A B X Y Z L R START DU DD DL DR. A line with only a frame releases all.
bool input_load_script(const char* path);
void input_mark_match_start();   // online match reached frame 1: `@match` script sections begin now
}  // namespace host
