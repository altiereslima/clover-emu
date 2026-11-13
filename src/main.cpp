// clover-emu: runs the NES Classic Edition (CLV) firmware userland on macOS.
#include "kernel.h"
#include "display.h"
#include "hle.h"
#include <SDL.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <map>
#include <algorithm>

namespace fs = std::filesystem;

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
            "  --fullscreen      start fullscreen\n"
            "  --run 'CMD ARGS'  run a single guest command instead of booting /sbin/init\n"
            "  --nowindow        no window (for --run tests)\n"
            "  --fps             print guest frame rate\n"
            "  --strace          log every syscall\n"
            "  --log LIST        extra logging: proc,vfs,hle,gl,signal,mem,dev,syscall\n"
            "  --shots DIR       write a screenshot every 2 s\n"
            "\nControls: arrows=D-pad  X=A  Z=B  S=X  A=Y  Q/W=L/R  RShift/Tab=Select  Enter=Start\n"
            "          Esc/R=console RESET  F11=fullscreen  closing the window = power switch off\n");
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
static bool extract_firmware(const std::string& image, const std::string& root) {
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

static void load_casefold_map(const std::string& root) {
    fs::path cf = root + ".casefold";
    std::ifstream map(cf / "map.txt");
    std::string l;
    while (std::getline(map, l)) {
        size_t tab = l.find('\t');
        if (tab == std::string::npos) continue;
        std::string guest = l.substr(0, tab);
        vfs::add_override(guest, (cf / l.substr(tab + 1)).string() + guest);
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

int main(int argc, char** argv) {
    std::string root, image, data, run_cmd;
    int ww = 1280, wh = 720;
    bool fullscreen = false, nowindow = false;
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
    char exe_buf[4096];
    u32 exe_sz = sizeof exe_buf;
    fs::path exe_dir = fs::current_path();
    if (_NSGetExecutablePath(exe_buf, &exe_sz) == 0) exe_dir = fs::weakly_canonical(fs::path(exe_buf)).parent_path();
    auto has_firmware = [](const fs::path& d) {
        std::error_code e;
        if (fs::is_directory(d / "firmware", e)) return true;
        for (auto& f : fs::directory_iterator(d, e))
            if (f.path().extension() == ".hsqs") return true;
        return false;
    };
    fs::path base;
    std::string name;
    if (!image.empty()) {
        if (!fs::exists(image)) {
            fprintf(stderr, "firmware image not found: %s\n", image.c_str());
            return 1;
        }
        base = fs::absolute(image).parent_path();
        name = fs::path(image).stem().string();
    } else if (!root.empty()) {
        base = fs::absolute(root).parent_path();
        if (base.filename() == "firmware") base = base.parent_path();
        name = fs::absolute(root).filename().string();
    } else {
        for (fs::path cand : {fs::current_path(), exe_dir, exe_dir.parent_path(), exe_dir.parent_path().parent_path()})
            if (has_firmware(cand)) {
                base = cand;
                break;
            }
        if (base.empty()) {
            fprintf(stderr, "no firmware found: put your console's .hsqs image here, or pass it as an argument\n\n");
            usage();
            return 1;
        }
        std::vector<std::string> names;
        std::error_code e;
        for (auto& f : fs::directory_iterator(base, e))
            if (f.path().extension() == ".hsqs") names.push_back(f.path().stem().string());
        for (auto& f : fs::directory_iterator(base / "firmware", e))
            if (f.is_directory() && fs::exists(f.path() / "bin/busybox") &&
                std::find(names.begin(), names.end(), f.path().filename().string()) == names.end())
                names.push_back(f.path().filename().string());
        if (names.size() != 1) {
            fprintf(stderr, "several firmware images found; pick one:\n");
            for (auto& n : names) fprintf(stderr, "  ./build/clover-emu %s.hsqs\n", n.c_str());
            return 1;
        }
        name = names[0];
        if (fs::exists(base / (name + ".hsqs"))) image = (base / (name + ".hsqs")).string();
    }
    if (root.empty()) root = (base / "firmware" / name).string();
    if (!fs::exists(fs::path(root) / "bin/busybox")) {
        if (image.empty()) {
            fprintf(stderr, "rootfs not found at %s and no .hsqs image given\n", root.c_str());
            return 1;
        }
        fs::create_directories(fs::path(root).parent_path());
        if (!extract_firmware(image, root)) return 1;
    }
    root = fs::canonical(root).string();
    load_casefold_map(root);
    {
        std::ifstream bt(fs::path(root) / "etc/clover/boardtype");
        std::string board;
        std::getline(bt, board);
        display::g_snes = board.find("shvc") != std::string::npos;
        fprintf(stderr, "[kernel] board type: %s\n", board.c_str());
    }
    if (data.empty()) data = (base / "saves" / name).string();
    fs::path rundir = base / "run" / name;
    std::error_code ec;
    fs::remove_all(rundir, ec);
    for (const char* d : {"var", "tmp", "run", "shm", "sys", "hle"}) fs::create_directories(rundir / d);
    fs::create_directories(fs::path(data));
    prepare_sysfs(rundir / "sys");

    g_kernel.rootfs = root;
    g_kernel.datadir = fs::canonical(data).string();
    g_kernel.rundir = fs::canonical(rundir).string();
    g_kernel.boot_time_ns = now_ns();
    signal(SIGPIPE, SIG_IGN);

    vfs::init(g_kernel.rootfs, g_kernel.datadir, g_kernel.rundir);
    hle_init(g_kernel.rundir);
    display::init_fb();

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_MAC_CTRL_CLICK_EMULATE_RIGHT_CLICK, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (!nowindow) {
        if (!display::init_window(2 * ww / 1280 ? 2 * ww / 1280 : 2, fullscreen)) return 1;
        if (ww != 1280 || wh != 720) SDL_SetWindowSize(display::window(), ww, wh);
    }

    std::shared_ptr<Process> init;
    if (!run_cmd.empty()) {
        auto args = split_cmd(run_cmd);
        init = spawn_init(args[0], args, {"PATH=/sbin:/usr/sbin:/bin:/usr/bin", "HOME=/", "TERM=linux"});
    } else {
        LOGI("[kernel] booting %s (rootfs %s)\n", "/sbin/init", root.c_str());
        init = spawn_init("/sbin/init", {"/sbin/init"}, {"HOME=/", "TERM=linux"});
    }
    if (nowindow) {
        while (!g_kernel.shutting_down) usleep(10000);
    } else {
        display::main_loop();
    }
    int st = init ? init->exit_status & 0xffff : 0;
    LOGI("[kernel] halted (init status 0x%x)\n", st);
    {
        std::error_code ec2;
        fs::remove_all("/tmp/clover-sock-" + std::to_string(getuid()) + "-" + std::to_string(getpid()), ec2);
    }
    // Hard exit: guest threads may still be blocked.
    fflush(stderr);
    _Exit(run_cmd.empty() ? 0 : ((st >> 8) & 0xff));
}
