// Achievements: see achievements.h.
//
// XDBF layout (big-endian): header { "XDBF", u32 version, u32 entryTableLength, u32 entryCount,
// u32 freeTableLength, u32 freeCount }, then entries { u16 namespace, u64 id, u32 offset,
// u32 length } and free entries (8 bytes); entry data starts after both tables.
// Namespace 1 (metadata) "XACH": { "XACH", u32 version, u32 size, u16 count } then 0x24-byte
// entries { u16 id, u16 nameString, u16 descriptionString, u16 lockedString, u32 imageId,
// u16 gamerscore, u16 pad, u32 flags, 16 bytes unused }. Namespace 2: images (PNG) by image id.
// Namespace 3: string tables "XSTR" per language id: { "XSTR", u32 version, u32 size, u16 count }
// then { u16 id, u16 length, UTF-8 bytes }.
#include "achievements.h"
#include "runtime.h"
#include "settings.h"

#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include <image.h>   // XenonUtils

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

namespace
{
    std::mutex g_lock;
    std::vector<AchievementDef> g_defs;
    std::map<uint32_t, std::vector<uint8_t>> g_images;
    bool g_loaded = false;

    uint16_t BE16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
    uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
    uint64_t BE64(const uint8_t* p) { return uint64_t(BE32(p)) << 32 | BE32(p + 4); }

    std::filesystem::path UnlockFile() { return PortDir() / "userdata" / "achievements.ini"; }

    void LoadUnlocks()
    {
        std::ifstream f(UnlockFile());
        std::string line;
        std::map<uint32_t, int64_t> when;
        while (std::getline(f, line))
        {
            unsigned id = 0;
            long long t = 0;
            if (sscanf_s(line.c_str(), "%u = %lld", &id, &t) == 2) when[id] = t;
        }
        for (auto& d : g_defs) if (auto it = when.find(d.id); it != when.end()) d.unlockedAt = it->second;
    }

    void SaveUnlocks()
    {
        std::error_code ec;
        std::filesystem::create_directories(UnlockFile().parent_path(), ec);
        std::ofstream f(UnlockFile(), std::ios::trunc);
        f << "# Peggle 2 recompiled: unlocked achievements (id = unix time)\n";
        for (auto& d : g_defs) if (d.unlockedAt) f << d.id << " = " << d.unlockedAt << "\n";
    }

    // ---- Pop-ups -----------------------------------------------------------------------------
    struct Popup { std::string title, detail; };
    std::deque<Popup> g_popups;
    std::chrono::steady_clock::time_point g_popupStart{};
    constexpr auto kPopupTime = std::chrono::milliseconds(5000);

    // A short two-note chime (synthesized: no asset needed), as a 16-bit mono WAV in memory.
    std::vector<uint8_t> Chime(int volumePercent)
    {
        const int rate = 44100, n = rate * 7 / 10;
        std::vector<int16_t> pcm(size_t(n), 0);
        const double notes[2][2] = { { 0.0, 880.0 }, { 0.12, 1318.5 } };   // start (s), frequency (Hz)
        for (auto& note : notes)
            for (int i = int(note[0] * rate); i < n; i++)
            {
                double t = double(i) / rate - note[0];
                double v = std::sin(2 * 3.14159265358979 * note[1] * t) * std::exp(-t * 5.0) * 0.35 * volumePercent / 100.0;
                pcm[size_t(i)] = int16_t(std::clamp(pcm[size_t(i)] + v * 32767.0, -32768.0, 32767.0));
            }
        std::vector<uint8_t> wav(44 + pcm.size() * 2);
        auto put32 = [&](size_t at, uint32_t v) { memcpy(&wav[at], &v, 4); };
        auto put16 = [&](size_t at, uint16_t v) { memcpy(&wav[at], &v, 2); };
        memcpy(&wav[0], "RIFF", 4); put32(4, uint32_t(wav.size() - 8)); memcpy(&wav[8], "WAVEfmt ", 8);
        put32(16, 16); put16(20, 1); put16(22, 1); put32(24, rate); put32(28, rate * 2); put16(32, 2); put16(34, 16);
        memcpy(&wav[36], "data", 4); put32(40, uint32_t(pcm.size() * 2));
        memcpy(&wav[44], pcm.data(), pcm.size() * 2);
        return wav;
    }

    void PlayPopupSound()
    {
        const Settings& s = GetSettings();
        if (s.popupVolume <= 0) return;
        static std::vector<uint8_t> sound;   // must stay alive while it plays (SND_ASYNC)
        sound.clear();
        if (!s.popupSound.empty())
        {
            std::ifstream f(std::filesystem::u8path(s.popupSound), std::ios::binary);
            sound.assign(std::istreambuf_iterator<char>(f), {});
        }
        if (sound.size() < 44) sound = Chime(s.popupVolume);
        PlaySoundW((LPCWSTR)sound.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
}

bool LoadAchievementsFromImage(const uint8_t* xex, size_t xexSize, const uint8_t* image, uint64_t imageBase,
                               size_t imageSize, uint32_t languageId)
{
    // Resource info header (key 0x000002FF) in the XEX header: { u32 size, { char name[8], u32 address, u32 size }... }
    if (xexSize < 0x18 || memcmp(xex, "XEX2", 4) != 0) return false;
    uint32_t headerCount = BE32(xex + 0x14);
    const uint8_t* xdbf = nullptr;
    size_t xdbfSize = 0;
    for (uint32_t i = 0; i < headerCount && 0x18 + i * 8 + 8 <= xexSize; i++)
    {
        if (BE32(xex + 0x18 + i * 8) != 0x000002FF) continue;
        uint32_t at = BE32(xex + 0x18 + i * 8 + 4);
        if (at + 4 > xexSize) break;
        uint32_t n = (BE32(xex + at) - 4) / 16;
        for (uint32_t j = 0; j < n && at + 4 + j * 16 + 16 <= xexSize; j++)
        {
            uint32_t addr = BE32(xex + at + 4 + j * 16 + 8), size = BE32(xex + at + 4 + j * 16 + 12);
            if (addr >= imageBase && addr - imageBase + size <= imageSize)
            {
                const uint8_t* p = image + (addr - imageBase);
                if (memcmp(p, "XDBF", 4) == 0) { xdbf = p; xdbfSize = size; }
            }
        }
    }
    if (!xdbf || xdbfSize < 0x18) { Log("[achievements] no XDBF resource in the game"); return false; }
    uint32_t tableLen = BE32(xdbf + 8), count = BE32(xdbf + 12), freeLen = BE32(xdbf + 16);
    size_t dataStart = 0x18 + size_t(tableLen) * 0x12 + size_t(freeLen) * 8;
    struct Entry { uint16_t ns; uint64_t id; const uint8_t* data; uint32_t len; };
    std::vector<Entry> entries;
    for (uint32_t i = 0; i < count && 0x18 + i * 0x12 + 0x12 <= xdbfSize; i++)
    {
        const uint8_t* e = xdbf + 0x18 + i * 0x12;
        uint32_t off = BE32(e + 10), len = BE32(e + 14);
        if (dataStart + off + len > xdbfSize) continue;
        entries.push_back({ BE16(e), BE64(e + 2), xdbf + dataStart + off, len });
    }
    auto find = [&](uint16_t ns, uint64_t id) -> const Entry* {
        for (auto& e : entries) if (e.ns == ns && e.id == id) return &e;
        return nullptr;
    };
    auto tag = [](const char* t) { return uint64_t(uint8_t(t[0])) << 24 | uint64_t(uint8_t(t[1])) << 16 | uint64_t(uint8_t(t[2])) << 8 | uint8_t(t[3]); };

    // Strings in the requested language, falling back to English.
    std::map<uint16_t, std::string> strings;
    auto loadStrings = [&](uint32_t lang) {
        const Entry* e = find(3, lang);
        if (!e || e->len < 14 || memcmp(e->data, "XSTR", 4) != 0) return false;
        uint16_t n = BE16(e->data + 12);
        size_t p = 14;
        for (uint16_t k = 0; k < n && p + 4 <= e->len; k++)
        {
            uint16_t sid = BE16(e->data + p), slen = BE16(e->data + p + 2);
            if (p + 4 + slen > e->len) break;
            strings.emplace(sid, std::string((const char*)e->data + p + 4, slen));   // emplace: first table wins
            p += 4 + slen;
        }
        return true;
    };
    bool haveLang = loadStrings(languageId);
    if (languageId != 1) loadStrings(1);

    const Entry* xach = find(1, tag("XACH"));
    if (!xach || xach->len < 14) { Log("[achievements] no achievement table"); return false; }
    std::vector<AchievementDef> defs;
    uint16_t n = BE16(xach->data + 12);
    for (uint16_t k = 0; k < n && 14 + size_t(k + 1) * 0x24 <= xach->len; k++)
    {
        const uint8_t* a = xach->data + 14 + size_t(k) * 0x24;
        AchievementDef d;
        d.id = BE16(a);
        d.name = strings[BE16(a + 2)];
        d.description = strings[BE16(a + 4)];
        d.lockedDescription = strings[BE16(a + 6)];
        d.imageId = BE32(a + 8);
        d.gamerscore = BE16(a + 12);
        d.secret = (BE32(a + 16) & 0x8) == 0;   // "show unachieved" flag clear: secret
        defs.push_back(std::move(d));
    }
    std::map<uint32_t, std::vector<uint8_t>> images;
    for (auto& e : entries)
        if (e.ns == 2) images[uint32_t(e.id)].assign(e.data, e.data + e.len);

    std::lock_guard<std::mutex> g(g_lock);
    g_defs = std::move(defs);
    g_images = std::move(images);
    LoadUnlocks();
    g_loaded = true;
    uint32_t unlocked = 0, total = 0, score = 0;
    for (auto& d : g_defs) { total += d.gamerscore; if (d.unlockedAt) { unlocked++; score += d.gamerscore; } }
    Log("[achievements] %zu achievements (%u G), %u unlocked (%u G); strings in language %u%s", g_defs.size(), total, unlocked,
        score, languageId, haveLang ? "" : " (not in the game: English)");
    return true;
}

bool LoadAchievements(const std::filesystem::path& gameDir, uint32_t languageId)
{
    std::ifstream f(gameDir / "Default.xex", std::ios::binary);
    std::vector<uint8_t> xex((std::istreambuf_iterator<char>(f)), {});
    if (xex.empty()) return false;
    Image image = Image::ParseImage(xex.data(), xex.size());
    if (!image.data) return false;
    return LoadAchievementsFromImage(xex.data(), xex.size(), image.data.get(), image.base, image.size, languageId);
}

const std::vector<AchievementDef>& Achievements() { return g_defs; }

const std::vector<uint8_t>* AchievementImage(uint32_t imageId)
{
    auto it = g_images.find(imageId);
    return it == g_images.end() ? nullptr : &it->second;
}

void AwardAchievement(uint32_t id)
{
    Popup p;
    {
        std::lock_guard<std::mutex> g(g_lock);
        AchievementDef* d = nullptr;
        for (auto& a : g_defs) if (a.id == id) d = &a;
        if (!d) { Log("[achievements] the game awarded unknown achievement %u", id); return; }
        if (d->unlockedAt) return;   // already had it (games re-send these)
        d->unlockedAt = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        SaveUnlocks();
        Log("[achievements] unlocked %u: %s (%u G)", id, d->name.c_str(), d->gamerscore);
        if (!GetSettings().achievementPopups) return;
        p.title = "Achievement unlocked";
        p.detail = std::to_string(d->gamerscore) + "G - " + d->name;
        g_popups.push_back(p);
        if (g_popups.size() == 1) g_popupStart = std::chrono::steady_clock::now();
    }
    PlayPopupSound();
}

bool CurrentAchievementPopup(std::string& title, std::string& detail)
{
    bool next = false;
    {
        std::lock_guard<std::mutex> g(g_lock);
        auto now = std::chrono::steady_clock::now();
        if (!g_popups.empty() && now - g_popupStart > kPopupTime)
        {
            g_popups.pop_front();
            g_popupStart = now;
            next = !g_popups.empty();   // the next queued pop-up starts now
        }
        if (!g_popups.empty())
        {
            title = g_popups.front().title;
            detail = g_popups.front().detail;
        }
    }
    if (next) PlayPopupSound();
    return !title.empty();
}
