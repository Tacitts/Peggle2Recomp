// Host execution of Xenos draws (D3D12).
//
// First version, enough for the 2D menus:
//   - one render target: everything is drawn into the window's back buffer (no EDRAM model,
//     resolves are ignored), no depth;
//   - auto-indexed draws; triangle lists/strips directly, triangle fans and quad lists through a
//     generated index buffer, rectangle lists expanded on the CPU (4th corner = a + b - c);
//   - shaders: microcode translated by xenos_shader.cpp, all constants from raw register-file
//     constant buffers, texture fetch slot N = descriptor N of the draw's tables;
//   - textures: mip 0 decoded on the CPU (DecodeGuestTexture) with the fetch-constant swizzle
//     applied, cached by description + a sampled content hash;
//   - blending, colour write mask, scissor and the simple alpha-test functions from registers.
#include "runtime.h"
#include "xenos.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>

namespace
{
    constexpr uint32_t kSlotsPerDraw = 32;
    constexpr uint32_t kSrvPerFrame = 32768;
    constexpr uint32_t kSamplerHeapSize = 2048;
    constexpr uint64_t kUploadPerFrame = 64ull << 20;
    constexpr uint32_t kRawCbSize = 4096 + 32 + 512;     // float4[256], uint4[2], int4[32]
    constexpr uint32_t kSharedCbSize = 20 * 16;

    ID3D12Device* g_dev = nullptr;
    ID3D12RootSignature* g_rootSig = nullptr;
    uint32_t g_frames = 0;

    // Shader-visible heaps: SRVs are a per-frame ring, samplers are deduplicated tables.
    ID3D12DescriptorHeap* g_srvHeap = nullptr;
    ID3D12DescriptorHeap* g_samplerHeap = nullptr;
    ID3D12DescriptorHeap* g_cpuSrvHeap = nullptr;        // non-visible: one SRV per cached texture
    uint32_t g_srvSize = 0, g_samplerSize = 0;
    uint32_t g_cpuSrvNext = 1;                           // slot 0: the dummy texture
    constexpr uint32_t kCpuSrvCount = 4096;
    std::vector<uint32_t> g_cpuSrvFree;
    uint32_t g_samplerNext = 0;
    std::unordered_map<std::string, uint32_t> g_samplerTables;   // 32 descs (as bytes) -> heap offset

    struct FrameResources
    {
        ID3D12Resource* upload = nullptr;
        uint8_t* cpu = nullptr;
        uint64_t gpu = 0, used = 0;
        uint32_t srvUsed = 0;
        std::vector<ID3D12Resource*> release;            // freed when this slot is reused
    };
    std::vector<FrameResources> g_frameRes;

    ID3D12Resource* g_dummyTexture = nullptr;

    struct CachedTexture
    {
        ID3D12Resource* resource = nullptr;
        uint32_t cpuSrv = 0;
        uint64_t contentHash = 0;
        uint64_t lastFrame = 0;
    };
    std::unordered_map<std::string, CachedTexture> g_textures;
    uint64_t g_frameCounter = 0;

    std::unordered_map<std::string, ID3D12PipelineState*> g_psos;
    std::unordered_map<uint64_t, bool> g_loggedSkip;

    template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

    ID3D12Resource* CreateBuffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* r = nullptr;
        if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
        return r;
    }

    // Linear allocation from the frame's upload buffer. Returns the CPU pointer, sets gpuVa.
    uint8_t* Alloc(FrameResources& f, uint64_t size, uint64_t align, uint64_t& gpuVa)
    {
        uint64_t at = (f.used + align - 1) & ~(align - 1);
        if (at + size > kUploadPerFrame) return nullptr;
        f.used = at + size;
        gpuVa = f.gpu + at;
        return f.cpu + at;
    }

    bool CreateRootSignature()
    {
        D3D12_DESCRIPTOR_RANGE ranges[4]{};
        for (int i = 0; i < 3; i++)
        {
            ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[i].NumDescriptors = kSlotsPerDraw;
            ranges[i].BaseShaderRegister = 0;
            ranges[i].RegisterSpace = UINT(i);   // Texture2D, Texture3D, TextureCube arrays
        }
        ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        ranges[3].NumDescriptors = kSlotsPerDraw;
        ranges[3].RegisterSpace = 3;

        D3D12_ROOT_PARAMETER p[9]{};
        auto cbv = [&](int i, UINT reg, D3D12_SHADER_VISIBILITY vis) {
            p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p[i].Descriptor.ShaderRegister = reg;
            p[i].Descriptor.RegisterSpace = 4;
            p[i].ShaderVisibility = vis;
        };
        cbv(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);   // VertexShaderConstants (empty: no constant table)
        cbv(1, 1, D3D12_SHADER_VISIBILITY_PIXEL);    // PixelShaderConstants (empty)
        cbv(2, 2, D3D12_SHADER_VISIBILITY_ALL);      // SharedConstants
        cbv(3, 3, D3D12_SHADER_VISIBILITY_VERTEX);   // RawShaderConstants (VS register file)
        cbv(4, 3, D3D12_SHADER_VISIBILITY_PIXEL);    // RawShaderConstants (PS register file)
        for (int i = 0; i < 4; i++)
        {
            p[5 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[5 + i].DescriptorTable.NumDescriptorRanges = 1;
            p[5 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
            p[5 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 9;
        rs.pParameters = p;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
        {
            Log("[draw] root signature: %s", err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        bool ok = SUCCEEDED(g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rootSig)));
        blob->Release();
        return ok;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE CpuSrv(uint32_t i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_cpuSrvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * g_srvSize;
        return h;
    }

    ID3D12Resource* CreateTexture(uint32_t w, uint32_t h)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        ID3D12Resource* r = nullptr;
        if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
        return r;
    }

    void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
    }

    // Upload RGBA8 texels into a new texture (copy recorded into `list`).
    ID3D12Resource* UploadTexture(ID3D12GraphicsCommandList* list, FrameResources& f, const std::vector<uint32_t>& rgba, uint32_t w, uint32_t h)
    {
        ID3D12Resource* tex = CreateTexture(w, h);
        if (!tex) return nullptr;
        uint32_t pitch = (w * 4 + 255) & ~255u;
        ID3D12Resource* up = CreateBuffer(uint64_t(pitch) * h, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!up) { tex->Release(); return nullptr; }
        uint8_t* p = nullptr;
        up->Map(0, nullptr, (void**)&p);
        for (uint32_t y = 0; y < h; y++) memcpy(p + size_t(y) * pitch, rgba.data() + size_t(y) * w, size_t(w) * 4);
        up->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = up;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, pitch };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(list, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        f.release.push_back(up);
        return tex;
    }

    // ---- Textures ------------------------------------------------------------------------------
    // Texture fetch constant (6 dwords, Xenia's xe_gpu_texture_fetch_t):
    //   dword0: clamp_x:3 @10, clamp_y:3 @13, pitch:9 @22 (texels / 32), tiled:1 @31
    //   dword1: format:6, endian:2 @6, base_address:20 @12 (physical >> 12)
    //   dword2: width-1:13, height-1:13 @13
    //   dword3: swizzle:12 @1 (3 bits per component: 0-3 = X-W, 4 = 0, 5 = 1), mag_filter:2 @19, min_filter:2 @21
    bool DescribeTexture(const uint32_t* f, GuestTextureInfo& t)
    {
        uint32_t base = (f[1] >> 12) & 0xFFFFF;
        if (!base) return false;
        t.address = 0xA0000000u | (base << 12);
        t.width = (f[2] & 0x1FFF) + 1;
        t.height = ((f[2] >> 13) & 0x1FFF) + 1;
        t.pitch = ((f[0] >> 22) & 0x1FF) * 32;
        t.tiled = (f[0] >> 31) & 1;
        t.format = f[1] & 0x3F;
        t.endian = (f[1] >> 6) & 3;
        return t.width <= 8192 && t.height <= 8192;
    }

    uint64_t SampleHash(const GuestTextureInfo& t)
    {
        // Cheap change detection: 64 samples of 16 bytes spread over the texture's footprint.
        uint64_t bytes = uint64_t(std::max(t.pitch, t.width)) * t.height * 4;
        if (t.format == 18 || t.format == 52) bytes /= 8;               // DXT1: 0.5 byte/texel
        else if (t.format == 19 || t.format == 20 || t.format == 53 || t.format == 54) bytes /= 4;
        else if (t.format == 2) bytes /= 4;
        else if (t.format == 4 || t.format == 10) bytes /= 2;
        bytes = std::max<uint64_t>(bytes, 16);
        uint64_t h = 1469598103934665603ull;
        for (uint64_t i = 0; i < 64; i++)
        {
            uint64_t at = uint64_t(t.address) + (bytes - 16) * i / 63;
            if (at + 16 > 0xC0000000ull) break;
            const uint8_t* p = GuestPtr(uint32_t(at));
            for (int k = 0; k < 16; k++) { h ^= p[k]; h *= 1099511628211ull; }
        }
        return h;
    }

    // Decoded BGRA -> the format's native components (x, y, z, w), then the fetch swizzle.
    void ApplySwizzle(std::vector<uint32_t>& px, uint32_t format, uint32_t swizzle)
    {
        bool dxt = format == 18 || format == 19 || format == 20 || format == 52 || format == 53 || format == 54;
        for (uint32_t& p : px)
        {
            uint8_t b = uint8_t(p), g = uint8_t(p >> 8), r = uint8_t(p >> 16), a = uint8_t(p >> 24);
            uint8_t n[4];
            if (dxt || format == 4) { n[0] = r; n[1] = g; n[2] = b; n[3] = a; }       // DXT, 5_6_5
            else if (format == 6) { n[0] = b; n[1] = g; n[2] = r; n[3] = a; }         // 8_8_8_8: x = first byte
            else if (format == 2) { n[0] = r; n[1] = 0; n[2] = 0; n[3] = 255; }       // 8
            else if (format == 10) { n[0] = r; n[1] = a; n[2] = 0; n[3] = 255; }      // 8_8
            else { n[0] = r; n[1] = g; n[2] = b; n[3] = a; }
            uint8_t o[4];
            for (int c = 0; c < 4; c++)
            {
                uint32_t s = (swizzle >> (3 * c)) & 7;
                o[c] = s < 4 ? n[s] : (s == 4 ? 0 : 255);
            }
            p = o[0] | (o[1] << 8) | (o[2] << 16) | (uint32_t(o[3]) << 24);   // R8G8B8A8
        }
    }

    // CPU SRV index for the texture in fetch slot `f` (0 = dummy).
    uint32_t TextureFor(ID3D12GraphicsCommandList* list, FrameResources& fr, const uint32_t* f)
    {
        GuestTextureInfo t{};
        if (!DescribeTexture(f, t)) return 0;
        uint32_t swizzle = (f[3] >> 1) & 0xFFF;
        char key[96];
        snprintf(key, sizeof(key), "%08X %ux%u p%u f%u e%u t%u s%03X", t.address, t.width, t.height, t.pitch, t.format, t.endian, t.tiled, swizzle);
        uint64_t content = SampleHash(t);
        auto it = g_textures.find(key);
        if (it != g_textures.end() && it->second.contentHash == content)
        {
            it->second.lastFrame = g_frameCounter;
            return it->second.cpuSrv;
        }
        std::vector<uint32_t> px;
        if (!DecodeGuestTexture(t, px))
        {
            static int logged = 0;
            if (logged++ < 20) Log("[draw] texture format %u (%ux%u) not decoded", t.format, t.width, t.height);
            return 0;
        }
        ApplySwizzle(px, t.format, swizzle);
        ID3D12Resource* tex = UploadTexture(list, fr, px, t.width, t.height);
        if (!tex) return 0;
        CachedTexture& c = g_textures[key];
        if (c.resource) fr.release.push_back(c.resource);
        else
        {
            if (!g_cpuSrvFree.empty()) { c.cpuSrv = g_cpuSrvFree.back(); g_cpuSrvFree.pop_back(); }
            else if (g_cpuSrvNext < kCpuSrvCount) c.cpuSrv = g_cpuSrvNext++;
            else { tex->Release(); g_textures.erase(key); return 0; }
        }
        c.resource = tex;
        c.contentHash = content;
        c.lastFrame = g_frameCounter;
        g_dev->CreateShaderResourceView(tex, nullptr, CpuSrv(c.cpuSrv));
        return c.cpuSrv;
    }

    D3D12_SAMPLER_DESC SamplerFor(const uint32_t* f)
    {
        auto mode = [](uint32_t m) {
            switch (m)
            {
            case 0: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            case 1: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
            case 6: case 7: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            }
        };
        D3D12_SAMPLER_DESC s{};
        bool linearMag = ((f[3] >> 19) & 3) == 1, linearMin = ((f[3] >> 21) & 3) == 1;
        s.Filter = linearMin ? (linearMag ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT)
                             : (linearMag ? D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT);
        s.AddressU = mode((f[0] >> 10) & 7);
        s.AddressV = mode((f[0] >> 13) & 7);
        s.AddressW = mode((f[0] >> 16) & 7);
        s.MaxAnisotropy = 1;
        s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        return s;
    }

    // Offset of a 32-entry sampler table in the sampler heap (deduplicated, persistent).
    uint32_t SamplerTable(const D3D12_SAMPLER_DESC (&descs)[kSlotsPerDraw])
    {
        std::string key((const char*)descs, sizeof(descs));
        auto it = g_samplerTables.find(key);
        if (it != g_samplerTables.end()) return it->second;
        if (g_samplerNext + kSlotsPerDraw > kSamplerHeapSize) { g_samplerTables.clear(); g_samplerNext = 0; }
        uint32_t at = g_samplerNext;
        g_samplerNext += kSlotsPerDraw;
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_samplerHeap->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t i = 0; i < kSlotsPerDraw; i++)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE hi = h;
            hi.ptr += SIZE_T(at + i) * g_samplerSize;
            g_dev->CreateSampler(&descs[i], hi);
        }
        g_samplerTables[key] = at;
        return at;
    }

    // ---- Vertex formats ----------------------------------------------------------------------
    DXGI_FORMAT VertexDxgi(const XVertexElement& e, uint32_t& bytes)
    {
        bool s = e.isSigned, i = e.isInteger;
        switch (e.format)
        {
        case 6:  bytes = 4;  return i ? (s ? DXGI_FORMAT_R8G8B8A8_SINT : DXGI_FORMAT_R8G8B8A8_UINT) : (s ? DXGI_FORMAT_R8G8B8A8_SNORM : DXGI_FORMAT_R8G8B8A8_UNORM);
        case 7:  bytes = 4;  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case 25: bytes = 4;  return i ? (s ? DXGI_FORMAT_R16G16_SINT : DXGI_FORMAT_R16G16_UINT) : (s ? DXGI_FORMAT_R16G16_SNORM : DXGI_FORMAT_R16G16_UNORM);
        case 26: bytes = 8;  return i ? (s ? DXGI_FORMAT_R16G16B16A16_SINT : DXGI_FORMAT_R16G16B16A16_UINT) : (s ? DXGI_FORMAT_R16G16B16A16_SNORM : DXGI_FORMAT_R16G16B16A16_UNORM);
        case 31: bytes = 4;  return DXGI_FORMAT_R16G16_FLOAT;
        case 32: bytes = 8;  return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case 33: bytes = 4;  return s ? DXGI_FORMAT_R32_SINT : DXGI_FORMAT_R32_UINT;
        case 34: bytes = 8;  return s ? DXGI_FORMAT_R32G32_SINT : DXGI_FORMAT_R32G32_UINT;
        case 35: bytes = 16; return s ? DXGI_FORMAT_R32G32B32A32_SINT : DXGI_FORMAT_R32G32B32A32_UINT;
        case 36: bytes = 4;  return DXGI_FORMAT_R32_FLOAT;
        case 37: bytes = 8;  return DXGI_FORMAT_R32G32_FLOAT;
        case 38: bytes = 16; return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case 57: bytes = 12; return DXGI_FORMAT_R32G32B32_FLOAT;
        default: bytes = 4;  return DXGI_FORMAT_UNKNOWN;
        }
    }

    // d = a + b - c for one vertex element (rectangle lists): float32 and 8/16-bit unorm.
    void CombineElement(uint8_t* d, const uint8_t* a, const uint8_t* b, const uint8_t* c, const XVertexElement& e, uint32_t bytes)
    {
        bool isFloat = e.format == 36 || e.format == 37 || e.format == 38 || e.format == 57;
        if (isFloat)
        {
            for (uint32_t k = 0; k < bytes / 4; k++)
            {
                float fa, fb, fc;
                memcpy(&fa, a + 4 * k, 4); memcpy(&fb, b + 4 * k, 4); memcpy(&fc, c + 4 * k, 4);
                float r = fa + fb - fc;
                memcpy(d + 4 * k, &r, 4);
            }
        }
        else if (e.format == 6 && !e.isInteger && !e.isSigned)
        {
            for (uint32_t k = 0; k < 4; k++) d[k] = uint8_t(std::clamp(int(a[k]) + int(b[k]) - int(c[k]), 0, 255));
        }
        else if ((e.format == 25 || e.format == 26) && !e.isSigned)
        {
            for (uint32_t k = 0; k < bytes / 2; k++)
            {
                uint16_t ua, ub, uc;
                memcpy(&ua, a + 2 * k, 2); memcpy(&ub, b + 2 * k, 2); memcpy(&uc, c + 2 * k, 2);
                uint16_t r = uint16_t(std::clamp(int(ua) + int(ub) - int(uc), 0, 65535));
                memcpy(d + 2 * k, &r, 2);
            }
        }
        else memcpy(d, b, bytes);   // unsupported: copy a neighbour
    }

    // ---- Pipeline state ---------------------------------------------------------------------
    D3D12_BLEND BlendFactor(uint32_t f)
    {
        switch (f)
        {
        case 0: return D3D12_BLEND_ZERO;
        case 1: return D3D12_BLEND_ONE;
        case 4: return D3D12_BLEND_SRC_COLOR;
        case 5: return D3D12_BLEND_INV_SRC_COLOR;
        case 6: return D3D12_BLEND_SRC_ALPHA;
        case 7: return D3D12_BLEND_INV_SRC_ALPHA;
        case 8: return D3D12_BLEND_DEST_COLOR;
        case 9: return D3D12_BLEND_INV_DEST_COLOR;
        case 10: return D3D12_BLEND_DEST_ALPHA;
        case 11: return D3D12_BLEND_INV_DEST_ALPHA;
        case 12: case 14: return D3D12_BLEND_BLEND_FACTOR;
        case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
        case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
        default: return D3D12_BLEND_ONE;
        }
    }
    D3D12_BLEND AlphaFactor(uint32_t f)
    {
        D3D12_BLEND b = BlendFactor(f);
        switch (b)   // colour factors are not allowed for alpha
        {
        case D3D12_BLEND_SRC_COLOR: return D3D12_BLEND_SRC_ALPHA;
        case D3D12_BLEND_INV_SRC_COLOR: return D3D12_BLEND_INV_SRC_ALPHA;
        case D3D12_BLEND_DEST_COLOR: return D3D12_BLEND_DEST_ALPHA;
        case D3D12_BLEND_INV_DEST_COLOR: return D3D12_BLEND_INV_DEST_ALPHA;
        default: return b;
        }
    }
    D3D12_BLEND_OP BlendOp(uint32_t op)
    {
        switch (op)
        {
        case 1: return D3D12_BLEND_OP_SUBTRACT;
        case 2: return D3D12_BLEND_OP_MIN;
        case 3: return D3D12_BLEND_OP_MAX;
        case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
        default: return D3D12_BLEND_OP_ADD;
        }
    }

    ID3D12PipelineState* PipelineFor(const DrawRecord& d, const TranslatedShader& vs, const TranslatedShader& ps,
                                     const std::vector<D3D12_INPUT_ELEMENT_DESC>& layout, D3D12_PRIMITIVE_TOPOLOGY_TYPE topo)
    {
        uint32_t blend = d.Reg(xreg::RB_BLENDCONTROL0), mask = d.Reg(xreg::RB_COLOR_MASK) & 0xF;
        std::string key;
        key.resize(32);
        snprintf(key.data(), key.size(), "%016llX%08X", (unsigned long long)(d.vsHash ^ (d.psHash * 31)), blend);
        key += char('0' + mask);
        key += char('0' + topo);
        for (auto& e : layout) { key += char(e.Format); key += char(e.InputSlot); key += std::to_string(e.AlignedByteOffset); }
        auto it = g_psos.find(key);
        if (it != g_psos.end()) return it->second;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rootSig;
        pd.VS = { vs.dxil.data(), vs.dxil.size() };
        pd.PS = { ps.dxil.data(), ps.dxil.size() };
        auto& rt = pd.BlendState.RenderTarget[0];
        uint32_t cs = blend & 0x1F, cop = (blend >> 5) & 7, cd = (blend >> 8) & 0x1F;
        uint32_t as = (blend >> 16) & 0x1F, aop = (blend >> 21) & 7, ad = (blend >> 24) & 0x1F;
        rt.BlendEnable = !(cs == 1 && cd == 0 && cop == 0 && as == 1 && ad == 0 && aop == 0);
        rt.SrcBlend = BlendFactor(cs);
        rt.DestBlend = BlendFactor(cd);
        rt.BlendOp = BlendOp(cop);
        rt.SrcBlendAlpha = AlphaFactor(as);
        rt.DestBlendAlpha = AlphaFactor(ad);
        rt.BlendOpAlpha = BlendOp(aop);
        rt.RenderTargetWriteMask = UINT8(mask);
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = FALSE;
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.InputLayout = { layout.data(), UINT(layout.size()) };
        pd.PrimitiveTopologyType = topo;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        pd.SampleDesc.Count = 1;
        ID3D12PipelineState* pso = nullptr;
        HRESULT hr = g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
        if (FAILED(hr))
            Log("[draw] PSO for vs %016llX ps %016llX failed (0x%08X)", (unsigned long long)d.vsHash, (unsigned long long)d.psHash, uint32_t(hr));
        else
            Log("[draw] PSO #%zu: vs %016llX ps %016llX blend 0x%08X mask 0x%X topology %d, %zu inputs", g_psos.size() + 1,
                (unsigned long long)d.vsHash, (unsigned long long)d.psHash, blend, mask, int(topo), layout.size());
        g_psos[key] = pso;   // null on failure: don't retry every frame
        return pso;
    }

    void SkipOnce(uint64_t key, const char* why, const DrawRecord& d)
    {
        if (g_loggedSkip[key]) return;
        g_loggedSkip[key] = true;
        Log("[draw] skipping draws (vs %016llX ps %016llX prim %u): %s", (unsigned long long)d.vsHash, (unsigned long long)d.psHash, d.prim, why);
    }

    float AsFloat(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

    // Execute one draw. Returns false if it was skipped.
    bool ExecuteDraw(ID3D12GraphicsCommandList* list, FrameResources& fr, const DrawRecord& d, uint32_t rtW, uint32_t rtH)
    {
        if (!d.vsCode || !d.psCode || !d.numIndices) return false;
        uint32_t colorControl = d.Reg(xreg::RB_COLORCONTROL);
        bool alphaTest = (colorControl >> 3) & 1;
        uint32_t alphaFunc = colorControl & 7;
        if (alphaTest && alphaFunc == 0) return false;           // alpha test "never"
        if ((d.Reg(xreg::RB_COLOR_MASK) & 0xF) == 0) return false; // no colour writes (depth-only)

        auto vs = TranslateShader(d.vsHash, false, *d.vsCode);
        auto ps = TranslateShader(d.psHash, true, *d.psCode);
        if (!vs || !ps) { SkipOnce(d.vsHash ^ d.psHash, "shader translation failed", d); return false; }

        // Primitive type -> topology (+ generated indices / expanded vertices).
        D3D12_PRIMITIVE_TOPOLOGY topo;
        D3D12_PRIMITIVE_TOPOLOGY_TYPE topoType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        uint32_t n = d.numIndices;
        std::vector<uint16_t> indices;
        bool rectList = false;
        switch (d.prim)
        {
        case 4: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
        case 6: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
        case 5:   // triangle fan
            topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            for (uint32_t i = 1; i + 1 < n; i++) { indices.push_back(0); indices.push_back(uint16_t(i)); indices.push_back(uint16_t(i + 1)); }
            break;
        case 13:  // quad list
            topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            for (uint32_t q = 0; q + 3 < n; q += 4)
                for (uint32_t k : { 0u, 1u, 2u, 0u, 2u, 3u }) indices.push_back(uint16_t(q + k));
            break;
        case 8: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; rectList = true; break;
        default: SkipOnce(d.prim * 0x9E3779B97F4A7C15ull, "unsupported primitive type", d); return false;
        }

        // Input layout: one input slot per vertex fetch constant used.
        const auto& elements = vs->info.vertexElements;
        std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
        std::vector<uint32_t> slotOf(elements.size()), elementBytes(elements.size());
        std::vector<uint32_t> streamIndex;   // input slot -> index in d.streams
        std::vector<uint32_t> strides;
        for (size_t i = 0; i < elements.size(); i++)
        {
            const auto& e = elements[i];
            uint32_t bytes;
            DXGI_FORMAT fmt = VertexDxgi(e, bytes);
            if (fmt == DXGI_FORMAT_UNKNOWN) { SkipOnce(d.vsHash + e.format, "unsupported vertex format", d); return false; }
            uint32_t s = 0;
            for (; s < d.streams.size(); s++) if (d.streams[s].fetchSlot == e.fetchSlot) break;
            if (s == d.streams.size()) return false;
            uint32_t slot = uint32_t(std::find(streamIndex.begin(), streamIndex.end(), s) - streamIndex.begin());
            if (slot == streamIndex.size()) { streamIndex.push_back(s); strides.push_back(e.strideDwords * 4); }
            slotOf[i] = slot;
            elementBytes[i] = bytes;
            static const char* kTexcoord = "TEXCOORD";
            layout.push_back({ kTexcoord, e.semanticIndex, fmt, slot, e.offsetDwords * 4, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
        }

        // Vertex buffers (rect lists: 3 vertices -> 6, adding the 4th corner).
        uint32_t drawVertices = rectList ? (n / 3) * 6 : n;
        std::vector<D3D12_VERTEX_BUFFER_VIEW> views(streamIndex.size());
        for (size_t slot = 0; slot < streamIndex.size(); slot++)
        {
            const auto& src = d.streams[streamIndex[slot]].data;
            uint32_t stride = std::max<uint32_t>(strides[slot], 4);
            uint64_t size = uint64_t(stride) * drawVertices;
            uint64_t va;
            uint8_t* dst = Alloc(fr, size, 16, va);
            if (!dst) return false;
            if (!rectList)
            {
                uint64_t copy = std::min<uint64_t>(size, src.size());
                memcpy(dst, src.data(), copy);
                if (copy < size) memset(dst + copy, 0, size - copy);
            }
            else
            {
                // Diagonal from element 0 of this draw (assumed position, float32 x/y).
                const auto& e0 = elements[0];
                const auto& s0 = d.streams[streamIndex[slotOf[0]]].data;
                uint32_t stride0 = e0.strideDwords * 4;
                for (uint32_t r = 0; r + 2 < n; r += 3)
                {
                    auto vtx = [&](const std::vector<uint8_t>& s, uint32_t st, uint32_t i) -> const uint8_t* {
                        size_t at = size_t(st) * (r + i);
                        static const uint8_t zero[64] = {};
                        return at + st <= s.size() ? s.data() + at : zero;
                    };
                    float x[3], y[3];
                    for (uint32_t i = 0; i < 3; i++)
                    {
                        const uint8_t* p = vtx(s0, stride0, i) + e0.offsetDwords * 4;
                        if (e0.format == 37 || e0.format == 38 || e0.format == 57) { memcpy(&x[i], p, 4); memcpy(&y[i], p + 4, 4); }
                        else { x[i] = float(i == 1); y[i] = float(i == 2); }
                    }
                    auto dist = [&](int a, int b) { return (x[a] - x[b]) * (x[a] - x[b]) + (y[a] - y[b]) * (y[a] - y[b]); };
                    int a = 1, b = 2, c = 0;   // default: v0 is the corner
                    if (dist(0, 1) >= dist(0, 2) && dist(0, 1) >= dist(1, 2)) { a = 0; b = 1; c = 2; }
                    else if (dist(0, 2) >= dist(1, 2)) { a = 0; b = 2; c = 1; }
                    const uint8_t* va_ = vtx(src, stride, a);
                    const uint8_t* vb = vtx(src, stride, b);
                    const uint8_t* vc = vtx(src, stride, c);
                    uint8_t* out = dst + size_t(r / 3) * 6 * stride;
                    memcpy(out + 0 * stride, va_, stride);
                    memcpy(out + 1 * stride, vc, stride);
                    memcpy(out + 2 * stride, vb, stride);
                    memcpy(out + 3 * stride, va_, stride);
                    memcpy(out + 4 * stride, vb, stride);
                    uint8_t* vd = out + 5 * stride;
                    memcpy(vd, va_, stride);
                    for (size_t i = 0; i < elements.size(); i++)
                    {
                        if (slotOf[i] != slot) continue;
                        uint32_t off = elements[i].offsetDwords * 4;
                        if (off + elementBytes[i] > stride) continue;
                        CombineElement(vd + off, va_ + off, vb + off, vc + off, elements[i], elementBytes[i]);
                    }
                }
            }
            views[slot] = { va, UINT(size), stride };
        }

        // Constants: raw register files per stage, shared constants.
        auto rawCb = [&](const uint32_t* consts) -> uint64_t {
            uint64_t va;
            uint8_t* p = Alloc(fr, kRawCbSize, 256, va);
            if (!p) return 0;
            memcpy(p, consts, 4096);
            memcpy(p + 4096, d.bools, 32);
            int32_t* loops = (int32_t*)(p + 4096 + 32);
            for (uint32_t i = 0; i < 32; i++)
            {
                uint32_t l = d.loops[i];
                loops[i * 4 + 0] = int32_t(l & 0xFF);              // count
                loops[i * 4 + 1] = int32_t((l >> 8) & 0xFF);       // start
                loops[i * 4 + 2] = int32_t(int8_t((l >> 16) & 0xFF));   // step
                loops[i * 4 + 3] = 0;
            }
            return va;
        };
        uint64_t vsRaw = rawCb(d.vsConsts), psRaw = rawCb(d.psConsts);
        uint64_t sharedVa;
        uint8_t* shared = Alloc(fr, 512, 256, sharedVa);
        if (!vsRaw || !psRaw || !shared) return false;
        memset(shared, 0, 512);
        uint32_t vte = d.Reg(xreg::PA_CL_VTE_CNTL);
        float vp[6];
        for (int i = 0; i < 6; i++) vp[i] = AsFloat(d.Reg(xreg::PA_CL_VPORT_XSCALE + i));
        float* c16 = (float*)(shared + 16 * 16);
        float* c17 = (float*)(shared + 17 * 16);
        float* c18 = (float*)(shared + 18 * 16);
        float* c19 = (float*)(shared + 19 * 16);
        (void)c16;   // g_Booleans, g_SwappedTexcoords, g_HalfPixelOffset: 0
        uint32_t spec = 0;
        float alphaRef = AsFloat(d.Reg(xreg::RB_ALPHA_REF));
        if (alphaTest && (alphaFunc == 4 || alphaFunc == 6)) spec |= 2;   // SPEC_CONSTANT_ALPHA_TEST: clip(a - ref)
        c17[0] = alphaFunc == 4 ? alphaRef + 1.0f / 512.0f : alphaRef;
        memcpy(&c17[1], &spec, 4);
        memcpy(&c17[2], &vte, 4);
        c18[0] = (vte & 1) ? vp[0] : 1.0f;   // x scale
        c18[1] = (vte & 4) ? vp[2] : 1.0f;   // y scale
        c18[2] = (vte & 2) ? vp[1] : 0.0f;   // x offset
        c18[3] = (vte & 8) ? vp[3] : 0.0f;   // y offset
        c19[0] = 2.0f / float(rtW);
        c19[1] = -2.0f / float(rtH);
        c19[2] = -1.0f;
        c19[3] = 1.0f;

        // Descriptor tables: textures and samplers for all 32 fetch slots.
        if (fr.srvUsed + kSlotsPerDraw > kSrvPerFrame) return false;
        uint32_t srvBase = uint32_t(&fr - g_frameRes.data()) * kSrvPerFrame + fr.srvUsed;
        fr.srvUsed += kSlotsPerDraw;
        D3D12_SAMPLER_DESC samplers[kSlotsPerDraw];
        uint32_t used = ps->info.textureSlotMask | vs->info.textureSlotMask;
        for (uint32_t s = 0; s < kSlotsPerDraw; s++)
        {
            const uint32_t* f = d.fetch + s * 6;
            uint32_t cpu = (used & (1u << s)) ? TextureFor(list, fr, f) : 0;
            D3D12_CPU_DESCRIPTOR_HANDLE dst = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
            dst.ptr += SIZE_T(srvBase + s) * g_srvSize;
            g_dev->CopyDescriptorsSimple(1, dst, CpuSrv(cpu), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            samplers[s] = (used & (1u << s)) ? SamplerFor(f) : SamplerFor(d.fetch);
            if (!(used & (1u << s))) { samplers[s] = {}; samplers[s].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; samplers[s].AddressU = samplers[s].AddressV = samplers[s].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; samplers[s].MaxAnisotropy = 1; samplers[s].MaxLOD = D3D12_FLOAT32_MAX; }
        }
        uint32_t samplerBase = SamplerTable(samplers);

        ID3D12PipelineState* pso = PipelineFor(d, *vs, *ps, layout, topoType);
        if (!pso) return false;

        // Scissor: PA_SC_WINDOW_SCISSOR_TL/BR (x bits 0-14, y bits 16-30).
        uint32_t tl = d.Reg(xreg::PA_SC_WINDOW_SCISSOR_TL), br = d.Reg(xreg::PA_SC_WINDOW_SCISSOR_BR);
        D3D12_RECT sc{ LONG(tl & 0x7FFF), LONG((tl >> 16) & 0x7FFF), LONG(br & 0x7FFF), LONG((br >> 16) & 0x7FFF) };
        if (sc.right <= sc.left || sc.bottom <= sc.top) sc = { 0, 0, LONG(rtW), LONG(rtH) };
        sc.right = std::min<LONG>(sc.right, rtW);
        sc.bottom = std::min<LONG>(sc.bottom, rtH);

        list->SetPipelineState(pso);
        list->SetGraphicsRootConstantBufferView(0, vsRaw);
        list->SetGraphicsRootConstantBufferView(1, psRaw);
        list->SetGraphicsRootConstantBufferView(2, sharedVa);
        list->SetGraphicsRootConstantBufferView(3, vsRaw);
        list->SetGraphicsRootConstantBufferView(4, psRaw);
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
        srvGpu.ptr += UINT64(srvBase) * g_srvSize;
        D3D12_GPU_DESCRIPTOR_HANDLE smpGpu = g_samplerHeap->GetGPUDescriptorHandleForHeapStart();
        smpGpu.ptr += UINT64(samplerBase) * g_samplerSize;
        list->SetGraphicsRootDescriptorTable(5, srvGpu);
        list->SetGraphicsRootDescriptorTable(6, srvGpu);
        list->SetGraphicsRootDescriptorTable(7, srvGpu);
        list->SetGraphicsRootDescriptorTable(8, smpGpu);
        float blendFactor[4] = { AsFloat(d.Reg(0x2105)), AsFloat(d.Reg(0x2106)), AsFloat(d.Reg(0x2107)), AsFloat(d.Reg(0x2108)) };
        list->OMSetBlendFactor(blendFactor);
        list->RSSetScissorRects(1, &sc);
        list->IASetPrimitiveTopology(topo);
        if (!views.empty()) list->IASetVertexBuffers(0, UINT(views.size()), views.data());
        if (!indices.empty())
        {
            uint64_t iva;
            uint8_t* ip = Alloc(fr, indices.size() * 2, 16, iva);
            if (!ip) return false;
            memcpy(ip, indices.data(), indices.size() * 2);
            D3D12_INDEX_BUFFER_VIEW ib{ iva, UINT(indices.size() * 2), DXGI_FORMAT_R16_UINT };
            list->IASetIndexBuffer(&ib);
            list->DrawIndexedInstanced(UINT(indices.size()), 1, 0, 0, 0);
        }
        else list->DrawInstanced(drawVertices, 1, 0, 0);
        return true;
    }
}

bool DrawInit(ID3D12Device* device, uint32_t framesInFlight)
{
    g_dev = device;
    g_frames = framesInFlight;
    if (!CreateRootSignature()) return false;
    g_srvSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_samplerSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSrvPerFrame * framesInFlight;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    hd.NumDescriptors = kSamplerHeapSize;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_samplerHeap)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kCpuSrvCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_cpuSrvHeap)))) return false;

    g_frameRes.resize(framesInFlight);
    for (auto& f : g_frameRes)
    {
        f.upload = CreateBuffer(kUploadPerFrame, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!f.upload) return false;
        f.upload->Map(0, nullptr, (void**)&f.cpu);
        f.gpu = f.upload->GetGPUVirtualAddress();
    }
    // Dummy texture (1x1 opaque white) for unused fetch slots: an SRV with no resource would
    // also work for 2D, but a real one keeps Texture3D/Cube views of the slot harmless.
    g_dummyTexture = CreateTexture(1, 1);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    sd.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(g_dummyTexture, &sd, CpuSrv(0));
    Log("[draw] D3D12 draw translation ready");
    return true;
}

uint32_t DrawFrame(ID3D12GraphicsCommandList* list, size_t rtvHandle, uint32_t rtW, uint32_t rtH, const DrawList& draws, uint32_t frameSlot)
{
    if (!g_rootSig || frameSlot >= g_frameRes.size()) return 0;
    FrameResources& fr = g_frameRes[frameSlot];
    for (auto* r : fr.release) r->Release();
    fr.release.clear();
    fr.used = 0;
    fr.srvUsed = 0;
    ++g_frameCounter;

    static bool dummyReady = false;
    if (!dummyReady)
    {
        Barrier(list, g_dummyTexture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        dummyReady = true;
    }

    ID3D12DescriptorHeap* heaps[] = { g_srvHeap, g_samplerHeap };
    list->SetDescriptorHeaps(2, heaps);
    list->SetGraphicsRootSignature(g_rootSig);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{ rtvHandle };
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{ 0, 0, float(rtW), float(rtH), 0, 1 };
    list->RSSetViewports(1, &vp);

    uint32_t executed = 0;
    for (const auto& d : draws)
        if (ExecuteDraw(list, fr, *d, rtW, rtH)) executed++;

    // Drop textures unused for ~10 s (the resource goes away when this slot comes back).
    if ((g_frameCounter % 600) == 0)
    {
        for (auto it = g_textures.begin(); it != g_textures.end();)
        {
            if (g_frameCounter - it->second.lastFrame > 600)
            {
                fr.release.push_back(it->second.resource);
                g_cpuSrvFree.push_back(it->second.cpuSrv);
                it = g_textures.erase(it);
            }
            else ++it;
        }
    }
    return executed;
}
