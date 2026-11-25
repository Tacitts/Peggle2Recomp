// Controller input (XamInput*). Guest controller 1 is always connected and merges:
//   - the first connected host XInput controller (same XINPUT_GAMEPAD layout and button bits),
//   - the keyboard, while a window of this process has focus (see kKeyMap),
//   - PEGGLE2_AUTOPRESS (debug pulses of A/Start).
// Guest controllers 2-4 are not connected.
#include "runtime.h"

#include <cstdlib>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

namespace
{
    // Keyboard -> gamepad. Buttons use the XINPUT_GAMEPAD_* bits.
    struct KeyButton { int vk; uint16_t button; };
    constexpr KeyButton kKeyMap[] = {
        { VK_UP, 0x0001 }, { VK_DOWN, 0x0002 }, { VK_LEFT, 0x0004 }, { VK_RIGHT, 0x0008 },  // D-pad
        { VK_RETURN, 0x0010 }, { 'P', 0x0010 },                                             // Start
        { VK_TAB, 0x0020 }, { VK_BACK, 0x0020 },                                            // Back
        { 'Q', 0x0100 }, { 'E', 0x0200 },                                                   // LB / RB
        { VK_SPACE, 0x1000 }, { 'Z', 0x1000 },                                              // A (fire / confirm)
        { VK_ESCAPE, 0x2000 }, { 'X', 0x2000 },                                             // B (back)
        { 'C', 0x4000 }, { 'V', 0x8000 },                                                   // X / Y
    };
    // WASD: left stick (aim), IJKL: right stick, Shift / Ctrl: left / right trigger.

    // Set from the game window's WM_KEYDOWN/WM_KEYUP (render.cpp), cleared when it loses focus.
    // g_tapped latches a key-down until the next poll, so a tap shorter than one game frame
    // is still seen as one press.
    std::atomic<bool> g_keys[256], g_tapped[256];
    bool Down(int vk)
    {
        return vk >= 0 && vk < 256 && (g_keys[vk].load(std::memory_order_relaxed) || g_tapped[vk].load(std::memory_order_relaxed));
    }
    int16_t Axis(int neg, int pos) { return int16_t((Down(pos) ? 32767 : 0) - (Down(neg) ? 32767 : 0)); }

    // Larger magnitude wins per axis, so keyboard and controller can be used together.
    int16_t MergeAxis(int16_t a, int16_t b) { return std::abs(int(a)) >= std::abs(int(b)) ? a : b; }

    XINPUT_GAMEPAD ReadHostPad()
    {
        XINPUT_GAMEPAD pad{};
        // Polling a disconnected slot is slow, so only re-scan for a controller every 2 s.
        static int slot = -1;
        static ULONGLONG nextScan = 0;
        XINPUT_STATE st{};
        if (slot >= 0 && XInputGetState(slot, &st) == ERROR_SUCCESS)
            pad = st.Gamepad;
        else if (GetTickCount64() >= nextScan)
        {
            int prev = slot;
            slot = -1;
            nextScan = GetTickCount64() + 2000;
            for (int i = 0; i < 4; i++)
                if (XInputGetState(i, &st) == ERROR_SUCCESS) { slot = i; pad = st.Gamepad; break; }
            if (slot != prev) Log("[input] host controller %s", slot >= 0 ? "connected" : "not found");
        }

        for (auto& k : kKeyMap) if (Down(k.vk)) pad.wButtons |= k.button;
        pad.sThumbLX = MergeAxis(pad.sThumbLX, Axis('A', 'D'));
        pad.sThumbLY = MergeAxis(pad.sThumbLY, Axis('S', 'W'));
        pad.sThumbRX = MergeAxis(pad.sThumbRX, Axis('J', 'L'));
        pad.sThumbRY = MergeAxis(pad.sThumbRY, Axis('K', 'I'));
        if (Down(VK_SHIFT)) pad.bLeftTrigger = 255;
        if (Down(VK_CONTROL)) pad.bRightTrigger = 255;
        for (auto& t : g_tapped) t.store(false, std::memory_order_relaxed);
        return pad;
    }

    constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 0x48F;
    std::atomic<uint32_t> g_packet{ 0 };

    // Debug aid for shader capture: with PEGGLE2_AUTOPRESS set, pulse A and Start alternately
    // (pressed 150 ms out of every 1.5 s) so the game advances past the intro and through menus
    // without a controller. Returns the buttons held right now.
    uint16_t AutoPressButtons()
    {
        static const bool enabled = getenv("PEGGLE2_AUTOPRESS") != nullptr;
        if (!enabled)
            return 0;
        static const auto start = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (ms % 1500 >= 150)
            return 0;
        return ((ms / 1500) % 2) ? 0x0010 /* START */ : 0x1000 /* A */;
    }

    // X_INPUT_GAMEPAD: u16 buttons, u8 leftTrigger, u8 rightTrigger, s16 lx, ly, rx, ry
    void ReadPad(uint32_t pad)
    {
        XINPUT_GAMEPAD p = ReadHostPad();
        p.wButtons |= AutoPressButtons();

        // The packet number changes whenever the state does (titles use it to skip work).
        static std::mutex lock;
        static XINPUT_GAMEPAD last{};
        {
            std::lock_guard<std::mutex> g(lock);
            if (memcmp(&p, &last, sizeof(p)) != 0) { last = p; ++g_packet; }
        }
        Store16(pad + 0, p.wButtons);
        Store8(pad + 2, p.bLeftTrigger);
        Store8(pad + 3, p.bRightTrigger);
        Store16(pad + 4, uint16_t(p.sThumbLX));
        Store16(pad + 6, uint16_t(p.sThumbLY));
        Store16(pad + 8, uint16_t(p.sThumbRX));
        Store16(pad + 10, uint16_t(p.sThumbRY));
    }
}

void SetHostKey(int vk, bool down)
{
    if (vk < 0 || vk >= 256) return;
    g_keys[vk].store(down, std::memory_order_relaxed);
    if (down) g_tapped[vk].store(true, std::memory_order_relaxed);
}

void ClearHostKeys()
{
    for (auto& k : g_keys) k.store(false, std::memory_order_relaxed);
    for (auto& t : g_tapped) t.store(false, std::memory_order_relaxed);
}

// u32 XamInputGetState(u32 userIndex, u32 flags, X_INPUT_STATE* state)
// X_INPUT_STATE: u32 packetNumber, X_INPUT_GAMEPAD gamepad
IMPORT(XamInputGetState)
{
    uint32_t user = ctx.r3.u32, state = ctx.r5.u32;
    static std::atomic<bool> logged{ false };
    if (!logged.exchange(true)) Log("[input] first XamInputGetState(user %u, flags 0x%X)", user, ctx.r4.u32);
    if (user != 0 && user != 0xFF) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    if (state)
    {
        ReadPad(state + 4);
        Store32(state, g_packet.load());
    }
    ctx.r3.u64 = 0;
}

// u32 XamInputSetState(u32 userIndex, u32 flags, X_INPUT_VIBRATION*): rumble ignored
IMPORT(XamInputSetState)
{
    ctx.r3.u64 = (ctx.r3.u32 == 0 || ctx.r3.u32 == 0xFF) ? 0 : X_ERROR_DEVICE_NOT_CONNECTED;
}

// u32 XamInputGetCapabilities(u32 userIndex, u32 flags, X_INPUT_CAPABILITIES* caps)
// X_INPUT_CAPABILITIES: u8 type, u8 subType, u16 flags, X_INPUT_GAMEPAD gamepad, X_INPUT_VIBRATION vibration
IMPORT(XamInputGetCapabilities)
{
    uint32_t user = ctx.r3.u32, caps = ctx.r5.u32;
    static std::atomic<bool> logged{ false };
    if (!logged.exchange(true)) Log("[input] first XamInputGetCapabilities(user %u, flags 0x%X)", user, ctx.r4.u32);
    if (user != 0 && user != 0xFF) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    if (caps)
    {
        memset(GuestPtr(caps), 0, 0x14);
        Store8(caps + 0, 0x01);         // XINPUT_DEVTYPE_GAMEPAD
        Store8(caps + 1, 0x01);         // XINPUT_DEVSUBTYPE_GAMEPAD
        Store16(caps + 4, 0xFFFF);      // all buttons supported
        Store8(caps + 6, 0xFF);
        Store8(caps + 7, 0xFF);
        Store16(caps + 8, 0xFFFF); Store16(caps + 10, 0xFFFF); Store16(caps + 12, 0xFFFF); Store16(caps + 14, 0xFFFF);
    }
    ctx.r3.u64 = 0;
}
