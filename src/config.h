#pragma once

#include <string>

// Tunables read from %LOCALAPPDATA%\openxr-ca-fix\config.ini. The file is
// created with defaults on first run, re-read whenever it changes, and
// rewritten when values are changed with the in-headset hotkeys.
//
// All values are in pixels of the game's eye image, measured 45 degrees off
// centre, so they come out as everyday numbers like 1.5 rather than tiny
// coefficients. The layer converts them per view (see ProcessView).
// Arrays are per eye: 0 = left, 1 = right.
struct CaConfig {
    bool enabled = true;
    // 1 = re-read config.ini while running and accept the tuning hotkeys.
    // 0 = read the file once when VR starts, then leave it and the keyboard alone.
    bool liveUpdate = false;
    // How far to pull each colour inward at 45 degrees off centre. Negative
    // pushes it outward. The shift grows with the square of the distance from
    // centre, so it is zero in the middle and largest at the edges.
    float red[2] = {0.0f, 0.0f};
    float blue[2] = {0.0f, 0.0f};
    // Extra pull that grows with the fourth power of distance (both eyes).
    float redOuter = 0.0f, blueOuter = 0.0f;
    // Lens centre in pixels from straight ahead. +x moves it right, +y up.
    float centerX[2] = {0.0f, 0.0f};
    float centerY[2] = {0.0f, 0.0f};

    bool IsIdentity() const {
        return red[0] == 0 && red[1] == 0 && blue[0] == 0 && blue[1] == 0 && redOuter == 0 &&
               blueOuter == 0;
    }
};

// Returns true if the config was (re)loaded. Cheap to call often: it only
// touches the file when its modification time changes.
bool ReloadConfigIfChanged(CaConfig& config);

// Reads config.ini now regardless of its timestamp (once per VR session).
void ForceReloadConfig(CaConfig& config);

// Rewrites config.ini with these values (comments are regenerated).
void SaveConfig(const CaConfig& config);
