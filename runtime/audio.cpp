// Audio driver imports (XAudio render driver, XMA contexts).
//
// The console's audio driver calls every registered render-driver client once per
// 256-sample frame (48 kHz -> every 5.33 ms) from its own thread; the client mixes and
// hands back a frame with XAudioSubmitRenderDriverFrame. Here a dedicated guest thread does
// the calling and submitted frames are discarded (silent output) until a host audio
// backend exists.
#include "runtime.h"

#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    // The driver calls callback(argCell), where argCell is a guest pointer to a u32 holding
    // the registered argument (not the argument itself).
    struct Client { uint32_t callback, arg, argCell; bool active; };
    std::mutex g_lock;
    std::vector<Client> g_clients;
    std::once_flag g_threadOnce;
    std::atomic<uint64_t> g_framesRequested{ 0 }, g_framesSubmitted{ 0 };

    constexpr uint32_t kDriverMagic = 0x41550000;
    constexpr auto kFramePeriod = std::chrono::nanoseconds(1'000'000'000ll * 256 / 48000);

    void AudioThread()
    {
        PPCContext ctx{};
        InitGuestThread(ctx, 0x10000, 0xA0D10);   // runtime-owned "audio driver" thread
        Log("[audio] driver thread started (frame every %.2f ms)", kFramePeriod.count() / 1e6);
        auto next = std::chrono::steady_clock::now();
        for (;;)
        {
            next += kFramePeriod;
            std::vector<Client> clients;
            {
                std::lock_guard<std::mutex> g(g_lock);
                clients = g_clients;
            }
            for (auto& c : clients)
            {
                if (!c.active) continue;
                PPCContext call = ctx;
                call.r3.u64 = c.argCell;
                CallGuest(call, c.callback);
                g_framesRequested++;
            }
            // Log on crossing thresholds (several clients advance the counter per tick).
            static uint64_t lastBucket = 0;
            uint64_t n = g_framesRequested.load();
            uint64_t bucket = n < 1000 ? (n ? 1 : 0) : 2 + n / 60000;
            if (bucket != lastBucket)
            {
                lastBucket = bucket;
                Log("[audio] %llu frames requested, %llu submitted (%zu clients)", (unsigned long long)n,
                    (unsigned long long)g_framesSubmitted.load(), clients.size());
            }
            auto now = std::chrono::steady_clock::now();
            if (next > now) std::this_thread::sleep_until(next);
            else if (now - next > std::chrono::milliseconds(100)) next = now;   // fell behind: resync
        }
    }
}

// XAudioRegisterRenderDriverClient(u32* callback /* {u32 fn, u32 arg} */, u32* driverOut)
IMPORT(XAudioRegisterRenderDriverClient)
{
    uint32_t cb = ctx.r3.u32, out = ctx.r4.u32;
    Client c{ Load32(cb), Load32(cb + 4), AllocRuntime(4), true };
    Store32(c.argCell, c.arg);
    uint32_t index;
    {
        std::lock_guard<std::mutex> g(g_lock);
        index = uint32_t(g_clients.size());
        g_clients.push_back(c);
    }
    Log("[audio] render driver client %u: callback 0x%08X(0x%08X)", index, c.callback, c.arg);
    if (out) Store32(out, kDriverMagic | index);
    std::call_once(g_threadOnce, [] { std::thread(AudioThread).detach(); });
    ctx.r3.u64 = 0;
}

// XAudioUnregisterRenderDriverClient(u32 driver)
IMPORT(XAudioUnregisterRenderDriverClient)
{
    uint32_t index = ctx.r3.u32 & 0xFFFF;
    std::lock_guard<std::mutex> g(g_lock);
    if (index < g_clients.size()) g_clients[index].active = false;
    ctx.r3.u64 = 0;
}

// XAudioSubmitRenderDriverFrame(u32 driver, f32* samples /* 256 x 6 channels, big-endian */)
IMPORT(XAudioSubmitRenderDriverFrame)
{
    g_framesSubmitted++;
    ctx.r3.u64 = 0;
}

// XAudioGetSpeakerConfig(u32* config): stereo
IMPORT(XAudioGetSpeakerConfig)
{
    if (ctx.r3.u32) Store32(ctx.r3.u32, 0x00010001);
    ctx.r3.u64 = 0;
}

// XAudioGetVoiceCategoryVolume(u32 category, f32* volume): full volume
IMPORT(XAudioGetVoiceCategoryVolume)
{
    if (ctx.r4.u32) Store32(ctx.r4.u32, 0x3F800000);
    ctx.r3.u64 = 0;
}

IMPORT(XAudioEnableDucker) { ctx.r3.u64 = 0; }
IMPORT(XAudioGetDuckerLevel) { if (ctx.r3.u32) Store32(ctx.r3.u32, 0x3F800000); ctx.r3.u64 = 0; }
IMPORT(XAudioGetDuckerThreshold) { if (ctx.r3.u32) Store32(ctx.r3.u32, 0); ctx.r3.u64 = 0; }
IMPORT(XAudioGetDuckerAttackTime) { if (ctx.r3.u32) Store32(ctx.r3.u32, 0); ctx.r3.u64 = 0; }
IMPORT(XAudioGetDuckerReleaseTime) { if (ctx.r3.u32) Store32(ctx.r3.u32, 0); ctx.r3.u64 = 0; }
IMPORT(XAudioGetDuckerHoldTime) { if (ctx.r3.u32) Store32(ctx.r3.u32, 0); ctx.r3.u64 = 0; }

// ---- XMA ------------------------------------------------------------------------------------
// XMACreateContext(u32* contextOut): a 64-byte hardware context the title fills in and the
// XMA decoder consumes. No decoder yet: contexts are plain memory.
IMPORT(XMACreateContext)
{
    uint32_t c = AllocPhysical(0x40, 0x40);
    if (ctx.r3.u32) Store32(ctx.r3.u32, c);
    ctx.r3.u64 = c ? 0 : 0x8007000E;
}
IMPORT(XMAReleaseContext) { ctx.r3.u64 = 0; }
