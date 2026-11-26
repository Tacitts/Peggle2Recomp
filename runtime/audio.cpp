// Audio driver imports (XAudio render driver, XMA contexts).
//
// The console's audio driver calls every registered render-driver client once per
// 256-sample frame (48 kHz -> every 5.33 ms) from its own thread; the client mixes and
// hands back a frame with XAudioSubmitRenderDriverFrame. Here a dedicated guest thread does
// the calling; the frames of all clients for one tick are summed, downmixed to stereo and
// played through WASAPI (shared mode, Windows converts the rate if the device isn't 48 kHz).
#include "runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

// ---- Host output (WASAPI) ------------------------------------------------------------------------
namespace
{
    constexpr uint32_t kRate = 48000, kFrameSamples = 256, kGuestChannels = 6;
    std::mutex g_outLock;
    std::deque<float> g_out;                     // interleaved stereo, waiting for the device
    constexpr size_t kMaxQueued = kRate / 5 * 2; // 200 ms: beyond this, drop the oldest samples
    std::atomic<float> g_peak{ 0 };

    void PushStereo(const float* lr, size_t frames)
    {
        std::lock_guard<std::mutex> g(g_outLock);
        g_out.insert(g_out.end(), lr, lr + frames * 2);
        while (g_out.size() > kMaxQueued) g_out.pop_front();
    }

    void WasapiThread()
    {
        NameHostThread("wasapi");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        IAudioClient* client = nullptr;
        IAudioRenderClient* render = nullptr;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = kRate * 8;
        UINT32 bufferFrames = 0;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
            FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
            FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) ||
            FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      400000 /* 40 ms */, 0, &wf, nullptr)) ||
            FAILED(client->SetEventHandle(ev)) ||
            FAILED(client->GetBufferSize(&bufferFrames)) ||
            FAILED(client->GetService(IID_PPV_ARGS(&render))) ||
            FAILED(client->Start()))
        {
            Log("[audio] WASAPI output unavailable: sound is discarded");
            return;
        }
        Log("[audio] WASAPI output started (48 kHz stereo, %u-frame buffer)", bufferFrames);
        for (;;)
        {
            WaitForSingleObject(ev, 100);
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) continue;
            UINT32 free = bufferFrames - padding;
            if (!free) continue;
            BYTE* data = nullptr;
            if (FAILED(render->GetBuffer(free, &data))) continue;
            float* out = (float*)data;
            size_t have;
            {
                std::lock_guard<std::mutex> g(g_outLock);
                have = std::min<size_t>(g_out.size() / 2, free);
                for (size_t i = 0; i < have * 2; i++) out[i] = g_out[i];
                g_out.erase(g_out.begin(), g_out.begin() + have * 2);
            }
            for (size_t i = have * 2; i < size_t(free) * 2; i++) out[i] = 0.0f;   // underrun: silence
            render->ReleaseBuffer(free, 0);
        }
    }
}

namespace
{
    // The driver calls callback(argCell), where argCell is a guest pointer to a u32 holding
    // the registered argument (not the argument itself).
    struct Client { uint32_t callback, arg, argCell; bool active; };
    std::mutex g_lock;
    std::vector<Client> g_clients;
    std::once_flag g_threadOnce;
    std::atomic<uint64_t> g_framesRequested{ 0 }, g_framesSubmitted{ 0 };
    // Sum of the frames the clients submitted during the current tick (planar, 6 x 256).
    std::mutex g_mixLock;
    float g_mix[kGuestChannels * kFrameSamples];
    bool g_mixHasData = false;

    // Flush one tick: 6 channels (FL FR FC LFE BL BR) -> stereo, to the WASAPI queue.
    void FlushTick()
    {
        float lr[kFrameSamples * 2];
        float peak = 0;
        {
            std::lock_guard<std::mutex> g(g_mixLock);
            for (uint32_t i = 0; i < kFrameSamples; i++)
            {
                const float* c = g_mix;
                float fl = c[0 * kFrameSamples + i], fr = c[1 * kFrameSamples + i], fc = c[2 * kFrameSamples + i];
                float bl = c[4 * kFrameSamples + i], br = c[5 * kFrameSamples + i];
                float l = fl + 0.7071f * (fc + bl), r = fr + 0.7071f * (fc + br);
                lr[i * 2] = std::clamp(l, -1.0f, 1.0f);
                lr[i * 2 + 1] = std::clamp(r, -1.0f, 1.0f);
                peak = std::max({ peak, std::fabs(l), std::fabs(r) });
            }
            std::fill(std::begin(g_mix), std::end(g_mix), 0.0f);
            g_mixHasData = false;
        }
        if (peak > g_peak.load()) g_peak = peak;
        PushStereo(lr, kFrameSamples);
    }

    constexpr uint32_t kDriverMagic = 0x41550000;
    constexpr auto kFramePeriod = std::chrono::nanoseconds(1'000'000'000ll * 256 / 48000);

    void LogXmaContexts();
    std::vector<uint32_t> LiveXmaContexts();

    void AudioThread()
    {
        NameHostThread("audio");
        PPCContext ctx{};
        InitGuestThread(ctx, 0x10000, 0xA0D10);   // runtime-owned "audio driver" thread
        Log("[audio] driver thread started (frame every %.2f ms)", kFramePeriod.count() / 1e6);
        std::thread(WasapiThread).detach();
        auto next = std::chrono::steady_clock::now();
        auto nextPeakLog = next + std::chrono::seconds(10);
        for (;;)
        {
            next += kFramePeriod;
            std::vector<Client> clients;
            {
                std::lock_guard<std::mutex> g(g_lock);
                clients = g_clients;
            }
            XmaTick();   // hardware XMA decoding, before the clients mix
            for (auto& c : clients)
            {
                if (!c.active) continue;
                PPCContext call = ctx;
                call.r3.u64 = c.argCell;
                CallGuest(call, c.callback);
                g_framesRequested++;
            }
            FlushTick();
            if (std::chrono::steady_clock::now() >= nextPeakLog)
            {
                nextPeakLog += std::chrono::seconds(10);
                Log("[audio] peak level over the last 10 s: %.3f, XMA packets decoded: %llu", g_peak.exchange(0.0f),
                    (unsigned long long)XmaPacketsDecoded());
                LogXmaContexts();
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

// XAudioSubmitRenderDriverFrame(u32 driver, f32* samples /* 6 channels x 256, planar, big-endian */)
IMPORT(XAudioSubmitRenderDriverFrame)
{
    g_framesSubmitted++;
    if (uint32_t p = ctx.r4.u32)
    {
        std::lock_guard<std::mutex> g(g_mixLock);
        for (uint32_t i = 0; i < kGuestChannels * kFrameSamples; i++)
        {
            uint32_t bits = Load32(p + i * 4);
            float f;
            memcpy(&f, &bits, 4);
            if (std::isfinite(f)) g_mix[i] += f;
        }
        g_mixHasData = true;
    }
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
namespace
{
    std::mutex g_xmaLock;
    std::vector<uint32_t> g_xmaContexts;   // guest addresses of live contexts


    // Debugging aid: the live contexts' 10 dwords (Xenia's XMA_CONTEXT_DATA layout).
    void LogXmaContexts()
    {
        std::lock_guard<std::mutex> g(g_xmaLock);
        int shown = 0;
        for (uint32_t c : g_xmaContexts)
        {
            uint32_t d[10];
            bool any = false;
            for (int i = 0; i < 10; i++) { d[i] = Load32(c + i * 4); any |= d[i] != 0; }
            if (!any || shown++ >= 6) continue;
            Log("[xma] ctx %08X: %08X %08X %08X %08X %08X in0 %08X in1 %08X out %08X work %08X d9 %08X",
                c, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9]);
        }
        Log("[xma] %zu live contexts", g_xmaContexts.size());
    }
}

std::vector<uint32_t> LiveXmaContexts()
{
    std::lock_guard<std::mutex> g(g_xmaLock);
    return g_xmaContexts;
}

IMPORT(XMACreateContext)
{
    uint32_t c = AllocPhysical(0x40, 0x40);
    if (ctx.r3.u32) Store32(ctx.r3.u32, c);
    if (c)
    {
        memset(GuestPtr(c), 0, 0x40);
        std::lock_guard<std::mutex> g(g_xmaLock);
        g_xmaContexts.push_back(c);
    }
    ctx.r3.u64 = c ? 0 : 0x8007000E;
}
IMPORT(XMAReleaseContext)
{
    std::lock_guard<std::mutex> d(XmaDecoderLock());   // wait out a tick that may be writing to it
    std::lock_guard<std::mutex> g(g_xmaLock);
    g_xmaContexts.erase(std::remove(g_xmaContexts.begin(), g_xmaContexts.end(), ctx.r3.u32), g_xmaContexts.end());
    ctx.r3.u64 = 0;
}
