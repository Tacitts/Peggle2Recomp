// Port settings (settings.h): parse / write peggle2_settings.ini.
#include "settings.h"
#include "runtime.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

const char* const kButtonNames[16] = {
    "DPadUp", "DPadDown", "DPadLeft", "DPadRight", "Start", "Back", "LeftStick", "RightStick",
    "LB", "RB", "Unused10", "Unused11", "A", "B", "X", "Y",
};

// Keyboard controls. In Peggle 2: left stick aims, A fires, the triggers fine-aim, Start pauses.
const KeyControl kKeyControls[] = {
    { "DPadUp", 0x0001, 0, 0 },   { "DPadDown", 0x0002, 0, 0 }, { "DPadLeft", 0x0004, 0, 0 }, { "DPadRight", 0x0008, 0, 0 },
    { "Start", 0x0010, 0, 0 },    { "Back", 0x0020, 0, 0 },     { "LeftStick", 0x0040, 0, 0 }, { "RightStick", 0x0080, 0, 0 },
    { "LB", 0x0100, 0, 0 },       { "RB", 0x0200, 0, 0 },       { "A", 0x1000, 0, 0 },         { "B", 0x2000, 0, 0 },
    { "X", 0x4000, 0, 0 },        { "Y", 0x8000, 0, 0 },
    { "AimLeft", 0, 1, -1 },      { "AimRight", 0, 1, 1 },      { "AimUp", 0, 2, 1 },          { "AimDown", 0, 2, -1 },
    { "RightStickLeft", 0, 3, -1 }, { "RightStickRight", 0, 3, 1 }, { "RightStickUp", 0, 4, 1 }, { "RightStickDown", 0, 4, -1 },
    { "LT", 0, 5, 1 },            { "RT", 0, 6, 1 },
};
const int kKeyControlCount = int(sizeof(kKeyControls) / sizeof(kKeyControls[0]));
static_assert(sizeof(kKeyControls) / sizeof(kKeyControls[0]) == 24, "Settings::keys has 24 rows");

Settings::Settings()
{
    for (int i = 0; i < 16; i++) buttonMap[i] = uint16_t(1u << i);
    static const int def[24][2] = {
        { VK_UP, 0 }, { VK_DOWN, 0 }, { VK_LEFT, 0 }, { VK_RIGHT, 0 },
        { VK_RETURN, 'P' }, { VK_TAB, VK_BACK }, { 0, 0 }, { 0, 0 },
        { 'Q', 0 }, { 'E', 0 }, { VK_SPACE, 'Z' }, { VK_ESCAPE, 'X' },
        { 'C', 0 }, { 'V', 0 },
        { 'A', 0 }, { 'D', 0 }, { 'W', 0 }, { 'S', 0 },
        { 'J', 0 }, { 'L', 0 }, { 'I', 0 }, { 'K', 0 },
        { VK_SHIFT, 0 }, { VK_CONTROL, 0 },
    };
    memcpy(keys, def, sizeof(keys));
}

static Settings g_settings;
Settings& GetSettings() { return g_settings; }

std::filesystem::path PortDir()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

std::filesystem::path SettingsPath() { return PortDir() / "peggle2_settings.ini"; }

namespace
{
    std::string Trim(const std::string& s)
    {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
    bool ParseBool(const std::string& v, bool def)
    {
        if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
        if (v == "false" || v == "0" || v == "no" || v == "off") return false;
        return def;
    }
    int ParseInt(const std::string& v, int def, int lo, int hi)
    {
        char* end = nullptr;
        long n = strtol(v.c_str(), &end, 0);
        if (end == v.c_str()) return def;
        return int(std::clamp<long>(n, lo, hi));
    }
}

void LoadPortSettings()
{
    g_settings = Settings();
    std::ifstream f(SettingsPath());
    if (!f) { Log("[settings] %s not found: defaults", SettingsPath().string().c_str()); return; }
    std::map<std::string, std::string> kv;
    std::string line, section;
    while (std::getline(f, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') { section = Trim(line.substr(1, line.size() - 2)); continue; }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string v = Trim(line.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        kv[section + "." + Trim(line.substr(0, eq))] = v;
    }
    Settings& s = g_settings;
    auto get = [&](const char* k) -> const std::string* { auto it = kv.find(k); return it == kv.end() ? nullptr : &it->second; };
    auto B = [&](const char* k, bool& out) { if (auto* v = get(k)) out = ParseBool(*v, out); };
    auto I = [&](const char* k, int& out, int lo, int hi) { if (auto* v = get(k)) out = ParseInt(*v, out, lo, hi); };
    auto S = [&](const char* k, std::string& out) { if (auto* v = get(k)) out = *v; };
    B("display.fullscreen", s.fullscreen);
    I("display.monitor", s.monitor, 0, 16);
    I("display.window_width", s.windowWidth, 0, 16384);
    I("display.window_height", s.windowHeight, 0, 16384);
    B("display.stretch", s.stretch);
    B("display.vsync", s.vsync);
    I("graphics.render_scale", s.renderScale, 1, 4);
    I("graphics.msaa", s.msaa, 1, 4);
    if (s.msaa == 3) s.msaa = 2;
    B("graphics.fxaa", s.fxaa);
    I("graphics.anisotropy", s.anisotropy, 1, 16);
    B("gameplay.fps60", s.fps60);
    B("gameplay.fps_counter", s.fpsCounter);
    B("gameplay.perf_overlay", s.perfOverlay);
    B("gameplay.trial", s.trial);
    S("gameplay.language", s.language);
    for (int i = 0; i < 16; i++)
    {
        std::string k = std::string("controls.button_") + kButtonNames[i];
        if (auto* v = get(k.c_str())) s.buttonMap[i] = uint16_t(ParseInt(*v, s.buttonMap[i], 0, 0xFFFF));
    }
    for (int i = 0; i < kKeyControlCount; i++)
    {
        std::string k = std::string("controls.key_") + kKeyControls[i].name;
        if (auto* v = get(k.c_str()))
        {
            int a = 0, b = 0;
            if (sscanf_s(v->c_str(), "%i , %i", &a, &b) >= 1) { s.keys[i][0] = std::clamp(a, 0, 255); s.keys[i][1] = std::clamp(b, 0, 255); }
        }
    }
    I("controls.deadzone", s.deadzone, 0, 90);
    B("controls.invert_lx", s.invertLX);
    B("controls.invert_ly", s.invertLY);
    B("controls.invert_rx", s.invertRX);
    B("controls.invert_ry", s.invertRY);
    B("controls.vibration", s.vibration);
    I("controls.vibration_strength", s.vibrationStrength, 0, 100);
    B("extras.achievement_popups", s.achievementPopups);
    S("extras.popup_sound", s.popupSound);
    I("extras.popup_volume", s.popupVolume, 0, 100);
    S("extras.update_mode", s.updateMode);
    if (s.updateMode != "ask" && s.updateMode != "auto" && s.updateMode != "off") s.updateMode = "ask";
    S("extras.last_update_check", s.lastUpdateCheck);
    Log("[settings] loaded %s", SettingsPath().string().c_str());
}

bool SavePortSettings()
{
    const Settings& s = g_settings;
    std::ostringstream o;
    auto b = [](bool v) { return v ? "true" : "false"; };
    o << "# Peggle 2 recompiled: settings (written by the launcher; safe to edit by hand)\n\n";
    o << "[display]\nfullscreen = " << b(s.fullscreen) << "\nmonitor = " << s.monitor
      << "\nwindow_width = " << s.windowWidth << "\nwindow_height = " << s.windowHeight
      << "\nstretch = " << b(s.stretch) << "\nvsync = " << b(s.vsync) << "\n\n";
    o << "[graphics]\nrender_scale = " << s.renderScale << "\nmsaa = " << s.msaa << "\nfxaa = " << b(s.fxaa)
      << "\nanisotropy = " << s.anisotropy << "\n\n";
    o << "[gameplay]\nfps60 = " << b(s.fps60) << "\nfps_counter = " << b(s.fpsCounter) << "\nperf_overlay = " << b(s.perfOverlay)
      << "\ntrial = " << b(s.trial) << "\nlanguage = " << s.language << "\n\n";
    o << "[controls]\n";
    for (int i = 0; i < 16; i++)
        if (i != 10 && i != 11)
        {
            char buf[16];
            snprintf(buf, sizeof(buf), "0x%04X", s.buttonMap[i]);
            o << "button_" << kButtonNames[i] << " = " << buf << "\n";
        }
    for (int i = 0; i < kKeyControlCount; i++) o << "key_" << kKeyControls[i].name << " = " << s.keys[i][0] << ", " << s.keys[i][1] << "\n";
    o << "deadzone = " << s.deadzone << "\ninvert_lx = " << b(s.invertLX) << "\ninvert_ly = " << b(s.invertLY)
      << "\ninvert_rx = " << b(s.invertRX) << "\ninvert_ry = " << b(s.invertRY) << "\nvibration = " << b(s.vibration)
      << "\nvibration_strength = " << s.vibrationStrength << "\n\n";
    o << "[extras]\nachievement_popups = " << b(s.achievementPopups) << "\npopup_sound = \"" << s.popupSound << "\""
      << "\npopup_volume = " << s.popupVolume << "\nupdate_mode = " << s.updateMode
      << "\nlast_update_check = " << s.lastUpdateCheck << "\n";
    std::ofstream f(SettingsPath(), std::ios::binary | std::ios::trunc);
    if (!f) { Log("[settings] cannot write %s", SettingsPath().string().c_str()); return false; }
    f << o.str();
    return bool(f);
}

void ResetPortSettings() { g_settings = Settings(); }

void ApplySettingsToEnvironment()
{
    auto set = [](const char* name, const char* value) {
        if (!getenv(name)) _putenv_s(name, value);   // an explicit environment variable wins
    };
    if (g_settings.fps60) set("PEGGLE2_60FPS", "1");
}
