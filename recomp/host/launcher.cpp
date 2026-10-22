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

} // namespace

// ---- display ---------------------------------------------------------------------------------

std::vector<Resolution> Resolutions()
{
    std::vector<Resolution> r = {
        {320, 240, "4:3 fast"}, {640, 480, "4:3 original"}, {960, 720, "4:3"}, {1280, 960, "4:3"},
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

namespace {

const char *RatioLabel(int w, int h)
{
    for (auto &x : Resolutions()) if (x.w == w && x.h == h) return x.label;
    return "custom";
}

float AutoAspect(int w, int h) { return (4.0f * h / 3.0f) / (float)w; }

// ---- drawing -------------------------------------------------------------------------------

constexpr float kW = 480, kH = 380;            // logical size; the font is 8x8
constexpr float kRow0 = 60, kRowH = 20, kLabelX = 24, kValueX = 176;

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

// s in lines of at most n characters, broken at spaces or slashes where possible
std::vector<std::string> Wrap(const std::string &s, size_t n)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at < s.size()) {
        size_t len = SDL_min(n, s.size() - at);
        if (at + len < s.size()) {
            size_t cut = s.find_last_of(" /", at + len - 1);
            if (cut != std::string::npos && cut > at + n / 2) len = cut - at + 1;
        }
        out.push_back(s.substr(at, len));
        at += len;
    }
    return out;
}

enum Item { FOLDER, EXE, RESOLUTION, ASPECT, DRAW, MAXDIST, FRAMERATE, FPSCOUNTER, FULLSCREEN, CROP, SHOW,
            START, QUIT, COUNT };

const int kFrameRates[] = {0, 30, 20, 15};      // maxfps (0 = 60), as in the options overlay

struct Pick { bool pending = false, done = false; int item = FOLDER; std::string path; };

void SDLCALL OnPick(void *user, const char *const *files, int)
{
    auto *p = static_cast<Pick *>(user);
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
        else if (!SDL_strcasecmp(key.c_str(), "drawdistance")) s.drawDistance = SDL_strcasecmp(val.c_str(), "auto") && v > 0 ? v : 0;
        else if (!SDL_strcasecmp(key.c_str(), "fullscreen")) s.fullscreen = v != 0;
        else if (!SDL_strcasecmp(key.c_str(), "crop")) s.crop = v != 0;
        else if (!SDL_strcasecmp(key.c_str(), "launcher")) s.showLauncher = v != 0;
        else if (!SDL_strcasecmp(key.c_str(), "exe")) s.exePath = val;
        else if (!SDL_strcasecmp(key.c_str(), "ddmax") && v > 0) s.ddMax = v;
        else if (!SDL_strcasecmp(key.c_str(), "maxfps")) s.maxfps = SDL_max((int)v, 0);
        else if (!SDL_strcasecmp(key.c_str(), "fps")) s.fpsCounter = v != 0;
    }
    return s;
}

void SaveLauncherSettings(const std::string &prefDir, const LauncherSettings &s)
{
    char aspect[32];
    if (s.aspect > 0) snprintf(aspect, sizeof(aspect), "%.3f", s.aspect);
    else snprintf(aspect, sizeof(aspect), "auto");
    char draw[32];
    if (s.drawDistance > 0) snprintf(draw, sizeof(draw), "%.2f", s.drawDistance);
    else snprintf(draw, sizeof(draw), "auto");
    char ddmax[32];
    snprintf(ddmax, sizeof(ddmax), "%.2f", s.ddMax);
    std::vector<std::pair<std::string, std::string>> values = {
        {"scrwidth", std::to_string(s.scrWidth)}, {"scrheight", std::to_string(s.scrHeight)},
        {"aspect", aspect}, {"fullscreen", s.fullscreen ? "1" : "0"},
        {"crop", s.crop ? "1" : "0"}, {"launcher", s.showLauncher ? "1" : "0"},
        {"exe", s.exePath},             // empty: no value, the folder's perfect.exe
        {"ddmax", ddmax}, {"maxfps", std::to_string(s.maxfps)},
    };
    if (s.hasFpsCounter) values.push_back({"fps", s.fpsCounter ? "1" : "0"});
    std::string path = prefDir + "/graphics.ini";
    // "original" (the game's controller with its own timing) is kept while auto
    bool original = false;
    std::string key, val;
    for (auto &l : ReadLines(path))
        if (KeyOf(l, key, val) && !SDL_strcasecmp(key.c_str(), "drawdistance")) original = !SDL_strcasecmp(val.c_str(), "original");
    if (!(original && s.drawDistance <= 0)) values.push_back({"drawdistance", draw});
    UpdateIniValues(path, values);
}

void UpdateIniValues(const std::string &path, const std::vector<std::pair<std::string, std::string>> &values)
{
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

bool RunLauncher(LauncherSettings &s, std::string (*checkDir)(const std::string &dir, const std::string &exe),
                 const LauncherPrepare *prep)
{
    SDL_Window *win = SDL_CreateWindow("Densha de GO! FINAL - Settings", (int)kW * 2, (int)kH * 2,
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    if (!win) return true;                       // no launcher: start with the current settings
    SDL_Renderer *r = SDL_CreateRenderer(win, nullptr);
    if (!r) { SDL_DestroyWindow(win); return true; }
    SDL_SetRenderLogicalPresentation(r, (int)kW, (int)kH, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(r, 1);

    std::vector<Resolution> res = Resolutions();
    // the rows shown (the fps counter only where the renderer has one)
    std::vector<int> rows;
    for (int i = FOLDER; i < START; i++)
        if (i != FPSCOUNTER || s.hasFpsCounter) rows.push_back(i);
    auto rowOf = [&](int item) {
        for (size_t k = 0; k < rows.size(); k++) if (rows[k] == item) return (int)k;
        return -1;
    };
    int sel = START;
    std::string dirProblem = checkDir(s.gameDir, s.exePath);
    std::string prepared, prepText;              // what prep was started for; its status line
    int prepState = 0;
    Pick pick;
    bool result = false, running = true;

    auto resIndex = [&]() {
        for (size_t i = 0; i < res.size(); i++) if (res[i].w == s.scrWidth && res[i].h == s.scrHeight) return (int)i;
        return -1;
    };
    auto fpsIndex = [&]() {
        for (int i = 0; i < 4; i++) if (kFrameRates[i] == s.maxfps) return i;
        return 0;
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
            if (s.drawDistance <= 0) { s.drawDistance = 0.90f; break; }        // auto -> fixed 0.90
            if (s.drawDistance <= 0.20f && dir < 0) { s.drawDistance = 0; break; }  // below 0.20 -> auto
            s.drawDistance = SDL_clamp(roundf((s.drawDistance + dir * 0.05f) * 100) / 100, 0.20f, 2.00f);
            break;
        case MAXDIST: s.ddMax = SDL_clamp(roundf((s.ddMax + dir * 0.05f) * 100) / 100, 0.50f, 1.50f); break;
        case FRAMERATE: s.maxfps = kFrameRates[(fpsIndex() + dir + 4) % 4]; break;
        case FPSCOUNTER: s.fpsCounter = !s.fpsCounter; break;
        case FULLSCREEN: s.fullscreen = !s.fullscreen; break;
        case CROP: s.crop = !s.crop; break;
        case SHOW: s.showLauncher = !s.showLauncher; break;
        default: break;
        }
    };
    auto startOk = [&]() { return dirProblem.empty() && (!prep || prepState == 1); };
    auto activate = [&](int item) {
        switch (item) {
        case FOLDER:
            if (!pick.pending) {
                pick = Pick();
                pick.pending = true;
                pick.item = FOLDER;
                SDL_ShowOpenFolderDialog(OnPick, &pick, win, s.gameDir.empty() ? nullptr : s.gameDir.c_str(), false);
            }
            break;
        case EXE:
            if (!pick.pending) {
                static const SDL_DialogFileFilter filter[] = {{"Windows programs", "exe"}};
                pick = Pick();
                pick.pending = true;
                pick.item = EXE;
                std::string at = !s.exePath.empty() ? s.exePath : s.gameDir;
                SDL_ShowOpenFileDialog(OnPick, &pick, win, filter, 1, at.empty() ? nullptr : at.c_str(), false);
            }
            break;
        case ASPECT: s.aspect = 0; break;                       // back to auto
        case START:
            if (startOk()) { result = true; running = false; }
            else if (!dirProblem.empty()) sel = FOLDER;
            break;
        case QUIT: running = false; break;
        default: change(item, 1); break;
        }
    };
    auto rowY = [&](int item) { return item < START ? kRow0 + rowOf(item) * kRowH : kH - 40; };
    auto step = [&](int d) {                         // Up/Down over the rows and the buttons
        if (sel >= START) { if (d < 0) sel = rows.back(); return; }
        int k = rowOf(sel) + d;
        if (k < 0) return;
        sel = k >= (int)rows.size() ? START : rows[k];
    };

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
            else if (e.type == SDL_EVENT_KEY_DOWN && !pick.pending) {
                switch (e.key.key) {
                case SDLK_UP: step(-1); break;
                case SDLK_DOWN: step(1); break;
                case SDLK_LEFT: if (sel == QUIT) sel = START; else change(sel, -1); break;
                case SDLK_RIGHT: if (sel == START) sel = QUIT; else change(sel, 1); break;
                case SDLK_DELETE: case SDLK_BACKSPACE:
                    if (sel == EXE) { s.exePath.clear(); dirProblem = checkDir(s.gameDir, s.exePath); }
                    break;
                case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: activate(sel); break;
                case SDLK_ESCAPE: running = false; break;
                }
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !pick.pending) {
                SDL_ConvertEventToRenderCoordinates(r, &e);
                float x = e.button.x, y = e.button.y;
                for (int i = 0; i < COUNT; i++) {
                    if (i < START && rowOf(i) < 0) continue;
                    float ry = rowY(i);
                    bool hit = i < START ? (y >= ry - 6 && y < ry + kRowH - 6)
                                         : (y >= ry - 8 && y < ry + 16 &&
                                            (i == START ? x < kW / 2 : x >= kW / 2));
                    if (!hit) continue;
                    sel = i;
                    bool arrows = i == RESOLUTION || i == ASPECT || i == DRAW || i == MAXDIST || i == FRAMERATE;
                    if (arrows && x >= kValueX - 4 && x < kValueX + 12) change(i, -1);
                    else if (arrows && x >= kW - 40) change(i, 1);
                    else activate(i);
                }
            }
        }
        if (pick.pending && pick.done) {
            pick.pending = false;
            if (!pick.path.empty()) {
                if (pick.item == FOLDER) {
                    while (pick.path.size() > 1 && pick.path.back() == '/') pick.path.pop_back();
                    s.gameDir = pick.path;
                } else {
                    s.exePath = pick.path;
                }
                dirProblem = checkDir(s.gameDir, s.exePath);
            }
        }
        // the game code for this folder and exe (prep: started once per choice)
        if (prep && dirProblem.empty()) {
            std::string key = s.gameDir + "\n" + s.exePath;
            if (key != prepared) { prep->start(s.gameDir, s.exePath); prepared = key; }
            prepState = prep->poll(s.gameDir, s.exePath, prepText);
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
        Text(r, kLabelX, 24, kMuted, prep ? "Settings - and preparing the game" : "Settings - recompiled");

        char buf[160];
        for (int i : rows) {
            float y = rowY(i);
            if (i == sel) {
                SDL_FRect hl = {8, y - 6, kW - 16, kRowH - 2};
                SetColor(r, kSelect);
                SDL_RenderFillRect(r, &hl);
            }
            const char *label = "";
            std::string value;
            bool arrows = false, dim = false;
            switch (i) {
            case FOLDER:
                label = "Game folder";
                value = s.gameDir.empty() ? "(not set) - press Return" : Fit(s.gameDir, 33);
                break;
            case EXE:
                label = "perfect.exe";
                value = s.exePath.empty() ? "In the game folder (Return: other)" : Fit(s.exePath, 33);
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
                if (s.drawDistance > 0) snprintf(buf, sizeof(buf), "%.2f   fixed", s.drawDistance);
                else snprintf(buf, sizeof(buf), "Auto (by load)");
                value = buf;
                arrows = true;
                break;
            case MAXDIST:
                label = "Max distance";
                snprintf(buf, sizeof(buf), "%.2f", s.ddMax);
                value = buf;
                arrows = true;
                dim = s.drawDistance > 0;                // only for auto
                break;
            case FRAMERATE:
                label = "Frame rate";
                snprintf(buf, sizeof(buf), "%d frames/s", s.maxfps ? s.maxfps : 60);
                value = buf;
                arrows = true;
                break;
            case FPSCOUNTER: label = "FPS counter"; value = s.fpsCounter ? "On" : "Off"; break;
            case FULLSCREEN: label = "Fullscreen"; value = s.fullscreen ? "On" : "Off  (F11 in game)"; break;
            case CROP: label = "Hide screen edges"; value = s.crop ? "On" : "Off"; break;
            case SHOW: label = "Show at startup"; value = s.showLauncher ? "On" : "Off  (hold Shift)"; break;
            }
            Text(r, kLabelX, y, i == sel ? kText : kMuted, label);
            Color vc = dim ? kMuted : kText;
            if (arrows) {
                Text(r, kValueX, y, kAccent, "<");
                Text(r, kValueX + 16, y, vc, value);
                Text(r, kW - 32, y, kAccent, ">");
            } else {
                Text(r, kValueX + 16, y, vc, value);
            }
        }
        // folder and preparation status
        float sy = rowY(SHOW) + kRowH + 2;
        if (!dirProblem.empty()) {
            std::string msg = dirProblem;
            int line = 0;
            while (!msg.empty() && line < 3) {
                size_t nl = msg.find('\n');
                Text(r, kLabelX, sy + line * 11, kWarn, Fit(msg.substr(0, nl), 54));
                msg = nl == std::string::npos ? "" : msg.substr(nl + 1);
                line++;
            }
        } else if (prep) {
            if (prepState < 0) {                         // why it failed: up to three lines
                std::vector<std::string> lines = Wrap(prepText, 54);
                for (size_t k = 0; k < lines.size() && k < 3; k++) Text(r, kLabelX, sy + k * 11, kWarn, lines[k]);
            } else {
                Text(r, kLabelX, sy, prepState ? kAccent : kText, Fit(prepText, 54));
            }
            if (prepState == 0) {                        // a moving bar while it works
                float t = (float)(SDL_GetTicks() % 1600) / 1600.0f;
                SDL_FRect track = {kLabelX, sy + 14, kW - 2 * kLabelX, 3};
                SetColor(r, kPanel);
                SDL_RenderFillRect(r, &track);
                SDL_FRect bar = {kLabelX + t * (kW - 2 * kLabelX - 60), sy + 14, 60, 3};
                SetColor(r, kAccent);
                SDL_RenderFillRect(r, &bar);
            }
        } else {
            Text(r, kLabelX, sy, kMuted, "Game folder OK");
        }
        // buttons
        for (int i = START; i <= QUIT; i++) {
            float bx = i == START ? kW / 2 - 132 : kW / 2 + 12, y = rowY(i);
            SDL_FRect b = {bx, y - 8, 120, 24};
            bool usable = i != START || startOk();
            SetColor(r, i == sel ? (usable ? kAccent : kWarn) : kPanel);
            SDL_RenderFillRect(r, &b);
            Color tc = i == sel ? kBg : (usable ? kText : kMuted);
            const char *t = i == START ? (dirProblem.empty() && prep && prepState == 0 ? "Preparing..." : "Start") : "Quit";
            Text(r, bx + 60 - strlen(t) * 4, y, tc, t);
        }
        Text(r, kLabelX, kH - 14, kMuted, "Arrows select/change  Return OK  Esc quit");
        // test hooks: RECOMP_LAUNCHER_SHOT=<seconds>[,...] saves the window (launcher-<n>.bmp),
        // RECOMP_LAUNCHER_AUTOSTART=1 presses Start as soon as it can
        static const char *shots = getenv("RECOMP_LAUNCHER_SHOT");
        static Uint64 opened = SDL_GetTicks();
        static int shotN;
        if (shots && *shots && SDL_GetTicks() - opened >= (Uint64)(atof(shots) * 1000)) {
            SDL_Surface *img = SDL_RenderReadPixels(r, nullptr);
            if (!img) fprintf(stderr, "launcher shot: %s\n", SDL_GetError());
            if (img) {
                char name[32];
                snprintf(name, sizeof name, "launcher-%d.bmp", ++shotN);
                SDL_SaveBMP(img, name);
                SDL_DestroySurface(img);
            }
            shots = strchr(shots, ',');
            if (shots) shots++;
        }
        if (getenv("RECOMP_LAUNCHER_AUTOSTART") && startOk()) { result = true; running = false; }
        SDL_RenderPresent(r);
    }

    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    return result;
}
