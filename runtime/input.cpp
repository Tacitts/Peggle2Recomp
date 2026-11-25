// Controller input (XamInput*). Controller 0 is connected with nothing pressed; 1-3 are not
// connected. Real input (XInput / keyboard) plugs in at ReadPad().
#include "runtime.h"

namespace
{
    constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 0x48F;
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
        memset(GuestPtr(pad), 0, 12);
        static uint16_t last = 0;
        uint16_t buttons = AutoPressButtons();
        if (buttons != last) { last = buttons; ++g_packet; }
        Store16(pad, buttons);
    }
}

// u32 XamInputGetState(u32 userIndex, u32 flags, X_INPUT_STATE* state)
// X_INPUT_STATE: u32 packetNumber, X_INPUT_GAMEPAD gamepad
IMPORT(XamInputGetState)
{
    uint32_t user = ctx.r3.u32, state = ctx.r5.u32;
    if (user != 0 && user != 0xFF) { ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED; return; }
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
    ctx.r3.u64 = (ctx.r3.u32 == 0 || ctx.r3.u32 == 0xFF) ? 0 : ERROR_DEVICE_NOT_CONNECTED;
}

// u32 XamInputGetCapabilities(u32 userIndex, u32 flags, X_INPUT_CAPABILITIES* caps)
// X_INPUT_CAPABILITIES: u8 type, u8 subType, u16 flags, X_INPUT_GAMEPAD gamepad, X_INPUT_VIBRATION vibration
IMPORT(XamInputGetCapabilities)
{
    uint32_t user = ctx.r3.u32, caps = ctx.r5.u32;
    if (user != 0 && user != 0xFF) { ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED; return; }
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
