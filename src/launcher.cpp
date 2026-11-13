// Launcher screens, rendered on the CPU (anti-aliased signed-distance shapes + stb_truetype text)
// and shown through display::present_rgba. Everything is drawn in code: no image assets.
#include "launcher.h"
#include "common.h"
#include "display.h"
#include "ui.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <tuple>

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "ext/stb_truetype.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace fs = std::filesystem;

namespace launcher {
namespace {

// ---------------------------------------------------------------- colors and canvas
struct C {
    float r, g, b, a;
};
constexpr C hex(u32 rgb, float a = 1) {
    return {((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a};
}
C mix(const C& x, const C& y, float t) {
    return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t};
}
constexpr float kPi = 3.14159265f;
float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float smooth(float e0, float e1, float x) {
    float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}

float sd_rrect(float px, float py, float cx, float cy, float hx, float hy, float r) {
    float qx = std::fabs(px - cx) - (hx - r), qy = std::fabs(py - cy) - (hy - r);
    float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

struct Canvas {
    int w = 0, h = 0;
    std::vector<u8> px;

    void resize(int W, int H) {
        if (W == w && H == h) return;
        w = W;
        h = H;
        px.assign((size_t)w * h * 4, 0);
    }
    void blend(int x, int y, const C& c, float a) {
        if (a <= 0.003f) return;
        if (a > 1) a = 1;
        u8* p = &px[((size_t)y * w + x) * 4];
        float ia = 1 - a;
        p[0] = (u8)(c.r * 255 * a + p[0] * ia + 0.5f);
        p[1] = (u8)(c.g * 255 * a + p[1] * ia + 0.5f);
        p[2] = (u8)(c.b * 255 * a + p[2] * ia + 0.5f);
        p[3] = 255;
    }
    void vgradient(const C& top, const C& bot) {
        for (int y = 0; y < h; y++) {
            C c = mix(top, bot, h > 1 ? (float)y / (h - 1) : 0);
            u8 r = (u8)(c.r * 255), g = (u8)(c.g * 255), b = (u8)(c.b * 255);
            u8* p = &px[(size_t)y * w * 4];
            for (int x = 0; x < w; x++, p += 4) {
                p[0] = r;
                p[1] = g;
                p[2] = b;
                p[3] = 255;
            }
        }
    }
    // Soft round light spot (background ambience).
    void glow(float cx, float cy, float rad, const C& c, float alpha) {
        int x0 = std::max(0, (int)(cx - rad)), x1 = std::min(w, (int)(cx + rad) + 1);
        int y0 = std::max(0, (int)(cy - rad)), y1 = std::min(h, (int)(cy + rad) + 1);
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy) / rad;
                if (d < 1) blend(x, y, c, alpha * (1 - smooth(0, 1, d)));
            }
    }
    // Rounded rectangle: filled (stroke == 0) or outlined (stroke = line width, centred on the edge),
    // with a vertical gradient from `top` to `bot`.
    void rrect(float x, float y, float rw, float rh, float r, const C& top, const C& bot, float alpha = 1, float stroke = 0) {
        if (rw <= 0 || rh <= 0) return;
        float hx = rw / 2, hy = rh / 2, cx = x + hx, cy = y + hy;
        r = std::min(r, std::min(hx, hy));
        float m = 2 + stroke;
        int x0 = std::max(0, (int)std::floor(x - m)), x1 = std::min(w, (int)std::ceil(x + rw + m));
        int y0 = std::max(0, (int)std::floor(y - m)), y1 = std::min(h, (int)std::ceil(y + rh + m));
        bool grad = top.r != bot.r || top.g != bot.g || top.b != bot.b || top.a != bot.a;
        for (int py = y0; py < y1; py++) {
            C c = grad ? mix(top, bot, clamp01((py + 0.5f - y) / rh)) : top;
            for (int pxi = x0; pxi < x1; pxi++) {
                float d = sd_rrect(pxi + 0.5f, py + 0.5f, cx, cy, hx, hy, r);
                float cov = stroke > 0 ? clamp01(stroke * 0.5f + 0.5f - std::fabs(d)) : clamp01(0.5f - d);
                if (cov > 0) blend(pxi, py, c, cov * alpha * c.a);
            }
        }
    }
    void rrect(float x, float y, float rw, float rh, float r, const C& c, float alpha = 1) { rrect(x, y, rw, rh, r, c, c, alpha); }
    void circle(float cx, float cy, float rad, const C& c, float alpha = 1) {
        rrect(cx - rad, cy - rad, 2 * rad, 2 * rad, rad, c, alpha);
    }
    void ring(float cx, float cy, float rad, float thick, const C& c, float alpha = 1) {
        rrect(cx - rad, cy - rad, 2 * rad, 2 * rad, rad, c, c, alpha, thick);
    }
    void shadow(float x, float y, float rw, float rh, float r, float blur, float alpha) {
        float hx = rw / 2, hy = rh / 2, cx = x + hx, cy = y + hy;
        r = std::min(r, std::min(hx, hy));
        int x0 = std::max(0, (int)(x - blur - 2)), x1 = std::min(w, (int)(x + rw + blur + 2));
        int y0 = std::max(0, (int)(y - blur - 2)), y1 = std::min(h, (int)(y + rh + blur + 2));
        C k{0, 0, 0, 1};
        for (int py = y0; py < y1; py++)
            for (int pxi = x0; pxi < x1; pxi++) {
                float d = sd_rrect(pxi + 0.5f, py + 0.5f, cx, cy, hx, hy, r);
                blend(pxi, py, k, alpha * (1 - smooth(-blur * 0.5f, blur, d)));
            }
    }
    void darken(float a) {  // a = 0 (unchanged) .. 1 (black)
        if (a <= 0) return;
        int k = (int)((1 - clamp01(a)) * 256);
        for (size_t i = 0; i < px.size(); i += 4) {
            px[i] = (u8)((px[i] * k) >> 8);
            px[i + 1] = (u8)((px[i + 1] * k) >> 8);
            px[i + 2] = (u8)((px[i + 2] * k) >> 8);
        }
    }
};

// ---------------------------------------------------------------- fonts
struct Font {
    std::vector<u8> data;
    stbtt_fontinfo info{};
};

std::unique_ptr<Font> load_font(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return nullptr;
    auto font = std::make_unique<Font>();
    font->data.assign(std::istreambuf_iterator<char>(f), {});
    if (font->data.size() < 64) return nullptr;
    int off = stbtt_GetFontOffsetForIndex(font->data.data(), 0);
    if (off < 0 || !stbtt_InitFont(&font->info, font->data.data(), off)) return nullptr;
    return font;
}

struct Glyph {
    std::vector<u8> bmp;
    int w = 0, h = 0, xoff = 0, yoff = 0;
    float advance = 0;
};

struct Text {
    std::vector<std::unique_ptr<Font>> regular, bold;  // fallback chains
    std::map<std::tuple<const Font*, int, int>, Glyph> cache;
    bool ok = false;

    void init() {
        std::string lang = ui::lang_code();
        std::vector<std::pair<std::string, std::string>> latin, cjk, extra;  // {regular, bold}
#ifdef _WIN32
        std::string dir = std::string(getenv("WINDIR") ? getenv("WINDIR") : "C:\\Windows") + "\\Fonts\\";
        latin = {{dir + "segoeui.ttf", dir + "segoeuib.ttf"}, {dir + "arial.ttf", dir + "arialbd.ttf"}};
        std::pair<std::string, std::string> ja{dir + "YuGothM.ttc", dir + "YuGothB.ttc"}, ja2{dir + "msgothic.ttc", dir + "msgothic.ttc"},
            hans{dir + "msyh.ttc", dir + "msyhbd.ttc"}, hant{dir + "msjh.ttc", dir + "msjhbd.ttc"},
            ko{dir + "malgun.ttf", dir + "malgunbd.ttf"};
        extra = {{dir + "seguisym.ttf", dir + "seguisym.ttf"}};
#elif defined(__APPLE__)
        latin = {{"/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/Helvetica.ttc"},
                 {"/Library/Fonts/Arial Unicode.ttf", "/Library/Fonts/Arial Unicode.ttf"}};
        std::pair<std::string, std::string> ja{"/System/Library/Fonts/Hiragino Sans GB.ttc", "/System/Library/Fonts/Hiragino Sans GB.ttc"},
            ja2 = ja, hans = ja, hant = ja,
            ko{"/System/Library/Fonts/AppleSDGothicNeo.ttc", "/System/Library/Fonts/AppleSDGothicNeo.ttc"};
#else
        latin = {{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"},
                 {"/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"}};
        std::pair<std::string, std::string> ja{"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                               "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"},
            ja2 = ja, hans = ja, hant = ja, ko = ja;
#endif
        // the user's own script first, so shared Han characters use the right regional forms
        if (lang == "ja") cjk = {ja, ja2, hans, hant, ko};
        else if (lang == "zh-Hant") cjk = {hant, hans, ja, ko};
        else if (lang == "ko") cjk = {ko, hans, ja, hant};
        else cjk = {hans, hant, ja, ko};
        auto add = [&](const std::vector<std::pair<std::string, std::string>>& list) {
            for (auto& [r, b] : list) {
                auto fr = load_font(fs::u8path(r));
                if (!fr) continue;
                auto fb = load_font(fs::u8path(b));
                bold.push_back(fb ? std::move(fb) : load_font(fs::u8path(r)));
                regular.push_back(std::move(fr));
            }
        };
        bool have_latin = false;
        for (auto& l : latin) {
            size_t before = regular.size();
            add({l});
            if (regular.size() > before) {
                have_latin = true;
                break;
            }
        }
        add(cjk);
        add(extra);
        ok = have_latin;
    }

    // font + glyph index for a code point (first font of the chain that has it)
    std::pair<const Font*, int> find(u32 cp, bool b) {
        auto& chain = b ? bold : regular;
        for (auto& f : chain)
            if (int g = stbtt_FindGlyphIndex(&f->info, (int)cp)) return {f.get(), g};
        return {chain.empty() ? nullptr : chain[0].get(), 0};
    }

    const Glyph& glyph(const Font* f, int g, int size) {
        auto key = std::make_tuple(f, g, size);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        Glyph gl;
        float sc = stbtt_ScaleForMappingEmToPixels(&f->info, (float)size);
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&f->info, g, &adv, &lsb);
        gl.advance = adv * sc;
        int w = 0, h = 0, xo = 0, yo = 0;
        if (g) {
            u8* b = stbtt_GetGlyphBitmap(&f->info, sc, sc, g, &w, &h, &xo, &yo);
            if (b) {
                gl.bmp.assign(b, b + (size_t)w * h);
                stbtt_FreeBitmap(b, nullptr);
            }
        }
        gl.w = w;
        gl.h = h;
        gl.xoff = xo;
        gl.yoff = yo;
        return cache.emplace(key, std::move(gl)).first->second;
    }

    static std::vector<u32> decode(const std::string& s) {
        std::vector<u32> out;
        for (size_t i = 0; i < s.size();) {
            u8 c = (u8)s[i];
            u32 cp;
            int n;
            if (c < 0x80) cp = c, n = 1;
            else if ((c >> 5) == 6) cp = c & 31, n = 2;
            else if ((c >> 4) == 14) cp = c & 15, n = 3;
            else cp = c & 7, n = 4;
            for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | ((u8)s[i + k] & 63);
            out.push_back(cp);
            i += n;
        }
        return out;
    }

    float width(const std::vector<u32>& cps, int size, bool b) {
        float x = 0;
        for (u32 cp : cps) {
            auto [f, g] = find(cp, b);
            if (f) x += glyph(f, g, size).advance;
        }
        return x;
    }
    float width(const std::string& s, int size, bool b) { return width(decode(s), size, b); }

    // Shorten with an ellipsis to fit max_w.
    std::string fit(const std::string& s, int size, bool b, float max_w) {
        auto cps = decode(s);
        if (width(cps, size, b) <= max_w) return s;
        const u32 ell = 0x2026;
        while (!cps.empty()) {
            cps.pop_back();
            auto t = cps;
            t.push_back(ell);
            if (width(t, size, b) <= max_w) {
                cps = t;
                break;
            }
        }
        std::string out;
        for (u32 cp : cps) {
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) out += (char)(0xc0 | (cp >> 6)), out += (char)(0x80 | (cp & 63));
            else if (cp < 0x10000)
                out += (char)(0xe0 | (cp >> 12)), out += (char)(0x80 | ((cp >> 6) & 63)), out += (char)(0x80 | (cp & 63));
            else
                out += (char)(0xf0 | (cp >> 18)), out += (char)(0x80 | ((cp >> 12) & 63)),
                    out += (char)(0x80 | ((cp >> 6) & 63)), out += (char)(0x80 | (cp & 63));
        }
        return out;
    }

    // Draw at baseline y; align: 0 left, 1 centre, 2 right. Returns the width.
    float draw(Canvas& cv, const std::string& s, float x, float y, int size, bool b, const C& c, int align = 0, float alpha = 1) {
        auto cps = decode(s);
        float w = width(cps, size, b);
        if (align == 1) x -= w / 2;
        else if (align == 2) x -= w;
        float pen = x;
        for (u32 cp : cps) {
            auto [f, g] = find(cp, b);
            if (!f) continue;
            const Glyph& gl = glyph(f, g, size);
            int gx = (int)std::lround(pen) + gl.xoff, gy = (int)std::lround(y) + gl.yoff;
            for (int j = 0; j < gl.h; j++) {
                int yy = gy + j;
                if (yy < 0 || yy >= cv.h) continue;
                for (int i = 0; i < gl.w; i++) {
                    int xx = gx + i;
                    if (xx < 0 || xx >= cv.w) continue;
                    u8 v = gl.bmp[(size_t)j * gl.w + i];
                    if (v) cv.blend(xx, yy, c, v / 255.0f * alpha * c.a);
                }
            }
            pen += gl.advance;
        }
        return w;
    }
};

Text g_text;
Canvas g_cv;
bool g_init = false;

bool init() {
    if (!g_init) {
        g_init = true;
        g_text.init();
    }
    return g_text.ok;
}

// Canvas sized to the window's drawable area, capped at 1920x1080 (stretched with linear filtering).
void fit_canvas() {
    int dw = 1280, dh = 720;
    display::drawable_size(dw, dh);
    if (dw < 16 || dh < 16) dw = 1280, dh = 720;
    float s = std::min({1.0f, 1920.0f / dw, 1080.0f / dh});
    g_cv.resize(std::max(16, (int)std::lround(dw * s)), std::max(16, (int)std::lround(dh * s)));
}

// ---------------------------------------------------------------- palette
const C BG_TOP = hex(0x2a2a31), BG_BOT = hex(0x141418);
const C CARD = hex(0x2f2f37), CARD_SEL = hex(0x393943);
const C TEXT = hex(0xf4f4f6), TEXT_DIM = hex(0x9d9daa), LINE = hex(0x3d3d46);
const C RED = hex(0xe60012), RED_DARK = hex(0x9e0b1c);
const C SHELL = hex(0xe6e6ea), SHELL_DARK = hex(0x3b3b43), INK = hex(0x1c1c21);
const C CYAN_A = hex(0x00d0ff), CYAN_B = hex(0x8ff3ff);

void background(float H, float W) {
    g_cv.vgradient(BG_TOP, BG_BOT);
    g_cv.glow(W * 0.5f, H * 0.58f, H * 0.75f, hex(0x4a4a58), 0.35f);
}

// red app-name badge (top left) and clock (top right)
void header(float W, float H) {
    int fs = (int)(H * 0.026f);
    float tw = g_text.width(ui::APP_NAME, fs, true);
    float bh = H * 0.052f, bx = W * 0.04f, by = H * 0.045f;
    g_cv.rrect(bx, by, tw + bh * 1.1f, bh, bh / 2, RED, RED_DARK);
    g_text.draw(g_cv, ui::APP_NAME, bx + bh * 0.55f, by + bh * 0.5f + fs * 0.36f, fs, true, TEXT);
    std::time_t now = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    char clock[16];
    std::strftime(clock, sizeof clock, "%H:%M", &lt);
    g_text.draw(g_cv, clock, W * 0.96f, by + bh * 0.5f + fs * 0.42f, (int)(H * 0.032f), false, TEXT, 2);
    g_cv.rrect(W * 0.04f, H * 0.135f, W * 0.92f, std::max(1.0f, H * 0.0018f), 0, LINE);
}

// ---------------------------------------------------------------- console artwork
enum Kind { NES, SNES, GENERIC, OTHER };

Kind kind_of(const std::string& stem, bool& usa) {
    std::string s = stem;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    usa = s.find("usa") != std::string::npos;
    if (s.find("snes") != std::string::npos || s.find("shvc") != std::string::npos || s.find("sfc") != std::string::npos) return SNES;
    if (s.find("nes") != std::string::npos || s.find("hvc") != std::string::npos || s.find("famicom") != std::string::npos) return NES;
    return GENERIC;
}

void dpad(float cx, float cy, float arm, float thick, const C& c) {
    g_cv.rrect(cx - arm, cy - thick / 2, 2 * arm, thick, thick * 0.18f, c);
    g_cv.rrect(cx - thick / 2, cy - arm, thick, 2 * arm, thick * 0.18f, c);
}

// flat controller with a dark face plate, d-pad and two round buttons
void pad_flat(float cx, float cy, float w, const C& btn) {
    float h = w * 0.42f, x = cx - w / 2, y = cy - h / 2;
    g_cv.shadow(x, y + h * 0.08f, w, h, h * 0.14f, h * 0.18f, 0.35f);
    g_cv.rrect(x, y, w, h, h * 0.14f, SHELL, hex(0xcfcfd5));
    float in = h * 0.13f;
    g_cv.rrect(x + in, y + in, w - 2 * in, h - 2 * in, h * 0.08f, INK);
    dpad(x + w * 0.2f, cy, h * 0.26f, h * 0.11f, hex(0x5a5a62));
    g_cv.rrect(cx - w * 0.11f, cy + h * 0.06f, w * 0.08f, h * 0.07f, h * 0.035f, hex(0x8a8a93));
    g_cv.rrect(cx + w * 0.02f, cy + h * 0.06f, w * 0.08f, h * 0.07f, h * 0.035f, hex(0x8a8a93));
    for (float bx : {0.70f, 0.84f}) {
        g_cv.circle(x + w * bx, cy + h * 0.04f, h * 0.115f, btn);
        g_cv.ring(x + w * bx, cy + h * 0.04f, h * 0.115f, h * 0.025f, hex(0xf0f0f2), 0.6f);
    }
}

void art_nes(float ax, float ay, float aw, float ah) {
    float bw = aw * 0.62f, bh = aw * 0.27f, x = ax + (aw - bw) / 2, y = ay + ah * 0.18f;
    g_cv.shadow(x, y + bh * 0.1f, bw, bh, bh * 0.08f, bh * 0.22f, 0.4f);
    g_cv.rrect(x, y, bw, bh, bh * 0.08f, SHELL_DARK);
    g_cv.rrect(x, y, bw, bh * 0.58f, bh * 0.08f, SHELL, hex(0xd4d4da));
    g_cv.rrect(x + bw * 0.30f, y + bh * 0.16f, bw * 0.62f, bh * 0.08f, bh * 0.04f, hex(0xc3c3ca));
    g_cv.rrect(x + bw * 0.30f, y + bh * 0.32f, bw * 0.62f, bh * 0.05f, bh * 0.025f, hex(0xcbcbd1));
    for (float px : {0.10f, 0.25f}) g_cv.rrect(x + bw * px, y + bh * 0.68f, bw * 0.10f, bh * 0.18f, bh * 0.03f, INK);
    g_cv.rrect(x + bw * 0.07f, y + bh * 0.22f, bw * 0.035f, bh * 0.08f, bh * 0.02f, RED);
    pad_flat(ax + aw * 0.5f, ay + ah * 0.78f, aw * 0.46f, RED);
}

void art_snes(float ax, float ay, float aw, float ah, bool usa) {
    float bw = aw * 0.58f, bh = aw * 0.28f, x = ax + (aw - bw) / 2, y = ay + ah * 0.16f;
    g_cv.shadow(x, y + bh * 0.1f, bw, bh, bh * 0.2f, bh * 0.22f, 0.4f);
    g_cv.rrect(x, y, bw, bh, bh * 0.2f, hex(0xdcdce2), hex(0xc4c4cc));
    g_cv.rrect(x + bw * 0.06f, y + bh * 0.08f, bw * 0.88f, bh * 0.62f, bh * 0.16f, hex(0xe9e9ee), hex(0xd2d2d9));
    g_cv.rrect(x + bw * 0.28f, y + bh * 0.2f, bw * 0.44f, bh * 0.12f, bh * 0.05f, hex(0x3d3d45));
    C sw = usa ? hex(0x6b5bb0) : hex(0x9a9aa3);
    g_cv.rrect(x + bw * 0.12f, y + bh * 0.48f, bw * 0.13f, bh * 0.11f, bh * 0.05f, sw);
    g_cv.rrect(x + bw * 0.75f, y + bh * 0.48f, bw * 0.13f, bh * 0.11f, bh * 0.05f, sw);
    g_cv.circle(x + bw * 0.08f, y + bh * 0.82f, bh * 0.025f, RED);
    // dog-bone controller
    float w = aw * 0.50f, h = w * 0.36f, cx = ax + aw * 0.5f, cy = ay + ah * 0.78f, l = cx - w / 2 + h / 2, r = cx + w / 2 - h / 2;
    g_cv.shadow(cx - w / 2, cy - h / 2 + h * 0.08f, w, h, h / 2, h * 0.18f, 0.35f);
    g_cv.rrect(cx - w / 2, cy - h / 2, w, h, h / 2, hex(0xe2e2e8), hex(0xcacad1));
    dpad(l, cy, h * 0.27f, h * 0.11f, hex(0x3f3f47));
    float o = h * 0.2f, br = h * 0.095f;
    C top = usa ? hex(0xb4a8e0) : hex(0x2d6cdf), left = usa ? hex(0xb4a8e0) : hex(0x2fa84f);
    C right = usa ? hex(0x5a4aa0) : RED, bottom = usa ? hex(0x5a4aa0) : hex(0xf2c230);
    g_cv.circle(r, cy - o, br, top);
    g_cv.circle(r - o, cy, br, left);
    g_cv.circle(r + o, cy, br, right);
    g_cv.circle(r, cy + o, br, bottom);
    g_cv.rrect(cx - w * 0.09f, cy + h * 0.05f, w * 0.07f, h * 0.07f, h * 0.035f, hex(0x8a8a93));
    g_cv.rrect(cx + w * 0.02f, cy + h * 0.05f, w * 0.07f, h * 0.07f, h * 0.035f, hex(0x8a8a93));
}

void art_generic(float ax, float ay, float aw, float ah) { pad_flat(ax + aw * 0.5f, ay + ah * 0.5f, aw * 0.6f, RED); }

void art_other(float ax, float ay, float aw, float ah) {
    float fw = aw * 0.42f, fh = fw * 0.72f, x = ax + (aw - fw) / 2, y = ay + (ah - fh) / 2;
    g_cv.shadow(x, y + fh * 0.06f, fw, fh, fh * 0.08f, fh * 0.16f, 0.35f);
    g_cv.rrect(x, y - fh * 0.1f, fw * 0.42f, fh * 0.3f, fh * 0.07f, hex(0x8f8f9b));
    g_cv.rrect(x, y + fh * 0.06f, fw, fh * 0.94f, fh * 0.08f, hex(0xb4b4c0), hex(0x9a9aa6));
    float cx = x + fw / 2, cy = y + fh * 0.56f, a = fh * 0.2f, t = fh * 0.07f;
    g_cv.rrect(cx - a, cy - t / 2, 2 * a, t, t / 2, hex(0x5b5b66));
    g_cv.rrect(cx - t / 2, cy - a, t, 2 * a, t / 2, hex(0x5b5b66));
}

// ---------------------------------------------------------------- chooser layout
struct Card {
    float x, y, w, h;
};

std::vector<Card> layout(int n, float W, float H) {
    float ch = H * 0.47f, cw = ch * 0.82f, gap = W * 0.03f;
    float total = n * cw + (n - 1) * gap;
    if (total > W * 0.92f) {
        float s = W * 0.92f / total;
        ch *= s;
        cw *= s;
        gap *= s;
        total = W * 0.92f;
    }
    std::vector<Card> r;
    float x = (W - total) / 2, y = H * 0.555f - ch / 2;
    for (int i = 0; i < n; i++) r.push_back({x + i * (cw + gap), y, cw, ch});
    return r;
}

void draw_card(const Card& c0, const Item& it, Kind k, bool usa, float focus, float press, float t, float H) {
    float s = 1 + 0.045f * focus - 0.04f * press;
    float w = c0.w * s, h = c0.h * s, x = c0.x + (c0.w - w) / 2, y = c0.y + (c0.h - h) / 2;
    float r = H * 0.03f * s;
    g_cv.shadow(x, y + H * 0.012f, w, h, r, H * (0.025f + 0.015f * focus), 0.45f + 0.15f * focus);
    if (focus > 0.01f) {  // animated selection frame
        float g = H * 0.009f, pulse = 0.5f + 0.5f * std::sin(t * 4.5f);
        g_cv.rrect(x - g, y - g, w + 2 * g, h + 2 * g, r + g, mix(CYAN_A, CYAN_B, pulse), mix(CYAN_B, CYAN_A, pulse), focus,
                   H * 0.0065f);
    }
    g_cv.rrect(x, y, w, h, r, mix(CARD, CARD_SEL, focus), mix(hex(0x29292f), hex(0x30303a), focus));
    float pad = w * 0.045f, aw = w - 2 * pad, ah = h * 0.66f;
    float ax = x + pad, ay = y + pad;
    C t0, t1;
    switch (k) {
    case NES: t0 = hex(0xd8132b), t1 = hex(0x8a0a1b); break;
    case SNES: t0 = usa ? hex(0x6f5cbc) : hex(0x5a5ab4), t1 = usa ? hex(0x3b2f78) : hex(0x2f2f6e); break;
    case GENERIC: t0 = hex(0x45454f), t1 = hex(0x2a2a31); break;
    default: t0 = hex(0x3c3c45), t1 = hex(0x26262c); break;
    }
    g_cv.rrect(ax, ay, aw, ah, r * 0.7f, t0, t1);
    g_cv.glow(ax + aw * 0.5f, ay + ah * 0.3f, aw * 0.55f, hex(0xffffff), 0.10f);
    switch (k) {
    case NES: art_nes(ax, ay, aw, ah); break;
    case SNES: art_snes(ax, ay, aw, ah, usa); break;
    case GENERIC: art_generic(ax, ay, aw, ah); break;
    default: art_other(ax, ay, aw, ah); break;
    }
    float ty = ay + ah, rest = y + h - ty;
    int fs1 = (int)(H * 0.029f * s), fs2 = (int)(H * 0.02f * s);
    g_text.draw(g_cv, g_text.fit(it.title, fs1, true, aw), x + w / 2, ty + rest * 0.45f, fs1, true, TEXT, 1);
    g_text.draw(g_cv, g_text.fit(it.subtitle, fs2, false, aw), x + w / 2, ty + rest * 0.76f, fs2, false, TEXT_DIM, 1);
}

// four-dot face-button diagram with one dot filled (0 top, 1 right, 2 bottom, 3 left)
float button_hint(float x, float cy, float H, int which) {
    float o = H * 0.0125f, r = H * 0.0068f;
    float cx = x + o + r;
    const float d[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
    for (int i = 0; i < 4; i++) {
        if (i == which) g_cv.circle(cx + d[i][0] * o, cy + d[i][1] * o, r, TEXT);
        else g_cv.ring(cx + d[i][0] * o, cy + d[i][1] * o, r * 0.8f, H * 0.0022f, TEXT_DIM);
    }
    return 2 * (o + r);
}

float key_hint(float x, float cy, float H, const char* key) {
    int fs = (int)(H * 0.018f);
    float tw = g_text.width(key, fs, true), bh = H * 0.032f, bw = std::max(bh, tw + bh * 0.7f);
    g_cv.rrect(x, cy - bh / 2, bw, bh, bh * 0.25f, TEXT_DIM, TEXT_DIM, 1, H * 0.0022f);
    g_text.draw(g_cv, key, x + bw / 2, cy + fs * 0.36f, fs, true, TEXT_DIM, 1);
    return bw;
}

void footer(float W, float H) {
    g_cv.rrect(W * 0.04f, H * 0.875f, W * 0.92f, std::max(1.0f, H * 0.0018f), 0, LINE);
    float cy = H * 0.93f;
    int fs = (int)(H * 0.024f);
    // laid out right to left: [Esc] Quit   (o) OK
    float x = W * 0.96f;
    x -= g_text.width(ui::tr(ui::QUIT), fs, false);
    g_text.draw(g_cv, ui::tr(ui::QUIT), x, cy + fs * 0.36f, fs, false, TEXT);
    x -= H * 0.012f;
    float kw = g_text.width("Esc", (int)(H * 0.018f), true) + H * 0.032f * 0.7f;
    kw = std::max(kw, H * 0.032f);
    x -= kw;
    key_hint(x, cy, H, "Esc");
    x -= H * 0.04f;
    x -= g_text.width(ui::tr(ui::OK), fs, false);
    g_text.draw(g_cv, ui::tr(ui::OK), x, cy + fs * 0.36f, fs, false, TEXT);
    x -= H * 0.012f;
    float bw = 2 * (H * 0.0125f + H * 0.0068f);
    x -= bw;
    button_hint(x, cy, H, 1);
}

// ---------------------------------------------------------------- sounds
struct Sound {
    SDL_AudioDeviceID dev = 0;
    int rate = 48000;
    void open() {
        SDL_AudioSpec want{}, have{};
        want.freq = 48000;
        want.format = AUDIO_F32SYS;
        want.channels = 1;
        want.samples = 512;
        dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (dev) {
            rate = have.freq;
            SDL_PauseAudioDevice(dev, 0);
        }
    }
    void close() {
        if (dev) SDL_CloseAudioDevice(dev);
        dev = 0;
    }
    // short soft "bloop": sine sweep with a quick decay
    void blip(float f0, float f1, float dur, float vol, float delay = 0) {
        if (!dev) return;
        int n0 = (int)(delay * rate), n = (int)(dur * rate);
        std::vector<float> s(n0 + n, 0.0f);
        double ph = 0;
        for (int i = 0; i < n; i++) {
            float t = (float)i / n, f = f0 + (f1 - f0) * t;
            ph += 2 * kPi * f / rate;
            float env = std::min(1.0f, i / (0.004f * rate)) * std::exp(-5.0f * t);
            s[n0 + i] = vol * env * (float)std::sin(ph);
        }
        SDL_QueueAudio(dev, s.data(), (u32)(s.size() * sizeof(float)));
    }
};

}  // namespace

// ---------------------------------------------------------------- public
int choose(const std::vector<Item>& items_in, int def) {
    if (!init()) return -3;
    std::vector<Item> items = items_in;
    items.push_back({ui::tr(ui::OTHER_FILE), ".hsqs", ""});
    int n = (int)items.size();
    std::vector<Kind> kinds;
    std::vector<bool> usa;
    for (int i = 0; i < n; i++) {
        bool u = false;
        kinds.push_back(i == n - 1 ? OTHER : kind_of(items[i].stem, u));
        usa.push_back(u);
    }
    int sel = std::clamp(def, 0, n - 1);
    LOGI("[launcher] %d cards, default %d\n", n, sel);
    std::vector<float> focus(n, 0.0f);
    focus[sel] = 1;

    std::vector<SDL_GameController*> pads;
    auto open_pads = [&] {
        for (int i = 0; i < SDL_NumJoysticks(); i++)
            if (SDL_IsGameController(i)) {
                SDL_GameController* p = SDL_GameControllerOpen(i);
                if (p && std::find(pads.begin(), pads.end(), p) == pads.end()) pads.push_back(p);
            }
    };
    open_pads();
    int cursor_before = SDL_ShowCursor(SDL_QUERY);
    SDL_ShowCursor(SDL_ENABLE);
    SDL_RaiseWindow(display::window());
    Sound snd;
    snd.open();

    int result = -4;  // pending
    u64 start = SDL_GetTicks64(), confirm_at = 0, last = start;
    int stick_dir = 0;
    u64 stick_next = 0;
    float press = 0;
    std::vector<Card> cards;
    float mscale_x = 1, mscale_y = 1;

    auto move = [&](int d) {
        int ns = std::clamp(sel + d, 0, n - 1);
        if (ns != sel) {
            sel = ns;
            snd.blip(1250, 1500, 0.045f, 0.10f);
        }
    };
    auto confirm = [&] {
        if (confirm_at) return;
        confirm_at = SDL_GetTicks64();
        snd.blip(880, 990, 0.06f, 0.12f);
        snd.blip(1320, 1480, 0.09f, 0.12f, 0.065f);
    };

    for (;;) {
        u64 now = SDL_GetTicks64();
        float dt = std::min(0.05f, (now - last) / 1000.0f), t = (now - start) / 1000.0f;
        last = now;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: result = -2; break;
            case SDL_KEYDOWN:
                if (confirm_at) break;
                switch (e.key.keysym.scancode) {
                case SDL_SCANCODE_LEFT: case SDL_SCANCODE_UP: move(-1); break;
                case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_DOWN: case SDL_SCANCODE_TAB: move(+1); break;
                case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER:
                    if (e.key.keysym.mod & KMOD_ALT) display::toggle_fullscreen(), SDL_ShowCursor(SDL_ENABLE);
                    else confirm();
                    break;
                case SDL_SCANCODE_SPACE: case SDL_SCANCODE_X: case SDL_SCANCODE_K: confirm(); break;
                case SDL_SCANCODE_F11: display::toggle_fullscreen(), SDL_ShowCursor(SDL_ENABLE); break;
                case SDL_SCANCODE_ESCAPE: result = -2; break;
                default: break;
                }
                break;
            case SDL_CONTROLLERDEVICEADDED: open_pads(); break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (confirm_at) break;
                switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: case SDL_CONTROLLER_BUTTON_DPAD_UP: move(-1); break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move(+1); break;
                // confirm with the right face button (Nintendo A), the bottom one (Xbox A) or Start;
                // there is no "back" on the pad, so nobody quits by accident
                case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_START: confirm(); break;
                default: break;
                }
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                    int d = e.caxis.value < -20000 ? -1 : e.caxis.value > 20000 ? 1 : 0;
                    if (d != stick_dir) {
                        stick_dir = d;
                        if (d && !confirm_at) {
                            LOGI("[launcher] stick moved selection (axis value %d)\n", e.caxis.value);
                            move(d);
                        }
                        stick_next = now + 420;
                    }
                }
                break;
            case SDL_MOUSEMOTION:
            case SDL_MOUSEBUTTONDOWN: {
                if (confirm_at) break;
                float mx = (e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x) * mscale_x;
                float my = (e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y) * mscale_y;
                for (int i = 0; i < (int)cards.size(); i++)
                    if (mx >= cards[i].x && mx < cards[i].x + cards[i].w && my >= cards[i].y && my < cards[i].y + cards[i].h) {
                        if (i != sel) {
                            sel = i;
                            snd.blip(1250, 1500, 0.045f, 0.08f);
                        }
                        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) confirm();
                    }
                break;
            }
            default: break;
            }
        }
        if (stick_dir && !confirm_at && now >= stick_next) {
            move(stick_dir);
            stick_next = now + 160;
        }
        if (result != -4) break;

        // animation
        float k = 1 - std::exp(-dt * 14);
        for (int i = 0; i < n; i++) focus[i] += ((i == sel ? 1.0f : 0.0f) - focus[i]) * k;
        float since = confirm_at ? (now - confirm_at) / 1000.0f : 0;
        press = confirm_at ? std::sin(std::min(1.0f, since / 0.16f) * kPi) : 0;

        // draw
        fit_canvas();
        float W = (float)g_cv.w, H = (float)g_cv.h;
        int ww = 1, wh = 1;
        SDL_GetWindowSize(display::window(), &ww, &wh);
        mscale_x = W / std::max(1, ww);
        mscale_y = H / std::max(1, wh);
        background(H, W);
        header(W, H);
        g_text.draw(g_cv, ui::tr(ui::CHOOSE), W / 2, H * 0.235f, (int)(H * 0.043f), true, TEXT, 1);
        cards = layout(n, W, H);
        for (int i = 0; i < n; i++)
            if (i != sel) draw_card(cards[i], items[i], kinds[i], usa[i], focus[i], 0, t, H);
        draw_card(cards[sel], items[sel], kinds[sel], usa[sel], focus[sel], press, t, H);  // selected on top
        footer(W, H);
        g_cv.darken(1 - clamp01(t / 0.3f));   // fade in
        if (confirm_at) g_cv.darken(clamp01((since - 0.12f) / 0.25f));  // fade out
        display::present_rgba(g_cv.px.data(), g_cv.w, g_cv.h);
        if (confirm_at && since > 0.4f) {
            result = sel == n - 1 ? -1 : sel;
            break;
        }
        u64 spent = SDL_GetTicks64() - now;
        if (spent < 16) SDL_Delay((u32)(16 - spent));
    }
    snd.close();
    for (auto* p : pads) SDL_GameControllerClose(p);
    SDL_ShowCursor(cursor_before);
    return result;
}

bool progress(float frac) {
    if (!init()) return false;
    static u64 start = SDL_GetTicks64();
    float t = (SDL_GetTicks64() - start) / 1000.0f;
    fit_canvas();
    float W = (float)g_cv.w, H = (float)g_cv.h;
    background(H, W);
    header(W, H);
    g_text.draw(g_cv, ui::tr(ui::PREPARING), W / 2, H * 0.47f, (int)(H * 0.033f), true, TEXT, 1);
    float bw = W * 0.34f, bh = H * 0.014f, bx = (W - bw) / 2, by = H * 0.53f;
    g_cv.rrect(bx, by, bw, bh, bh / 2, hex(0x3a3a43));
    float fw = std::max(bh, bw * clamp01(frac));
    g_cv.rrect(bx, by, fw, bh, bh / 2, hex(0xff3347), RED);
    // a soft highlight sweeping over the filled part
    float sx = bx + std::fmod(t * 0.9f, 1.4f) / 1.4f * (fw + bh * 6) - bh * 3;
    g_cv.glow(std::min(sx, bx + fw), by + bh / 2, bh * 3, hex(0xffffff), 0.35f);
    char pct[16];
    snprintf(pct, sizeof pct, "%d%%", (int)std::lround(clamp01(frac) * 100));
    g_text.draw(g_cv, pct, W / 2, by + bh + H * 0.045f, (int)(H * 0.024f), false, TEXT_DIM, 1);
    g_cv.darken(1 - clamp01(t / 0.25f));
    display::present_rgba(g_cv.px.data(), g_cv.w, g_cv.h);
    return true;
}

void fade_out() {
    if (!g_text.ok || g_cv.px.empty()) return;
    std::vector<u8> base = g_cv.px;
    for (int i = 1; i <= 12; i++) {
        g_cv.px = base;
        g_cv.darken(i / 12.0f);
        display::present_rgba(g_cv.px.data(), g_cv.w, g_cv.h);
        SDL_Delay(16);
    }
}

// ---------------------------------------------------------------- in-game pause menu
namespace {
// Convex polygon with anti-aliased edges (points in clockwise or counter-clockwise order).
void poly(const std::vector<std::pair<float, float>>& p, const C& c) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (auto& [x, y] : p) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    float area = 0;
    for (size_t i = 0; i < p.size(); i++) {
        auto& a = p[i];
        auto& b = p[(i + 1) % p.size()];
        area += a.first * b.second - b.first * a.second;
    }
    float sgn = area > 0 ? 1.0f : -1.0f;
    for (int y = std::max(0, (int)y0 - 1); y < std::min(g_cv.h, (int)y1 + 2); y++)
        for (int x = std::max(0, (int)x0 - 1); x < std::min(g_cv.w, (int)x1 + 2); x++) {
            float px = x + 0.5f, py = y + 0.5f, d = -1e9f;
            for (size_t i = 0; i < p.size(); i++) {
                auto& a = p[i];
                auto& b = p[(i + 1) % p.size()];
                float ex = b.first - a.first, ey = b.second - a.second, len = std::sqrt(ex * ex + ey * ey);
                if (len < 1e-6f) continue;
                // signed distance to the edge line, positive outside
                d = std::max(d, -sgn * (ex * (py - a.second) - ey * (px - a.first)) / len);
            }
            float cov = clamp01(0.5f - d);
            if (cov > 0) g_cv.blend(x, y, c, cov * c.a);
        }
}

void box_blur(std::vector<u8>& img, int w, int h, int r) {
    if (r < 1) return;
    std::vector<u8> tmp(img.size());
    auto pass = [&](const std::vector<u8>& src, std::vector<u8>& dst, bool horiz) {
        int n = horiz ? w : h, lines = horiz ? h : w;
        for (int l = 0; l < lines; l++) {
            int acc[3] = {0, 0, 0};
            auto at = [&](int i) -> const u8* {
                i = std::clamp(i, 0, n - 1);
                return &src[((size_t)(horiz ? l * w + i : i * w + l)) * 4];
            };
            for (int i = -r; i <= r; i++)
                for (int k = 0; k < 3; k++) acc[k] += at(i)[k];
            for (int i = 0; i < n; i++) {
                u8* d = &dst[((size_t)(horiz ? l * w + i : i * w + l)) * 4];
                for (int k = 0; k < 3; k++) d[k] = (u8)(acc[k] / (2 * r + 1));
                d[3] = 255;
                const u8* add = at(i + r + 1);
                const u8* sub = at(i - r);
                for (int k = 0; k < 3; k++) acc[k] += add[k] - sub[k];
            }
        }
    };
    for (int it = 0; it < 2; it++) {
        pass(img, tmp, true);
        pass(tmp, img, false);
    }
}

// The paused game, letterboxed into the canvas, blurred and dimmed.
std::vector<u8> paused_backdrop(const std::vector<u8>& frame, int fw, int fh) {
    int W = g_cv.w, H = g_cv.h;
    std::vector<u8> out((size_t)W * H * 4, 0);
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    if (!frame.empty() && fw > 0 && fh > 0) {
        int tw = W, th = W * fh / fw;
        if (th > H) th = H, tw = H * fw / fh;
        int ox = (W - tw) / 2, oy = (H - th) / 2;
        for (int y = 0; y < th; y++) {
            int sy = std::min(fh - 1, y * fh / th);
            for (int x = 0; x < tw; x++) {
                int sx = std::min(fw - 1, x * fw / tw);
                memcpy(&out[((size_t)(oy + y) * W + ox + x) * 4], &frame[((size_t)sy * fw + sx) * 4], 4);
            }
        }
        box_blur(out, W, H, std::max(2, H / 180));
    }
    for (size_t i = 0; i < out.size(); i += 4)
        for (int k = 0; k < 3; k++) out[i + k] = (u8)(out[i + k] * 0.42f);
    return out;
}

void icon_play(float cx, float cy, float s, const C& c) {
    poly({{cx - s * 0.32f, cy - s * 0.42f}, {cx + s * 0.45f, cy}, {cx - s * 0.32f, cy + s * 0.42f}}, c);
}
void icon_list(float cx, float cy, float s, const C& c) {
    float q = s * 0.36f, g = s * 0.1f;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) g_cv.rrect(cx - q - g / 2 + i * (q + g), cy - q - g / 2 + j * (q + g), q, q, q * 0.22f, c);
}
void icon_fullscreen(float cx, float cy, float s, const C& c) {
    float a = s * 0.42f, l = s * 0.28f, t = s * 0.11f;
    for (int sx : {-1, 1})
        for (int sy : {-1, 1}) {
            float x = cx + sx * a, y = cy + sy * a;
            g_cv.rrect(std::min(x, x - sx * l), y - t / 2, l, t, t / 2, c);
            g_cv.rrect(x - t / 2, std::min(y, y - sy * l), t, l, t / 2, c);
        }
}
void icon_power(float cx, float cy, float s, const C& c, const C& bg) {
    float r = s * 0.38f, t = s * 0.11f;
    g_cv.ring(cx, cy + s * 0.04f, r, t, c);
    g_cv.rrect(cx - t * 1.6f, cy - r - t, t * 3.2f, r * 0.9f, t, bg);  // gap at the top
    g_cv.rrect(cx - t / 2, cy - r - t * 0.2f, t, r * 0.95f, t / 2, c);
}
}  // namespace

int pause_menu(const std::vector<u8>& frame, int fw, int fh, const std::function<void()>& pump) {
    if (!init()) return MENU_RESUME;
    enum { R_RESUME, R_LIST, R_FULL, R_QUIT, NROWS };
    const ui::Msg labels[NROWS] = {ui::RESUME, ui::GAME_LIST, ui::FULLSCREEN, ui::POWER_OFF};
    int sel = 0;
    float focus[NROWS] = {1, 0, 0, 0};
    std::vector<u8> backdrop;
    int bd_w = 0, bd_h = 0;

    std::vector<SDL_GameController*> pads;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i))
            if (SDL_GameController* p = SDL_GameControllerOpen(i)) pads.push_back(p);
    int cursor_before = SDL_ShowCursor(SDL_QUERY);
    SDL_ShowCursor(SDL_ENABLE);
    Sound snd;
    snd.open();
    snd.blip(990, 880, 0.07f, 0.10f);

    int result = -1;
    u64 start = SDL_GetTicks64(), last = start, confirm_at = 0;
    int confirm_row = -1, stick_dir = 0;
    u64 stick_next = 0;
    std::vector<Card> rows(NROWS);
    float mscale_x = 1, mscale_y = 1;

    auto is_full = [] { return (SDL_GetWindowFlags(display::window()) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0; };
    auto move = [&](int d) {
        int ns = std::clamp(sel + d, 0, NROWS - 1);
        if (ns != sel) {
            sel = ns;
            snd.blip(1250, 1500, 0.045f, 0.10f);
        }
    };
    auto confirm = [&] {
        if (confirm_at) return;
        if (sel == R_FULL) {  // toggles in place
            display::toggle_fullscreen();
            SDL_ShowCursor(SDL_ENABLE);
            snd.blip(1100, 1320, 0.06f, 0.12f);
            return;
        }
        confirm_at = SDL_GetTicks64();
        confirm_row = sel;
        snd.blip(880, 990, 0.06f, 0.12f);
        snd.blip(1320, 1480, 0.09f, 0.12f, 0.065f);
    };
    auto resume = [&] {
        if (confirm_at) return;
        sel = R_RESUME;
        confirm_at = SDL_GetTicks64();
        confirm_row = R_RESUME;
        snd.blip(880, 990, 0.06f, 0.10f);
    };

    for (;;) {
        u64 now = SDL_GetTicks64();
        float dt = std::min(0.05f, (now - last) / 1000.0f), t = (now - start) / 1000.0f;
        last = now;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                sel = R_QUIT;
                confirm_row = R_QUIT;
                confirm_at = now;
                break;
            case SDL_KEYDOWN:
                if (e.key.repeat && e.key.keysym.scancode != SDL_SCANCODE_UP && e.key.keysym.scancode != SDL_SCANCODE_DOWN) break;
                switch (e.key.keysym.scancode) {
                case SDL_SCANCODE_UP: case SDL_SCANCODE_LEFT: move(-1); break;
                case SDL_SCANCODE_DOWN: case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_TAB: move(+1); break;
                case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER:
                    if (e.key.keysym.mod & KMOD_ALT) display::toggle_fullscreen(), SDL_ShowCursor(SDL_ENABLE);
                    else confirm();
                    break;
                case SDL_SCANCODE_SPACE: case SDL_SCANCODE_X: case SDL_SCANCODE_K: confirm(); break;
                case SDL_SCANCODE_F11: display::toggle_fullscreen(), SDL_ShowCursor(SDL_ENABLE); break;
                case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: resume(); break;
                default: break;
                }
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (SDL_IsGameController(e.cdevice.which))
                    if (SDL_GameController* p = SDL_GameControllerOpen(e.cdevice.which)) pads.push_back(p);
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: case SDL_CONTROLLER_BUTTON_DPAD_LEFT: move(-1); break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: move(+1); break;
                case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_B: confirm(); break;
                case SDL_CONTROLLER_BUTTON_START: case SDL_CONTROLLER_BUTTON_GUIDE: resume(); break;
                default: break;
                }
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                    int d = e.caxis.value < -20000 ? -1 : e.caxis.value > 20000 ? 1 : 0;
                    if (d != stick_dir) {
                        stick_dir = d;
                        if (d) move(d);
                        stick_next = now + 420;
                    }
                }
                break;
            case SDL_MOUSEMOTION:
            case SDL_MOUSEBUTTONDOWN: {
                if (confirm_at) break;
                float mx = (e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x) * mscale_x;
                float my = (e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y) * mscale_y;
                for (int i = 0; i < NROWS; i++)
                    if (mx >= rows[i].x && mx < rows[i].x + rows[i].w && my >= rows[i].y && my < rows[i].y + rows[i].h) {
                        if (i != sel) {
                            sel = i;
                            snd.blip(1250, 1500, 0.045f, 0.08f);
                        }
                        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) confirm();
                    }
                break;
            }
            default: break;
            }
        }
        if (stick_dir && !confirm_at && now >= stick_next) {
            move(stick_dir);
            stick_next = now + 160;
        }
        if (pump) pump();

        float k = 1 - std::exp(-dt * 16);
        for (int i = 0; i < NROWS; i++) focus[i] += ((i == sel ? 1.0f : 0.0f) - focus[i]) * k;
        float since = confirm_at ? (now - confirm_at) / 1000.0f : 0;

        fit_canvas();
        float W = (float)g_cv.w, H = (float)g_cv.h;
        if (bd_w != g_cv.w || bd_h != g_cv.h) {
            backdrop = paused_backdrop(frame, fw, fh);
            bd_w = g_cv.w;
            bd_h = g_cv.h;
        }
        int ww = 1, wh = 1;
        SDL_GetWindowSize(display::window(), &ww, &wh);
        mscale_x = W / std::max(1, ww);
        mscale_y = H / std::max(1, wh);
        memcpy(g_cv.px.data(), backdrop.data(), backdrop.size());
        header(W, H);

        // panel pops in, and shrinks away on resume
        float appear = smooth(0, 0.16f, t), leave = confirm_at ? smooth(0.05f, 0.22f, since) : 0;
        float vis = appear * (1 - leave), s = 0.94f + 0.06f * vis;
        float rh = H * 0.085f, gap = H * 0.016f, pad = H * 0.04f, title_h = H * 0.1f;
        float pw = std::max(W * 0.34f, H * 0.62f) * s, ph = (title_h + (float)NROWS * rh + (float)(NROWS - 1) * gap + 2 * pad) * s;
        float px = (W - pw) / 2, py = (H - ph) / 2 + H * 0.02f;
        g_cv.shadow(px, py + H * 0.012f, pw, ph, H * 0.03f, H * 0.04f, 0.5f * vis);
        g_cv.rrect(px, py, pw, ph, H * 0.03f * s, hex(0x2c2c33), hex(0x222228), vis);
        g_text.draw(g_cv, ui::tr(ui::PAUSED), W / 2, py + (pad + title_h * 0.62f) * s, (int)(H * 0.04f * s), true, TEXT, 1, vis);
        for (int i = 0; i < NROWS; i++) {
            float f = focus[i], rs = s * (1 + 0.02f * f);
            float rw = pw - 2 * pad * s, x = px + pad * s, y = py + (pad + title_h + i * (rh + gap)) * s;
            float h = rh * s;
            x -= rw * (rs / s - 1) / 2;
            rw *= rs / s;
            rows[i] = {x, y, rw, h};
            C bg = mix(hex(0x34343c), hex(0x40404b), f);
            if (f > 0.01f) {
                float g = H * 0.007f, pulse = 0.5f + 0.5f * std::sin(t * 4.5f);
                g_cv.rrect(x - g, y - g, rw + 2 * g, h + 2 * g, h * 0.28f + g, mix(CYAN_A, CYAN_B, pulse), mix(CYAN_B, CYAN_A, pulse),
                           f * vis, H * 0.0055f);
            }
            g_cv.rrect(x, y, rw, h, h * 0.28f, bg, bg, vis);
            float ic = h * 0.5f, icx = x + h * 0.62f, icy = y + h / 2;
            C icol = i == R_QUIT ? hex(0xff4d5e) : TEXT;
            icol.a = vis;
            C ibg = bg;
            switch (i) {
            case R_RESUME: icon_play(icx, icy, ic, icol); break;
            case R_LIST: icon_list(icx, icy, ic, icol); break;
            case R_FULL: icon_fullscreen(icx, icy, ic, icol); break;
            default: icon_power(icx, icy, ic, icol, ibg); break;
            }
            int fs = (int)(H * 0.03f * s);
            g_text.draw(g_cv, ui::tr(labels[i]), x + h * 1.2f, icy + fs * 0.36f, fs, false, TEXT, 0, vis);
            if (i == R_FULL) {  // on/off switch
                bool on = is_full();
                float sw = h * 0.9f, sh = h * 0.46f, sx = x + rw - sw - h * 0.3f, sy = icy - sh / 2;
                g_cv.rrect(sx, sy, sw, sh, sh / 2, on ? hex(0x00b4e6) : hex(0x5a5a66), on ? hex(0x00b4e6) : hex(0x5a5a66), vis);
                g_cv.circle(on ? sx + sw - sh / 2 : sx + sh / 2, icy, sh * 0.4f, hex(0xffffff), vis);
            }
        }
        // footer: (o) OK    [START] [Esc] Resume
        g_cv.rrect(W * 0.04f, H * 0.875f, W * 0.92f, std::max(1.0f, H * 0.0018f), 0, LINE);
        float cy = H * 0.93f;
        int fs = (int)(H * 0.024f);
        float x = W * 0.96f;
        x -= g_text.width(ui::tr(ui::RESUME), fs, false);
        g_text.draw(g_cv, ui::tr(ui::RESUME), x, cy + fs * 0.36f, fs, false, TEXT);
        x -= H * 0.012f;
        for (const char* key : {"Esc", "START"}) {
            float kw = std::max(H * 0.032f, g_text.width(key, (int)(H * 0.018f), true) + H * 0.032f * 0.7f);
            x -= kw;
            key_hint(x, cy, H, key);
            x -= H * 0.01f;
        }
        x -= H * 0.03f;
        x -= g_text.width(ui::tr(ui::OK), fs, false);
        g_text.draw(g_cv, ui::tr(ui::OK), x, cy + fs * 0.36f, fs, false, TEXT);
        x -= H * 0.012f + 2 * (H * 0.0125f + H * 0.0068f);
        button_hint(x, cy, H, 1);

        display::present_rgba(g_cv.px.data(), g_cv.w, g_cv.h);
        if (confirm_at && since > 0.24f) {
            result = confirm_row == R_LIST ? MENU_GAME_LIST : confirm_row == R_QUIT ? MENU_QUIT : MENU_RESUME;
            break;
        }
        u64 spent = SDL_GetTicks64() - now;
        if (spent < 16) SDL_Delay((u32)(16 - spent));
    }
    SDL_Delay(60);  // let the confirm sound finish
    snd.close();
    for (auto* p : pads) SDL_GameControllerClose(p);
    SDL_ShowCursor(cursor_before);
    return result;
}
}  // namespace launcher
