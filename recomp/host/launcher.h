// launcher.h - settings window shown before the game starts (like DDGModernizer's dashboard)
#pragma once
#include <string>
#include <utility>
#include <vector>

struct LauncherSettings {
    std::string gameDir;        // install folder (cddata + perfect.exe)
    std::string exePath;        // perfect.exe; empty = the one in gameDir
    int scrWidth = 640, scrHeight = 480;
    float aspect = 0;           // 3D X aspect; 0 = auto
    float drawDistance = 0;     // RendPow; 0 = auto (the game's controller, by load)
    float ddMax = 1.0f;         // auto: the largest draw distance
    int maxfps = 0;             // frames per second at most; 0 = 60
    bool fpsCounter = false;    // frames-per-second counter (only where hasFpsCounter)
    bool hasFpsCounter = false; // set by the caller: the renderer has one
    bool fullscreen = false;
    bool crop = true;           // show only the game's canvas
    bool showLauncher = true;   // show this window at startup
};

// Preparing the game code while the window is open (RECOMP_JIT builds: translating the exe and
// compiling it). start is called with the folder and exePath (empty: the folder's) whenever they
// are usable; poll returns 0 while working (text: what), 1 when the game code for them is ready,
// -1 if it failed (text: why). Start stays disabled until it is ready.
struct LauncherPrepare {
    void (*start)(const std::string &dir, const std::string &exe);
    int (*poll)(const std::string &dir, const std::string &exe, std::string &text);
};

// Settings from graphics.ini (and the given game folder).
LauncherSettings LoadLauncherSettings(const std::string &prefDir, const std::string &gameDir);
// Writes the settings into graphics.ini, keeping other lines.
void SaveLauncherSettings(const std::string &prefDir, const LauncherSettings &s);

// Sets key = value lines in an ini file (replacing the key's lines, appending missing keys),
// keeping all other lines.
void UpdateIniValues(const std::string &path, const std::vector<std::pair<std::string, std::string>> &values);

// The resolutions offered (scrwidth x scrheight), with a label (aspect ratio).
struct Resolution { int w, h; const char *label; };
std::vector<Resolution> Resolutions();

// Runs the settings window. checkDir returns "" for a usable game folder and exe (exePath; empty:
// the folder's), otherwise the reason. prep: see LauncherPrepare (nullptr: none). Returns true for Start (settings updated),
// false for Quit.
bool RunLauncher(LauncherSettings &s, std::string (*checkDir)(const std::string &dir, const std::string &exe),
                 const LauncherPrepare *prep = nullptr);
