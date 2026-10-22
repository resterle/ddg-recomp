// overlay.cpp - options menu drawn over the running game.
//
// Opened and closed with the gamepad's Y button or F10 (the game uses neither). Up/Down select,
// Left/Right change, A/Return on "Close" or B/Esc close. Changes apply at once (the resolution
// after a restart) and are written to graphics.ini when the menu closes. The menu is rasterised
// here with a 5x7 font into a small RGBA image that the renderer draws over the picture
// (CGraphic::SetOverlay). While it is open the game gets no input; it keeps running.

#include "overlay.h"
#include "launcher.h"
#include "../../port/src/unb/Graphic.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// ---- font ------------------------------------------------------------------------------------

struct Glyph { char c; const char *rows; };   // 7 rows of 5 pixels, '#' set

const Glyph kFont[] = {
    {'A', ".###. #...# #...# ##### #...# #...# #...#"}, {'B', "####. #...# #...# ####. #...# #...# ####."},
    {'C', ".###. #...# #.... #.... #.... #...# .###."}, {'D', "####. #...# #...# #...# #...# #...# ####."},
    {'E', "##### #.... #.... ####. #.... #.... #####"}, {'F', "##### #.... #.... ####. #.... #.... #...."},
    {'G', ".###. #...# #.... #.### #...# #...# .####"}, {'H', "#...# #...# #...# ##### #...# #...# #...#"},
    {'I', ".###. ..#.. ..#.. ..#.. ..#.. ..#.. .###."}, {'J', "..### ...#. ...#. ...#. ...#. #..#. .##.."},
    {'K', "#...# #..#. #.#.. ##... #.#.. #..#. #...#"}, {'L', "#.... #.... #.... #.... #.... #.... #####"},
    {'M', "#...# ##.## #.#.# #.#.# #...# #...# #...#"}, {'N', "#...# #...# ##..# #.#.# #..## #...# #...#"},
    {'O', ".###. #...# #...# #...# #...# #...# .###."}, {'P', "####. #...# #...# ####. #.... #.... #...."},
    {'Q', ".###. #...# #...# #...# #.#.# #..#. .##.#"}, {'R', "####. #...# #...# ####. #.#.. #..#. #...#"},
    {'S', ".#### #.... #.... .###. ....# ....# ####."}, {'T', "##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#.."},
    {'U', "#...# #...# #...# #...# #...# #...# .###."}, {'V', "#...# #...# #...# #...# #...# .#.#. ..#.."},
    {'W', "#...# #...# #...# #.#.# #.#.# #.#.# .#.#."}, {'X', "#...# #...# .#.#. ..#.. .#.#. #...# #...#"},
    {'Y', "#...# #...# .#.#. ..#.. ..#.. ..#.. ..#.."}, {'Z', "##### ....# ...#. ..#.. .#... #.... #####"},
    {'0', ".###. #...# #..## #.#.# ##..# #...# .###."}, {'1', "..#.. .##.. ..#.. ..#.. ..#.. ..#.. .###."},
    {'2', ".###. #...# ....# ...#. ..#.. .#... #####"}, {'3', "##### ...#. ..#.. ...#. ....# #...# .###."},
    {'4', "...#. ..##. .#.#. #..#. ##### ...#. ...#."}, {'5', "##### #.... ####. ....# ....# #...# .###."},
    {'6', "..##. .#... #.... ####. #...# #...# .###."}, {'7', "##### ....# ...#. ..#.. .#... .#... .#..."},
    {'8', ".###. #...# #...# .###. #...# #...# .###."}, {'9', ".###. #...# #...# .#### ....# ...#. .##.."},
    {'.', "..... ..... ..... ..... ..... .##.. .##.."}, {':', "..... .##.. .##.. ..... .##.. .##.. ....."},
    {'/', "..... ....# ...#. ..#.. .#... #.... ....."}, {'-', "..... ..... ..... ##### ..... ..... ....."},
    {'(', "...#. ..#.. .#... .#... .#... ..#.. ...#."}, {')', ".#... ..#.. ...#. ...#. ...#. ..#.. .#..."},
    {'<', "...#. ..#.. .#... #.... .#... ..#.. ...#."}, {'>', ".#... ..#.. ...#. ....# ...#. ..#.. .#..."},
    {'%', "##... ##..# ...#. ..#.. .#... #..## ...##"}, {'+', "..... ..#.. ..#.. ##### ..#.. ..#.. ....."},
    {',', "..... ..... ..... ..... .##.. ..#.. .#..."},
};

const char *GlyphRows(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (const Glyph &g : kFont) if (g.c == c) return g.rows;
    return nullptr;                                 // space and anything unknown: blank
}

// ---- image -----------------------------------------------------------------------------------

constexpr int kW = 264, kH = 122;                   // logical pixels, scaled up by the renderer
constexpr int kLabelX = 12, kValueX = 112, kRow0 = 24, kRowH = 12;

struct Color { uint8_t r, g, b; };
const Color kBg = {18, 22, 18}, kFrame = {70, 96, 64}, kText = {226, 232, 222}, kMuted = {140, 152, 136},
            kAccent = {124, 195, 111}, kWarn = {230, 162, 74}, kSelect = {40, 62, 37};

struct Image {
    std::vector<uint8_t> px = std::vector<uint8_t>((size_t)kW * kH * 4);
    void Fill(int x, int y, int w, int h, Color c)
    {
        for (int j = SDL_max(y, 0); j < SDL_min(y + h, kH); j++)
            for (int i = SDL_max(x, 0); i < SDL_min(x + w, kW); i++) {
                uint8_t *p = &px[((size_t)j * kW + i) * 4];
                p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 255;
            }
    }
    void Text(int x, int y, Color c, const std::string &s)
    {
        for (char ch : s) {
            if (const char *rows = GlyphRows(ch))
                for (int row = 0; row < 7; row++)
                    for (int b = 0; b < 5; b++)
                        if (rows[row * 6 + b] == '#') Fill(x + b, y + row, 1, 1, c);
            x += 6;
        }
    }
    void Centered(int y, Color c, const std::string &s) { Text((kW - (int)s.size() * 6 + 1) / 2, y, c, s); }
};

// ---- menu ------------------------------------------------------------------------------------

enum Row { RESOLUTION, DRAW, DDMAX, FRAMERATE, FPS, CLOSE, ROWS };

OverlayHost g_host;
OverlayOptions g_opt, g_saved;                     // edited, and as in graphics.ini
bool g_open, g_waitRelease, g_prevButton;
int g_sel = DRAW;
uint64_t g_lastDraw;

const int kFrameRates[] = {0, 30, 20, 15};          // maxfps; 0 = 60

bool RowShown(int r) { return r != FPS || CGraphic::HasFpsCounter(); }

std::string Fmt(const char *f, double v)
{
    char b[32];
    snprintf(b, sizeof(b), f, v);
    return b;
}

std::string Value(int r)
{
    switch (r) {
    case RESOLUTION: {
        std::string label;
        for (const Resolution &x : Resolutions()) if (x.w == g_opt.scrWidth && x.h == g_opt.scrHeight) label = x.label;
        std::string s = std::to_string(g_opt.scrWidth) + "X" + std::to_string(g_opt.scrHeight);
        return label.empty() ? s : s + " " + label;
    }
    case DRAW:
        if (g_opt.drawDistance < 0) return "ORIGINAL";
        if (g_opt.drawDistance == 0) return "AUTO" + Fmt(" (%.2f)", g_host.drawDistanceNow());
        return Fmt("%.2f FIXED", g_opt.drawDistance);
    case DDMAX: return Fmt("%.2f", g_opt.ddMax) + (g_opt.drawDistance == 0 ? "" : " (WITH AUTO)");
    case FRAMERATE: return std::to_string(g_opt.maxfps > 0 ? g_opt.maxfps : 60);
    case FPS: return g_opt.fpsCounter ? "ON" : "OFF";
    default: return "";
    }
}

void Draw()
{
    static const char *const kLabels[ROWS] = {"RESOLUTION", "DRAW DISTANCE", "MAX DISTANCE", "FRAME RATE", "FPS COUNTER", "CLOSE"};
    Image im;
    im.Fill(0, 0, kW, kH, kFrame);
    im.Fill(1, 1, kW - 2, kH - 2, kBg);
    im.Centered(7, kAccent, "OPTIONS");
    int y = kRow0;
    for (int r = 0; r < ROWS; r++) {
        if (!RowShown(r)) continue;
        bool sel = r == g_sel;
        if (sel) im.Fill(4, y - 2, kW - 8, kRowH - 1, kSelect);
        im.Text(kLabelX, y, sel ? kText : kMuted, kLabels[r]);
        std::string v = Value(r);
        if (!v.empty()) im.Text(kValueX, y, kText, sel ? "< " + v + " >" : "  " + v);
        y += kRowH;
    }
    y += 4;
    if (g_opt.scrWidth != g_host.current.scrWidth || g_opt.scrHeight != g_host.current.scrHeight)
        im.Centered(y, kWarn, "RESOLUTION CHANGES AFTER A RESTART");
    im.Centered(kH - 12, kMuted, "UP/DOWN SELECT  LEFT/RIGHT CHANGE  B CLOSE");
    CGraphic::SetOverlay(im.px.data(), kW, kH);
    g_lastDraw = SDL_GetTicksNS();
}

void Change(int r, int dir)
{
    switch (r) {
    case RESOLUTION: {
        std::vector<Resolution> res = Resolutions();
        int i = -1, n = (int)res.size();
        for (int k = 0; k < n; k++) if (res[k].w == g_opt.scrWidth && res[k].h == g_opt.scrHeight) i = k;
        i = i < 0 ? 0 : (i + dir + n) % n;
        g_opt.scrWidth = res[i].w;
        g_opt.scrHeight = res[i].h;
        return;                                     // after a restart
    }
    case DRAW: {                                    // auto, then fixed 0.30 ... 1.50
        float d = g_opt.drawDistance;
        if (d <= 0) d = dir > 0 ? 0.30f : 0;
        else d = roundf((d + dir * 0.05f) * 100) / 100;
        g_opt.drawDistance = d < 0.30f ? 0 : SDL_min(d, 1.50f);
        break;
    }
    case DDMAX: g_opt.ddMax = SDL_clamp(roundf((g_opt.ddMax + dir * 0.05f) * 100) / 100, 0.50f, 1.50f); break;
    case FRAMERATE: {
        int i = 0, n = (int)SDL_arraysize(kFrameRates);
        for (int k = 0; k < n; k++) if (kFrameRates[k] == g_opt.maxfps) i = k;
        g_opt.maxfps = kFrameRates[SDL_clamp(i - dir, 0, n - 1)];     // right = more frames
        break;
    }
    case FPS: g_opt.fpsCounter = !g_opt.fpsCounter; break;
    default: return;
    }
    g_host.apply(g_opt);
}

void Save()
{
    std::vector<std::pair<std::string, std::string>> v;
    if (g_opt.scrWidth != g_saved.scrWidth || g_opt.scrHeight != g_saved.scrHeight) {
        v.push_back({"scrwidth", std::to_string(g_opt.scrWidth)});
        v.push_back({"scrheight", std::to_string(g_opt.scrHeight)});
    }
    if (g_opt.drawDistance != g_saved.drawDistance)
        v.push_back({"drawdistance", g_opt.drawDistance > 0 ? Fmt("%.2f", g_opt.drawDistance) : "auto"});
    if (g_opt.ddMax != g_saved.ddMax) v.push_back({"ddmax", Fmt("%.2f", g_opt.ddMax)});
    if (g_opt.maxfps != g_saved.maxfps) v.push_back({"maxfps", std::to_string(g_opt.maxfps)});
    if (g_opt.fpsCounter != g_saved.fpsCounter) v.push_back({"fps", g_opt.fpsCounter ? "1" : "0"});
    if (v.empty()) return;
    UpdateIniValues(g_host.iniPath, v);
    g_saved = g_opt;
    fprintf(stderr, "options: %zu settings saved to %s\n", v.size(), g_host.iniPath.c_str());
}

void Close()
{
    g_open = false;
    g_waitRelease = true;                           // the closing button must not reach the game
    CGraphic::SetOverlay(nullptr, 0, 0);
    Save();
}

} // namespace

void OverlayInit(const OverlayHost &host)
{
    g_host = host;
    g_opt = g_saved = host.current;
}

bool OverlayUpdate(uint8_t *keys, bool menuButton)
{
    bool pressed = menuButton && !g_prevButton;
    g_prevButton = menuButton;
    if (!g_host.apply) return false;
    if (!g_open) {
        if (pressed) {
            g_open = true;
            if (!RowShown(g_sel)) g_sel = DRAW;
            Draw();
        } else if (g_waitRelease) {
            bool any = menuButton;
            for (int i = 0; i < 256; i++) any |= keys[i] != 0;
            g_waitRelease = any;
        }
        if (!g_open && !g_waitRelease) return false;
        memset(keys, 0, 256);
        return true;
    }
    if (pressed) { Close(); memset(keys, 0, 256); return true; }

    // key presses, repeating while held (after 0.4 s, every 0.12 s)
    static uint64_t heldSince[256], lastRepeat[256];
    uint64_t now = SDL_GetTicksNS();
    auto hit = [&](uint8_t vk) {
        if (!keys[vk]) { heldSince[vk] = 0; return false; }
        if (!heldSince[vk]) { heldSince[vk] = lastRepeat[vk] = now; return true; }
        if (now - heldSince[vk] > 400000000ull && now - lastRepeat[vk] > 120000000ull) { lastRepeat[vk] = now; return true; }
        return false;
    };
    bool up = hit(0x26), down = hit(0x28), left = hit(0x25), right = hit(0x27), enter = hit(0x0d), esc = hit(0x1b);
    memset(keys, 0, 256);
    if (esc || (enter && g_sel == CLOSE)) { Close(); return true; }
    bool redraw = false;
    if (up || down) {
        do g_sel = (g_sel + (down ? 1 : ROWS - 1)) % ROWS; while (!RowShown(g_sel));
        redraw = true;
    }
    if (left || right || enter) {
        Change(g_sel, left ? -1 : 1);
        redraw = true;
    }
    if (redraw || now - g_lastDraw > 250000000ull) Draw();      // the auto draw distance changes
    return true;
}
