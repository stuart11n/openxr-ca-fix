#include "tuning.h"

#include <windows.h>

#include "log.h"

namespace {

constexpr unsigned long long kHudFrames = 90 * 4;  // readout stays ~4 s after a key
constexpr int kRepeatDelay = 30;                   // frames before a held arrow repeats
constexpr int kRepeatEvery = 6;

const char* const kParamNames[] = {"blue", "red", "centre x", "centre y"};
const char* const kScopeNames[] = {"left eye", "right eye", "both eyes"};

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Edge-triggered key with optional auto-repeat while held.
struct Key {
    int vk;
    bool repeat;
    int held = 0;

    bool Fired(bool active) {
        if (!active || !Down(vk)) {
            held = 0;
            return false;
        }
        ++held;
        if (held == 1) return true;
        return repeat && held > kRepeatDelay && (held - kRepeatDelay) % kRepeatEvery == 0;
    }
};

Key g_up{VK_UP, false}, g_down{VK_DOWN, false}, g_left{VK_LEFT, true}, g_right{VK_RIGHT, true};
Key g_test{'T', false}, g_enable{'E', false}, g_leftEye{'L', false}, g_rightEye{'R', false},
    g_both{'B', false};

float* Value(CaConfig& c, int param, int eye) {
    switch (param) {
        case 0: return &c.blue[eye];
        case 1: return &c.red[eye];
        case 2: return &c.centerX[eye];
        default: return &c.centerY[eye];
    }
}

float Value(const CaConfig& c, int param, int eye) {
    switch (param) {
        case 0: return c.blue[eye];
        case 1: return c.red[eye];
        case 2: return c.centerX[eye];
        default: return c.centerY[eye];
    }
}

}  // namespace

bool PollTuningKeys(CaConfig& config, TuningUi& ui, unsigned long long frame) {
    const bool active = Down(VK_CONTROL) && Down(VK_SHIFT);
    bool changed = false, touched = false;

    if (g_test.Fired(active)) {
        ui.testPattern = !ui.testPattern;
        Log("tuning: test grid %s", ui.testPattern ? "on" : "off");
        touched = true;
    }
    if (g_enable.Fired(active)) {
        config.enabled = !config.enabled;
        Log("tuning: correction %s", config.enabled ? "on" : "off");
        changed = touched = true;
    }
    if (g_leftEye.Fired(active)) {
        ui.scope = 0;
        touched = true;
    }
    if (g_rightEye.Fired(active)) {
        ui.scope = 1;
        touched = true;
    }
    if (g_both.Fired(active)) {
        ui.scope = 2;
        touched = true;
    }
    if (g_up.Fired(active)) {
        ui.param = (ui.param + 3) % 4;
        touched = true;
    }
    if (g_down.Fired(active)) {
        ui.param = (ui.param + 1) % 4;
        touched = true;
    }

    int dir = 0;
    if (g_left.Fired(active)) dir = -1;
    if (g_right.Fired(active)) dir = +1;
    if (dir) {
        const float step = ui.param < 2 ? 0.25f : 10.0f;
        for (int eye = 0; eye < 2; ++eye) {
            if (ui.scope != 2 && ui.scope != eye) continue;
            *Value(config, ui.param, eye) += dir * step;
        }
        Log("tuning: %s (%s) -> L=%.2f R=%.2f", kParamNames[ui.param], kScopeNames[ui.scope],
            *Value(config, ui.param, 0), *Value(config, ui.param, 1));
        changed = touched = true;
    }
    if (touched) ui.hudUntilFrame = frame + kHudFrames;
    return changed;
}

bool HudVisible(const TuningUi& ui, unsigned long long frame) {
    return ui.testPattern || frame < ui.hudUntilFrame;
}

bool HudInEye(const TuningUi& ui, int eye) { return ui.scope == 2 || ui.scope == eye; }

float HudParam(const TuningUi& ui) { return static_cast<float>(ui.param + 1); }

float HudValue(const CaConfig& config, const TuningUi& ui, int eye) {
    return Value(config, ui.param, eye);
}
