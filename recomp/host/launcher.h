// launcher.h - settings window shown before the game starts (like DDGModernizer's dashboard)
#pragma once
#include <string>

struct LauncherSettings {
    std::string gameDir;        // install folder (cddata + perfect.exe)
    int scrWidth = 640, scrHeight = 480;
    float aspect = 0;           // 3D X aspect; 0 = auto
    float drawDistance = 0.6f;  // RendPow
    bool fullscreen = false;
    bool crop = true;           // show only the game's canvas
    bool showLauncher = true;   // show this window at startup
};

// Settings from graphics.ini (and the given game folder).
LauncherSettings LoadLauncherSettings(const std::string &prefDir, const std::string &gameDir);
// Writes the settings into graphics.ini, keeping other lines.
void SaveLauncherSettings(const std::string &prefDir, const LauncherSettings &s);

// Runs the settings window. checkDir returns "" for a usable game folder, otherwise the reason.
// Returns true for Start (settings updated), false for Quit.
bool RunLauncher(LauncherSettings &s, std::string (*checkDir)(const std::string &dir));
