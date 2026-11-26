// Achievements (achievements.cpp): definitions from the game's own XDBF resource in Default.xex
// (names, descriptions, gamerscore, icons), unlocks kept in userdata\achievements.ini, and an
// Xbox-style pop-up when the game awards one (XGI app message, xmsg.cpp).
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct AchievementDef
{
    uint16_t id = 0;
    uint16_t gamerscore = 0;
    uint32_t imageId = 0;
    bool secret = false;
    std::string name, description, lockedDescription;
    int64_t unlockedAt = 0;   // unix time, 0 = locked
};

// Parse the definitions from the game folder's Default.xex (decrypts it with XenonUtils).
// languageId: Xbox language (1 English ...). Safe to call more than once.
bool LoadAchievements(const std::filesystem::path& gameDir, uint32_t languageId);
// Same, from an already loaded guest image (base address + bytes) and the XEX header.
bool LoadAchievementsFromImage(const uint8_t* xexHeader, size_t xexSize, const uint8_t* image, uint64_t imageBase,
                               size_t imageSize, uint32_t languageId);
const std::vector<AchievementDef>& Achievements();
// PNG bytes of an achievement icon (empty if not found).
const std::vector<uint8_t>* AchievementImage(uint32_t imageId);
// The game earned achievement `id` (guest thread): record it and queue a pop-up.
void AwardAchievement(uint32_t id);
// For the renderer: the pop-up to show now, if any (title line, detail line).
bool CurrentAchievementPopup(std::string& title, std::string& detail);
uint32_t XboxLanguageId();   // kernel.cpp: the language the game is told
