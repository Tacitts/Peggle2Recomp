// XMsg: messages from the title to system "apps" (XMsgInProcessCall / XMsgStartIORequest[Ex]).
//
// Only the dashboard music player (XMP, app 0xFA) matters so far: the title asks who controls
// playback and takes control for its own soundtrack. If that request never completes (or the
// "controller changed" notification never arrives), the title keeps its music ducked. The
// player here never plays anything: idle, volume 1.0, and the title in control.
// Message layouts follow Xenia's documentation of the XMP app (big-endian u32 fields).
// Other apps' messages complete successfully without doing anything.
#include "runtime.h"
#include "achievements.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
    constexpr uint32_t kAppXmp = 0xFA;
    constexpr uint32_t X_E_SUCCESS = 0, X_E_FAIL = 0x80004005;
    constexpr uint32_t kNotifyPlaybackBehaviorChanged = 0x0A000002;
    constexpr uint32_t kNotifyPlaybackControllerChanged = 0x0A000003;

    std::mutex g_lock;
    uint32_t g_playbackClient = 1;     // 0 system (user's music), 1 title
    uint32_t g_volume = 0x3F800000;    // 1.0f
    uint32_t g_playbackMode = 0, g_repeatMode = 0, g_behaviorFlags = 0;

    void StoreIf(uint32_t ptr, uint32_t value) { if (ptr) Store32(ptr, value); }

    uint32_t XmpMessage(uint32_t message, uint32_t buf)
    {
        std::lock_guard<std::mutex> g(g_lock);
        auto arg = [&](uint32_t i) { return buf ? Load32(buf + i * 4) : 0u; };
        switch (message)
        {
        case 0x00070003:   // continue
        case 0x00070004:   // stop
        case 0x00070005:   // pause
            return X_E_SUCCESS;
        case 0x00070008:   // set playback behaviour {client, mode, repeat, flags}
            g_playbackMode = arg(1);
            g_repeatMode = arg(2);
            g_behaviorFlags = arg(3);
            PostNotification(kNotifyPlaybackBehaviorChanged, 0);
            return X_E_SUCCESS;
        case 0x00070009:   // get status {client, state*}: idle
            StoreIf(arg(1), 0);
            return X_E_SUCCESS;
        case 0x0007000B:   // get volume {client, float*}
            StoreIf(arg(1), g_volume);
            return X_E_SUCCESS;
        case 0x0007000C:   // set volume {client, float}
            g_volume = arg(1);
            return X_E_SUCCESS;
        case 0x0007001A:   // set playback controller {client, controller, playback client}
            g_playbackClient = arg(2);
            Log("[xmsg] music player: playback controller -> %s", g_playbackClient ? "title" : "system");
            PostNotification(kNotifyPlaybackControllerChanged, g_playbackClient ? 0 : 1);
            return X_E_SUCCESS;
        case 0x0007001B:   // get playback controller {client, controller*, locked*}
            StoreIf(arg(1), 0);
            StoreIf(arg(2), 0);
            return X_E_SUCCESS;
        case 0x00070029:   // get playback behaviour {client, mode*, repeat*, flags*}
            StoreIf(arg(1), g_playbackMode);
            StoreIf(arg(2), g_repeatMode);
            StoreIf(arg(3), g_behaviorFlags);
            return X_E_SUCCESS;
        default:
        {
            static int logged = 0;
            if (logged++ < 10) Log("[xmsg] music player message 0x%08X not handled", message);
            return X_E_FAIL;
        }
        }
    }

    // XGI app (0xFB): XUserWriteAchievements arrives as message 0x000B0008 with a buffer
    // { u32 count, u32 achievements } -> count x { u32 userIndex, u32 achievementId }.
    constexpr uint32_t kAppXgi = 0xFB, kXgiWriteAchievements = 0x000B0008;

    uint32_t Dispatch(uint32_t app, uint32_t message, uint32_t buf)
    {
        if (app == kAppXmp) return XmpMessage(message, buf);
        if (app == kAppXgi && message == kXgiWriteAchievements && buf)
        {
            uint32_t count = Load32(buf), list = Load32(buf + 4);
            for (uint32_t i = 0; i < count && i < 64 && list; i++) AwardAchievement(Load32(list + i * 8 + 4));
            return X_E_SUCCESS;
        }
        // Everything else succeeds without doing anything; log each kind once.
        static std::mutex seenLock;
        static std::vector<uint64_t> seen;
        {
            std::lock_guard<std::mutex> g(seenLock);
            uint64_t key = uint64_t(app) << 32 | message;
            if (std::find(seen.begin(), seen.end(), key) == seen.end() && seen.size() < 64)
            {
                seen.push_back(key);
                Log("[xmsg] app 0x%X message 0x%08X: not handled (buffer 0x%08X: %08X %08X %08X %08X)", app, message, buf,
                    buf ? Load32(buf) : 0, buf ? Load32(buf + 4) : 0, buf ? Load32(buf + 8) : 0, buf ? Load32(buf + 12) : 0);
            }
        }
        return X_E_SUCCESS;
    }
}

// u32 XMsgInProcessCall(u32 app, u32 message, u32 param1 (buffer), u32 param2 (length))
IMPORT(XMsgInProcessCall)
{
    ctx.r3.u64 = Dispatch(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}

// u32 XMsgStartIORequest(u32 app, u32 message, XOVERLAPPED*, u32 buffer, u32 length)
IMPORT(XMsgStartIORequest)
{
    uint32_t result = Dispatch(ctx.r3.u32, ctx.r4.u32, ctx.r6.u32);
    ctx.r3.u64 = ctx.r5.u32 ? CompleteOverlapped(ctx.r5.u32, result) : result;
}

// u32 XMsgStartIORequestEx(u32 app, u32 message, XOVERLAPPED*, u32 buffer, u32 length, u32 extra)
IMPORT(XMsgStartIORequestEx)
{
    uint32_t result = Dispatch(ctx.r3.u32, ctx.r4.u32, ctx.r6.u32);
    ctx.r3.u64 = ctx.r5.u32 ? CompleteOverlapped(ctx.r5.u32, result) : result;
}

// u32 XMsgCancelIORequest(XOVERLAPPED*, BOOL wait): requests complete immediately, nothing to cancel
IMPORT(XMsgCancelIORequest)
{
    ctx.r3.u64 = 0;
}
