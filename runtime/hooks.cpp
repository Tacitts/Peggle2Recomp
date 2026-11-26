// Guest function hooks (XenonRecomp's override mechanism).
//
// Every recompiled function sub_X is a weak alias of __imp__sub_X. Defining a strong sub_X
// here replaces it everywhere; calling __imp__sub_X runs the original code.
//
// Debugging recipe (see README "Debugging aids"):
//   TRACE_FUNC(addr, "label")      log arguments and return value of a guest function
//   WatchGuestWrite(addr)          hardware write watchpoint (logs value + guest call chain)
//   LogGuestBacktrace()            guest call chain from inside a hook
// Crash reports name the exact guest instruction per frame (PDB line -> "// XXXXXXXX insn").
// Note: a hooked function's original body (__imp__sub_X) may be attributed in call chains
// to the guest function that precedes it, because the function table points at the hook.
#include "runtime.h"
#include "xenos.h"
#include <algorithm>
#include <atomic>
#include <chrono>

#define TRACE_FUNC(addr, label)                                                              \
    PPC_FUNC_IMPL(__imp__sub_##addr);                                                         \
    PPC_FUNC(sub_##addr)                                                                      \
    {                                                                                         \
        uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32;          \
        __imp__sub_##addr(ctx, base);                                                         \
        Log("[trace] %s sub_%s(0x%08X, 0x%08X, 0x%08X, 0x%08X) -> 0x%08X", label, #addr,     \
            a3, a4, a5, a6, ctx.r3.u32);                                                      \
    }

// Shader capture: functions that check the container magic 0x102A11xx (found with
// tools/analysis/find_lis.py). Before each runs, any pointer argument that points at a
// container is saved to build/runtime/shaders/.
#define CAPTURE_SHADER_ARGS(addr)                                                             \
    PPC_FUNC_IMPL(__imp__sub_##addr);                                                         \
    PPC_FUNC(sub_##addr)                                                                      \
    {                                                                                         \
        SaveShaderContainer(ctx.r3.u32, "sub_" #addr " r3") ||                                \
        SaveShaderContainer(ctx.r4.u32, "sub_" #addr " r4") ||                                \
        SaveShaderContainer(ctx.r5.u32, "sub_" #addr " r5") ||                                \
        SaveShaderContainer(ctx.r6.u32, "sub_" #addr " r6");                                  \
        __imp__sub_##addr(ctx, base);                                                         \
    }

CAPTURE_SHADER_ARGS(82615118)
CAPTURE_SHADER_ARGS(82635B48)
CAPTURE_SHADER_ARGS(82636C60)
CAPTURE_SHADER_ARGS(82660780)
CAPTURE_SHADER_ARGS(82673CB0)
CAPTURE_SHADER_ARGS(826F2AE8)
CAPTURE_SHADER_ARGS(8272EB90)
CAPTURE_SHADER_ARGS(827CBBC0)

// The game's format-into-string helper (r3 = destination std::string, r4 = format, r5.. = args).
// Calls whose format is a printable string in the image are mirrored into our log as [game]
// lines (this is how "Startup failed. Reason=10" was found).
PPC_FUNC_IMPL(__imp__sub_82453B78);
PPC_FUNC(sub_82453B78)
{
    bool inImage = ctx.r4.u32 >= 0x82000000 && ctx.r4.u32 < 0x82DF0000;
    std::string fmt = inImage ? LoadCString(ctx.r4.u32, 512) : std::string();
    bool printable = fmt.size() >= 4;
    for (char ch : fmt)
        if (((unsigned char)ch < 0x20 && ch != '\n' && ch != '\t') || (unsigned char)ch >= 0x7F) printable = false;
    if (printable)
    {
        uint32_t args[4] = { ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32 };
        std::string out;
        int ai = 0;
        for (size_t i = 0; i < fmt.size(); i++)
        {
            if (fmt[i] != '%' || i + 1 >= fmt.size()) { out.push_back(fmt[i]); continue; }
            char c = fmt[++i];
            if (c == '%') { out.push_back('%'); continue; }
            uint32_t a = ai < 4 ? args[ai++] : 0;
            char buf[64];
            if (c == 's') out += LoadCString(a, 256);
            else if (c == 'x' || c == 'X') { snprintf(buf, sizeof(buf), "%x", a); out += buf; }
            else { snprintf(buf, sizeof(buf), "%d", int32_t(a)); out += buf; }
        }
        Log("[game] %s", out.c_str());
    }
    __imp__sub_82453B78(ctx, base);
}

// ---- Flip pacing ---------------------------------------------------------------------------
// The title's D3D counts flips at device+16976: its vblank handler (0x8261EF88, r3 = device)
// flips queued frames whose target vblank has come, and the flip-request handler (0x8261F0B8,
// GPU interrupt) flips at once when a frame is due now. Each increase is one frame flipped,
// in order; the renderer then presents the next submitted frame (render.cpp).
static std::atomic<uint32_t> g_d3dDevice{ 0 };
static uint32_t g_flipCount = 0;   // GPU thread only (both handlers run on it)
static void NoteFlips()
{
    uint32_t dev = g_d3dDevice.load(std::memory_order_relaxed);
    if (!dev) return;
    uint32_t n = Load32(dev + 16976);
    for (uint32_t i = g_flipCount; i != n && n - i < 8; i++) RendererGuestFlip();
    g_flipCount = n;
}
PPC_FUNC_IMPL(__imp__sub_8261EF88);
PPC_FUNC(sub_8261EF88)
{
    if (!g_d3dDevice.load(std::memory_order_relaxed))
    {
        g_d3dDevice = ctx.r3.u32;
        g_flipCount = Load32(ctx.r3.u32 + 16976);
    }
    __imp__sub_8261EF88(ctx, base);
    NoteFlips();
}

// ---- Optional 60 fps: PEGGLE2_60FPS=1 -----------------------------------------------------------
// The title runs its logic at a fixed 60 updates per second and draws every second update: the
// frame step (0x8242F8A8, loop mode byte 0x828CE748 = 1) calls the update (0x8242F2C8) and the
// per-update tick (0x8243CE30, fixed step in f31) twice, then draws, and every flip request is
// 0x205 (swap interval 2: bits 8-11; tear threshold 5% in the low byte), so 30 fps on a 60 Hz
// display. With PEGGLE2_60FPS set, the second update pair is skipped and the flip handler
// (0x8261F0B8, called from the GPU interrupt) sees interval 1: one update per drawn frame, the
// same 60 updates per second, drawn at 60 fps. If a frame takes longer than a vblank the game
// slows down (as a 60 fps console game would) instead of skipping.
static bool Sixty()
{
    static const bool on = getenv("PEGGLE2_60FPS") && atoi(getenv("PEGGLE2_60FPS")) != 0;
    return on;
}
PPC_FUNC_IMPL(__imp__sub_8261F0B8);
PPC_FUNC(sub_8261F0B8)
{
    if (Sixty() && ((ctx.r3.u32 >> 8) & 0xF) > 1)
        ctx.r3.u64 = (ctx.r3.u32 & ~0xF00u) | 0x100u;
    // Tear window (low byte, percent of a vblank): a frame that is due but arrives more than 5%
    // into the current vblank waits for the next one, to avoid tearing on the console. Our
    // presentation is always vsynced (no tearing possible) and adds its own vblank of latency,
    // so that wait only made late movie frames later and pushed the following flips back.
    // 100% flips a late frame at once. PEGGLE2_TEAR_WINDOW=<percent> overrides (5 = console).
    static const uint32_t tearWindow = [] {
        const char* v = getenv("PEGGLE2_TEAR_WINDOW");
        return v ? uint32_t(std::clamp(atoi(v), 0, 100)) : 100u;
    }();
    ctx.r3.u64 = (ctx.r3.u32 & ~0xFFu) | tearWindow;
    __imp__sub_8261F0B8(ctx, base);
    NoteFlips();   // an immediate flip
}
PPC_FUNC_IMPL(__imp__sub_8242F2C8);
PPC_FUNC(sub_8242F2C8)
{
    if (Sixty() && uint32_t(ctx.lr) == 0x8242FA0C) return;   // second update of the frame step
    __imp__sub_8242F2C8(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_8243CE30);
PPC_FUNC(sub_8243CE30)
{
    if (Sixty() && uint32_t(ctx.lr) == 0x8242FA34) return;   // its per-update tick
    __imp__sub_8243CE30(ctx, base);
}

// ---- Native memcpy ----------------------------------------------------------------------------
// 0x825F0AE0 is the title's memcpy (r3 dest, r4 src, r5 length; returns dest): 290 PowerPC
// instructions with 8/32-byte paths and dcbt hints, called from 1,083 places, and the hottest
// function in loading spikes. Guest memory is one flat block, so a host copy does the same job.
// memmove keeps overlapping copies well-defined (the title's routine copies forward in chunks).
PPC_FUNC_IMPL(__imp__sub_825F0AE0);
PPC_FUNC(sub_825F0AE0)
{
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, len = ctx.r5.u32;
    if (len && dst != src) memmove(GuestPtr(dst), GuestPtr(src), len);
    // r3 (the return value) is already the destination.
}
