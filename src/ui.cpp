#include "ui.h"
#include "common.h"
#include "platform.h"
#include <SDL.h>
#ifdef _WIN32
#include <SDL_syswm.h>
#endif
#include <filesystem>
#include <fstream>
#include <regex>
#include <algorithm>
#ifdef _WIN32
#include "winutil.h"
#include <commctrl.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <io.h>
#endif

namespace fs = std::filesystem;

namespace ui {
namespace {
fs::path P(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string u8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// ---------------------------------------------------------------- texts
enum Lang { EN, PT, ES, FR, DE, IT, NL, RU, JA, ZH_HANS, ZH_HANT, KO, LANG_COUNT };

const char* const k_text[LANG_COUNT][MSG_COUNT] = {
    {"Which console do you want to turn on?", "Other file…", "Cancel",
     "Locate your NES / Super NES Classic Edition firmware (.hsqs)", "Firmware image",
     "This file could not be used as a NES / Super NES Classic Edition firmware:",
     "Console Classics stopped because of an error:", "Details were saved to:", "Could not open the window (SDL / OpenGL):", "OK", "Quit", "Getting your console ready (first run only)…", "Resume", "Game list", "Fullscreen", "Paused", "Turn off and quit"},
    {"Qual console você quer ligar?", "Outro arquivo…", "Cancelar",
     "Localize o firmware do seu NES / Super NES Classic Edition (.hsqs)", "Imagem de firmware",
     "Este arquivo não pôde ser usado como firmware de NES / Super NES Classic Edition:",
     "O Console Classics parou por causa de um erro:", "Os detalhes foram salvos em:",
     "Não foi possível abrir a janela (SDL / OpenGL):", "OK", "Sair", "Preparando o console (só na primeira vez)…", "Continuar", "Lista de jogos", "Tela cheia", "Pausado", "Desligar e sair"},
    {"¿Qué consola quieres encender?", "Otro archivo…", "Cancelar",
     "Busca el firmware de tu NES / Super NES Classic Edition (.hsqs)", "Imagen de firmware",
     "Este archivo no se pudo usar como firmware de NES / Super NES Classic Edition:",
     "Console Classics se detuvo por un error:", "Los detalles se guardaron en:", "No se pudo abrir la ventana (SDL / OpenGL):", "Aceptar", "Salir", "Preparando la consola (solo la primera vez)…", "Continuar", "Lista de juegos", "Pantalla completa", "En pausa", "Apagar y salir"},
    {"Quelle console voulez-vous allumer ?", "Autre fichier…", "Annuler",
     "Localisez le firmware de votre NES / Super NES Classic Edition (.hsqs)", "Image de firmware",
     "Ce fichier ne peut pas être utilisé comme firmware NES / Super NES Classic Edition :",
     "Console Classics s'est arrêté à cause d'une erreur :", "Les détails ont été enregistrés dans :",
     "Impossible d'ouvrir la fenêtre (SDL / OpenGL) :", "OK", "Quitter", "Préparation de la console (première fois seulement)…", "Reprendre", "Liste des jeux", "Plein écran", "En pause", "Éteindre et quitter"},
    {"Welche Konsole möchtest du einschalten?", "Andere Datei…", "Abbrechen",
     "Wähle die Firmware deines NES / Super NES Classic Edition (.hsqs)", "Firmware-Abbild",
     "Diese Datei kann nicht als NES / Super NES Classic Edition-Firmware verwendet werden:",
     "Console Classics wurde wegen eines Fehlers beendet:", "Details wurden gespeichert in:",
     "Das Fenster konnte nicht geöffnet werden (SDL / OpenGL):", "OK", "Beenden", "Konsole wird vorbereitet (nur beim ersten Start)…", "Fortsetzen", "Spieleliste", "Vollbild", "Pausiert", "Ausschalten und beenden"},
    {"Quale console vuoi accendere?", "Altro file…", "Annulla",
     "Individua il firmware del tuo NES / Super NES Classic Edition (.hsqs)", "Immagine firmware",
     "Questo file non può essere usato come firmware NES / Super NES Classic Edition:",
     "Console Classics si è interrotto a causa di un errore:", "I dettagli sono stati salvati in:",
     "Impossibile aprire la finestra (SDL / OpenGL):", "OK", "Esci", "Preparazione della console (solo la prima volta)…", "Riprendi", "Elenco giochi", "Schermo intero", "In pausa", "Spegni ed esci"},
    {"Welke console wil je aanzetten?", "Ander bestand…", "Annuleren",
     "Zoek de firmware van je NES / Super NES Classic Edition (.hsqs)", "Firmware-image",
     "Dit bestand kan niet worden gebruikt als NES / Super NES Classic Edition-firmware:",
     "Console Classics is gestopt door een fout:", "Details zijn opgeslagen in:", "Kan het venster niet openen (SDL / OpenGL):", "OK", "Afsluiten", "Console wordt voorbereid (alleen de eerste keer)…", "Doorgaan", "Spellenlijst", "Volledig scherm", "Gepauzeerd", "Uitzetten en afsluiten"},
    {"Какую консоль включить?", "Другой файл…", "Отмена",
     "Укажите прошивку вашей NES / Super NES Classic Edition (.hsqs)", "Образ прошивки",
     "Этот файл нельзя использовать как прошивку NES / Super NES Classic Edition:",
     "Console Classics остановлен из-за ошибки:", "Подробности сохранены в:", "Не удалось открыть окно (SDL / OpenGL):", "ОК", "Выход", "Подготовка консоли (только при первом запуске)…", "Продолжить", "Список игр", "Полный экран", "Пауза", "Выключить и выйти"},
    {"どのゲーム機の電源を入れますか？", "ほかのファイル…", "キャンセル",
     "NES / Super NES Classic Edition のファームウェア (.hsqs) を選んでください", "ファームウェアイメージ",
     "このファイルは NES / Super NES Classic Edition のファームウェアとして使用できません:",
     "エラーのため Console Classics を終了しました:", "詳細の保存先:", "ウィンドウを開けませんでした (SDL / OpenGL):", "決定", "終了", "ゲーム機を準備しています（初回のみ）…", "再開", "ゲーム一覧", "全画面表示", "一時停止中", "電源を切って終了"},
    {"要开启哪台主机？", "其他文件…", "取消", "请选择你的 NES / Super NES Classic Edition 固件 (.hsqs)", "固件镜像",
     "无法将此文件用作 NES / Super NES Classic Edition 固件：", "Console Classics 因错误而停止：", "详细信息已保存到：",
     "无法打开窗口 (SDL / OpenGL)：", "确定", "退出", "正在准备主机（仅首次运行）…", "继续", "游戏列表", "全屏", "已暂停", "关机并退出"},
    {"要開啟哪台主機？", "其他檔案…", "取消", "請選擇你的 NES / Super NES Classic Edition 韌體 (.hsqs)", "韌體映像",
     "無法將此檔案用作 NES / Super NES Classic Edition 韌體：", "Console Classics 因錯誤而停止：", "詳細資訊已儲存至：",
     "無法開啟視窗 (SDL / OpenGL)：", "確定", "結束", "正在準備主機（僅首次執行）…", "繼續", "遊戲列表", "全螢幕", "已暫停", "關機並結束"},
    {"어떤 게임기를 켤까요?", "다른 파일…", "취소", "NES / Super NES Classic Edition 펌웨어(.hsqs)를 선택하세요",
     "펌웨어 이미지", "이 파일은 NES / Super NES Classic Edition 펌웨어로 사용할 수 없습니다:",
     "오류로 인해 Console Classics가 중지되었습니다:", "자세한 내용이 저장된 위치:", "창을 열 수 없습니다 (SDL / OpenGL):", "확인", "종료", "게임기를 준비하는 중입니다(처음 한 번만)…", "계속하기", "게임 목록", "전체 화면", "일시 정지됨", "전원 끄고 종료"},
};

Lang lang_from_tag(std::string t) {
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return (char)tolower(c == '_' ? '-' : c); });
    auto is = [&](const char* p) { return t.rfind(p, 0) == 0; };
    if (is("pt")) return PT;
    if (is("es")) return ES;
    if (is("fr")) return FR;
    if (is("de")) return DE;
    if (is("it")) return IT;
    if (is("nl")) return NL;
    if (is("ru")) return RU;
    if (is("ja")) return JA;
    if (is("ko")) return KO;
    if (is("zh"))
        return (t.find("hant") != std::string::npos || t.find("-tw") != std::string::npos ||
                t.find("-hk") != std::string::npos || t.find("-mo") != std::string::npos)
                   ? ZH_HANT
                   : ZH_HANS;
    return EN;
}

// The language Windows (or the desktop session) displays its own UI in.
Lang system_lang() {
    if (const char* force = getenv("CLOVER_LANG"); force && *force) return lang_from_tag(force);  // testing
#ifdef _WIN32
    ULONG n = 0, len = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &n, nullptr, &len) && len) {
        std::wstring buf(len, L'\0');
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &n, buf.data(), &len) && n)
            return lang_from_tag(wide_to_utf8(buf.c_str(), wcslen(buf.c_str())));
    }
    return EN;
#else
    Lang l = EN;
    if (SDL_Locale* loc = SDL_GetPreferredLocales()) {
        if (loc[0].language) {
            std::string tag = loc[0].language;
            if (loc[0].country) tag += std::string("-") + loc[0].country;
            l = lang_from_tag(tag);
        }
        SDL_free(loc);
    } else if (const char* e = getenv("LANG")) {
        l = lang_from_tag(e);
    }
    return l;
#endif
}

Lang g_lang = LANG_COUNT;
bool g_console = true;
std::string g_log;

fs::path settings_dir() {
    char* p = SDL_GetPrefPath("", APP_NAME);
    if (!p) return {};
    fs::path r = P(p);
    SDL_free(p);
    return r;
}
}  // namespace

const char* tr(Msg m) {
    if (g_lang == LANG_COUNT) g_lang = system_lang();
    return k_text[g_lang][m];
}

const char* lang_code() {
    static const char* const codes[LANG_COUNT] = {"en", "pt", "es", "fr", "de", "it", "nl", "ru", "ja", "zh-Hans", "zh-Hant", "ko"};
    if (g_lang == LANG_COUNT) g_lang = system_lang();
    return codes[g_lang];
}

static SDL_Window* g_owner = nullptr;
void set_owner(SDL_Window* w) { g_owner = w; }

bool has_console() { return g_console; }
const std::string& log_path() { return g_log; }

// ---------------------------------------------------------------- console / log file
#ifdef _WIN32
void init_console() {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return;  // pipe, file or console
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {  // started from cmd / PowerShell
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONIN$", "r", stdin);
        return;
    }
    // Double-clicked: no console. Keep a log next to the executable (or in the settings folder).
    g_console = false;
    std::vector<fs::path> dirs;
    std::string exe = plat::exe_path();
    if (!exe.empty()) dirs.push_back(P(exe).parent_path());
    dirs.push_back(settings_dir());
    for (auto& d : dirs) {
        if (d.empty()) continue;
        fs::path lp = d / (std::string(APP_NAME) + ".log");
        // shared for reading, so the log can be watched while the emulator runs
        HANDLE h = CreateFileW(lp.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        int fd = _open_osfhandle((intptr_t)h, 0);
        if (fd < 0) {
            CloseHandle(h);
            continue;
        }
        // A GUI process starts without stdio descriptors: give each stream one, then point it at the log.
        FILE* f = nullptr;
        freopen_s(&f, "NUL", "w", stderr);
        freopen_s(&f, "NUL", "w", stdout);
        _dup2(fd, _fileno(stderr));
        _dup2(fd, _fileno(stdout));
        _close(fd);
        setvbuf(stderr, nullptr, _IONBF, 0);
        setvbuf(stdout, nullptr, _IONBF, 0);
        g_log = u8(lp);
        break;
    }
}
#else
void init_console() {}
#endif

// ---------------------------------------------------------------- dialogs
#ifdef _WIN32
namespace {
HICON app_icon() {
    return (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
}
}  // namespace
#endif

static int choose_sdl(const std::vector<Choice>& items, int def) {
    std::vector<SDL_MessageBoxButtonData> b;
    for (size_t i = 0; i < items.size(); i++)
        b.push_back({(int)i == def ? (u32)SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0u, (int)i, items[i].label.c_str()});
    b.push_back({0, -1, tr(OTHER_FILE)});
    b.push_back({SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, -2, tr(CANCEL)});
    SDL_MessageBoxData d{};
    d.flags = SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
    d.title = APP_NAME;
    d.message = tr(CHOOSE);
    d.numbuttons = (int)b.size();
    d.buttons = b.data();
    int hit = -2;
    if (SDL_ShowMessageBox(&d, &hit) != 0) return -2;
    return hit;
}

int choose(const std::vector<Choice>& items, int def) {
#ifdef _WIN32
    // Task dialog with command links (needs comctl32 v6, selected by the embedded manifest).
    using TaskDialogIndirect_t = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    static auto tdi = (TaskDialogIndirect_t)GetProcAddress(LoadLibraryW(L"comctl32.dll"), "TaskDialogIndirect");
    if (tdi) {
        std::vector<std::wstring> texts;
        for (auto& it : items) texts.push_back(utf8_to_wide(it.label + "\n" + it.note));
        texts.push_back(utf8_to_wide(tr(OTHER_FILE)));
        std::vector<TASKDIALOG_BUTTON> btn;
        for (size_t i = 0; i < texts.size(); i++) btn.push_back({(int)(100 + i), texts[i].c_str()});
        std::wstring title = utf8_to_wide(APP_NAME), instr = utf8_to_wide(tr(CHOOSE));
        TASKDIALOGCONFIG c{};
        c.cbSize = sizeof c;
        c.hInstance = GetModuleHandleW(nullptr);
        c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        c.pszWindowTitle = title.c_str();
        if (HICON ic = app_icon()) {
            c.dwFlags |= TDF_USE_HICON_MAIN;
            c.hMainIcon = ic;
        }
        c.pszMainInstruction = instr.c_str();
        c.cButtons = (UINT)btn.size();
        c.pButtons = btn.data();
        c.nDefaultButton = 100 + std::max(def, 0);
        int pressed = 0;
        if (SUCCEEDED(tdi(&c, &pressed, nullptr, nullptr))) {
            if (pressed >= 100 && pressed < 100 + (int)items.size()) return pressed - 100;
            if (pressed == 100 + (int)items.size()) return -1;
            return -2;
        }
    }
#endif
    return choose_sdl(items, def);
}

std::string pick_firmware() {
#ifdef _WIN32
    std::string out;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
        std::wstring title = utf8_to_wide(tr(PICK_TITLE)), filt = utf8_to_wide(std::string(tr(FILTER)) + " (*.hsqs)");
        COMDLG_FILTERSPEC spec[] = {{filt.c_str(), L"*.hsqs"}, {L"*.*", L"*.*"}};
        d->SetFileTypes(2, spec);
        d->SetTitle(title.c_str());
        FILEOPENDIALOGOPTIONS o = 0;
        d->GetOptions(&o);
        d->SetOptions(o | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
        // start next to the executable (unless Windows remembers a folder from a previous pick)
        std::string exe = plat::exe_path();
        IShellItem* folder = nullptr;
        if (!exe.empty() &&
            SUCCEEDED(SHCreateItemFromParsingName(P(exe).parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            d->SetDefaultFolder(folder);
            folder->Release();
        }
        HWND owner = nullptr;
        if (g_owner) {
            SDL_SysWMinfo wm;
            SDL_VERSION(&wm.version);
            if (SDL_GetWindowWMInfo(g_owner, &wm)) owner = wm.info.win.window;
        }
        if (SUCCEEDED(d->Show(owner))) {
            IShellItem* it = nullptr;
            if (SUCCEEDED(d->GetResult(&it))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    out = wide_to_utf8(p, wcslen(p));
                    CoTaskMemFree(p);
                }
                it->Release();
            }
        }
        d->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return out;
#else
    auto shq = [](const std::string& s) {
        std::string o = "'";
        for (char c : s) o += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return o + "'";
    };
    std::string prompt = tr(PICK_TITLE);
#ifdef __APPLE__
    std::string esc;
    for (char c : prompt) esc += (c == '"' || c == '\\') ? std::string("\\") + c : std::string(1, c);
    std::string cmd = "osascript -e " + shq("POSIX path of (choose file with prompt \"" + esc + "\")") + " 2>/dev/null";
#else
    std::string cmd = "zenity --file-selection --title=" + shq(prompt) + " --file-filter='*.hsqs' 2>/dev/null";
#endif
    std::string out;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[4096];
        while (fgets(buf, sizeof buf, p)) out += buf;
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
#endif
}

void error(const std::string& headline, const std::string& detail) {
    std::string text = headline + "\n\n" + detail;
    if (!g_log.empty()) text += std::string("\n\n") + tr(LOG_AT) + "\n" + g_log;
    fprintf(stderr, "%s\n", text.c_str());
    fflush(stderr);
#ifdef _WIN32
    MessageBoxW(nullptr, utf8_to_wide(text).c_str(), utf8_to_wide(APP_NAME).c_str(), MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, text.c_str(), nullptr);
#endif
}

// ---------------------------------------------------------------- names and settings
namespace {
void parts(const std::string& stem, std::string& console, std::string& region, std::string& version) {
    std::string s = stem;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    auto has = [&](const char* p) { return s.find(p) != std::string::npos; };
    region.clear();
    version.clear();
    if (has("snes")) console = "Super NES Classic Edition";
    else if (has("shvc") || has("sfc")) console = "Super Famicom Mini";
    else if (has("nes")) console = "NES Classic Edition";
    else if (has("hvc") || has("famicom")) console = "Famicom Mini";
    else {
        console = stem;
        return;
    }
    if (has("usa")) region = "USA";
    else if (has("eur")) region = "Europe";
    std::smatch m;
    if (std::regex_search(s, m, std::regex("v(\\d+(\\.\\d+)+)"))) version = "v" + m[1].str();
}
}  // namespace

std::string friendly_name(const std::string& stem) {
    std::string c, r, v;
    parts(stem, c, r, v);
    if (!r.empty()) c += " (" + r + ")";
    if (!v.empty()) c += " " + v;
    return c;
}

void describe(const std::string& stem, std::string& console, std::string& details) {
    std::string r, v;
    parts(stem, console, r, v);
    details = r;
    if (!v.empty()) details += (details.empty() ? "" : " · ") + v;
}

std::string remembered_image() {
    fs::path d = settings_dir();
    if (d.empty()) return {};
    std::ifstream f(d / "last-firmware.txt");
    std::string l;
    std::getline(f, l);
    while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
    return l;
}

void remember_image(const std::string& image) {
    fs::path d = settings_dir();
    if (d.empty()) return;
    std::ofstream(d / "last-firmware.txt") << image << "\n";
}
}  // namespace ui
