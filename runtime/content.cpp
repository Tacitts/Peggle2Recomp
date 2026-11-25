// Storage devices and content packages (save games): the device selector, device data and
// XamContent* create/close/delete. One hard drive (device id 1) is always present; content
// packages are host directories under userdata/content/<content type>/<file name>, and an open
// package is mounted at its root name ("save:" etc.) through MountContentRoot (fs.cpp).
// Layouts follow Xenia's published structures (xam_content.cc / xam_ui.cc).
#include "runtime.h"

namespace
{
    constexpr uint32_t kHddDeviceId = 1;
    constexpr uint32_t kDeviceTypeHdd = 1;
    constexpr uint64_t kDeviceBytes = 20ull << 30;   // reported size: 20 GB, all free

    constexpr uint32_t X_ERROR_FILE_NOT_FOUND = 0x2;
    constexpr uint32_t X_ERROR_PATH_NOT_FOUND = 0x3;
    constexpr uint32_t X_ERROR_ALREADY_EXISTS = 0xB7;
    constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 0x48F;
    constexpr uint32_t XN_SYS_UI = 0x00000009;      // param 1 = system UI shown, 0 = hidden

    // XCONTENT_DATA: u32 deviceId, u32 contentType, u16 displayName[128], char fileName[42]
    struct ContentData
    {
        uint32_t deviceId = 0, contentType = 0;
        std::string fileName;
    };

    ContentData ReadContentData(uint32_t p)
    {
        ContentData c;
        c.deviceId = Load32(p + 0x00);
        c.contentType = Load32(p + 0x04);
        for (uint32_t i = 0; i < 42; i++)
        {
            char ch = char(Load8(p + 0x108 + i));
            if (!ch) break;
            // Keep the host path inside the content directory.
            c.fileName.push_back((ch == '\\' || ch == '/' || ch == ':' || ch == '.') ? '_' : ch);
        }
        return c;
    }

    std::filesystem::path ContentDir(const ContentData& c)
    {
        char type[16];
        snprintf(type, sizeof(type), "%08X", c.contentType);
        return UserDataDir() / "content" / type / (c.fileName.empty() ? std::string("_") : c.fileName);
    }
}

// u32 XamShowDeviceSelectorUI(u32 userIndex, u32 contentType, u32 contentFlags,
//                             u64 totalRequested, u32* deviceId, XOVERLAPPED*)
// No UI: the only device (the hard drive) is picked, as the dashboard does with one device.
IMPORT(XamShowDeviceSelectorUI)
{
    uint32_t deviceIdPtr = ctx.r7.u32, ov = ctx.r8.u32;
    Log("[content] device selector (user %u, content type 0x%X) -> hard drive", ctx.r3.u32, ctx.r4.u32);
    PostNotification(XN_SYS_UI, 1);
    if (deviceIdPtr) Store32(deviceIdPtr, kHddDeviceId);
    PostNotification(XN_SYS_UI, 0);
    ctx.r3.u64 = ov ? CompleteOverlapped(ov, X_ERROR_SUCCESS) : X_ERROR_SUCCESS;
}

// u32 XamContentGetDeviceData(u32 deviceId, X_CONTENT_DEVICE_DATA*)
// { u32 deviceId; u32 deviceType; u64 totalBytes; u64 freeBytes; u16 name[28]; }
IMPORT(XamContentGetDeviceData)
{
    uint32_t id = ctx.r3.u32, data = ctx.r4.u32;
    if ((id & 0x0FFFFFFF) != kHddDeviceId) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    if (data)
    {
        memset(GuestPtr(data), 0, 0x50);
        Store32(data + 0x00, kHddDeviceId);
        Store32(data + 0x04, kDeviceTypeHdd);
        Store64(data + 0x08, kDeviceBytes);
        Store64(data + 0x10, kDeviceBytes);
        const char* name = "Hard Drive";
        for (uint32_t i = 0; name[i]; i++) Store16(data + 0x18 + i * 2, uint16_t(name[i]));
    }
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// u32 XamContentCreateEx(u32 userIndex, char* rootName, XCONTENT_DATA*, u32 flags,
//                        u32* disposition, u32* licenseMask, u32 cacheSize, u64 contentSize,
//                        XOVERLAPPED*)
// flags & 0xF: 1 create new, 2 create always, 3 open existing, 4 open always, 5 truncate existing.
IMPORT(XamContentCreateEx)
{
    std::string root = LoadCString(ctx.r4.u32, 64);
    ContentData c = ReadContentData(ctx.r5.u32);
    uint32_t mode = ctx.r6.u32 & 0xF, dispositionPtr = ctx.r7.u32, licensePtr = ctx.r8.u32;
    uint32_t ov = StackArg(ctx, 8);

    auto dir = ContentDir(c);
    std::error_code ec;
    bool exists = std::filesystem::is_directory(dir, ec);
    uint32_t result = X_ERROR_SUCCESS, disposition = 0;
    switch (mode)
    {
    case 1: if (exists) result = X_ERROR_ALREADY_EXISTS; else disposition = 1; break;
    case 2: if (exists) std::filesystem::remove_all(dir, ec); disposition = 1; break;
    case 3: if (!exists) result = X_ERROR_PATH_NOT_FOUND; else disposition = 2; break;
    case 4: disposition = exists ? 2 : 1; break;
    case 5:
        if (!exists) result = X_ERROR_PATH_NOT_FOUND;
        else { std::filesystem::remove_all(dir, ec); disposition = 2; }
        break;
    default: result = X_ERROR_PATH_NOT_FOUND; break;
    }
    if (result == X_ERROR_SUCCESS)
    {
        std::filesystem::create_directories(dir, ec);
        MountContentRoot(root, dir);
    }
    if (dispositionPtr) Store32(dispositionPtr, disposition);
    if (licensePtr) Store32(licensePtr, 0);
    Log("[content] create '%s' -> %s (type 0x%X, mode %u): result 0x%X, disposition %u", root.c_str(),
        c.fileName.c_str(), c.contentType, mode, result, disposition);
    ctx.r3.u64 = ov ? CompleteOverlapped(ov, result) : result;
}

// u32 XamContentClose(char* rootName, XOVERLAPPED*)
IMPORT(XamContentClose)
{
    std::string root = LoadCString(ctx.r3.u32, 64);
    uint32_t result = UnmountContentRoot(root) ? X_ERROR_SUCCESS : X_ERROR_FILE_NOT_FOUND;
    Log("[content] close '%s': result 0x%X", root.c_str(), result);
    ctx.r3.u64 = ctx.r4.u32 ? CompleteOverlapped(ctx.r4.u32, result) : result;
}

// u32 XamContentDelete(u32 userIndex, XCONTENT_DATA*, XOVERLAPPED*)
IMPORT(XamContentDelete)
{
    ContentData c = ReadContentData(ctx.r4.u32);
    auto dir = ContentDir(c);
    std::error_code ec;
    uint32_t result = std::filesystem::remove_all(dir, ec) > 0 ? X_ERROR_SUCCESS : X_ERROR_FILE_NOT_FOUND;
    Log("[content] delete %s (type 0x%X): result 0x%X", c.fileName.c_str(), c.contentType, result);
    ctx.r3.u64 = ctx.r5.u32 ? CompleteOverlapped(ctx.r5.u32, result) : result;
}
