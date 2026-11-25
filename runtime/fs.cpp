// File system imports: Nt*File on top of the host file system, plus the handle table.
//
// Guest path mapping (case-insensitive, backslash separators):
//   game:  d:  \Device\Cdrom0  \Device\Harddisk0\Partition1   -> <game dir>  (read-only)
//   any other drive (cache:, update:, content drives, ...)     -> <exe dir>\userdata\<drive>
// (No comment line may end in a backslash: it would continue onto the next line.)
#include "runtime.h"

#include <mutex>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace fs = std::filesystem;

// ---- Handle table ---------------------------------------------------------------------------
static std::mutex g_handleLock;
static std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_handles;
static uint32_t g_nextHandle = 0x00100000;

uint32_t CreateHandle(std::shared_ptr<KernelObject> obj)
{
    std::lock_guard<std::mutex> g(g_handleLock);
    uint32_t h = g_nextHandle;
    g_nextHandle += 4;
    g_handles[h] = std::move(obj);
    return h;
}

std::shared_ptr<KernelObject> LookupHandle(uint32_t handle)
{
    std::lock_guard<std::mutex> g(g_handleLock);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second;
}

bool CloseHandle(uint32_t handle)
{
    std::lock_guard<std::mutex> g(g_handleLock);
    return g_handles.erase(handle) != 0;
}

IMPORT(NtClose)
{
    // Unknown handles still report success: other object types are not tracked yet.
    CloseHandle(ctx.r3.u32);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---- Files ------------------------------------------------------------------------------------
struct FileObject : KernelObject
{
    const char* TypeName() const override { return "File"; }
    HANDLE host = INVALID_HANDLE_VALUE;
    fs::path path;
    std::string guestPath;
    bool directory = false;
    uint64_t position = 0;
    // Directory enumeration state for NtQueryDirectoryFile.
    std::vector<fs::directory_entry> entries;
    size_t nextEntry = 0;
    bool enumerated = false;
    ~FileObject() override { if (host != INVALID_HANDLE_VALUE) ::CloseHandle(host); }
};

fs::path UserDataDir()
{
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path() / "userdata";
}

static std::string LowerStr(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// Content roots opened by XamContentCreateEx ("save:" -> userdata/content/...). Checked
// before the default <drive> -> userdata/<drive> mapping.
static std::mutex g_mountLock;
static std::unordered_map<std::string, fs::path> g_mounts;   // lower-case root name -> host dir

void MountContentRoot(const std::string& root, const fs::path& dir)
{
    std::lock_guard<std::mutex> g(g_mountLock);
    g_mounts[LowerStr(root)] = dir;
}

bool UnmountContentRoot(const std::string& root)
{
    std::lock_guard<std::mutex> g(g_mountLock);
    return g_mounts.erase(LowerStr(root)) != 0;
}

static bool LookupMount(const std::string& lowerRoot, fs::path* dir)
{
    std::lock_guard<std::mutex> g(g_mountLock);
    auto it = g_mounts.find(lowerRoot);
    if (it == g_mounts.end()) return false;
    *dir = it->second;
    return true;
}

// Translate a guest path to a host path. Returns empty on unknown device.
static fs::path MapGuestPath(const std::string& guest, bool* readOnly)
{
    std::string p = guest;
    for (auto& c : p) if (c == '/') c = '\\';
    std::string lower = LowerStr(p);
    *readOnly = false;

    static const char* gamePrefixes[] = { "game:", "d:", "\\device\\cdrom0", "\\device\\harddisk0\\partition1" };
    for (const char* pre : gamePrefixes)
    {
        size_t n = strlen(pre);
        if (lower.compare(0, n, pre) == 0)
        {
            *readOnly = true;
            std::string rest = p.substr(n);
            while (!rest.empty() && rest[0] == '\\') rest.erase(0, 1);
            return rest.empty() ? g_gameDir : g_gameDir / rest;
        }
    }
    size_t colon = p.find(':');
    if (colon != std::string::npos && colon < 32)
    {
        std::string drive = LowerStr(p.substr(0, colon));
        std::string rest = p.substr(colon + 1);
        while (!rest.empty() && rest[0] == '\\') rest.erase(0, 1);
        fs::path dir;
        if (!LookupMount(drive, &dir))
            dir = UserDataDir() / drive;
        std::error_code ec;
        fs::create_directories(dir, ec);
        return rest.empty() ? dir : dir / rest;
    }
    return {};
}

// OBJECT_ATTRIBUTES { u32 RootDirectory; u32 ObjectName (ANSI_STRING*); u32 Attributes; }
static std::string ObjectAttributesPath(uint32_t oa)
{
    if (!oa) return {};
    uint32_t root = Load32(oa + 0);
    uint32_t name = Load32(oa + 4);
    std::string path;
    if (name)
    {
        uint16_t len = Load16(name);
        uint32_t buf = Load32(name + 4);
        path.assign((const char*)GuestPtr(buf), len);
    }
    if (root)
    {
        if (auto dir = LookupHandleAs<FileObject>(root))
            path = dir->guestPath + (dir->guestPath.empty() || dir->guestPath.back() == '\\' ? "" : "\\") + path;
    }
    return path;
}

static uint64_t ToU64(FILETIME ft) { return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

static void SetIosb(uint32_t iosb, uint32_t status, uint32_t info)
{
    if (!iosb) return;
    Store32(iosb + 0, status);
    Store32(iosb + 4, info);
}

// FILE_NETWORK_OPEN_INFORMATION: 4 x u64 times, u64 alloc, u64 eof, u32 attributes, u32 pad
static bool WriteNetworkOpenInfo(const fs::path& p, uint32_t out)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &d)) return false;
    uint64_t size = (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    bool dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    Store64(out + 0x00, ToU64(d.ftCreationTime));
    Store64(out + 0x08, ToU64(d.ftLastAccessTime));
    Store64(out + 0x10, ToU64(d.ftLastWriteTime));
    Store64(out + 0x18, ToU64(d.ftLastWriteTime));
    Store64(out + 0x20, (size + 0xFFF) & ~0xFFFull);
    Store64(out + 0x28, size);
    Store32(out + 0x30, dir ? 0x10 : 0x80);
    Store32(out + 0x34, 0);
    return true;
}

// NTSTATUS NtQueryFullAttributesFile(OBJECT_ATTRIBUTES* oa, FILE_NETWORK_OPEN_INFORMATION* out)
IMPORT(NtQueryFullAttributesFile)
{
    std::string guest = ObjectAttributesPath(ctx.r3.u32);
    bool ro;
    fs::path host = MapGuestPath(guest, &ro);
    bool ok = !host.empty() && WriteNetworkOpenInfo(host, ctx.r4.u32);
    Log("[fs] query %s -> %s", guest.c_str(), ok ? "found" : "not found");
    ctx.r3.u64 = ok ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
}

static uint32_t OpenFile(uint32_t handleOut, uint32_t access, uint32_t oa, uint32_t iosb, uint32_t disposition, const char* api)
{
    std::string guest = ObjectAttributesPath(oa);
    bool readOnly;
    fs::path host = MapGuestPath(guest, &readOnly);
    if (host.empty())
    {
        Log("[fs] %s %s -> unknown device", api, guest.c_str());
        SetIosb(iosb, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    auto obj = std::make_shared<FileObject>();
    obj->path = host;
    obj->guestPath = guest;
    std::error_code ec;
    bool exists = fs::exists(host, ec);
    obj->directory = exists && fs::is_directory(host, ec);

    constexpr uint32_t GENERIC_WRITE_ = 0x40000000, FILE_WRITE_DATA_ = 0x2, FILE_APPEND_DATA_ = 0x4;
    bool wantWrite = (access & (GENERIC_WRITE_ | FILE_WRITE_DATA_ | FILE_APPEND_DATA_)) != 0;
    // Dispositions: 0 supersede, 1 open, 2 create, 3 open_if, 4 overwrite, 5 overwrite_if
    if (!obj->directory)
    {
        if (readOnly && (wantWrite || disposition == 0 || disposition == 2 || disposition >= 4))
        {
            if (!exists || wantWrite)
            {
                Log("[fs] %s %s -> denied (read-only game data)", api, guest.c_str());
                SetIosb(iosb, STATUS_ACCESS_DENIED, 0);
                return STATUS_ACCESS_DENIED;
            }
        }
        DWORD hostDisp;
        switch (disposition)
        {
        case 0: hostDisp = CREATE_ALWAYS; break;
        case 1: hostDisp = OPEN_EXISTING; break;
        case 2: hostDisp = CREATE_NEW; break;
        case 3: hostDisp = OPEN_ALWAYS; break;
        case 4: hostDisp = TRUNCATE_EXISTING; break;
        default: hostDisp = CREATE_ALWAYS; break;
        }
        if (readOnly) hostDisp = OPEN_EXISTING;
        if (!exists && (hostDisp == OPEN_EXISTING || hostDisp == TRUNCATE_EXISTING))
        {
            Log("[fs] %s %s -> not found", api, guest.c_str());
            SetIosb(iosb, STATUS_OBJECT_NAME_NOT_FOUND, 0);
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if (!readOnly) fs::create_directories(host.parent_path(), ec);
        obj->host = CreateFileW(host.wstring().c_str(), readOnly || !wantWrite ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, hostDisp, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (obj->host == INVALID_HANDLE_VALUE)
        {
            DWORD err = GetLastError();
            uint32_t st = err == ERROR_FILE_EXISTS ? STATUS_OBJECT_NAME_COLLISION :
                          err == ERROR_PATH_NOT_FOUND ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;
            Log("[fs] %s %s -> host error %lu", api, guest.c_str(), err);
            SetIosb(iosb, st, 0);
            return st;
        }
    }
    else if (!exists)
    {
        SetIosb(iosb, STATUS_OBJECT_NAME_NOT_FOUND, 0);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    uint32_t h = CreateHandle(obj);
    Store32(handleOut, h);
    SetIosb(iosb, STATUS_SUCCESS, exists ? 1 : 2); // FILE_OPENED / FILE_CREATED
    Log("[fs] %s %s -> handle 0x%08X%s", api, guest.c_str(), h, obj->directory ? " (directory)" : "");
    return STATUS_SUCCESS;
}

// NtCreateFile(u32* handle, u32 access, OBJECT_ATTRIBUTES*, IO_STATUS_BLOCK*, u64* allocSize,
//              u32 attributes, u32 share, u32 disposition, u32 options)
IMPORT(NtCreateFile)
{
    ctx.r3.u64 = OpenFile(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, "create");
}

// NtOpenFile(u32* handle, u32 access, OBJECT_ATTRIBUTES*, IO_STATUS_BLOCK*, u32 share, u32 options)
IMPORT(NtOpenFile)
{
    ctx.r3.u64 = OpenFile(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 1, "open");
}

// NtReadFile(handle, event, apcRoutine, apcContext, IO_STATUS_BLOCK*, buffer, length, u64* offset)
IMPORT(NtReadFile)
{
    auto f = LookupHandleAs<FileObject>(ctx.r3.u32);
    uint32_t iosb = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, offsetPtr = ctx.r10.u32;
    if (!f || f->host == INVALID_HANDLE_VALUE)
    {
        SetIosb(iosb, STATUS_INVALID_HANDLE, 0);
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    uint64_t offset = f->position;
    if (offsetPtr)
    {
        uint64_t o = Load64(offsetPtr);
        if (o != 0xFFFFFFFFFFFFFFFEull) offset = o; // FILE_USE_FILE_POINTER_POSITION
    }
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset);
    ov.OffsetHigh = DWORD(offset >> 32);
    DWORD got = 0;
    BOOL ok = ReadFile(f->host, GuestPtr(buffer), length, &got, &ov);
    if (!ok && GetLastError() != ERROR_HANDLE_EOF)
    {
        SetIosb(iosb, STATUS_END_OF_FILE, 0);
        ctx.r3.u64 = STATUS_END_OF_FILE;
        return;
    }
    f->position = offset + got;
    uint32_t status = (got == 0 && length != 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    SetIosb(iosb, status, got);
    ctx.r3.u64 = status;
}

// NtReadFileScatter(handle, event, apc, apcCtx, IO_STATUS_BLOCK*, FILE_SEGMENT_ELEMENT* segs, length, u64* offset)
// Each segment element is a u64 whose low 32 bits are a page-sized guest buffer.
IMPORT(NtReadFileScatter)
{
    auto f = LookupHandleAs<FileObject>(ctx.r3.u32);
    uint32_t iosb = ctx.r7.u32, segs = ctx.r8.u32, length = ctx.r9.u32, offsetPtr = ctx.r10.u32;
    if (!f || f->host == INVALID_HANDLE_VALUE) { SetIosb(iosb, STATUS_INVALID_HANDLE, 0); ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint64_t offset = offsetPtr ? Load64(offsetPtr) : f->position;
    uint32_t total = 0;
    for (uint32_t i = 0; total < length; i++)
    {
        uint32_t page = uint32_t(Load64(segs + i * 8));
        uint32_t chunk = std::min<uint32_t>(0x1000, length - total);
        OVERLAPPED ov{};
        ov.Offset = DWORD(offset + total);
        ov.OffsetHigh = DWORD((offset + total) >> 32);
        DWORD got = 0;
        ReadFile(f->host, GuestPtr(page), chunk, &got, &ov);
        total += got;
        if (got < chunk) break;
    }
    f->position = offset + total;
    SetIosb(iosb, STATUS_SUCCESS, total);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtWriteFile(handle, event, apcRoutine, apcContext, IO_STATUS_BLOCK*, buffer, length, u64* offset)
IMPORT(NtWriteFile)
{
    auto f = LookupHandleAs<FileObject>(ctx.r3.u32);
    uint32_t iosb = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, offsetPtr = ctx.r10.u32;
    if (!f || f->host == INVALID_HANDLE_VALUE) { SetIosb(iosb, STATUS_INVALID_HANDLE, 0); ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint64_t offset = f->position;
    if (offsetPtr)
    {
        uint64_t o = Load64(offsetPtr);
        if (o != 0xFFFFFFFFFFFFFFFEull) offset = o;
    }
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset);
    ov.OffsetHigh = DWORD(offset >> 32);
    DWORD wrote = 0;
    BOOL ok = WriteFile(f->host, GuestPtr(buffer), length, &wrote, &ov);
    f->position = offset + wrote;
    SetIosb(iosb, ok ? STATUS_SUCCESS : STATUS_ACCESS_DENIED, wrote);
    ctx.r3.u64 = ok ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
}

IMPORT(NtFlushBuffersFile)
{
    if (auto f = LookupHandleAs<FileObject>(ctx.r3.u32); f && f->host != INVALID_HANDLE_VALUE) FlushFileBuffers(f->host);
    ctx.r3.u64 = STATUS_SUCCESS;
}

static uint64_t HostFileSize(const FileObject& f)
{
    LARGE_INTEGER sz{};
    if (f.host != INVALID_HANDLE_VALUE) GetFileSizeEx(f.host, &sz);
    return uint64_t(sz.QuadPart);
}

// NtQueryInformationFile(handle, IO_STATUS_BLOCK*, buffer, length, class)
IMPORT(NtQueryInformationFile)
{
    auto f = LookupHandleAs<FileObject>(ctx.r3.u32);
    uint32_t iosb = ctx.r4.u32, out = ctx.r5.u32, length = ctx.r6.u32, cls = ctx.r7.u32;
    if (!f) { SetIosb(iosb, STATUS_INVALID_HANDLE, 0); ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint64_t size = HostFileSize(*f);
    uint32_t written = 0;
    switch (cls)
    {
    case 5: // FileStandardInformation
        Store64(out + 0x00, (size + 0xFFF) & ~0xFFFull);
        Store64(out + 0x08, size);
        Store32(out + 0x10, 1);
        Store8(out + 0x14, 0);
        Store8(out + 0x15, f->directory ? 1 : 0);
        written = 0x18;
        break;
    case 14: // FilePositionInformation
        Store64(out, f->position);
        written = 8;
        break;
    case 6: // FileInternalInformation
        Store64(out, std::hash<std::string>{}(f->path.string()));
        written = 8;
        break;
    case 34: // FileNetworkOpenInformation
        WriteNetworkOpenInfo(f->path, out);
        written = 0x38;
        break;
    case 4: // FileBasicInformation: 4 x u64 times, u32 attributes
    {
        uint8_t tmp[0x38];
        uint32_t scratch = AllocRuntime(0x40);
        WriteNetworkOpenInfo(f->path, scratch);
        memcpy(tmp, GuestPtr(scratch), 0x38);
        memcpy(GuestPtr(out), tmp, 0x20);
        Store32(out + 0x20, Load32(scratch + 0x30));
        written = 0x28;
        break;
    }
    default:
        Log("[fs] NtQueryInformationFile: unhandled class %u (length 0x%X) on %s", cls, length, f->guestPath.c_str());
        SetIosb(iosb, STATUS_INVALID_PARAMETER, 0);
        ctx.r3.u64 = STATUS_INVALID_PARAMETER;
        return;
    }
    SetIosb(iosb, STATUS_SUCCESS, written);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtSetInformationFile(handle, IO_STATUS_BLOCK*, buffer, length, class)
IMPORT(NtSetInformationFile)
{
    auto f = LookupHandleAs<FileObject>(ctx.r3.u32);
    uint32_t iosb = ctx.r4.u32, in = ctx.r5.u32, cls = ctx.r7.u32;
    if (!f) { SetIosb(iosb, STATUS_INVALID_HANDLE, 0); ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    switch (cls)
    {
    case 14: // FilePositionInformation
        f->position = Load64(in);
        break;
    case 20: // FileEndOfFileInformation
    case 19: // FileAllocationInformation
        if (f->host != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER li;
            li.QuadPart = LONGLONG(Load64(in));
            SetFilePointerEx(f->host, li, nullptr, FILE_BEGIN);
            SetEndOfFile(f->host);
        }
        break;
    case 13: // FileDispositionInformation (delete on close): ignored for now
        break;
    default:
        Log("[fs] NtSetInformationFile: unhandled class %u on %s", cls, f->guestPath.c_str());
        break;
    }
    SetIosb(iosb, STATUS_SUCCESS, 0);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtQueryVolumeInformationFile(handle, IO_STATUS_BLOCK*, buffer, length, class)
IMPORT(NtQueryVolumeInformationFile)
{
    uint32_t iosb = ctx.r4.u32, out = ctx.r5.u32, cls = ctx.r7.u32;
    switch (cls)
    {
    case 3: // FileFsSizeInformation: u64 total units, u64 free units, u32 sectors/unit, u32 bytes/sector
        Store64(out + 0x00, 0x100000);
        Store64(out + 0x08, 0x80000);
        Store32(out + 0x10, 8);
        Store32(out + 0x14, 512);
        SetIosb(iosb, STATUS_SUCCESS, 0x18);
        ctx.r3.u64 = STATUS_SUCCESS;
        return;
    case 5: // FileFsAttributeInformation: u32 attrs, u32 max name len, u32 name len, wchar name[]
        Store32(out + 0x00, 0);
        Store32(out + 0x04, 255);
        Store32(out + 0x08, 8);
        Store16(out + 0x0C, 'F'); Store16(out + 0x0E, 'A'); Store16(out + 0x10, 'T'); Store16(out + 0x12, 'X');
        SetIosb(iosb, STATUS_SUCCESS, 0x14);
        ctx.r3.u64 = STATUS_SUCCESS;
        return;
    }
    Log("[fs] NtQueryVolumeInformationFile: unhandled class %u", cls);
    SetIosb(iosb, STATUS_INVALID_PARAMETER, 0);
    ctx.r3.u64 = STATUS_INVALID_PARAMETER;
}

// NtQueryDirectoryFile(handle, event, apc, apcCtx, IO_STATUS_BLOCK*, buffer, length, ANSI_STRING* mask)
// Returns one FILE_DIRECTORY_INFORMATION per call:
//   u32 next, u32 index, 4 x u64 times, u64 eof, u64 alloc, u32 attributes, u32 nameLength, char name[]
IMPORT(NtQueryDirectoryFile)
{
    auto f = LookupHandleAs<FileObject>(ctx.r3.u32);
    uint32_t iosb = ctx.r7.u32, out = ctx.r8.u32, length = ctx.r9.u32, mask = ctx.r10.u32;
    if (!f || !f->directory) { SetIosb(iosb, STATUS_INVALID_HANDLE, 0); ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if (!f->enumerated)
    {
        std::string pattern = "*";
        if (mask)
        {
            uint16_t len = Load16(mask);
            if (len) pattern.assign((const char*)GuestPtr(Load32(mask + 4)), len);
        }
        std::error_code ec;
        for (auto& e : fs::directory_iterator(f->path, ec))
        {
            std::string name = e.path().filename().string();
            if (pattern == "*" || pattern == "*.*" || LowerStr(name) == LowerStr(pattern) ||
                (pattern.size() > 1 && pattern[0] == '*' && LowerStr(name).ends_with(LowerStr(pattern.substr(1)))))
                f->entries.push_back(e);
        }
        f->enumerated = true;
    }
    if (f->nextEntry >= f->entries.size())
    {
        SetIosb(iosb, STATUS_NO_MORE_FILES, 0);
        ctx.r3.u64 = STATUS_NO_MORE_FILES;
        return;
    }
    auto& e = f->entries[f->nextEntry];
    std::string name = e.path().filename().string();
    uint32_t need = 0x40 + uint32_t(name.size());
    if (need > length) { SetIosb(iosb, 0x80000005, 0); ctx.r3.u64 = 0x80000005; return; } // BUFFER_OVERFLOW
    uint32_t info = AllocRuntime(0x40);
    WriteNetworkOpenInfo(e.path(), info);
    memset(GuestPtr(out), 0, need);
    Store32(out + 0x00, 0);
    Store32(out + 0x04, uint32_t(f->nextEntry));
    memcpy(GuestPtr(out + 0x08), GuestPtr(info), 0x20);     // times
    Store64(out + 0x28, Load64(info + 0x28));                // eof
    Store64(out + 0x30, Load64(info + 0x20));                // alloc
    Store32(out + 0x38, Load32(info + 0x30));                // attributes
    Store32(out + 0x3C, uint32_t(name.size()));
    memcpy(GuestPtr(out + 0x40), name.data(), name.size());
    f->nextEntry++;
    SetIosb(iosb, STATUS_SUCCESS, need);
    ctx.r3.u64 = STATUS_SUCCESS;
}
