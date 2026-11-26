// Host renderer: window + D3D12 swap chain.
//
// Draw translation (PM4 draws -> D3D12, XenosRecomp shaders, EDRAM resolves) builds on this
// device/queue/swap chain. Until then each frame shows a *texture preview*: the largest
// texture the game bound for a draw this frame (gpu.cpp), untiled and converted on the CPU and
// copied into the back buffer. That makes movie frames, backgrounds and UI atlases visible.
// The title shows game frames, FPS and the previewed texture.
#include "runtime.h"
#include "frame_stats.h"
#include "settings.h"
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include "achievements.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// ---- Xenos texture decoding (CPU) ----------------------------------------------------------------
namespace
{
    // Xbox 360 tiled address -> linear block index. x/y in blocks, pitch in blocks (multiple of
    // 32), bpb = bytes per block. Standard XGAddress2DTiledOffset formula.
    uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bpb)
    {
        uint32_t alignedWidth = (pitch + 31) & ~31u;
        uint32_t logBpp = (bpb >> 2) + ((bpb >> 1) >> (bpb >> 2));
        uint32_t macro = ((x >> 5) + (y >> 5) * (alignedWidth >> 5)) << (logBpp + 7);
        uint32_t micro = ((x & 7) + ((y & 6) << 2)) << logBpp;
        uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + logBpp)) + ((y & 1) << 4);
        return (((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
                ((((y & 8) >> 2) + (x >> 3)) & 3) << 6) >> logBpp;
    }

    void SwapBlock(uint8_t* b, uint32_t n, uint32_t endian)
    {
        for (uint32_t i = 0; i + 4 <= n; i += 4)
        {
            uint8_t t;
            switch (endian)
            {
            case 1: std::swap(b[i], b[i + 1]); std::swap(b[i + 2], b[i + 3]); break;   // 8in16
            case 2: t = b[i]; b[i] = b[i + 3]; b[i + 3] = t; t = b[i + 1]; b[i + 1] = b[i + 2]; b[i + 2] = t; break;   // 8in32
            case 3: std::swap(b[i], b[i + 2]); std::swap(b[i + 1], b[i + 3]); break;   // 16in32
            default: break;
            }
        }
    }

    uint32_t Rgb565ToBgra(uint16_t c)
    {
        uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        return 0xFF000000u | ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
    }

    // DXT colour block (8 bytes, little-endian after swap) -> 16 BGRA texels.
    void DecodeColorBlock(const uint8_t* s, uint32_t out[16], bool dxt1)
    {
        uint16_t c0 = uint16_t(s[0] | (s[1] << 8)), c1 = uint16_t(s[2] | (s[3] << 8));
        uint32_t p[4] = { Rgb565ToBgra(c0), Rgb565ToBgra(c1), 0, 0 };
        auto mix = [](uint32_t a, uint32_t b, int wa, int wb, int d) {
            uint32_t r = 0xFF000000u;
            for (int sh = 0; sh < 24; sh += 8)
                r |= ((((a >> sh) & 255) * wa + ((b >> sh) & 255) * wb) / d) << sh;
            return r;
        };
        if (!dxt1 || c0 > c1) { p[2] = mix(p[0], p[1], 2, 1, 3); p[3] = mix(p[0], p[1], 1, 2, 3); }
        else { p[2] = mix(p[0], p[1], 1, 1, 2); p[3] = 0; }
        uint32_t bits = s[4] | (s[5] << 8) | (s[6] << 16) | (uint32_t(s[7]) << 24);
        for (int i = 0; i < 16; i++) out[i] = p[(bits >> (2 * i)) & 3];
    }

    // Bytes per block and block size of the supported formats (false if unsupported).
    bool FormatBlocks(uint32_t fmt, uint32_t& bpb, uint32_t& blockDim)
    {
        switch (fmt)
        {
        case 2: bpb = 1; blockDim = 1; return true;                     // 8
        case 4: case 10: bpb = 2; blockDim = 1; return true;            // 5_6_5, 8_8
        case 6: bpb = 4; blockDim = 1; return true;                     // 8_8_8_8
        case 26: bpb = 8; blockDim = 1; return true;                    // 16_16_16_16
        case 18: case 52: bpb = 8; blockDim = 4; return true;           // DXT1
        case 19: case 20: case 53: case 54: bpb = 16; blockDim = 4; return true;   // DXT2/3, DXT4/5
        default: return false;
        }
    }

    // Decode mip 0 of a texture into BGRA8 (w x h). Returns false for unsupported formats.
    // `snapshot` (optional): a copy of the texture's footprint taken when the draw was recorded.
    bool DecodeTexture(const GuestTextureInfo& t, std::vector<uint32_t>& out, const uint8_t* snapshot = nullptr, size_t snapshotSize = 0, bool allowFast = true)
    {
        enum { k_8 = 2, k_5_6_5 = 4, k_8_8_8_8 = 6, k_8_8 = 10, k_DXT1 = 18, k_DXT2_3 = 19, k_DXT4_5 = 20, k_16_16_16_16 = 26,
               k_DXT1_AS_16 = 52, k_DXT2_3_AS_16 = 53, k_DXT4_5_AS_16 = 54 };
        uint32_t fmt = t.format, bpb, blockDim;
        switch (fmt)
        {
        case k_8: bpb = 1; blockDim = 1; break;
        case k_5_6_5: case k_8_8: bpb = 2; blockDim = 1; break;
        case k_8_8_8_8: bpb = 4; blockDim = 1; break;
        case k_16_16_16_16: bpb = 8; blockDim = 1; break;
        case k_DXT1: case k_DXT1_AS_16: bpb = 8; blockDim = 4; break;
        case k_DXT2_3: case k_DXT4_5: case k_DXT2_3_AS_16: case k_DXT4_5_AS_16: bpb = 16; blockDim = 4; break;
        default: return false;
        }
        uint32_t w = t.width, h = t.height;
        uint32_t bw = (w + blockDim - 1) / blockDim, bh = (h + blockDim - 1) / blockDim;
        uint32_t pitchBlocks = std::max<uint32_t>((t.pitch ? t.pitch : w) / blockDim, bw);
        if (t.tiled) pitchBlocks = (pitchBlocks + 31) & ~31u;
        out.assign(size_t(w) * h, 0xFF000000u);

        // Fast path: one texel per block, the whole footprint readable, and a byte order that
        // stays inside the texel (as the general loop below would apply it).
        if (allowFast && blockDim == 1 && (t.endian == 0 || bpb >= 4 || (bpb == 2 && t.endian == 1)))
        {
            uint64_t footprint = t.tiled ? uint64_t(pitchBlocks) * ((bh + 31) & ~31u) * bpb
                                         : (uint64_t(bh - 1) * pitchBlocks + bw) * bpb;
            const uint8_t* base = nullptr;
            if (snapshot) base = footprint <= snapshotSize ? snapshot : nullptr;
            else if (uint64_t(t.address) + footprint <= 0xC0000000ull) base = (const uint8_t*)GuestPtr(t.address);
            if (base)
            {
                if (fmt == k_8 && !t.tiled)   // movie planes: expand each byte to grey, opaque
                {
                    for (uint32_t y = 0; y < h; y++)
                    {
                        const uint8_t* s = base + size_t(y) * pitchBlocks;
                        uint32_t* o = out.data() + size_t(y) * w;
                        for (uint32_t x = 0; x < w; x++) o[x] = 0xFF000000u | (uint32_t(s[x]) * 0x010101u);
                    }
                    return true;
                }
                for (uint32_t y = 0; y < h; y++)
                {
                    uint32_t* o = out.data() + size_t(y) * w;
                    for (uint32_t x = 0; x < w; x++)
                    {
                        uint32_t idx = t.tiled ? TiledOffset(x, y, pitchBlocks, bpb) : y * pitchBlocks + x;
                        uint8_t block[8];
                        memcpy(block, base + size_t(idx) * bpb, bpb);
                        if (t.endian) SwapBlock(block, bpb < 4 ? 4 : bpb, t.endian);   // bpb 2: 8in16 only
                        if (fmt == k_8) o[x] = 0xFF000000u | (block[0] * 0x010101u);
                        else if (fmt == k_8_8) o[x] = (uint32_t(block[1]) << 24) | (block[0] * 0x010101u);
                        else if (fmt == k_5_6_5) o[x] = Rgb565ToBgra(uint16_t(block[0] | (block[1] << 8)));
                        else if (fmt == k_16_16_16_16) o[x] = block[1] | (block[3] << 8) | (block[5] << 16) | (uint32_t(block[7]) << 24);
                        else o[x] = block[0] | (block[1] << 8) | (block[2] << 16) | (uint32_t(block[3]) << 24);
                    }
                }
                return true;
            }
        }
        uint8_t block[16];
        for (uint32_t by = 0; by < bh; by++)
        {
            for (uint32_t bx = 0; bx < bw; bx++)
            {
                uint32_t idx = t.tiled ? TiledOffset(bx, by, pitchBlocks, bpb) : by * pitchBlocks + bx;
                uint64_t src = uint64_t(t.address) + uint64_t(idx) * bpb;
                if (snapshot)
                {
                    size_t at = size_t(idx) * bpb;
                    if (at + bpb <= snapshotSize) memcpy(block, snapshot + at, bpb);
                    else memset(block, 0, bpb);
                }
                else
                {
                    if (src + bpb > 0xC0000000ull) return false;   // stay inside the physical view
                    memcpy(block, GuestPtr(uint32_t(src)), bpb);
                }
                SwapBlock(block, std::max<uint32_t>(bpb, 4), t.endian);
                if (blockDim == 1)
                {
                    uint32_t& o = out[size_t(by) * w + bx];
                    if (fmt == k_8) o = 0xFF000000u | (block[0] * 0x010101u);
                    else if (fmt == k_8_8) o = (uint32_t(block[1]) << 24) | (block[0] * 0x010101u);
                    else if (fmt == k_5_6_5) o = Rgb565ToBgra(uint16_t(block[0] | (block[1] << 8)));
                    else if (fmt == k_16_16_16_16)   // unorm 16-bit x, y, z, w -> top bytes, in 8888's byte order
                        o = block[1] | (block[3] << 8) | (block[5] << 16) | (uint32_t(block[7]) << 24);
                    else o = block[0] | (block[1] << 8) | (block[2] << 16) | (uint32_t(block[3]) << 24);
                    continue;
                }
                uint32_t texels[16];
                bool dxt1 = fmt == k_DXT1 || fmt == k_DXT1_AS_16;
                DecodeColorBlock(dxt1 ? block : block + 8, texels, dxt1);
                if (!dxt1)
                {
                    bool explicitAlpha = fmt == k_DXT2_3 || fmt == k_DXT2_3_AS_16;
                    uint8_t a[16];
                    if (explicitAlpha)
                        for (int i = 0; i < 16; i++) a[i] = uint8_t(((block[i / 2] >> ((i & 1) * 4)) & 15) * 17);
                    else
                    {
                        uint8_t a0 = block[0], a1 = block[1], pal[8] = { a0, a1 };
                        for (int i = 2; i < 8; i++)
                            pal[i] = a0 > a1 ? uint8_t(((8 - i) * a0 + (i - 1) * a1) / 7)
                                             : (i < 6 ? uint8_t(((6 - i) * a0 + (i - 1) * a1) / 5) : (i == 6 ? 0 : 255));
                        uint64_t bits = 0;
                        for (int i = 0; i < 6; i++) bits |= uint64_t(block[2 + i]) << (8 * i);
                        for (int i = 0; i < 16; i++) a[i] = pal[(bits >> (3 * i)) & 7];
                    }
                    for (int i = 0; i < 16; i++) texels[i] = (texels[i] & 0x00FFFFFFu) | (uint32_t(a[i]) << 24);
                }
                for (int py = 0; py < 4; py++)
                    for (int px = 0; px < 4; px++)
                    {
                        uint32_t x = bx * 4 + px, y = by * 4 + py;
                        if (x < w && y < h) out[size_t(y) * w + x] = texels[py * 4 + px];
                    }
            }
        }
        return true;
    }
}

bool DecodeGuestTexture(const GuestTextureInfo& t, std::vector<uint32_t>& out, const uint8_t* snapshot, size_t snapshotSize)
{
    return DecodeTexture(t, out, snapshot, snapshotSize);
}

bool GuestDxtUploadable(const GuestTextureInfo& t, uint32_t& bpb)
{
    switch (t.format)
    {
    case 18: case 52: bpb = 8; break;                            // DXT1
    case 19: case 20: case 53: case 54: bpb = 16; break;         // DXT2/3, DXT4/5
    default: return false;
    }
    return (t.width % 4) == 0 && (t.height % 4) == 0 && t.endian <= 1;
}

bool CopyGuestDxtBlocks(const GuestTextureInfo& t, uint8_t* dst, uint32_t dstPitch, const uint8_t* snapshot, size_t snapshotSize)
{
    uint32_t bpb;
    if (!GuestDxtUploadable(t, bpb)) return false;
    uint32_t bw = t.width / 4, bh = t.height / 4;
    uint32_t pitchBlocks = std::max<uint32_t>((t.pitch ? t.pitch : t.width) / 4, bw);
    if (t.tiled) pitchBlocks = (pitchBlocks + 31) & ~31u;
    uint64_t footprint = t.tiled ? uint64_t(pitchBlocks) * ((bh + 31) & ~31u) * bpb : (uint64_t(bh - 1) * pitchBlocks + bw) * bpb;
    const uint8_t* base = nullptr;
    if (snapshot) base = footprint <= snapshotSize ? snapshot : nullptr;
    else if (uint64_t(t.address) + footprint <= 0xC0000000ull) base = (const uint8_t*)GuestPtr(t.address);
    if (!base) return false;
    for (uint32_t by = 0; by < bh; by++)
    {
        uint8_t* row = dst + size_t(by) * dstPitch;
        if (!t.tiled) memcpy(row, base + size_t(by) * pitchBlocks * bpb, size_t(bw) * bpb);
        else
            for (uint32_t bx = 0; bx < bw; bx++)
                memcpy(row + size_t(bx) * bpb, base + size_t(TiledOffset(bx, by, pitchBlocks, bpb)) * bpb, bpb);
        if (t.endian == 1)   // 8in16: swap the bytes of every 16-bit word
            for (size_t i = 0; i + 1 < size_t(bw) * bpb; i += 2) std::swap(row[i], row[i + 1]);
    }
    return true;
}

// Bytes of guest memory mip 0 occupies (tiled textures: width and height padded to 32 blocks).
bool GuestTextureFootprint(const GuestTextureInfo& t, uint32_t& bytes)
{
    uint32_t bpb, blockDim;
    if (!FormatBlocks(t.format, bpb, blockDim)) return false;
    uint32_t bw = (t.width + blockDim - 1) / blockDim, bh = (t.height + blockDim - 1) / blockDim;
    uint32_t pitchBlocks = std::max<uint32_t>((t.pitch ? t.pitch : t.width) / blockDim, bw);
    if (t.tiled) { pitchBlocks = (pitchBlocks + 31) & ~31u; bh = (bh + 31) & ~31u; }
    uint64_t b = uint64_t(pitchBlocks) * bh * bpb;
    if (b > (64u << 20)) return false;
    bytes = uint32_t(b);
    return true;
}

#include "xenos.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
    constexpr UINT kWidth = 1280, kHeight = 720, kBuffers = 2;

    HWND g_hwnd = nullptr;
    ID3D12Device* g_device = nullptr;
    ID3D12CommandQueue* g_queue = nullptr;
    IDXGISwapChain3* g_swapChain = nullptr;
    ID3D12DescriptorHeap* g_rtvHeap = nullptr;
    ID3D12Resource* g_backBuffers[kBuffers] = {};
    ID3D12CommandAllocator* g_allocators[kBuffers] = {};
    ID3D12GraphicsCommandList* g_list = nullptr;
    ID3D12Fence* g_fence = nullptr;
    HANDLE g_fenceEvent = nullptr;
    UINT64 g_fenceValues[kBuffers] = {};
    UINT64 g_nextFence = 1;
    UINT g_rtvSize = 0;

    std::mutex g_presentLock;
    std::condition_variable g_presentCv;
    uint64_t g_requested = 0, g_presented = 0;
    // Frame pacing stats: each submitted draw list gets a serial; a present shows one serial.
    uint64_t g_drawSerial = 0;          // under g_drawsLock
    uint64_t g_shownSerial = 0;         // serial of the draw list the last PresentFrame drew
    FrameHist g_newFrameHist;           // time between presents that showed a new draw list
    FrameHist g_renderWorkHist;         // PresentFrame CPU time (includes the Present() wait)
    std::atomic<bool> g_ready{ false };

    // PEGGLE2_CAPTURE_TITLE=<png path>: the launcher's art capture. The game runs hidden and muted
    // with no input; the frame where its title screen shows (750, about 25 s in) is saved to that
    // path at 1280x720 and the process exits. The art comes from the player's own copy.
    const char* g_captureTitle = getenv("PEGGLE2_CAPTURE_TITLE");
    constexpr uint64_t kTitleFrame = 750;

    // Texture preview: latest texture from the game, and one upload buffer per back buffer
    // (persistently mapped; 1280x720 BGRA8, row pitch 5120 is already 256-byte aligned).
    GuestTextureInfo g_pendingPreview{};
    bool g_hasPreview = false;
    ID3D12Resource* g_upload[kBuffers] = {};
    uint8_t* g_uploadPtr[kBuffers] = {};
    constexpr UINT kUploadPitch = kWidth * 4;
    std::vector<uint32_t> g_decoded;
    char g_previewDesc[96] = "none";

    // Window / swap chain size. The game always renders a 1280x720 frame; draws are rasterized
    // at the back buffer's real resolution into a letterboxed viewport (aspect preserved).
    UINT g_bbW = kWidth, g_bbH = kHeight;
    std::atomic<bool> g_resizePending{ false };
    std::atomic<UINT> g_pendingW{ kWidth }, g_pendingH{ kHeight };
    bool g_fullscreen = false;
    RECT g_windowedRect{};
    LONG_PTR g_windowedStyle = 0;

    // Borderless fullscreen on the window's monitor (F11 or Alt+Enter); the game is unaware.
    void ToggleFullscreen(HWND hwnd)
    {
        if (!g_fullscreen)
        {
            GetWindowRect(hwnd, &g_windowedRect);
            g_windowedStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongPtrW(hwnd, GWL_STYLE, (g_windowedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        }
        else
        {
            SetWindowLongPtrW(hwnd, GWL_STYLE, g_windowedStyle);
            SetWindowPos(hwnd, nullptr, g_windowedRect.left, g_windowedRect.top, g_windowedRect.right - g_windowedRect.left,
                         g_windowedRect.bottom - g_windowedRect.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
        }
        g_fullscreen = !g_fullscreen;
        ShowCursor(g_fullscreen ? FALSE : TRUE);
        Log("[render] %s", g_fullscreen ? "fullscreen" : "windowed");
    }

    // Translated draws: the latest complete frame from the GPU thread (RendererSubmitDraws).
    bool g_drawReady = false;
    std::mutex g_drawsLock;
    // Flip pacing (under g_drawsLock): every swap the title requests ends in exactly one flip, in
    // order, so submitted draw lists wait in a queue and each guest flip shows the next one, at
    // the cadence the title chose (swap interval 2 from the previous flip, 5% tear window).
    // Before the first flip is seen (or with PEGGLE2_PRESENT=submit) lists are shown at once.
    struct Submitted { uint64_t serial; uint32_t front; DrawList draws; };
    std::deque<Submitted> g_recent;     // submitted, not yet flipped
    DrawList g_latestDraws;             // the list to present next
    uint32_t g_latestFront = 0;
    uint64_t g_latestSerial = 0;
    bool g_flipPacing = false;          // a guest flip has been seen
    uint32_t g_pendingFlips = 0;        // flips that came before their draws
    uint64_t g_flipsMatched = 0, g_flipsEarly = 0, g_flipsUnmatched = 0;
    const bool g_presentOnSubmit = [] { const char* v = getenv("PEGGLE2_PRESENT"); return v && strcmp(v, "submit") == 0; }();
    uint32_t g_lastExecuted = 0;
    const bool g_forcePreview = getenv("PEGGLE2_PREVIEW") != nullptr;

    std::atomic<bool> g_screenshotRequested{ false };   // F12 (see SaveScreenshot)

    // ---- F2 frame counter / F3 performance overlay / achievement pop-up ----------------------
    // Text drawn with GDI into a small bitmap (two slots: 0 the counter, 1 the pop-up) and copied
    // into the back buffer: the counter top-left, the pop-up at the bottom centre.
    std::atomic<bool> g_showFps{ false }, g_showPerf{ false };
    constexpr UINT kOverlayW = 512, kOverlayH = 96, kOverlayPitch = kOverlayW * 4;
    ID3D12Resource* g_overlayUpload[kBuffers] = {};
    uint8_t* g_overlayPtr[kBuffers] = {};
    HDC g_overlayDc = nullptr;
    HBITMAP g_overlayBmp = nullptr;
    uint32_t* g_overlayBits = nullptr;
    HFONT g_overlayFont = nullptr;
    struct OverlayStats
    {
        std::chrono::steady_clock::time_point windowStart{}, lastNew{};
        uint32_t newFrames = 0, late = 0;          // this second
        double worstMs = 0, workSum = 0;
        uint32_t workN = 0;
        double fps = 0, shownWorst = 0, workAvg = 0;   // last completed second
        uint32_t lateLastSec = 0;
        std::deque<uint32_t> lateHistory;          // late frames per second, last 10 s
    } g_ov;

    // Rows of text, each with a colour; returns the height used.
    UINT DrawOverlayText(const std::vector<std::pair<std::string, COLORREF>>& rows, int slot = 0)
    {
        if (!g_overlayDc)
        {
            g_overlayDc = CreateCompatibleDC(nullptr);
            BITMAPINFO bi{};
            bi.bmiHeader = { sizeof(BITMAPINFOHEADER), LONG(kOverlayW), -LONG(kOverlayH * 2), 1, 32, BI_RGB };
            g_overlayBmp = CreateDIBSection(g_overlayDc, &bi, DIB_RGB_COLORS, (void**)&g_overlayBits, nullptr, 0);
            SelectObject(g_overlayDc, g_overlayBmp);
            g_overlayFont = CreateFontW(-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
            SelectObject(g_overlayDc, g_overlayFont);
            SetBkMode(g_overlayDc, TRANSPARENT);
        }
        UINT h = std::min<UINT>(UINT(rows.size()) * 22 + 8, kOverlayH);
        int y0 = slot * int(kOverlayH);
        RECT all{ 0, y0, LONG(kOverlayW), LONG(y0 + h) };
        HBRUSH bg = CreateSolidBrush(slot ? RGB(30, 30, 30) : RGB(16, 16, 16));
        FillRect(g_overlayDc, &all, bg);
        DeleteObject(bg);
        int y = y0 + 4;
        UINT w = 0;
        for (auto& [text, color] : rows)
        {
            SetTextColor(g_overlayDc, color);
            TextOutA(g_overlayDc, 8, y, text.c_str(), int(text.size()));
            SIZE sz{};
            GetTextExtentPoint32A(g_overlayDc, text.c_str(), int(text.size()), &sz);
            w = std::max<UINT>(w, UINT(sz.cx) + 16);
            y += 22;
        }
        GdiFlush();
        return h | (std::min(w, kOverlayW) << 16);
    }

    void NoteOverlayFrame(double gapMs, double workMs, bool newFrame)
    {
        auto now = std::chrono::steady_clock::now();
        if (g_ov.windowStart.time_since_epoch().count() == 0) g_ov.windowStart = now;
        g_ov.workSum += workMs;
        g_ov.workN++;
        if (newFrame)
        {
            g_ov.newFrames++;
            if (gapMs > 0) g_ov.worstMs = std::max(g_ov.worstMs, gapMs);
            if (gapMs > 34.5) g_ov.late++;
        }
        double el = std::chrono::duration<double>(now - g_ov.windowStart).count();
        if (el >= 1.0)
        {
            g_ov.fps = g_ov.newFrames / el;
            g_ov.shownWorst = g_ov.worstMs;
            g_ov.workAvg = g_ov.workN ? g_ov.workSum / g_ov.workN : 0;
            g_ov.lateLastSec = g_ov.late;
            g_ov.lateHistory.push_back(g_ov.late);
            while (g_ov.lateHistory.size() > 10) g_ov.lateHistory.pop_front();
            g_ov.windowStart = now;
            g_ov.newFrames = g_ov.late = g_ov.workN = 0;
            g_ov.worstMs = g_ov.workSum = 0;
        }
    }

    // Window placement from the launcher settings: monitor, size (16:9), fullscreen.
    HMONITOR PickMonitor(int index)
    {
        struct Ctx { int want, n; HMONITOR found, primary; } c{ index, 0, nullptr, nullptr };
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = (Ctx*)lp;
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(m, &mi);
            if (mi.dwFlags & MONITORINFOF_PRIMARY) c->primary = m;
            if (c->n++ == c->want) c->found = m;
            return TRUE;
        }, (LPARAM)&c);
        return c.found ? c.found : c.primary;
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CLOSE:
            Log("[render] window closed: exiting");
            TerminateProcess(GetCurrentProcess(), 0);
            break;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED && LOWORD(lp) && HIWORD(lp))
            {
                g_pendingW = LOWORD(lp);
                g_pendingH = HIWORD(lp);
                g_resizePending = true;
            }
            break;
        // Keyboard state for input.cpp, from the window's own messages so it follows focus.
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            // Window controls (not passed to the game): F11 or Alt+Enter toggles fullscreen.
            if (wp == VK_F11 || (wp == VK_RETURN && (lp & (1 << 29))))
            {
                if (!(lp & (1 << 30))) ToggleFullscreen(hwnd);   // ignore auto-repeat
                return 0;
            }
            if (wp == VK_F2 || wp == VK_F3)   // frame counter / performance overlay
            {
                if (!(lp & (1 << 30))) (wp == VK_F2 ? g_showFps : g_showPerf) = !(wp == VK_F2 ? g_showFps : g_showPerf).load();
                return 0;
            }
            if (wp == VK_F12)   // screenshot (window control, not passed to the game)
            {
                if (!(lp & (1 << 30))) g_screenshotRequested = true;
                return 0;
            }
            SetHostKey(int(wp), true);
            if (msg == WM_SYSKEYDOWN && wp != VK_F4) return 0;   // keep Alt/F10 from opening the system menu
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            SetHostKey(int(wp), false);
            break;
        case WM_KILLFOCUS:
            ClearHostKeys();
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    bool CreateWindowAndDevice()
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"Peggle2Recomp";
        // Client size: the launcher's choice (16:9), or the largest 16:9 size that fits 85% of the
        // chosen monitor's work area; centred on that monitor.
        const Settings& set = GetSettings();
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(PickMonitor(set.monitor), &mi);
        LONG workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
        LONG cw = set.windowWidth > 0 ? set.windowWidth : std::min<LONG>(workW * 85 / 100, workH * 85 / 100 * 16 / 9);
        cw = std::max<LONG>(cw, 640);
        if (g_captureTitle) cw = LONG(kWidth);   // art capture: native 1280x720
        LONG ch = cw * 9 / 16;
        RECT r{ 0, 0, cw, ch };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        LONG ww = r.right - r.left, wh = r.bottom - r.top;
        LONG x = mi.rcWork.left + std::max<LONG>(0, (workW - ww) / 2), y = mi.rcWork.top + std::max<LONG>(0, (workH - wh) / 2);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));   // icon built from the game's logo, if present
        RegisterClassW(&wc);
        g_hwnd = CreateWindowW(wc.lpszClassName, L"Peggle 2", WS_OVERLAPPEDWINDOW | (g_captureTitle ? 0 : WS_VISIBLE),
                               x, y, ww, wh, nullptr, nullptr, wc.hInstance, nullptr);
        if (!g_hwnd) { Log("[render] CreateWindow failed (%lu)", GetLastError()); return false; }
        {
            // Black title bar with white text, like the launcher (Windows 11; dark mode elsewhere).
            BOOL dark = TRUE;
            COLORREF caption = RGB(0, 0, 0), captionText = RGB(255, 255, 255);
            DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
            DwmSetWindowAttribute(g_hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
            DwmSetWindowAttribute(g_hwnd, 36 /* DWMWA_TEXT_COLOR */, &captionText, sizeof(captionText));
        }
        g_showFps = set.fpsCounter && !g_captureTitle;
        g_showPerf = set.perfOverlay && !g_captureTitle;

        IDXGIFactory4* factory = nullptr;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) { Log("[render] DXGI factory failed"); return false; }
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device))))
        {
            Log("[render] D3D12CreateDevice failed (no D3D12 GPU?)");
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue));

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = kWidth;
        sd.Height = kHeight;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // matches the preview's CPU-side BGRA8
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBuffers;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1* sc1 = nullptr;
        if (FAILED(factory->CreateSwapChainForHwnd(g_queue, g_hwnd, &sd, nullptr, nullptr, &sc1))) { Log("[render] swap chain failed"); return false; }
        sc1->QueryInterface(IID_PPV_ARGS(&g_swapChain));
        sc1->Release();
        factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
        factory->Release();

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kBuffers;
        g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap));
        g_rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; i < kBuffers; i++)
        {
            g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]));
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(i) * g_rtvSize;
            g_device->CreateRenderTargetView(g_backBuffers[i], nullptr, h);
            g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocators[i]));
        }
        for (UINT i = 0; i < kBuffers; i++)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = UINT64(kUploadPitch) * kHeight;
            rd.Height = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_upload[i]));
            g_upload[i]->Map(0, nullptr, (void**)&g_uploadPtr[i]);
            rd.Width = UINT64(kOverlayPitch) * kOverlayH * 2;
            g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_overlayUpload[i]));
            g_overlayUpload[i]->Map(0, nullptr, (void**)&g_overlayPtr[i]);
        }
        g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0], nullptr, IID_PPV_ARGS(&g_list));
        g_list->Close();
        g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
        g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Log("[render] window + D3D12 swap chain ready (%ux%u)", kWidth, kHeight);
        g_drawReady = DrawInit(g_device, kBuffers);
        if (!g_drawReady) Log("[render] draw translation unavailable: showing the texture preview only");
        return true;
    }

    // Minimal PNG writer (stored/uncompressed deflate) for debugging snapshots of the preview.
    uint32_t Crc32(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu)
    {
        for (size_t i = 0; i < n; i++)
        {
            c ^= d[i];
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        }
        return c;
    }

    void WritePng(const char* path, const uint32_t* bgra, UINT w, UINT h, UINT pitchPixels)
    {
        std::vector<uint8_t> raw;
        raw.reserve(size_t(h) * (1 + w * 3));
        for (UINT y = 0; y < h; y++)
        {
            raw.push_back(0);   // filter: none
            for (UINT x = 0; x < w; x++)
            {
                uint32_t p = bgra[size_t(y) * pitchPixels + x];
                raw.push_back(uint8_t(p >> 16)); raw.push_back(uint8_t(p >> 8)); raw.push_back(uint8_t(p));
            }
        }
        std::vector<uint8_t> z = { 0x78, 0x01 };
        uint32_t a = 1, b = 0;
        for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
        for (size_t off = 0; off < raw.size(); off += 65535)
        {
            uint16_t len = uint16_t(std::min<size_t>(65535, raw.size() - off));
            z.push_back(off + len >= raw.size() ? 1 : 0);
            z.push_back(uint8_t(len)); z.push_back(uint8_t(len >> 8));
            z.push_back(uint8_t(~len)); z.push_back(uint8_t(~len >> 8));
            z.insert(z.end(), raw.begin() + off, raw.begin() + off + len);
        }
        uint32_t adler = (b << 16) | a;
        for (int s = 24; s >= 0; s -= 8) z.push_back(uint8_t(adler >> s));
        FILE* f = fopen(path, "wb");
        if (!f) return;
        auto be32 = [&](uint32_t v) { uint8_t t[4] = { uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v) }; fwrite(t, 1, 4, f); };
        auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
            be32(uint32_t(data.size()));
            std::vector<uint8_t> td(type, type + 4);
            td.insert(td.end(), data.begin(), data.end());
            fwrite(td.data(), 1, td.size(), f);
            be32(~Crc32(td.data(), td.size()));
        };
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        fwrite(sig, 1, 8, f);
        std::vector<uint8_t> ihdr = { uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                                      uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0 };
        chunk("IHDR", ihdr);
        chunk("IDAT", z);
        chunk("IEND", {});
        fclose(f);
    }

    // Decode the preview texture, scale it to fit (nearest), and write it into upload buffer i.
    // Returns the placed size (0 if nothing to show).
    void PreparePreview(UINT i, const GuestTextureInfo* tex, UINT& outW, UINT& outH)
    {
        outW = outH = 0;
        if (!tex) return;
        if (!DecodeTexture(*tex, g_decoded))
        {
            snprintf(g_previewDesc, sizeof(g_previewDesc), "%ux%u fmt %u (unsupported)", tex->width, tex->height, tex->format);
            return;
        }
        double scale = std::min({ 1.0, double(kWidth) / tex->width, double(kHeight) / tex->height });
        outW = std::max<UINT>(1, UINT(tex->width * scale));
        outH = std::max<UINT>(1, UINT(tex->height * scale));
        for (UINT y = 0; y < outH; y++)
        {
            uint32_t* row = (uint32_t*)(g_uploadPtr[i] + size_t(y) * kUploadPitch);
            uint32_t sy = std::min<uint32_t>(uint32_t(y / scale), tex->height - 1);
            for (UINT x = 0; x < outW; x++)
            {
                uint32_t sx = std::min<uint32_t>(uint32_t(x / scale), tex->width - 1);
                row[x] = g_decoded[size_t(sy) * tex->width + sx] | 0xFF000000u;   // show colour, ignore alpha
            }
        }
        snprintf(g_previewDesc, sizeof(g_previewDesc), "%ux%u fmt %u%s", tex->width, tex->height, tex->format, tex->tiled ? " tiled" : "");
    }

    void WaitIdle()
    {
        g_queue->Signal(g_fence, g_nextFence);
        g_fence->SetEventOnCompletion(g_nextFence++, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }

    void ResizeSwapChain()
    {
        UINT w = g_pendingW, h = g_pendingH;
        g_resizePending = false;
        if (w == g_bbW && h == g_bbH) return;
        WaitIdle();
        for (auto& bb : g_backBuffers) { bb->Release(); bb = nullptr; }
        if (FAILED(g_swapChain->ResizeBuffers(kBuffers, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
        {
            Log("[render] ResizeBuffers(%u x %u) failed", w, h);
            g_swapChain->ResizeBuffers(kBuffers, g_bbW, g_bbH, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        }
        else { g_bbW = w; g_bbH = h; }
        for (UINT i = 0; i < kBuffers; i++)
        {
            g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]));
            D3D12_CPU_DESCRIPTOR_HANDLE rh = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rh.ptr += SIZE_T(i) * g_rtvSize;
            g_device->CreateRenderTargetView(g_backBuffers[i], nullptr, rh);
        }
    }

    // Screenshots: F12 (not passed to the game) or PEGGLE2_SCREENSHOT_FRAME=<n> save the presented
    // frame to screenshots/peggle2_<time>.png.
    // Comma-separated frame numbers, each captured once.
    std::vector<uint64_t> ScreenshotFrames()
    {
        std::vector<uint64_t> v;
        if (const char* e = getenv("PEGGLE2_SCREENSHOT_FRAME"))
            for (const char* p = e; *p;)
            {
                char* end = nullptr;
                uint64_t n = strtoull(p, &end, 10);
                if (end == p) break;
                v.push_back(n);
                p = *end ? end + 1 : end;
            }
        return v;
    }
    std::vector<uint64_t> g_screenshotFrames = ScreenshotFrames();

    void SaveScreenshot(ID3D12Resource* readback, UINT pitch)
    {
        uint8_t* p = nullptr;
        if (FAILED(readback->Map(0, nullptr, (void**)&p))) return;
        std::error_code ec;
        std::filesystem::create_directories("screenshots", ec);
        SYSTEMTIME st;
        GetLocalTime(&st);
        char path[512];
        snprintf(path, sizeof(path), "screenshots/peggle2_%04u%02u%02u-%02u%02u%02u-%03u.png", st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        if (g_captureTitle) snprintf(path, sizeof(path), "%s", g_captureTitle);
        WritePng(path, (const uint32_t*)p, g_bbW, g_bbH, pitch / 4);
        readback->Unmap(0, nullptr);
        Log("[render] screenshot saved to %s", path);
        if (g_captureTitle)
        {
            Log("[render] title art captured: exiting");
            LogFlush();
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }

    void PresentFrame(uint64_t frame, const GuestTextureInfo* preview)
    {
        if (g_resizePending) ResizeSwapChain();
        UINT i = g_swapChain->GetCurrentBackBufferIndex();
        if (g_fence->GetCompletedValue() < g_fenceValues[i])
        {
            g_fence->SetEventOnCompletion(g_fenceValues[i], g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, INFINITE);
        }
        DrawList draws;
        uint32_t front = 0;
        if (g_drawReady && !g_forcePreview)
        {
            std::lock_guard<std::mutex> g(g_drawsLock);
            draws = g_latestDraws;
            front = g_latestFront;
            g_shownSerial = g_latestSerial;
        }
        UINT pw = 0, ph = 0;
        if (draws.empty()) PreparePreview(i, preview, pw, ph);
        if (pw && ph && (frame == 300 || frame == 600 || frame == 1200 || frame == 2400))
        {
            char path[64];
            snprintf(path, sizeof(path), "preview_%llu.png", (unsigned long long)frame);
            WritePng(path, (const uint32_t*)g_uploadPtr[i], pw, ph, kUploadPitch / 4);
            Log("[render] frame %llu preview (%s) saved to %s", (unsigned long long)frame, g_previewDesc, path);
        }
        g_allocators[i]->Reset();
        g_list->Reset(g_allocators[i], nullptr);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_backBuffers[i];
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_list->ResourceBarrier(1, &b);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(i) * g_rtvSize;
        float pulse = 0.5f + 0.5f * float(sin(double(frame) * 0.05));
        const float color[4] = { 0.05f, 0.10f + 0.15f * pulse, 0.25f + 0.25f * pulse, 1.0f };
        const float black[4] = { 0, 0, 0, 1 };
        g_list->ClearRenderTargetView(rtv, draws.empty() ? color : black, 0, nullptr);
        if (!draws.empty())
        {
            // Letterboxed game area in back-buffer pixels.
            float scale = std::min(float(g_bbW) / kWidth, float(g_bbH) / kHeight);
            float area[4] = { (g_bbW - kWidth * scale) * 0.5f, (g_bbH - kHeight * scale) * 0.5f, kWidth * scale, kHeight * scale };
            if (GetSettings().stretch) { area[0] = area[1] = 0; area[2] = float(g_bbW); area[3] = float(g_bbH); }   // fill, no bars
            g_lastExecuted = DrawFrame(g_list, rtv.ptr, g_bbW, g_bbH, area, draws, front, i);
            snprintf(g_previewDesc, sizeof(g_previewDesc), "%u/%zu draws", g_lastExecuted, draws.size());
            if (frame == 600 || frame == 1200 || frame == 2400 || frame == 3600)
                Log("[render] frame %llu: %u of %zu draws executed", (unsigned long long)frame, g_lastExecuted, draws.size());
        }

        if (pw > g_bbW || ph > g_bbH) pw = ph = 0;   // preview doesn't fit a small window
        if (pw && ph)
        {
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            g_list->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = g_backBuffers[i];
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = g_upload[i];
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, pw, ph, 1, kUploadPitch };
            g_list->CopyTextureRegion(&dst, (g_bbW - pw) / 2, (g_bbH - ph) / 2, 0, &src, nullptr);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        }
        else
        {
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        }
        // F2 / F3 overlay: copied over the top-left corner of the frame.
        if (g_showFps || g_showPerf)
        {
            char line[160];
            std::vector<std::pair<std::string, COLORREF>> rows;
            snprintf(line, sizeof(line), "%.0f fps", g_ov.fps);
            rows.push_back({ line, g_ov.fps >= 29.5 || g_ov.fps >= 59.5 ? RGB(120, 230, 120) : RGB(255, 200, 80) });
            if (g_showPerf)
            {
                uint32_t late10 = 0;
                for (uint32_t n : g_ov.lateHistory) late10 += n;
                snprintf(line, sizeof(line), "worst frame %.1f ms   render %.1f ms", g_ov.shownWorst, g_ov.workAvg);
                rows.push_back({ line, RGB(230, 230, 230) });
                snprintf(line, sizeof(line), "late frames: %u last second, %u last 10 s", g_ov.lateLastSec, late10);
                rows.push_back({ line, late10 ? RGB(255, 200, 80) : RGB(230, 230, 230) });
            }
            UINT packed = DrawOverlayText(rows), oh = packed & 0xFFFF, ow = packed >> 16;
            if (ow > g_bbW) ow = g_bbW;
            if (oh > g_bbH) oh = g_bbH;
            for (UINT y = 0; y < oh; y++) memcpy(g_overlayPtr[i] + size_t(y) * kOverlayPitch, g_overlayBits + size_t(y) * kOverlayW, size_t(ow) * 4);
            if (b.Transition.StateBefore != D3D12_RESOURCE_STATE_COPY_DEST)
            {
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                g_list->ResourceBarrier(1, &b);
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            }
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = g_backBuffers[i];
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = g_overlayUpload[i];
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, kOverlayW, kOverlayH * 2, 1, kOverlayPitch };
            D3D12_BOX box{ 0, 0, 0, ow, oh, 1 };
            UINT at = std::min<UINT>(12, g_bbW - ow);
            g_list->CopyTextureRegion(&dst, at, std::min<UINT>(12, g_bbH - oh), 0, &src, &box);
        }
        // Achievement pop-up: bottom centre, for a few seconds.
        if (std::string title, detail; CurrentAchievementPopup(title, detail))
        {
            UINT packed = DrawOverlayText({ { title, RGB(140, 220, 110) }, { detail, RGB(240, 240, 240) } }, 1);
            UINT ph2 = std::min<UINT>(packed & 0xFFFF, g_bbH), pw2 = std::min<UINT>(std::max<UINT>(packed >> 16, 320), std::min(kOverlayW, g_bbW));
            for (UINT y = 0; y < ph2; y++)
                memcpy(g_overlayPtr[i] + size_t(kOverlayH + y) * kOverlayPitch, g_overlayBits + size_t(kOverlayH + y) * kOverlayW, size_t(pw2) * 4);
            if (b.Transition.StateBefore != D3D12_RESOURCE_STATE_COPY_DEST)
            {
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                g_list->ResourceBarrier(1, &b);
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            }
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = g_backBuffers[i];
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = g_overlayUpload[i];
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, kOverlayW, kOverlayH * 2, 1, kOverlayPitch };
            D3D12_BOX box{ 0, kOverlayH, 0, pw2, kOverlayH + ph2, 1 };
            UINT px = (g_bbW - pw2) / 2, py = g_bbH > ph2 + 40 ? g_bbH - ph2 - 40 : 0;
            g_list->CopyTextureRegion(&dst, px, py, 0, &src, &box);
        }
        bool shot = g_screenshotRequested.exchange(false);
        if (g_captureTitle && frame == kTitleFrame) shot = true;
        if (auto it = std::find(g_screenshotFrames.begin(), g_screenshotFrames.end(), frame); it != g_screenshotFrames.end())
        {
            g_screenshotFrames.erase(it);
            shot = true;
        }
        ID3D12Resource* readback = nullptr;
        UINT shotPitch = (g_bbW * 4 + 255) & ~255u;
        if (shot)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = UINT64(shotPitch) * g_bbH;
            rd.Height = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
            {
                D3D12_RESOURCE_STATES before = b.Transition.StateBefore;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                g_list->ResourceBarrier(1, &b);
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                src.pResource = g_backBuffers[i];
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.pResource = readback;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, g_bbW, g_bbH, 1, shotPitch };
                g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                (void)before;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            }
        }
        g_list->ResourceBarrier(1, &b);
        g_list->Close();
        ID3D12CommandList* lists[] = { g_list };
        g_queue->ExecuteCommandLists(1, lists);
        g_swapChain->Present(GetSettings().vsync ? 1 : 0, 0);
        g_fenceValues[i] = g_nextFence;
        g_queue->Signal(g_fence, g_nextFence++);
        if (readback)
        {
            g_fence->SetEventOnCompletion(g_fenceValues[i], g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, INFINITE);
            SaveScreenshot(readback, shotPitch);
            readback->Release();
        }
    }

    // Host display vblank -> guest vblank (gpu.cpp), so the game's 30 fps (a flip every second
    // vblank) is locked to the monitor's refresh instead of a free-running timer that drifts
    // against it. Only for ~60 Hz displays (the guest expects 60 Hz vblanks); otherwise, or
    // with PEGGLE2_VBLANK=timer, the guest keeps its own timer.
    void VblankThread()
    {
        NameHostThread("vblank");
        if (const char* v = getenv("PEGGLE2_VBLANK"); v && strcmp(v, "timer") == 0)
        {
            Log("[render] vblank: guest timer (PEGGLE2_VBLANK=timer)");
            return;
        }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        IDXGIOutput* out = nullptr;
        int recheck = 0, fast = 0;
        int lastMode = -1;   // logged state: 0 timer, 1 host
        auto last = std::chrono::steady_clock::now();
        for (;;)
        {
            if (!out || --recheck <= 0)
            {
                if (out) { out->Release(); out = nullptr; }
                recheck = 300;   // re-pick the output every ~5 s (window moved, mode changed)
                HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFOEXW mi{};
                mi.cbSize = sizeof(mi);
                DEVMODEW dm{};
                dm.dmSize = sizeof(dm);
                DWORD hz = GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
                bool sixty = hz >= 59 && hz <= 61;
                if (sixty && FAILED(g_swapChain->GetContainingOutput(&out))) out = nullptr;
                int mode = out ? 1 : 0;
                if (mode != lastMode)
                {
                    Log("[render] vblank: %s (display %lu Hz)", mode ? "locked to the display" : "guest timer", (unsigned long)hz);
                    lastMode = mode;
                }
                if (!out) { GpuHostVblankLost(); Sleep(500); recheck = 0; continue; }
            }
            if (FAILED(out->WaitForVBlank()))
            {
                out->Release();
                out = nullptr;
                GpuHostVblankLost();
                Sleep(100);
                continue;
            }
            auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::milliseconds(8))   // not a real vblank wait (e.g. display asleep)
            {
                if (++fast > 30) { GpuHostVblankLost(); out->Release(); out = nullptr; fast = 0; lastMode = -1; Sleep(500); }
                continue;
            }
            fast = 0;
            last = now;
            GpuHostVblank();
        }
    }

    void RenderThread()
    {
        NameHostThread("render");
        if (!CreateWindowAndDevice()) return;
        g_ready = true;
        if (GetSettings().fullscreen && !g_captureTitle) ToggleFullscreen(g_hwnd);
        std::thread(VblankThread).detach();
        auto lastTitle = std::chrono::steady_clock::now();
        uint64_t lastFrames = 0;
        uint64_t lastStatFrame = 0, lastSerial = 0, dupPresents = 0, skippedLists = 0;
        std::chrono::steady_clock::time_point lastNew{}, lastStats = lastTitle;
        for (;;)
        {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            uint64_t frame;
            GuestTextureInfo preview{};
            bool hasPreview;
            {
                std::unique_lock<std::mutex> lk(g_presentLock);
                // Present only new frames: re-presenting the old one at 60 Hz made a frame that
                // arrived during that Present() wait an extra vblank (50 ms instead of 33).
                bool fresh = g_presentCv.wait_for(lk, std::chrono::milliseconds(50), [] { return g_requested != g_presented; });
                if (!fresh && !g_resizePending) continue;   // keep pumping window messages
                frame = g_requested;
                g_presented = g_requested;
                preview = g_pendingPreview;
                hasPreview = g_hasPreview;
            }
            auto workStart = std::chrono::steady_clock::now();
            PresentFrame(frame, hasPreview ? &preview : nullptr);
            auto now = std::chrono::steady_clock::now();
            double workMs = std::chrono::duration<double, std::milli>(now - workStart).count();
            g_renderWorkHist.Add(workMs);
            {
                bool isNew = frame != lastStatFrame && g_shownSerial != lastSerial;
                double gapMs = isNew && lastNew.time_since_epoch().count() ? std::chrono::duration<double, std::milli>(now - lastNew).count() : 0;
                NoteOverlayFrame(gapMs, workMs, isNew);
            }
            if (frame != lastStatFrame)   // a present the game asked for
            {
                lastStatFrame = frame;
                if (g_shownSerial == lastSerial) dupPresents++;
                else
                {
                    if (lastSerial && g_shownSerial > lastSerial + 1) skippedLists += g_shownSerial - lastSerial - 1;
                    double gap = lastNew.time_since_epoch().count() ? std::chrono::duration<double, std::milli>(now - lastNew).count() : 0;
                    if (gap) g_newFrameHist.Add(gap);
                    static int lateLogged = 0;
                    if (gap > 40 && frame > 900 && lateLogged < 40 && getenv("PEGGLE2_LATE_LOG"))
                    {
                        lateLogged++;
                        Log("[late] frame %llu shown %.1f ms after the previous; this present's render work %.1f ms (draw list %llu)",
                            (unsigned long long)frame, gap, std::chrono::duration<double, std::milli>(now - workStart).count(),
                            (unsigned long long)g_shownSerial);
                    }
                    lastNew = now;
                    lastSerial = g_shownSerial;
                }
            }
            if (now - lastStats > std::chrono::seconds(10))
            {
                lastStats = now;
                Log("[frames] %s", g_swapHist.TakeLine("game swaps").c_str());
                Log("[frames] %s; %llu repeated a frame, %llu frames never shown", g_newFrameHist.TakeLine("new frames shown").c_str(),
                    (unsigned long long)dupPresents, (unsigned long long)skippedLists);
                Log("[frames] %s", g_renderWorkHist.TakeLine("render thread per present").c_str());
                {
                    std::lock_guard<std::mutex> g(g_drawsLock);
                    if (g_flipPacing)
                        Log("[frames] guest flips: %llu showed a queued frame, %llu came before their draws, %llu frames superseded",
                            (unsigned long long)g_flipsMatched, (unsigned long long)g_flipsEarly, (unsigned long long)g_flipsUnmatched);
                    g_flipsMatched = g_flipsEarly = g_flipsUnmatched = 0;
                }
                dupPresents = skippedLists = 0;
            }
            if (now - lastTitle > std::chrono::seconds(1))
            {
                double secs = std::chrono::duration<double>(now - lastTitle).count();
                wchar_t title[160];
                if (g_showFps || g_showPerf || getenv("PEGGLE2_DEBUG_TITLE"))
                    swprintf(title, 160, L"Peggle 2  -  %.0f fps  -  frame %llu  -  %hs", double(frame - lastFrames) / secs,
                             (unsigned long long)frame, g_previewDesc);
                else
                    swprintf(title, 160, L"Peggle 2");
                SetWindowTextW(g_hwnd, title);
                lastTitle = now;
                lastFrames = frame;
            }
        }
    }
}

void RendererSubmitDraws(DrawList&& draws, uint32_t frontBuffer)
{
    {
    std::lock_guard<std::mutex> g(g_drawsLock);
    uint64_t serial = ++g_drawSerial;
    bool present = false;
    if (!g_flipPacing || g_presentOnSubmit)
        present = true;                                   // no flips seen yet: show it now
    else if (g_pendingFlips)
    {
        present = true;                                   // the title already flipped to it
        g_pendingFlips--;
        g_flipsEarly++;
    }
    if (present)
    {
        g_latestDraws = std::move(draws);
        g_latestFront = frontBuffer;
        g_latestSerial = serial;
    }
    else
    {
        g_recent.push_back({ serial, frontBuffer, std::move(draws) });
        // Lost track (more frames than flips): keep the newest two, the rest were superseded.
        while (g_recent.size() > 3) { g_recent.pop_front(); g_flipsUnmatched++; }
    }
    if (!present) return;
    }
    // With draw translation, a frame is presented when its draws are complete (here, on the GPU
    // thread), not at VdSwap, which runs ahead of the GPU thread and showed the old list again.
    if (g_drawReady && !g_forcePreview)
    {
        std::lock_guard<std::mutex> lk(g_presentLock);
        g_requested++;
    }
    g_presentCv.notify_one();
}

void RendererGuestFlip()
{
    if (g_presentOnSubmit || !g_drawReady || g_forcePreview) return;
    {
        std::lock_guard<std::mutex> g(g_drawsLock);
        if (!g_flipPacing)
        {
            g_flipPacing = true;
            g_recent.clear();
            Log("[render] presenting on the title's flips (PEGGLE2_PRESENT=submit presents on submit)");
            return;   // the frame this flip shows was already presented on submit
        }
        if (g_recent.empty())
        {
            if (g_pendingFlips < 2) g_pendingFlips++;   // its draws are still on the way
            return;
        }
        Submitted s = std::move(g_recent.front());
        g_recent.pop_front();
        g_latestDraws = std::move(s.draws);
        g_latestFront = s.front;
        g_latestSerial = s.serial;
        g_flipsMatched++;
    }
    {
        std::lock_guard<std::mutex> lk(g_presentLock);
        g_requested++;
    }
    g_presentCv.notify_one();
}

void StartRenderer()
{
    std::thread(RenderThread).detach();
}

// Called from VdSwap: one game frame finished. A frame without a usable texture keeps showing
// the previous preview, so short loading frames don't flicker.
void SavePngFile(const char* path, const uint32_t* bgra, unsigned w, unsigned h, unsigned pitchPixels)
{
    WritePng(path, bgra, w, h, pitchPixels);
}

void RendererPresent(const GuestTextureInfo* preview)
{
    {
        std::lock_guard<std::mutex> lk(g_presentLock);
        if (preview) { g_pendingPreview = *preview; g_hasPreview = true; }
        if (g_drawReady && !g_forcePreview) return;   // presented from RendererSubmitDraws
        g_requested++;
    }
    g_presentCv.notify_one();
}
