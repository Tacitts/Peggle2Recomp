// Threads, dispatcher objects (events, semaphores, mutants, timers) and waits.
//
// Every waitable object has a DISPATCHER_HEADER in guest memory, and that header IS the
// state: objects created through Nt*/Ex* get a runtime-allocated body, objects the game
// initialises inline in its own memory are used as-is. One global lock + condition
// variable serialises all signal/wait operations.
//
// DISPATCHER_HEADER: +0 u8 Type, +1 u8 Absolute, +2 u8 Size, +3 u8 Inserted,
//                    +4 s32 SignalState, +8 LIST_ENTRY WaitListHead
// Types: 0 notification event, 1 synchronization event, 2 mutant, 5 semaphore,
//        6 thread, 8 notification timer, 9 synchronization timer.
// KMUTANT: +0x18 OwnerThread.  KSEMAPHORE: +0x10 Limit.
#include "runtime.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>

using Clock = std::chrono::steady_clock;

constexpr uint32_t kStatusTimeout = 0x00000102;
constexpr uint32_t kStatusAbandonedWait0 = 0x00000080;
constexpr uint32_t kStatusMutantNotOwned = 0xC0000046;
constexpr uint32_t kStatusSemaphoreLimitExceeded = 0xC0000047;
constexpr uint32_t kCurrentThreadHandle = 0xFFFFFFFE;

enum : uint8_t
{
    kNotificationEvent = 0, kSynchronizationEvent = 1, kMutant = 2, kSemaphore = 5,
    kThread = 6, kNotificationTimer = 8, kSynchronizationTimer = 9,
};

thread_local uint32_t t_kthread = 0;
thread_local std::atomic<uint32_t> t_waitingOn{ 0 };

static std::mutex g_dispatchLock;
static std::condition_variable g_dispatchCv;

// ---- Handle-table objects ------------------------------------------------------------------
struct DispatcherObject : KernelObject
{
    uint32_t guest = 0;   // guest address of the DISPATCHER_HEADER
    const char* TypeName() const override { return "Dispatcher"; }
};

struct ThreadObject : DispatcherObject
{
    const char* TypeName() const override { return "Thread"; }
    uint32_t threadId = 0;
    uint32_t stackSize = 0;
    uint32_t xapiStartup = 0, startAddress = 0, startContext = 0;
    std::mutex suspendLock;
    std::condition_variable suspendCv;
    int suspendCount = 0;
};

static std::mutex g_threadMapLock;
static std::unordered_map<uint32_t, std::shared_ptr<ThreadObject>> g_threadsByKthread;
static std::unordered_map<uint32_t, std::shared_ptr<ThreadObject>> g_threadsById;
static std::atomic<uint32_t> g_nextThreadId{ 2 };

static uint8_t HeaderType(uint32_t obj) { return Load8(obj); }
static int32_t SignalState(uint32_t obj) { return int32_t(Load32(obj + 4)); }
static void SetSignalState(uint32_t obj, int32_t v) { Store32(obj + 4, uint32_t(v)); }

static void InitHeader(uint32_t obj, uint8_t type, int32_t state, uint8_t sizeWords)
{
    Store8(obj + 0, type);
    Store8(obj + 1, 0);
    Store8(obj + 2, sizeWords);
    Store8(obj + 3, 0);
    SetSignalState(obj, state);
    Store32(obj + 8, obj + 8);    // empty wait list (points at itself)
    Store32(obj + 12, obj + 8);
}

static uint32_t ResolveHandle(uint32_t handle)
{
    if (handle == kCurrentThreadHandle) return t_kthread;
    auto obj = LookupHandleAs<DispatcherObject>(handle);
    return obj ? obj->guest : 0;
}

// ---- Wait core (caller holds g_dispatchLock) ---------------------------------------------------
static bool IsSignaled(uint32_t obj)
{
    switch (HeaderType(obj))
    {
    case kMutant:
        return SignalState(obj) > 0 || Load32(obj + 0x18) == t_kthread;
    default:
        return SignalState(obj) > 0;
    }
}

static void Acquire(uint32_t obj)
{
    switch (HeaderType(obj))
    {
    case kSynchronizationEvent:
    case kSynchronizationTimer:
        SetSignalState(obj, 0);
        break;
    case kSemaphore:
        SetSignalState(obj, SignalState(obj) - 1);
        break;
    case kMutant:
        SetSignalState(obj, SignalState(obj) - 1);
        Store32(obj + 0x18, t_kthread);
        break;
    default:
        break; // notification events/timers, threads: stay signalled
    }
}

// timeout: guest pointer to s64 in 100 ns units (negative = relative, positive = absolute
// FILETIME, 0 = poll), or 0 for infinite.
static bool Deadline(uint32_t timeoutPtr, Clock::time_point& deadline)
{
    if (!timeoutPtr) return false;
    int64_t t = int64_t(Load64(timeoutPtr));
    if (t <= 0)
    {
        deadline = Clock::now() + std::chrono::microseconds(-t / 10);
    }
    else
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        int64_t now = int64_t((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
        deadline = Clock::now() + std::chrono::microseconds(std::max<int64_t>(0, t - now) / 10);
    }
    return true;
}

// Returns STATUS_WAIT_0 + index (wait any), STATUS_SUCCESS (wait all) or kStatusTimeout.
static uint32_t WaitObjects(const uint32_t* objs, uint32_t count, bool waitAll, uint32_t timeoutPtr)
{
    Clock::time_point deadline;
    bool hasDeadline = Deadline(timeoutPtr, deadline);
    struct WaitMark { ~WaitMark() { t_waitingOn = 0; } } mark;
    t_waitingOn = count ? objs[0] : 0;
    std::unique_lock<std::mutex> lock(g_dispatchLock);
    for (;;)
    {
        if (waitAll)
        {
            bool all = true;
            for (uint32_t i = 0; i < count; i++) all &= objs[i] == 0 || IsSignaled(objs[i]);
            if (all)
            {
                for (uint32_t i = 0; i < count; i++) if (objs[i]) Acquire(objs[i]);
                return STATUS_SUCCESS;
            }
        }
        else
        {
            for (uint32_t i = 0; i < count; i++)
            {
                if (!objs[i] || IsSignaled(objs[i]))
                {
                    if (objs[i]) Acquire(objs[i]);
                    return STATUS_SUCCESS + i;
                }
            }
        }
        if (hasDeadline && Clock::now() >= deadline) return kStatusTimeout;
        // Bounded sleep: also catches state the game changed without going through the kernel.
        auto wakeAt = Clock::now() + std::chrono::milliseconds(5);
        if (hasDeadline && deadline < wakeAt) wakeAt = deadline;
        g_dispatchCv.wait_until(lock, wakeAt);
    }
}

static void NotifyAll() { g_dispatchCv.notify_all(); }

// ---- Events --------------------------------------------------------------------------------
static int32_t SetEventState(uint32_t obj, int32_t state);
uint32_t CreateEventHandle(bool synchronization, bool initialState, uint32_t* guestObject)
{
    uint32_t body = AllocRuntime(0x10, 16);
    InitHeader(body, synchronization ? kSynchronizationEvent : kNotificationEvent, initialState ? 1 : 0, 4);
    auto obj = std::make_shared<DispatcherObject>();
    obj->guest = body;
    if (guestObject) *guestObject = body;
    return CreateHandle(obj);
}

void SignalGuestEvent(uint32_t guestObject, bool state) { SetEventState(guestObject, state ? 1 : 0); }

void SignalHandle(uint32_t handle)
{
    if (uint32_t obj = ResolveHandle(handle)) SetEventState(obj, 1);
}

// NtCreateEvent(u32* handle, OBJECT_ATTRIBUTES*, u32 type, u32 initialState)
IMPORT(NtCreateEvent)
{
    uint32_t handleOut = ctx.r3.u32;
    uint32_t body = AllocRuntime(0x10, 16);
    InitHeader(body, ctx.r5.u32 ? kSynchronizationEvent : kNotificationEvent, ctx.r6.u32 ? 1 : 0, 4);
    auto obj = std::make_shared<DispatcherObject>();
    obj->guest = body;
    Store32(handleOut, CreateHandle(obj));
    ctx.r3.u64 = STATUS_SUCCESS;
}

static int32_t SetEventState(uint32_t obj, int32_t state)
{
    std::lock_guard<std::mutex> g(g_dispatchLock);
    int32_t prev = SignalState(obj);
    SetSignalState(obj, state);
    NotifyAll();
    return prev;
}

IMPORT(NtSetEvent)
{
    uint32_t obj = ResolveHandle(ctx.r3.u32);
    if (!obj) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    int32_t prev = SetEventState(obj, 1);
    if (ctx.r4.u32) Store32(ctx.r4.u32, uint32_t(prev));
    ctx.r3.u64 = STATUS_SUCCESS;
}
IMPORT(NtClearEvent)
{
    uint32_t obj = ResolveHandle(ctx.r3.u32);
    if (!obj) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    SetEventState(obj, 0);
    ctx.r3.u64 = STATUS_SUCCESS;
}
// LONG KeSetEvent(KEVENT*, increment, wait) / KeResetEvent(KEVENT*) -> previous state
IMPORT(KeSetEvent) { ctx.r3.u64 = uint32_t(SetEventState(ctx.r3.u32, 1)); }
IMPORT(KeResetEvent) { ctx.r3.u64 = uint32_t(SetEventState(ctx.r3.u32, 0)); }

// ---- Semaphores and mutants -------------------------------------------------------------------
// NtCreateSemaphore(u32* handle, OBJECT_ATTRIBUTES*, s32 initial, s32 limit)
IMPORT(NtCreateSemaphore)
{
    uint32_t body = AllocRuntime(0x14, 16);
    InitHeader(body, kSemaphore, int32_t(ctx.r5.u32), 5);
    Store32(body + 0x10, ctx.r6.u32);
    auto obj = std::make_shared<DispatcherObject>();
    obj->guest = body;
    Store32(ctx.r3.u32, CreateHandle(obj));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtReleaseSemaphore(handle, s32 count, s32* previous)
IMPORT(NtReleaseSemaphore)
{
    uint32_t obj = ResolveHandle(ctx.r3.u32);
    if (!obj) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    std::lock_guard<std::mutex> g(g_dispatchLock);
    int32_t prev = SignalState(obj), limit = int32_t(Load32(obj + 0x10)), add = int32_t(ctx.r4.u32);
    if (prev + add > limit) { ctx.r3.u64 = kStatusSemaphoreLimitExceeded; return; }
    SetSignalState(obj, prev + add);
    if (ctx.r5.u32) Store32(ctx.r5.u32, uint32_t(prev));
    NotifyAll();
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtCreateMutant(u32* handle, OBJECT_ATTRIBUTES*, BOOL initialOwner)
IMPORT(NtCreateMutant)
{
    uint32_t body = AllocRuntime(0x20, 16);
    bool owned = ctx.r5.u32 != 0;
    InitHeader(body, kMutant, owned ? 0 : 1, 8);
    Store32(body + 0x18, owned ? t_kthread : 0);
    auto obj = std::make_shared<DispatcherObject>();
    obj->guest = body;
    Store32(ctx.r3.u32, CreateHandle(obj));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtReleaseMutant(handle, s32* previous)
IMPORT(NtReleaseMutant)
{
    uint32_t obj = ResolveHandle(ctx.r3.u32);
    if (!obj) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    std::lock_guard<std::mutex> g(g_dispatchLock);
    if (Load32(obj + 0x18) != t_kthread) { ctx.r3.u64 = kStatusMutantNotOwned; return; }
    int32_t prev = SignalState(obj);
    SetSignalState(obj, prev + 1);
    if (prev + 1 > 0) Store32(obj + 0x18, 0);
    if (ctx.r4.u32) Store32(ctx.r4.u32, uint32_t(prev));
    NotifyAll();
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---- Timers ------------------------------------------------------------------------------------
struct TimerState
{
    Clock::time_point due;
    uint32_t periodMs = 0;
    bool armed = false;
};
static std::map<uint32_t, TimerState> g_timers;   // guest timer -> state (under g_dispatchLock)
static std::once_flag g_timerThreadOnce;

static void TimerThread()
{
    for (;;)
    {
        {
            std::lock_guard<std::mutex> g(g_dispatchLock);
            auto now = Clock::now();
            bool fired = false;
            for (auto& [obj, t] : g_timers)
            {
                if (!t.armed || now < t.due) continue;
                SetSignalState(obj, 1);
                fired = true;
                if (t.periodMs) t.due += std::chrono::milliseconds(t.periodMs);
                else t.armed = false;
            }
            if (fired) NotifyAll();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// NtCreateTimer(u32* handle, OBJECT_ATTRIBUTES*, u32 type)  type 0 notification, 1 synchronization
IMPORT(NtCreateTimer)
{
    uint32_t body = AllocRuntime(0x28, 16);
    InitHeader(body, ctx.r5.u32 ? kSynchronizationTimer : kNotificationTimer, 0, 10);
    auto obj = std::make_shared<DispatcherObject>();
    obj->guest = body;
    Store32(ctx.r3.u32, CreateHandle(obj));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtSetTimerEx(handle, s64* dueTime, apcRoutine, apcMode, apcContext, BOOL resume, s32 periodMs, BOOL* prevState)
IMPORT(NtSetTimerEx)
{
    uint32_t obj = ResolveHandle(ctx.r3.u32);
    if (!obj) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if (ctx.r5.u32) Log("[timer] APC routine 0x%08X ignored (APCs not implemented)", ctx.r5.u32);
    std::call_once(g_timerThreadOnce, [] { std::thread(TimerThread).detach(); });
    Clock::time_point due;
    if (!Deadline(ctx.r4.u32, due)) due = Clock::now();
    std::lock_guard<std::mutex> g(g_dispatchLock);
    if (ctx.r10.u32) Store32(ctx.r10.u32, SignalState(obj) > 0 ? 1 : 0);
    SetSignalState(obj, 0);
    auto& t = g_timers[obj];
    t.due = due;
    t.periodMs = ctx.r9.u32;
    t.armed = true;
    ctx.r3.u64 = STATUS_SUCCESS;
}

IMPORT(NtCancelTimer)
{
    uint32_t obj = ResolveHandle(ctx.r3.u32);
    if (!obj) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    std::lock_guard<std::mutex> g(g_dispatchLock);
    if (ctx.r4.u32) Store32(ctx.r4.u32, SignalState(obj) > 0 ? 1 : 0);
    g_timers[obj].armed = false;
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---- Waits -------------------------------------------------------------------------------------
// KeWaitForSingleObject(object*, reason, mode, alertable, s64* timeout)
IMPORT(KeWaitForSingleObject)
{
    uint32_t obj = ctx.r3.u32;
    ctx.r3.u64 = WaitObjects(&obj, 1, false, ctx.r7.u32);
}

// KeWaitForMultipleObjects(count, object** array, waitType, reason, mode, alertable, s64* timeout, waitBlocks)
IMPORT(KeWaitForMultipleObjects)
{
    uint32_t count = std::min<uint32_t>(ctx.r3.u32, 64), objs[64];
    for (uint32_t i = 0; i < count; i++) objs[i] = Load32(ctx.r4.u32 + i * 4);
    ctx.r3.u64 = WaitObjects(objs, count, ctx.r5.u32 == 0, ctx.r9.u32);
}

// NtWaitForSingleObjectEx(handle, mode, alertable, s64* timeout)
IMPORT(NtWaitForSingleObjectEx)
{
    uint32_t obj = ResolveHandle(ctx.r3.u32);
    if (!obj)
    {
        // Non-dispatcher handles (files) are always signalled.
        ctx.r3.u64 = LookupHandle(ctx.r3.u32) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
        return;
    }
    ctx.r3.u64 = WaitObjects(&obj, 1, false, ctx.r6.u32);
}

// NtWaitForMultipleObjectsEx(count, u32* handles, waitType, mode, alertable, s64* timeout)
IMPORT(NtWaitForMultipleObjectsEx)
{
    uint32_t count = std::min<uint32_t>(ctx.r3.u32, 64), objs[64];
    for (uint32_t i = 0; i < count; i++) objs[i] = ResolveHandle(Load32(ctx.r4.u32 + i * 4));
    ctx.r3.u64 = WaitObjects(objs, count, ctx.r5.u32 == 0, ctx.r8.u32);
}

// KeDelayExecutionThread(mode, alertable, s64* interval)
IMPORT(KeDelayExecutionThread)
{
    Clock::time_point deadline;
    if (Deadline(ctx.r5.u32, deadline))
    {
        auto now = Clock::now();
        if (deadline > now) std::this_thread::sleep_until(deadline);
        else std::this_thread::yield();
    }
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---- Threads ----------------------------------------------------------------------------------
uint32_t AllocKThread(uint32_t threadId)
{
    uint32_t kthread = AllocRuntime(0x400, 16);
    InitHeader(kthread, kThread, 0, 0x10);
    Store32(kthread + 0x14C, threadId);
    return kthread;
}

static void ThreadMain(std::shared_ptr<ThreadObject> t)
{
    {
        std::unique_lock<std::mutex> lk(t->suspendLock);
        t->suspendCv.wait(lk, [&] { return t->suspendCount <= 0; });
    }
    PPCContext ctx{};
    NameHostThread("guest %u (0x%08X)", t->threadId, t->startAddress);
    InitGuestThread(ctx, t->stackSize, t->threadId, t->guest);
    Log("[thread %u] start 0x%08X(ctx 0x%08X)%s", t->threadId, t->startAddress, t->startContext,
        t->xapiStartup ? " via XAPI startup" : "");
    if (t->xapiStartup)
    {
        ctx.r3.u64 = t->startAddress;
        ctx.r4.u64 = t->startContext;
        CallGuest(ctx, t->xapiStartup);
    }
    else
    {
        ctx.r3.u64 = t->startContext;
        CallGuest(ctx, t->startAddress);
    }
    Log("[thread %u] exited (r3 = 0x%08X)", t->threadId, ctx.r3.u32);
    UnregisterGuestThread();
    SetEventState(t->guest, 1);
}

// ExCreateThread(u32* handle, u32 stackSize, u32* threadId, u32 xapiStartup,
//                u32 startAddress, u32 startContext, u32 flags)
IMPORT(ExCreateThread)
{
    auto t = std::make_shared<ThreadObject>();
    t->threadId = g_nextThreadId++;
    t->stackSize = std::max<uint32_t>(ctx.r4.u32 ? ctx.r4.u32 : 0x10000, 0x10000);
    t->xapiStartup = ctx.r6.u32;
    t->startAddress = ctx.r7.u32;
    t->startContext = ctx.r8.u32;
    t->guest = AllocKThread(t->threadId);
    t->suspendCount = (ctx.r9.u32 & 1) ? 1 : 0;   // CREATE_SUSPENDED
    {
        std::lock_guard<std::mutex> g(g_threadMapLock);
        g_threadsByKthread[t->guest] = t;
        g_threadsById[t->threadId] = t;
    }
    uint32_t h = CreateHandle(t);
    if (ctx.r3.u32) Store32(ctx.r3.u32, h);
    if (ctx.r5.u32) Store32(ctx.r5.u32, t->threadId);
    Log("[ExCreateThread] id %u entry 0x%08X ctx 0x%08X stack 0x%X%s -> handle 0x%08X", t->threadId, t->startAddress,
        t->startContext, t->stackSize, t->suspendCount ? " (suspended)" : "", h);
    std::thread(ThreadMain, t).detach();
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtResumeThread(handle, u32* previousSuspendCount)
IMPORT(NtResumeThread)
{
    auto t = LookupHandleAs<ThreadObject>(ctx.r3.u32);
    if (!t) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    int prev;
    {
        std::lock_guard<std::mutex> lk(t->suspendLock);
        prev = t->suspendCount;
        if (t->suspendCount > 0) t->suspendCount--;
    }
    t->suspendCv.notify_all();
    if (ctx.r4.u32) Store32(ctx.r4.u32, uint32_t(prev));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ExTerminateThread(exitStatus): mark the KTHREAD signalled and end the host thread.
IMPORT(ExTerminateThread)
{
    Log("[thread] ExTerminateThread(0x%08X)", ctx.r3.u32);
    UnregisterGuestThread();
    if (t_kthread) SetEventState(t_kthread, 1);
    ExitThread(ctx.r3.u32);
}

IMPORT(KeSetAffinityThread) { ctx.r3.u64 = 0; }
IMPORT(KeSetBasePriorityThread) { ctx.r3.u64 = 0; }
IMPORT(KeQueryBasePriorityThread) { ctx.r3.u64 = 0; }

// ---- Object manager ----------------------------------------------------------------------------
// ObReferenceObjectByHandle(handle, objectType, void** out): out = guest body pointer
IMPORT(ObReferenceObjectByHandle)
{
    uint32_t obj = ResolveHandle(ctx.r3.u32);
    if (!obj) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if (ctx.r5.u32) Store32(ctx.r5.u32, obj);
    ctx.r3.u64 = STATUS_SUCCESS;
}
IMPORT(ObDereferenceObject) {}

// ObOpenObjectByPointer(object*, u32* handle)   (two arguments on Xbox; see OpenThread at 0x82319288)
IMPORT(ObOpenObjectByPointer)
{
    uint32_t ptr = ctx.r3.u32;
    std::shared_ptr<DispatcherObject> obj;
    {
        std::lock_guard<std::mutex> g(g_threadMapLock);
        if (auto it = g_threadsByKthread.find(ptr); it != g_threadsByKthread.end()) obj = it->second;
    }
    if (!obj) { obj = std::make_shared<DispatcherObject>(); obj->guest = ptr; }
    if (ctx.r4.u32) Store32(ctx.r4.u32, CreateHandle(obj));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ObLookupThreadByThreadId(threadId, void** out)
IMPORT(ObLookupThreadByThreadId)
{
    std::lock_guard<std::mutex> g(g_threadMapLock);
    auto it = g_threadsById.find(ctx.r3.u32);
    if (it == g_threadsById.end()) { ctx.r3.u64 = STATUS_INVALID_PARAMETER; return; }
    Store32(ctx.r4.u32, it->second->guest);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtDuplicateObject(handle, u32* newHandle, options)
IMPORT(NtDuplicateObject)
{
    auto obj = LookupHandle(ctx.r3.u32);
    if (!obj && ctx.r3.u32 == kCurrentThreadHandle)
    {
        std::lock_guard<std::mutex> g(g_threadMapLock);
        if (auto it = g_threadsByKthread.find(t_kthread); it != g_threadsByKthread.end()) obj = it->second;
    }
    if (!obj) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    Store32(ctx.r4.u32, CreateHandle(obj));
    if (ctx.r5.u32 & 1) CloseHandle(ctx.r3.u32);   // DUPLICATE_CLOSE_SOURCE
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---- Spinlocks ----------------------------------------------------------------------------------
// KSPIN_LOCK is a u32 in guest memory: 0 = free, otherwise the owner. Real atomics on the
// guest word, so title code that also touches it with lwarx/stwcx stays coherent.
static void SpinAcquire(uint32_t lock)
{
    auto* p = (volatile long*)GuestPtr(lock);
    long owner = long(__builtin_bswap32(t_kthread ? t_kthread : 1));
    for (int spins = 0;; spins++)
    {
        if (*p == 0 && _InterlockedCompareExchange(p, owner, 0) == 0) return;
        if (spins > 64) std::this_thread::yield();
        else _mm_pause();
    }
}
static void SpinRelease(uint32_t lock)
{
    _InterlockedExchange((volatile long*)GuestPtr(lock), 0);
}

// u32 KfAcquireSpinLock(KSPIN_LOCK*) -> previous IRQL ; KfReleaseSpinLock(KSPIN_LOCK*, irql)
IMPORT(KfAcquireSpinLock) { SpinAcquire(ctx.r3.u32); ctx.r3.u64 = 0; }
IMPORT(KfReleaseSpinLock) { SpinRelease(ctx.r3.u32); }
IMPORT(KeAcquireSpinLockAtRaisedIrql) { SpinAcquire(ctx.r3.u32); }
IMPORT(KeReleaseSpinLockFromRaisedIrql) { SpinRelease(ctx.r3.u32); }
IMPORT(KeLockL2) {}
IMPORT(KeUnlockL2) {}

// ---- Exceptions / DPCs / APCs --------------------------------------------------------------
// RtlRaiseException(EXCEPTION_RECORD*): code, flags, record, address, numParams, info[]
IMPORT(RtlRaiseException)
{
    uint32_t rec = ctx.r3.u32;
    uint32_t code = Load32(rec);
    if (code == 0x406D1388 && Load32(rec + 0x14) == 0x1000)
    {
        // MSVC "set thread name" convention: info[1] = name, info[2] = thread id
        Log("[thread] name for id 0x%X: \"%s\"", Load32(rec + 0x1C), LoadCString(Load32(rec + 0x18), 64).c_str());
        return;
    }
    Log("[RtlRaiseException] code 0x%08X flags 0x%X address 0x%08X (not handled)", code, Load32(rec + 4), Load32(rec + 0xC));
    if (code == 0xE06D7363 && Load32(rec + 0x10) >= 3)
    {
        // MSVC C++ throw: info[0] magic, info[1] thrown object, info[2] ThrowInfo.
        // ThrowInfo { attrs, unwind, forwardCompat, CatchableTypeArray* }
        // CatchableTypeArray { count, CatchableType* [] }; CatchableType { props, TypeDescriptor*, ... }
        // TypeDescriptor { vftable, spare, char name[] }
        uint32_t obj = Load32(rec + 0x18), info = Load32(rec + 0x1C);
        uint32_t cta = info ? Load32(info + 12) : 0;
        uint32_t count = cta ? Load32(cta) : 0;
        for (uint32_t i = 0; i < count && i < 8; i++)
        {
            uint32_t ct = Load32(cta + 4 + i * 4);
            uint32_t td = ct ? Load32(ct + 4) : 0;
            if (td) Log("[RtlRaiseException]   C++ type: %s", LoadCString(td + 8, 128).c_str());
        }
        // std::exception: { vftable; const char* what; bool doFree }
        uint32_t what = obj ? Load32(obj + 4) : 0;
        if (what >= 0x40000000 && what < 0xC0000000 || (what >= 0x82000000 && what < 0x82DF0000))
            Log("[RtlRaiseException]   what(): \"%s\"", LoadCString(what, 200).c_str());
        Log("[RtlRaiseException]   thrown from:");
        LogGuestBacktrace();
    }
}

// KDPC: we only need the routine and context. KeInitializeDpc(dpc, routine, context)
static std::mutex g_dpcLock;
static std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> g_dpcs;

IMPORT(KeInitializeDpc)
{
    std::lock_guard<std::mutex> g(g_dpcLock);
    g_dpcs[ctx.r3.u32] = { ctx.r4.u32, ctx.r5.u32 };
}

// KeInsertQueueDpc(dpc, arg1, arg2): run the routine(dpc, context, arg1, arg2) right away
// on the calling thread, reusing its stack below the current frame.
IMPORT(KeInsertQueueDpc)
{
    std::pair<uint32_t, uint32_t> d;
    {
        std::lock_guard<std::mutex> g(g_dpcLock);
        auto it = g_dpcs.find(ctx.r3.u32);
        if (it == g_dpcs.end()) { ctx.r3.u64 = 0; return; }
        d = it->second;
    }
    PPCContext c = ctx;
    c.r3.u64 = ctx.r3.u32;
    c.r4.u64 = d.second;
    c.r5.u64 = ctx.r4.u32;
    c.r6.u64 = ctx.r5.u32;
    CallGuest(c, d.first);
    ctx.r3.u64 = 1;
}

IMPORT(KiApcNormalRoutineNop) {}
