// Guest address space and simple allocators.
#include "runtime.h"

#include <mutex>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

uint8_t* g_base = nullptr;

bool CommitGuest(uint32_t addr, uint32_t size)
{
    if (size == 0) return true;
    uint64_t start = addr & ~0xFFFull;
    uint64_t end = (uint64_t(addr) + size + 0xFFF) & ~0xFFFull;
    return VirtualAlloc(g_base + start, size_t(end - start), MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

namespace
{
    struct BumpRegion
    {
        const char* name;
        uint32_t next, limit;
        bool commit = true;   // false for the physical region: it is a mapped view, already committed
        std::mutex lock;

        uint32_t Alloc(uint32_t size, uint32_t align)
        {
            if (align < 16) align = 16;
            std::lock_guard<std::mutex> g(lock);
            uint64_t a = (uint64_t(next) + align - 1) & ~uint64_t(align - 1);
            uint64_t end = a + ((uint64_t(size) + 0xFFF) & ~0xFFFull);
            if (end > limit)
            {
                Log("[mem] %s region exhausted (size 0x%X)", name, size);
                return 0;
            }
            next = uint32_t(end);
            if (commit && !CommitGuest(uint32_t(a), uint32_t(end - a)))
            {
                Log("[mem] commit failed at 0x%08X size 0x%X", uint32_t(a), size);
                return 0;
            }
            memset(g_base + a, 0, size_t(end - a));
            return uint32_t(a);
        }
    };

    // 0x7FC80000-0x7FCFFFFF is the Xenos GPU register window (MMIO); keep allocators below it.
    BumpRegion g_virtual{ "virtual", 0x40000000, 0x7C000000 };
    // Physical memory: allocated through the 0xA0000000 view; the same bytes are visible at
    // 0xC0000000 and 0xE0000000 (see ReserveGuestSpace).
    BumpRegion g_physical{ "physical", 0xA0000000, 0xC0000000, false };
    // Runtime-owned kernel structures, in their own area so they never collide with title allocations.
    BumpRegion g_runtime{ "runtime", 0x7C000000, 0x7F000000 };
}

uint32_t AllocVirtual(uint32_t size, uint32_t align) { return g_virtual.Alloc(size, align); }
uint32_t AllocPhysical(uint32_t size, uint32_t align) { return g_physical.Alloc(size, align); }
uint32_t AllocRuntime(uint32_t size, uint32_t align) { return g_runtime.Alloc(size, align); }

std::string LoadCString(uint32_t addr, size_t max)
{
    std::string s;
    if (!addr) return s;
    for (size_t i = 0; i < max; i++)
    {
        char c = (char)Load8(addr + uint32_t(i));
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

// Guest address space layout:
//   0x00000000-0x9FFFFFFF  ordinary reservation (image, function table, virtual heap, MMIO)
//   0xA0000000-0xBFFFFFFF  \
//   0xC0000000-0xDFFFFFFF   > three views of ONE 512 MB section: the console's physical RAM,
//   0xE0000000-0xFFFFFFFF  /  aliased with 64 KB, 16 MB and 4 KB pages (phys = addr & 0x1FFFFFFF)
// Mapping views inside one reservation needs placeholders (VirtualAlloc2 / MapViewOfFile3,
// Windows 10 1803+), resolved at runtime from kernelbase.dll.
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);

bool ReserveGuestSpace()
{
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    auto pVirtualAlloc2 = kb ? (VirtualAlloc2Fn)GetProcAddress(kb, "VirtualAlloc2") : nullptr;
    auto pMapViewOfFile3 = kb ? (MapViewOfFile3Fn)GetProcAddress(kb, "MapViewOfFile3") : nullptr;
    if (!pVirtualAlloc2 || !pMapViewOfFile3)
    {
        Log("[mem] VirtualAlloc2/MapViewOfFile3 unavailable (Windows 10 1803+ required)");
        return false;
    }
    constexpr uint64_t kPhysStart = 0xA0000000ull, kPhysSize = 0x20000000ull;

    g_base = (uint8_t*)pVirtualAlloc2(nullptr, nullptr, kGuestSpace, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    if (!g_base) { Log("[mem] placeholder reservation failed (%lu)", GetLastError()); return false; }

    // Split the placeholder: [0, 0xA0000000) and three 512 MB alias slots. Each split carves
    // the range out of a larger placeholder; the last slot (0xE0000000) is what remains after
    // splitting the previous one, and splitting a placeholder into itself fails (487).
    for (uint64_t at = kPhysStart; at + kPhysSize < kGuestSpace; at += kPhysSize)
        if (!VirtualFree(g_base + at, kPhysSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        { Log("[mem] placeholder split at 0x%llX failed (%lu)", at, GetLastError()); return false; }

    // Low part becomes an ordinary reservation (commit/decommit on demand).
    if (!pVirtualAlloc2(nullptr, g_base, kPhysStart, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0))
    { Log("[mem] low reservation failed (%lu)", GetLastError()); return false; }

    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                        DWORD(kPhysSize >> 32), DWORD(kPhysSize), nullptr);
    if (!section) { Log("[mem] physical section failed (%lu)", GetLastError()); return false; }
    for (uint64_t at = kPhysStart; at < kGuestSpace; at += kPhysSize)
    {
        if (!pMapViewOfFile3(section, GetCurrentProcess(), g_base + at, 0, kPhysSize, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
        { Log("[mem] physical view at 0x%llX failed (%lu)", at, GetLastError()); return false; }
    }
    Log("[mem] guest space at %p: physical RAM (512 MB) aliased at 0xA0000000, 0xC0000000, 0xE0000000", g_base);
    return true;
}
