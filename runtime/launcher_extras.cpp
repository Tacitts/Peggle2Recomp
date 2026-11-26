// Registers the optional launcher pieces that live in their own modules: installing the game
// from the player's package (install.cpp) and checking for port updates (updater.cpp).
#include "launcher.h"
#include "runtime.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

bool InstallGamePackage(HWND owner);
bool InstallGamePackageFrom(HWND owner, const std::filesystem::path& file);
void CheckForUpdates(HWND owner, bool interactive);

void AddLauncherExtras(LauncherInfo& info)
{
    info.installPackage = [](HWND__* owner) { return InstallGamePackage(owner); };
    info.installPackageFrom = [](HWND__* owner, const std::filesystem::path& file) { return InstallGamePackageFrom(owner, file); };
    info.checkForUpdates = [](HWND__* owner, bool interactive) { CheckForUpdates(owner, interactive); };
}
