// Port settings: peggle2_settings.ini next to the executable (plain text, "key = value" in
// [sections]), edited by the launcher (launcher.cpp) or by hand. Read once at startup; the
// launcher writes it back. Environment variables (PEGGLE2_60FPS, ...) still override.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

struct Settings
{
    // (No [launcher] options: the launcher always opens before the game.)

    // [display]
    bool fullscreen = false;           // borderless fullscreen on `monitor` (F11 / Alt+Enter toggle)
    int monitor = 0;                   // 0 = primary, 1.. = others in enumeration order
    int windowWidth = 0, windowHeight = 0;   // 0 = fit to the screen
    bool stretch = false;              // fill the window instead of letterboxing to 16:9
    bool vsync = true;

    // [graphics]
    int renderScale = 1;               // internal resolution: 1 = 720p ... 4 = 2880p
    int msaa = 1;                      // 1, 2 or 4
    bool fxaa = false;
    int anisotropy = 16;               // texture filtering: 1 (off) .. 16

    // [gameplay]
    bool fps60 = false;                // draw every logic update (60 fps); see hooks.cpp
    bool fpsCounter = false;           // F2
    bool perfOverlay = false;          // F3
    bool trial = false;                // run as the XBLA trial
    std::string language = "auto";    // auto, en, fr, de, es, it, ja, ...

    // [controls]
    // Controller remap: for each guest button (XINPUT bit order, see kButtonNames), the host
    // button that drives it.
    uint16_t buttonMap[16];
    // Keyboard bindings: virtual-key codes per guest control (kKeyControls order), 2 per control.
    int keys[24][2];
    int deadzone = 0;                  // percent of full stick travel ignored around centre
    bool invertLX = false, invertLY = false, invertRX = false, invertRY = false;
    bool vibration = true;
    int vibrationStrength = 100;       // percent

    // [extras]
    bool achievementPopups = true;
    std::string popupSound;            // empty = built-in chime, or a .wav path
    int popupVolume = 80;              // percent
    std::string updateMode = "ask";    // ask, auto, off
    std::string lastUpdateCheck;       // release tag seen last

    Settings();
};

// Guest controls in kKeyControls order (keyboard bindings) and the 16 XINPUT button bits.
extern const char* const kButtonNames[16];
struct KeyControl { const char* name; uint16_t button; int axis; int dir; };   // axis: 0 none, 1 LX, 2 LY, 3 RX, 4 RY, 5 LT, 6 RT
extern const KeyControl kKeyControls[];
extern const int kKeyControlCount;

Settings& GetSettings();
std::filesystem::path SettingsPath();
std::filesystem::path PortDir();           // folder of the executable
void LoadPortSettings();
bool SavePortSettings();
void ResetPortSettings();
// Apply settings that are environment switches elsewhere (PEGGLE2_60FPS ...), unless set already.
void ApplySettingsToEnvironment();
