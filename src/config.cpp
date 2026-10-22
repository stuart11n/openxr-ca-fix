#include "config.h"

#include <windows.h>
#include <share.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "log.h"

namespace {

const char kHeader[] =
    "; openxr-ca-fix: corrects colour fringing near the edges of the lenses.\n"
    ";\n"
    "; live_update=1 turns on tuning while you fly: this file is re-read about twice\n"
    "; a second and the hotkeys below work. With live_update=0 (the default) the file\n"
    "; is read once when VR starts and the keyboard is ignored. Changing it from 0\n"
    "; to 1 by hand takes effect the next time VR starts.\n"
    ";\n"
    "; EASIEST WAY TO TUNE (needs live_update=1): in the headset, hold Ctrl+Shift and press\n"
    ";   T            test grid on/off (white lines on black, replaces the game image)\n"
    ";   Up / Down    choose what to adjust; the box at the centre of the view shows it:\n"
    ";                  blue border = blue, red border = red,\n"
    ";                  white border = lens centre left/right, yellow = centre up/down\n"
    ";   Left / Right decrease / increase it\n"
    ";   L / R / B    adjust the left eye only / right eye only / both eyes\n"
    ";                  (the box only shows in the eyes being adjusted)\n"
    ";   E            correction on/off, for a before/after\n"
    "; Every change is saved to this file straight away.\n"
    ";\n"
    "; What to look for, at a line near an edge of the view:\n"
    ";   colour on the line's OUTER side (facing away from the middle) -> increase it\n"
    ";   colour on the line's INNER side (facing the middle)           -> decrease it\n"
    ";   magenta outside -> increase red and blue; green outside -> decrease both\n"
    ";   yellow inside + blue outside is one problem: blue too far out, increase blue\n"
    "; If one side of an eye needs a different amount than the other side, move\n"
    "; that eye's lens centre towards the side that is over-corrected.\n"
    ";\n"
    "; Values: red/blue are pixels of pull towards the middle, measured 45 degrees\n"
    "; off centre (negative pushes outward). *_outer adds pull that only shows near\n"
    "; the far edge. Centres are pixels from straight ahead, +x right, +y up.\n"
    "; You can also edit this file by hand; saved edits apply within a second.\n"
    "\n";

std::string ConfigPath() {
    char base[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::string dir = std::string(base) + "\\openxr-ca-fix";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\config.ini";
}

const std::string& Path() {
    static const std::string path = ConfigPath();
    return path;
}

void Trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    s = (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
}

// Returns false for keys from the first config format (raw coefficients).
bool Apply(CaConfig& c, const std::string& key, const std::string& value, bool& rewrite) {
    if (key == "red_k1" || key == "red_k2" || key == "blue_k1" || key == "blue_k2") return false;
    float f = static_cast<float>(atof(value.c_str()));
    if (key == "enabled") c.enabled = f != 0.0f;
    else if (key == "live_update") c.liveUpdate = f != 0.0f;
    else if (key == "left_red") c.red[0] = f;
    else if (key == "right_red") c.red[1] = f;
    else if (key == "left_blue") c.blue[0] = f;
    else if (key == "right_blue") c.blue[1] = f;
    else if (key == "red" || key == "blue") {
        // Earlier format: one value for both eyes. Rewrite in the per-eye format.
        float* v = key == "red" ? c.red : c.blue;
        v[0] = v[1] = f;
        rewrite = true;
    }
    else if (key == "red_outer") c.redOuter = f;
    else if (key == "blue_outer") c.blueOuter = f;
    else if (key == "left_center_x") c.centerX[0] = f;
    else if (key == "left_center_y") c.centerY[0] = f;
    else if (key == "right_center_x") c.centerX[1] = f;
    else if (key == "right_center_y") c.centerY[1] = f;
    else Log("config: unknown key '%s'", key.c_str());
    return true;
}

std::string Generate(const CaConfig& c) {
    char values[512];
    snprintf(values, sizeof(values),
             "live_update=%d\n"
             "enabled=%d\n"
             "\n"
             "left_red=%.2f\n"
             "left_blue=%.2f\n"
             "right_red=%.2f\n"
             "right_blue=%.2f\n"
             "\n"
             "red_outer=%.2f\n"
             "blue_outer=%.2f\n"
             "\n"
             "left_center_x=%.0f\n"
             "left_center_y=%.0f\n"
             "right_center_x=%.0f\n"
             "right_center_y=%.0f\n",
             c.liveUpdate ? 1 : 0, c.enabled ? 1 : 0, c.red[0], c.blue[0], c.red[1], c.blue[1],
             c.redOuter, c.blueOuter,
             c.centerX[0], c.centerY[0], c.centerX[1], c.centerY[1]);
    return std::string(kHeader) + values;
}

FILETIME g_lastWrite{};
bool g_haveFile = false;

bool Write(const CaConfig& c) {
    FILE* f = _fsopen(Path().c_str(), "w", _SH_DENYWR);
    if (!f) return false;
    fputs(Generate(c).c_str(), f);
    fclose(f);
    // Remember our own write so it isn't reloaded as an outside edit.
    WIN32_FILE_ATTRIBUTE_DATA attrs;
    if (GetFileAttributesExA(Path().c_str(), GetFileExInfoStandard, &attrs)) {
        g_lastWrite = attrs.ftLastWriteTime;
        g_haveFile = true;
    }
    return true;
}

}  // namespace

void ForceReloadConfig(CaConfig& config) {
    g_haveFile = false;
    ReloadConfigIfChanged(config);
}

void SaveConfig(const CaConfig& config) {
    if (Path().empty()) return;
    if (!Write(config)) Log("config: could not write %s", Path().c_str());
}

bool ReloadConfigIfChanged(CaConfig& config) {
    const std::string& path = Path();
    if (path.empty()) return false;

    WIN32_FILE_ATTRIBUTE_DATA attrs;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attrs)) {
        if (Write(CaConfig())) Log("config: wrote defaults to %s", path.c_str());
        config = CaConfig();
        return true;
    }
    if (g_haveFile && CompareFileTime(&attrs.ftLastWriteTime, &g_lastWrite) == 0) return false;

    FILE* f = _fsopen(path.c_str(), "r", _SH_DENYNO);
    if (!f) return false;  // editor may hold it briefly; retry next time
    g_lastWrite = attrs.ftLastWriteTime;
    g_haveFile = true;

    CaConfig c;
    bool oldFormat = false, rewrite = false, sawLiveUpdate = false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        size_t semi = s.find(';');
        if (semi != std::string::npos) s.resize(semi);
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq), value = s.substr(eq + 1);
        Trim(key);
        Trim(value);
        if (key == "live_update") sawLiveUpdate = true;
        if (!key.empty() && !Apply(c, key, value, rewrite)) oldFormat = true;
    }
    fclose(f);
    if (oldFormat) {
        // The first format's coefficients don't translate to pixels without
        // knowing the view; keep the old file and start fresh.
        std::string backup = path.substr(0, path.size() - 4) + ".v1.ini";
        MoveFileExA(path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING);
        Log("config: old format moved to %s", backup.c_str());
        c = CaConfig();
        Write(c);
    } else if (rewrite || !sawLiveUpdate) {
        // Older files: rewrite so new keys like live_update show up in the file.
        Write(c);
    }
    config = c;
    Log("config: live_update=%d enabled=%d red L/R=(%g, %g) blue L/R=(%g, %g) outer r/b=(%g, %g) "
        "centre L=(%g, %g) R=(%g, %g)",
        c.liveUpdate ? 1 : 0, c.enabled ? 1 : 0, c.red[0], c.red[1], c.blue[0], c.blue[1],
        c.redOuter, c.blueOuter,
        c.centerX[0], c.centerY[0], c.centerX[1], c.centerY[1]);
    return true;
}
