#pragma once

#include "d3d9_pe.hpp"
#include "d3d9_pe_state_shadow.hpp"

#include <span>
#include <vector>

namespace dxmt9::d3d9::pe::process_vertices {

struct FvfProcessLayout {
  UINT stride = 0;
  UINT streamStride[16]{};
  UINT positionStream = 0;
  UINT positionOffset = 0;
  UINT positionType = D3DDECLTYPE_FLOAT3;
  UINT positionBytes = 0;
  UINT normalStream = 0;
  UINT normalOffset = 0;
  UINT normalType = D3DDECLTYPE_FLOAT3;
  UINT normalBytes = 12u;
  UINT tangentStream = 0;
  UINT tangentOffset = 0;
  UINT tangentType = D3DDECLTYPE_FLOAT3;
  UINT tangentBytes = 12u;
  UINT binormalStream = 0;
  UINT binormalOffset = 0;
  UINT binormalType = D3DDECLTYPE_FLOAT3;
  UINT binormalBytes = 12u;
  UINT blendWeightStream = 0;
  UINT blendWeightOffset = 0;
  UINT blendWeightType = D3DDECLTYPE_FLOAT4;
  UINT blendWeightBytes = 0;
  UINT blendIndicesStream = 0;
  UINT blendIndicesOffset = 0;
  UINT blendIndicesType = D3DDECLTYPE_UBYTE4;
  UINT blendIndicesBytes = 0;
  UINT psizeStream = 0;
  UINT psizeOffset = 0;
  UINT diffuseStream = 0;
  UINT diffuseOffset = 0;
  UINT specularStream = 0;
  UINT specularOffset = 0;
  UINT texStream[8]{};
  UINT texOffset[8]{};
  UINT texBytes[8]{};
  UINT texType[8]{};
  UINT texCount = 0;
  struct GenericInput {
    UINT usage = 0;
    UINT usageIndex = 0;
    UINT stream = 0;
    UINT offset = 0;
    UINT type = D3DDECLTYPE_FLOAT4;
    UINT bytes = 0;
  };
  GenericInput genericInput[16]{};
  UINT genericInputCount = 0;
  bool normal = false;
  bool tangent = false;
  bool binormal = false;
  bool blendWeight = false;
  bool blendIndices = false;
  bool psize = false;
  bool diffuse = false;
  bool specular = false;
};

struct ProcessShaderReg {
  UINT type = 0;
  UINT index = 0;
};

struct ProcessShaderIo {
  int inputPosition = -1;
  int inputNormal = -1;
  int inputTangent = -1;
  int inputBinormal = -1;
  int inputBlendWeight = -1;
  int inputBlendIndices = -1;
  int inputPSize = -1;
  int inputDiffuse = -1;
  int inputSpecular = -1;
  int inputTex[8]{-1, -1, -1, -1, -1, -1, -1, -1};
  struct GenericInput {
    UINT usage = 0;
    UINT usageIndex = 0;
    int reg = -1;
  };
  GenericInput inputGeneric[16]{};
  UINT inputGenericCount = 0;
  bool inputGenericOverflow = false;
  ProcessShaderReg outputPosition{};
  ProcessShaderReg outputPSize{};
  ProcessShaderReg outputDiffuse{};
  ProcessShaderReg outputSpecular{};
  ProcessShaderReg outputTex[8]{};
  bool hasOutputPosition = false;
  bool hasOutputPSize = false;
  bool hasOutputDiffuse = false;
  bool hasOutputSpecular = false;
  bool hasOutputTex[8]{};
  UINT major = 0;
  // vs_1_x: shader does not need to DCL inputs because the v# registers have
  // fixed FFP semantics. We still default-map v0=POSITION/v3=NORMAL/v5=DIFFUSE
  // /v6=SPECULAR/v7..14=TEXCOORD0..7 so that a shader which reads those
  // registers without a DCL still binds the right stream — but we must NOT
  // require streams for inputs the shader never reads. This bitmask tracks
  // which v# registers actually appear as a source operand in the parsed
  // instructions; the SWVP-programmable validator gates require*Read() on it.
  std::uint32_t usedInputMask = 0;
};

// Returns the encoded byte size, or 0 for an unknown/unused declaration type.
UINT vertexElementTypeSize(UINT type) noexcept;
bool processFvfXyzbPosition(DWORD positionMask);
bool describeProcessFvf(DWORD fvf, FvfProcessLayout &layout);
bool describeProcessDeclaration(IDirect3DVertexDeclaration9 *declaration,
                                FvfProcessLayout &layout,
                                bool destination,
                                D9CVertexDecl *validatedRaw = nullptr);
bool analyzeSimpleProcessVertexShader(const std::vector<DWORD> &words,
                                      ProcessShaderIo &io);

// Borrowed PE-side state consumed synchronously by processVertices. The
// callee neither retains nor AddRefs any span element or COM pointer.
struct Context {
  D9CDevice *device = nullptr;
  const void *deviceIdentity = nullptr;
  DWORD fvf = 0;
  IDirect3DVertexDeclaration9 *vertexDeclaration = nullptr;
  IDirect3DVertexShader9 *vertexShader = nullptr;
  std::span<IDirect3DVertexBuffer9 *const, D9C_DRAW_PACKET_MAX_STREAMS>
      streamSources;
  std::span<const UINT, D9C_DRAW_PACKET_MAX_STREAMS> streamOffsets;
  std::span<const UINT, D9C_DRAW_PACKET_MAX_STREAMS> streamStrides;
  std::span<const UINT, D9C_DRAW_PACKET_MAX_STREAMS> streamFrequencies;
  std::span<IDirect3DBaseTexture9 *const, D9C_DRAW_PACKET_MAX_TEXTURES>
      textures;
  const PeHotStateShadow &state;
  const PeConstShadowBlock &constants;
};

HRESULT processVertices(const Context &context,
                        UINT srcStart, UINT dstIndex, UINT vertexCount,
                        IDirect3DVertexBuffer9 *dstBuffer,
                        IDirect3DVertexDeclaration9 *declaration,
                        DWORD flags) noexcept;

}  // namespace dxmt9::d3d9::pe::process_vertices
