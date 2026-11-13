// Friendly launcher pieces for people who just double-click the executable: firmware chooser,
// file picker, error dialogs and a log file when there is no console. Texts follow the system UI
// language (English, Português, Español, Français, Deutsch, Italiano, Nederlands, Русский,
// 日本語, 简体中文, 繁體中文, 한국어).
#pragma once
#include <string>
#include <vector>

struct SDL_Window;

namespace ui {
// Name shown to people (window title, launcher, dialogs, log and settings folder).
constexpr const char* APP_NAME = "Console Classics";

// Windows GUI build: attach to the parent console when started from a terminal; otherwise send
// stderr to clover-emu.log next to the executable. No-op elsewhere.
void init_console();
bool has_console();
const std::string& log_path();  // "" when logging to a console / terminal

enum Msg {
    CHOOSE, OTHER_FILE, CANCEL, PICK_TITLE, FILTER, BAD_IMAGE, FATAL, LOG_AT, NO_WINDOW, OK, QUIT, PREPARING,
    RESUME, GAME_LIST, FULLSCREEN, PAUSED, POWER_OFF, MSG_COUNT
};
const char* tr(Msg m);
// "en", "pt", "es", "fr", "de", "it", "nl", "ru", "ja", "zh-Hans", "zh-Hant", "ko"
const char* lang_code();
// Window that native dialogs (file picker) are modal to, so they show above a fullscreen window.
void set_owner(SDL_Window* w);

struct Choice {
    std::string label;  // e.g. "Super NES Classic Edition (USA) v2.0.7"
    std::string note;   // e.g. the file name
};
// Index of the chosen entry, -1 for "other file...", -2 when cancelled.
int choose(const std::vector<Choice>& items, int default_index);
// Native "open file" dialog for a .hsqs image; "" when cancelled.
std::string pick_firmware();
// Error dialog (always also written to stderr).
void error(const std::string& headline, const std::string& detail);

// "NES Classic Edition v1.0.7" style name from an image / folder name.
std::string friendly_name(const std::string& stem);
// The same split in two: console ("Super NES Classic Edition", or the stem when unknown) and
// details ("USA · v2.0.7", possibly empty).
void describe(const std::string& stem, std::string& console, std::string& details);
// Last firmware image the user booted (persisted in the per-user settings folder).
std::string remembered_image();
void remember_image(const std::string& image);
}  // namespace ui
