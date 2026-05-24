#include "device_c_common.hpp"

#include <cstdint>
#include <cstring>

namespace dxmt9::d3d9::devicec {

namespace {

constexpr uint32_t kD3DPRESENT_INTERVAL_ONE = 0x00000001u;
constexpr uint32_t kD3DPRESENT_INTERVAL_TWO = 0x00000002u;
constexpr uint32_t kD3DPRESENT_INTERVAL_IMMEDIATE = 0x80000000u;

}  // namespace

uint32_t usageFromD3D(uint32_t usage) {
  using namespace dxmt9::core;
  uint32_t out = 0;
  if ((usage & 0x00000001u) != 0) out |= UsageRenderTarget;
  if ((usage & 0x00000002u) != 0) out |= UsageDepthStencil;
  if ((usage & 0x00000008u) != 0) out |= UsageWriteOnly;
  if ((usage & 0x00000200u) != 0) out |= UsageDynamic;
  if ((usage & 0x00000400u) != 0) out |= UsageAutoGenMipmap;
  return out;
}

uint32_t usageToD3D(uint32_t usage) {
  using namespace dxmt9::core;
  uint32_t out = 0;
  if ((usage & UsageRenderTarget) != 0) out |= 0x00000001u;
  if ((usage & UsageDepthStencil) != 0) out |= 0x00000002u;
  if ((usage & UsageWriteOnly) != 0) out |= 0x00000008u;
  if ((usage & UsageDynamic) != 0) out |= 0x00000200u;
  if ((usage & UsageAutoGenMipmap) != 0) out |= 0x00000400u;
  return out;
}

dxmt9::core::Format fmtFromD3D(uint32_t d3d) {
  using F = dxmt9::core::Format;
  switch (d3d) {
    case 21: return F::A8R8G8B8;
    case 22: return F::X8R8G8B8;
    case 32: return F::A8B8G8R8;
    case 33: return F::X8B8G8R8;
    case 23: return F::R5G6B5;
    case 25: return F::A1R5G5B5;
    case 24: return F::X1R5G5B5;
    case 26: return F::A4R4G4B4;
    case 28: return F::A8;
    case 20: return F::R8G8B8;
    case 113: return F::A16B16G16R16F;
    case 116: return F::A32B32G32R32F;
    case 112: return F::G16R16F;
    case 111: return F::R16F;
    case 115: return F::G32R32F;
    case 114: return F::R32F;
    case 36: return F::A16B16G16R16;
    case 110: return F::Q16W16V16U16;
    case 34: return F::G16R16;
    case 35: return F::A2R10G10B10;
    case 31: return F::A2B10G10R10;
    case 50: return F::L8;
    case 81: return F::L16;
    case 51: return F::A8L8;
    case 60: return F::V8U8;
    case 63: return F::Q8W8V8U8;
    case 64: return F::V16U16;
    case 117: return F::CxV8U8;
    case 827611204: return F::DXT1;
    case 844388420: return F::DXT2;
    case 861165636: return F::DXT3;
    case 877942852: return F::DXT4;
    case 894720068: return F::DXT5;
    case 826889281: return F::ATI1;
    case 843666497: return F::ATI2;
    case 75: return F::D24S8;
    case 77: return F::D24X8;
    case 80: return F::D16;
    case 71: return F::D32;
    case 82: return F::D32F_LOCKABLE;
    case 84: return F::D32_LOCKABLE;
    case 70: return F::D16_LOCKABLE;
    case 73: return F::D15S1;
    case 79: return F::D24X4S4;
    case 83: return F::D24FS8;
    case 85: return F::S8_LOCKABLE;
    // FOURCC 'INTZ' = 0x5A544E49 — vendor depth-as-color pseudo-format.
    case 1515474505u: return F::INTZ;
    // FOURCC 'DF16' = 0x36314644 — vendor 16-bit depth-as-texture format.
    case 909198916u: return F::DF16;
    // FOURCC 'DF24' = 0x34324644 — vendor 24-bit depth-as-texture format.
    case 875710020u: return F::DF24;
    // Vendor FOURCC pseudo-formats (R-FORMAT-11..14, classification only).
    // FOURCC 'RESZ' = 0x5A534552 — MSAA depth-resolve command surface.
    case 1515406674u: return F::Resz;
    // FOURCC 'NULL' = 0x4C4C554E — null render target (depth/stencil-only).
    case 1280070990u: return F::NullRt;
    // FOURCC 'ATOC' = 0x434F5441 — alpha-to-coverage render-state token.
    case 1129272385u: return F::Atoc;
    // FOURCC 'NVDB' = 0x4244564E — NVIDIA depth-bounds probe (unsupported).
    case 1111774798u: return F::Nvdb;
    // FOURCC 'RAWZ' = 0x5A574152 — vendor depth-readback probe (unsupported).
    case 1515667794u: return F::Rawz;
    case 101: return F::INDEX16;
    case 102: return F::INDEX32;
    default: return F::Unknown;
  }
}

uint32_t fmtToD3D(dxmt9::core::Format format) {
  using F = dxmt9::core::Format;
  switch (format) {
    case F::A8R8G8B8: return 21;
    case F::X8R8G8B8: return 22;
    case F::A8B8G8R8: return 32;
    case F::X8B8G8R8: return 33;
    case F::R5G6B5: return 23;
    case F::A1R5G5B5: return 25;
    case F::X1R5G5B5: return 24;
    case F::A4R4G4B4: return 26;
    case F::A8: return 28;
    case F::R8G8B8: return 20;
    case F::A16B16G16R16F: return 113;
    case F::A32B32G32R32F: return 116;
    case F::G16R16F: return 112;
    case F::R16F: return 111;
    case F::G32R32F: return 115;
    case F::R32F: return 114;
    case F::A16B16G16R16: return 36;
    case F::Q16W16V16U16: return 110;
    case F::G16R16: return 34;
    case F::A2R10G10B10: return 35;
    case F::A2B10G10R10: return 31;
    case F::L8: return 50;
    case F::L16: return 81;
    case F::A8L8: return 51;
    case F::V8U8: return 60;
    case F::Q8W8V8U8: return 63;
    case F::V16U16: return 64;
    case F::CxV8U8: return 117;
    case F::DXT1: return 827611204u;
    case F::DXT2: return 844388420u;
    case F::DXT3: return 861165636u;
    case F::DXT4: return 877942852u;
    case F::DXT5: return 894720068u;
    case F::ATI1:
    case F::BC4: return 826889281u;
    case F::ATI2:
    case F::BC5: return 843666497u;
    case F::D24S8: return 75;
    case F::D24X8: return 77;
    case F::D16: return 80;
    case F::D32: return 71;
    case F::D32F_LOCKABLE: return 82;
    case F::D32_LOCKABLE: return 84;
    case F::D16_LOCKABLE: return 70;
    case F::D15S1: return 73;
    case F::D24X4S4: return 79;
    case F::D24FS8: return 83;
    case F::S8_LOCKABLE: return 85;
    case F::INTZ: return 1515474505u;
    case F::DF16: return 909198916u;
    case F::DF24: return 875710020u;
    case F::Resz: return 1515406674u;
    case F::NullRt: return 1280070990u;
    case F::Atoc: return 1129272385u;
    case F::Nvdb: return 1111774798u;
    case F::Rawz: return 1515667794u;
    case F::INDEX16: return 101;
    case F::INDEX32: return 102;
    case F::Unknown:
    default: return 0;
  }
}

dxmt9::core::MultiSampleType msTypeFromD3D(uint32_t d3d) {
  using M = dxmt9::core::MultiSampleType;
  switch (d3d) {
    case 0: return M::None;
    case 2: return M::Two;
    case 4: return M::Four;
    case 8: return M::Eight;
    default: return M::None;
  }
}

bool isSupportedD3DMultisample(uint32_t d3d) {
  switch (d3d) {
    case 0:
    case 2:
    case 4:
    case 8:
      return true;
    default:
      return false;
  }
}

uint32_t msTypeToD3D(dxmt9::core::MultiSampleType ms) {
  using M = dxmt9::core::MultiSampleType;
  switch (ms) {
    case M::Two: return 2;
    case M::Four: return 4;
    case M::Eight: return 8;
    case M::None:
    default: return 0;
  }
}

dxmt9::core::PresentInterval presentIntervalFromD3D(uint32_t d3d) {
  if (d3d == kD3DPRESENT_INTERVAL_IMMEDIATE) {
    return dxmt9::core::PresentInterval::Immediate;
  }
  if (d3d >= kD3DPRESENT_INTERVAL_TWO) {
    return dxmt9::core::PresentInterval::Two;
  }
  return dxmt9::core::PresentInterval::Default;
}

uint32_t presentIntervalToD3D(dxmt9::core::PresentInterval interval) {
  switch (interval) {
    case dxmt9::core::PresentInterval::Immediate:
      return kD3DPRESENT_INTERVAL_IMMEDIATE;
    case dxmt9::core::PresentInterval::Two:
      return kD3DPRESENT_INTERVAL_TWO;
    case dxmt9::core::PresentInterval::Default:
    default:
      return kD3DPRESENT_INTERVAL_ONE;
  }
}

dxmt9::core::Pool poolFromD3D(uint32_t d3d) {
  using P = dxmt9::core::Pool;
  switch (d3d) {
    case 1: return P::Managed;
    case 2: return P::SystemMem;
    case 3: return P::Scratch;
    default: return P::Default;
  }
}

uint32_t poolToD3D(dxmt9::core::Pool pool) {
  using P = dxmt9::core::Pool;
  switch (pool) {
    case P::Managed: return 1;
    case P::SystemMem: return 2;
    case P::Scratch: return 3;
    case P::Default:
    default: return 0;
  }
}

uint32_t textureTypeToResourceType(dxmt9::core::TextureType type) {
  using T = dxmt9::core::TextureType;
  switch (type) {
    case T::Volume: return 4;
    case T::Cube: return 5;
    case T::TwoD:
    case T::Array2D:
    default: return 3;
  }
}

dxmt9::core::PrimitiveType ptFromD3D(uint32_t d3d) {
  using P = dxmt9::core::PrimitiveType;
  if (d3d >= 1 && d3d <= 6) {
    return static_cast<P>(d3d - 1);
  }
  return P::TriangleList;
}

dxmt9::core::IndexType idxTypeFromD3D(uint32_t d3d) {
  return d3d == 102 ? dxmt9::core::IndexType::UInt32 : dxmt9::core::IndexType::UInt16;
}

dxmt9::core::PresentParameters ppFromC(const D9CPresentParams& c) {
  dxmt9::core::PresentParameters p;
  p.backBufferWidth = c.backBufferWidth;
  p.backBufferHeight = c.backBufferHeight;
  p.backBufferFormat = fmtFromD3D(c.backBufferFormat);
  p.backBufferCount = c.backBufferCount;
  p.windowed = c.windowed != 0;
  p.enableAutoDepthStencil = c.enableAutoDepthStencil != 0;
  p.autoDepthStencilFormat = fmtFromD3D(c.autoDepthStencilFormat);
  p.multiSampleType = msTypeFromD3D(c.multiSampleType);
  p.multiSampleQuality = c.multiSampleQuality;
  p.flags = c.flags;
  p.fullScreenRefreshRateInHz = c.fullScreenRefreshRateHz;
  p.deviceWindow = dxmt9::core::Handle{c.deviceWindow};
  p.presentationInterval = presentIntervalFromD3D(c.presentationInterval);
  p.presentationIntervalRaw = c.presentationInterval;
  p.swapEffect = c.swapEffect;
  p.discardSwapEffect = c.swapEffect != 2;
  return p;
}

dxmt9::core::DisplayModeEx dmExFromC(const D9CDisplayModeEx& c) {
  dxmt9::core::DisplayModeEx m;
  m.width = c.width;
  m.height = c.height;
  m.refreshRate = c.refreshRate;
  m.format = fmtFromD3D(c.format);
  m.scanLineOrdering = static_cast<dxmt9::core::DisplayScanLineOrdering>(c.scanLineOrdering);
  return m;
}

void fillCCaps(const dxmt9::core::DeviceCaps& src, D9CCaps* out) {
  std::memset(out, 0, sizeof(*out));
  out->deviceType = static_cast<uint32_t>(src.deviceType);
  out->caps = src.caps;
  out->caps2 = src.caps2;
  out->caps3 = src.caps3;
  out->presentationIntervals = src.presentationIntervals;
  out->cursorCaps = src.cursorCaps;
  out->primitiveMiscCaps = src.primitiveMiscCaps;
  out->rasterCaps = src.rasterCaps;
  out->zCmpCaps = src.zCmpCaps;
  out->srcBlendCaps = src.srcBlendCaps;
  out->destBlendCaps = src.destBlendCaps;
  // gap.md §C.7: AlphaCmpCaps now has a dedicated slot. The legacy
  // alphaBlendCaps slot remains populated (same value) so older PE
  // bridge code paths that still read from it continue to see the
  // correct bitmask while we migrate to alphaCmpCaps.
  out->alphaCmpCaps = src.alphaCmpCaps;
  out->alphaBlendCaps = src.alphaCmpCaps;
  out->shadeCaps = src.shadeCaps;
  out->textureCaps = src.textureCaps;
  out->textureFilterCaps = src.textureFilterCaps;
  out->cubetextureFilterCaps = src.cubetextureFilterCaps;
  out->volumeTextureFilterCaps = src.volumeTextureFilterCaps;
  out->maxAnisotropy = src.maxAnisotropy;
  out->maxUserClipPlanes = src.maxUserClipPlanes;
  out->maxVertexW = src.maxVertexW;
  out->guardBandLeft = src.guardBandLeft;
  out->guardBandRight = src.guardBandRight;
  out->guardBandTop = src.guardBandTop;
  out->guardBandBottom = src.guardBandBottom;
  out->extentsAdjust = src.extentsAdjust;
  out->stencilCaps = src.stencilCaps;
  out->lineCaps = src.lineCaps;
  out->textureBlendCaps = src.textureBlendCaps;
  out->vertexShaderVersion = src.vertexShaderVersion;
  out->pixelShaderVersion = src.pixelShaderVersion;
  out->maxVertexShaderConst = src.maxVertexShaderConst;
  out->pixelShader1xMaxValue = src.pixelShader1xMaxValue;
  out->ps20DynamicFlowControlDepth = src.ps20DynamicFlowControlDepth;
  out->ps20NumTemps = src.ps20NumTemps;
  out->ps20StaticFlowControlDepth = src.ps20StaticFlowControlDepth;
  out->ps20NumInstructionSlots = src.ps20NumInstructionSlots;
  out->vs20DynamicFlowControlDepth = src.vs20DynamicFlowControlDepth;
  out->vs20NumTemps = src.vs20NumTemps;
  out->vs20StaticFlowControlDepth = src.vs20StaticFlowControlDepth;
  out->maxTextureWidth = src.maxTextureWidth;
  out->maxTextureHeight = src.maxTextureHeight;
  out->maxVolumeExtent = src.maxVolumeExtent;
  out->maxTextureRepeat = src.maxTextureRepeat;
  out->maxAnisotropy = src.maxAnisotropy;
  out->maxPointSize = src.maxPointSize;
  out->maxPrimitiveCount = src.maxPrimitiveCount;
  out->maxVertexIndex = src.maxVertexIndex;
  out->maxStreams = src.maxStreams;
  out->maxStreamStride = src.maxStreamStride;
  out->numSimultaneousRTs = src.numSimultaneousRTs;
  out->maxVertexBlendMatrices = src.maxVertexBlendMatrices;
  out->maxVertexBlendMatrixIndex = src.maxVertexBlendMatrixIndex;
  out->fvfCaps = src.fvfCaps;
  out->textureAddressCaps = src.textureAddressCaps;
  out->volumeTextureAddressCaps = src.volumeTextureAddressCaps;
  out->maxTextureAspectRatio = src.maxTextureAspectRatio;
  out->vs20Caps = src.vs20Caps;
  out->ps20Caps = src.ps20Caps;
  out->maxSimultaneousTextures = src.maxSimultaneousTextures;
  out->maxActiveLights = src.maxActiveLights;
  out->vertexProcessingCaps = src.vertexProcessingCaps;
  out->vertexTextureFilterCaps = src.vertexTextureFilterCaps;
  out->maxVShaderInstructionsExecuted = src.maxVShaderInstructionsExecuted;
  out->maxPShaderInstructionsExecuted = src.maxPShaderInstructionsExecuted;
  out->maxVertexShader30InstructionSlots = src.maxVertexShader30InstructionSlots;
  out->maxPixelShader30InstructionSlots = src.maxPixelShader30InstructionSlots;
  out->maxTextureBlendStages = 8;
  out->devCaps = src.devCaps;
  out->devCaps2 = src.devCaps2;
  out->declTypes = src.declTypes;
  out->stretchRectFilterCaps = src.stretchRectFilterCaps;
  out->masterAdapterOrdinal = src.masterAdapterOrdinal;
  out->adapterOrdinalInGroup = src.adapterOrdinalInGroup;
  out->numberOfAdaptersInGroup = src.numberOfAdaptersInGroup;
}

uint32_t transformStateFromD3D(uint32_t state) {
  switch (state) {
    case 2u: return dxmt9::core::XFORM_VIEW;
    case 3u: return dxmt9::core::XFORM_PROJECTION;
    default:
      break;
  }
  if (state >= 16u && state < 24u) {
    return dxmt9::core::XFORM_TEXTURE_BASE + (state - 16u);
  }
  // D3DTS_WORLDMATRIX(i) is encoded as state == 256 + i for i in [0, 255].
  // Previously this mapping clamped at i < 4 (matching the legacy
  // 4-matrix FFP fixed-function blend slots) and silently dropped
  // anything beyond, which broke programmable vertex-shader skinning
  // rigs that bind more than four world matrices. Wine routes the
  // full range to wined3d without truncation; the canonical slot
  // table reserves kMaxWorldMatrices entries to match.
  if (state >= 256u && state < 256u + dxmt9::core::kMaxWorldMatrices) {
    return dxmt9::core::XFORM_WORLD_BASE + (state - 256u);
  }
  return state;
}

int32_t setShaderFloatConst(D9CDevice* d, uint32_t start, const float* data, uint32_t cnt,
                            bool pixelShader) {
  auto& state = d->dev().mutableState();
  if (pixelShader) {
    auto& consts = state.psConst;
    for (uint32_t i = 0; i < cnt && (start + i) < consts.float4.size(); ++i) {
      consts.float4[start + i][0] = data[i * 4 + 0];
      consts.float4[start + i][1] = data[i * 4 + 1];
      consts.float4[start + i][2] = data[i * 4 + 2];
      consts.float4[start + i][3] = data[i * 4 + 3];
    }
  } else {
    auto& consts = state.vsConst;
    for (uint32_t i = 0; i < cnt && (start + i) < consts.float4.size(); ++i) {
      consts.float4[start + i][0] = data[i * 4 + 0];
      consts.float4[start + i][1] = data[i * 4 + 1];
      consts.float4[start + i][2] = data[i * 4 + 2];
      consts.float4[start + i][3] = data[i * 4 + 3];
    }
  }
  return dxmt9::core::D3D_OK;
}

}  // namespace dxmt9::d3d9::devicec
