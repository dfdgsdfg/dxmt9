#include "core_private.hpp"
#include "dxmt9/core.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace dxmt9::core {

namespace {

bool isCompressedFormatImpl(Format format) {
  switch (format) {
  case Format::DXT1:
  case Format::DXT2:
  case Format::DXT3:
  case Format::DXT4:
  case Format::DXT5:
  case Format::ATI1:
  case Format::BC4:
  case Format::ATI2:
  case Format::BC5:
    return true;
  default:
    return false;
  }
}

struct FormatEntry {
  FormatInfo info;
};

const std::vector<FormatEntry> &formatEntries() {
  static const std::vector<FormatEntry> entries = {
      {{Format::A8R8G8B8, BackendPixelFormat::BGRA8Unorm, FormatClass::Required,
        4, true, false, false, true}},
      {{Format::X8R8G8B8, BackendPixelFormat::BGRA8Unorm, FormatClass::Required,
        4, true, false, false, true}},
      {{Format::A8B8G8R8, BackendPixelFormat::RGBA8Unorm, FormatClass::Required,
        4, true, false, false, true}},
      {{Format::X8B8G8R8, BackendPixelFormat::RGBA8Unorm, FormatClass::Required,
        4, true, false, false, true}},
      {{Format::R5G6B5, BackendPixelFormat::B5G6R5Unorm, FormatClass::Required,
        2, true, false, false, true}},
      {{Format::A1R5G5B5, BackendPixelFormat::BGR5A1Unorm,
        FormatClass::Required, 2, true, false, false, true}},
      {{Format::X1R5G5B5, BackendPixelFormat::BGR5A1Unorm,
        FormatClass::Required, 2, true, false, false, true}},
      {{Format::A4R4G4B4, BackendPixelFormat::ABGR4Unorm, FormatClass::Required,
        2, true, false, false, true}},
      {{Format::A8, BackendPixelFormat::A8Unorm, FormatClass::Required, 1, true,
        false, false, true}},
      {{Format::R8G8B8, BackendPixelFormat::Unknown, FormatClass::Unsupported,
        3, false, false, false, true}},
      {{Format::A16B16G16R16F, BackendPixelFormat::RGBA16Float,
        FormatClass::Required, 8, true, false, false, true}},
      {{Format::A32B32G32R32F, BackendPixelFormat::RGBA32Float,
        FormatClass::Required, 16, true, false, false, true}},
      {{Format::G16R16F, BackendPixelFormat::RG16Float, FormatClass::Required,
        4, true, false, false, true}},
      {{Format::R16F, BackendPixelFormat::R16Float, FormatClass::Required, 2,
        true, false, false, true}},
      {{Format::G32R32F, BackendPixelFormat::RG32Float, FormatClass::Required,
        8, true, false, false, true}},
      {{Format::R32F, BackendPixelFormat::R32Float, FormatClass::Required, 4,
        true, false, false, true}},
      {{Format::A16B16G16R16, BackendPixelFormat::RGBA16Unorm,
        FormatClass::Required, 8, true, false, false, true}},
      {{Format::G16R16, BackendPixelFormat::RG16Unorm, FormatClass::Required, 4,
        true, false, false, true}},
      {{Format::A2R10G10B10, BackendPixelFormat::RGB10A2Unorm,
        FormatClass::Required, 4, true, false, false, true}},
      {{Format::A2B10G10R10, BackendPixelFormat::BGR10A2Unorm,
        FormatClass::Optional, 4, true, false, false, true}},
      {{Format::L8, BackendPixelFormat::R8Unorm, FormatClass::Required, 1,
        false, false, false, true}},
      {{Format::L16, BackendPixelFormat::R16Unorm, FormatClass::Required, 2,
        false, false, false, true}},
      {{Format::A8L8, BackendPixelFormat::RG8Unorm, FormatClass::Required, 2,
        false, false, false, true}},
      {{Format::V8U8, BackendPixelFormat::RG8Snorm, FormatClass::Required, 2,
        true, false, false, true}},
      {{Format::Q8W8V8U8, BackendPixelFormat::RGBA8Snorm, FormatClass::Required,
        4, true, false, false, true}},
      {{Format::V16U16, BackendPixelFormat::RG16Snorm, FormatClass::Required, 4,
        true, false, false, true}},
      {{Format::CxV8U8, BackendPixelFormat::Unknown, FormatClass::Unsupported,
        0, false, false, false, true}},
      {{Format::DXT1, BackendPixelFormat::BC1_RGBA, FormatClass::Required, 0,
        false, false, true, true}},
      {{Format::DXT2, BackendPixelFormat::BC2_RGBA, FormatClass::Required, 0,
        false, false, true, true}},
      {{Format::DXT3, BackendPixelFormat::BC2_RGBA, FormatClass::Required, 0,
        false, false, true, true}},
      {{Format::DXT4, BackendPixelFormat::BC3_RGBA, FormatClass::Required, 0,
        false, false, true, true}},
      {{Format::DXT5, BackendPixelFormat::BC3_RGBA, FormatClass::Required, 0,
        false, false, true, true}},
      {{Format::ATI1, BackendPixelFormat::BC4_RUnorm, FormatClass::Required, 0,
        false, false, true, true}},
      {{Format::BC4, BackendPixelFormat::BC4_RUnorm, FormatClass::Required, 0,
        false, false, true, true}},
      {{Format::ATI2, BackendPixelFormat::BC5_RGUnorm, FormatClass::Required, 0,
        false, false, true, true}},
      {{Format::BC5, BackendPixelFormat::BC5_RGUnorm, FormatClass::Required, 0,
        false, false, true, true}},
      {{Format::D24S8, BackendPixelFormat::Depth24Unorm_Stencil8,
        FormatClass::Required, 4, false, true, false, true}},
      {{Format::D24X8, BackendPixelFormat::Depth24Unorm_Stencil8,
        FormatClass::Required, 4, false, true, false, true}},
      {{Format::D16, BackendPixelFormat::Depth16Unorm, FormatClass::Required, 2,
        false, true, false, true}},
      {{Format::D32, BackendPixelFormat::Depth32Float, FormatClass::Required, 4,
        false, true, false, true}},
      {{Format::D32F_LOCKABLE, BackendPixelFormat::Depth32Float,
        FormatClass::Required, 4, false, true, false, true}},
      {{Format::D16_LOCKABLE, BackendPixelFormat::Depth16Unorm,
        FormatClass::Required, 2, false, true, false, true}},
      {{Format::D15S1, BackendPixelFormat::Unknown, FormatClass::Unsupported, 0,
        false, false, false, true}},
      {{Format::D24X4S4, BackendPixelFormat::Unknown, FormatClass::Unsupported,
        0, false, false, false, true}},
      {{Format::D24FS8, BackendPixelFormat::Depth32Float_Stencil8,
        FormatClass::Optional, 8, false, true, false, true}},
      {{Format::S8_LOCKABLE, BackendPixelFormat::Unknown,
        FormatClass::Unsupported, 1, false, false, false, true}},
      {{Format::INDEX16, BackendPixelFormat::Unknown, FormatClass::Required, 2,
        false, false, false, true}},
      {{Format::INDEX32, BackendPixelFormat::Unknown, FormatClass::Required, 4,
        false, false, false, true}},
  };
  return entries;
}

} // namespace

const std::vector<FormatInfo> &formatTable() {
  static const std::vector<FormatInfo> table = [] {
    std::vector<FormatInfo> out;
    out.reserve(formatEntries().size());
    for (const auto &entry : formatEntries()) {
      out.push_back(entry.info);
    }
    return out;
  }();
  return table;
}

const FormatInfo *findFormatInfo(Format format) {
  for (const auto &entry : formatTable()) {
    if (entry.format == format) {
      return &entry;
    }
  }
  return nullptr;
}

FormatClass classifyFormat(Format format) {
  if (const auto *info = findFormatInfo(format)) {
    return info->support;
  }
  return FormatClass::Unsupported;
}

BackendPixelFormat backendPixelFormat(Format format) {
  if (const auto *info = findFormatInfo(format)) {
    return info->backendFormat;
  }
  return BackendPixelFormat::Unknown;
}

u32 bytesPerPixel(Format format) {
  if (const auto *info = findFormatInfo(format)) {
    return info->bytesPerPixel;
  }
  return 0;
}

bool isCompressedFormat(Format format) {
  return isCompressedFormatImpl(format);
}

u32 formatBlockWidth(Format format) {
  return isCompressedFormatImpl(format) ? 4u : 1u;
}

u32 formatBlockHeight(Format format) {
  return isCompressedFormatImpl(format) ? 4u : 1u;
}

u32 formatBlockBytes(Format format) {
  switch (format) {
  case Format::DXT1:
  case Format::ATI1:
  case Format::BC4:
    return 8;
  case Format::DXT2:
  case Format::DXT3:
  case Format::DXT4:
  case Format::DXT5:
  case Format::ATI2:
  case Format::BC5:
    return 16;
  default:
    return bytesPerPixel(format);
  }
}

u32 formatRowPitch(Format format, u32 width) {
  const u32 blockBytes = formatBlockBytes(format);
  if (width == 0 || blockBytes == 0) {
    return 0;
  }
  const u32 blockWidth = formatBlockWidth(format);
  const u32 blockColumns = (width + blockWidth - 1u) / blockWidth;
  return blockColumns * blockBytes;
}

u32 formatRowCount(Format format, u32 height) {
  if (height == 0 || formatBlockBytes(format) == 0) {
    return 0;
  }
  const u32 blockHeight = formatBlockHeight(format);
  return (height + blockHeight - 1u) / blockHeight;
}

std::size_t formatByteSize(Format format, u32 width, u32 height) {
  return static_cast<std::size_t>(formatRowPitch(format, width)) *
         formatRowCount(format, height);
}

std::string formatName(Format format) {
  switch (format) {
  case Format::Unknown:
    return "Unknown";
  case Format::A8R8G8B8:
    return "A8R8G8B8";
  case Format::X8R8G8B8:
    return "X8R8G8B8";
  case Format::A8B8G8R8:
    return "A8B8G8R8";
  case Format::X8B8G8R8:
    return "X8B8G8R8";
  case Format::R5G6B5:
    return "R5G6B5";
  case Format::A1R5G5B5:
    return "A1R5G5B5";
  case Format::X1R5G5B5:
    return "X1R5G5B5";
  case Format::A4R4G4B4:
    return "A4R4G4B4";
  case Format::A8:
    return "A8";
  case Format::R8G8B8:
    return "R8G8B8";
  case Format::A16B16G16R16F:
    return "A16B16G16R16F";
  case Format::A32B32G32R32F:
    return "A32B32G32R32F";
  case Format::G16R16F:
    return "G16R16F";
  case Format::R16F:
    return "R16F";
  case Format::G32R32F:
    return "G32R32F";
  case Format::R32F:
    return "R32F";
  case Format::A16B16G16R16:
    return "A16B16G16R16";
  case Format::G16R16:
    return "G16R16";
  case Format::A2R10G10B10:
    return "A2R10G10B10";
  case Format::A2B10G10R10:
    return "A2B10G10R10";
  case Format::L8:
    return "L8";
  case Format::L16:
    return "L16";
  case Format::A8L8:
    return "A8L8";
  case Format::V8U8:
    return "V8U8";
  case Format::Q8W8V8U8:
    return "Q8W8V8U8";
  case Format::V16U16:
    return "V16U16";
  case Format::CxV8U8:
    return "CxV8U8";
  case Format::DXT1:
    return "DXT1";
  case Format::DXT2:
    return "DXT2";
  case Format::DXT3:
    return "DXT3";
  case Format::DXT4:
    return "DXT4";
  case Format::DXT5:
    return "DXT5";
  case Format::ATI1:
    return "ATI1";
  case Format::BC4:
    return "BC4";
  case Format::ATI2:
    return "ATI2";
  case Format::BC5:
    return "BC5";
  case Format::D24S8:
    return "D24S8";
  case Format::D24X8:
    return "D24X8";
  case Format::D16:
    return "D16";
  case Format::D32:
    return "D32";
  case Format::D32F_LOCKABLE:
    return "D32F_LOCKABLE";
  case Format::D16_LOCKABLE:
    return "D16_LOCKABLE";
  case Format::D15S1:
    return "D15S1";
  case Format::D24X4S4:
    return "D24X4S4";
  case Format::D24FS8:
    return "D24FS8";
  case Format::S8_LOCKABLE:
    return "S8_LOCKABLE";
  case Format::INDEX16:
    return "INDEX16";
  case Format::INDEX32:
    return "INDEX32";
  }
  return "Unknown";
}

std::string backendFormatName(BackendPixelFormat format) {
  switch (format) {
  case BackendPixelFormat::Unknown:
    return "Unknown";
  case BackendPixelFormat::BGRA8Unorm:
    return "BGRA8Unorm";
  case BackendPixelFormat::RGBA8Unorm:
    return "RGBA8Unorm";
  case BackendPixelFormat::B5G6R5Unorm:
    return "B5G6R5Unorm";
  case BackendPixelFormat::BGR5A1Unorm:
    return "BGR5A1Unorm";
  case BackendPixelFormat::ABGR4Unorm:
    return "ABGR4Unorm";
  case BackendPixelFormat::A8Unorm:
    return "A8Unorm";
  case BackendPixelFormat::RGBA16Float:
    return "RGBA16Float";
  case BackendPixelFormat::RGBA32Float:
    return "RGBA32Float";
  case BackendPixelFormat::RG16Float:
    return "RG16Float";
  case BackendPixelFormat::R16Float:
    return "R16Float";
  case BackendPixelFormat::RG32Float:
    return "RG32Float";
  case BackendPixelFormat::R32Float:
    return "R32Float";
  case BackendPixelFormat::RGBA16Unorm:
    return "RGBA16Unorm";
  case BackendPixelFormat::RG16Unorm:
    return "RG16Unorm";
  case BackendPixelFormat::RGB10A2Unorm:
    return "RGB10A2Unorm";
  case BackendPixelFormat::BGR10A2Unorm:
    return "BGR10A2Unorm";
  case BackendPixelFormat::R8Unorm:
    return "R8Unorm";
  case BackendPixelFormat::R16Unorm:
    return "R16Unorm";
  case BackendPixelFormat::RG8Unorm:
    return "RG8Unorm";
  case BackendPixelFormat::RG8Snorm:
    return "RG8Snorm";
  case BackendPixelFormat::RGBA8Snorm:
    return "RGBA8Snorm";
  case BackendPixelFormat::RG16Snorm:
    return "RG16Snorm";
  case BackendPixelFormat::BC1_RGBA:
    return "BC1_RGBA";
  case BackendPixelFormat::BC2_RGBA:
    return "BC2_RGBA";
  case BackendPixelFormat::BC3_RGBA:
    return "BC3_RGBA";
  case BackendPixelFormat::BC4_RUnorm:
    return "BC4_RUnorm";
  case BackendPixelFormat::BC5_RGUnorm:
    return "BC5_RGUnorm";
  case BackendPixelFormat::Depth24Unorm_Stencil8:
    return "Depth24Unorm_Stencil8";
  case BackendPixelFormat::Depth32Float:
    return "Depth32Float";
  case BackendPixelFormat::Depth32Float_Stencil8:
    return "Depth32Float_Stencil8";
  case BackendPixelFormat::Depth16Unorm:
    return "Depth16Unorm";
  }
  return "Unknown";
}

bool formatSupportsUsage(Format format, u32 usage,
                         const BackendLimits &limits) {
  const auto *info = findFormatInfo(format);
  if (!info || info->support == FormatClass::Unsupported) {
    return false;
  }

  if (format == Format::A2B10G10R10 && !limits.supportsBgr10A2) {
    return false;
  }

  if ((usage & UsageRenderTarget) != 0) {
    if (!info->renderTarget || info->depthStencil || info->compressed) {
      return false;
    }
  }

  if ((usage & UsageDepthStencil) != 0) {
    if (!info->depthStencil) {
      return format == Format::D24S8 || format == Format::D24X8 ||
             format == Format::D16 || format == Format::D32 ||
             format == Format::D32F_LOCKABLE ||
             format == Format::D16_LOCKABLE || format == Format::D24FS8;
    }
    if (format == Format::D24FS8 && !limits.supportsDepth32FloatStencil8) {
      return false;
    }
  }

  if (info->support == FormatClass::Optional) {
    if (format == Format::A2B10G10R10) {
      return limits.supportsBgr10A2;
    }
    if (format == Format::D24FS8) {
      return limits.supportsDepth32FloatStencil8;
    }
  }

  if (isCompressedFormat(format) &&
      ((usage & UsageRenderTarget) != 0 || (usage & UsageDepthStencil) != 0)) {
    return false;
  }

  if (format == Format::R8G8B8 && usage != 0) {
    return false;
  }

  return true;
}

bool isDisplayModeFormat(Format format) {
  const auto *info = findFormatInfo(format);
  return info && info->renderTarget && !info->depthStencil && !info->compressed;
}

std::vector<DisplayMode> makeAdapterModes(Format format,
                                          const BackendLimits &limits) {
  if (!isDisplayModeFormat(format) ||
      !formatSupportsUsage(format, UsageRenderTarget, limits)) {
    return {};
  }

  constexpr std::array<std::pair<u32, u32>, 5> kCommonModes = {
      std::pair{640u, 480u},  std::pair{800u, 600u},   std::pair{1024u, 768u},
      std::pair{1280u, 720u}, std::pair{1920u, 1080u},
  };

  std::vector<DisplayMode> modes;
  for (const auto &[width, height] : kCommonModes) {
    if (width > limits.maxTextureSize || height > limits.maxTextureSize) {
      continue;
    }
    modes.push_back({width, height, 60, format});
  }
  return modes;
}

// hashBytes / hashString moved to src/util/util_hash.cpp so the ELF
// winemetal.so unix module can link them without pulling d3d9 in.

DeviceCaps makeDefaultCaps(const BackendLimits &limits) {
  constexpr u32 kCaps = 0x00000000u;
  constexpr u32 kCaps2 = 0x20000u | 0x40000000u | 0x20000000u;
  constexpr u32 kCaps3 = 0x00000020u | 0x00000100u | 0x00000200u;
  constexpr u32 kCursorCaps = 0x00000001u | 0x00000002u;
  constexpr u32 kPrimitiveMiscCaps = 0x002ecff2u;
  constexpr u32 kRasterCaps = 0x07332191u;
  constexpr u32 kCmpCaps = 0x000000ffu;
  constexpr u32 kShadeCaps =
      0x00000008u | 0x00000200u | 0x00004000u | 0x00080000u;
  constexpr u32 kTextureCaps = 0x0001ec85u;
  constexpr u32 kFilterCaps = 0x07030700u;
  constexpr u32 kCubeFilterCaps = 0x07030700u;
  constexpr u32 kVolumeFilterCaps = 0x03030300u;
  constexpr u32 kStretchRectFilterCaps = 0x03000300u;
  constexpr u32 kAddressCaps = 0x0000001fu;
  constexpr u32 kStencilCaps = 0x00000001u | 0x00000002u | 0x00000004u |
                               0x00000008u | 0x00000010u | 0x00000020u |
                               0x00000040u | 0x00000080u | 0x00000100u;
  constexpr u32 kSrcBlendCaps = 0x00003fffu;
  constexpr u32 kDestBlendCaps = 0x000027ffu;
  constexpr u32 kTextureOpCaps = 0x03feffffu;
  constexpr u32 kVertexProcessingCaps = 0x0000013bu;
  constexpr u32 kDeclTypes = 0x0000030fu;
  constexpr u32 kFvfCaps = 0x00100008u;
  constexpr u32 kLineCaps = 0x0000001fu;
  constexpr u32 kDevCaps = 0x0019aff0u;
  constexpr u32 kDevCaps2 = 0x00000001u | 0x00000010u | 0x00000040u;

  DeviceCaps caps;
  caps.maxTextureWidth = std::min(16384u, limits.maxTextureSize);
  caps.maxTextureHeight = std::min(16384u, limits.maxTextureSize);
  caps.maxRenderTargetWidth = caps.maxTextureWidth;
  caps.maxRenderTargetHeight = caps.maxTextureHeight;
  caps.maxAnisotropy = limits.maxAnisotropy;
  caps.numSimultaneousRTs =
      std::min(kMaxRenderTargets, limits.maxColorAttachments);
  caps.maxVertexShaderConst = kMaxVertexConstants;
  caps.maxSimultaneousTextures = 8;
  caps.maxActiveLights = kMaxLights;
  caps.maxStreams = kMaxStreams;
  caps.vertexShaderVersion = 0xfffe0300u;
  caps.pixelShaderVersion = 0xffff0300u;
  caps.caps = kCaps;
  caps.caps2 = kCaps2;
  caps.caps3 = kCaps3;
  caps.presentationIntervals = 0x80000000u | 0x00000001u;
  caps.cursorCaps = kCursorCaps;
  caps.primitiveMiscCaps = kPrimitiveMiscCaps;
  caps.textureCaps = kTextureCaps;
  caps.textureFilterCaps = kFilterCaps;
  caps.cubetextureFilterCaps = kCubeFilterCaps;
  caps.volumeTextureFilterCaps = kVolumeFilterCaps;
  caps.rasterCaps = kRasterCaps;
  caps.zCmpCaps = kCmpCaps;
  caps.alphaCmpCaps = kCmpCaps;
  caps.shadeCaps = kShadeCaps;
  caps.stencilCaps = kStencilCaps;
  caps.srcBlendCaps = kSrcBlendCaps;
  caps.destBlendCaps = kDestBlendCaps;
  caps.alphaBlendCaps = kCmpCaps;
  caps.textureBlendCaps = kTextureOpCaps;
  caps.textureAddressCaps = kAddressCaps;
  caps.volumeTextureAddressCaps = kAddressCaps;
  caps.lineCaps = kLineCaps;
  caps.fvfCaps = kFvfCaps;
  caps.vertexProcessingCaps = kVertexProcessingCaps;
  caps.devCaps = kDevCaps;
  caps.devCaps2 = kDevCaps2;
  caps.declTypes = kDeclTypes;
  caps.stretchRectFilterCaps = kStretchRectFilterCaps;
  caps.vs20Caps = 0x00000001u;
  caps.ps20Caps = 0x0000001fu;
  caps.maxTextureRepeat = 32768;
  caps.maxTextureAspectRatio = 16384;
  caps.maxUserClipPlanes = 8;
  caps.maxPointSize = 64.0f;
  caps.maxPrimitiveCount = 5592405;
  caps.maxStreamStride = 1024;
  caps.maxVertexBlendMatrixIndex = 0;
  caps.pixelShader1xMaxValue = 1024.0f;
  caps.vertexTextureFilterCaps = 0x01000100u;
  caps.maxVShaderInstructionsExecuted = 65535;
  caps.maxPShaderInstructionsExecuted = 65535;
  caps.maxVertexShader30InstructionSlots = 512;
  caps.maxPixelShader30InstructionSlots = 512;
  return caps;
}

std::array<f32, 2> halfPixelFixup(const Viewport &viewport) {
  if (viewport.width == 0 || viewport.height == 0) {
    return {0.0f, 0.0f};
  }
  return {1.0f / static_cast<f32>(viewport.width),
          1.0f / static_cast<f32>(viewport.height)};
}

} // namespace dxmt9::core
