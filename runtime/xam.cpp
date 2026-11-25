// XAM (system services) imports: notifications, the signed-in profile, profile settings,
// licensing.
#include "runtime.h"

#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

uint32_t CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t length)
{
    if (!ov) return result;
    Store32(ov + 0x00, result);
    Store32(ov + 0x04, length);
    // Extended error: HRESULT_FROM_WIN32(result), as XGetOverlappedExtendedError reports it.
    Store32(ov + 0x18, result ? (0x80070000u | (result & 0xFFFF)) : 0);
    if (uint32_t routine = Load32(ov + 0x10))
        Log("[xam] overlapped completion routine 0x%08X not called (APCs not implemented)", routine);
    if (uint32_t ev = Load32(ov + 0x0C)) SignalHandle(ev);
    return X_ERROR_IO_PENDING;
}

// ---- Notification listeners -------------------------------------------------------------------
// A listener is a waitable handle (signalled while notifications are queued) plus a queue of
// {id, param}. XNotifyGetNext pops matching entries. Nothing is posted yet; when system events
// are emulated (sign-in, storage, audio device changes), PostNotification() feeds them in.
namespace
{
    struct Listener
    {
        uint64_t mask;
        uint32_t event;   // guest event object
        std::deque<std::pair<uint32_t, uint32_t>> queue;
    };
    std::mutex g_lock;
    std::unordered_map<uint32_t, Listener> g_listeners;   // handle -> listener
}

void PostNotification(uint32_t id, uint32_t param)
{
    // Notification ids carry their area in bits 25+ (XNID macro); listeners filter by mask.
    uint32_t area = 1u << ((id >> 25) & 0x3F);
    std::lock_guard<std::mutex> g(g_lock);
    for (auto& [h, l] : g_listeners)
    {
        if (!(l.mask & area)) continue;
        l.queue.push_back({ id, param });
        SignalGuestEvent(l.event, true);
    }
}

// HANDLE XamNotifyCreateListener(u64 areaMask, u32 version)
IMPORT(XamNotifyCreateListener)
{
    uint32_t event = 0;
    uint32_t h = CreateEventHandle(false, false, &event);
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_listeners[h] = Listener{ ctx.r3.u64, event, {} };
    }
    Log("[xam] notification listener 0x%08X for areas 0x%llX", h, (unsigned long long)ctx.r3.u64);
    // Like the dashboard: a new system-area listener immediately learns who is signed in
    // (XN_SYS_SIGNINCHANGED, param = bitmask of signed-in users).
    if (ctx.r3.u64 & 1) PostNotification(0x0000000A, 0x1);
    ctx.r3.u64 = h;
}

// ---- Signed-in profile ------------------------------------------------------------------------
// One local (offline) profile in slot 0.
namespace
{
    constexpr uint64_t kXuid = 0xE000000000000001ull;   // offline XUIDs start with 0xE
    constexpr const char* kGamertag = "Player";
    constexpr uint32_t X_ERROR_NO_SUCH_USER = 0x525;
    constexpr uint32_t X_ERROR_INSUFFICIENT_BUFFER = 0x7A;
}

// u32 XamUserGetSigninState(u32 userIndex): 0 none, 1 local, 2 Live
IMPORT(XamUserGetSigninState) { ctx.r3.u64 = ctx.r3.u32 == 0 ? 1 : 0; }

// u32 XamUserGetSigninInfo(u32 userIndex, u32 flags, X_USER_SIGNIN_INFO* info)
// { u64 xuid; u32 flags; u32 signinState; u32 guestNumber; u32 sponsorIndex; char name[16]; }
IMPORT(XamUserGetSigninInfo)
{
    uint32_t user = ctx.r3.u32, info = ctx.r5.u32;
    memset(GuestPtr(info), 0, 0x28);
    if (user != 0) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    Store64(info + 0x00, kXuid);
    Store32(info + 0x08, 0);
    Store32(info + 0x0C, 1);
    memcpy(GuestPtr(info + 0x18), kGamertag, strlen(kGamertag));
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// u32 XamUserGetXUID(u32 userIndex, u32 typeMask, u64* xuid)
IMPORT(XamUserGetXUID)
{
    if (ctx.r3.u32 != 0) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    if (ctx.r5.u32) Store64(ctx.r5.u32, kXuid);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// u32 XamUserGetName(u32 userIndex, char* buffer, u32 size)
IMPORT(XamUserGetName)
{
    if (ctx.r3.u32 != 0) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    uint32_t buf = ctx.r4.u32, size = ctx.r5.u32;
    if (buf && size)
    {
        size_t n = std::min<size_t>(strlen(kGamertag), size - 1);
        memcpy(GuestPtr(buf), kGamertag, n);
        Store8(buf + uint32_t(n), 0);
    }
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// u32 XamUserCheckPrivilege(u32 userIndex, u32 privilege, BOOL* result): everything allowed
IMPORT(XamUserCheckPrivilege)
{
    if (ctx.r5.u32) Store32(ctx.r5.u32, 1);
    ctx.r3.u64 = ctx.r3.u32 == 0 ? X_ERROR_SUCCESS : X_ERROR_NO_SUCH_USER;
}

// u32 XamUserAreUsersFriends(u32 userIndex, u64* xuids, u32 count, BOOL* results, XOVERLAPPED*)
IMPORT(XamUserAreUsersFriends)
{
    uint32_t results = ctx.r6.u32, count = ctx.r5.u32;
    for (uint32_t i = 0; results && i < count; i++) Store32(results + i * 4, 0);
    ctx.r3.u64 = CompleteOverlapped(ctx.r7.u32, X_ERROR_SUCCESS);
}

// u32 XamUserGetDeviceContext(u32 userIndex, u32 unknown, u32* out)
IMPORT(XamUserGetDeviceContext)
{
    if (ctx.r5.u32) Store32(ctx.r5.u32, 0);
    ctx.r3.u64 = ctx.r3.u32 == 0 ? X_ERROR_SUCCESS : X_ERROR_NO_SUCH_USER;
}

// u32 XamContentGetLicenseMask(u32* mask, XOVERLAPPED*): full game
IMPORT(XamContentGetLicenseMask)
{
    if (ctx.r3.u32) Store32(ctx.r3.u32, 0xFFFFFFFF);
    ctx.r3.u64 = CompleteOverlapped(ctx.r4.u32, X_ERROR_SUCCESS);
}

// ---- Enumerators ---------------------------------------------------------------------------
// Achievements / stats enumerators. Nothing is stored yet, so every enumeration is empty;
// what matters is that creation reports a sane buffer size and a real handle (the game
// allocates the reported size: an unwritten size once produced a 1 GB request and bad_alloc).
namespace
{
    struct Enumerator : KernelObject
    {
        const char* TypeName() const override { return "Enumerator"; }
        uint32_t itemSize = 0, count = 0;
    };
    constexpr uint32_t X_ERROR_NO_MORE_FILES = 0x12;
}

// XamUserCreateAchievementEnumerator(titleId, userIndex, u64 xuid, flags, startIndex, count,
//                                    u32* bufferSize, HANDLE* handle)
IMPORT(XamUserCreateAchievementEnumerator)
{
    uint32_t count = ctx.r8.u32, sizePtr = ctx.r9.u32, handlePtr = ctx.r10.u32;
    auto e = std::make_shared<Enumerator>();
    e->itemSize = 0x24 + 0x400;   // X_ACHIEVEMENT_DETAILS + room for its strings
    e->count = count;
    if (sizePtr) Store32(sizePtr, e->itemSize * std::max<uint32_t>(count, 1));
    if (handlePtr) Store32(handlePtr, CreateHandle(e));
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamUserCreateStatsEnumerator(titleId, unk1, unk2, numStatSpecs, statSpecs*, u32* bufferSize, HANDLE*)
IMPORT(XamUserCreateStatsEnumerator)
{
    uint32_t count = ctx.r6.u32, sizePtr = ctx.r8.u32, handlePtr = ctx.r9.u32;
    auto e = std::make_shared<Enumerator>();
    e->itemSize = 0x40;
    e->count = count;
    if (sizePtr) Store32(sizePtr, e->itemSize * std::max<uint32_t>(count, 1));
    if (handlePtr) Store32(handlePtr, CreateHandle(e));
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamEnumerate(HANDLE, flags, buffer, bufferSize, u32* itemsReturned, XOVERLAPPED*)
IMPORT(XamEnumerate)
{
    uint32_t items = ctx.r7.u32, ov = ctx.r8.u32;
    if (items) Store32(items, 0);
    if (ov) { ctx.r3.u64 = CompleteOverlapped(ov, X_ERROR_NO_MORE_FILES); return; }
    ctx.r3.u64 = X_ERROR_NO_MORE_FILES;
}

// ---- Profile settings -----------------------------------------------------------------------
// Setting id: bits 28-31 type (1 s32, 2 s64, 3 f64, 4 UTF-16 string, 5 f32, 6 binary,
// 7 FILETIME), bits 16-27 maximum size, bits 0-15 index. Titles keep save data in the
// title-specific binary settings (0x63E83FFF / 3FFE / 3FFD), so writes are persisted to
// userdata/profile/settings.bin.
//
// Entry layout (0x28 bytes): +0 from (0 none, 1 default, 2 title), +8 user index / xuid,
// +0x10 setting id, +0x18 X_USER_DATA { u8 type; pad[7]; value at +8 } where binary/string
// values are { u32 size; u32 ptr } at +0x20/+0x24.
namespace
{
    std::mutex g_settingsLock;
    std::map<uint32_t, std::vector<uint8_t>> g_settings;   // id -> raw value bytes (big-endian)
    bool g_settingsLoaded = false;

    std::filesystem::path SettingsPath()
    {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        return std::filesystem::path(exe).parent_path() / "userdata" / "profile" / "settings.bin";
    }

    void LoadSettings()
    {
        if (g_settingsLoaded) return;
        g_settingsLoaded = true;
        std::ifstream f(SettingsPath(), std::ios::binary);
        uint32_t id, len;
        while (f.read((char*)&id, 4) && f.read((char*)&len, 4))
        {
            std::vector<uint8_t> v(len);
            if (len && !f.read((char*)v.data(), len)) break;
            g_settings[id] = std::move(v);
        }
        Log("[xam] profile settings loaded: %zu stored values", g_settings.size());
    }

    void SaveSettings()
    {
        auto p = SettingsPath();
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        for (auto& [id, v] : g_settings)
        {
            uint32_t len = uint32_t(v.size());
            f.write((const char*)&id, 4);
            f.write((const char*)&len, 4);
            f.write((const char*)v.data(), len);
        }
    }

    bool IsSized(uint32_t type) { return type == 4 || type == 6; }
}

// u32 XamUserReadProfileSettings(u32 titleId, u32 userIndex, u32 xuidCount, u64* xuids,
//     u32 settingCount, u32* settingIds, u32* bufferSize, buffer, XOVERLAPPED*)
IMPORT(XamUserReadProfileSettings)
{
    uint32_t user = ctx.r4.u32, xuidCount = ctx.r5.u32, xuids = ctx.r6.u32, count = ctx.r7.u32;
    uint32_t ids = ctx.r8.u32, sizePtr = ctx.r9.u32, buffer = ctx.r10.u32, ov = StackArg(ctx, 8);
    uint32_t users = xuidCount ? xuidCount : 1;
    // User 0xFF = "any user" (the voice engine queries dashboard title 0xFFFE07D1 this way,
    // first with a NULL buffer to get the size, and fails with E_FAIL on anything but 0x7A).
    if (!xuidCount && user != 0 && user != 0xFF) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    if (user == 0xFF) user = 0;

    uint32_t entries = count * users;
    uint32_t needed = 8 + entries * 0x28;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t id = Load32(ids + i * 4);
        if (IsSized(id >> 28)) needed += ((id >> 16) & 0xFFF) * users;
    }
    uint32_t have = Load32(sizePtr);
    if (!buffer || have < needed)
    {
        Store32(sizePtr, needed);
        ctx.r3.u64 = X_ERROR_INSUFFICIENT_BUFFER;
        return;
    }

    std::lock_guard<std::mutex> g(g_settingsLock);
    LoadSettings();
    memset(GuestPtr(buffer), 0, needed);
    Store32(buffer + 0, entries);
    Store32(buffer + 4, buffer + 8);
    uint32_t extra = buffer + 8 + entries * 0x28;
    for (uint32_t u = 0; u < users; u++)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t id = Load32(ids + i * 4), type = id >> 28, maxSize = (id >> 16) & 0xFFF;
            uint32_t e = buffer + 8 + (u * count + i) * 0x28;
            if (xuidCount) Store64(e + 0x08, Load64(xuids + u * 8));
            else Store32(e + 0x08, user);
            Store32(e + 0x10, id);
            Store8(e + 0x18, uint8_t(type));
            auto it = g_settings.find(id);
            if (IsSized(type))
            {
                uint32_t len = 0;
                if (it != g_settings.end())
                {
                    len = std::min<uint32_t>(uint32_t(it->second.size()), maxSize);
                    memcpy(GuestPtr(extra), it->second.data(), len);
                }
                Store32(e + 0x20, len);
                Store32(e + 0x24, extra);
                Store32(e + 0x00, it != g_settings.end() ? 2 : 0);
                extra += maxSize;
            }
            else
            {
                if (it != g_settings.end() && it->second.size() == 8)
                    memcpy(GuestPtr(e + 0x20), it->second.data(), 8);
                Store32(e + 0x00, it != g_settings.end() ? 2 : 1);   // stored, else profile default (0)
            }
        }
    }
    Log("[xam] read %u profile settings (title 0x%08X)", entries, ctx.r3.u32);
    ctx.r3.u64 = CompleteOverlapped(ov, X_ERROR_SUCCESS);
}

// u32 XamUserWriteProfileSettings(u32 titleId, u32 userIndex, u32 count, settings*, XOVERLAPPED*)
IMPORT(XamUserWriteProfileSettings)
{
    uint32_t count = ctx.r5.u32, settings = ctx.r6.u32, ov = ctx.r7.u32;
    std::lock_guard<std::mutex> g(g_settingsLock);
    LoadSettings();
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t e = settings + i * 0x28;
        uint32_t id = Load32(e + 0x10), type = Load8(e + 0x18);
        std::vector<uint8_t> v;
        if (IsSized(type))
        {
            uint32_t len = Load32(e + 0x20), ptr = Load32(e + 0x24);
            v.assign(GuestPtr(ptr), GuestPtr(ptr) + len);
        }
        else
        {
            v.assign(GuestPtr(e + 0x20), GuestPtr(e + 0x20) + 8);
        }
        g_settings[id] = std::move(v);
    }
    SaveSettings();
    Log("[xam] wrote %u profile settings", count);
    ctx.r3.u64 = CompleteOverlapped(ov, X_ERROR_SUCCESS);
}

// BOOL XNotifyGetNext(HANDLE listener, u32 matchId, u32* idOut, u32* paramOut)
IMPORT(XNotifyGetNext)
{
    uint32_t h = ctx.r3.u32, match = ctx.r4.u32, idOut = ctx.r5.u32, paramOut = ctx.r6.u32;
    std::lock_guard<std::mutex> g(g_lock);
    auto it = g_listeners.find(h);
    if (it == g_listeners.end()) { ctx.r3.u64 = 0; return; }
    auto& q = it->second.queue;
    for (auto e = q.begin(); e != q.end(); ++e)
    {
        if (match && e->first != match) continue;
        if (idOut) Store32(idOut, e->first);
        if (paramOut) Store32(paramOut, e->second);
        q.erase(e);
        if (q.empty()) SignalGuestEvent(it->second.event, false);
        ctx.r3.u64 = 1;
        return;
    }
    ctx.r3.u64 = 0;
}
