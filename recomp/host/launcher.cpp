// launcher.cpp - settings window shown before the game starts (like DDGModernizer's dashboard).
//
// Drawn with SDL's 2D renderer and its built-in 8x8 debug font in a window of its own, which is
// destroyed before the game claims its window for SDL GPU. Keyboard: Up/Down select, Left/Right
// change, Return activates, Esc quits. Mouse: click a row, or the < > arrows of a value.

#include "launcher.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// ---- settings files ----------------------------------------------------------------------

std::vector<std::string> ReadLines(const std::string &path)
{
    std::vector<std::string> lines;
    if (FILE *f = fopen(path.c_str(), "r")) {
        char buf[1024];
        while (fgets(buf, sizeof(buf), f)) {
            std::string l = buf;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            lines.push_back(l);
        }
        fclose(f);
    }
    return lines;
}

bool KeyOf(const std::string &line, std::string &key, std::string &val)
{
    char k[64], v[512];
    if (sscanf(line.c_str(), " %63[^= ] = %511[^\n]", k, v) != 2) return false;
    key = k;
    val = v;
    while (!val.empty() && val.back() == ' ') val.pop_back();
    return true;
}

// ---- display ---------------------------------------------------------------------------------

struct Resolution { int w, h; const char *label; };

std::vector<Resolution> Resolutions()
{
    std::vector<Resolution> r = {
        {640, 480, "4:3 original"}, {960, 720, "4:3"}, {1280, 960, "4:3"},
        {854, 480, "16:9"}, {1280, 720, "16:9"}, {1920, 1080, "16:9"},
        {1280, 800, "16:10"}, {1680, 1050, "16:10"}, {1920, 1200, "16:10"},
    };
    // the shape of this display, at 1080 lines
    if (const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
        if (m->w > 0 && m->h > 0) {
            int h = 1080, w = (int)lround(1080.0 * m->w / m->h);
            w += w & 1;
            bool known = false;
            for (auto &x : r) known |= x.w * m->h == x.h * m->w;
            if (!known) r.push_back({w, h, "this display"});
        }
    }
    return r;
}

const char *RatioLabel(int w, int h)
{
    for (auto &x : Resolutions()) if (x.w == w && x.h == h) return x.label;
    return "custom";
}

float AutoAspect(int w, int h) { return (4.0f * h / 3.0f) / (float)w; }

// ---- drawing -------------------------------------------------------------------------------

constexpr float kW = 480, kH = 300;            // logical size; the font is 8x8
constexpr float kRow0 = 64, kRowH = 22, kLabelX = 24, kValueX = 176;

struct Color { Uint8 r, g, b; };
const Color kBg = {18, 22, 18}, kPanel = {28, 34, 27}, kText = {226, 232, 222}, kMuted = {140, 152, 136},
            kAccent = {124, 195, 111}, kWarn = {230, 162, 74}, kSelect = {40, 62, 37};

void SetColor(SDL_Renderer *r, Color c) { SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255); }

void Text(SDL_Renderer *r, float x, float y, Color c, const std::string &s)
{
    SetColor(r, c);
    SDL_RenderDebugText(r, x, y, s.c_str());
}

std::string Fit(const std::string &s, size_t n)
{
    if (s.size() <= n) return s;
    return "..." + s.substr(s.size() - (n - 3));
}

enum Item { FOLDER, RESOLUTION, ASPECT, DRAW, FULLSCREEN, CROP, SHOW, START, QUIT, COUNT };

struct FolderPick { bool pending = false, done = false; std::string path; };

void SDLCALL OnFolder(void *user, const char *const *files, int)
{
    auto *p = static_cast<FolderPick *>(user);
    if (files && files[0]) p->path = files[0];
    p->done = true;
}

} // namespace

// ---- settings I/O ----------------------------------------------------------------------------

LauncherSettings LoadLauncherSettings(const std::string &prefDir, const std::string &gameDir)
{
    LauncherSettings s;
    s.gameDir = gameDir;
    std::string key, val;
    for (auto &l : ReadLines(prefDir + "/graphics.ini")) {
        if (!KeyOf(l, key, val)) continue;
        float v = (float)atof(val.c_str());
        if (!SDL_strcasecmp(key.c_str(), "scrwidth") && v > 0) s.scrWidth = (int)v;
        else if (!SDL_strcasecmp(key.c_str(), "scrheight") && v > 0) s.scrHeight = (int)v;
        else if (!SDL_strcasecmp(key.c_str(), "aspect")) s.aspect = SDL_strcasecmp(val.c_str(), "auto") ? v : 0;
        else if (!SDL_strcasecmp(key.c_str(), "drawdistance") && v > 0) s.drawDistance = v;
        else if (!SDL_strcasecmp(key.c_str(), "fullscreen")) s.fullscreen = v != 0;
        else if (!SDL_strcasecmp(key.c_str(), "crop")) s.crop = v != 0;
        else if (!SDL_strcasecmp(key.c_str(), "launcher")) s.showLauncher = v != 0;
    }
    return s;
}

void SaveLauncherSettings(const std::string &prefDir, const LauncherSettings &s)
{
    char aspect[32];
    if (s.aspect > 0) snprintf(aspect, sizeof(aspect), "%.3f", s.aspect);
    else snprintf(aspect, sizeof(aspect), "auto");
    char draw[32];
    snprintf(draw, sizeof(draw), "%.2f", s.drawDistance);
    std::vector<std::pair<std::string, std::string>> values = {
        {"scrwidth", std::to_string(s.scrWidth)}, {"scrheight", std::to_string(s.scrHeight)},
        {"aspect", aspect}, {"drawdistance", draw}, {"fullscreen", s.fullscreen ? "1" : "0"},
        {"crop", s.crop ? "1" : "0"}, {"launcher", s.showLauncher ? "1" : "0"},
    };
    std::string path = prefDir + "/graphics.ini";
    std::vector<std::string> lines = ReadLines(path);
    std::vector<bool> written(values.size(), false);
    std::string key, val;
    for (auto &l : lines) {
        if (!KeyOf(l, key, val)) continue;
        for (size_t i = 0; i < values.size(); i++)
            if (!SDL_strcasecmp(key.c_str(), values[i].first.c_str())) {
                l = values[i].first + " = " + values[i].second;
                written[i] = true;
            }
    }
    for (size_t i = 0; i < values.size(); i++)
        if (!written[i]) lines.push_back(values[i].first + " = " + values[i].second);
    if (FILE *f = fopen(path.c_str(), "w")) {
        for (auto &l : lines) fprintf(f, "%s\n", l.c_str());
        fclose(f);
    }
}

// ---- the window --------------------------------------------------------------------------------

bool RunLauncher(LauncherSettings &s, std::string (*checkDir)(const std::string &dir))
{
    SDL_Window *win = SDL_CreateWindow("Densha de GO! FINAL - Settings", (int)kW * 2, (int)kH * 2,
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    if (!win) return true;                       // no launcher: start with the current settings
    SDL_Renderer *r = SDL_CreateRenderer(win, nullptr);
    if (!r) { SDL_DestroyWindow(win); return true; }
    SDL_SetRenderLogicalPresentation(r, (int)kW, (int)kH, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(r, 1);

    std::vector<Resolution> res = Resolutions();
    int sel = START;
    std::string dirProblem = checkDir(s.gameDir);
    FolderPick pick;
    bool result = false, running = true;

    auto resIndex = [&]() {
        for (size_t i = 0; i < res.size(); i++) if (res[i].w == s.scrWidth && res[i].h == s.scrHeight) return (int)i;
        return -1;
    };
    auto change = [&](int item, int dir) {
        switch (item) {
        case RESOLUTION: {
            int i = resIndex();
            i = i < 0 ? 0 : (i + dir + (int)res.size()) % (int)res.size();
            s.scrWidth = res[i].w;
            s.scrHeight = res[i].h;
            break;
        }
        case ASPECT: {
            float a = s.aspect > 0 ? s.aspect : AutoAspect(s.scrWidth, s.scrHeight);
            if (s.aspect <= 0 && dir < 0) { s.aspect = roundf(a * 100) / 100; break; }
            a = roundf((a + dir * 0.01f) * 100) / 100;
            s.aspect = SDL_clamp(a, 0.40f, 1.50f);
            break;
        }
        case DRAW:
            s.drawDistance = SDL_clamp(roundf((s.drawDistance + dir * 0.05f) * 100) / 100, 0.20f, 2.00f);
            break;
        case FULLSCREEN: s.fullscreen = !s.fullscreen; break;
        case CROP: s.crop = !s.crop; break;
        case SHOW: s.showLauncher = !s.showLauncher; break;
        default: break;
        }
    };
    auto activate = [&](int item) {
        switch (item) {
        case FOLDER:
            if (!pick.pending) {
                pick = FolderPick();
                pick.pending = true;
                SDL_ShowOpenFolderDialog(OnFolder, &pick, win, s.gameDir.empty() ? nullptr : s.gameDir.c_str(), false);
            }
            break;
        case ASPECT: s.aspect = 0; break;                       // back to auto
        case START:
            if (dirProblem.empty()) { result = true; running = false; }
            else sel = FOLDER;
            break;
        case QUIT: running = false; break;
        default: change(item, 1); break;
        }
    };
    auto rowY = [](int item) { return item < START ? kRow0 + item * kRowH : kH - 44; };

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
            else if (e.type == SDL_EVENT_KEY_DOWN && !pick.pending) {
                switch (e.key.key) {
                case SDLK_UP: sel = sel == START || sel == QUIT ? SHOW : (sel == FOLDER ? FOLDER : sel - 1); break;
                case SDLK_DOWN: sel = sel >= SHOW ? (sel == SHOW ? START : sel) : sel + 1; break;
                case SDLK_LEFT: if (sel == QUIT) sel = START; else change(sel, -1); break;
                case SDLK_RIGHT: if (sel == START) sel = QUIT; else change(sel, 1); break;
                case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: activate(sel); break;
                case SDLK_ESCAPE: running = false; break;
                }
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !pick.pending) {
                SDL_ConvertEventToRenderCoordinates(r, &e);
                float x = e.button.x, y = e.button.y;
                for (int i = 0; i < COUNT; i++) {
                    float ry = rowY(i);
                    bool hit = i < START ? (y >= ry - 6 && y < ry + kRowH - 6)
                                         : (y >= ry - 8 && y < ry + 16 &&
                                            (i == START ? x < kW / 2 : x >= kW / 2));
                    if (!hit) continue;
                    sel = i;
                    bool arrows = i == RESOLUTION || i == ASPECT || i == DRAW;
                    if (arrows && x >= kValueX - 4 && x < kValueX + 12) change(i, -1);
                    else if (arrows && x >= kW - 40) change(i, 1);
                    else activate(i);
                }
            }
        }
        if (pick.pending && pick.done) {
            pick.pending = false;
            if (!pick.path.empty()) {
                while (pick.path.size() > 1 && pick.path.back() == '/') pick.path.pop_back();
                s.gameDir = pick.path;
                dirProblem = checkDir(s.gameDir);
            }
        }

        // ---- draw
        SetColor(r, kBg);
        SDL_RenderClear(r);
        SDL_FRect band = {0, 0, kW, 40};
        SetColor(r, kPanel);
        SDL_RenderFillRect(r, &band);
        SDL_FRect stripe = {0, 40, kW, 2};
        SetColor(r, kAccent);
        SDL_RenderFillRect(r, &stripe);
        Text(r, kLabelX, 12, kText, "DENSHA DE GO! FINAL");
        Text(r, kLabelX, 24, kMuted, "Settings - recompiled for macOS");

        char buf[160];
        for (int i = 0; i < START; i++) {
            float y = rowY(i);
            if (i == sel) {
                SDL_FRect hl = {8, y - 6, kW - 16, kRowH - 2};
                SetColor(r, kSelect);
                SDL_RenderFillRect(r, &hl);
            }
            const char *label = "";
            std::string value, note;
            bool arrows = false;
            switch (i) {
            case FOLDER:
                label = "Game folder";
                value = s.gameDir.empty() ? "(not set) - press Return" : Fit(s.gameDir, 36);
                break;
            case RESOLUTION:
                label = "Resolution";
                snprintf(buf, sizeof(buf), "%d x %d  %s", s.scrWidth, s.scrHeight, RatioLabel(s.scrWidth, s.scrHeight));
                value = buf;
                arrows = true;
                break;
            case ASPECT:
                label = "3D aspect";
                if (s.aspect > 0) snprintf(buf, sizeof(buf), "%.2f", s.aspect);
                else snprintf(buf, sizeof(buf), "Auto (%.2f)", AutoAspect(s.scrWidth, s.scrHeight));
                value = buf;
                arrows = true;
                break;
            case DRAW:
                label = "Draw distance";
                snprintf(buf, sizeof(buf), "%.2f   original 0.90", s.drawDistance);
                value = buf;
                arrows = true;
                break;
            case FULLSCREEN: label = "Fullscreen"; value = s.fullscreen ? "On" : "Off  (F11 in game)"; break;
            case CROP: label = "Hide screen edges"; value = s.crop ? "On" : "Off"; break;
            case SHOW: label = "Show at startup"; value = s.showLauncher ? "On" : "Off  (hold Shift)"; break;
            }
            Text(r, kLabelX, y, i == sel ? kText : kMuted, label);
            if (arrows) {
                Text(r, kValueX, y, kAccent, "<");
                Text(r, kValueX + 16, y, kText, value);
                Text(r, kW - 32, y, kAccent, ">");
            } else {
                Text(r, kValueX + 16, y, kText, value);
            }
        }
        // folder status
        float sy = rowY(SHOW) + kRowH + 4;
        if (!dirProblem.empty()) {
            std::string msg = dirProblem;
            size_t nl;
            int line = 0;
            while (!msg.empty() && line < 3) {
                nl = msg.find('\n');
                std::string part = msg.substr(0, nl);
                Text(r, kLabelX, sy + line * 11, kWarn, Fit(part, 54));
                msg = nl == std::string::npos ? "" : msg.substr(nl + 1);
                line++;
            }
        } else {
            Text(r, kLabelX, sy, kMuted, "Game folder OK");
        }
        // buttons
        for (int i = START; i <= QUIT; i++) {
            float bx = i == START ? kW / 2 - 132 : kW / 2 + 12, y = rowY(i);
            SDL_FRect b = {bx, y - 8, 120, 24};
            bool usable = i != START || dirProblem.empty();
            SetColor(r, i == sel ? (usable ? kAccent : kWarn) : kPanel);
            SDL_RenderFillRect(r, &b);
            Color tc = i == sel ? kBg : (usable ? kText : kMuted);
            const char *t = i == START ? "Start" : "Quit";
            Text(r, bx + 60 - strlen(t) * 4, y, tc, t);
        }
        Text(r, kLabelX, kH - 14, kMuted, "Arrows select/change  Return OK  Esc quit");
        SDL_RenderPresent(r);
    }

    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    return result;
}
