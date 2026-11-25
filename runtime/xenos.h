// Xenos draw translation: types shared by the command processor (gpu.cpp), the shader
// translator (xenos_shader.cpp) and the D3D12 draw executor (draw.cpp).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Register indices (dwords) used by draw translation.
namespace xreg
{
    constexpr uint32_t ALU_CONSTANTS = 0x4000;      // VS c0-c255 at 0x4000, PS c0-c255 at 0x4400
    constexpr uint32_t FETCH_CONSTANTS = 0x4800;    // 32 slots x 6 dwords (texture) = 96 x 2 (vertex)
    constexpr uint32_t BOOL_CONSTANTS = 0x4900;     // 8 dwords = 256 bits
    constexpr uint32_t LOOP_CONSTANTS = 0x4908;     // 32 dwords
    constexpr uint32_t STATE_BASE = 0x2000;         // render state block captured per draw
    constexpr uint32_t STATE_COUNT = 0x400;
    constexpr uint32_t RB_SURFACE_INFO = 0x2000;
    constexpr uint32_t RB_COLOR_INFO = 0x2001;
    constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x2080;
    constexpr uint32_t PA_SC_WINDOW_SCISSOR_TL = 0x2081;
    constexpr uint32_t PA_SC_WINDOW_SCISSOR_BR = 0x2082;
    constexpr uint32_t RB_COLOR_MASK = 0x2104;
    constexpr uint32_t RB_ALPHA_REF = 0x210E;
    constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F; // XSCALE, XOFFSET, YSCALE, YOFFSET, ZSCALE, ZOFFSET
    constexpr uint32_t RB_DEPTHCONTROL = 0x2200;
    constexpr uint32_t RB_BLENDCONTROL0 = 0x2201;
    constexpr uint32_t RB_COLORCONTROL = 0x2202;
    constexpr uint32_t PA_SU_SC_MODE_CNTL = 0x2205;
    constexpr uint32_t PA_CL_VTE_CNTL = 0x2206;
}

// One vertex attribute read by a vfetch instruction of a vertex shader.
struct XVertexElement
{
    uint32_t fetchSlot = 0;        // vertex fetch constant index (0-95)
    uint32_t offsetDwords = 0;     // from the vertex start
    uint32_t strideDwords = 0;
    uint32_t format = 0;           // Xenos VertexFormat
    bool isSigned = false;         // formatCompAll
    bool isInteger = false;        // numFormatAll (0 = normalized fraction)
    uint32_t semanticIndex = 0;    // TEXCOORD<n> in the translated shader
};

// Static facts about a shader's microcode (computed once per hash).
struct ShaderInfo
{
    bool isPixel = false;
    std::vector<XVertexElement> vertexElements;   // vertex shaders
    uint32_t textureSlotMask = 0;                // texture fetch slots used
    uint32_t vertexSlotMask[3] = {};             // vertex fetch slots used (96 bits)
};

// Walk a shader's control flow and fetch instructions (microcode as big-endian dwords).
ShaderInfo AnalyzeShader(bool pixel, const std::vector<uint32_t>& beWords);

// Microcode -> HLSL (XenosRecomp) -> DXIL (dxcompiler.dll). Cached by hash; null on failure.
struct TranslatedShader
{
    ShaderInfo info;
    std::vector<uint8_t> dxil;
};
std::shared_ptr<const TranslatedShader> TranslateShader(uint64_t hash, bool pixel, const std::vector<uint32_t>& beWords);

// A vertex buffer copied out of guest memory at draw time, byte-swapped to little-endian.
struct VertexStream
{
    uint32_t fetchSlot = 0;
    std::vector<uint8_t> data;
};

// Everything needed to replay one draw on the host, captured when the GPU thread executes it.
struct DrawRecord
{
    uint64_t vsHash = 0, psHash = 0;
    std::shared_ptr<const std::vector<uint32_t>> vsCode, psCode;
    uint32_t prim = 0, numIndices = 0;
    uint32_t vsConsts[1024], psConsts[1024];   // 256 float4 each
    uint32_t bools[8], loops[32];
    uint32_t fetch[32 * 6];
    uint32_t state[xreg::STATE_COUNT];
    std::vector<VertexStream> streams;
    uint32_t Reg(uint32_t index) const { return state[index - xreg::STATE_BASE]; }
};

using DrawList = std::vector<std::shared_ptr<const DrawRecord>>;

// GPU thread -> renderer: the draws of one finished frame (at VdSwap's marker).
void RendererSubmitDraws(DrawList&& draws);

// Host draw execution (draw.cpp), called from the render thread (render.cpp).
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
bool DrawInit(ID3D12Device* device, uint32_t framesInFlight);
// Record `draws` into `list` (render target already bound state: RENDER_TARGET), using the
// per-frame resources of `frameSlot` (the caller has waited for that slot's previous frame).
// Returns the number of draws executed.
// `gameArea` = {x, y, width, height}: where the game's 1280x720 frame goes in the render target.
uint32_t DrawFrame(ID3D12GraphicsCommandList* list, size_t rtvHandle, uint32_t rtWidth, uint32_t rtHeight,
                   const float gameArea[4], const DrawList& draws, uint32_t frameSlot);

// Texture decoding shared with the preview (render.cpp): mip 0 to BGRA8.
struct GuestTextureInfo;
bool DecodeGuestTexture(const GuestTextureInfo& t, std::vector<uint32_t>& out);
