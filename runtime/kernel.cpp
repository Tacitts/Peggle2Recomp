// Kernel (xboxkrnl.exe) and XAM imports: the first, minimal set.
// Everything not implemented here gets a logging stub from tools/gen_imports.py.
#include "runtime.h"

#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <xex.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Allocation logging is chatty while loading (about 1,800 lines per level load): log the first
// 40 calls of each kind, then every 500th.
#define LogSome() ([] { static std::atomic<uint32_t> n{ 0 }; uint32_t k = n++; return k < 40 || k % 500 == 0; }())

// ---- Watchdog ------------------------------------------------------------------------------
struct GuestThreadInfo
{
    uint32_t id;
    const char* const* lastImport;           // address of that thread's g_lastImport
    const std::atomic<uint32_t>* waitingOn;   // address of that thread's t_waitingOn
    HANDLE host;                              // for stack sampling
};
static std::mutex g_threadInfoLock;
static std::vector<GuestThreadInfo> g_threadInfo;

void StartWatchdog(int periodSeconds)
{
    std::thread([periodSeconds] {
        for (int t = periodSeconds;; t += periodSeconds)
        {
            std::this_thread::sleep_for(std::chrono::seconds(periodSeconds));
            std::lock_guard<std::mutex> g(g_threadInfoLock);
            Log("[watch] t=%ds, %zu guest threads", t, g_threadInfo.size());
            for (auto& ti : g_threadInfo)
            {
                uint32_t w = ti.waitingOn->load();
                if (w) Log("[watch]   thread %-6u last import %-34s WAITING on 0x%08X (type %u, state %d)", ti.id, *ti.lastImport, w, Load8(w), int32_t(Load32(w + 4)));
                else
                {
                    // Not blocked in a kernel wait: show where it is running.
                    Log("[watch]   thread %-6u last import %s  -- running, sampled stack:", ti.id, *ti.lastImport);
                    SampleThreadStack(ti.host);
                }
            }
        }
    }).detach();
}

// Called when a guest thread ends: its thread_local state (last import, wait marker) is about
// to be destroyed, so the watchdog must stop reading it.
void UnregisterGuestThread()
{
    std::lock_guard<std::mutex> g(g_threadInfoLock);
    for (auto it = g_threadInfo.begin(); it != g_threadInfo.end(); ++it)
    {
        if (it->lastImport == &g_lastImport)
        {
            ::CloseHandle(it->host);
            g_threadInfo.erase(it);
            break;
        }
    }
}

// ---- Guest thread setup ------------------------------------------------------------------
// KPCR / KTHREAD offsets follow Xenia's X_KPCR / X_KTHREAD layout (verify as needed):
//   KPCR+0x000 tls_ptr, +0x030 pcr_ptr (self), +0x070 stack_base, +0x074 stack_limit,
//   KPCR+0x100 current_thread, +0x10C current_cpu
//   KTHREAD+0x05C stack_base, +0x060 stack_limit, +0x14C thread_id
void InitGuestThread(PPCContext& ctx, uint32_t stackSize, uint32_t threadId, uint32_t kthread)
{
    // 64 KB guard below every stack: decommitted, so an overflow faults right away instead
    // of silently corrupting whatever was allocated before it.
    constexpr uint32_t kGuard = 0x10000;
    uint32_t region = AllocVirtual(stackSize + kGuard, 0x10000);
    VirtualFree(GuestPtr(region), kGuard, MEM_DECOMMIT);
    uint32_t stack = region + kGuard;
    uint32_t stackTop = stack + stackSize;
    uint32_t pcr = AllocRuntime(0x2D8, 0x1000);
    uint32_t tls = AllocRuntime(0x1000, 0x10);
    uint32_t thread = kthread ? kthread : AllocKThread(threadId);
    t_kthread = thread;
    {
        std::lock_guard<std::mutex> g(g_threadInfoLock);
        HANDLE self = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
        g_threadInfo.push_back({ threadId, &g_lastImport, &t_waitingOn, self });
    }

    Store32(pcr + 0x000, tls);
    Store32(pcr + 0x030, pcr);
    Store32(pcr + 0x070, stackTop);
    Store32(pcr + 0x074, stack);
    Store32(pcr + 0x100, thread);
    Store8(pcr + 0x10C, 0);
    Store32(thread + 0x05C, stackTop);
    Store32(thread + 0x060, stack);
    Store32(thread + 0x14C, threadId);

    ctx.r1.u64 = stackTop - 0x100;   // leave a small back-chain area
    ctx.r13.u64 = pcr;
    // PPCFPSCRRegister caches MXCSR in `csr` and writes the whole value back on every FPSCR
    // change. Seed it from the host (exceptions masked, 0x1F80) or the first write unmasks
    // every SSE exception and an inexact result kills the process (0xC000008F).
    ctx.fpscr.loadFromHost();
    Log("[thread %u] stack 0x%08X-0x%08X pcr 0x%08X", threadId, stack, stackTop, pcr);
}

// ---- Import table setup --------------------------------------------------------------------
struct FuncSlotEntry { uint32_t slot, thunk; const char* name; };
struct VarSlotEntry { uint32_t slot; const char* name; };
#define IMPORT_FUNC_SLOT(slot, thunk, name) { slot, thunk, #name },
static const FuncSlotEntry kFuncSlots[] = {
#include "generated/import_func_slots.inc"
};
#undef IMPORT_FUNC_SLOT
#define IMPORT_VAR(slot, name) { slot, #name },
static const VarSlotEntry kVarSlots[] = {
#include "generated/import_vars.inc"
};
#undef IMPORT_VAR

static uint32_t g_xexHeader = 0;      // guest copy of the XEX header
static uint32_t g_moduleEntry = 0;    // LDR_DATA_TABLE_ENTRY for the title
static uint32_t g_timeStampBundle = 0;

static uint64_t HostFileTime()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

static uint32_t AllocVariable(const char* name)
{
    std::string n = name;
    if (n == "XboxHardwareInfo")
    {
        uint32_t a = AllocRuntime(0x10);
        Store32(a + 0, 0x00000000);   // flags
        Store8(a + 4, 6);             // number of hardware threads
        return a;
    }
    if (n == "XboxKrnlVersion")
    {
        uint32_t a = AllocRuntime(0x10);
        Store16(a + 0, 2); Store16(a + 2, 0); Store16(a + 4, 17559); Store16(a + 6, 0);
        return a;
    }
    if (n == "KeTimeStampBundle")
    {
        // { u64 interrupt_time; u64 system_time; u32 tick_count; } kept current by a host thread
        g_timeStampBundle = AllocRuntime(0x18);
        return g_timeStampBundle;
    }
    if (n == "ExLoadedCommandLine")
    {
        uint32_t a = AllocRuntime(0x100);
        const char* cmd = "\"default.xex\"";
        memcpy(GuestPtr(a), cmd, strlen(cmd) + 1);
        return a;
    }
    if (n == "XexExecutableModuleHandle")
    {
        // The variable holds a pointer to the title's LDR_DATA_TABLE_ENTRY.
        uint32_t a = AllocRuntime(4);
        Store32(a, g_moduleEntry);
        return a;
    }
    if (n == "VdGpuClockInMHz")
    {
        uint32_t a = AllocRuntime(4);
        Store32(a, 500);
        return a;
    }
    // ExThreadObjectType, KeDebugMonitorData, KeCertMonitorData, VdGlobalDevice,
    // VdGlobalXamDevice, VdHSIOCalibrationLock: zeroed storage for now.
    return AllocRuntime(0x100);
}

void InitImports(const uint8_t* xexFile, size_t xexSize)
{
    // Guest copy of the XEX header (RtlImageXexHeaderField walks it).
    const auto* hdr = reinterpret_cast<const Xex2Header*>(xexFile);
    uint32_t headerSize = hdr->headerSize;
    g_xexHeader = AllocRuntime(headerSize);
    memcpy(GuestPtr(g_xexHeader), xexFile, headerSize);

    // LDR_DATA_TABLE_ENTRY (Xenia layout): +0x18 dll_base, +0x1C image_base,
    // +0x20 image_size, +0x3C entry_point, +0x58 xex_header_base.
    g_moduleEntry = AllocRuntime(0x100);
    Store32(g_moduleEntry + 0x18, uint32_t(PPC_IMAGE_BASE));
    Store32(g_moduleEntry + 0x1C, uint32_t(PPC_IMAGE_BASE));
    Store32(g_moduleEntry + 0x20, uint32_t(PPC_IMAGE_SIZE));
    Store32(g_moduleEntry + 0x58, g_xexHeader);

    for (auto& f : kFuncSlots)
        Store32(f.slot, f.thunk);
    for (auto& v : kVarSlots)
    {
        uint32_t a = AllocVariable(v.name);
        Store32(v.slot, a);
        Log("[imports] variable %-28s -> 0x%08X", v.name, a);
    }
    Log("[imports] %zu function slots, %zu variables", std::size(kFuncSlots), std::size(kVarSlots));

    if (g_timeStampBundle)
    {
        std::thread([] {
            auto start = std::chrono::steady_clock::now();
            for (;;)
            {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                Store64(g_timeStampBundle + 0x0, uint64_t(ms) * 10000);
                Store64(g_timeStampBundle + 0x8, HostFileTime());
                Store32(g_timeStampBundle + 0x10, uint32_t(ms));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
    }
}

// ---- Debug / misc ----------------------------------------------------------------------------
IMPORT(DbgPrint)
{
    Log("[DbgPrint] %s", LoadCString(ctx.r3.u32).c_str());
    ctx.r3.u64 = 0;
}

IMPORT(DbgBreakPoint) { Log("[DbgBreakPoint]"); }
IMPORT(KeEnableFpuExceptions) {}
IMPORT(KeEnterCriticalRegion) {}
IMPORT(KeLeaveCriticalRegion) {}
IMPORT(KeGetCurrentProcessType) { ctx.r3.u64 = 1; }   // title process
IMPORT(KeSetCurrentProcessType) {}

IMPORT(KeBugCheck)
{
    Log("[KeBugCheck] code 0x%08X", ctx.r3.u32);
    TerminateProcess(GetCurrentProcess(), 4);
}
IMPORT(KeBugCheckEx)
{
    Log("[KeBugCheckEx] code 0x%08X (0x%08X 0x%08X 0x%08X 0x%08X)", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    TerminateProcess(GetCurrentProcess(), 4);
}

// ---- Time ------------------------------------------------------------------------------------
IMPORT(KeQueryPerformanceFrequency) { ctx.r3.u64 = 50000000; }
IMPORT(KeQuerySystemTime)
{
    if (ctx.r3.u32) Store64(ctx.r3.u32, HostFileTime());
}

// ---- Memory ----------------------------------------------------------------------------------
// Reservations made through NtAllocateVirtualMemory: base -> {size, protect}.
struct Reservation { uint32_t size, protect; };
static std::mutex g_reservationLock;
static std::map<uint32_t, Reservation> g_reservations;

// NTSTATUS NtQueryVirtualMemory(u32 address, MEMORY_BASIC_INFORMATION* out, u32 debug)
// MBI (32-bit): BaseAddress, AllocationBase, AllocationProtect, RegionSize, State, Protect, Type
IMPORT(NtQueryVirtualMemory)
{
    uint32_t addr = ctx.r3.u32, out = ctx.r4.u32;
    uint32_t page = addr & ~0xFFFu;
    uint32_t allocBase = 0, allocSize = 0, protect = 4;
    {
        std::lock_guard<std::mutex> g(g_reservationLock);
        auto it = g_reservations.upper_bound(addr);
        if (it != g_reservations.begin())
        {
            --it;
            if (addr < it->first + it->second.size) { allocBase = it->first; allocSize = it->second.size; protect = it->second.protect; }
        }
    }
    if (!allocBase)
    {
        Store32(out + 0x00, page);
        Store32(out + 0x04, 0);
        Store32(out + 0x08, 0);
        Store32(out + 0x0C, 0x1000);
        Store32(out + 0x10, 0x10000);   // MEM_FREE
        Store32(out + 0x14, 1);         // PAGE_NOACCESS
        Store32(out + 0x18, 0);
        ctx.r3.u64 = STATUS_SUCCESS;
        return;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(GuestPtr(page), &mbi, sizeof(mbi));
    uint32_t regionEnd = std::min<uint64_t>(uint64_t(GuestAddr(mbi.BaseAddress)) + mbi.RegionSize, uint64_t(allocBase) + allocSize);
    bool committed = mbi.State == MEM_COMMIT;
    Store32(out + 0x00, page);
    Store32(out + 0x04, allocBase);
    Store32(out + 0x08, protect);
    Store32(out + 0x0C, regionEnd - page);
    Store32(out + 0x10, committed ? 0x1000 : 0x2000);   // MEM_COMMIT / MEM_RESERVE
    Store32(out + 0x14, committed ? protect : 0);
    Store32(out + 0x18, 0x20000);                         // MEM_PRIVATE
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtAllocateVirtualMemory(u32* base, u32* size, u32 type, u32 protect, u32 debug)
IMPORT(NtAllocateVirtualMemory)
{
    uint32_t basePtr = ctx.r3.u32, sizePtr = ctx.r4.u32, type = ctx.r5.u32;
    uint32_t want = Load32(basePtr), size = Load32(sizePtr);
    constexpr uint32_t MEM_RESERVE_ = 0x2000;
    // Page size: MEM_LARGE_PAGES (0x20000000) = 64 KB pages, otherwise 4 KB.
    uint32_t page = (type & 0x20000000) ? 0x10000 : 0x1000;
    if (want != 0 && !(type & MEM_RESERVE_))
    {
        // Committing inside an existing reservation; write back page-rounded base/size.
        uint32_t start = want & ~(page - 1), end = (want + size + page - 1) & ~(page - 1);
        CommitGuest(start, end - start);
        Store32(basePtr, start);
        Store32(sizePtr, end - start);
        if (LogSome()) Log("[mem] NtAllocateVirtualMemory commit 0x%08X size 0x%X (type 0x%X)", start, end - start, type);
        ctx.r3.u64 = STATUS_SUCCESS;
        return;
    }
    uint32_t aligned = (size + 0xFFFF) & ~0xFFFFu;
    uint32_t a = AllocVirtual(aligned, 0x10000);
    if (LogSome()) Log("[mem] NtAllocateVirtualMemory(want 0x%08X, size 0x%X, type 0x%X, protect 0x%X) -> 0x%08X", want, size, type, ctx.r6.u32, a);
    if (!a) { ctx.r3.u64 = STATUS_NO_MEMORY; return; }
    // Reserve without commit leaves the pages uncommitted, as on hardware (the title's heap
    // commits pieces explicitly and inspects commit state with NtQueryVirtualMemory).
    if (!(type & 0x1000)) VirtualFree(GuestPtr(a), aligned, MEM_DECOMMIT);
    {
        std::lock_guard<std::mutex> g(g_reservationLock);
        g_reservations[a] = { aligned, ctx.r6.u32 };
    }
    Store32(basePtr, a);
    Store32(sizePtr, aligned);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtFreeVirtualMemory(u32* base, u32* size, u32 type, u32 debug)
// MEM_DECOMMIT (0x4000): pages are decommitted, so a later commit sees zeroed memory as on
// hardware. MEM_RELEASE (0x8000): decommitted too; the address range is not reused (bump).
IMPORT(NtFreeVirtualMemory)
{
    uint32_t addr = Load32(ctx.r3.u32), size = Load32(ctx.r4.u32), type = ctx.r5.u32;
    uint32_t start = addr & ~0xFFFu, end = (addr + size + 0xFFF) & ~0xFFFu;
    if ((type & 0xC000) && end > start)
        VirtualFree(GuestPtr(start), end - start, MEM_DECOMMIT);
    if (LogSome()) Log("[mem] NtFreeVirtualMemory(0x%08X, size 0x%X, type 0x%X)", start, end - start, type);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// u32 MmAllocatePhysicalMemoryEx(u32 flags, u32 size, u32 protect, u32 min, u32 max, u32 align)
IMPORT(MmAllocatePhysicalMemoryEx)
{
    uint32_t size = ctx.r4.u32, align = ctx.r8.u32;
    if (align < 0x1000) align = 0x1000;
    uint32_t a = AllocPhysical(size, align);
    if (LogSome()) Log("[mem] MmAllocatePhysicalMemoryEx(flags 0x%X, size 0x%X, protect 0x%X, align 0x%X) -> 0x%08X", ctx.r3.u32, size, ctx.r5.u32, align, a);
    ctx.r3.u64 = a;
}
IMPORT(MmFreePhysicalMemory) {}
IMPORT(MmGetPhysicalAddress) { ctx.r3.u64 = ctx.r3.u32 & 0x1FFFFFFF; }
IMPORT(MmQueryAddressProtect) { ctx.r3.u64 = 0x04; } // PAGE_READWRITE

// u32 XamAlloc(u32 flags, u32 size, u32* out)
IMPORT(XamAlloc)
{
    uint32_t a = AllocVirtual(ctx.r4.u32, 16);
    Log("[mem] XamAlloc(flags 0x%X, size 0x%X) -> 0x%08X", ctx.r3.u32, ctx.r4.u32, a);
    if (ctx.r5.u32) Store32(ctx.r5.u32, a);
    ctx.r3.u64 = a ? 0 : 0x8007000E; // E_OUTOFMEMORY
}
IMPORT(XamFree) { ctx.r3.u64 = 0; }

IMPORT(RtlFillMemoryUlong)
{
    uint32_t dst = ctx.r3.u32, len = ctx.r4.u32, pattern = ctx.r5.u32;
    for (uint32_t i = 0; i + 4 <= len; i += 4) Store32(dst + i, pattern);
}
IMPORT(RtlCompareMemoryUlong)
{
    uint32_t src = ctx.r3.u32, len = ctx.r4.u32, pattern = ctx.r5.u32, n = 0;
    while (n + 4 <= len && Load32(src + n) == pattern) n += 4;
    ctx.r3.u64 = n;
}

// ---- Critical sections ---------------------------------------------------------------------
// Guest RTL_CRITICAL_SECTION is backed by a host recursive mutex keyed by guest address.
// Guest fields are kept faithful (titles may inspect them directly):
//   +0x10 LockCount (-1 = free, n = owned with n waiters), +0x14 RecursionCount,
//   +0x18 OwningThread (guest KTHREAD).
static std::mutex g_csMapLock;
static std::unordered_map<uint32_t, std::recursive_mutex*> g_csMap;

static std::recursive_mutex& HostCs(uint32_t cs)
{
    std::lock_guard<std::mutex> g(g_csMapLock);
    auto& m = g_csMap[cs];
    if (!m) m = new std::recursive_mutex();
    return *m;
}

static void InitCs(uint32_t cs)
{
    Store32(cs + 0x10, 0xFFFFFFFF);
    Store32(cs + 0x14, 0);
    Store32(cs + 0x18, 0);
    HostCs(cs);
}

IMPORT(RtlInitializeCriticalSection) { InitCs(ctx.r3.u32); }
IMPORT(RtlInitializeCriticalSectionAndSpinCount) { InitCs(ctx.r3.u32); ctx.r3.u64 = STATUS_SUCCESS; }
static void CsAcquired(uint32_t cs)
{
    Store32(cs + 0x14, Load32(cs + 0x14) + 1);
    Store32(cs + 0x18, t_kthread);
    Store32(cs + 0x10, 0);
}

IMPORT(RtlEnterCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    HostCs(cs).lock();
    CsAcquired(cs);
}
IMPORT(RtlLeaveCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t rec = Load32(cs + 0x14) - 1;
    Store32(cs + 0x14, rec);
    if (rec == 0)
    {
        Store32(cs + 0x18, 0);
        Store32(cs + 0x10, 0xFFFFFFFF);
    }
    HostCs(cs).unlock();
}
IMPORT(RtlTryEnterCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    bool ok = HostCs(cs).try_lock();
    if (ok) CsAcquired(cs);
    ctx.r3.u64 = ok ? 1 : 0;
}

// ---- TLS ---------------------------------------------------------------------------------------
static std::atomic<uint32_t> g_nextTls{ 0 };
static thread_local uint32_t t_tls[64];

IMPORT(KeTlsAlloc)
{
    uint32_t i = g_nextTls++;
    ctx.r3.u64 = i < 64 ? i : 0xFFFFFFFF;
}
IMPORT(KeTlsFree) { ctx.r3.u64 = 1; }
IMPORT(KeTlsGetValue) { ctx.r3.u64 = ctx.r3.u32 < 64 ? t_tls[ctx.r3.u32] : 0; }
IMPORT(KeTlsSetValue)
{
    if (ctx.r3.u32 < 64) { t_tls[ctx.r3.u32] = ctx.r4.u32; ctx.r3.u64 = 1; }
    else ctx.r3.u64 = 0;
}

// ---- Strings -----------------------------------------------------------------------------------
// ANSI_STRING / UNICODE_STRING: { u16 Length; u16 MaximumLength; u32 Buffer; }
IMPORT(RtlInitAnsiString)
{
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    uint16_t len = src ? uint16_t(LoadCString(src, 0xFFFE).size()) : 0;
    Store16(dst, len);
    Store16(dst + 2, src ? len + 1 : 0);
    Store32(dst + 4, src);
}
IMPORT(RtlInitUnicodeString)
{
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    uint16_t len = 0;
    if (src) while (Load16(src + len) != 0 && len < 0xFFFC) len += 2;
    Store16(dst, len);
    Store16(dst + 2, src ? len + 2 : 0);
    Store32(dst + 4, src);
}
IMPORT(RtlFreeAnsiString) {}

// ---- XEX header / modules ----------------------------------------------------------------
// u32 RtlImageXexHeaderField(u32 xexHeaderBase, u32 key): value for keys with size 0,
// otherwise a guest pointer to the field's data; 0 if absent.
IMPORT(RtlImageXexHeaderField)
{
    uint32_t key = ctx.r4.u32;
    const uint8_t* host = GuestPtr(g_xexHeader);
    const auto* hdr = reinterpret_cast<const Xex2Header*>(host);
    uint32_t result = 0;
    for (uint32_t i = 0; i < hdr->headerCount; i++)
    {
        const auto& opt = reinterpret_cast<const Xex2OptHeader*>(hdr + 1)[i];
        if (opt.key != key) continue;
        if ((key & 0xFF) == 0) result = opt.value;
        else result = g_xexHeader + opt.offset;
        break;
    }
    ctx.r3.u64 = result;
}

// Export ordinal -> name tables from XenonUtils (xex.cpp).
extern std::unordered_map<size_t, const char*> XamExports;
extern std::unordered_map<size_t, const char*> XboxKernelExports;

// Module handles: one LDR_DATA_TABLE_ENTRY per system module the title asks for.
// The title module itself is g_moduleEntry.
static std::mutex g_moduleLock;
static std::unordered_map<std::string, uint32_t> g_modules;   // lower-case name -> entry
static std::unordered_map<uint32_t, std::string> g_moduleNames;

static std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

static uint32_t ModuleHandle(const std::string& rawName)
{
    std::string name = Lower(rawName);
    if (name != "xam.xex" && name != "xboxkrnl.exe") return 0;
    std::lock_guard<std::mutex> g(g_moduleLock);
    auto& h = g_modules[name];
    if (!h)
    {
        h = AllocRuntime(0x100);
        g_moduleNames[h] = name;
    }
    return h;
}

// NTSTATUS XexLoadImage(const char* name, u32 flags, u32 minVersion, u32* handle)
IMPORT(XexLoadImage)
{
    std::string name = LoadCString(ctx.r3.u32);
    uint32_t h = ModuleHandle(name);
    Log("[XexLoadImage] %s -> 0x%08X", name.c_str(), h);
    if (ctx.r6.u32) Store32(ctx.r6.u32, h);
    ctx.r3.u64 = h ? STATUS_SUCCESS : 0xC0000135; // STATUS_DLL_NOT_FOUND
}

// NTSTATUS XexGetModuleHandle(const char* name, u32* handle); name == NULL means the title.
IMPORT(XexGetModuleHandle)
{
    uint32_t h = ctx.r3.u32 ? ModuleHandle(LoadCString(ctx.r3.u32)) : g_moduleEntry;
    if (ctx.r4.u32) Store32(ctx.r4.u32, h);
    ctx.r3.u64 = h ? STATUS_SUCCESS : 0xC0000135;
}

IMPORT(XexUnloadImage) { ctx.r3.u64 = STATUS_SUCCESS; }

// NTSTATUS XexGetProcedureAddress(u32 handle, u32 ordinal, u32* out)
// Resolves to the import thunk the title already links against; otherwise reports
// STATUS_PROCEDURE_NOT_FOUND, which titles treat as "not on this system version".
IMPORT(XexGetProcedureAddress)
{
    uint32_t handle = ctx.r3.u32, ordinal = ctx.r4.u32, out = ctx.r5.u32;
    std::string module;
    {
        std::lock_guard<std::mutex> g(g_moduleLock);
        auto it = g_moduleNames.find(handle);
        if (it != g_moduleNames.end()) module = it->second;
    }
    const auto& table = module == "xboxkrnl.exe" ? XboxKernelExports : XamExports;
    const char* exportName = nullptr;
    if (!module.empty())
        if (auto it = table.find(ordinal); it != table.end()) exportName = it->second;

    uint32_t thunk = 0;
    if (exportName)
    {
        std::string want = std::string("__imp__") + exportName;
        for (auto& f : kFuncSlots)
            if (want == std::string("__imp__") + f.name) { thunk = f.thunk; break; }
    }
    Log("[XexGetProcedureAddress] %s ordinal 0x%X (%s) -> %s", module.empty() ? "?" : module.c_str(), ordinal,
        exportName ? exportName : "unknown", thunk ? "linked thunk" : "not found");
    if (out) Store32(out, thunk);
    ctx.r3.u64 = thunk ? STATUS_SUCCESS : 0xC000007A; // STATUS_PROCEDURE_NOT_FOUND
}

IMPORT(XexCheckExecutablePrivilege) { ctx.r3.u64 = 0; }

IMPORT(XGetGameRegion) { ctx.r3.u64 = 0xFFFF; }
IMPORT(XGetAVPack) { ctx.r3.u64 = 6; }        // HDMI
IMPORT(XGetVideoMode)
{
    // XVIDEO_MODE: width, height, interlaced, widescreen, hi-def, refresh, ...
    uint32_t p = ctx.r3.u32;
    memset(GuestPtr(p), 0, 0x30);
    Store32(p + 0x00, 1280);
    Store32(p + 0x04, 720);
    Store32(p + 0x08, 0);
    Store32(p + 0x0C, 1);
    Store32(p + 0x10, 1);
    Store32(p + 0x14, 0x42700000); // 60.0f
}
IMPORT(XamGetSystemVersion) { ctx.r3.u64 = 0x20000000 | (17559 << 8); }
IMPORT(XamGetExecutionId) { if (ctx.r3.u32) Store32(ctx.r3.u32, 0); ctx.r3.u64 = 0x80004005; }
IMPORT(XamLoaderTerminateTitle)
{
    Log("[XamLoaderTerminateTitle] title asked to exit");
    TerminateProcess(GetCurrentProcess(), 0);
}
