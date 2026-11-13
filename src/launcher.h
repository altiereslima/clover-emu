// In-window launcher screens (drawn into the emulator window before the firmware boots):
// the console chooser and the first-run "preparing" progress screen. Controller, keyboard and
// mouse friendly; texts follow the system language (see ui.h).
#pragma once
#include "common.h"
#include <functional>
#include <string>
#include <vector>

namespace launcher {
struct Item {
    std::string title;     // friendly console name
    std::string subtitle;  // file name
    std::string stem;      // image / folder name, used to pick the artwork
};
// Returns the chosen index, -1 for "other file...", -2 to quit, or -3 when it cannot run (no usable
// font found), in which case the caller should fall back to a native dialog.
int choose(const std::vector<Item>& items, int default_index);
// First-run extraction screen, frac in [0, 1]. Returns false when it cannot draw (no font).
bool progress(float frac);
// Fade the last launcher frame to black (before the firmware takes over the window).
void fade_out();

// In-game pause menu over the (blurred) paused frame `frame` (RGBA, top row first). `pump` runs
// every frame (main-thread work queue). Returns one of the MENU_* actions.
enum { MENU_RESUME, MENU_GAME_LIST, MENU_QUIT };
int pause_menu(const std::vector<u8>& frame, int fw, int fh, const std::function<void()>& pump);
}  // namespace launcher
