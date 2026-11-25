// Peggle 2 recompilation runtime: entry point.
//
// Usage: Peggle2.exe [path\to\game]   (default: .\game next to the exe, then ..\game)
#include "runtime.h"

#include <algorithm>
#include <cstdarg>
#include <filesystem>
#include <mutex>
#include <vector>

#include <file.h>
#include <image.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <unordered_map>

PPC_EXTERN_FUNC(_xstart);

bool ReserveGuestSpace();
thread_local const char* g_lastImport = "(none)";

static FILE* g_logFile = nullptr;
static std::mutex g_logLock;

void Log(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> g(g_logLock);
    fprintf(stdout, "%s\n", buf);
    fflush(stdout);
    if (g_logFile) { fprintf(g_logFile, "%s\n", buf); fflush(g_logFile); }
}

void CallGuest(PPCContext& ctx, uint32_t guestFunc)
{
    PPCFunc* fn = FuncSlot(guestFunc);
    if (!fn)
    {
        Log("[call] no recompiled function at 0x%08X", guestFunc);
        return;
    }
    fn(ctx, g_base);
}

// ---- Crash reporting: map a host RIP back to the guest function it belongs to ---------
static std::vector<std::pair<uintptr_t, uint32_t>> g_hostToGuest;

static void BuildHostToGuest()
{
    for (auto* m = PPCFuncMappings; m->host; m++)
        g_hostToGuest.push_back({ (uintptr_t)m->host, uint32_t(m->guest) });
    std::sort(g_hostToGuest.begin(), g_hostToGuest.end());
}

static uint32_t GuestFuncForHost(uintptr_t rip)
{
    auto it = std::upper_bound(g_hostToGuest.begin(), g_hostToGuest.end(), std::make_pair(rip, UINT32_MAX));
    if (it == g_hostToGuest.begin()) return 0;
    return std::prev(it)->second;
}

// ---- Host address -> generated source line -> guest instruction (needs the PDB) ------------
// XenonRecomp writes "// XXXXXXXX mnemonic operands" above every translated instruction, so
// the nearest such comment at or above the line is the guest instruction being executed.
static std::mutex g_symLock;
static bool g_symReady = false;
static std::unordered_map<std::string, std::vector<std::string>> g_sourceCache;

static std::string GuestInsnForHost(uintptr_t rip)
{
    std::lock_guard<std::mutex> g(g_symLock);
    if (!g_symReady)
    {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        g_symReady = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    }
    if (!g_symReady) return {};
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD disp = 0;
    if (!SymGetLineFromAddr64(GetCurrentProcess(), rip, &disp, &line)) return {};
    std::string file = line.FileName;
    if (file.find("ppc_recomp") == std::string::npos) return {};
    auto& lines = g_sourceCache[file];
    if (lines.empty())
    {
        FILE* f = fopen(file.c_str(), "rb");
        if (!f) return {};
        std::string cur;
        for (int ch; (ch = fgetc(f)) != EOF;)
        {
            if (ch == '\n') { lines.push_back(cur); cur.clear(); }
            else if (ch != '\r') cur.push_back(char(ch));
        }
        lines.push_back(cur);
        fclose(f);
    }
    for (int i = int(line.LineNumber) - 1; i >= 0 && i > int(line.LineNumber) - 200; i--)
    {
        const std::string& s = lines[size_t(i)];
        size_t p = s.find("// ");
        if (p != std::string::npos && s.size() > p + 11 && isxdigit((unsigned char)s[p + 3]) && s[p + 11] == ' ')
            return s.substr(p + 3);
    }
    return {};
}

// Log the guest call chain for a host CONTEXT (shared by the crash handler and hooks).
static void LogGuestChain(CONTEXT c)
{
    uint32_t prevGuest = 0;
    for (int depth = 0; depth < 32 && c.Rip; depth++)
    {
        uint32_t guest = GuestFuncForHost(c.Rip);
        HMODULE mod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)c.Rip, &mod);
        bool inExe = mod == GetModuleHandleA(nullptr);
        // Depth 0 is the faulting instruction itself; deeper frames are return addresses, so
        // look one byte back to land inside the call.
        std::string insn = inExe ? GuestInsnForHost(depth == 0 ? c.Rip : c.Rip - 1) : std::string();
        if (inExe && !insn.empty()) Log("     #%-2d guest %s  (in function 0x%08X)", depth, insn.c_str(), guest);
        else if (inExe && guest && guest != prevGuest) Log("     #%-2d guest 0x%08X  (host %p)", depth, guest, (void*)c.Rip);
        else if (!inExe) Log("     #%-2d host  %p (outside the exe)", depth, (void*)c.Rip);
        prevGuest = guest;
        DWORD64 imageBase;
        auto* fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (!fe) break;
        void* handlerData;
        DWORD64 establisher;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &handlerData, &establisher, nullptr);
    }
}

void LogGuestBacktrace()
{
    CONTEXT c;
    RtlCaptureContext(&c);
    LogGuestChain(c);
}

// Sample another thread's guest call chain (watchdog). While the thread is suspended only
// lock-free work is done (capture context, unwind to raw addresses); symbolisation happens
// after it resumes, so a lock it holds can't deadlock us.
void SampleThreadStack(void* hThread)
{
    uintptr_t rips[24];
    int n = 0;
    if (SuspendThread((HANDLE)hThread) == (DWORD)-1) return;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext((HANDLE)hThread, &c))
    {
        for (; n < 24 && c.Rip; n++)
        {
            rips[n] = c.Rip;
            DWORD64 imageBase;
            auto* fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (!fe) { n++; break; }
            void* handlerData;
            DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &handlerData, &establisher, nullptr);
        }
    }
    ResumeThread((HANDLE)hThread);
    uint32_t prev = 0;
    for (int i = 0; i < n && i < 12; i++)
    {
        std::string insn = GuestInsnForHost(i == 0 ? rips[i] : rips[i] - 1);
        uint32_t guest = GuestFuncForHost(rips[i]);
        if (!insn.empty()) Log("[watch]       #%-2d guest %s  (in function 0x%08X)", i, insn.c_str(), guest);
        else if (guest && guest != prev) Log("[watch]       #%-2d host %p (~guest 0x%08X)", i, (void*)rips[i], guest);
        prev = guest;
    }
}

// ---- Hardware write watchpoint (debugging aid) ---------------------------------------------
// WatchGuestWrite(addr) arms DR0 on the calling thread for 4-byte writes to guest `addr`;
// every write is logged with the new value and the guest call chain.
static std::atomic<uint32_t> g_watchAddr{ 0 };

static void SetDr0(HANDLE thread, uintptr_t addr, bool enable)
{
    SuspendThread(thread);
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(thread, &c);
    c.Dr0 = addr;
    // L0 enable, RW0 = 01 (write), LEN0 = 11 (4 bytes)
    c.Dr7 = enable ? ((c.Dr7 & ~0xF0003ull) | 0x1 | (0x1 << 16) | (0x3 << 18)) : (c.Dr7 & ~0x3ull);
    SetThreadContext(thread, &c);
    ResumeThread(thread);
}

static void ArmWatch(uintptr_t hostAddr, bool enable)
{
    HANDLE self;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::thread([&] { SetDr0(self, hostAddr, enable); }).join();   // can't change our own context while running
    CloseHandle(self);
}

void WatchGuestWrite(uint32_t addr)
{
    g_watchAddr = addr;
    ArmWatch((uintptr_t)(g_base + addr), true);
    Log("[watch] armed write watchpoint on guest 0x%08X", addr);
}

void UnwatchGuestWrite()
{
    ArmWatch(0, false);
    g_watchAddr = 0;
}

static LONG CALLBACK CrashHandler(PEXCEPTION_POINTERS ep)
{
    auto* rec = ep->ExceptionRecord;
    if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && (ep->ContextRecord->Dr6 & 1) && g_watchAddr)
    {
        uint32_t a = g_watchAddr;
        Log("[watch] write to guest 0x%08X, new value 0x%08X; guest call chain:", a, Load32(a));
        LogGuestChain(*ep->ContextRecord);
        ep->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Ignore debugger/C++ exceptions; only report real faults.
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && rec->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION &&
        rec->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO && rec->ExceptionCode != EXCEPTION_BREAKPOINT &&
        rec->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t rip = (uintptr_t)rec->ExceptionAddress;
    Log("==== CRASH: code 0x%08lX at host %p (inside guest function ~0x%08X)", rec->ExceptionCode, (void*)rip, GuestFuncForHost(rip));
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
    {
        uintptr_t addr = rec->ExceptionInformation[1];
        const char* kind = rec->ExceptionInformation[0] == 0 ? "read" : rec->ExceptionInformation[0] == 1 ? "write" : "execute";
        if (addr >= (uintptr_t)g_base && addr < (uintptr_t)g_base + kGuestSpace)
            Log("     %s of guest address 0x%08X (uncommitted page)", kind, uint32_t(addr - (uintptr_t)g_base));
        else
            Log("     %s of host address %p (outside guest memory)", kind, (void*)addr);
    }
    Log("     guest thread: KTHREAD 0x%08X id %u; last import called on this thread: %s", t_kthread,
        t_kthread ? Load32(t_kthread + 0x14C) : 0, g_lastImport);

    // Guest call chain: unwind the host stack and map each frame to its guest function.
    LogGuestChain(*ep->ContextRecord);
    if (g_logFile) fflush(g_logFile);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Installed in every function-table slot that has no recompiled function.
static void MissingFunctionTrap(PPCContext& ctx, uint8_t* base)
{
    Log("==== indirect call to guest 0x%08X, which is not a recompiled function start (lr 0x%08llX)",
        ctx.ctr.u32, (unsigned long long)ctx.lr);
    Log("     add it as a function in config/peggle2.base.toml if it is real code");
    __debugbreak(); // reported by CrashHandler with the guest call chain
}

// ---- Startup ------------------------------------------------------------------------------
static std::filesystem::path FindGameDir(int argc, char** argv)
{
    std::vector<std::filesystem::path> candidates;
    if (argc > 1) candidates.emplace_back(argv[1]);
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    auto dir = std::filesystem::path(exe).parent_path();
    candidates.push_back(dir / "game");
    for (auto d = dir; d.has_parent_path() && d != d.parent_path(); d = d.parent_path())
        candidates.push_back(d / "game");
    for (auto& c : candidates)
        if (std::filesystem::exists(c / "Default.xex")) return c;
    return {};
}

std::filesystem::path g_gameDir;

int main(int argc, char** argv)
{
    g_logFile = fopen("peggle2_runtime.log", "w");
    AddVectoredExceptionHandler(1, CrashHandler);

    g_gameDir = FindGameDir(argc, argv);
    if (g_gameDir.empty())
    {
        Log("Could not find game\\Default.xex. Pass the extracted game folder as the first argument.");
        return 1;
    }
    Log("[boot] game folder: %s", g_gameDir.string().c_str());

    if (!ReserveGuestSpace())
    {
        Log("[boot] failed to reserve 4 GB of address space");
        return 1;
    }

    // Load and decrypt/decompress the XEX (XenonUtils); the import thunks come back
    // patched to `nop; nop; nop; blr`, matching what the recompiler saw.
    auto xex = LoadFile((g_gameDir / "Default.xex").string().c_str());
    if (xex.empty()) { Log("[boot] cannot read Default.xex"); return 1; }
    Image image = Image::ParseImage(xex.data(), xex.size());
    if (image.base != PPC_IMAGE_BASE || image.size != PPC_IMAGE_SIZE)
    {
        Log("[boot] XEX image (base 0x%zX size 0x%X) does not match the recompiled code (base 0x%llX size 0x%llX)",
            image.base, image.size, PPC_IMAGE_BASE, PPC_IMAGE_SIZE);
        return 1;
    }
    CommitGuest(uint32_t(image.base), image.size);
    memcpy(g_base + image.base, image.data.get(), image.size);
    Log("[boot] image mapped at guest 0x%08zX (0x%X bytes), entry 0x%08zX", image.base, image.size, image.entry_point);

    // Indirect-call table: 8 bytes per guest instruction, right after the image.
    uint64_t tableStart = PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
    uint64_t tableSize = PPC_CODE_SIZE * 2;
    if (!VirtualAlloc(g_base + tableStart, size_t(tableSize), MEM_COMMIT, PAGE_READWRITE))
    {
        Log("[boot] cannot commit function table");
        return 1;
    }
    size_t mapped = 0;
    for (auto* m = PPCFuncMappings; m->host; m++, mapped++)
        FuncSlot(uint32_t(m->guest)) = m->host;
    // Every other code address gets a trap, so an indirect call to something that was not
    // recompiled reports its target (indirect calls load the target into CTR first).
    size_t traps = 0;
    for (uint64_t a = PPC_CODE_BASE; a < PPC_CODE_BASE + PPC_CODE_SIZE; a += 4)
        if (!FuncSlot(uint32_t(a))) { FuncSlot(uint32_t(a)) = MissingFunctionTrap; traps++; }
    Log("[boot] %zu recompiled functions registered (%zu other code addresses trap)", mapped, traps);
    BuildHostToGuest();

    // Hardware register windows (GPU at 0x7FC80000, XMA at 0x7FEA0000, ...). D3D and the
    // audio driver touch these directly. For now they are plain zeroed memory: no hardware
    // is emulated yet, so reads return whatever was last written.
    CommitGuest(kMmioBase, kMmioSize);
    Log("[boot] MMIO window 0x%08X-0x%08X backed by plain memory (no GPU/XMA emulation yet)", kMmioBase, kMmioBase + kMmioSize - 1);

    InitImports(xex.data(), xex.size());

    StartWatchdog(10);

    PPCContext ctx{};
    InitGuestThread(ctx, 1024 * 1024, 1);
    Log("[boot] calling _xstart (0x%08zX)", image.entry_point);
    _xstart(ctx, g_base);
    Log("[boot] _xstart returned (r3 = 0x%08X)", ctx.r3.u32);
    return 0;
}
