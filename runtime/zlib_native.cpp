// Native zlib for the title's inflate() (loading hitches, see NEXT_STEPS.md).
//
// The title links zlib 1.1.3; decompressing assets in recompiled code costs 100-200 ms frames
// while loading (inflate_fast 0x82500300 on thread 11). Every use goes through inflate() at
// 0x8248A988, so that one function is replaced: each guest z_stream gets a host z_stream that
// does the work directly on guest memory, and the guest struct's public fields are updated.
// The guest's own zlib state is never advanced, which is fine because nothing but inflate()
// reads it. Opt-in with PEGGLE2_NATIVE_ZLIB=1: works (streams decode, no errors), but in a
// first A/B on this machine it did not reduce loading spikes (85 slow frames with the title's
// zlib, 110 with native, one 60 s menu run each), and the spike profile showed render-thread
// driver waits and texture copies rather than inflate during those frames.
//
// Guest z_stream (zlib 1.1.3, 32-bit, big-endian): +0 next_in, +4 avail_in, +8 total_in,
// +12 next_out, +16 avail_out, +20 total_out, +24 msg, +28 state, ..., +48 adler.
// Guest internal_state: +0 mode, +4 sub (8 bytes), +12 nowrap, +16 wbits.
#include "runtime.h"

#ifdef PEGGLE2_HAVE_ZLIB

#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <zlib.h>

namespace
{
    struct HostStream
    {
        z_stream s{};
        bool live = false;
        ~HostStream() { if (live) inflateEnd(&s); }
    };

    std::mutex g_lock;
    std::unordered_map<uint32_t, std::unique_ptr<HostStream>> g_streams;   // guest z_stream -> host
    std::atomic<uint64_t> g_calls{ 0 }, g_bytesOut{ 0 }, g_streamsCreated{ 0 };

    bool Enabled()
    {
        static const bool on = [] {
            const char* v = getenv("PEGGLE2_NATIVE_ZLIB");
            bool enabled = v && v[0] == '1';
            Log("[zlib] native inflate %s (zlib %s; PEGGLE2_NATIVE_ZLIB=1 to enable)", enabled ? "enabled" : "disabled", zlibVersion());
            return enabled;
        }();
        return on;
    }
}

PPC_FUNC_IMPL(__imp__sub_8248A988);
PPC_FUNC(sub_8248A988)
{
    uint32_t z = ctx.r3.u32;
    int flush = int(ctx.r4.s32);
    uint32_t state = z ? Load32(z + 28) : 0;
    if (!Enabled() || !z || !state)
    {
        __imp__sub_8248A988(ctx, base);   // let the title's zlib report the error
        return;
    }

    HostStream* hs;
    {
        std::lock_guard<std::mutex> g(g_lock);
        auto& slot = g_streams[z];
        // New stream, or the title re-initialised/reset this z_stream (total_in back to 0).
        if (!slot || (Load32(z + 8) == 0 && slot->s.total_in != 0))
        {
            slot = std::make_unique<HostStream>();
            int nowrap = int(Load32(state + 12)), wbits = int(Load32(state + 16));
            if (wbits < 8 || wbits > 15) wbits = 15;
            if (inflateInit2(&slot->s, nowrap ? -wbits : wbits) != Z_OK)
            {
                g_streams.erase(z);
                __imp__sub_8248A988(ctx, base);
                return;
            }
            slot->live = true;
            uint64_t n = ++g_streamsCreated;
            if (n <= 3 || n % 500 == 0)
                Log("[zlib] host stream #%llu for guest z_stream 0x%08X (%s, window 2^%d)", (unsigned long long)n, z,
                    nowrap ? "raw" : "zlib header", wbits);
        }
        hs = slot.get();
    }

    z_stream& s = hs->s;
    uint32_t inAddr = Load32(z + 0), inLen = Load32(z + 4);
    uint32_t outAddr = Load32(z + 12), outLen = Load32(z + 16);
    s.next_in = inLen ? GuestPtr(inAddr) : nullptr;
    s.avail_in = inLen;
    s.next_out = outLen ? GuestPtr(outAddr) : nullptr;
    s.avail_out = outLen;

    int ret = inflate(&s, flush);

    uint32_t consumed = inLen - s.avail_in, produced = outLen - s.avail_out;
    Store32(z + 0, inAddr + consumed);
    Store32(z + 4, s.avail_in);
    Store32(z + 8, Load32(z + 8) + consumed);
    Store32(z + 12, outAddr + produced);
    Store32(z + 16, s.avail_out);
    Store32(z + 20, Load32(z + 20) + produced);
    Store32(z + 48, uint32_t(s.adler));
    if (ret == Z_NEED_DICT) Log("[zlib] stream 0x%08X needs a preset dictionary (not supported natively)", z);
    if (ret < 0 && ret != Z_BUF_ERROR) Log("[zlib] inflate error %d on guest z_stream 0x%08X (%s)", ret, z, s.msg ? s.msg : "");

    uint64_t calls = ++g_calls;
    uint64_t out = (g_bytesOut += produced);
    if (calls % 2000 == 0)
        Log("[zlib] %llu inflate calls, %.1f MB decompressed natively", (unsigned long long)calls, out / 1048576.0);
    ctx.r3.s64 = ret;
}

#endif
