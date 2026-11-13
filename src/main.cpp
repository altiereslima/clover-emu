// clover-emu: runs the NES / Super NES Classic Edition firmware userland on macOS, Linux and Windows.
#include "kernel.h"
#include "display.h"
#include "hle.h"
#include "ui.h"
#include "launcher.h"
#include "lang.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <algorithm>
#include <atomic>
#include <thread>
#ifdef _WIN32
#include "squashfs.h"
#include "winutil.h"
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

// UTF-8 <-> std::filesystem (paths are UTF-8 strings everywhere else; Windows paths are wide).
static std::string u8str(const fs::path& p) {
    auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}
static fs::path P(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

static void usage() {
    fprintf(stderr,
            "usage: clover-emu [options] [FIRMWARE.hsqs]\n"
            "\n"
            "Boots a NES / Super NES Classic Edition firmware image. With no image argument, the only\n"
            ".hsqs (or already-extracted firmware) next to the binary / in the current folder is used.\n"
            "Each image is extracted once to firmware/<name>/ and keeps its saves in saves/<name>/.\n"
            "\n"
            "  --image FILE      firmware image (same as the positional argument)\n"
            "  --root DIR        use an already-extracted rootfs folder instead\n"
            "  --data DIR        persistent /var/lib folder (default: saves/<name>)\n"
            "  --window WxH      window size in points (default 1280x720)\n"
            "  --windowed        start in a window (default: fullscreen)\n"
            "  --fullscreen      start fullscreen (the default)\n"
            "  --no-ptbr         keep the firmware's European Portuguese menus (default: Brazilian)\n"
            "  --run 'CMD ARGS'  run a single guest command instead of booting /sbin/init\n"
            "  --nowindow        no window (for --run tests)\n"
            "  --fps             print guest frame rate\n"
            "  --strace          log every syscall\n"
            "  --log LIST        extra logging: proc,vfs,hle,gl,signal,mem,dev,syscall\n"
            "  --shots DIR       write a screenshot every 2 s\n"
            "\nControls: arrows=D-pad  X=A  Z=B  S=X  A=Y  Q/W=L/R  RShift/Tab=Select  Enter=Start\n"
            "          Esc/R=console RESET  F11/Alt+Enter=fullscreen  closing the window = power switch off\n");
}

static void write_file(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s;
}

static void prepare_sysfs(const fs::path& sys) {
    fs::path usb = sys / "devices/virtual/android_usb/android0";
    for (const char* f : {"enable", "idVendor", "idProduct", "iManufacturer", "iProduct", "iSerial", "functions"})
        write_file(usb / f, "0\n");
    write_file(usb / "f_clover/model", "1\n");
    write_file(sys / "class/graphics/fb0/virtual_size", "1280,1440\n");
    write_file(sys / "class/graphics/fb0/modes", "U:1280x720p-60\n");
    write_file(sys / "class/graphics/fb0/bits_per_pixel", "32\n");
    write_file(sys / "class/disp/disp/attr/hdmi", "1\n");
    write_file(sys / "kernel/uevent_helper", "\n");
    fs::create_directories(sys / "class/input");
    fs::create_directories(sys / "bus");
    fs::create_directories(sys / "module");
    fs::create_directories(sys / "dev/char");
    fs::create_directories(sys / "dev/block");
    fs::create_directories(sys / "firmware");
    fs::create_directories(sys / "fs/cgroup");
}

#ifdef _WIN32
// Windows: the built-in SquashFS extractor. NTFS is case-insensitive like the usual macOS disk, so
// paths that differ only by case are extracted into <root>.casefold/<n>/ and mapped back by
// exact-case VFS overrides (same layout as the unsquashfs-based version).
static bool extract_firmware(const std::string& image, const std::string& final_root, std::string& err,
                             const std::function<void(float)>& progress) {
    fprintf(stderr, "extracting %s -> %s\n", image.c_str(), final_root.c_str());
    std::vector<std::string> paths;
    if (!sqfs::list(image, paths, err) || paths.empty()) {
        if (err.empty()) err = "empty image";
        fprintf(stderr, "cannot read firmware image: %s\n", err.c_str());
        return false;
    }
    // Extract next to the final folder and rename at the end, so an interrupted first run never
    // leaves a half-extracted firmware behind.
    std::string root = final_root + ".partial";
    {
        std::error_code ec;
        fs::remove_all(P(root), ec);
    }
    size_t total = paths.size(), seen = 0;
    auto tick = [&] {
        if (progress && (++seen % 64) == 0) progress(0.97f * (float)seen / (float)total);
    };
    std::map<std::string, std::vector<std::string>> folded;
    for (auto& p : paths) {
        std::string k = p;
        for (auto& c : k) c = (char)tolower((unsigned char)c);
        folded[k].push_back(p);
    }
    std::vector<std::string> moved;
    for (auto& [k, v] : folded)
        if (v.size() > 1) {
            std::sort(v.begin(), v.end());
            for (size_t i = 1; i < v.size(); i++) moved.push_back(v[i]);
        }
    std::vector<std::string> top;
    for (auto& m : moved)
        if (std::none_of(moved.begin(), moved.end(), [&](const std::string& o) { return o != m && m.compare(0, o.size() + 1, o + "/") == 0; }))
            top.push_back(m);
    if (!sqfs::extract(image, root, top, "", err, tick)) {
        fprintf(stderr, "extraction failed: %s\n", err.c_str());
        return false;
    }
    if (!hfs::exists(root + "/bin/busybox")) {
        err = "not a NES / SNES Classic firmware (bin/busybox missing)";
        fprintf(stderr, "extraction incomplete: %s/bin/busybox missing\n", root.c_str());
        std::error_code ec;
        fs::remove_all(P(root), ec);
        return false;
    }
    {
        std::error_code ec;
        fs::remove_all(P(final_root), ec);
        fs::rename(P(root), P(final_root), ec);
        if (ec) {
            err = "cannot rename " + root + ": " + ec.message();
            return false;
        }
    }
    fs::path cf = P(final_root + ".casefold");
    std::error_code ec;
    fs::remove_all(cf, ec);
    if (!top.empty()) {
        fs::create_directories(cf);
        std::ofstream map(cf / "map.txt");
        for (size_t i = 0; i < top.size(); i++) {
            std::string d = u8str(cf / std::to_string(i));
            std::string e2;
            if (!sqfs::extract(image, d, {}, top[i], e2)) fprintf(stderr, "  %s: %s\n", top[i].c_str(), e2.c_str());
            map << top[i] << "\t" << i << "\n";
            fprintf(stderr, "  case-colliding path %s kept separately\n", top[i].c_str());
        }
    }
    if (progress) progress(1.0f);
    return true;
}

#else
static std::string shq(const std::string& s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    return o + "'";
}

// macOS filesystems are usually case-insensitive, but firmware images may contain paths that differ
// only by case (e.g. /etc/clover/VERSION and /etc/clover/version/). Such entries are extracted into
// <root>.casefold/<n>/ and mapped back into the guest namespace by exact-case VFS overrides.
static bool extract_firmware(const std::string& image, const std::string& root, std::string& err,
                             const std::function<void(float)>& progress) {
    (void)progress;
    err = "unsquashfs failed (brew install squashfs)";
    fprintf(stderr, "extracting %s -> %s\n", image.c_str(), root.c_str());
    std::string list_cmd = "unsquashfs -l -d '' " + shq(image) + " 2>/dev/null";
    FILE* pf = popen(list_cmd.c_str(), "r");
    if (!pf) return false;
    std::vector<std::string> paths;
    char line[8192];
    while (fgets(line, sizeof line, pf)) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (!l.empty() && l[0] == '/') paths.push_back(l);
    }
    if (pclose(pf) != 0 || paths.empty()) {
        fprintf(stderr, "unsquashfs failed (brew install squashfs)\n");
        return false;
    }
    std::map<std::string, std::vector<std::string>> folded;
    for (auto& p : paths) {
        std::string k = p;
        for (auto& c : k) c = (char)tolower((unsigned char)c);
        folded[k].push_back(p);
    }
    std::vector<std::string> moved;  // colliding paths extracted separately (all but the first spelling)
    for (auto& [k, v] : folded)
        if (v.size() > 1) {
            std::sort(v.begin(), v.end());
            for (size_t i = 1; i < v.size(); i++) moved.push_back(v[i]);
        }
    // drop entries nested under another moved entry
    std::vector<std::string> top;
    for (auto& m : moved)
        if (std::none_of(moved.begin(), moved.end(), [&](const std::string& o) { return o != m && m.compare(0, o.size() + 1, o + "/") == 0; }))
            top.push_back(m);
    std::string cmd = "unsquashfs -f -d " + shq(root) + (top.empty() ? "" : " -excludes") + " " + shq(image);
    for (auto& m : top) cmd += " " + shq(m.substr(1));
    cmd += " >/dev/null 2>&1";
    // Non-zero exit is expected when the image contains device nodes (not creatable without root);
    // /dev is provided by the emulator anyway. Success is judged by the extracted contents below.
    (void)system(cmd.c_str());
    fs::path cf = root + ".casefold";
    std::error_code ec;
    fs::remove_all(cf, ec);
    if (!top.empty()) {
        fs::create_directories(cf);
        std::ofstream map(cf / "map.txt");
        for (size_t i = 0; i < top.size(); i++) {
            fs::path d = cf / std::to_string(i);
            std::string c2 = "unsquashfs -f -d " + shq(d.string()) + " " + shq(image) + " " + shq(top[i].substr(1)) + " >/dev/null 2>&1";
            (void)system(c2.c_str());
            map << top[i] << "\t" << i << "\n";
            fprintf(stderr, "  case-colliding path %s kept separately\n", top[i].c_str());
        }
    }
    if (!fs::exists(fs::path(root) / "bin/busybox")) {
        fprintf(stderr, "extraction incomplete: %s/bin/busybox missing\n", root.c_str());
        return false;
    }
    return true;
}

#endif

static void load_casefold_map(const std::string& root) {
    fs::path cf = P(root + ".casefold");
    std::ifstream map(cf / "map.txt");
    std::string l;
    while (std::getline(map, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
        size_t tab = l.find('\t');
        if (tab == std::string::npos) continue;
        std::string guest = l.substr(0, tab);
        vfs::add_override(guest, u8str(cf / l.substr(tab + 1)) + guest);
    }
}

static std::vector<std::string> split_cmd(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    bool q = false;
    for (char c : s) {
        if (c == '\'' || c == '"') { q = !q; continue; }
        if (c == ' ' && !q) {
            if (!cur.empty()) v.push_back(cur);
            cur.clear();
        } else cur += c;
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

int main(int argc_, char** argv_) {
    ui::init_console();
    plat::init();
    SDL_SetMainReady();
    g_fatal_hook = [](const char* m) {
        if (!ui::has_console()) ui::error(ui::tr(ui::FATAL), m);
    };
    // Errors: a dialog when double-clicked (no console), plain text in a terminal.
    auto fail = [](ui::Msg headline, const std::string& detail) {
        if (ui::has_console()) fprintf(stderr, "%s\n%s\n", ui::tr(headline), detail.c_str());
        else ui::error(ui::tr(headline), detail);
        return 1;
    };
    // UTF-8 command line (on Windows argv is in the ANSI code page)
    std::vector<std::string> args_utf8;
#ifdef _WIN32
    {
        int n = 0;
        LPWSTR* wa = CommandLineToArgvW(GetCommandLineW(), &n);
        for (int i = 0; i < n; i++) args_utf8.push_back(wide_to_utf8(wa[i], wcslen(wa[i])));
        LocalFree(wa);
    }
#else
    for (int i = 0; i < argc_; i++) args_utf8.push_back(argv_[i]);
#endif
    std::vector<char*> argv_ptrs;
    for (auto& a : args_utf8) argv_ptrs.push_back(a.data());
    int argc = (int)argv_ptrs.size();
    char** argv = argv_ptrs.data();
    std::string root, image, data, run_cmd;
    int ww = 1280, wh = 720;
    bool fullscreen = true, nowindow = false, ptbr = true;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { usage(); exit(2); }
            return argv[++i];
        };
        if (a == "--root") root = next();
        else if (a == "--image") image = next();
        else if (a == "--data") data = next();
        else if (a == "--window") sscanf(next().c_str(), "%dx%d", &ww, &wh);
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--windowed") fullscreen = false;
        else if (a == "--no-ptbr") ptbr = false;
        else if (a == "--run") run_cmd = next();
        else if (a == "--nowindow") nowindow = true;
        else if (a == "--fps") { extern bool g_show_fps; g_show_fps = true; }
        else if (a == "--shots") display::g_shot_dir = next();
        else if (a == "--strace") g_kernel.strace = true;
        else if (a == "--log") {
            std::string l = next();
            if (l.find("proc") != std::string::npos) g_log_mask |= LOG_PROC;
            if (l.find("vfs") != std::string::npos) g_log_mask |= LOG_VFS;
            if (l.find("hle") != std::string::npos) g_log_mask |= LOG_HLE;
            if (l.find("gl") != std::string::npos) g_log_mask |= LOG_GL;
            if (l.find("signal") != std::string::npos) g_log_mask |= LOG_SIGNAL;
            if (l.find("mem") != std::string::npos) g_log_mask |= LOG_MEM;
            if (l.find("dev") != std::string::npos) g_log_mask |= LOG_DEV;
            if (l.find("syscall") != std::string::npos) g_kernel.strace = true;
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (!a.empty() && a[0] != '-' && image.empty()) {
            image = a;  // positional firmware image
        } else {
            fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    // Layout (all relative to the "base" folder):
    //   <name>.hsqs          firmware image
    //   firmware/<name>/     extracted rootfs (+ firmware/<name>.casefold/)
    //   saves/<name>/        persistent /var/lib
    //   run/<name>/          per-boot tmpfs (/var, /tmp, /run, /sys)
    fs::path exe_dir = fs::current_path();
    std::string exe = plat::exe_path();
    if (!exe.empty()) exe_dir = fs::weakly_canonical(P(exe)).parent_path();
    auto has_firmware = [](const fs::path& d) {
        std::error_code e;
        if (fs::is_directory(d / "firmware", e)) return true;
        for (auto& f : fs::directory_iterator(d, e))
            if (f.path().extension() == ".hsqs") return true;
        return false;
    };

    // Window first: the console chooser and the first-run progress are drawn in it.
    display::init_fb();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Host controllers: XInput (Xbox 360/One/Series and compatibles, Guide button included),
    // RawInput, and HIDAPI drivers (DualShock 4, DualSense, Switch Pro, 8BitDo, ...).
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_MAC_CTRL_CLICK_EMULATE_RIGHT_CLICK, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
        return fail(ui::NO_WINDOW, SDL_GetError());
    if (!nowindow) {
        if (!display::init_window(2 * ww / 1280 ? 2 * ww / 1280 : 2, fullscreen)) return fail(ui::NO_WINDOW, SDL_GetError());
        if (ww != 1280 || wh != 720) SDL_SetWindowSize(display::window(), ww, wh);
        ui::set_owner(display::window());
    }

    fs::path base;
    std::string name;
    if (image.empty() && root.empty()) {
        // No image given (e.g. the exe was double-clicked): look next to the exe / current folder.
        // One image boots directly; several ask which console to turn on; none asks for the file.
        struct Found {
            std::string image;  // "" = already-extracted firmware folder without its .hsqs
            fs::path base;
            std::string name;
        };
        std::vector<Found> found;
        for (fs::path cand : {fs::current_path(), exe_dir, exe_dir.parent_path(), exe_dir.parent_path().parent_path()}) {
            if (!has_firmware(cand)) continue;
            std::vector<std::string> names;
            std::error_code e;
            for (auto& f : fs::directory_iterator(cand, e))
                if (f.path().extension() == ".hsqs") names.push_back(u8str(f.path().stem()));
            std::sort(names.begin(), names.end());
            for (auto& n : names) found.push_back({u8str(cand / P(n + ".hsqs")), cand, n});
            std::vector<std::string> extracted;
            for (auto& f : fs::directory_iterator(cand / "firmware", e)) {
                std::string n = u8str(f.path().filename());
                if (f.is_directory() && fs::exists(f.path() / "bin/busybox") &&
                    std::find(names.begin(), names.end(), n) == names.end())
                    extracted.push_back(n);
            }
            std::sort(extracted.begin(), extracted.end());
            for (auto& n : extracted) found.push_back({"", cand, n});
            break;
        }
        std::string last = ui::remembered_image();
        std::error_code e;
        if (!last.empty() && fs::is_regular_file(P(last), e) &&
            std::none_of(found.begin(), found.end(), [&](const Found& f) {
                std::error_code e2;
                return !f.image.empty() && fs::equivalent(P(f.image), P(last), e2);
            }))
            found.push_back({last, P(last).parent_path(), u8str(P(last).stem())});

        int pick = 0;
        if (found.size() > 1) {
            if (nowindow) {
                fprintf(stderr, "several firmware images found; pick one:\n");
                for (auto& f : found) fprintf(stderr, "  clover-emu \"%s\"\n", f.image.empty() ? f.name.c_str() : f.image.c_str());
                return 1;
            }
            std::vector<launcher::Item> items;
            int def = 0;
            std::vector<std::string> files;
            for (size_t i = 0; i < found.size(); i++) {
                std::string console, details;
                ui::describe(found[i].name, console, details);
                files.push_back(found[i].image.empty() ? found[i].name : u8str(P(found[i].image).filename()));
                items.push_back({console, details.empty() ? files.back() : details, found[i].name});
                std::error_code e2;
                if (!last.empty() && !found[i].image.empty() && fs::equivalent(P(found[i].image), P(last), e2)) def = (int)i;
            }
            // cards that would look identical show their file name instead
            std::vector<bool> dup(items.size(), false);
            for (size_t i = 0; i < items.size(); i++)
                for (size_t j = 0; j < items.size(); j++)
                    if (i != j && items[i].title == items[j].title && items[i].subtitle == items[j].subtitle) dup[i] = true;
            for (size_t i = 0; i < items.size(); i++)
                if (dup[i]) items[i].subtitle = files[i];
            // In-window chooser; "other file" opens the file picker and cancelling it comes back here.
            for (;;) {
                pick = launcher::choose(items, def);
                if (pick == -3) {  // no usable font: native dialog instead
                    std::vector<ui::Choice> native;
                    for (auto& it : items) native.push_back({it.title, it.subtitle});
                    pick = ui::choose(native, def);
                }
                if (pick == -2) return 0;
                if (pick != -1) break;
                image = ui::pick_firmware();
                if (!image.empty()) break;
                def = (int)items.size();  // back on the "other file" card
            }
        } else if (found.empty()) {
            if (nowindow) {
                fprintf(stderr, "no firmware found: put your console's .hsqs image here, or pass it as an argument\n\n");
                usage();
                return 1;
            }
            pick = -1;
            image = ui::pick_firmware();
            if (image.empty()) return 0;
        }
        if (pick == -1) {
            // image chosen with the file picker
        } else if (found[pick].image.empty()) {
            base = found[pick].base;
            name = found[pick].name;
        } else {
            image = found[pick].image;
        }
    }
    if (!image.empty()) {
        if (!fs::exists(P(image))) return fail(ui::BAD_IMAGE, image);
        image = u8str(fs::absolute(P(image)));
        base = P(image).parent_path();
        name = u8str(P(image).stem());
        // Keep firmware/ and saves/ next to the image; if that folder is read-only, use the
        // per-user settings folder instead.
        std::error_code ec;
        fs::create_directories(base / "firmware", ec);
        if (ec) {
            if (char* pp = SDL_GetPrefPath("", ui::APP_NAME)) {
                base = P(pp);
                SDL_free(pp);
            }
        }
        if (!nowindow) ui::remember_image(image);
    } else if (!root.empty()) {
        base = fs::absolute(P(root)).parent_path();
        if (base.filename() == "firmware") base = base.parent_path();
        name = u8str(fs::absolute(P(root)).filename());
    }
    if (root.empty()) root = u8str(base / "firmware" / P(name));
    bool need_extract = !fs::exists(P(root) / "bin/busybox");
    if (need_extract && image.empty()) return fail(ui::BAD_IMAGE, "rootfs not found at " + root + " and no .hsqs image given");

    if (need_extract) {
        fs::create_directories(P(root).parent_path());
        std::string abs_root = u8str(fs::absolute(P(root))), err;
        bool ok = false;
        if (nowindow) {
            ok = extract_firmware(image, abs_root, err, nullptr);
        } else {
            std::atomic<float> prog{0};
            std::atomic<bool> done{false};
            std::thread worker([&] {
                ok = extract_firmware(image, abs_root, err, [&](float f) { prog = f; });
                done = true;
            });
            bool quit = false, fancy = true;
            while (!done) {
                SDL_Event ev;
                while (SDL_PollEvent(&ev)) {
                    if (ev.type == SDL_QUIT) quit = true;
                    if (ev.type == SDL_KEYDOWN && (ev.key.keysym.scancode == SDL_SCANCODE_F11 ||
                                                   (ev.key.keysym.scancode == SDL_SCANCODE_RETURN && (ev.key.keysym.mod & KMOD_ALT))))
                        display::toggle_fullscreen();
                }
                if (fancy) fancy = launcher::progress(prog);
                if (!fancy) display::draw_progress(prog);
                SDL_Delay(16);
            }
            worker.join();
            if (quit) return 0;
            if (ok) {
                if (fancy) {
                    launcher::progress(1.0f);
                    SDL_Delay(200);
                    launcher::fade_out();
                } else {
                    display::draw_progress(1.0f);
                    SDL_Delay(150);
                }
                // black in both buffers, so a stale progress frame never flashes during boot
                display::draw_progress(-1);
                display::draw_progress(-1);
            }
        }
        if (!ok) return fail(ui::BAD_IMAGE, image + "\n\n" + err);
    }
    root = u8str(fs::canonical(P(root)));
    load_casefold_map(root);
    {
        std::ifstream bt(P(root) / "etc/clover/boardtype");
        std::string board;
        std::getline(bt, board);
        display::g_snes = board.find("shvc") != std::string::npos;
        fprintf(stderr, "[kernel] board type: %s\n", board.c_str());
    }
    if (data.empty()) data = u8str(base / "saves" / P(name));
    fs::path rundir = base / "run" / P(name);
    std::error_code ec;
    fs::remove_all(rundir, ec);
    for (const char* d : {"var", "tmp", "run", "shm", "sys", "hle"}) fs::create_directories(rundir / d);
    fs::create_directories(P(data));
    prepare_sysfs(rundir / "sys");

    g_kernel.rootfs = root;
    g_kernel.datadir = u8str(fs::canonical(P(data)));
    g_kernel.rundir = u8str(fs::canonical(rundir));
    g_kernel.boot_time_ns = now_ns();

    vfs::init(g_kernel.rootfs, g_kernel.datadir, g_kernel.rundir);
    if (ptbr) lang::apply_ptbr(g_kernel.rootfs, g_kernel.rundir, g_kernel.datadir);
    hle_init(g_kernel.rundir);

    std::shared_ptr<Process> init;
    if (!run_cmd.empty()) {
        auto args = split_cmd(run_cmd);
        init = spawn_init(args[0], args, {"PATH=/sbin:/usr/sbin:/bin:/usr/bin", "HOME=/", "TERM=linux"});
    } else {
        LOGI("[kernel] booting %s (rootfs %s)\n", "/sbin/init", root.c_str());
        init = spawn_init("/sbin/init", {"/sbin/init"}, {"HOME=/", "TERM=linux"});
    }
    if (nowindow) {
        while (!g_kernel.shutting_down) plat::usleep_us(10000);
    } else {
        display::main_loop();
    }
    int st = init ? init->exit_status & 0xffff : 0;
    LOGI("[kernel] halted (init status 0x%x)\n", st);
    // Hard exit: guest threads may still be blocked.
    fflush(stderr);
    hard_exit(run_cmd.empty() ? 0 : ((st >> 8) & 0xff));
}
