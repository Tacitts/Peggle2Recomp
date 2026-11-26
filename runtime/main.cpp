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
#include <tlhelp32.h>
#include <timeapi.h>
#include <thread>
#include <xmmintrin.h>
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
    // Buffered: flushing every line (twice) on the calling thread blocked game threads while
    // loading. LogFlushThread flushes every 100 ms; crashes flush at once (CrashHandler).
    std::lock_guard<std::mutex> g(g_logLock);
    fprintf(stdout, "%s\n", buf);
    if (g_logFile) fprintf(g_logFile, "%s\n", buf);
}

void LogFlush()
{
    std::lock_guard<std::mutex> g(g_logLock);
    fflush(stdout);
    if (g_logFile) fflush(g_logFile);
}

static void LogFlushThread()
{
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        LogFlush();
    }
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
    // Ignore debugger/C++ exceptions; only report real faults (floating-point traps included).
    const DWORD code = rec->ExceptionCode;
    const bool fp = code == EXCEPTION_FLT_INEXACT_RESULT || code == EXCEPTION_FLT_INVALID_OPERATION ||
                    code == EXCEPTION_FLT_DIVIDE_BY_ZERO || code == EXCEPTION_FLT_OVERFLOW ||
                    code == EXCEPTION_FLT_UNDERFLOW || code == EXCEPTION_FLT_DENORMAL_OPERAND ||
                    code == STATUS_FLOAT_MULTIPLE_TRAPS || code == STATUS_FLOAT_MULTIPLE_FAULTS;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_BREAKPOINT &&
        code != EXCEPTION_STACK_OVERFLOW && !fp)
    {
        LogFlush();
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // The report formats numbers: mask FP exceptions first so it can't trap again itself.
    _mm_setcsr(_mm_getcsr() | 0x1F80);
    if (fp) Log("==== floating-point trap: MXCSR at the fault 0x%08X", ep->ContextRecord->MxCsr);

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
    LogFlush();
    return EXCEPTION_CONTINUE_SEARCH;
}

// Installed in every function-table slot that has no recompiled function.
static void MissingFunctionTrap(PPCContext& ctx, uint8_t* base)
{
#ifdef PPC_CONFIG_CTR_AS_LOCAL
    // CTR lives in the calling function's locals: the target isn't recorded in the context.
    Log("==== indirect call to an address that is not a recompiled function start (lr 0x%08llX)",
        (unsigned long long)ctx.lr);
#else
    Log("==== indirect call to guest 0x%08X, which is not a recompiled function start (lr 0x%08llX)",
        ctx.ctr.u32, (unsigned long long)ctx.lr);
#endif
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

// Keep the previous run's log (crash reports!) in logs/<timestamp>.log; the 10 newest are kept.
static void RotateLogs()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists("peggle2_runtime.log", ec)) return;
    fs::create_directories("logs", ec);
    auto written = fs::last_write_time("peggle2_runtime.log", ec);
    auto sys = std::chrono::clock_cast<std::chrono::system_clock>(written);
    std::time_t t = std::chrono::system_clock::to_time_t(sys);
    std::tm tm{};
    localtime_s(&tm, &t);
    char name[64];
    strftime(name, sizeof(name), "logs/peggle2_%Y%m%d-%H%M%S.log", &tm);
    fs::rename("peggle2_runtime.log", name, ec);
    std::vector<fs::path> old;
    for (auto& e : fs::directory_iterator("logs", ec))
        if (e.path().extension() == ".log") old.push_back(e.path());
    std::sort(old.begin(), old.end());
    for (size_t i = 0; i + 10 < old.size(); i++) fs::remove(old[i], ec);
}

// ---- Thread names and the sampling profiler (PEGGLE2_PROFILE=start,seconds) ------------------
void NameHostThread(const char* fmt, ...)
{
    char buf[128];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    wchar_t w[128];
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, w, 128);
    SetThreadDescription(GetCurrentThread(), w);
}

// Samples every thread of the process about once a millisecond (suspend, read RIP, resume) and
// logs, per thread, the share of samples per function: guest functions show as sub_XXXXXXXX,
// waits as the system call they sit in. The window title (with fps) is logged at both ends.
struct ProfThread { HANDLE h = nullptr; std::string name; std::unordered_map<uint64_t, uint32_t> rips; uint64_t samples = 0; };

static std::string OwnWindowTitle()
{
    struct P { DWORD pid; char title[256]; } p{ GetCurrentProcessId(), "" };
    EnumWindows([](HWND w, LPARAM l) -> BOOL {
        auto* p = (P*)l;
        DWORD wp = 0;
        GetWindowThreadProcessId(w, &wp);
        if (wp == p->pid && IsWindowVisible(w)) { GetWindowTextA(w, p->title, sizeof(p->title)); return FALSE; }
        return TRUE;
    }, (LPARAM)&p);
    return std::string(p.title);
}

static void ProfilerThread(double startSeconds, double seconds)
{
    NameHostThread("profiler");
    Sleep(DWORD(startSeconds * 1000));
    std::unordered_map<DWORD, ProfThread> threads;
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    auto refresh = [&] {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || threads.count(te.th32ThreadID)) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            ProfThread& t = threads[te.th32ThreadID];
            t.h = h;
            PWSTR d = nullptr;
            char name[128] = "";
            if (SUCCEEDED(GetThreadDescription(h, &d)) && d)
            {
                WideCharToMultiByte(CP_UTF8, 0, d, -1, name, sizeof(name), nullptr, nullptr);
                LocalFree(d);
            }
            t.name = name[0] ? name : ("host " + std::to_string(te.th32ThreadID));
        }
        CloseHandle(snap);
    };
    Log("[profile] start (%.0f s), window: %s", seconds, OwnWindowTitle().c_str());
    timeBeginPeriod(1);
    LARGE_INTEGER f, t0, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    uint64_t rounds = 0;
    std::vector<std::pair<ProfThread*, uint64_t>> got;
    got.reserve(256);
    do
    {
        if (rounds++ % 500 == 0) refresh();
        got.clear();
        for (auto& [id, t] : threads)
        {
            if (SuspendThread(t.h) == DWORD(-1)) continue;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            BOOL ok = GetThreadContext(t.h, &c);
            ResumeThread(t.h);
            if (ok) got.push_back({ &t, c.Rip });   // no allocation while a thread is suspended
        }
        for (auto& [t, rip] : got) { t->rips[rip]++; t->samples++; }
        Sleep(1);
        QueryPerformanceCounter(&now);
    } while (double(now.QuadPart - t0.QuadPart) / f.QuadPart < seconds);
    timeEndPeriod(1);
    Log("[profile] %llu rounds, window: %s", (unsigned long long)rounds, OwnWindowTitle().c_str());

    {
        std::lock_guard<std::mutex> g(g_symLock);
        if (!g_symReady)
        {
            SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
            g_symReady = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
        }
    }
    std::unordered_map<uint64_t, std::string> symCache;
    auto symName = [&](uint64_t rip) -> std::string {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        auto* si = (SYMBOL_INFO*)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        std::lock_guard<std::mutex> g(g_symLock);
        IMAGEHLP_MODULE64 mi{};
        mi.SizeOfStruct = sizeof(mi);
        std::string mod = SymGetModuleInfo64(GetCurrentProcess(), rip, &mi) ? std::string(mi.ModuleName) + "!" : "?!";
        if (SymFromAddr(GetCurrentProcess(), rip, &disp, si)) return mod + si->Name;
        char b[32];
        snprintf(b, sizeof(b), "%llX", (unsigned long long)rip);
        return mod + b;
    };
    std::vector<ProfThread*> order;
    for (auto& [id, t] : threads) order.push_back(&t);
    std::sort(order.begin(), order.end(), [](ProfThread* a, ProfThread* b) { return a->name < b->name; });
    for (ProfThread* t : order)
    {
        if (!t->samples) continue;
        std::unordered_map<std::string, uint64_t> byFunc;
        for (auto& [rip, n] : t->rips)
        {
            auto it = symCache.find(rip);
            if (it == symCache.end()) it = symCache.emplace(rip, symName(rip)).first;
            byFunc[it->second] += n;
        }
        std::vector<std::pair<uint64_t, std::string>> top;
        for (auto& [name, n] : byFunc) top.push_back({ n, name });
        std::sort(top.rbegin(), top.rend());
        std::string line;
        for (size_t i = 0; i < top.size() && i < 12; i++)
        {
            char b[320];
            snprintf(b, sizeof(b), "%s%.1f%% %s", i ? ", " : "", 100.0 * top[i].first / t->samples, top[i].second.c_str());
            line += b;
        }
        Log("[profile] %-26s %6llu: %s", t->name.c_str(), (unsigned long long)t->samples, line.c_str());
    }
}

// ---- Spike profiler (PEGGLE2_PROFILE=spikes) ------------------------------------------------------
// Samples every thread about once a millisecond into a ring. VdSwap reports game frames slower
// than 40 ms (ProfilerReportSpike); for each, the log gets the frame's length and, per thread that
// was busy in it, the functions its samples fell in.
namespace
{
    // stack: 1 + sequence number of its g_stackRing entry (0 = none); game threads get a call stack per sample
    // so a slow frame can be broken down inclusively (which caller the time was spent under).
    struct SpikeSample { int64_t qpc; DWORD tid; uint64_t rip; uint64_t stack; };
    constexpr size_t kSpikeRing = 1 << 20;
    constexpr size_t kStackDepth = 20, kStackRing = 1 << 17;
    struct SpikeStack { uint64_t rip[kStackDepth]; uint32_t n; };
    std::vector<SpikeStack> g_stackRing;
    uint64_t g_stackPos = 0;
    std::vector<SpikeSample> g_spikeRing;
    std::atomic<uint64_t> g_spikePos{ 0 };
    std::atomic<bool> g_spikeMode{ false };
    std::mutex g_spikeReqLock;
    std::vector<std::pair<int64_t, int64_t>> g_spikeReqs;
}

void ProfilerReportSpike(int64_t qpcStart, int64_t qpcEnd)
{
    if (!g_spikeMode.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> g(g_spikeReqLock);
    if (g_spikeReqs.size() < 64) g_spikeReqs.push_back({ qpcStart, qpcEnd });
}

static void SpikeProfilerThread()
{
    NameHostThread("profiler");
    g_spikeRing.resize(kSpikeRing);
    g_stackRing.resize(kStackRing);
    g_spikeMode = true;
    std::unordered_map<DWORD, ProfThread> threads;
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    auto refresh = [&] {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            auto it = threads.find(te.th32ThreadID);
            if (it != threads.end() && it->second.name.rfind("host ", 0) != 0) continue;   // named already
            HANDLE h = it != threads.end() ? it->second.h
                     : OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            ProfThread& t = threads[te.th32ThreadID];
            t.h = h;
            PWSTR d = nullptr;
            char name[128] = "";
            if (SUCCEEDED(GetThreadDescription(h, &d)) && d)
            {
                WideCharToMultiByte(CP_UTF8, 0, d, -1, name, sizeof(name), nullptr, nullptr);
                LocalFree(d);
            }
            t.name = name[0] ? name : ("host " + std::to_string(te.th32ThreadID));
        }
        CloseHandle(snap);
    };
    {
        std::lock_guard<std::mutex> g(g_symLock);
        if (!g_symReady)
        {
            SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
            g_symReady = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
        }
    }
    std::unordered_map<uint64_t, std::string> symCache;
    auto symName = [&](uint64_t rip) -> const std::string& {
        auto it = symCache.find(rip);
        if (it != symCache.end()) return it->second;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        auto* si = (SYMBOL_INFO*)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        std::string name;
        {
            std::lock_guard<std::mutex> g(g_symLock);
            IMAGEHLP_MODULE64 mi{};
            mi.SizeOfStruct = sizeof(mi);
            std::string mod = SymGetModuleInfo64(GetCurrentProcess(), rip, &mi) ? std::string(mi.ModuleName) + "!" : "?!";
            if (SymFromAddr(GetCurrentProcess(), rip, &disp, si)) name = mod + si->Name;
            else { char b[32]; snprintf(b, sizeof(b), "%llX", (unsigned long long)rip); name = mod + b; }
        }
        return symCache.emplace(rip, std::move(name)).first->second;
    };
    auto isWait = [](const std::string& n) {
        return n.find("ntdll!Zw") == 0 || n.find("ntdll!Nt") == 0 || n.find("SleepConditionVariable") != std::string::npos ||
               n.find("RtlDelayExecution") != std::string::npos || n.find("SwitchToThread") != std::string::npos ||
               n.find("WaitForVerticalBlank") != std::string::npos;
    };

    timeBeginPeriod(1);
    Log("[profile] spike mode: game frames over 40 ms are broken down per thread");
    struct Got { DWORD id; uint64_t rip; uint64_t stack; };
    std::vector<Got> got;
    got.reserve(256);
    for (uint64_t round = 0;; round++)
    {
        if (round % 500 == 0) refresh();
        got.clear();
        for (auto& [id, t] : threads)
        {
            // Game threads (guest threads and the unnamed main thread) get a call stack.
            bool wantStack = t.name.rfind("guest", 0) == 0 || t.name.rfind("host ", 0) == 0;
            if (SuspendThread(t.h) == DWORD(-1)) continue;
            CONTEXT c{};
            c.ContextFlags = wantStack ? (CONTEXT_CONTROL | CONTEXT_INTEGER) : CONTEXT_CONTROL;
            BOOL ok = GetThreadContext(t.h, &c);
            uint64_t rip = c.Rip;
            uint64_t stack = 0;
            if (ok && wantStack)
            {
                // Only lock-free unwinding while the thread is suspended (as SampleThreadStack).
                SpikeStack& st = g_stackRing[g_stackPos % kStackRing];
                st.n = 0;
                for (; st.n < kStackDepth && c.Rip; st.n++)
                {
                    st.rip[st.n] = st.n ? c.Rip - 1 : c.Rip;
                    DWORD64 imageBase;
                    auto* fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
                    if (!fe) { st.n++; break; }
                    void* handlerData;
                    DWORD64 establisher;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &handlerData, &establisher, nullptr);
                }
                stack = ++g_stackPos;   // 1 + the entry's sequence number
            }
            ResumeThread(t.h);
            if (ok) got.push_back({ id, rip, stack });
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        for (auto& g : got) g_spikeRing[g_spikePos++ % kSpikeRing] = { now.QuadPart, g.id, g.rip, g.stack };

        // Reports whose frame ended at least 5 ms ago.
        std::vector<std::pair<int64_t, int64_t>> due;
        {
            std::lock_guard<std::mutex> g(g_spikeReqLock);
            for (size_t i = 0; i < g_spikeReqs.size();)
                if (now.QuadPart - g_spikeReqs[i].second > f.QuadPart / 200) { due.push_back(g_spikeReqs[i]); g_spikeReqs.erase(g_spikeReqs.begin() + i); }
                else i++;
        }
        for (auto [a, b] : due)
        {
            std::unordered_map<DWORD, std::unordered_map<std::string, uint32_t>> byThread, inclusive;
            std::unordered_map<DWORD, uint32_t> total;
            uint64_t end = g_spikePos.load();
            for (uint64_t k = end > kSpikeRing ? end - kSpikeRing : 0; k < end; k++)
            {
                const SpikeSample& s = g_spikeRing[k % kSpikeRing];
                if (s.qpc < a || s.qpc > b) continue;
                total[s.tid]++;
                const std::string& n = symName(s.rip);
                if (isWait(n)) continue;
                byThread[s.tid][n]++;
                // Inclusive: every guest function on the stack, once per sample. (A stack slot
                // overwritten since is skipped: the ring holds the last 131,072 stacks.)
                if (s.stack && g_stackPos - (s.stack - 1) <= kStackRing)
                {
                    const SpikeStack& st = g_stackRing[(s.stack - 1) % kStackRing];
                    std::vector<const std::string*> seen;
                    for (uint32_t i = 0; i < st.n; i++)
                    {
                        const std::string& f = symName(st.rip[i]);
                        if (f.find("__imp__sub_") == std::string::npos) continue;   // guest functions only
                        if (std::find(seen.begin(), seen.end(), &f) != seen.end()) continue;
                        seen.push_back(&f);
                        inclusive[s.tid][f]++;
                    }
                }
            }
            Log("[spike] game frame took %.1f ms", double(b - a) * 1000.0 / f.QuadPart);
            for (auto& [tid, funcs] : byThread)
            {
                uint32_t busy = 0;
                for (auto& [n, c] : funcs) busy += c;
                if (busy * 10 < total[tid]) continue;   // under 10% busy: skip
                std::vector<std::pair<uint32_t, std::string>> top;
                for (auto& [n, c] : funcs) top.push_back({ c, n });
                std::sort(top.rbegin(), top.rend());
                std::string line;
                for (size_t i = 0; i < top.size() && i < 8; i++)
                {
                    char buf[300];
                    snprintf(buf, sizeof(buf), "%s%u %s", i ? ", " : "", top[i].first, top[i].second.c_str());
                    line += buf;
                }
                auto it = threads.find(tid);
                Log("[spike]   %-24s busy %u/%u samples: %s", it != threads.end() ? it->second.name.c_str() : "?", busy, total[tid], line.c_str());
                if (auto inc = inclusive.find(tid); inc != inclusive.end() && busy * 2 >= total[tid])
                {
                    std::vector<std::pair<uint32_t, std::string>> topInc;
                    for (auto& [n, c] : inc->second) topInc.push_back({ c, n });
                    std::sort(topInc.rbegin(), topInc.rend());
                    std::string incLine;
                    for (size_t i = 0; i < topInc.size() && i < 14; i++)
                    {
                        char buf[300];
                        std::string n = topInc[i].second.substr(topInc[i].second.find("__imp__sub_") + 11);
                        snprintf(buf, sizeof(buf), "%s%u %s", i ? ", " : "", topInc[i].first, n.c_str());
                        incLine += buf;
                    }
                    Log("[spike]     inclusive (samples under each guest function): %s", incLine.c_str());
                }
            }
        }
        Sleep(1);
    }
}

int main(int argc, char** argv)
{
    // 1 ms timer resolution for the whole run: the GPU thread's idle wait, the audio tick and
    // the guest's own Sleep()s otherwise last a 15.6 ms timer tick, which made frames late.
    timeBeginPeriod(1);
    RotateLogs();
    g_logFile = fopen("peggle2_runtime.log", "w");
    if (g_logFile) setvbuf(g_logFile, nullptr, _IOFBF, 1 << 16);
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    std::thread(LogFlushThread).detach();
    atexit(LogFlush);
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
    if (const char* p = getenv("PEGGLE2_PROFILE"); p && strcmp(p, "spikes") == 0)
        std::thread(SpikeProfilerThread).detach();
    else if (p)   // "start,seconds": sample all threads
    {
        double start = atof(p), secs = strchr(p, ',') ? atof(strchr(p, ',') + 1) : 20;
        std::thread(ProfilerThread, start, secs).detach();
    }

    // Hardware register windows (GPU at 0x7FC80000, XMA at 0x7FEA0000, ...). D3D and the
    // audio driver touch these directly. For now they are plain zeroed memory: no hardware
    // is emulated yet, so reads return whatever was last written.
    CommitGuest(kMmioBase, kMmioSize);
    Log("[boot] MMIO window 0x%08X-0x%08X backed by plain memory (no GPU/XMA emulation yet)", kMmioBase, kMmioBase + kMmioSize - 1);

    InitImports(xex.data(), xex.size());

    StartWatchdog(10);
    StartRenderer();

    PPCContext ctx{};
    InitGuestThread(ctx, 1024 * 1024, 1);
    Log("[boot] calling _xstart (0x%08zX)", image.entry_point);
    _xstart(ctx, g_base);
    Log("[boot] _xstart returned (r3 = 0x%08X)", ctx.r3.u32);
    return 0;
}
