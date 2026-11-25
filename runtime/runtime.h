// Peggle 2 recompilation runtime: shared declarations.
#pragma once

// Standard headers first: ppc_context.h defines short macros that can break them.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ppc_config.h"
#include "ppc_context.h"

// ---- Guest memory --------------------------------------------------------------------
// The whole 32-bit guest address space is one reservation; guest address A lives at
// g_base + A. Guest memory is big-endian.
extern uint8_t* g_base;

constexpr uint64_t kGuestSpace = 0x100000000ull;

inline uint8_t* GuestPtr(uint32_t addr) { return g_base + addr; }
inline uint32_t GuestAddr(const void* host) { return uint32_t((const uint8_t*)host - g_base); }

inline uint8_t  Load8(uint32_t a)  { return *(volatile uint8_t*)(g_base + a); }
inline uint16_t Load16(uint32_t a) { return __builtin_bswap16(*(volatile uint16_t*)(g_base + a)); }
inline uint32_t Load32(uint32_t a) { return __builtin_bswap32(*(volatile uint32_t*)(g_base + a)); }
inline uint64_t Load64(uint32_t a) { return __builtin_bswap64(*(volatile uint64_t*)(g_base + a)); }
inline void Store8(uint32_t a, uint8_t v)   { *(volatile uint8_t*)(g_base + a) = v; }
inline void Store16(uint32_t a, uint16_t v) { *(volatile uint16_t*)(g_base + a) = __builtin_bswap16(v); }
inline void Store32(uint32_t a, uint32_t v) { *(volatile uint32_t*)(g_base + a) = __builtin_bswap32(v); }
inline void Store64(uint32_t a, uint64_t v) { *(volatile uint64_t*)(g_base + a) = __builtin_bswap64(v); }

std::string LoadCString(uint32_t addr, size_t max = 4096);

// Commit host pages backing [addr, addr+size). Idempotent.
bool CommitGuest(uint32_t addr, uint32_t size);

// Bump allocators over guest regions (frees are currently ignored).
//   virtual:  0x40000000 - 0x7BFFFFFF  (NtAllocateVirtualMemory, XamAlloc, stacks)
//   runtime:  0x7C000000 - 0x7EFFFFFF  (kernel structures owned by this runtime)
//   MMIO:     0x7FC00000 - 0x7FFFFFFF  (hardware registers; plain memory for now, see main.cpp)
//               0x7FC80000 Xenos GPU registers, 0x7FEA0000 XMA audio decoder registers
//   physical: 0xA0000000 - 0xBFFFFFFF  (MmAllocatePhysicalMemoryEx, GPU-visible memory)
constexpr uint32_t kMmioBase = 0x7FC00000;
constexpr uint32_t kMmioSize = 0x00400000;
constexpr uint32_t kGpuRegisterBase = 0x7FC80000;
constexpr uint32_t kXmaRegisterBase = 0x7FEA0000;
uint32_t AllocVirtual(uint32_t size, uint32_t align = 0x10000);
uint32_t AllocPhysical(uint32_t size, uint32_t align = 0x1000);
// Small zeroed runtime-owned allocation (kernel structures, strings).
uint32_t AllocRuntime(uint32_t size, uint32_t align = 16);

// ---- Function table ------------------------------------------------------------------
inline PPCFunc*& FuncSlot(uint32_t guest)
{
    return PPC_LOOKUP_FUNC(g_base, guest);
}
// Call a guest function by address (through the indirect-call table).
void CallGuest(PPCContext& ctx, uint32_t guestFunc);

// ---- Threads -------------------------------------------------------------------------
// Builds a guest stack + KPCR for the calling host thread, binds it to `kthread` (allocated
// if 0) and fills ctx (r1, r13). Sets t_kthread.
void InitGuestThread(PPCContext& ctx, uint32_t stackSize, uint32_t threadId, uint32_t kthread = 0);
uint32_t AllocKThread(uint32_t threadId);        // KTHREAD with a thread DISPATCHER_HEADER
void UnregisterGuestThread();                    // call before a guest host thread exits
// Waitable event handle (for runtime-created objects such as notification listeners).
uint32_t CreateEventHandle(bool synchronization, bool initialState, uint32_t* guestObject = nullptr);
void SignalGuestEvent(uint32_t guestObject, bool state);
void SignalHandle(uint32_t handle);              // set the event behind a guest handle

// Arguments beyond r3-r10 live in the caller's parameter save area (64-bit ABI):
// argument n (0-based, n >= 8) is at r1 + 0x54 + (n - 8) * 8.
inline uint32_t StackArg(const PPCContext& ctx, int n) { return Load32(ctx.r1.u32 + 0x54 + (n - 8) * 8); }

// XOVERLAPPED: +0 InternalLow (result), +4 InternalHigh (length), +0xC hEvent,
// +0x10 completion routine, +0x18 extended error. Completes an async XAM call immediately.
constexpr uint32_t X_ERROR_SUCCESS = 0, X_ERROR_IO_PENDING = 997;
uint32_t CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length = 0);
void PostNotification(uint32_t id, uint32_t param);
extern thread_local uint32_t t_kthread;          // guest KTHREAD of the current host thread

// ---- Logging -------------------------------------------------------------------------
void Log(const char* fmt, ...);
extern thread_local const char* g_lastImport;
// Guest object the current thread is blocked on (0 when not waiting); read by the watchdog.
extern thread_local std::atomic<uint32_t> t_waitingOn;
// A texture as described by a Xenos texture fetch constant (decoded in gpu.cpp).
struct GuestTextureInfo
{
    uint32_t address = 0;      // guest address of mip 0 (0xA0000000 | physical)
    uint32_t width = 0, height = 0;
    uint32_t pitch = 0;        // row pitch in texels (multiple of 32)
    uint32_t format = 0;       // xenos TextureFormat
    uint32_t endian = 0;       // 0 none, 1 8in16, 2 8in32, 3 16in32
    bool tiled = false;
};

// Host renderer (render.cpp): window + D3D12 swap chain; RendererPresent() once per VdSwap.
// `preview` (may be null) is the frame's largest bound texture, shown until draws are
// translated.
void StartRenderer();
void RendererPresent(const GuestTextureInfo* preview);

// Log the guest call chain of the calling host thread (for hooks).
void LogGuestBacktrace();
// Log the guest call chain of another host thread (HANDLE), sampled safely (watchdog).
void SampleThreadStack(void* hThread);
// Hardware write watchpoint (DR0) on the calling thread: logs every 4-byte write to `addr`.
void WatchGuestWrite(uint32_t addr);
void UnwatchGuestWrite();
// Watchdog: logs every guest thread's last import and wait state periodically.
void StartWatchdog(int periodSeconds);

// ---- Imports -------------------------------------------------------------------------
// Implement a kernel/XAM export:   IMPORT(XamAlloc) { ... ctx.r3.u64 = 0; }
// The name must match the export name used by XenonRecomp (see build/imports.txt).
// tools/gen_imports.py emits a logging stub for every import that has no IMPORT().
#define IMPORT(name) \
    static void name##_impl(PPCContext& ctx, uint8_t* base); \
    PPC_FUNC(__imp__##name) { g_lastImport = #name; name##_impl(ctx, base); } \
    static void name##_impl(PPCContext& ctx, uint8_t* base)

void InitImports(const uint8_t* xexFile, size_t xexSize);   // fills IAT + variable slots

// ---- Kernel objects ------------------------------------------------------------------
extern std::filesystem::path g_gameDir;

struct KernelObject
{
    virtual ~KernelObject() = default;
    virtual const char* TypeName() const = 0;
};
// Handle table. Handles are guest-visible u32 values; NtClose releases them.
uint32_t CreateHandle(std::shared_ptr<KernelObject> obj);
std::shared_ptr<KernelObject> LookupHandle(uint32_t handle);
bool CloseHandle(uint32_t handle);
template <class T> std::shared_ptr<T> LookupHandleAs(uint32_t handle)
{
    return std::dynamic_pointer_cast<T>(LookupHandle(handle));
}

// NTSTATUS values
constexpr uint32_t STATUS_SUCCESS = 0;
constexpr uint32_t STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t STATUS_ACCESS_DENIED = 0xC0000022;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t STATUS_NO_MORE_FILES = 0x80000006;
