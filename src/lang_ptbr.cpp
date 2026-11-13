#include "lang.h"
#include "common.h"
#include "vfs.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

#include <sstream>

namespace fs = std::filesystem;

namespace lang {
namespace {

// Our own Brazilian Portuguese texts, by menu string id. Line breaks follow the layout of the
// original dialogs (the menus do not wrap text by themselves).
const std::map<std::string, std::string> k_common = {
    {"sys_copyright_Title", "Avisos legais"},
    {"sys_copyright_hud_Scroll", "Rolar"},
    {"sys_copyright_hud_Page", "Selecionar"},
    {"sys_copyright_hud_PlayStop", "Iniciar/Parar"},
    {"sys_copyright_hud_Return", "Voltar"},
    {"sys_copyright_oss_Title", "Software de cód. aberto"},
    {"sys_copyright_games_Title", "Propriedade intelectual"},
    {"sys_display_hud_Select", "Selecionar"},
    {"sys_display_hud_Decide", "OK"},
    {"sys_display_hud_Return", "Voltar"},
    {"sys_display_Title", "Exibição"},
    {"sys_display_frames_Title", "Moldura"},
    {"sys_display_item_DotByDot", "Resolução original"},
    {"sys_error_backupBroken_Yes", "OK"},
    {"sys_error_menuSaveLoadFail",
     "C2: As configurações foram restauradas.\n\nSe o problema continuar, anote\no código de erro e acesse\nsupport.nintendo.com."},
    {"sys_err_menuSaveStoreFail",
     "C3: Ocorreu um erro.\n\nSe o problema continuar, anote\no código de erro e acesse\nsupport.nintendo.com."},
    {"sys_error_resumeStoreFail",
     "C4: Ocorreu um erro.\n\nSe o problema continuar, anote\no código de erro e acesse\nsupport.nintendo.com."},
    {"sys_error_menuCrash", "C5: Ocorreu um erro.\n\nSe o problema continuar, anote\no código de erro e acesse\nsupport.nintendo.com."},
    {"sys_error_resumeLoadFail", "C6: Este ponto de suspensão não pode\nser usado.\n\nApague-o da lista de pontos de suspensão."},
    {"sys_error_gameCrash", "C7: Ocorreu um erro.\n\nSe o problema continuar, anote\no código de erro e acesse\nsupport.nintendo.com."},
    {"sys_error_Unexcepted", "C8: Ocorreu um erro.\n\nSe o problema continuar, anote\no código de erro e acesse\nsupport.nintendo.com."},
    {"sys_error_replayFail", "C9: Os dados de repetição\nestão corrompidos.\n\nUse os dados do ponto de\nsuspensão para iniciar o jogo."},
    {"sys_gametitlelist_hud_Menu", "Menu"},
    {"sys_gametitlelist_hud_Resume", "Lista de pontos de suspensão"},
    {"sys_gametitlelist_hud_Sort", "Ordenar"},
    {"sys_gametitlelist_hud_Decide", "Iniciar jogo"},
    {"sys_language_hud_Title", "Idiomas"},
    {"sys_language_hud_Select", "Selecionar"},
    {"sys_language_hud_Decide", "OK"},
    {"sys_language_hud_Return", "Voltar"},
    {"sys_language_Decide", "OK"},
    {"sys_manual_Title", "Manuais"},
    {"sys_manual_hud_Back", "Voltar"},
    {"sys_manual_Page", "Consulte os manuais dos jogos e outras informações em um celular, tablet ou PC."},
    {"sys_menubarU_cm_Option", "Opções"},
    {"sys_menubarU_cm_Display", "Exibição"},
    {"sys_menubarU_cm_Language", "Idioma"},
    {"sys_menubarU_cm_Copyright", "Avisos legais"},
    {"sys_menubarU_cm_Manual", "Manuais"},
    {"sys_menubarU_hud_Gametitle", "Lista de jogos"},
    {"sys_menubarU_hud_Select", "Selecionar"},
    {"sys_menubarU_hud_Return", "Voltar"},
    {"sys_menubarU_hud_Decide", "OK"},
    {"sys_option_hud_Select", "Selecionar"},
    {"sys_option_hud_Decide", "OK"},
    {"sys_option_hud_Return", "Voltar"},
    {"sys_option_init_Title", "Restaurar o console"},
    {"sys_option_init_cm_Decide", "Restaurar configurações de fábrica"},
    {"sys_option_init_dialog_Confirm",
     "As configurações do console voltarão ao\nestado inicial, e os dados apagados não\npoderão ser recuperados.\nDeseja continuar?"},
    {"sys_option_init_dialog_cm_No", "Cancelar"},
    {"sys_option_init_dialog_cm_Yes", "Restaurar"},
    {"sys_option_Title", "Opções"},
    {"sys_option_settings_Title", "Configurações"},
    {"sys_option_settings_MyPlayDemo", "Demo de repetições"},
    {"sys_option_settings_AutoShutdown", "Desligar automaticamente"},
    {"sys_option_settings_Burn-inPrevention", "Evitar marcas na tela"},
    {"sys_resume_Title", "Lista de pontos de suspensão"},
    {"sys_resume_hud_Lock", "Bloquear/Desbloquear"},
    {"sys_resume_hud_Edit", "Mover/Apagar"},
    {"sys_resume_hud_Select", "Selecionar"},
    {"sys_resume_hud_Change", "Mover/Apagar"},
    {"sys_resume_hud_GameStart", "Iniciar jogo"},
    {"sys_resume_hud_Rollback", "Retroceder"},
    {"sys_resume_hud_Decide", "OK"},
    {"sys_resume_hud_Return", "Voltar"},
    {"sys_resume_hud_Register", "Salvar"},
    {"sys_resume_hud_Substitute", "Segure"},
    {"sys_resume_Replay", "Retroceder"},
    {"sys_resume_1_select_delete_dialog_Confitm",
     "Os dados do ponto de suspensão\nserão apagados e não poderão\nser recuperados. Deseja continuar?"},
    {"sys_resume_select_delete_dialog_cm_No", "Cancelar"},
    {"sys_resume_select_delete_dialog_cm_Yes", "Apagar"},
    {"sys_resume_Empty", "Sem dados"},
    {"sys_sort_order_TitleName", "Título"},
    {"sys_sort_order_Players", "Para 2 jogadores"},
    {"sys_sort_order_RecentPlay", "Jogados recentemente"},
    {"sys_sort_order_PlayCount", "Mais jogados"},
    {"sys_sort_order_ReleaseDate", "Data de lançamento"},
    {"sys_sort_order_Manufacturer", "Editora"},
    {"sys_unlock_attention", "Conclua a primeira fase de\nStar Fox para desbloquear\neste jogo!"},
    {"sys_unlock_dialog_01", "Você desbloqueou o Star Fox 2!"},
};

// Texts that differ between the two consoles' menus.
const std::map<std::string, std::string> k_nes = {
    {"sys_error_resumeBroken", "Os dados do ponto de suspensão\nestão corrompidos e serão apagados.\nEm seguida, você voltará ao Menu HOME."},
    {"sys_option_settings_AutoPlayDemo", "Demo"},
    {"sys_resume_attention", "Se você retomar um jogo a partir de\num ponto de suspensão, os dados\nsalvos serão substituídos."},
    {"sys_resume_explanation", "Ao reiniciar um jogo, um ponto de suspensão será criado."},
};
const std::map<std::string, std::string> k_snes = {
    {"sys_option_settings_AutoPlayDemo", "Demo tradicional"},
    {"sys_resume_attention", "Se você retomar o jogo a partir deste\nponto de suspensão, os dados\nsalvos serão substituídos."},
    {"sys_resume_explanation", "Ao reiniciar um jogo, você pode criar um ponto de suspensão."},
};

const char* const k_label = "Português (Brasil)";

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\n') o += "\\n";
        else if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else o += c;
    }
    return o;
}

fs::path P(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string to_u8(const fs::path& p) {
    auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

// One `"key": "value",` line, split around the value. (Hand-written: MSVC's std::regex recurses per
// character and overflows the stack on the long copyright texts.)
struct Entry {
    std::string key, prefix, suffix;  // prefix = everything before the value's opening quote
};
bool parse_entry(const std::string& s, Entry& e) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= s.size() || s[i] != '"') return false;
    size_t k0 = ++i;
    while (i < s.size() && s[i] != '"') i++;
    if (i >= s.size()) return false;
    e.key = s.substr(k0, i - k0);
    i++;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= s.size() || s[i] != ':') return false;
    i++;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= s.size() || s[i] != '"') return false;
    e.prefix = s.substr(0, i);
    i++;
    while (i < s.size() && s[i] != '"') i += (s[i] == '\\') ? 2 : 1;
    if (i >= s.size()) return false;
    e.suffix = s.substr(i + 1);
    return true;
}

// Rewrite one strings file; returns the number of values replaced.
int patch_file(const fs::path& src, const fs::path& dst, bool is_pt, bool nes) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return 0;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str(), out;
    int n = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos + 1);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        std::string body = line, eol;
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) eol.insert(eol.begin(), body.back()), body.pop_back();
        Entry e;
        if (parse_entry(body, e)) {
            const std::string* value = nullptr;
            std::string label = k_label;
            if (e.key == "sys_language_EUR_pt") value = &label;
            else if (is_pt) {
                auto& specific = nes ? k_nes : k_snes;
                if (auto it = specific.find(e.key); it != specific.end()) value = &it->second;
                else if (auto it2 = k_common.find(e.key); it2 != k_common.end()) value = &it2->second;
            }
            if (value) {
                body = e.prefix + "\"" + json_escape(*value) + "\"" + e.suffix;
                n++;
            }
        }
        out += body + eol;
    }
    if (!n) return 0;
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    std::ofstream(dst, std::ios::binary) << out;
    return n;
}

// ---------------------------------------------------------------- shutdown splash
// The splash is a raw 1280x720 BGRA framebuffer image, compressed without any header: an LZ4
// block (SNES) or an LZO1X stream (NES).
constexpr int SW = 1280, SH = 720;

bool lz4_decode(const std::vector<u8>& s, std::vector<u8>& out) {
    size_t i = 0, n = s.size();
    out.clear();
    while (i < n) {
        u8 tok = s[i++];
        size_t lit = tok >> 4;
        if (lit == 15)
            for (u8 b = 255; b == 255 && i < n;) lit += (b = s[i++]);
        if (i + lit > n) return false;
        out.insert(out.end(), s.begin() + i, s.begin() + i + lit);
        i += lit;
        if (i >= n) break;
        if (i + 2 > n) return false;
        size_t off = s[i] | (s[i + 1] << 8);
        i += 2;
        size_t ml = tok & 15;
        if (ml == 15)
            for (u8 b = 255; b == 255 && i < n;) ml += (b = s[i++]);
        ml += 4;
        if (!off || off > out.size() || out.size() + ml > (size_t)SW * SH * 4) return false;
        size_t st = out.size() - off;
        for (size_t k = 0; k < ml; k++) out.push_back(out[st + k]);
    }
    return true;
}

bool lzo1x_decode(const std::vector<u8>& s, std::vector<u8>& out) {
    size_t ip = 0, n = s.size();
    out.clear();
    auto need = [&](size_t k) { return ip + k <= n; };
    auto match = [&](size_t dist, size_t len) {
        if (!dist || dist > out.size() || out.size() + len > (size_t)SW * SH * 4) return false;
        size_t st = out.size() - dist;
        for (size_t k = 0; k < len; k++) out.push_back(out[st + k]);
        return true;
    };
    auto literals = [&](size_t len) {
        if (!need(len)) return false;
        out.insert(out.end(), s.begin() + ip, s.begin() + ip + len);
        ip += len;
        return true;
    };
    if (!need(1)) return false;
    int state = 0;
    if (s[0] > 17) {
        size_t t = s[ip++] - 17;
        if (!literals(t)) return false;
        state = t >= 4 ? 4 : (int)t;
    }
    for (;;) {
        if (!need(1)) return false;
        size_t t = s[ip++], st;
        if (t < 16) {
            if (state == 0) {
                if (t == 0) {
                    while (need(1) && s[ip] == 0) t += 255, ip++;
                    if (!need(1)) return false;
                    t += 15 + s[ip++];
                }
                if (!literals(t + 3)) return false;
                state = 4;
                continue;
            }
            if (!need(1)) return false;
            size_t d = 1 + (t >> 2) + ((size_t)s[ip++] << 2);
            if (state == 4 ? !match(d + 0x800, 3) : !match(d, 2)) return false;
            st = t & 3;
        } else if (t >= 64) {
            if (!need(1)) return false;
            size_t d = 1 + ((t >> 2) & 7) + ((size_t)s[ip++] << 3);
            if (!match(d, (t >> 5) + 1)) return false;
            st = t & 3;
        } else if (t >= 32) {
            size_t l = t & 31;
            if (!l) {
                while (need(1) && s[ip] == 0) l += 255, ip++;
                if (!need(1)) return false;
                l += 31 + s[ip++];
            }
            if (!need(2)) return false;
            size_t v = s[ip] | (s[ip + 1] << 8);
            ip += 2;
            if (!match(1 + (v >> 2), l + 2)) return false;
            st = v & 3;
        } else {
            size_t l = t & 7;
            if (!l) {
                while (need(1) && s[ip] == 0) l += 255, ip++;
                if (!need(1)) return false;
                l += 7 + s[ip++];
            }
            if (!need(2)) return false;
            size_t v = s[ip] | (s[ip + 1] << 8);
            ip += 2;
            size_t d = ((t & 8) << 11) + (v >> 2);
            if (!d) break;  // end of stream
            if (!match(d + 0x4000, l + 2)) return false;
            st = v & 3;
        }
        if (st && !literals(st)) return false;
        state = (int)st;
    }
    return true;
}

// Re-compression as one run of literals: valid for any LZ4 / LZO1X decoder, ~3.7 MB per image.
std::vector<u8> lz4_encode_literals(const std::vector<u8>& raw) {
    std::vector<u8> o;
    size_t len = raw.size();
    o.push_back(0xf0);
    size_t rest = len - 15;
    while (rest >= 255) o.push_back(255), rest -= 255;
    o.push_back((u8)rest);
    o.insert(o.end(), raw.begin(), raw.end());
    return o;
}

std::vector<u8> lzo1x_encode_literals(const std::vector<u8>& raw) {
    std::vector<u8> o;
    size_t tt = raw.size() - 18;  // raw.size() > 18
    o.push_back(0);
    while (tt > 255) o.push_back(0), tt -= 255;
    o.push_back((u8)tt);
    o.insert(o.end(), raw.begin(), raw.end());
    o.insert(o.end(), {0x11, 0x00, 0x00});  // end-of-stream marker
    return o;
}

// The emulator's own 7x7 pixel font (drawn in 3x3 screen-pixel units, 8-unit advance), in the
// style of the console's splash lettering.
const std::map<char, const char*> k_glyphs = {
    {'D', "#####..|##..##.|##...##|##...##|##...##|##..##.|#####.."},
    {'E', "#######|##.....|##.....|######.|##.....|##.....|#######"},
    {'S', ".####..|##..##.|##.....|.#####.|.....##|##...##|.#####."},
    {'L', "##.....|##.....|##.....|##.....|##.....|##.....|#######"},
    {'I', ".######|...##..|...##..|...##..|...##..|...##..|.######"},
    {'G', "..#####|.##....|##.....|##..###|##...##|.##..##|..#####"},
    {'A', "..###..|.##.##.|##...##|##...##|#######|##...##|##...##"},
    {'N', "##...##|###..##|####.##|#######|##.####|##..###|##...##"},
    {'O', ".#####.|##...##|##...##|##...##|##...##|##...##|.#####."},
    {'.', ".......|.......|.......|.......|.......|...##..|...##.."},
};

// Redraw the "SHUTTING DOWN..." line (white on black, rows 351-372) as "DESLIGANDO...".
bool redraw_splash(std::vector<u8>& px) {
    const int ty = 351, unit = 3, adv = 8 * unit, cx2 = 448 + 823;  // original text box centre * 2
    // sanity check: the original has white text in that band and black around it
    int white = 0;
    for (int y = ty; y < ty + 21; y++)
        for (int x = 440; x < 840; x++) white += px[((size_t)y * SW + x) * 4 + 1] > 128;
    if (white < 500) return false;
    for (int y = ty - 6; y < ty + 27; y++)
        for (int x = 420; x < 860; x++) {
            u8* p = &px[((size_t)y * SW + x) * 4];
            p[0] = p[1] = p[2] = 0;
            p[3] = 255;
        }
    const std::string text = "DESLIGANDO...";
    int width = (int)text.size() * adv - unit;
    int x0 = (cx2 - width) / 2;
    for (size_t i = 0; i < text.size(); i++) {
        const char* g = k_glyphs.at(text[i]);
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 7; c++) {
                if (g[r * 8 + c] != '#') continue;
                for (int dy = 0; dy < unit; dy++)
                    for (int dx = 0; dx < unit; dx++) {
                        int x = x0 + (int)i * adv + c * unit + dx, y = ty + r * unit + dy;
                        u8* p = &px[((size_t)y * SW + x) * 4];
                        p[0] = p[1] = p[2] = p[3] = 255;
                    }
            }
    }
    return true;
}

bool console_lang_is_pt(const std::string& datadir) {
    std::ifstream f(P(datadir) / "clover/profiles/0/lang.txt");
    std::string s;
    std::getline(f, s);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s.find("pt") != std::string::npos;
}

// The translated splash is used whenever the console's saved language is Portuguese at the moment
// the firmware opens it, so choosing Português during a session already applies to its shutdown.
void apply_splash(const fs::path& root, const fs::path& outroot, const std::string& datadir) {
    for (const char* rel : {"usr/share/clover-mcp/splash/shutdown.lz4", "usr/share/clover-mcp/splash/shutdown.lzo"}) {
        fs::path src = root / rel;
        std::ifstream in(src, std::ios::binary);
        if (!in) continue;
        std::vector<u8> data((std::istreambuf_iterator<char>(in)), {}), raw;
        bool lz4 = src.extension() == ".lz4";
        if (!(lz4 ? lz4_decode(data, raw) : lzo1x_decode(data, raw)) || raw.size() != (size_t)SW * SH * 4) {
            LOGI("[lang] %s: unexpected splash format, left in English\n", rel);
            continue;
        }
        if (!redraw_splash(raw)) {
            LOGI("[lang] %s: text not where expected, left in English\n", rel);
            continue;
        }
        std::vector<u8> enc = lz4 ? lz4_encode_literals(raw) : lzo1x_encode_literals(raw);
        fs::path dst = outroot / rel;
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        std::ofstream(dst, std::ios::binary).write((const char*)enc.data(), (std::streamsize)enc.size());
        vfs::add_conditional_override(std::string("/") + rel, to_u8(dst), [datadir] { return console_lang_is_pt(datadir); });
        LOGI("[lang] shutdown screen: DESLIGANDO... when the console is in Portuguese\n");
    }
}

}  // namespace

void apply_ptbr(const std::string& rootfs, const std::string& rundir, const std::string& datadir) {
    fs::path root = P(rootfs), outroot = P(rundir) / "lang";
    std::vector<std::pair<fs::path, bool>> dirs;  // strings folders, is-NES
    std::error_code ec;
    dirs.push_back({root / "usr/share/clover-ui/resources/strings", true});
    for (auto& d : fs::directory_iterator(root / "usr/share/ui", ec))
        if (d.is_directory()) dirs.push_back({d.path() / "resources/strings", false});
    int files = 0, values = 0;
    for (auto& [dir, nes] : dirs) {
        for (auto& l : fs::directory_iterator(dir, ec)) {
            if (!l.is_directory()) continue;
            bool is_pt = l.path().filename() == "eur_pt";
            for (const char* name : {"strings.json", "strings.lng"}) {
                fs::path src = l.path() / name;
                if (!fs::is_regular_file(src, ec)) continue;
                std::string guest = to_u8(fs::relative(src, root, ec));
                if (ec || guest.empty()) continue;
                guest = "/" + guest;
                fs::path dst = outroot / P(guest.substr(1));
                int n = patch_file(src, dst, is_pt, nes);
                if (n) {
                    vfs::add_override(guest, to_u8(dst));
                    files++;
                    values += n;
                }
            }
        }
    }
    if (files) LOGI("[lang] Português (Brasil): %d texts in %d menu files\n", values, files);
    apply_splash(root, outroot, datadir);
}
}  // namespace lang
