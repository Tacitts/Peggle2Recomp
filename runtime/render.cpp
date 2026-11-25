// Host renderer: window + D3D12 swap chain.
//
// Draw translation (PM4 draws -> D3D12, XenosRecomp shaders, EDRAM resolves) builds on this
// device/queue/swap chain. Until then each frame shows a *texture preview*: the largest
// texture the game bound for a draw this frame (gpu.cpp), untiled and converted on the CPU and
// copied into the back buffer. That makes movie frames, backgrounds and UI atlases visible.
// The title shows game frames, FPS and the previewed texture.
#include "runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
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

    // Decode mip 0 of a texture into BGRA8 (w x h). Returns false for unsupported formats.
    bool DecodeTexture(const GuestTextureInfo& t, std::vector<uint32_t>& out)
    {
        enum { k_8 = 2, k_5_6_5 = 4, k_8_8_8_8 = 6, k_8_8 = 10, k_DXT1 = 18, k_DXT2_3 = 19, k_DXT4_5 = 20,
               k_DXT1_AS_16 = 52, k_DXT2_3_AS_16 = 53, k_DXT4_5_AS_16 = 54 };
        uint32_t fmt = t.format, bpb, blockDim;
        switch (fmt)
        {
        case k_8: bpb = 1; blockDim = 1; break;
        case k_5_6_5: case k_8_8: bpb = 2; blockDim = 1; break;
        case k_8_8_8_8: bpb = 4; blockDim = 1; break;
        case k_DXT1: case k_DXT1_AS_16: bpb = 8; blockDim = 4; break;
        case k_DXT2_3: case k_DXT4_5: case k_DXT2_3_AS_16: case k_DXT4_5_AS_16: bpb = 16; blockDim = 4; break;
        default: return false;
        }
        uint32_t w = t.width, h = t.height;
        uint32_t bw = (w + blockDim - 1) / blockDim, bh = (h + blockDim - 1) / blockDim;
        uint32_t pitchBlocks = std::max<uint32_t>((t.pitch ? t.pitch : w) / blockDim, bw);
        if (t.tiled) pitchBlocks = (pitchBlocks + 31) & ~31u;
        out.assign(size_t(w) * h, 0xFF000000u);
        uint8_t block[16];
        for (uint32_t by = 0; by < bh; by++)
        {
            for (uint32_t bx = 0; bx < bw; bx++)
            {
                uint32_t idx = t.tiled ? TiledOffset(bx, by, pitchBlocks, bpb) : by * pitchBlocks + bx;
                uint64_t src = uint64_t(t.address) + uint64_t(idx) * bpb;
                if (src + bpb > 0xC0000000ull) return false;   // stay inside the physical view
                memcpy(block, GuestPtr(uint32_t(src)), bpb);
                SwapBlock(block, std::max<uint32_t>(bpb, 4), t.endian);
                if (blockDim == 1)
                {
                    uint32_t& o = out[size_t(by) * w + bx];
                    if (fmt == k_8) o = 0xFF000000u | (block[0] * 0x010101u);
                    else if (fmt == k_8_8) o = (uint32_t(block[1]) << 24) | (block[0] * 0x010101u);
                    else if (fmt == k_5_6_5) o = Rgb565ToBgra(uint16_t(block[0] | (block[1] << 8)));
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

bool DecodeGuestTexture(const GuestTextureInfo& t, std::vector<uint32_t>& out) { return DecodeTexture(t, out); }

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
    std::atomic<bool> g_ready{ false };

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
    DrawList g_latestDraws;
    uint32_t g_lastExecuted = 0;
    const bool g_forcePreview = getenv("PEGGLE2_PREVIEW") != nullptr;

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
        RegisterClassW(&wc);
        RECT r{ 0, 0, LONG(kWidth), LONG(kHeight) };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        g_hwnd = CreateWindowW(wc.lpszClassName, L"Peggle 2 (recomp)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        if (!g_hwnd) { Log("[render] CreateWindow failed (%lu)", GetLastError()); return false; }

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
        if (g_drawReady && !g_forcePreview)
        {
            std::lock_guard<std::mutex> g(g_drawsLock);
            draws = g_latestDraws;
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
            g_lastExecuted = DrawFrame(g_list, rtv.ptr, g_bbW, g_bbH, area, draws, i);
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
        g_list->ResourceBarrier(1, &b);
        g_list->Close();
        ID3D12CommandList* lists[] = { g_list };
        g_queue->ExecuteCommandLists(1, lists);
        g_swapChain->Present(1, 0);
        g_fenceValues[i] = g_nextFence;
        g_queue->Signal(g_fence, g_nextFence++);
    }

    void RenderThread()
    {
        if (!CreateWindowAndDevice()) return;
        g_ready = true;
        auto lastTitle = std::chrono::steady_clock::now();
        uint64_t lastFrames = 0;
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
                g_presentCv.wait_for(lk, std::chrono::milliseconds(16), [] { return g_requested != g_presented; });
                frame = g_requested;
                g_presented = g_requested;
                preview = g_pendingPreview;
                hasPreview = g_hasPreview;
            }
            PresentFrame(frame, hasPreview ? &preview : nullptr);
            auto now = std::chrono::steady_clock::now();
            if (now - lastTitle > std::chrono::seconds(1))
            {
                double secs = std::chrono::duration<double>(now - lastTitle).count();
                wchar_t title[160];
                swprintf(title, 160, L"Peggle 2 (recomp)  -  frame %llu  -  %.0f fps  -  %hs",
                         (unsigned long long)frame, double(frame - lastFrames) / secs, g_previewDesc);
                SetWindowTextW(g_hwnd, title);
                lastTitle = now;
                lastFrames = frame;
            }
        }
    }
}

void RendererSubmitDraws(DrawList&& draws)
{
    std::lock_guard<std::mutex> g(g_drawsLock);
    g_latestDraws = std::move(draws);
}

void StartRenderer()
{
    std::thread(RenderThread).detach();
}

// Called from VdSwap: one game frame finished. A frame without a usable texture keeps showing
// the previous preview, so short loading frames don't flicker.
void RendererPresent(const GuestTextureInfo* preview)
{
    {
        std::lock_guard<std::mutex> lk(g_presentLock);
        g_requested++;
        if (preview) { g_pendingPreview = *preview; g_hasPreview = true; }
    }
    g_presentCv.notify_one();
}
