// Video driver (Vd*) imports and a minimal Xenos command processor.
//
// No rendering yet. The command processor exists so the game's D3D can make progress:
// it consumes the primary ring buffer (PM4 packets), executes everything that synchronises
// CPU and GPU (register writes, scratch-register fences, MEM_WRITE, EVENT_WRITE_SHD,
// WAIT_REG_MEM, COND_WRITE, REG_RMW, indirect buffers, interrupts), skips draws/state, writes
// the read pointer back, and raises a 60 Hz vblank interrupt.
//
// Memory model: GPU addresses are physical; physical P is visible to the CPU at 0xA0000000+P
// (the physical allocator's range). Command buffers are big-endian dwords. Memory writes
// carry an endian-swap mode in the low 2 address bits (0 none, 1 8in16, 2 8in32, 3 16in32).
#include "runtime.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

namespace
{
    constexpr uint32_t kDisplayWidth = 1280, kDisplayHeight = 720;

    // Registers (dword indices)
    constexpr uint32_t REG_CP_RB_RPTR = 0x01C4;
    constexpr uint32_t REG_CP_RB_WPTR = 0x01C5;
    constexpr uint32_t REG_SCRATCH_UMSK = 0x01DC;
    constexpr uint32_t REG_SCRATCH_ADDR = 0x01DD;
    constexpr uint32_t REG_SCRATCH_REG0 = 0x0578;
    constexpr uint32_t REG_COHER_STATUS_HOST = 0x0A31;

    // PM4 type-3 opcodes
    enum : uint32_t
    {
        PM4_NOP = 0x10, PM4_REG_RMW = 0x21, PM4_INDIRECT_BUFFER_PFD = 0x37, PM4_WAIT_REG_MEM = 0x3C,
        PM4_MEM_WRITE = 0x3D, PM4_INDIRECT_BUFFER = 0x3F, PM4_COND_WRITE = 0x45, PM4_EVENT_WRITE = 0x46,
        PM4_ME_INIT = 0x48, PM4_INTERRUPT = 0x54, PM4_EVENT_WRITE_SHD = 0x58, PM4_EVENT_WRITE_EXT = 0x59,
        PM4_EVENT_WRITE_ZPD = 0x5B,
    };

    uint32_t g_regs[0x10000];
    uint32_t g_ringPhys = 0, g_ringDwords = 0;
    uint32_t g_rptrWriteBackPhys = 0;
    uint32_t g_readPtr = 0;   // dword index into the ring
    std::atomic<uint32_t> g_interruptCallback{ 0 }, g_interruptContext{ 0 };
    std::atomic<uint64_t> g_vblanks{ 0 }, g_packets{ 0 }, g_fences{ 0 }, g_interrupts{ 0 };
    std::once_flag g_startOnce;
    PPCContext g_gpuCtx{};    // guest context of the GPU thread (for interrupt callbacks)
    std::chrono::steady_clock::time_point g_nextVblank;

    uint32_t PhysToGuest(uint32_t phys) { return 0xA0000000u | (phys & 0x1FFFFFFFu); }

    uint32_t GpuSwap(uint32_t v, uint32_t mode)
    {
        switch (mode & 3)
        {
        case 1: return ((v >> 8) & 0x00FF00FF) | ((v << 8) & 0xFF00FF00);
        case 2: return __builtin_bswap32(v);
        case 3: return (v >> 16) | (v << 16);
        default: return v;
        }
    }

    // GPU memory write/read at a physical address whose low 2 bits select the swap mode.
    void GpuWrite(uint32_t addrWithMode, uint32_t value)
    {
        uint32_t a = PhysToGuest(addrWithMode & ~3u);
        *(volatile uint32_t*)GuestPtr(a) = GpuSwap(value, addrWithMode & 3);
    }
    uint32_t GpuRead(uint32_t addrWithMode)
    {
        uint32_t a = PhysToGuest(addrWithMode & ~3u);
        return GpuSwap(*(volatile uint32_t*)GuestPtr(a), addrWithMode & 3);
    }

    void CallInterrupt(uint32_t source, uint32_t cpu)
    {
        uint32_t cb = g_interruptCallback.load();
        if (!cb) return;
        PPCContext c = g_gpuCtx;
        c.r3.u64 = source;
        c.r4.u64 = g_interruptContext.load();
        // The interrupt is delivered on hardware thread `cpu`: the handler reads the current
        // processor number from KPCR+0x10C (lbz r11,268(r13)) to pick per-CPU state, so
        // set it to the target (PM4_INTERRUPT cpu mask; vblank uses CPU 2).
        Store8(c.r13.u32 + 0x10C, uint8_t(cpu));
        CallGuest(c, cb);
        g_interrupts++;
    }

    // Fire vblank (source 0) when due. Called from the command loop and from inside waits so
    // that a WAIT_REG_MEM on vblank-driven state can complete.
    void TickVblank()
    {
        auto now = std::chrono::steady_clock::now();
        if (now < g_nextVblank) return;
        g_nextVblank = now + std::chrono::microseconds(16667);
        uint64_t n = ++g_vblanks;
        CallInterrupt(0, 2);
        if (n == 1 || n == 60 || (n % 600) == 0)
            Log("[gpu] vblank %llu: %llu packets, %llu fences, %llu interrupts, ring rptr %u/%u",
                (unsigned long long)n, (unsigned long long)g_packets.load(), (unsigned long long)g_fences.load(),
                (unsigned long long)g_interrupts.load(), g_readPtr, g_ringDwords);
    }

    // MMIO sync: the CPU reads/writes GPU registers directly in the MMIO window, which is
    // plain memory. For registers used in CPU<->GPU handshakes, keep a shadow of the value last
    // published to MMIO: a different MMIO value means the CPU wrote it.
    constexpr uint32_t kSyncedRegs[] = {
        0x0578, 0x0579, 0x057A, 0x057B, 0x057C, 0x057D, 0x057E, 0x057F,   // SCRATCH_REG0-7
        REG_SCRATCH_UMSK, REG_SCRATCH_ADDR, REG_COHER_STATUS_HOST,
    };
    uint32_t g_mmioShadow[0x10000];
    bool IsSynced(uint32_t index)
    {
        for (uint32_t r : kSyncedRegs) if (r == index) return true;
        return false;
    }
    void PublishRegister(uint32_t index, uint32_t value)
    {
        Store32(kGpuRegisterBase + index * 4, value);
        g_mmioShadow[index] = value;
    }

    void WriteRegister(uint32_t index, uint32_t value);

    void SyncCpuRegisterWrites()
    {
        for (uint32_t r : kSyncedRegs)
        {
            uint32_t v = Load32(kGpuRegisterBase + r * 4);
            if (v != g_mmioShadow[r]) WriteRegister(r, v);   // CPU wrote it
        }
    }

    void WriteRegister(uint32_t index, uint32_t value)
    {
        index &= 0xFFFF;
        g_regs[index] = value;
        if (IsSynced(index)) PublishRegister(index, value);
        if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8)
        {
            uint32_t n = index - REG_SCRATCH_REG0;
            if (g_regs[REG_SCRATCH_UMSK] & (1u << n))
            {
                // Scratch registers mirror to memory: a common fence mechanism.
                Store32(PhysToGuest(g_regs[REG_SCRATCH_ADDR] + n * 4), value);
                g_fences++;
            }
        }
    }

    uint32_t ReadRegister(uint32_t index)
    {
        index &= 0xFFFF;
        if (index == REG_COHER_STATUS_HOST) g_regs[index] &= ~0x80000000u;   // coherency done
        return g_regs[index];
    }

    bool Compare(uint32_t func, uint32_t value, uint32_t ref)
    {
        switch (func & 7)
        {
        case 1: return value < ref;
        case 2: return value <= ref;
        case 3: return value == ref;
        case 4: return value != ref;
        case 5: return value >= ref;
        case 6: return value > ref;
        default: return true;
        }
    }

    // Reader over a command buffer in guest memory (physical address, dword count).
    struct Reader
    {
        uint32_t base, size, pos;   // ring: size = ring dwords, wraps
        bool ring;
        uint32_t Read()
        {
            uint32_t v = Load32(PhysToGuest(base + (pos % size) * 4));
            pos = ring ? (pos + 1) % size : pos + 1;
            return v;
        }
        void Skip(uint32_t n) { pos = ring ? (pos + n) % size : pos + n; }
    };

    void ExecuteBuffer(Reader& r, uint32_t endPos, int depth);

    // Recent packet history, dumped when a wait stalls (debugging aid).
    struct PacketRecord { uint32_t header, pos, depth, a0, a1, a2; };
    PacketRecord g_history[64];
    uint32_t g_historyNext = 0;

    void DumpHistory()
    {
        Log("[gpu] last packets before the stall (oldest first):");
        for (uint32_t i = 0; i < 40; i++)
        {
            const auto& p = g_history[(g_historyNext + 64 - 40 + i) % 64];
            if (!p.header && !p.pos) continue;
            uint32_t type = p.header >> 30;
            if (type == 3)
                Log("[gpu]   d%u @%5u type3 op 0x%02X count %3u  args %08X %08X %08X", p.depth, p.pos, (p.header >> 8) & 0x7F,
                    ((p.header >> 16) & 0x3FFF) + 1, p.a0, p.a1, p.a2);
            else
                Log("[gpu]   d%u @%5u type%u hdr %08X  args %08X %08X", p.depth, p.pos, type, p.header, p.a0, p.a1);
        }
    }

    void ExecutePacket(Reader& r, int depth)
    {
        uint32_t headerPos = r.pos;
        uint32_t header = r.Read();
        g_packets++;
        {
            // Peek at the first three payload dwords for the history (no consumption).
            Reader peek = r;
            auto& h = g_history[g_historyNext++ % 64];
            h = { header, headerPos, uint32_t(depth), peek.Read(), peek.Read(), peek.Read() };
        }
        switch (header >> 30)
        {
        case 0:   // register writes
        {
            uint32_t count = ((header >> 16) & 0x3FFF) + 1, index = header & 0x7FFF;
            bool oneReg = (header >> 15) & 1;
            for (uint32_t i = 0; i < count; i++) WriteRegister(oneReg ? index : index + i, r.Read());
            return;
        }
        case 1:   // two register writes
        {
            uint32_t r1 = header & 0x7FF, r2 = (header >> 11) & 0x7FF;
            WriteRegister(r1, r.Read());
            WriteRegister(r2, r.Read());
            return;
        }
        case 2:   // NOP
            return;
        }

        uint32_t opcode = (header >> 8) & 0x7F, count = ((header >> 16) & 0x3FFF) + 1;
        uint32_t start = r.pos;
        switch (opcode)
        {
        case PM4_INDIRECT_BUFFER:
        case PM4_INDIRECT_BUFFER_PFD:
        {
            uint32_t addr = r.Read(), len = r.Read() & 0xFFFFF;
            if (depth < 4 && len)
            {
                Reader ib{ addr & ~3u, len, 0, false };
                ExecuteBuffer(ib, len, depth + 1);
            }
            break;
        }
        case PM4_WAIT_REG_MEM:
        {
            uint32_t info = r.Read(), poll = r.Read(), ref = r.Read(), mask = r.Read(), wait = r.Read();
            (void)wait;
            auto start = std::chrono::steady_clock::now();
            bool warned = false;
            for (;;)
            {
                SyncCpuRegisterWrites();
                uint32_t v = (info & 0x10) ? GpuRead(poll) : ReadRegister(poll);
                if (Compare(info, v & mask, ref)) break;
                TickVblank();
                std::this_thread::yield();
                if (!warned && std::chrono::steady_clock::now() - start > std::chrono::seconds(2))
                {
                    Log("[gpu] WAIT_REG_MEM still waiting: %s 0x%08X & 0x%X func %u ref 0x%X (value 0x%X)",
                        (info & 0x10) ? "mem" : "reg", poll, mask, info & 7, ref, v & mask);
                    DumpHistory();
                    warned = true;
                }
            }
            break;
        }
        case PM4_REG_RMW:
        {
            uint32_t info = r.Read(), andMask = r.Read(), orMask = r.Read();
            uint32_t reg = info & 0x1FFF;
            uint32_t v = ReadRegister(reg);
            if (info & 0x80000000) andMask = ReadRegister(andMask & 0x1FFF);
            if (info & 0x40000000) orMask = ReadRegister(orMask & 0x1FFF);
            WriteRegister(reg, (v & andMask) | orMask);
            break;
        }
        case PM4_COND_WRITE:
        {
            uint32_t info = r.Read(), poll = r.Read(), ref = r.Read(), mask = r.Read(), waddr = r.Read(), wdata = r.Read();
            uint32_t v = (info & 0x10) ? GpuRead(poll) : ReadRegister(poll);
            if (Compare(info, v & mask, ref))
            {
                if (info & 0x100) GpuWrite(waddr, wdata);
                else WriteRegister(waddr, wdata);
            }
            break;
        }
        case PM4_MEM_WRITE:
        {
            uint32_t addr = r.Read();
            for (uint32_t i = 1; i < count; i++) GpuWrite(addr + (i - 1) * 4, r.Read());
            g_fences++;
            break;
        }
        case PM4_EVENT_WRITE_SHD:
        {
            uint32_t initiator = r.Read(), addr = r.Read(), value = r.Read();
            GpuWrite(addr, (initiator >> 31) ? uint32_t(g_vblanks.load()) : value);
            g_fences++;
            break;
        }
        case PM4_INTERRUPT:
        {
            uint32_t cpuMask = r.Read();
            for (uint32_t cpu = 0; cpu < 6; cpu++)
                if (cpuMask & (1u << cpu)) CallInterrupt(1, cpu);
            break;
        }
        default:   // ME_INIT, NOP, EVENT_WRITE(_EXT/_ZPD), draws, constants, state: skipped
            break;
        }
        // Consume whatever the handler did not read, so packets stay aligned.
        uint32_t used = r.ring ? (r.pos + r.size - start) % r.size : r.pos - start;
        if (used < count) r.Skip(count - used);
    }

    void ExecuteBuffer(Reader& r, uint32_t endPos, int depth)
    {
        while (r.pos != endPos)
        {
            if (!r.ring && r.pos >= endPos) break;
            ExecutePacket(r, depth);
        }
    }

    // Display-controller registers the CPU reads directly. The graphics interrupt handler only
    // runs its vblank processing (0x8261EF88: frame pacing, wakes the callback thread) when
    // bit 0 of the interrupt status register is set (lis 0x7FC8; lwz 0x6544 = reg 0x1951).
    void PublishDisplayRegisters()
    {
        Store32(kGpuRegisterBase + 0x1951 * 4, 0x00000001);   // interrupt status: vblank
        Store32(kGpuRegisterBase + 0x194C * 4, 0x000002D0);   // D1MODE_V_COUNTER
        Store32(kGpuRegisterBase + 0x1961 * 4, (kDisplayWidth << 16) | kDisplayHeight);   // viewport size
    }

    void GpuThread()
    {
        PublishDisplayRegisters();
        InitGuestThread(g_gpuCtx, 0x10000, 0x6B0);   // runtime-owned "GPU" thread
        g_nextVblank = std::chrono::steady_clock::now();
        Log("[gpu] command processor started: ring phys 0x%08X, %u dwords", g_ringPhys, g_ringDwords);
        for (;;)
        {
            SyncCpuRegisterWrites();
            uint32_t wptr = Load32(kGpuRegisterBase + REG_CP_RB_WPTR * 4) % (g_ringDwords ? g_ringDwords : 1);
            if (g_ringDwords && wptr != g_readPtr)
            {
                Reader rb{ g_ringPhys, g_ringDwords, g_readPtr, true };
                ExecuteBuffer(rb, wptr, 0);
                g_readPtr = rb.pos;
                g_regs[REG_CP_RB_RPTR] = g_readPtr;
                Store32(kGpuRegisterBase + REG_CP_RB_RPTR * 4, g_readPtr);
                if (g_rptrWriteBackPhys) Store32(PhysToGuest(g_rptrWriteBackPhys), g_readPtr);
            }
            TickVblank();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    void StartGpu() { std::call_once(g_startOnce, [] { std::thread(GpuThread).detach(); }); }
}

// ---- Display queries ---------------------------------------------------------------------------
// X_VIDEO_MODE: width, height, interlaced, widescreen, hi-def, f32 refresh, standard,
//               unknown (0x4A), unknown (0x01), reserved[3]
static void FillVideoMode(uint32_t p)
{
    memset(GuestPtr(p), 0, 0x30);
    Store32(p + 0x00, kDisplayWidth);
    Store32(p + 0x04, kDisplayHeight);
    Store32(p + 0x08, 0);
    Store32(p + 0x0C, 1);
    Store32(p + 0x10, 1);
    Store32(p + 0x14, 0x42700000);   // 60.0f
    Store32(p + 0x18, 1);            // NTSC
    Store32(p + 0x1C, 0x4A);
    Store32(p + 0x20, 0x01);
}

IMPORT(VdQueryVideoMode) { FillVideoMode(ctx.r3.u32); }
// bit 0 widescreen, bit 1 >= 1024 wide, bit 2 >= 1920 wide
IMPORT(VdQueryVideoFlags) { ctx.r3.u64 = 0x3; }
// VdGetCurrentDisplayGamma(u32* type, f32* power)
IMPORT(VdGetCurrentDisplayGamma)
{
    if (ctx.r3.u32) Store32(ctx.r3.u32, 1);
    if (ctx.r4.u32) Store32(ctx.r4.u32, 0x400E38E4);   // 2.2222f
}
IMPORT(VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 1; }
IMPORT(VdRetrainEDRAM) { ctx.r3.u64 = 0; }
IMPORT(VdRetrainEDRAMWorker) { ctx.r3.u64 = 0; }
IMPORT(VdInitializeEngines) { ctx.r3.u64 = 1; }
IMPORT(VdShutdownEngines) {}
IMPORT(VdEnableDisableClockGating) { ctx.r3.u64 = 0; }
IMPORT(VdSetDisplayMode) { ctx.r3.u64 = 0; }
IMPORT(VdSetDisplayModeOverride) { ctx.r3.u64 = 0; }
IMPORT(VdPersistDisplay) { if (ctx.r4.u32) Store32(ctx.r4.u32, 0); ctx.r3.u64 = 1; }
IMPORT(VdInitializeScalerCommandBuffer) { ctx.r3.u64 = 0; }
IMPORT(VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }
IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) {}

// VdGetCurrentDisplayInformation(X_DISPLAY_INFO*): zero, then the fields D3D reads.
IMPORT(VdGetCurrentDisplayInformation)
{
    uint32_t p = ctx.r3.u32;
    memset(GuestPtr(p), 0, 0x58);
    Store16(p + 0x00, kDisplayWidth);
    Store16(p + 0x02, kDisplayHeight);
    Store16(p + 0x0C, kDisplayWidth);
    Store16(p + 0x0E, kDisplayHeight);
    Store32(p + 0x44, 60);
}

// ---- Ring buffer / interrupts ------------------------------------------------------------------
// VdInitializeRingBuffer(u32 physAddr, u32 sizeLog2): ring is 2^(sizeLog2+3) bytes.
IMPORT(VdInitializeRingBuffer)
{
    g_ringPhys = ctx.r3.u32;
    g_ringDwords = (1u << (ctx.r4.u32 + 3)) / 4;
    g_readPtr = 0;
    Log("[gpu] ring buffer at phys 0x%08X, %u dwords", g_ringPhys, g_ringDwords);
    StartGpu();
}

// VdEnableRingBufferRPtrWriteBack(u32 physAddr, u32 blockSizeLog2)
IMPORT(VdEnableRingBufferRPtrWriteBack)
{
    g_rptrWriteBackPhys = ctx.r3.u32;
    Log("[gpu] read-pointer write-back at phys 0x%08X", g_rptrWriteBackPhys);
}

// VdSetGraphicsInterruptCallback(u32 callback, u32 context)
IMPORT(VdSetGraphicsInterruptCallback)
{
    g_interruptCallback = ctx.r3.u32;
    g_interruptContext = ctx.r4.u32;
    Log("[gpu] interrupt callback 0x%08X(0x%08X)", ctx.r3.u32, ctx.r4.u32);
}

// VdGetSystemCommandBuffer(u32* p0, u32* p1)
IMPORT(VdGetSystemCommandBuffer)
{
    if (ctx.r3.u32) Store32(ctx.r3.u32, 0);
    if (ctx.r4.u32) Store32(ctx.r4.u32, 0);
}

// ---- Shader capture --------------------------------------------------------------------------
// The game's shaders live compressed in main.rsb (PopCap RSB), so instead of parsing the
// archive, scan committed guest memory for Xbox 360 shader containers (XenosRecomp's
// signature: (flags & 0xFFFFFF00) == 0x102A1100, fields 0x1C/0x20 zero) once the game has
// loaded them, and write each unique container to build/runtime/shaders/<hash>.bin.
// XenosRecomp then converts that directory to HLSL/DXIL.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <set>

static void DumpShaderContainers()
{
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    auto dir = std::filesystem::path(exe).parent_path() / "shaders";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::set<uint64_t> seen;
    uint32_t found = 0, written = 0;
    // Virtual heap + runtime area, and physical RAM through its 0xA0000000 view.
    const std::pair<uint64_t, uint64_t> ranges[] = { { 0x40000000ull, 0x7C000000ull }, { 0xA0000000ull, 0xC0000000ull } };
    for (auto [lo, hi] : ranges)
    {
        uint64_t a = lo;
        while (a < hi)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(g_base + a, &mbi, sizeof(mbi))) break;
            uint64_t regionStart = uint64_t((uint8_t*)mbi.BaseAddress - g_base);
            uint64_t regionEnd = std::min<uint64_t>(regionStart + mbi.RegionSize, hi);
            if (mbi.State == MEM_COMMIT)
            {
                for (uint64_t p = std::max(regionStart, a); p + 0x24 <= regionEnd; p += 4)
                {
                    uint32_t flags = Load32(uint32_t(p));
                    if ((flags & 0xFFFFFF00) != 0x102A1100) continue;
                    uint32_t vsize = Load32(uint32_t(p + 4)), psize = Load32(uint32_t(p + 8));
                    if (Load32(uint32_t(p + 0x1C)) || Load32(uint32_t(p + 0x20))) continue;
                    uint64_t size = uint64_t(vsize) + psize;
                    if (!vsize || size > 0x40000 || p + size > regionEnd) continue;
                    found++;
                    // FNV-1a over the container bytes as the file name.
                    uint64_t h = 1469598103934665603ull;
                    const uint8_t* bytes = GuestPtr(uint32_t(p));
                    for (uint64_t i = 0; i < size; i++) { h ^= bytes[i]; h *= 1099511628211ull; }
                    if (!seen.insert(h).second) continue;
                    char name[32];
                    snprintf(name, sizeof(name), "%016llX.bin", (unsigned long long)h);
                    FILE* f = fopen((dir / name).string().c_str(), "wb");
                    if (f) { fwrite(bytes, 1, size_t(size), f); fclose(f); written++; }
                }
            }
            a = regionEnd;
        }
    }
    Log("[gpu] shader capture: %u containers in memory, %u unique written to %s", found, written, dir.string().c_str());
}

// VdSwap(u32* bufferPtr, fetch, ...): D3D reserves 64 dwords in the ring at bufferPtr for the
// swap packet. Fill it with one NOP packet covering the space (no presentation yet).
static std::atomic<uint32_t> g_swaps{ 0 };
IMPORT(VdSwap)
{
    if (uint32_t buf = ctx.r3.u32)
    {
        Store32(buf, (3u << 30) | ((63u - 1) << 16) | (PM4_NOP << 8));
        for (uint32_t i = 1; i < 64; i++) Store32(buf + i * 4, 0);
    }
    uint32_t n = ++g_swaps;
    RendererPresent();
    if (n <= 3 || (n % 600) == 0) Log("[gpu] VdSwap #%u", n);
    if (n == 300 || n == 1200) DumpShaderContainers();   // after loading screens have run
    ctx.r3.u64 = 0;
}
