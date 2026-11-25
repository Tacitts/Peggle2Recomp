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

// ---- Sound engine: random-container index past the end of its list ------------------------------
// Crash at the main menu (about 1 run in 4 when the intro is skipped): the sound engine's
// random container (Wwise, worker thread) picks an item by walking its list until it has passed
// N eligible items, N = rand() % (its running count of eligible items, +12 of the container).
// When that running count disagrees with the per-item flags, the walk runs past the list:
// 0x825A5C68 is then called with index 6 for a 4-entry array (descriptor in r6: {array, count})
// and reads a null entry at 0x825A5D38. Root cause not found yet (lost update between threads?).
// Mitigation: wrap such an index back into the list, so a valid variation plays instead.
PPC_FUNC_IMPL(__imp__sub_825A5C68);
PPC_FUNC(sub_825A5C68)
{
    uint32_t desc = ctx.r6.u32, idx = ctx.r5.u32 & 0xFFFF;
    uint32_t count = desc ? Load32(desc + 4) : 0;
    if (count && count < 0x10000 && idx >= count)
    {
        static std::atomic<int> logged{ 0 };
        if (logged++ < 10)
            Log("[sound] random container index %u past its %u-entry list (container %08X): wrapped to %u",
                idx, count, ctx.r7.u32, idx % count);
        ctx.r5.u64 = (ctx.r5.u64 & ~0xFFFFull) | (idx % count);
    }
    __imp__sub_825A5C68(ctx, base);
}
