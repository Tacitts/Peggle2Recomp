// Controller input (XamInput*). Controller 0 is connected with nothing pressed; 1-3 are not
// connected. Real input (XInput / keyboard) plugs in at ReadPad().
#include "runtime.h"

namespace
{
    constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 0x48F;
    std::atomic<uint32_t> g_packet{ 0 };

    // X_INPUT_GAMEPAD: u16 buttons, u8 leftTrigger, u8 rightTrigger, s16 lx, ly, rx, ry
    void ReadPad(uint32_t pad)
    {
        memset(GuestPtr(pad), 0, 12);
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
        Store32(state, g_packet.load());
        ReadPad(state + 4);
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
