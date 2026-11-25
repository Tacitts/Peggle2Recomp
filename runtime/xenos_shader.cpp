// Shader translation for draws: raw Xenos microcode (from IM_LOAD packets) -> HLSL -> DXIL.
//
// XenosRecomp translates shader *containers* (the XDK's compiled shader format: constant
// table, vertex element and interpolator tables, then the microcode). The GPU only ever sees
// the microcode, so a minimal container is synthesized around it:
//   - empty constant table: every constant reads the raw register file (RAW_CONSTANT etc.)
//     and texture fetch slot N uses descriptor N (see xenosrecomp-peggle2.patch),
//   - one vertex element per vfetch instruction, as TEXCOORD<n> in program order,
//   - interpolators 0-15 as TEXCOORD0-15 (VS export register n <-> PS input register n),
//   - pixel shader output: colour 0.
// The HLSL is then adjusted for this runtime (spec constants and the Xenos viewport transform
// come from the shared constant buffer) and compiled with dxcompiler.dll, loaded at runtime.
// pch.h (XenosRecomp: Windows, dxcapi, fmt, be<>) is force-included; XenosRecomp's headers go
// before runtime.h, whose ppc_context.h defines short macros.
#include "shader_recompiler.h"

#include <mutex>
#include <unordered_map>

#include "xenos.h"

// From runtime.h, which can't be included here: its STATUS_* constants clash with the
// <windows.h> macros pulled in by XenosRecomp's pch.h.
void Log(const char* fmt, ...);
std::filesystem::path UserDataDir();

namespace
{
    // XenosRecomp's shader_common.h, embedded at build time (CMake).
    const char kShaderCommon[] =
#include "shader_common_text.inc"
        ;

    uint32_t Swap(uint32_t v) { return __builtin_bswap32(v); }

    // Visit each fetch instruction reached by an exec block of the control-flow program.
    template <class F>
    void ForEachFetch(const std::vector<uint32_t>& be, F&& visit)
    {
        size_t total = be.size();
        size_t cfEnd = total;   // control flow ends where the first exec block's code starts
        for (size_t pos = 0; pos + 3 <= std::min(cfEnd, total); pos += 3)
        {
            uint32_t w0 = Swap(be[pos]), w1 = Swap(be[pos + 1]), w2 = Swap(be[pos + 2]);
            union { ControlFlowInstruction cf[2]; struct { uint32_t c0, c1, c2, c3; }; } u{};
            u.c0 = w0;
            u.c1 = w1 & 0xFFFF;
            u.c2 = (w1 >> 16) | (w2 << 16);
            u.c3 = w2 >> 16;
            for (auto& cf : u.cf)
            {
                switch (cf.opcode)
                {
                case ControlFlowOpcode::Exec: case ControlFlowOpcode::ExecEnd:
                case ControlFlowOpcode::CondExec: case ControlFlowOpcode::CondExecEnd:
                case ControlFlowOpcode::CondExecPred: case ControlFlowOpcode::CondExecPredEnd:
                case ControlFlowOpcode::CondExecPredClean: case ControlFlowOpcode::CondExecPredCleanEnd:
                {
                    uint32_t address = cf.exec.address, count = cf.exec.count, sequence = cf.exec.sequence;
                    if (address) cfEnd = std::min<size_t>(cfEnd, size_t(address) * 3);
                    for (uint32_t i = 0; i < count; i++, sequence >>= 2)
                    {
                        size_t at = size_t(address + i) * 3;
                        if (!(sequence & 1) || at + 3 > total) continue;
                        union { VertexFetchInstruction vf; TextureFetchInstruction tf; uint32_t d[3]; } f{};
                        f.d[0] = Swap(be[at]);
                        f.d[1] = Swap(be[at + 1]);
                        f.d[2] = Swap(be[at + 2]);
                        visit(address + i, f.vf, f.tf);
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }
    }

    struct BeWriter
    {
        std::vector<uint8_t> bytes;
        size_t Put(uint32_t v)
        {
            size_t at = bytes.size();
            for (int s = 24; s >= 0; s -= 8) bytes.push_back(uint8_t(v >> s));
            return at;
        }
        void Set(size_t at, uint32_t v) { for (int i = 0; i < 4; i++) bytes[at + i] = uint8_t(v >> (24 - 8 * i)); }
    };

    // Build a minimal shader container around the microcode (see the file comment).
    std::vector<uint8_t> BuildContainer(bool pixel, const std::vector<uint32_t>& be, const ShaderInfo& info,
                                        std::vector<std::pair<uint32_t, uint32_t>> vfetchAddresses)
    {
        constexpr uint32_t kUsageTexCoord = 5;
        BeWriter w;
        // ShaderContainer
        w.Put(0x102A1100u | (pixel ? 0u : 1u));   // flags: bit 0 set = vertex shader
        size_t virtualSizeAt = w.Put(0);
        w.Put(uint32_t(be.size() * 4));            // physical size
        w.Put(0);                                  // fieldC
        size_t constantTableAt = w.Put(0);
        w.Put(0);                                  // no definition table
        size_t shaderAt = w.Put(0);
        w.Put(0);                                  // field1C
        w.Put(0);                                  // field20
        // ConstantTableContainer: size + ConstantTable (7 dwords), no constants
        w.Set(constantTableAt, uint32_t(w.bytes.size()));
        w.Put(32);
        w.Put(28); w.Put(0); w.Put(0); w.Put(0); w.Put(28); w.Put(0); w.Put(0);
        // Shader
        w.Set(shaderAt, uint32_t(w.bytes.size()));
        w.Put(0);                                  // physicalOffset (from the end of the virtual part)
        w.Put(uint32_t(be.size() * 4));            // size
        w.Put(0);                                  // field8
        w.Put(0xFF00);                             // fieldC: no SV_Position register
        w.Put(0);                                  // field10
        w.Put(16u << 5);                           // interpolatorInfo: 16 interpolators
        w.Put(0);                                  // field18: tables start right after
        if (pixel)
        {
            w.Put(1);                              // outputs: colour 0
            for (uint32_t i = 0; i < 16; i++) w.Put(i | (kUsageTexCoord << 4) | (i << 8));
        }
        else
        {
            w.Put(uint32_t(vfetchAddresses.size()));
            w.Put(0);                              // field20
            for (auto& [address, semantic] : vfetchAddresses)
                w.Put((address & 0xFFF) | (kUsageTexCoord << 12) | ((semantic & 0xF) << 16));
            for (uint32_t i = 0; i < 16; i++) w.Put(i | (kUsageTexCoord << 4) | (i << 8));
        }
        w.Set(virtualSizeAt, uint32_t(w.bytes.size()));
        for (uint32_t v : be) { uint8_t b[4]; memcpy(b, &v, 4); w.bytes.insert(w.bytes.end(), b, b + 4); }   // already big-endian
        (void)info;
        return w.bytes;
    }

    void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
    {
        for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
            s.replace(p, from.size(), to);
    }

    std::string RuntimeInclude()
    {
        std::string inc = kShaderCommon;
        // Extra shared constants (D3D12 branch of DEFINE_SHARED_CONSTANTS; see draw.cpp).
        ReplaceAll(inc, "float g_AlphaThreshold : packoffset(c17.x);",
                   "float g_AlphaThreshold : packoffset(c17.x); \\\n"
                   "    uint g_SpecConstantsValue : packoffset(c17.y); \\\n"
                   "    uint g_VteFlags : packoffset(c17.z); \\\n"
                   "    float4 g_ViewportScaleOffset : packoffset(c18); \\\n"
                   "    float4 g_ScreenToNdc : packoffset(c19);");
        return inc;
    }

    std::string AdjustHlsl(std::string hlsl, bool pixel)
    {
        // Spec constants come from the shared constant buffer instead of a linked library.
        size_t entry = hlsl.find("#ifndef __spirv__\n[shader(");
        if (entry != std::string::npos)
            hlsl.insert(entry, "uint g_SpecConstants() { return g_SpecConstantsValue; }\n\n");
        if (!pixel)
        {
            // Xenos viewport transform (PA_CL_VTE_CNTL): optional divide by w, then viewport
            // scale/offset to render-target pixels; convert those to host clip space.
            ReplaceAll(hlsl, "oPos.xy += g_HalfPixelOffset * oPos.w;",
                       "{ float2 xenosScreen = ((g_VteFlags & 0x100) != 0 ? oPos.xy : oPos.xy / oPos.w) * g_ViewportScaleOffset.xy + g_ViewportScaleOffset.zw;"
                       " oPos.xy = (xenosScreen * g_ScreenToNdc.xy + g_ScreenToNdc.zw) * oPos.w; oPos.z = 0.0; }");
        }
        return hlsl;
    }

    // ---- DXC (dxcompiler.dll next to the executable) ----------------------------------------
    IDxcCompiler3* Dxc()
    {
        static IDxcCompiler3* compiler = [] () -> IDxcCompiler3* {
            HMODULE dll = LoadLibraryW((UserDataDir().parent_path() / "dxcompiler.dll").wstring().c_str());
            if (!dll) { Log("[shader] dxcompiler.dll not found next to the executable"); return nullptr; }
            auto create = (DxcCreateInstanceProc)GetProcAddress(dll, "DxcCreateInstance");
            IDxcCompiler3* c = nullptr;
            if (!create || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&c)))) { Log("[shader] DxcCreateInstance failed"); return nullptr; }
            return c;
        }();
        return compiler;
    }

    bool CompileDxil(const std::string& hlsl, bool pixel, std::vector<uint8_t>& out, std::string& errors)
    {
        IDxcCompiler3* dxc = Dxc();
        if (!dxc) { errors = "no DXC"; return false; }
        DxcBuffer src{ hlsl.data(), hlsl.size(), DXC_CP_UTF8 };
        const wchar_t* args[] = { L"-T", pixel ? L"ps_6_0" : L"vs_6_0", L"-HV", L"2021", L"-all-resources-bound",
                                  L"-Wno-ignored-attributes", L"-Qstrip_debug", L"-O3" };
        IDxcResult* result = nullptr;
        if (FAILED(dxc->Compile(&src, args, UINT32(std::size(args)), nullptr, IID_PPV_ARGS(&result))) || !result)
        {
            errors = "Compile call failed";
            return false;
        }
        HRESULT status = E_FAIL;
        result->GetStatus(&status);
        IDxcBlobUtf8* err = nullptr;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&err), nullptr)) && err)
        {
            errors.assign(err->GetStringPointer(), err->GetStringLength());
            err->Release();
        }
        bool ok = false;
        IDxcBlob* obj = nullptr;
        if (SUCCEEDED(status) && SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) && obj)
        {
            auto* p = (const uint8_t*)obj->GetBufferPointer();
            out.assign(p, p + obj->GetBufferSize());
            obj->Release();
            ok = true;
        }
        result->Release();
        return ok;
    }

    std::mutex g_cacheLock;
    std::unordered_map<uint64_t, std::shared_ptr<const TranslatedShader>> g_cache;
    std::unordered_map<uint64_t, bool> g_failed;
}

ShaderInfo AnalyzeShader(bool pixel, const std::vector<uint32_t>& be)
{
    ShaderInfo info;
    info.isPixel = pixel;
    uint32_t lastSlot = 0, lastStride = 0, semantic = 0;
    ForEachFetch(be, [&](uint32_t address, const VertexFetchInstruction& vf, const TextureFetchInstruction& tf) {
        (void)address;
        if (vf.opcode == FetchOpcode::VertexFetch)
        {
            uint32_t slot = vf.constIndex * 3 + vf.constIndexSelect;
            if (!vf.isMiniFetch) { lastSlot = slot; lastStride = vf.stride; }
            XVertexElement e;
            e.fetchSlot = vf.isMiniFetch ? lastSlot : slot;
            e.strideDwords = vf.isMiniFetch ? lastStride : vf.stride;
            e.offsetDwords = uint32_t(vf.offset);
            e.format = vf.format;
            e.isSigned = vf.formatCompAll;
            e.isInteger = vf.numFormatAll;
            e.semanticIndex = semantic++;
            info.vertexElements.push_back(e);
            info.vertexSlotMask[e.fetchSlot / 32] |= 1u << (e.fetchSlot % 32);
        }
        else if (tf.opcode == FetchOpcode::TextureFetch)
        {
            info.textureSlotMask |= 1u << tf.constIndex;
        }
    });
    return info;
}

std::shared_ptr<const TranslatedShader> TranslateShader(uint64_t hash, bool pixel, const std::vector<uint32_t>& be)
{
    {
        std::lock_guard<std::mutex> g(g_cacheLock);
        auto it = g_cache.find(hash);
        if (it != g_cache.end()) return it->second;
        if (g_failed.count(hash)) return nullptr;
    }
    auto t = std::make_shared<TranslatedShader>();
    t->info = AnalyzeShader(pixel, be);
    std::vector<std::pair<uint32_t, uint32_t>> vfetch;
    ForEachFetch(be, [&](uint32_t address, const VertexFetchInstruction& vf, const TextureFetchInstruction&) {
        if (vf.opcode == FetchOpcode::VertexFetch) vfetch.push_back({ address, uint32_t(vfetch.size()) });
    });

    std::string hlsl, errors;
    bool ok = false;
    if (vfetch.size() <= 16)
    {
        auto container = BuildContainer(pixel, be, t->info, vfetch);
        static const std::string include = RuntimeInclude();
        ShaderRecompiler recompiler;
        recompiler.recompile(container.data(), include);
        hlsl = AdjustHlsl(std::move(recompiler.out), pixel);
        ok = CompileDxil(hlsl, pixel, t->dxil, errors);
    }
    else errors = "more than 16 vertex fetches";

    // Keep the HLSL next to the microcode for inspection.
    auto dir = UserDataDir().parent_path() / "microcode";
    char name[48];
    snprintf(name, sizeof(name), "%s_%016llX.hlsl", pixel ? "ps" : "vs", (unsigned long long)hash);
    if (FILE* f = fopen((dir / name).string().c_str(), "wb")) { fwrite(hlsl.data(), 1, hlsl.size(), f); fclose(f); }

    std::lock_guard<std::mutex> g(g_cacheLock);
    if (!ok)
    {
        g_failed[hash] = true;
        Log("[shader] %s %016llX: translation failed: %.1500s", pixel ? "ps" : "vs", (unsigned long long)hash, errors.c_str());
        return nullptr;
    }
    Log("[shader] %s %016llX: %zu bytes DXIL, %zu vertex elements, textures 0x%X", pixel ? "ps" : "vs",
        (unsigned long long)hash, t->dxil.size(), t->info.vertexElements.size(), t->info.textureSlotMask);
    g_cache[hash] = t;
    return t;
}
