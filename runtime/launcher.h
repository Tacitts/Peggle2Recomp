// Launcher window (launcher.cpp): settings before the game starts.
#pragma once
#include <filesystem>
#include <functional>

struct HWND__;

struct LauncherInfo
{
    bool gameFound = false;
    std::filesystem::path gameDir;
    // Optional pieces supplied by other modules (left empty when unavailable).
    std::function<bool(HWND__* owner)> installPackage;                                   // install.cpp (asks for the file)
    std::function<bool(HWND__* owner, const std::filesystem::path& file)> installPackageFrom; // install.cpp (dropped file)
    std::function<void(HWND__* owner, bool interactive)> checkForUpdates;               // updater.cpp
};

bool LauncherWanted(bool gameFound);        // settings, Shift held, or game missing
bool RunLauncher(const LauncherInfo& info); // true: start the game
