// overlay.h - options menu drawn over the running game (gamepad Y, keyboard F10)
#pragma once
#include <cstdint>
#include <string>

struct OverlayOptions {
    int scrWidth = 640, scrHeight = 480;    // applies after a restart
    float drawDistance = 0;                 // RendPow fixed; 0 = auto; < 0 = original (only kept)
    float ddMax = 1.0f;                     // auto: largest draw distance
    int maxfps = 0;                         // frames per second at most; 0 = 60
    bool fpsCounter = false;
};

struct OverlayHost {
    OverlayOptions current;                 // as running and in graphics.ini
    std::string iniPath;                    // graphics.ini: changed values are written on closing
    void (*apply)(const OverlayOptions &);  // live changes (all but the resolution)
    float (*drawDistanceNow)();             // the draw distance in effect (RendPow)
};

void OverlayInit(const OverlayHost &host);

// Once per message-loop pass, with the game's key state (Windows virtual keys, gamepads mapped
// onto them) and whether the menu button (Y / F10) is held. While the menu is open, and after it
// closes until every key is released, it takes the input: keys are cleared and it returns true.
bool OverlayUpdate(uint8_t *keys, bool menuButton);
