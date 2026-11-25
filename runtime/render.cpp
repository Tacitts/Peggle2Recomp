// Host renderer: window + D3D12 swap chain.
//
// First milestone of the renderer: a window that presents once per game frame (VdSwap).
// Draw translation (PM4 draws -> D3D12, XenosRecomp shaders, textures, EDRAM resolves) builds
// on this device/queue/swap chain. For now each frame is cleared to a colour that pulses with
// the frame counter, and the title shows game frames and FPS so progress is visible.
#include "runtime.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

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

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_CLOSE)
        {
            Log("[render] window closed: exiting");
            TerminateProcess(GetCurrentProcess(), 0);
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
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
        g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0], nullptr, IID_PPV_ARGS(&g_list));
        g_list->Close();
        g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
        g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Log("[render] window + D3D12 swap chain ready (%ux%u)", kWidth, kHeight);
        return true;
    }

    void PresentFrame(uint64_t frame)
    {
        UINT i = g_swapChain->GetCurrentBackBufferIndex();
        if (g_fence->GetCompletedValue() < g_fenceValues[i])
        {
            g_fence->SetEventOnCompletion(g_fenceValues[i], g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, INFINITE);
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
        g_list->ClearRenderTargetView(rtv, color, 0, nullptr);

        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
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
            {
                std::unique_lock<std::mutex> lk(g_presentLock);
                g_presentCv.wait_for(lk, std::chrono::milliseconds(16), [] { return g_requested != g_presented; });
                frame = g_requested;
                g_presented = g_requested;
            }
            PresentFrame(frame);
            auto now = std::chrono::steady_clock::now();
            if (now - lastTitle > std::chrono::seconds(1))
            {
                double secs = std::chrono::duration<double>(now - lastTitle).count();
                wchar_t title[160];
                swprintf(title, 160, L"Peggle 2 (recomp)  -  game frame %llu  -  %.0f fps  -  renderer: clear only, draws not translated yet",
                         (unsigned long long)frame, double(frame - lastFrames) / secs);
                SetWindowTextW(g_hwnd, title);
                lastTitle = now;
                lastFrames = frame;
            }
        }
    }
}

void StartRenderer()
{
    std::thread(RenderThread).detach();
}

// Called from VdSwap: one game frame finished.
void RendererPresent()
{
    {
        std::lock_guard<std::mutex> lk(g_presentLock);
        g_requested++;
    }
    g_presentCv.notify_one();
}
