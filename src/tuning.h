#pragma once

#include "config.h"

// In-headset tuning: Ctrl+Shift hotkeys change the config live, and a small
// readout in the view shows what is being adjusted. See the config.ini header
// for the key list.
struct TuningUi {
    int param = 0;             // 0 blue, 1 red, 2 centre x, 3 centre y
    int scope = 2;             // 0 left eye, 1 right eye, 2 both
    bool testPattern = false;  // grid instead of the game image
    unsigned long long hudUntilFrame = 0;
};

// Polls the keyboard once per frame. Returns true if the config changed
// (the caller saves it).
bool PollTuningKeys(CaConfig& config, TuningUi& ui, unsigned long long frame);

bool HudVisible(const TuningUi& ui, unsigned long long frame);
bool HudInEye(const TuningUi& ui, int eye);
// 1 blue, 2 red, 3 centre x, 4 centre y (matches the shader's hudParam).
float HudParam(const TuningUi& ui);
float HudValue(const CaConfig& config, const TuningUi& ui, int eye);
