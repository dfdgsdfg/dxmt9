#include "dxmt9/core.hpp"
#include "dxmt9/assert.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "../dxmt9/dxmt9_presenter.hpp"
#include "util/util_bmp.hpp"
#include "util/config/config.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>

namespace dxmt9::core {

using detail::DrawParamInlineStorage;
using detail::DrawPayloadArenaStorage;
using detail::kDrawRunInlineParamCapacity;
using detail::kDrawRunInlinePayloadCapacity;

namespace {

constexpr u64 kFnvOffset = 1469598103934665603ull;
constexpr u64 kFnvPrime = 1099511628211ull;

u64 hashCombine(u64 seed, u64 value) {
  seed ^= value;
  seed *= kFnvPrime;
  return seed;
}

template <typename T>
u64 hashTrivial(const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  const auto* bytes = reinterpret_cast<const std::byte*>(std::addressof(value));
  u64 hash = kFnvOffset;
  for (size_t i = 0; i < sizeof(T); ++i) {
    hash ^= static_cast<u64>(bytes[i]);
    hash *= kFnvPrime;
  }
  return hash;
}

u64 hashStateEntry(u32 key, u32 value) {
  u64 hash = kFnvOffset;
  hash = hashCombine(hash, key);
  hash = hashCombine(hash, value);
  return hash;
}

u64 hashStateDigest(std::size_t count, u64 rollingHash) {
  u64 hash = hashCombine(kFnvOffset, static_cast<u64>(count));
  hash = hashCombine(hash, rollingHash);
  return hash;
}

template <typename StateValues>
u64 hashStateValues(const StateValues& values) {
  u64 hash = hashCombine(kFnvOffset, static_cast<u64>(values.size()));
  u64 rollingHash = 0;
  for (const auto& entry : values) {
    rollingHash ^= hashStateEntry(entry.first, entry.second);
  }
  return hashCombine(hash, rollingHash);
}

u64 hashStateMap(const std::unordered_map<u32, u32>& values) {
  return hashStateValues(values);
}

template <std::size_t MaxEntries>
u64 hashStateMap(const StateValueTable<MaxEntries>& values) {
  return hashStateDigest(values.size(), values.rollingHash);
}

template <std::size_t MaxEntries>
FlatStateSet<MaxEntries> makeFlatStateSet(const auto& values) {
  FlatStateSet<MaxEntries> set{};
  set.hash = hashStateMap(values);
  set.overflow = values.size() > MaxEntries;
  for (const auto& entry : values) {
    if (set.count >= MaxEntries) {
      continue;
    }
    set.entries[set.count++] = FlatStateEntry{.state = entry.first, .value = entry.second};
  }
  std::sort(set.entries.begin(), set.entries.begin() + set.count,
            [](const FlatStateEntry& a, const FlatStateEntry& b) {
              return a.state < b.state;
            });
  for (u32 i = 0; i < set.count; ++i) {
    set.occupied[i / 64u] |= 1ull << (i % 64u);
  }
  return set;
}

u64 hashVertexDeclElements(const VertexDeclSnapshot& decl) {
  u64 hash = hashCombine(kFnvOffset, static_cast<u64>(decl.elements.size()));
  hash = hashCombine(hash, decl.fvf);
  for (const auto& element : decl.elements) {
    hash = hashCombine(hash, hashTrivial(element));
  }
  return hash;
}

u64 hashShaderRefSummary(const ShaderRef& shader) {
  u64 hash = hashCombine(kFnvOffset, static_cast<u64>(shader.kind));
  hash = hashCombine(hash, shader.hash);
  hash = hashCombine(hash, shader.bytecode.hash);
  return hash;
}

u64 hashTextureTransforms(const std::array<Matrix4x4, kMaxTextureStages>& transforms) {
  u64 hash = hashCombine(kFnvOffset, transforms.size());
  for (const auto& transform : transforms) {
    hash = hashCombine(hash, hashTrivial(transform));
  }
  return hash;
}

u64 hashClipPlanes(const std::array<ClipPlane, kMaxClipPlanes>& planes) {
  u64 hash = hashCombine(kFnvOffset, planes.size());
  for (const auto& plane : planes) {
    hash = hashCombine(hash, hashTrivial(plane));
  }
  return hash;
}

u64 hashViewportScissor(const ViewportScissor& viewport) {
  u64 hash = hashCombine(kFnvOffset, hashTrivial(viewport.viewport));
  hash = hashCombine(hash, hashTrivial(viewport.scissor));
  hash = hashCombine(hash, viewport.scissorEnabled ? 1u : 0u);
  return hash;
}

u32 clampToByte(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return static_cast<u32>(std::lround(value * 255.0f)) & 0xffu;
}

std::optional<u32> parseEnvU32(const char* name) {
  return dxmt9::util::getenvU32(name);
}

std::optional<u32> parseEnvU32Auto(const char* name) {
  return dxmt9::util::getenvU32Auto(name);
}

std::string getenvString(const char* name) {
  return dxmt9::util::getenvString(name);
}

bool getenvFlag(const char* name) {
  return dxmt9::util::getenvFlag(name);
}

bool backendOwnsSurfaceContents(const SurfaceDesc& desc) {
  return desc.renderTarget ||
         desc.depthStencil ||
         (desc.usage & (UsageRenderTarget | UsageDepthStencil)) != 0 ||
         desc.multiSampleType != MultiSampleType::None;
}

bool backendOwnsTextureContents(const TextureDesc& desc) {
  return (desc.usage & (UsageRenderTarget | UsageDepthStencil)) != 0;
}

bool canTrustGpuReadback(const std::shared_ptr<dxmt9::Device>& backend) {
  return backend && backend->supportsGpuReadback();
}

bool renderTraceEnabled() {
  static const bool enabled = [] {
    return dxmt9::util::getenvFlag("DXMT_TRACE_RENDER");
  }();
  return enabled;
}

std::optional<u32> textureDumpHandle() {
  static const auto value = parseEnvU32Auto("DXMT_DUMP_TEXTURE_HANDLE");
  return value;
}

std::string textureDumpDir() {
  static const std::string value = [] {
    const auto env = getenvString("DXMT_DUMP_TEXTURE_DIR");
    return env.empty() ? std::string("/tmp") : env;
  }();
  return value;
}

void emitRenderTrace(const char* fmt, ...) {
  if (!renderTraceEnabled()) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Info, "dxmt9-render", fmt, args);
  va_end(args);
}

constexpr u32 kFvfPositionMask = 0x000eu;
constexpr u32 kFvfXyzrhw = 0x0004u;
constexpr u32 kFvfXyz = 0x0002u;
constexpr u32 kFvfNormal = 0x0010u;
constexpr u32 kFvfDiffuse = 0x0040u;
constexpr u32 kFvfSpecular = 0x0080u;
constexpr u32 kFvfTexCountMask = 0x0f00u;
constexpr u32 kFvfTexCountShift = 8u;

constexpr u32 kDeclTypeFloat1 = 0u;
constexpr u32 kDeclTypeFloat2 = 1u;
constexpr u32 kDeclTypeFloat3 = 2u;
constexpr u32 kDeclTypeFloat4 = 3u;
constexpr u32 kDeclTypeD3DColor = 4u;
u32 declTypeSize(u32 type) {
  switch (type) {
    case kDeclTypeFloat1:
      return 4;
    case kDeclTypeFloat2:
      return 8;
    case kDeclTypeFloat3:
      return 12;
    case kDeclTypeFloat4:
      return 16;
    case kDeclTypeD3DColor:
      return 4;
    default:
      return 0;
  }
}

u32 fvfTexcoordSize(u32 fvf, u32 index) {
  const u32 code = (fvf >> (16u + index * 2u)) & 0x3u;
  switch (code) {
    case 1u:
      return 3;
    case 2u:
      return 4;
    case 3u:
      return 1;
    default:
      return 2;
  }
}

u32 inferStreamZeroStride(const VertexDeclSnapshot& vertexDecl) {
  if (vertexDecl.streams[0].stride != 0) {
    return vertexDecl.streams[0].stride;
  }

  if (!vertexDecl.elements.empty()) {
    u32 stride = 0;
    for (const auto& element : vertexDecl.elements) {
      if (element.stream != 0) {
        continue;
      }
      const u32 size = declTypeSize(element.type);
      if (size != 0) {
        stride = std::max(stride, static_cast<u32>(element.offset + size));
      }
    }
    return stride;
  }

  const u32 fvf = vertexDecl.fvf;
  const u32 position = fvf & kFvfPositionMask;
  u32 stride = 0;
  if (position == kFvfXyzrhw) {
    stride = 16u;
  } else if (position == kFvfXyz) {
    stride = 12u;
  } else {
    return 0;
  }
  if ((fvf & kFvfNormal) != 0) {
    stride += 12u;
  }
  if ((fvf & kFvfDiffuse) != 0) {
    stride += 4u;
  }
  if ((fvf & kFvfSpecular) != 0) {
    stride += 4u;
  }
  const u32 texCount = (fvf & kFvfTexCountMask) >> kFvfTexCountShift;
  for (u32 i = 0; i < texCount; ++i) {
    stride += fvfTexcoordSize(fvf, i) * 4u;
  }
  return stride;
}

std::vector<u8> decomposeTriangleFanVertices(std::span<const u8> vertices,
                                             u32 primitiveCount,
                                             u32 stride) {
  if (primitiveCount == 0 || stride == 0) {
    return {};
  }

  const u32 sourceVertexCount = primitiveCount + 2u;
  const auto requiredBytes = static_cast<size_t>(sourceVertexCount) * stride;
  if (vertices.size() < requiredBytes) {
    return {};
  }

  std::vector<u8> out(static_cast<size_t>(primitiveCount) * 3u * stride);
  auto appendVertex = [&](size_t& offset, u32 index) {
    const auto sourceOffset = static_cast<size_t>(index) * stride;
    std::memcpy(out.data() + offset, vertices.data() + sourceOffset, stride);
    offset += stride;
  };

  size_t offset = 0;
  for (u32 i = 1; i + 1u < sourceVertexCount; ++i) {
    appendVertex(offset, 0);
    appendVertex(offset, i);
    appendVertex(offset, i + 1u);
  }
  return out;
}

u32 clampToBits(float value, u32 bits) {
  value = std::clamp(value, 0.0f, 1.0f);
  const u32 maxValue = (1u << bits) - 1u;
  return static_cast<u32>(std::lround(value * static_cast<float>(maxValue))) & maxValue;
}

u16 pack565(ColorRGBA c) {
  return static_cast<u16>((clampToBits(c.r, 5) << 11) | (clampToBits(c.g, 6) << 5) |
                          clampToBits(c.b, 5));
}

u16 pack1555(ColorRGBA c, bool forceAlpha) {
  const u16 a = static_cast<u16>(forceAlpha ? 1u : clampToBits(c.a, 1));
  return static_cast<u16>((a << 15) | (clampToBits(c.r, 5) << 10) |
                          (clampToBits(c.g, 5) << 5) | clampToBits(c.b, 5));
}

u16 pack4444(ColorRGBA c) {
  return static_cast<u16>((clampToBits(c.a, 4) << 12) | (clampToBits(c.r, 4) << 8) |
                          (clampToBits(c.g, 4) << 4) | clampToBits(c.b, 4));
}

u32 pack2101010(ColorRGBA c, bool bgraOrder) {
  const u32 a = clampToBits(c.a, 2);
  const u32 r = clampToBits(c.r, 10);
  const u32 g = clampToBits(c.g, 10);
  const u32 b = clampToBits(c.b, 10);
  if (bgraOrder) {
    return (a << 30) | (b << 20) | (g << 10) | r;
  }
  return (a << 30) | (r << 20) | (g << 10) | b;
}

ColorRGBA decodeColor(Format format, const u8* src) {
  switch (format) {
    case Format::A8R8G8B8:
    case Format::X8R8G8B8:
      return {src[2] / 255.0f, src[1] / 255.0f, src[0] / 255.0f,
              format == Format::X8R8G8B8 ? 1.0f : src[3] / 255.0f};
    case Format::A8B8G8R8:
    case Format::X8B8G8R8:
      return {src[0] / 255.0f, src[1] / 255.0f, src[2] / 255.0f,
              format == Format::X8B8G8R8 ? 1.0f : src[3] / 255.0f};
    case Format::R5G6B5: {
      const u16 v = std::bit_cast<u16>(std::array<u8, 2>{src[0], src[1]});
      return {((v >> 11) & 0x1f) / 31.0f, ((v >> 5) & 0x3f) / 63.0f, (v & 0x1f) / 31.0f, 1.0f};
    }
    case Format::A1R5G5B5:
    case Format::X1R5G5B5: {
      const u16 v = std::bit_cast<u16>(std::array<u8, 2>{src[0], src[1]});
      return {((v >> 10) & 0x1f) / 31.0f, ((v >> 5) & 0x1f) / 31.0f, (v & 0x1f) / 31.0f,
              format == Format::X1R5G5B5 ? 1.0f : ((v >> 15) & 1u) ? 1.0f : 0.0f};
    }
    case Format::A4R4G4B4: {
      const u16 v = std::bit_cast<u16>(std::array<u8, 2>{src[0], src[1]});
      return {((v >> 8) & 0xf) / 15.0f, ((v >> 4) & 0xf) / 15.0f, (v & 0xf) / 15.0f,
              ((v >> 12) & 0xf) / 15.0f};
    }
    case Format::A8:
      return {0.0f, 0.0f, 0.0f, src[0] / 255.0f};
    case Format::L8: {
      const float l = src[0] / 255.0f;
      return {l, l, l, 1.0f};
    }
    case Format::A8L8:
      return {src[0] / 255.0f, src[0] / 255.0f, src[0] / 255.0f, src[1] / 255.0f};
    case Format::A2R10G10B10: {
      const u32 v = std::bit_cast<u32>(std::array<u8, 4>{src[0], src[1], src[2], src[3]});
      return {((v >> 20) & 0x3ff) / 1023.0f, ((v >> 10) & 0x3ff) / 1023.0f,
              (v & 0x3ff) / 1023.0f, ((v >> 30) & 0x3) / 3.0f};
    }
    case Format::A2B10G10R10: {
      const u32 v = std::bit_cast<u32>(std::array<u8, 4>{src[0], src[1], src[2], src[3]});
      return {(v & 0x3ff) / 1023.0f, ((v >> 10) & 0x3ff) / 1023.0f,
              ((v >> 20) & 0x3ff) / 1023.0f, ((v >> 30) & 0x3) / 3.0f};
    }
    default:
      return {};
  }
}

bool writeBmpScreenshot(const std::string& path, Format format, u32 width, u32 height, u32 pitch,
                        std::span<const u8> bytes) {
  return dxmt9::util::writeBmp(path, format, width, height, pitch, bytes);
}

bool encodeColor(Format format, ColorRGBA c, u8* dst) {
  switch (format) {
    case Format::A8R8G8B8:
    case Format::X8R8G8B8:
      dst[0] = static_cast<u8>(clampToByte(c.b));
      dst[1] = static_cast<u8>(clampToByte(c.g));
      dst[2] = static_cast<u8>(clampToByte(c.r));
      dst[3] = format == Format::X8R8G8B8 ? 0xffu : static_cast<u8>(clampToByte(c.a));
      return true;
    case Format::A8B8G8R8:
    case Format::X8B8G8R8:
      dst[0] = static_cast<u8>(clampToByte(c.r));
      dst[1] = static_cast<u8>(clampToByte(c.g));
      dst[2] = static_cast<u8>(clampToByte(c.b));
      dst[3] = format == Format::X8B8G8R8 ? 0xffu : static_cast<u8>(clampToByte(c.a));
      return true;
    case Format::R5G6B5: {
      const u16 v = pack565(c);
      const auto raw = std::bit_cast<std::array<u8, 2>>(v);
      dst[0] = raw[0];
      dst[1] = raw[1];
      return true;
    }
    case Format::A1R5G5B5:
    case Format::X1R5G5B5: {
      const u16 v = pack1555(c, format == Format::X1R5G5B5);
      const auto raw = std::bit_cast<std::array<u8, 2>>(v);
      dst[0] = raw[0];
      dst[1] = raw[1];
      return true;
    }
    case Format::A4R4G4B4: {
      const u16 v = pack4444(c);
      const auto raw = std::bit_cast<std::array<u8, 2>>(v);
      dst[0] = raw[0];
      dst[1] = raw[1];
      return true;
    }
    case Format::A8:
      dst[0] = static_cast<u8>(clampToByte(c.a));
      return true;
    case Format::L8:
      dst[0] = static_cast<u8>(clampToByte((c.r + c.g + c.b) / 3.0f));
      return true;
    case Format::A8L8:
      dst[0] = static_cast<u8>(clampToByte((c.r + c.g + c.b) / 3.0f));
      dst[1] = static_cast<u8>(clampToByte(c.a));
      return true;
    case Format::A2R10G10B10: {
      const u32 v = pack2101010(c, false);
      const auto raw = std::bit_cast<std::array<u8, 4>>(v);
      std::memcpy(dst, raw.data(), 4);
      return true;
    }
    case Format::A2B10G10R10: {
      const u32 v = pack2101010(c, true);
      const auto raw = std::bit_cast<std::array<u8, 4>>(v);
      std::memcpy(dst, raw.data(), 4);
      return true;
    }
    case Format::R32F:
      std::memcpy(dst, &c.r, sizeof(float));
      return true;
    case Format::G32R32F:
    case Format::G16R16:
    case Format::V16U16:
    case Format::Q8W8V8U8:
      std::memcpy(dst, &c.r, std::min<size_t>(sizeof(float) * 2, 4));
      return true;
    default:
      return false;
  }
}

[[maybe_unused]] bool isColorRenderable(Format format) {
  switch (format) {
    case Format::A8R8G8B8:
    case Format::X8R8G8B8:
    case Format::A8B8G8R8:
    case Format::X8B8G8R8:
    case Format::R5G6B5:
    case Format::A1R5G5B5:
    case Format::X1R5G5B5:
    case Format::A4R4G4B4:
    case Format::A8:
    case Format::A16B16G16R16F:
    case Format::A32B32G32R32F:
    case Format::G16R16F:
    case Format::R16F:
    case Format::G32R32F:
    case Format::R32F:
    case Format::A16B16G16R16:
    case Format::G16R16:
    case Format::A2R10G10B10:
    case Format::A2B10G10R10:
    case Format::L8:
    case Format::L16:
    case Format::A8L8:
    case Format::V8U8:
    case Format::Q8W8V8U8:
    case Format::V16U16:
      return true;
    default:
      return false;
  }
}

bool isDepthFormat(Format format) {
  switch (format) {
    case Format::D24S8:
    case Format::D24X8:
    case Format::D16:
    case Format::D32:
    case Format::D32F_LOCKABLE:
    case Format::D16_LOCKABLE:
    case Format::D24FS8:
      return true;
    default:
      return false;
  }
}

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

const std::vector<FormatEntry>& formatEntries() {
  static const std::vector<FormatEntry> entries = {
      {{Format::A8R8G8B8, BackendPixelFormat::BGRA8Unorm, FormatClass::Required, 4, true, false,
        false, true}},
      {{Format::X8R8G8B8, BackendPixelFormat::BGRA8Unorm, FormatClass::Required, 4, true, false,
        false, true}},
      {{Format::A8B8G8R8, BackendPixelFormat::RGBA8Unorm, FormatClass::Required, 4, true, false,
        false, true}},
      {{Format::X8B8G8R8, BackendPixelFormat::RGBA8Unorm, FormatClass::Required, 4, true, false,
        false, true}},
      {{Format::R5G6B5, BackendPixelFormat::B5G6R5Unorm, FormatClass::Required, 2, true, false,
        false, true}},
      {{Format::A1R5G5B5, BackendPixelFormat::BGR5A1Unorm, FormatClass::Required, 2, true, false,
        false, true}},
      {{Format::X1R5G5B5, BackendPixelFormat::BGR5A1Unorm, FormatClass::Required, 2, true, false,
        false, true}},
      {{Format::A4R4G4B4, BackendPixelFormat::ABGR4Unorm, FormatClass::Required, 2, true, false,
        false, true}},
      {{Format::A8, BackendPixelFormat::A8Unorm, FormatClass::Required, 1, true, false, false,
        true}},
      {{Format::R8G8B8, BackendPixelFormat::Unknown, FormatClass::Unsupported, 3, false, false,
        false, true}},
      {{Format::A16B16G16R16F, BackendPixelFormat::RGBA16Float, FormatClass::Required, 8, true,
        false, false, true}},
      {{Format::A32B32G32R32F, BackendPixelFormat::RGBA32Float, FormatClass::Required, 16, true,
        false, false, true}},
      {{Format::G16R16F, BackendPixelFormat::RG16Float, FormatClass::Required, 4, true, false,
        false, true}},
      {{Format::R16F, BackendPixelFormat::R16Float, FormatClass::Required, 2, true, false, false,
        true}},
      {{Format::G32R32F, BackendPixelFormat::RG32Float, FormatClass::Required, 8, true, false,
        false, true}},
      {{Format::R32F, BackendPixelFormat::R32Float, FormatClass::Required, 4, true, false, false,
        true}},
      {{Format::A16B16G16R16, BackendPixelFormat::RGBA16Unorm, FormatClass::Required, 8, true,
        false, false, true}},
      {{Format::G16R16, BackendPixelFormat::RG16Unorm, FormatClass::Required, 4, true, false,
        false, true}},
      {{Format::A2R10G10B10, BackendPixelFormat::RGB10A2Unorm, FormatClass::Required, 4, true,
        false, false, true}},
      {{Format::A2B10G10R10, BackendPixelFormat::BGR10A2Unorm, FormatClass::Optional, 4, true,
        false, false, true}},
      {{Format::L8, BackendPixelFormat::R8Unorm, FormatClass::Required, 1, false, false, false,
        true}},
      {{Format::L16, BackendPixelFormat::R16Unorm, FormatClass::Required, 2, false, false, false,
        true}},
      {{Format::A8L8, BackendPixelFormat::RG8Unorm, FormatClass::Required, 2, false, false, false,
        true}},
      {{Format::V8U8, BackendPixelFormat::RG8Snorm, FormatClass::Required, 2, true, false, false,
        true}},
      {{Format::Q8W8V8U8, BackendPixelFormat::RGBA8Snorm, FormatClass::Required, 4, true, false,
        false, true}},
      {{Format::V16U16, BackendPixelFormat::RG16Snorm, FormatClass::Required, 4, true, false,
        false, true}},
      {{Format::CxV8U8, BackendPixelFormat::Unknown, FormatClass::Unsupported, 0, false, false,
        false, true}},
      {{Format::DXT1, BackendPixelFormat::BC1_RGBA, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::DXT2, BackendPixelFormat::BC2_RGBA, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::DXT3, BackendPixelFormat::BC2_RGBA, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::DXT4, BackendPixelFormat::BC3_RGBA, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::DXT5, BackendPixelFormat::BC3_RGBA, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::ATI1, BackendPixelFormat::BC4_RUnorm, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::BC4, BackendPixelFormat::BC4_RUnorm, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::ATI2, BackendPixelFormat::BC5_RGUnorm, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::BC5, BackendPixelFormat::BC5_RGUnorm, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::D24S8, BackendPixelFormat::Depth24Unorm_Stencil8, FormatClass::Required, 4, false,
        true, false, true}},
      {{Format::D24X8, BackendPixelFormat::Depth24Unorm_Stencil8, FormatClass::Required, 4, false,
        true, false, true}},
      {{Format::D16, BackendPixelFormat::Depth16Unorm, FormatClass::Required, 2, false, true,
        false, true}},
      {{Format::D32, BackendPixelFormat::Depth32Float, FormatClass::Required, 4, false, true,
        false, true}},
      {{Format::D32F_LOCKABLE, BackendPixelFormat::Depth32Float, FormatClass::Required, 4, false,
        true, false, true}},
      {{Format::D16_LOCKABLE, BackendPixelFormat::Depth16Unorm, FormatClass::Required, 2, false,
        true, false, true}},
      {{Format::D15S1, BackendPixelFormat::Unknown, FormatClass::Unsupported, 0, false, false,
        false, true}},
      {{Format::D24X4S4, BackendPixelFormat::Unknown, FormatClass::Unsupported, 0, false, false,
        false, true}},
      {{Format::D24FS8, BackendPixelFormat::Depth32Float_Stencil8, FormatClass::Optional, 8, false,
        true, false, true}},
      {{Format::S8_LOCKABLE, BackendPixelFormat::Unknown, FormatClass::Unsupported, 1, false,
        false, false, true}},
      {{Format::INDEX16, BackendPixelFormat::Unknown, FormatClass::Required, 2, false, false,
        false, true}},
      {{Format::INDEX32, BackendPixelFormat::Unknown, FormatClass::Required, 4, false, false,
        false, true}},
  };
  return entries;
}

template <typename StateValues>
u64 hashMap(const StateValues& values) {
  u64 hash = kFnvOffset;
  std::vector<std::pair<u32, u32>> sorted;
  sorted.reserve(values.size());
  for (const auto& entry : values) {
    sorted.emplace_back(entry.first, entry.second);
  }
  std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first < b.first; });
  for (const auto& [key, value] : sorted) {
    hash = hashCombine(hash, key);
    hash = hashCombine(hash, value);
  }
  return hash;
}

template <std::size_t MaxEntries>
u64 hashMap(const StateValueTable<MaxEntries>& values) {
  return hashStateDigest(values.size(), values.rollingHash);
}

u64 hashMap(const TransformTable& values) {
  return hashStateDigest(values.size(), values.rollingHash);
}

u64 hashColor(const ColorRGBA& color) {
  return hashCombine(hashCombine(hashCombine(hashCombine(kFnvOffset, std::bit_cast<u32>(color.r)),
                                             std::bit_cast<u32>(color.g)),
                                 std::bit_cast<u32>(color.b)),
                      std::bit_cast<u32>(color.a));
}

Matrix4x4 identityMatrix() {
  Matrix4x4 matrix{};
  matrix.m = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
              0.0f, 0.0f, 0.0f, 1.0f};
  return matrix;
}

Matrix4x4 multiplyMatrix(const Matrix4x4& left, const Matrix4x4& right) {
  Matrix4x4 result{};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t col = 0; col < 4; ++col) {
      float sum = 0.0f;
      for (size_t k = 0; k < 4; ++k) {
        sum += left.m[row * 4 + k] * right.m[k * 4 + col];
      }
      result.m[row * 4 + col] = sum;
    }
  }
  return result;
}

Matrix4x4 transposeMatrix(const Matrix4x4& matrix) {
  Matrix4x4 result{};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t col = 0; col < 4; ++col) {
      result.m[row * 4 + col] = matrix.m[col * 4 + row];
    }
  }
  return result;
}

bool invertMatrix(const Matrix4x4& matrix, Matrix4x4* out) {
  std::array<double, 32> aug{};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t col = 0; col < 4; ++col) {
      aug[row * 8 + col] = matrix.m[row * 4 + col];
      aug[row * 8 + 4 + col] = (row == col) ? 1.0 : 0.0;
    }
  }

  for (size_t col = 0; col < 4; ++col) {
    size_t pivotRow = col;
    double pivot = std::fabs(aug[pivotRow * 8 + col]);
    for (size_t row = col + 1; row < 4; ++row) {
      const double candidate = std::fabs(aug[row * 8 + col]);
      if (candidate > pivot) {
        pivot = candidate;
        pivotRow = row;
      }
    }
    if (pivot < 1.0e-20) {
      return false;
    }
    if (pivotRow != col) {
      for (size_t i = 0; i < 8; ++i) {
        std::swap(aug[col * 8 + i], aug[pivotRow * 8 + i]);
      }
    }
    const double invPivot = 1.0 / aug[col * 8 + col];
    for (size_t i = 0; i < 8; ++i) {
      aug[col * 8 + i] *= invPivot;
    }
    for (size_t row = 0; row < 4; ++row) {
      if (row == col) {
        continue;
      }
      const double factor = aug[row * 8 + col];
      if (factor == 0.0) {
        continue;
      }
      for (size_t i = 0; i < 8; ++i) {
        aug[row * 8 + i] -= factor * aug[col * 8 + i];
      }
    }
  }

  Matrix4x4 inverse{};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t col = 0; col < 4; ++col) {
      inverse.m[row * 4 + col] = static_cast<float>(aug[row * 8 + 4 + col]);
    }
  }
  *out = inverse;
  return true;
}

ClipPlane transformClipPlane(const Matrix4x4& transform, const ClipPlane& plane) {
  Matrix4x4 inverse{};
  if (!invertMatrix(transform, &inverse)) {
    return plane;
  }
  const Matrix4x4 inverseTranspose = transposeMatrix(inverse);
  ClipPlane out{};
  for (size_t row = 0; row < 4; ++row) {
    float sum = 0.0f;
    for (size_t col = 0; col < 4; ++col) {
      sum += inverseTranspose.m[row * 4 + col] * plane[col];
    }
    out[row] = sum;
  }
  return out;
}

Matrix4x4 lookupTransform(const DeviceState& state, u32 key) {
  if (state.transforms.contains(key)) {
    return state.transforms.at(key);
  }
  return identityMatrix();
}

u64 hashLight(const Light& light) {
  u64 hash = kFnvOffset;
  hash = hashCombine(hash, static_cast<u64>(light.type));
  hash = hashCombine(hash, static_cast<u64>(light.enabled));
  hash = hashCombine(hash, hashColor(light.diffuse));
  hash = hashCombine(hash, hashColor(light.specular));
  hash = hashCombine(hash, hashColor(light.ambient));
  for (float v : light.position) {
    hash = hashCombine(hash, std::bit_cast<u32>(v));
  }
  for (float v : light.direction) {
    hash = hashCombine(hash, std::bit_cast<u32>(v));
  }
  hash = hashCombine(hash, std::bit_cast<u32>(light.range));
  hash = hashCombine(hash, std::bit_cast<u32>(light.falloff));
  hash = hashCombine(hash, std::bit_cast<u32>(light.attenuation0));
  hash = hashCombine(hash, std::bit_cast<u32>(light.attenuation1));
  hash = hashCombine(hash, std::bit_cast<u32>(light.attenuation2));
  hash = hashCombine(hash, std::bit_cast<u32>(light.theta));
  hash = hashCombine(hash, std::bit_cast<u32>(light.phi));
  return hash;
}

u64 hashMaterial(const Material& material) {
  u64 hash = kFnvOffset;
  hash = hashCombine(hash, hashColor(material.emissive));
  hash = hashCombine(hash, hashColor(material.ambient));
  hash = hashCombine(hash, hashColor(material.diffuse));
  hash = hashCombine(hash, hashColor(material.specular));
  hash = hashCombine(hash, std::bit_cast<u32>(material.power));
  return hash;
}

u64 hashFfpVertexKey(const FfpVertexKey& key) {
  u64 hash = kFnvOffset;
  hash = hashCombine(hash, static_cast<u64>(key.lightingEnabled));
  hash = hashCombine(hash, static_cast<u64>(key.specularEnabled));
  hash = hashCombine(hash, static_cast<u64>(key.normalizeNormals));
  for (bool enabled : key.lightEnabled) {
    hash = hashCombine(hash, static_cast<u64>(enabled));
  }
  for (u32 type : key.lightType) {
    hash = hashCombine(hash, type);
  }
  for (u32 mode : key.colorMaterialMode) {
    hash = hashCombine(hash, mode);
  }
  hash = hashCombine(hash, static_cast<u64>(key.fogMode));
  hash = hashCombine(hash, static_cast<u64>(key.fogFromVertex));
  hash = hashCombine(hash, static_cast<u64>(key.rangeFog));
  for (u32 value : key.texCoordGen) {
    hash = hashCombine(hash, value);
  }
  for (u32 value : key.texTransformFlags) {
    hash = hashCombine(hash, value);
  }
  hash = hashCombine(hash, key.vertexBlend);
  hash = hashCombine(hash, static_cast<u64>(key.indexedVertexBlend));
  hash = hashCombine(hash, key.clipPlaneMask);
  return hash;
}

u64 hashFfpPixelKey(const FfpPixelKey& key) {
  u64 hash = kFnvOffset;
  for (const auto& stage : key.stages) {
    hash = hashCombine(hash, stage.colorOp);
    hash = hashCombine(hash, stage.colorArg1);
    hash = hashCombine(hash, stage.colorArg2);
    hash = hashCombine(hash, stage.alphaOp);
    hash = hashCombine(hash, stage.alphaArg1);
    hash = hashCombine(hash, stage.alphaArg2);
    hash = hashCombine(hash, stage.resultArg);
    hash = hashCombine(hash, stage.texType);
    hash = hashCombine(hash, stage.texCoordIndex);
  }
  hash = hashCombine(hash, static_cast<u64>(key.fogMode));
  hash = hashCombine(hash, static_cast<u64>(key.alphaTestEnable));
  hash = hashCombine(hash, key.alphaTestFunc);
  return hash;
}

u64 hashShaderBytecode(const ShaderBytecode& bytecode) {
  if (bytecode.hash != 0) {
    return bytecode.hash;
  }
  return hashBytes(std::as_bytes(std::span<const u8>(bytecode.bytes.data(), bytecode.bytes.size())));
}

u64 hashShaderRef(const ShaderRef& ref) {
  switch (ref.kind) {
    case ShaderRef::Kind::Bytecode:
      return ref.hash != 0 ? ref.hash : hashShaderBytecode(ref.bytecode);
    case ShaderRef::Kind::FixedFunctionVertex:
      return ref.vertexKey ? (ref.vertexKey->hash ? ref.vertexKey->hash : hashFfpVertexKey(*ref.vertexKey))
                           : 0;
    case ShaderRef::Kind::FixedFunctionPixel:
      return ref.pixelKey ? (ref.pixelKey->hash ? ref.pixelKey->hash : hashFfpPixelKey(*ref.pixelKey))
                          : 0;
    case ShaderRef::Kind::None:
      return 0;
  }
  return 0;
}

[[maybe_unused]] u64 hashStateState(const DeviceState& state) {
  u64 hash = kFnvOffset;
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.x));
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.y));
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.width));
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.height));
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.minZ));
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.maxZ));
  hash = hashCombine(hash, static_cast<u64>(state.scissorEnabled));
  hash = hashCombine(hash, state.scissorRect.left);
  hash = hashCombine(hash, state.scissorRect.top);
  hash = hashCombine(hash, state.scissorRect.right);
  hash = hashCombine(hash, state.scissorRect.bottom);
  hash = hashCombine(hash, hashMap(state.renderStates));
  for (const auto& stage : state.textureStageStates) {
    hash = hashCombine(hash, hashMap(stage));
  }
  for (const auto& sampler : state.samplerStates) {
    hash = hashCombine(hash, hashMap(sampler));
  }
  hash = hashCombine(hash, hashMap(state.transforms));
  for (const auto& clipPlane : state.clipPlanes) {
    for (float value : clipPlane) {
      hash = hashCombine(hash, std::bit_cast<u32>(value));
    }
  }
  for (const auto& light : state.lights) {
    hash = hashCombine(hash, hashLight(light));
  }
  for (bool enabled : state.lightEnabled) {
    hash = hashCombine(hash, static_cast<u64>(enabled));
  }
  hash = hashCombine(hash, hashMaterial(state.material));
  hash = hashCombine(hash, state.fvf);
  hash = hashCombine(hash, hashShaderRef(state.vertexShader));
  hash = hashCombine(hash, hashShaderRef(state.pixelShader));
  hash = hashCombine(hash, state.indexBuffer ? state.indexBuffer->handle().value : 0);
  hash = hashCombine(hash, static_cast<u64>(state.indexType == IndexType::UInt32));
  for (const auto& tex : state.textures) {
    hash = hashCombine(hash, tex ? tex->handle().value : 0);
  }
  for (const auto& rt : state.renderTargets) {
    hash = hashCombine(hash, rt.handle.value);
    hash = hashCombine(hash, rt.level);
  }
  hash = hashCombine(hash, state.depthStencil.handle.value);
  hash = hashCombine(hash, state.depthStencil.level);
  hash = hashCombine(hash, static_cast<u64>(state.inScene));
  return hash;
}

[[maybe_unused]] u32 compareLimitToUsage(const BackendLimits& limits, Format format, u32 usage) {
  (void)limits;
  (void)format;
  (void)usage;
  return 0;
}

[[maybe_unused]] std::shared_ptr<const std::vector<u8>> cloneBytes(std::span<const u8> bytes) {
  return std::make_shared<std::vector<u8>>(bytes.begin(), bytes.end());
}

struct NullBackendDevice final : BackendDevice {
  BufferHandle createBuffer(const BufferDesc&) override {
    return Handle{++next_};
  }

  TextureHandle createTexture(const TextureDesc&) override {
    return Handle{++next_};
  }

  void destroyBuffer(BufferHandle) override {}
  void destroyTexture(TextureHandle) override {}
  void submitClear(const ClearDesc&) override {}
  void present(const SwapDesc&) override {}

 private:
  u64 next_ = 1000;
};

u32 pitchForFormat(Format format, u32 width) {
  return formatRowPitch(format, width);
}

[[maybe_unused]] bool isSupportedDataFormat(Format format) {
  return bytesPerPixel(format) != 0 && !isCompressedFormatImpl(format) && !isDepthFormat(format);
}

void fillBuffer(std::vector<u8>& bytes, u32 pitch, u32 width, u32 height, Format format,
                const Rect* rect, ColorRGBA color) {
  if (!encodeColor(format, color, bytes.data())) {
    return;
  }

  const i32 left = rect ? std::max(0, rect->left) : 0;
  const i32 top = rect ? std::max(0, rect->top) : 0;
  const i32 right = rect ? std::min<i32>(width, rect->right) : static_cast<i32>(width);
  const i32 bottom = rect ? std::min<i32>(height, rect->bottom) : static_cast<i32>(height);
  const u32 bpp = bytesPerPixel(format);
  std::vector<u8> pixel(bpp);
  if (!encodeColor(format, color, pixel.data())) {
    return;
  }
  for (i32 y = top; y < bottom; ++y) {
    u8* row = bytes.data() + static_cast<size_t>(y) * pitch;
    for (i32 x = left; x < right; ++x) {
      std::memcpy(row + static_cast<size_t>(x) * bpp, pixel.data(), bpp);
    }
  }
}

bool copyPixels(std::vector<u8>& dst, u32 dstPitch, u32 dstWidth, u32 dstHeight, Format dstFormat,
                const std::vector<u8>& src, u32 srcPitch, u32 srcWidth, u32 srcHeight,
                Format srcFormat) {
  const u32 dstBpp = bytesPerPixel(dstFormat);
  const u32 srcBpp = bytesPerPixel(srcFormat);
  if (dstBpp == 0 || srcBpp == 0) {
    return false;
  }
  const u32 width = std::min(dstWidth, srcWidth);
  const u32 height = std::min(dstHeight, srcHeight);
  std::vector<u8> temp(dstBpp);
  for (u32 y = 0; y < height; ++y) {
    const u8* srcRow = src.data() + static_cast<size_t>(y) * srcPitch;
    u8* dstRow = dst.data() + static_cast<size_t>(y) * dstPitch;
    for (u32 x = 0; x < width; ++x) {
      const u8* srcPx = srcRow + static_cast<size_t>(x) * srcBpp;
      ColorRGBA color = decodeColor(srcFormat, srcPx);
      if (!encodeColor(dstFormat, color, temp.data())) {
        return false;
      }
      std::memcpy(dstRow + static_cast<size_t>(x) * dstBpp, temp.data(), dstBpp);
    }
  }
  return true;
}

[[maybe_unused]] bool stretchPixels(std::vector<u8>& dst, u32 dstPitch, u32 dstWidth, u32 dstHeight, Format dstFormat,
                   const std::vector<u8>& src, u32 srcPitch, u32 srcWidth, u32 srcHeight,
                   Format srcFormat) {
  const u32 dstBpp = bytesPerPixel(dstFormat);
  const u32 srcBpp = bytesPerPixel(srcFormat);
  if (dstBpp == 0 || srcBpp == 0) {
    return false;
  }
  std::vector<u8> temp(dstBpp);
  for (u32 y = 0; y < dstHeight; ++y) {
    const u32 srcY = srcHeight == 0 ? 0 : (y * srcHeight) / dstHeight;
    const u8* srcRow = src.data() + static_cast<size_t>(srcY) * srcPitch;
    u8* dstRow = dst.data() + static_cast<size_t>(y) * dstPitch;
    for (u32 x = 0; x < dstWidth; ++x) {
      const u32 srcX = srcWidth == 0 ? 0 : (x * srcWidth) / dstWidth;
      const u8* srcPx = srcRow + static_cast<size_t>(srcX) * srcBpp;
      ColorRGBA color = decodeColor(srcFormat, srcPx);
      if (!encodeColor(dstFormat, color, temp.data())) {
        return false;
      }
      std::memcpy(dstRow + static_cast<size_t>(x) * dstBpp, temp.data(), dstBpp);
    }
  }
  return true;
}

void fillDepthStencil(std::vector<u8>& bytes, u32 pitch, u32 width, u32 height, Format format,
                      const Rect* rect, bool clearDepth, f32 depth, bool clearStencil, u32 stencil) {
  if (width == 0 || height == 0) {
    return;
  }

  const i32 left = rect ? std::max(0, rect->left) : 0;
  const i32 top = rect ? std::max(0, rect->top) : 0;
  const i32 right = rect ? std::min<i32>(width, rect->right) : static_cast<i32>(width);
  const i32 bottom = rect ? std::min<i32>(height, rect->bottom) : static_cast<i32>(height);
  const u32 bpp = bytesPerPixel(format);
  if (bpp == 0) {
    return;
  }

  const auto encodeDepth24 = [](f32 value) -> u32 {
    const auto clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<u32>(std::lround(clamped * 16777215.0f)) & 0x00ffffffu;
  };

  const auto readDepth24 = [](const u8* px) -> f32 {
    const u32 raw = static_cast<u32>(px[0]) | (static_cast<u32>(px[1]) << 8) |
                    (static_cast<u32>(px[2]) << 16);
    return static_cast<f32>(raw) / 16777215.0f;
  };

  for (i32 y = top; y < bottom; ++y) {
    u8* row = bytes.data() + static_cast<size_t>(y) * pitch;
    for (i32 x = left; x < right; ++x) {
      u8* px = row + static_cast<size_t>(x) * bpp;
      f32 currentDepth = 0.0f;
      u32 currentStencil = 0;
      switch (format) {
        case Format::D16:
        case Format::D16_LOCKABLE: {
          const u16 raw = std::bit_cast<u16>(std::array<u8, 2>{px[0], px[1]});
          currentDepth = static_cast<f32>(raw) / 65535.0f;
          break;
        }
        case Format::D32:
        case Format::D32F_LOCKABLE: {
          const u32 raw = std::bit_cast<u32>(std::array<u8, 4>{px[0], px[1], px[2], px[3]});
          currentDepth = std::bit_cast<f32>(raw);
          break;
        }
        case Format::D24S8: {
          currentDepth = readDepth24(px);
          currentStencil = px[3];
          break;
        }
        case Format::D24X8: {
          currentDepth = readDepth24(px);
          break;
        }
        case Format::D24FS8: {
          const u32 raw = std::bit_cast<u32>(std::array<u8, 4>{px[0], px[1], px[2], px[3]});
          currentDepth = std::bit_cast<f32>(raw);
          currentStencil = px[4];
          break;
        }
        case Format::S8_LOCKABLE:
          currentStencil = px[0];
          break;
        default:
          break;
      }

      if (clearDepth) {
        currentDepth = depth;
      }
      if (clearStencil) {
        currentStencil = stencil & 0xffu;
      }

      switch (format) {
        case Format::D16:
        case Format::D16_LOCKABLE: {
          const u16 raw = static_cast<u16>(std::lround(std::clamp(currentDepth, 0.0f, 1.0f) * 65535.0f));
          const auto bytes16 = std::bit_cast<std::array<u8, 2>>(raw);
          px[0] = bytes16[0];
          px[1] = bytes16[1];
          break;
        }
        case Format::D32:
        case Format::D32F_LOCKABLE: {
          const u32 raw = std::bit_cast<u32>(currentDepth);
          const auto bytes32 = std::bit_cast<std::array<u8, 4>>(raw);
          px[0] = bytes32[0];
          px[1] = bytes32[1];
          px[2] = bytes32[2];
          px[3] = bytes32[3];
          break;
        }
        case Format::D24S8: {
          const u32 raw = encodeDepth24(currentDepth);
          px[0] = static_cast<u8>(raw & 0xffu);
          px[1] = static_cast<u8>((raw >> 8) & 0xffu);
          px[2] = static_cast<u8>((raw >> 16) & 0xffu);
          px[3] = static_cast<u8>(currentStencil);
          break;
        }
        case Format::D24X8: {
          const u32 raw = encodeDepth24(currentDepth);
          px[0] = static_cast<u8>(raw & 0xffu);
          px[1] = static_cast<u8>((raw >> 8) & 0xffu);
          px[2] = static_cast<u8>((raw >> 16) & 0xffu);
          px[3] = 0;
          break;
        }
        case Format::D24FS8: {
          const u32 raw = std::bit_cast<u32>(currentDepth);
          const auto bytes32 = std::bit_cast<std::array<u8, 4>>(raw);
          px[0] = bytes32[0];
          px[1] = bytes32[1];
          px[2] = bytes32[2];
          px[3] = bytes32[3];
          px[4] = static_cast<u8>(currentStencil);
          px[5] = 0;
          px[6] = 0;
          px[7] = 0;
          break;
        }
        case Format::S8_LOCKABLE:
          px[0] = static_cast<u8>(currentStencil);
          break;
        default:
          break;
      }
    }
  }
}

}  // namespace

const std::vector<FormatInfo>& formatTable() {
  static const std::vector<FormatInfo> table = [] {
    std::vector<FormatInfo> out;
    out.reserve(formatEntries().size());
    for (const auto& entry : formatEntries()) {
      out.push_back(entry.info);
    }
    return out;
  }();
  return table;
}

const FormatInfo* findFormatInfo(Format format) {
  for (const auto& entry : formatTable()) {
    if (entry.format == format) {
      return &entry;
    }
  }
  return nullptr;
}

FormatClass classifyFormat(Format format) {
  if (const auto* info = findFormatInfo(format)) {
    return info->support;
  }
  return FormatClass::Unsupported;
}

BackendPixelFormat backendPixelFormat(Format format) {
  if (const auto* info = findFormatInfo(format)) {
    return info->backendFormat;
  }
  return BackendPixelFormat::Unknown;
}

u32 bytesPerPixel(Format format) {
  if (const auto* info = findFormatInfo(format)) {
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
  return static_cast<std::size_t>(formatRowPitch(format, width)) * formatRowCount(format, height);
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

bool formatSupportsUsage(Format format, u32 usage, const BackendLimits& limits) {
  const auto* info = findFormatInfo(format);
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
      return format == Format::D24S8 || format == Format::D24X8 || format == Format::D16 ||
             format == Format::D32 || format == Format::D32F_LOCKABLE ||
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

  if (isCompressedFormat(format) && ((usage & UsageRenderTarget) != 0 ||
                                     (usage & UsageDepthStencil) != 0)) {
    return false;
  }

  if (format == Format::R8G8B8 && usage != 0) {
    return false;
  }

  return true;
}

bool isDisplayModeFormat(Format format) {
  const auto* info = findFormatInfo(format);
  return info && info->renderTarget && !info->depthStencil && !info->compressed;
}

std::vector<DisplayMode> makeAdapterModes(Format format, const BackendLimits& limits) {
  if (!isDisplayModeFormat(format) || !formatSupportsUsage(format, UsageRenderTarget, limits)) {
    return {};
  }

  constexpr std::array<std::pair<u32, u32>, 5> kCommonModes = {
      std::pair{640u, 480u},
      std::pair{800u, 600u},
      std::pair{1024u, 768u},
      std::pair{1280u, 720u},
      std::pair{1920u, 1080u},
  };

  std::vector<DisplayMode> modes;
  for (const auto& [width, height] : kCommonModes) {
    if (width > limits.maxTextureSize || height > limits.maxTextureSize) {
      continue;
    }
    modes.push_back({width, height, 60, format});
  }
  return modes;
}

PresentParameters normalizePresentParameters(const AdapterInfo& adapter, PresentParameters params) {
  if (getenvFlag("DXMT_FORCE_WINDOWED")) {
    params.windowed = true;
  }
  if (params.backBufferCount == 0) {
    params.backBufferCount = 1;
  }
  if (params.backBufferFormat == Format::Unknown) {
    params.backBufferFormat = adapter.displayMode.format;
  }
  if (!params.windowed) {
    if (params.backBufferWidth == 0) {
      params.backBufferWidth = adapter.displayMode.width;
    }
    if (params.backBufferHeight == 0) {
      params.backBufferHeight = adapter.displayMode.height;
    }
  } else {
    params.backBufferWidth = std::max(1u, params.backBufferWidth);
    params.backBufferHeight = std::max(1u, params.backBufferHeight);
  }
  return params;
}

constexpr u32 kD3dSwapEffectCopy = 3;
constexpr u32 kD3dSwapEffectFlipex = 5;

constexpr u32 kD3dPresentIntervalDefault = 0x00000000u;
constexpr u32 kD3dPresentIntervalOne = 0x00000001u;
constexpr u32 kD3dPresentIntervalTwo = 0x00000002u;
constexpr u32 kD3dPresentIntervalThree = 0x00000004u;
constexpr u32 kD3dPresentIntervalFour = 0x00000008u;
constexpr u32 kD3dPresentIntervalImmediate = 0x80000000u;

bool isValidPresentationInterval(PresentInterval interval) {
  switch (interval) {
    case PresentInterval::Immediate:
    case PresentInterval::Default:
    case PresentInterval::Two:
      return true;
  }
  return false;
}

bool isValidPresentationIntervalRaw(u32 interval) {
  switch (interval) {
    case kD3dPresentIntervalDefault:
    case kD3dPresentIntervalOne:
    case kD3dPresentIntervalTwo:
    case kD3dPresentIntervalThree:
    case kD3dPresentIntervalFour:
    case kD3dPresentIntervalImmediate:
      return true;
    default:
      return false;
  }
}

HResult validatePresentParameters(const PresentParameters& params, bool extended) {
  const u32 maxSwapEffect = extended ? kD3dSwapEffectFlipex : kD3dSwapEffectCopy;
  if (params.swapEffect == 0 || params.swapEffect > maxSwapEffect) {
    return D3DERR_INVALIDCALL;
  }

  const u32 maxBackBufferCount = extended ? 30u : 3u;
  if (params.backBufferCount > maxBackBufferCount) {
    return D3DERR_INVALIDCALL;
  }

  if (params.swapEffect == kD3dSwapEffectCopy && params.backBufferCount > 1) {
    return D3DERR_INVALIDCALL;
  }

  if (!isValidPresentationInterval(params.presentationInterval) ||
      !isValidPresentationIntervalRaw(params.presentationIntervalRaw)) {
    return D3DERR_INVALIDCALL;
  }

  return D3D_OK;
}

HResult validateFullscreenModeRelation(const PresentParameters& params,
                                       const DisplayModeEx* fullscreenMode) {
  if (!fullscreenMode) {
    return D3D_OK;
  }
  if (params.windowed) {
    return D3DERR_INVALIDCALL;
  }
  if (fullscreenMode->width != params.backBufferWidth ||
      fullscreenMode->height != params.backBufferHeight) {
    return D3DERR_INVALIDCALL;
  }
  return D3D_OK;
}

PresentParameters applyFullscreenMode(PresentParameters params, const DisplayModeEx* fullscreenMode) {
  if (!fullscreenMode) {
    return params;
  }
  params.windowed = false;
  if (fullscreenMode->width != 0) {
    params.backBufferWidth = fullscreenMode->width;
  }
  if (fullscreenMode->height != 0) {
    params.backBufferHeight = fullscreenMode->height;
  }
  if (fullscreenMode->format != Format::Unknown) {
    params.backBufferFormat = fullscreenMode->format;
  }
  return params;
}

SwapDesc makeSwapDesc(const PresentParameters& params) {
  SwapDesc desc;
  desc.window = params.deviceWindow;
  desc.width = params.backBufferWidth;
  desc.height = params.backBufferHeight;
  desc.format = params.backBufferFormat;
  desc.interval = params.presentationInterval;
  desc.windowed = params.windowed;
  desc.backBufferCount = std::max(1u, params.backBufferCount);
  desc.displaySyncEnabled = params.presentationInterval != PresentInterval::Immediate;
  desc.multiSampleType = params.multiSampleType;
  return desc;
}

// hashBytes / hashString moved to src/util/util_hash.cpp so the ELF
// winemetal.so unix module can link them without pulling d3d9 in.

DeviceCaps makeDefaultCaps(const BackendLimits& limits) {
  constexpr u32 kCaps = 0x00000000u;
  constexpr u32 kCaps2 = 0x20000u | 0x40000000u | 0x20000000u;
  constexpr u32 kCaps3 = 0x00000020u | 0x00000100u | 0x00000200u;
  constexpr u32 kCursorCaps = 0x00000001u | 0x00000002u;
  constexpr u32 kPrimitiveMiscCaps = 0x002ecff2u;
  constexpr u32 kRasterCaps = 0x07332191u;
  constexpr u32 kCmpCaps = 0x000000ffu;
  constexpr u32 kShadeCaps = 0x00000008u | 0x00000200u | 0x00004000u | 0x00080000u;
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
  caps.numSimultaneousRTs = std::min(kMaxRenderTargets, limits.maxColorAttachments);
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

std::array<f32, 2> halfPixelFixup(const Viewport& viewport) {
  if (viewport.width == 0 || viewport.height == 0) {
    return {0.0f, 0.0f};
  }
  return {1.0f / static_cast<f32>(viewport.width), 1.0f / static_cast<f32>(viewport.height)};
}

std::vector<u32> decomposeTriangleFanIndices(std::span<const u32> indices) {
  std::vector<u32> out;
  if (indices.size() < 3) {
    return out;
  }
  out.reserve((indices.size() - 2) * 3);
  for (size_t i = 1; i + 1 < indices.size(); ++i) {
    out.push_back(indices[0]);
    out.push_back(indices[i]);
    out.push_back(indices[i + 1]);
  }
  return out;
}

std::vector<u8> convertTextureUpload(Format format, u32 width, u32 height, std::span<const u8> input) {
  const u32 bpp = bytesPerPixel(format);
  if (bpp == 0) {
    return {};
  }
  std::vector<u8> output(static_cast<size_t>(width) * height * bpp);
  const u32 srcPitch = pitchForFormat(format, width);
  if (input.size() < output.size()) {
    return {};
  }
  if (!copyPixels(output, srcPitch, width, height, format, std::vector<u8>(input.begin(), input.end()),
                  srcPitch, width, height, format)) {
    // Fall back to a raw copy when the format is not color-decodable.
    std::copy_n(input.begin(), std::min(output.size(), input.size()), output.begin());
  }
  return output;
}

void DeviceState::reset() {
  viewport = {};
  scissorRect = {};
  scissorEnabled = false;
  renderStates.clear();
  for (auto& stage : textureStageStates) {
    stage.clear();
  }
  for (auto& sampler : samplerStates) {
    sampler.clear();
  }
  transforms.clear();
  lights = {};
  lightEnabled.fill(false);
  material = {};
  streamBuffers.fill(nullptr);
  streamOffsets.fill(0);
  streamStrides.fill(0);
  indexBuffer.reset();
  indexType = IndexType::UInt16;
  vertexDecl = {};
  fvf = 0;
  vertexShader = {};
  pixelShader = {};
  vsConst = {};
  psConst = {};
  clipPlanes = {};
  textures.fill(nullptr);
  renderTargets = {};
  depthStencil = {};
  inScene = false;

  renderStates.set(RS_LIGHTING, 1);
  renderStates.set(RS_SPECULAR_ENABLE, 0);
  renderStates.set(RS_NORMALIZE_NORMALS, 0);
  renderStates.set(RS_FOG_TABLE_MODE, static_cast<u32>(FogMode::None));
  renderStates.set(RS_FOG_FROM_VERTEX, 1);
  renderStates.set(RS_RANGE_FOG, 0);
  renderStates.set(RS_ALPHA_TEST_ENABLE, 0);
  renderStates.set(RS_ALPHA_FUNC, static_cast<u32>(CompareFunc::Always));
  renderStates.set(RS_ALPHA_REF, 0);
  renderStates.set(RS_FOG_ENABLE, 0);
  renderStates.set(RS_FOG_COLOR, 0);
  renderStates.set(RS_FOG_START, std::bit_cast<u32>(1.0f));
  renderStates.set(RS_FOG_END, std::bit_cast<u32>(1.0f));
  renderStates.set(RS_FOG_DENSITY, std::bit_cast<u32>(1.0f));
  renderStates.set(RS_AMBIENT, 0);
  renderStates.set(RS_DIFFUSE_MATERIAL_SOURCE, 1);
  renderStates.set(RS_SPECULAR_MATERIAL_SOURCE, 2);
  renderStates.set(RS_AMBIENT_MATERIAL_SOURCE, 0);
  renderStates.set(RS_EMISSIVE_MATERIAL_SOURCE, 0);
  renderStates.set(RS_VERTEX_BLEND, 0);
  renderStates.set(RS_CLIP_PLANE_ENABLE, 0);
  renderStates.set(RS_POINT_SPRITE_ENABLE, 0);
  renderStates.set(RS_POINT_SCALE_ENABLE, 0);
  renderStates.set(RS_CULL_MODE, static_cast<u32>(CullMode::Ccw));
  renderStates.set(RS_Z_WRITE_ENABLE, 1);
  renderStates.set(RS_Z_FUNC, static_cast<u32>(CompareFunc::LessEqual));
  renderStates.set(RS_SRC_BLEND, static_cast<u32>(BlendFactor::One));
  renderStates.set(RS_DEST_BLEND, static_cast<u32>(BlendFactor::Zero));
  renderStates.set(RS_BLEND_OP, static_cast<u32>(BlendOp::Add));
  renderStates.set(RS_COLOR_WRITE_ENABLE, 0xf);
  renderStates.set(RS_Z_ENABLE, 1);
  renderStates.set(RS_ALPHABLEND_ENABLE, 0);
  renderStates.set(RS_BLEND_FACTOR, 0xffffffffu);
  renderStates.set(RS_SEPARATE_ALPHA_BLEND_ENABLE, 0);
  renderStates.set(RS_SRC_BLEND_ALPHA, static_cast<u32>(BlendFactor::One));
  renderStates.set(RS_DEST_BLEND_ALPHA, static_cast<u32>(BlendFactor::Zero));
  renderStates.set(RS_BLEND_OP_ALPHA, static_cast<u32>(BlendOp::Add));
  renderStates.set(RS_STENCIL_ENABLE, 0);
  renderStates.set(RS_STENCIL_FUNC, static_cast<u32>(CompareFunc::Always));
  renderStates.set(RS_STENCIL_FAIL, static_cast<u32>(StencilOp::Keep));
  renderStates.set(RS_STENCIL_ZFAIL, static_cast<u32>(StencilOp::Keep));
  renderStates.set(RS_STENCIL_PASS, static_cast<u32>(StencilOp::Keep));
  renderStates.set(RS_STENCIL_REF, 0);
  renderStates.set(RS_STENCIL_MASK, 0xffu);
  renderStates.set(RS_STENCIL_WRITEMASK, 0xffu);
  renderStates.set(RS_STENCIL_CCW_FUNC, static_cast<u32>(CompareFunc::Always));
  renderStates.set(RS_STENCIL_CCW_FAIL, static_cast<u32>(StencilOp::Keep));
  renderStates.set(RS_STENCIL_CCW_ZFAIL, static_cast<u32>(StencilOp::Keep));
  renderStates.set(RS_STENCIL_CCW_PASS, static_cast<u32>(StencilOp::Keep));
  renderStates.set(RS_STENCIL_CCW_REF, 0);
  renderStates.set(RS_STENCIL_CCW_MASK, 0xffu);
  renderStates.set(RS_STENCIL_CCW_WRITEMASK, 0xffu);

  for (size_t stageIndex = 0; stageIndex < textureStageStates.size(); ++stageIndex) {
    auto& stage = textureStageStates[stageIndex];
    stage.set(TSS_COLOR_OP, static_cast<u32>(stageIndex == 0 ? TextureOp::Modulate : TextureOp::Disable));
    stage.set(TSS_COLOR_ARG1, 2);  // D3DTA_TEXTURE
    stage.set(TSS_COLOR_ARG2, 1);  // D3DTA_CURRENT
    stage.set(TSS_ALPHA_OP, static_cast<u32>(stageIndex == 0 ? TextureOp::SelectArg1 : TextureOp::Disable));
    stage.set(TSS_ALPHA_ARG1, 2);  // D3DTA_TEXTURE
    stage.set(TSS_ALPHA_ARG2, 1);  // D3DTA_CURRENT
    stage.set(TSS_RESULT_ARG, 1);  // D3DTA_CURRENT
    stage.set(TSS_TEXCOORD_INDEX, static_cast<u32>(stageIndex));
    stage.set(TSS_TEXTURE_TRANSFORM_FLAGS, 0);
    stage.set(TSS_TEXTURE_TYPE, 0);
  }

  for (auto& sampler : samplerStates) {
    sampler.set(SAMP_MIN_FILTER, 1);
    sampler.set(SAMP_MAG_FILTER, 1);
    sampler.set(SAMP_MIP_FILTER, 0);
    sampler.set(SAMP_ADDRESS_U, 1);
    sampler.set(SAMP_ADDRESS_V, 1);
    sampler.set(SAMP_ADDRESS_W, 1);
    sampler.set(SAMP_MAX_ANISOTROPY, 1);
    sampler.set(SAMP_MIPMAP_LOD_BIAS, 0);
    sampler.set(SAMP_BORDER_COLOR, 0);
  }

  material.diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
}

Buffer::Buffer(std::shared_ptr<Device> owner, BufferHandle handle, BufferDesc desc)
    : owner_(std::move(owner)), handle_(handle), desc_(desc), storage_(static_cast<size_t>(desc.size)) {
  if (auto ownerPtr = owner_.lock()) {
    backend_ = ownerPtr->upperDevice();
  }
}

Buffer::~Buffer() {
  invalidate();
}

LockedRegion Buffer::lock(u64 offset, u64 size, u32 flags) {
  if (!valid_) {
    return {};
  }
  if ((flags & UsageDiscard) != 0 && (desc_.usage & UsageDynamic) != 0) {
    storage_.assign(static_cast<size_t>(std::max<u64>(size, desc_.size)), 0);
    offset = 0;
  } else if (storage_.size() < offset + size) {
    storage_.resize(static_cast<size_t>(offset + size));
  }
  if (backend_ && handle_) {
    backend_->mapBuffer(handle_, flags);
  }
  locked_ = true;
  return {storage_.data() + offset, static_cast<u32>(size)};
}

void Buffer::unlock() {
  if (backend_ && handle_) {
    backend_->uploadBufferData(handle_, storage_);
    backend_->unmapBuffer(handle_);
  }
  locked_ = false;
}

void Buffer::invalidate() {
  if (!valid_) {
    return;
  }
  valid_ = false;
  if (backend_ && handle_) {
    backend_->destroyBuffer(handle_);
  }
  handle_ = {};
}

Texture::Texture(std::shared_ptr<Device> owner, TextureHandle handle, TextureDesc desc)
    : owner_(std::move(owner)), handle_(handle), desc_(desc) {
  if (auto ownerPtr = owner_.lock()) {
    backend_ = ownerPtr->upperDevice();
  }
  const u32 mipLevels = levelCount();
  const u32 faceCount = desc_.type == TextureType::Cube ? 6u : 1u;
  levels_.resize(static_cast<size_t>(mipLevels) * faceCount);
  for (u32 subresource = 0; subresource < levels_.size(); ++subresource) {
    const u32 level = mipLevelForSubresource(subresource);
    LevelStorage storage;
    storage.width = std::max(1u, desc_.width >> level);
    storage.height = std::max(1u, desc_.height >> level);
    storage.pitch = formatRowPitch(desc_.format, storage.width);
    storage.bytes.resize(formatByteSize(desc_.format, storage.width, storage.height), 0);
    levels_[subresource] = std::move(storage);
  }
}

Texture::~Texture() {
  invalidate();
}

u32 Texture::levelCount() const noexcept {
  return std::max(1u, desc_.levels);
}

u32 Texture::mipLevelForSubresource(u32 subresource) const noexcept {
  const u32 mipLevels = levelCount();
  if (desc_.type == TextureType::Cube && mipLevels != 0) {
    return subresource % mipLevels;
  }
  return subresource;
}

LockedRegion Texture::lockRect(u32 subresource, const Rect* rect, u32 flags) {
  if (!valid_ || subresource >= levels_.size()) {
    return {};
  }
  LevelStorage& storage = levels_[subresource];
  if ((flags & UsageDiscard) != 0) {
    storage.bytes.assign(formatByteSize(desc_.format, storage.width, storage.height), 0);
  }
  locked_ = true;
  if (storage.pitch == 0 || storage.bytes.empty()) {
    return {};
  }
  const u32 left = rect ? std::max(0, rect->left) : 0;
  const u32 top = rect ? std::max(0, rect->top) : 0;
  if (isCompressedFormat(desc_.format)) {
    const u32 blockWidth = formatBlockWidth(desc_.format);
    const u32 blockHeight = formatBlockHeight(desc_.format);
    const u32 blockBytes = formatBlockBytes(desc_.format);
    const u32 blockX = std::min(left, storage.width - 1u) / blockWidth;
    const u32 blockY = std::min(top, storage.height - 1u) / blockHeight;
    return {storage.bytes.data() + static_cast<size_t>(blockY) * storage.pitch +
                static_cast<size_t>(blockX) * blockBytes,
            storage.pitch};
  }
  const u32 bpp = bytesPerPixel(desc_.format);
  return {storage.bytes.data() + static_cast<size_t>(top) * storage.pitch +
              static_cast<size_t>(left) * bpp,
          storage.pitch};
}

void Texture::unlockRect(u32 subresource) {
  if (subresource < levels_.size()) {
    levels_[subresource].dirty = true;
    syncLevelToBackend(subresource);
  }
  locked_ = false;
}

std::shared_ptr<Surface> Texture::surfaceLevel(u32 subresource) {
  if (subresource >= levels_.size()) {
    return {};
  }
  if (subresource < surfaces_.size()) {
    if (auto surface = surfaces_[subresource].lock()) {
      return surface;
    }
  } else {
    surfaces_.resize(subresource + 1);
  }
  auto owner = owner_.lock();
  if (!owner) {
    return {};
  }
  const u32 level = mipLevelForSubresource(subresource);
  SurfaceDesc surfaceDesc;
  surfaceDesc.width = std::max(1u, desc_.width >> level);
  surfaceDesc.height = std::max(1u, desc_.height >> level);
  surfaceDesc.format = desc_.format;
  surfaceDesc.pool = desc_.pool;
  surfaceDesc.usage = desc_.usage;
  surfaceDesc.renderTarget = (desc_.usage & UsageRenderTarget) != 0;
  surfaceDesc.depthStencil = (desc_.usage & UsageDepthStencil) != 0;
  auto surfaceHandle = backend_ ? backend_->createSurfaceForTexture(handle_, subresource, surfaceDesc) : SurfaceHandle{};
  if (!surfaceHandle && backend_) {
    surfaceHandle = backend_->createSurface(surfaceDesc);
  }
  if (!surfaceHandle) {
    surfaceHandle = Handle{owner->nextHandle_++};
  }
  auto surface = std::make_shared<Surface>(owner, surfaceHandle, shared_from_this(), subresource);
  surfaces_[subresource] = surface;
  return surface;
}

std::span<const u8> Texture::levelBytes(u32 subresource) const {
  if (subresource >= levels_.size()) {
    return {};
  }
  const auto& storage = levels_[subresource];
  return std::span<const u8>(storage.bytes.data(), storage.bytes.size());
}

void Texture::fillColor(const Rect* rect, ColorRGBA color) {
  if (!valid_ || levels_.empty()) {
    return;
  }
  auto& storage = levels_[0];
  fillBuffer(storage.bytes, storage.pitch, storage.width, storage.height, desc_.format, rect, color);
  storage.dirty = true;
  syncLevelToBackend(0);
}

void Texture::fillColor(u32 subresource, const Rect* rect, ColorRGBA color) {
  if (!valid_ || subresource >= levels_.size()) {
    return;
  }
  auto& storage = levels_[subresource];
  fillBuffer(storage.bytes, storage.pitch, storage.width, storage.height, desc_.format, rect, color);
  storage.dirty = true;
  syncLevelToBackend(subresource);
}

void Texture::copyFrom(const Texture& src) {
  if (!valid_ || !src.valid_ || desc_.format != src.desc_.format) {
    return;
  }
  const size_t levels = std::min(levels_.size(), src.levels_.size());
  for (size_t i = 0; i < levels; ++i) {
    levels_[i].bytes = src.levels_[i].bytes;
    levels_[i].dirty = true;
    syncLevelToBackend(static_cast<u32>(i));
  }
}

void Texture::syncLevelToBackend(u32 subresource) {
  if (!valid_ || !backend_ || !handle_ || subresource >= levels_.size()) {
    return;
  }
  const auto& storage = levels_[subresource];
  if (storage.bytes.empty() || storage.width == 0 || storage.height == 0 || storage.pitch == 0) {
    return;
  }
  if (const auto wanted = textureDumpHandle(); wanted && *wanted == handle_.value) {
    const auto path = (std::filesystem::path(textureDumpDir()) /
                       ("dxmt9_tex_" + std::to_string(handle_.value) + "_subresource_" +
                        std::to_string(subresource) + ".bmp"))
                          .string();
    if (writeBmpScreenshot(path, desc_.format, storage.width, storage.height, storage.pitch,
                           std::span<const u8>(storage.bytes.data(), storage.bytes.size()))) {
      emitRenderTrace("texture dump handle=0x%x subresource=%u path=%s format=%u size=%ux%u pitch=%u",
                      handle_.value, subresource, path.c_str(), static_cast<unsigned>(desc_.format),
                      storage.width, storage.height, storage.pitch);
    } else {
      emitRenderTrace("texture dump handle=0x%x subresource=%u failed format=%u size=%ux%u pitch=%u",
                      handle_.value, subresource, static_cast<unsigned>(desc_.format), storage.width,
                      storage.height, storage.pitch);
    }
  }
  backend_->uploadTextureLevel(handle_, subresource, storage.width, storage.height, storage.pitch,
                               std::span<const u8>(storage.bytes.data(), storage.bytes.size()));
}

void Texture::invalidate() {
  if (!valid_) {
    return;
  }
  valid_ = false;
  if (backend_ && handle_) {
    backend_->destroyTexture(handle_);
  }
  handle_ = {};
}

Surface::Surface(std::shared_ptr<Device> owner, SurfaceHandle handle, SurfaceDesc desc)
    : owner_(std::move(owner)), handle_(handle), desc_(desc), containerKind_(ContainerKind::Device) {
  if (auto ownerPtr = owner_.lock()) {
    backend_ = ownerPtr->upperDevice();
  }
  if (desc_.width != 0 && desc_.height != 0) {
    standalonePitch_ = formatRowPitch(desc_.format, desc_.width);
    standaloneBytes_.resize(formatByteSize(desc_.format, desc_.width, desc_.height), 0);
  }
}

Surface::Surface(std::shared_ptr<Device> owner, SurfaceHandle handle, std::shared_ptr<Texture> texture,
                 u32 level)
    : owner_(std::move(owner)), textureContainer_(std::move(texture)), handle_(handle), level_(level),
      containerKind_(ContainerKind::Texture) {
  if (auto ownerPtr = owner_.lock()) {
    backend_ = ownerPtr->upperDevice();
  }
  if (auto tex = textureContainer_.lock()) {
    const u32 mipLevel = tex->mipLevelForSubresource(level_);
    desc_.width = std::max(1u, tex->desc().width >> mipLevel);
    desc_.height = std::max(1u, tex->desc().height >> mipLevel);
    desc_.format = tex->desc().format;
    desc_.pool = tex->desc().pool;
    desc_.usage = tex->desc().usage;
    desc_.renderTarget = (tex->desc().usage & UsageRenderTarget) != 0;
    desc_.depthStencil = (tex->desc().usage & UsageDepthStencil) != 0;
  }
}

Surface::~Surface() {
  invalidate();
}

LockedRegion Surface::lockRect(const Rect* rect, u32 flags) {
  if (!valid_) {
    return {};
  }
  if (containerKind_ == ContainerKind::Texture) {
    if (auto tex = textureContainer_.lock()) {
      return tex->lockRect(level_, rect, flags);
    }
    return {};
  }
  if ((flags & UsageDiscard) != 0) {
    standaloneBytes_.assign(formatByteSize(desc_.format, desc_.width, desc_.height), 0);
  }
  locked_ = true;
  if (standalonePitch_ == 0 || standaloneBytes_.empty()) {
    return {};
  }
  const u32 left = rect ? std::max(0, rect->left) : 0;
  const u32 top = rect ? std::max(0, rect->top) : 0;
  if (isCompressedFormat(desc_.format)) {
    const u32 blockWidth = formatBlockWidth(desc_.format);
    const u32 blockHeight = formatBlockHeight(desc_.format);
    const u32 blockBytes = formatBlockBytes(desc_.format);
    const u32 blockX = std::min(left, desc_.width - 1u) / blockWidth;
    const u32 blockY = std::min(top, desc_.height - 1u) / blockHeight;
    return {standaloneBytes_.data() + static_cast<size_t>(blockY) * standalonePitch_ +
                static_cast<size_t>(blockX) * blockBytes,
            standalonePitch_};
  }
  const u32 bpp = bytesPerPixel(desc_.format);
  return {standaloneBytes_.data() + static_cast<size_t>(top) * standalonePitch_ + static_cast<size_t>(left) * bpp,
          standalonePitch_};
}

void Surface::unlockRect() {
  if (containerKind_ == ContainerKind::Texture) {
    if (auto tex = textureContainer_.lock()) {
      tex->unlockRect(level_);
    }
  }
  locked_ = false;
}

void Surface::fillColor(const Rect* rect, ColorRGBA color) {
  if (!valid_) {
    return;
  }
  if (containerKind_ == ContainerKind::Texture) {
    if (auto tex = textureContainer_.lock()) {
      tex->fillColor(level_, rect, color);
    }
    return;
  }
  fillBuffer(standaloneBytes_, standalonePitch_, desc_.width, desc_.height, desc_.format, rect, color);
}

void Surface::copyFrom(const Surface& src) {
  if (!valid_ || !src.valid_ || desc_.format != src.desc_.format) {
    return;
  }
  if (containerKind_ == ContainerKind::Texture) {
    if (auto tex = textureContainer_.lock()) {
      if (src.containerKind_ == ContainerKind::Texture) {
        if (auto srcTex = src.textureContainer_.lock()) {
          tex->copyFrom(*srcTex);
        }
      }
    }
    return;
  }
  if (src.containerKind_ == ContainerKind::Texture) {
    if (auto srcTex = src.textureContainer_.lock()) {
      if (!srcTex->levelBytes(src.level_).empty()) {
        const auto bytes = srcTex->levelBytes(src.level_);
        const size_t count = std::min(bytes.size(), standaloneBytes_.size());
        std::copy_n(bytes.begin(), count, standaloneBytes_.begin());
      }
    }
    return;
  }
  const size_t count = std::min(standaloneBytes_.size(), src.standaloneBytes_.size());
  std::copy_n(src.standaloneBytes_.begin(), count, standaloneBytes_.begin());
}

void Surface::invalidate() {
  valid_ = false;
  if (backend_ && handle_) {
    backend_->destroySurface(handle_);
  }
  handle_ = {};
}

Query::Query(QueryType type) : type_(type) {
  if (type_ == QueryType::TimestampFreq) {
    resolvedValue_ = 1000000000ull;
  } else if (type_ == QueryType::TimestampDisjoint) {
    resolvedValue_ = 0ull;
  }
}

/*
 * TLA+: QuerySeqId binding
 *   QueryIds[q]       -> each core::Query instance.
 *   qIssuedSeqId[q]  -> Query::issuedSequenceId_.
 *   currentSeqId     -> Device::submittedSequenceId_ + 1 before assignment;
 *                       the assigned seqId is submittedSequenceId_ after ++.
 *   completedSeqId   -> Device::completedSequenceId_.
 *   qState[q]        -> derived at GetData: unresolved while
 *                       completedSeqId < qIssuedSeqId[q], resolved at the
 *                       S_OK/data path once completedSeqId >= qIssuedSeqId[q].
 *   pendingFlush     -> caller-side Query::GetData(FLUSH) recorder/bridge
 *                       flush before this core query read observes the fence.
 *
 * Action mapping:
 *   IssueQuery       -> Device::issueQuery() assigns the seqId; Query::end()
 *                       stores it as qIssuedSeqId for D3DISSUE_END.
 *   GetDataFlush    -> Query::getData() unresolved FLUSH branch returns
 *                       S_FALSE after the synchronous flush boundary.
 *   GetDataSOK      -> Query::getData() resolved branch guarded by
 *                       completedSeqId >= qIssuedSeqId[q].
 *   GPUComplete     -> Device::completeUpTo() advances completedSeqId.
 */
void Query::begin(u64 sequenceId) {
  active_ = true;
  issuedSequenceId_ = sequenceId;
  resolvedValue_.reset();
}

void Query::end(u64 sequenceId) {
  active_ = false;
  issuedSequenceId_ = sequenceId;
}

void Query::resolve(u64 value) {
  resolvedValue_ = value;
}

HRESULT Query::getData(void* output, size_t size, u32 flags, u64 completedSequenceId) const {
  if (type_ == QueryType::TimestampFreq) {
    if (output && size >= sizeof(u64)) {
      *static_cast<u64*>(output) = 1000000000ull;
    }
    return S_OK;
  }
  if (type_ == QueryType::TimestampDisjoint) {
    if (output && size >= sizeof(u64)) {
      *static_cast<u64*>(output) = 0ull;
    }
    return S_OK;
  }

  if (completedSequenceId < issuedSequenceId_) {
    if ((flags & QUERY_GETDATA_FLUSH) != 0) {
      // TLA+: QuerySeqId / GetDataFlush
      // The FLUSH caller has committed pending query records before this
      // read; the fence is still unresolved, so the poll reports S_FALSE.
      // TLA+: QuerySeqId / NoDeadlockOnFlushSpin
      // A repeated FLUSH spin observes progress through completedSequenceId.
      return S_FALSE;
    }
    return S_FALSE;
  }

  // TLA+: QuerySeqId / GetDataSOK
  // TLA+: QuerySeqId / QueryResolutionSafety
  // A resolved query is only reported once the GPU has completed the chunk
  // that issued it.
  DXMT_ASSERT(completedSequenceId >= issuedSequenceId_);

  if (type_ == QueryType::Event) {
    return S_OK;
  }

  const u64 value = resolvedValue_.value_or(0ull);
  if (type_ == QueryType::Occlusion) {
    if (!output || size < sizeof(u32)) {
      return D3DERR_INVALIDCALL;
    }
    *static_cast<u32*>(output) = static_cast<u32>(std::min<u64>(value, std::numeric_limits<u32>::max()));
    return S_OK;
  }
  if (type_ == QueryType::Timestamp) {
    if (!output || size < sizeof(u64)) {
      return D3DERR_INVALIDCALL;
    }
    *static_cast<u64*>(output) = value;
    return S_OK;
  }
  return D3DERR_NOTAVAILABLE;
}

namespace {

template <typename Map, typename Key, typename Value>
void setMapValue(Map& dst, const Key& key, const Value& value) {
  dst[key] = value;
}

template <std::size_t MaxEntries>
void setMapValue(StateValueTable<MaxEntries>& dst, u32 key, u32 value) {
  dst.set(key, value);
}

template <typename Map>
void applyMapDelta(Map& dst, const Map& before, const Map& after) {
  for (const auto& entry : after) {
    const auto key = entry.first;
    const auto& value = entry.second;
    const auto beforeIt = before.find(key);
    if (beforeIt == before.end() || beforeIt->second != value) {
      setMapValue(dst, key, value);
    }
  }
  for (const auto& entry : before) {
    const auto key = entry.first;
    if (!after.contains(key)) {
      dst.erase(key);
    }
  }
}

template <typename Map>
void captureMapDelta(Map& snapshot, const Map& before, const Map& recorded, const Map& current) {
  for (const auto& entry : recorded) {
    const auto key = entry.first;
    const auto& value = entry.second;
    const auto beforeIt = before.find(key);
    if (beforeIt == before.end() || beforeIt->second != value) {
      const auto currentIt = current.find(key);
      if (currentIt != current.end()) {
        setMapValue(snapshot, key, currentIt->second);
      } else {
        snapshot.erase(key);
      }
    }
  }
  for (const auto& entry : before) {
    const auto key = entry.first;
    const auto& value = entry.second;
    if (!recorded.contains(key)) {
      const auto currentIt = current.find(key);
      if (currentIt == current.end() || currentIt->second != value) {
        if (currentIt != current.end()) {
          setMapValue(snapshot, key, currentIt->second);
        } else {
          snapshot.erase(key);
        }
      }
    }
  }
}

template <typename T>
void applyIfChanged(T& dst, const T& before, const T& after) {
  if (!(before == after)) {
    dst = after;
  }
}

template <typename T>
void captureIfRecorded(T& snapshot, const T& before, const T& recorded, const T& current) {
  if (!(before == recorded)) {
    snapshot = current;
  }
}

template <size_t FloatCount>
void applyShaderConstantsDelta(ShaderConstantSnapshot<FloatCount>& dst,
                               const ShaderConstantSnapshot<FloatCount>& before,
                               const ShaderConstantSnapshot<FloatCount>& after) {
  for (size_t i = 0; i < after.float4.size(); ++i) {
    if (before.float4[i] != after.float4[i]) {
      dst.float4[i] = after.float4[i];
    }
  }
  for (size_t i = 0; i < after.int4.size(); ++i) {
    if (before.int4[i] != after.int4[i]) {
      dst.int4[i] = after.int4[i];
    }
  }
  for (size_t i = 0; i < after.bools.size(); ++i) {
    if (before.bools[i] != after.bools[i]) {
      dst.bools[i] = after.bools[i];
    }
  }
}

template <size_t FloatCount>
void captureShaderConstantsDelta(ShaderConstantSnapshot<FloatCount>& snapshot,
                                 const ShaderConstantSnapshot<FloatCount>& before,
                                 const ShaderConstantSnapshot<FloatCount>& recorded,
                                 const ShaderConstantSnapshot<FloatCount>& current) {
  for (size_t i = 0; i < recorded.float4.size(); ++i) {
    if (before.float4[i] != recorded.float4[i]) {
      snapshot.float4[i] = current.float4[i];
    }
  }
  for (size_t i = 0; i < recorded.int4.size(); ++i) {
    if (before.int4[i] != recorded.int4[i]) {
      snapshot.int4[i] = current.int4[i];
    }
  }
  for (size_t i = 0; i < recorded.bools.size(); ++i) {
    if (before.bools[i] != recorded.bools[i]) {
      snapshot.bools[i] = current.bools[i];
    }
  }
}

constexpr u32 kRsFillMode = 8;
constexpr u32 kRsShadeMode = 9;
constexpr u32 kRsLastPixel = 16;
constexpr u32 kRsDitherEnable = 26;
constexpr u32 kRsWrap0 = 128;
constexpr u32 kRsWrap1 = 129;
constexpr u32 kRsWrap2 = 130;
constexpr u32 kRsWrap3 = 131;
constexpr u32 kRsWrap4 = 132;
constexpr u32 kRsWrap5 = 133;
constexpr u32 kRsWrap6 = 134;
constexpr u32 kRsWrap7 = 135;
constexpr u32 kRsClipping = 136;
constexpr u32 kRsColorVertex = 141;
constexpr u32 kRsLocalViewer = 142;
constexpr u32 kRsPointSize = 154;
constexpr u32 kRsPointSizeMin = 155;
constexpr u32 kRsPointScaleA = 158;
constexpr u32 kRsPointScaleB = 159;
constexpr u32 kRsPointScaleC = 160;
constexpr u32 kRsMultisampleAntialias = 161;
constexpr u32 kRsMultisampleMask = 162;
constexpr u32 kRsPatchEdgeStyle = 163;
constexpr u32 kRsPointSizeMax = 166;
constexpr u32 kRsIndexedVertexBlendEnable = 167;
constexpr u32 kRsTweenFactor = 170;
constexpr u32 kRsPositionDegree = 172;
constexpr u32 kRsNormalDegree = 173;
constexpr u32 kRsSlopeScaleDepthBias = 175;
constexpr u32 kRsAntialiasedLineEnable = 176;
constexpr u32 kRsMinTessellationLevel = 178;
constexpr u32 kRsMaxTessellationLevel = 179;
constexpr u32 kRsAdaptiveTessX = 180;
constexpr u32 kRsAdaptiveTessY = 181;
constexpr u32 kRsAdaptiveTessZ = 182;
constexpr u32 kRsAdaptiveTessW = 183;
constexpr u32 kRsEnableAdaptiveTessellation = 184;
constexpr u32 kRsTwoSidedStencilMode = 185;
constexpr u32 kRsColorWriteEnable1 = 190;
constexpr u32 kRsColorWriteEnable2 = 191;
constexpr u32 kRsColorWriteEnable3 = 192;
constexpr u32 kRsDepthBias = 195;
constexpr u32 kRsWrap8 = 198;
constexpr u32 kRsWrap9 = 199;
constexpr u32 kRsWrap10 = 200;
constexpr u32 kRsWrap11 = 201;
constexpr u32 kRsWrap12 = 202;
constexpr u32 kRsWrap13 = 203;
constexpr u32 kRsWrap14 = 204;
constexpr u32 kRsWrap15 = 205;

constexpr auto kPixelStateRenderStates = std::to_array<u32>({
    RS_ALPHABLEND_ENABLE,
    RS_ALPHA_FUNC,
    RS_ALPHA_REF,
    RS_ALPHA_TEST_ENABLE,
    kRsAntialiasedLineEnable,
    RS_BLEND_FACTOR,
    RS_BLEND_OP,
    RS_BLEND_OP_ALPHA,
    RS_STENCIL_CCW_FAIL,
    RS_STENCIL_CCW_FUNC,
    RS_STENCIL_CCW_PASS,
    RS_STENCIL_CCW_ZFAIL,
    RS_COLOR_WRITE_ENABLE,
    kRsColorWriteEnable1,
    kRsColorWriteEnable2,
    kRsColorWriteEnable3,
    kRsDepthBias,
    RS_DEST_BLEND,
    RS_DEST_BLEND_ALPHA,
    kRsDitherEnable,
    kRsFillMode,
    RS_FOG_DENSITY,
    RS_FOG_END,
    RS_FOG_START,
    kRsLastPixel,
    RS_SCISSOR_TEST_ENABLE,
    RS_SEPARATE_ALPHA_BLEND_ENABLE,
    kRsShadeMode,
    kRsSlopeScaleDepthBias,
    RS_SRC_BLEND,
    RS_SRC_BLEND_ALPHA,
    RS_SRGB_WRITE_ENABLE,
    RS_STENCIL_ENABLE,
    RS_STENCIL_FAIL,
    RS_STENCIL_FUNC,
    RS_STENCIL_MASK,
    RS_STENCIL_PASS,
    RS_STENCIL_REF,
    RS_STENCIL_WRITEMASK,
    RS_STENCIL_ZFAIL,
    RS_TEXTURE_FACTOR,
    kRsTwoSidedStencilMode,
    kRsWrap0,
    kRsWrap1,
    kRsWrap10,
    kRsWrap11,
    kRsWrap12,
    kRsWrap13,
    kRsWrap14,
    kRsWrap15,
    kRsWrap2,
    kRsWrap3,
    kRsWrap4,
    kRsWrap5,
    kRsWrap6,
    kRsWrap7,
    kRsWrap8,
    kRsWrap9,
    RS_Z_ENABLE,
    RS_Z_FUNC,
    RS_Z_WRITE_ENABLE,
});

constexpr auto kVertexStateRenderStates = std::to_array<u32>({
    kRsAdaptiveTessW,
    kRsAdaptiveTessX,
    kRsAdaptiveTessY,
    kRsAdaptiveTessZ,
    RS_AMBIENT,
    RS_AMBIENT_MATERIAL_SOURCE,
    kRsClipping,
    RS_CLIP_PLANE_ENABLE,
    kRsColorVertex,
    RS_CULL_MODE,
    RS_DIFFUSE_MATERIAL_SOURCE,
    RS_EMISSIVE_MATERIAL_SOURCE,
    kRsEnableAdaptiveTessellation,
    RS_FOG_COLOR,
    RS_FOG_DENSITY,
    RS_FOG_ENABLE,
    RS_FOG_END,
    RS_FOG_START,
    RS_FOG_TABLE_MODE,
    RS_FOG_FROM_VERTEX,
    kRsIndexedVertexBlendEnable,
    RS_LIGHTING,
    kRsLocalViewer,
    kRsMaxTessellationLevel,
    kRsMinTessellationLevel,
    kRsMultisampleAntialias,
    kRsMultisampleMask,
    kRsNormalDegree,
    RS_NORMALIZE_NORMALS,
    kRsPatchEdgeStyle,
    kRsPointScaleA,
    kRsPointScaleB,
    kRsPointScaleC,
    RS_POINT_SCALE_ENABLE,
    kRsPointSize,
    kRsPointSizeMax,
    kRsPointSizeMin,
    RS_POINT_SPRITE_ENABLE,
    kRsPositionDegree,
    RS_RANGE_FOG,
    kRsShadeMode,
    RS_SPECULAR_ENABLE,
    RS_SPECULAR_MATERIAL_SOURCE,
    kRsTweenFactor,
    RS_VERTEX_BLEND,
});

template <size_t N>
void copyRenderStates(DeviceState& dst, const DeviceState& src, const std::array<u32, N>& keys) {
  for (u32 key : keys) {
    const auto it = src.renderStates.find(key);
    if (it != src.renderStates.end()) {
      dst.renderStates.set(key, it->second);
    } else {
      dst.renderStates.erase(key);
    }
  }
}

void syncRenderStateDerived(DeviceState& state) {
  const auto scissorIt = state.renderStates.find(RS_SCISSOR_TEST_ENABLE);
  state.scissorEnabled = scissorIt != state.renderStates.end() && scissorIt->second != 0;
}

void applyFullSnapshotState(DeviceState& dst, const DeviceState& src, StateBlockType type) {
  switch (type) {
    case StateBlockType::All:
    case StateBlockType::Recorded: {
      const bool inScene = dst.inScene;
      dst = src;
      dst.inScene = inScene;
      return;
    }
    case StateBlockType::PixelState:
      copyRenderStates(dst, src, kPixelStateRenderStates);
      dst.textureStageStates = src.textureStageStates;
      dst.samplerStates = src.samplerStates;
      dst.pixelShader = src.pixelShader;
      dst.psConst = src.psConst;
      syncRenderStateDerived(dst);
      return;
    case StateBlockType::VertexState:
      copyRenderStates(dst, src, kVertexStateRenderStates);
      dst.vertexDecl = src.vertexDecl;
      dst.fvf = src.fvf;
      dst.vertexShader = src.vertexShader;
      dst.vsConst = src.vsConst;
      dst.lights = src.lights;
      dst.lightEnabled = src.lightEnabled;
      syncRenderStateDerived(dst);
      return;
  }
}

}  // namespace

void StateBlock::capture(const DeviceState& state) {
  if (mode_ == CaptureMode::FullSnapshot) {
    snapshot_ = state;
    baseline_ = {};
    recordedRenderStates_.clear();
    return;
  }

  const DeviceState recorded = snapshot_;
  captureIfRecorded(snapshot_.viewport, baseline_.viewport, recorded.viewport, state.viewport);
  captureIfRecorded(snapshot_.scissorRect, baseline_.scissorRect, recorded.scissorRect, state.scissorRect);
  captureIfRecorded(snapshot_.scissorEnabled, baseline_.scissorEnabled, recorded.scissorEnabled, state.scissorEnabled);
  if (!recordedRenderStates_.empty()) {
    for (u32 key : recordedRenderStates_) {
      const auto currentIt = state.renderStates.find(key);
      if (currentIt != state.renderStates.end()) {
        snapshot_.renderStates.set(key, currentIt->second);
      } else {
        snapshot_.renderStates.erase(key);
      }
    }
  } else {
    captureMapDelta(snapshot_.renderStates, baseline_.renderStates, recorded.renderStates, state.renderStates);
  }
  for (size_t i = 0; i < snapshot_.textureStageStates.size(); ++i) {
    captureMapDelta(snapshot_.textureStageStates[i], baseline_.textureStageStates[i], recorded.textureStageStates[i],
                    state.textureStageStates[i]);
  }
  for (size_t i = 0; i < snapshot_.samplerStates.size(); ++i) {
    captureMapDelta(snapshot_.samplerStates[i], baseline_.samplerStates[i], recorded.samplerStates[i],
                    state.samplerStates[i]);
  }
  captureMapDelta(snapshot_.transforms, baseline_.transforms, recorded.transforms, state.transforms);
  for (size_t i = 0; i < snapshot_.lights.size(); ++i) {
    captureIfRecorded(snapshot_.lights[i], baseline_.lights[i], recorded.lights[i], state.lights[i]);
    captureIfRecorded(snapshot_.lightEnabled[i], baseline_.lightEnabled[i], recorded.lightEnabled[i],
                      state.lightEnabled[i]);
  }
  captureIfRecorded(snapshot_.material, baseline_.material, recorded.material, state.material);
  for (size_t i = 0; i < snapshot_.streamBuffers.size(); ++i) {
    if (baseline_.streamBuffers[i] != recorded.streamBuffers[i] ||
        baseline_.streamOffsets[i] != recorded.streamOffsets[i] ||
        baseline_.streamStrides[i] != recorded.streamStrides[i]) {
      snapshot_.streamBuffers[i] = state.streamBuffers[i];
      snapshot_.streamOffsets[i] = state.streamOffsets[i];
      snapshot_.streamStrides[i] = state.streamStrides[i];
    }
  }
  if (baseline_.indexBuffer != recorded.indexBuffer || baseline_.indexType != recorded.indexType) {
    snapshot_.indexBuffer = state.indexBuffer;
    snapshot_.indexType = state.indexType;
  }
  captureIfRecorded(snapshot_.vertexDecl, baseline_.vertexDecl, recorded.vertexDecl, state.vertexDecl);
  captureIfRecorded(snapshot_.fvf, baseline_.fvf, recorded.fvf, state.fvf);
  captureIfRecorded(snapshot_.vertexShader, baseline_.vertexShader, recorded.vertexShader, state.vertexShader);
  captureIfRecorded(snapshot_.pixelShader, baseline_.pixelShader, recorded.pixelShader, state.pixelShader);
  captureShaderConstantsDelta(snapshot_.vsConst, baseline_.vsConst, recorded.vsConst, state.vsConst);
  captureShaderConstantsDelta(snapshot_.psConst, baseline_.psConst, recorded.psConst, state.psConst);
  for (size_t i = 0; i < snapshot_.textures.size(); ++i) {
    captureIfRecorded(snapshot_.textures[i], baseline_.textures[i], recorded.textures[i], state.textures[i]);
  }
  for (size_t i = 0; i < snapshot_.renderTargets.size(); ++i) {
    captureIfRecorded(snapshot_.renderTargets[i], baseline_.renderTargets[i], recorded.renderTargets[i],
                      state.renderTargets[i]);
  }
  captureIfRecorded(snapshot_.depthStencil, baseline_.depthStencil, recorded.depthStencil, state.depthStencil);
  for (size_t i = 0; i < snapshot_.clipPlanes.size(); ++i) {
    captureIfRecorded(snapshot_.clipPlanes[i], baseline_.clipPlanes[i], recorded.clipPlanes[i], state.clipPlanes[i]);
  }
}

void StateBlock::captureDelta(const DeviceState& before, const DeviceState& after) {
  mode_ = CaptureMode::Delta;
  type_ = StateBlockType::Recorded;
  baseline_ = before;
  snapshot_ = after;
  recordedRenderStates_.clear();
}

void StateBlock::captureDelta(const DeviceState& before, const DeviceState& after,
                              const std::unordered_set<u32>& recordedRenderStates) {
  mode_ = CaptureMode::Delta;
  type_ = StateBlockType::Recorded;
  baseline_ = before;
  snapshot_ = after;
  recordedRenderStates_ = recordedRenderStates;
}

void StateBlock::apply(Device& device) const {
  auto& state = device.mutableState();
  if (mode_ == CaptureMode::FullSnapshot) {
    applyFullSnapshotState(state, snapshot_, type_);
    return;
  }

  applyIfChanged(state.viewport, baseline_.viewport, snapshot_.viewport);
  applyIfChanged(state.scissorRect, baseline_.scissorRect, snapshot_.scissorRect);
  applyIfChanged(state.scissorEnabled, baseline_.scissorEnabled, snapshot_.scissorEnabled);
  if (!recordedRenderStates_.empty()) {
    for (u32 key : recordedRenderStates_) {
      const auto snapshotIt = snapshot_.renderStates.find(key);
      if (snapshotIt != snapshot_.renderStates.end()) {
        state.renderStates.set(key, snapshotIt->second);
      } else {
        state.renderStates.erase(key);
      }
    }
  } else {
    applyMapDelta(state.renderStates, baseline_.renderStates, snapshot_.renderStates);
  }
  syncRenderStateDerived(state);
  for (size_t i = 0; i < state.textureStageStates.size(); ++i) {
    applyMapDelta(state.textureStageStates[i], baseline_.textureStageStates[i], snapshot_.textureStageStates[i]);
  }
  for (size_t i = 0; i < state.samplerStates.size(); ++i) {
    applyMapDelta(state.samplerStates[i], baseline_.samplerStates[i], snapshot_.samplerStates[i]);
  }
  applyMapDelta(state.transforms, baseline_.transforms, snapshot_.transforms);
  for (size_t i = 0; i < state.lights.size(); ++i) {
    applyIfChanged(state.lights[i], baseline_.lights[i], snapshot_.lights[i]);
    applyIfChanged(state.lightEnabled[i], baseline_.lightEnabled[i], snapshot_.lightEnabled[i]);
  }
  applyIfChanged(state.material, baseline_.material, snapshot_.material);
  for (size_t i = 0; i < state.streamBuffers.size(); ++i) {
    if (baseline_.streamBuffers[i] != snapshot_.streamBuffers[i] ||
        baseline_.streamOffsets[i] != snapshot_.streamOffsets[i] ||
        baseline_.streamStrides[i] != snapshot_.streamStrides[i]) {
      state.streamBuffers[i] = snapshot_.streamBuffers[i];
      state.streamOffsets[i] = snapshot_.streamOffsets[i];
      state.streamStrides[i] = snapshot_.streamStrides[i];
    }
  }
  if (baseline_.indexBuffer != snapshot_.indexBuffer || baseline_.indexType != snapshot_.indexType) {
    state.indexBuffer = snapshot_.indexBuffer;
    state.indexType = snapshot_.indexType;
  }
  applyIfChanged(state.vertexDecl, baseline_.vertexDecl, snapshot_.vertexDecl);
  applyIfChanged(state.fvf, baseline_.fvf, snapshot_.fvf);
  applyIfChanged(state.vertexShader, baseline_.vertexShader, snapshot_.vertexShader);
  applyIfChanged(state.pixelShader, baseline_.pixelShader, snapshot_.pixelShader);
  applyShaderConstantsDelta(state.vsConst, baseline_.vsConst, snapshot_.vsConst);
  applyShaderConstantsDelta(state.psConst, baseline_.psConst, snapshot_.psConst);
  for (size_t i = 0; i < state.textures.size(); ++i) {
    applyIfChanged(state.textures[i], baseline_.textures[i], snapshot_.textures[i]);
  }
  for (size_t i = 0; i < state.renderTargets.size(); ++i) {
    applyIfChanged(state.renderTargets[i], baseline_.renderTargets[i], snapshot_.renderTargets[i]);
  }
  applyIfChanged(state.depthStencil, baseline_.depthStencil, snapshot_.depthStencil);
  for (size_t i = 0; i < state.clipPlanes.size(); ++i) {
    applyIfChanged(state.clipPlanes[i], baseline_.clipPlanes[i], snapshot_.clipPlanes[i]);
  }
}

SwapChain::SwapChain(std::shared_ptr<Device> owner, SwapChainHandle handle, PresentParameters params,
                     std::shared_ptr<Surface> backBuffer, std::shared_ptr<Surface> depthStencil)
    : owner_(std::move(owner)), handle_(handle), params_(params), backBuffer_(std::move(backBuffer)),
      depthStencilSurface_(std::move(depthStencil)) {
  ensurePresenter();
}

SwapChain::~SwapChain() = default;

void SwapChain::ensurePresenter() {
  auto owner = owner_.lock();
  if (!owner) {
    return;
  }
  const auto& upper = owner->upperDevice();
  if (!upper) {
    return;
  }
  auto wmtDevice = upper->wmtDevice();
  if (!wmtDevice) {
    return;
  }
  const u64 hwnd = params_.deviceWindow.value;
  if (!hwnd) {
    return;
  }
  presenter_ = std::make_unique<dxmt9::Presenter>(wmtDevice, hwnd, 0ull,
                                                    upper->shaderArchive(),
                                                    upper->shaderArchivePath());
  if (!presenter_->valid()) {
    presenter_.reset();
  }
}

bool SwapChain::displaySyncEnabled() const noexcept {
  return params_.presentationInterval != PresentInterval::Immediate;
}

void SwapChain::resize(const PresentParameters& params) {
  params_ = params;
  auto owner = owner_.lock();
  if (!owner) {
    return;
  }

  const u32 width = std::max(1u, params_.backBufferWidth);
  const u32 height = std::max(1u, params_.backBufferHeight);
  backBuffer_ = owner->createSurface({width, height, params_.backBufferFormat, Pool::Default,
                                      UsageRenderTarget, true, false, params_.multiSampleType});
  if (params_.enableAutoDepthStencil) {
    depthStencilSurface_ = owner->createSurface({width, height, params_.autoDepthStencilFormat, Pool::Default,
                                                 UsageDepthStencil, false, true, params_.multiSampleType});
  } else {
    depthStencilSurface_.reset();
  }
}

HResult SwapChain::present(std::shared_ptr<dxmt9::Device> device, const SwapDesc& desc) {
  if (device) {
    SwapDesc adjusted = desc;
    if (backBuffer_) {
      adjusted.sourceSurface = backBuffer_->handle();
    }
    device->present(adjusted);
  }
  return D3D_OK;
}

Device::Device(AdapterInfo adapter, BackendLimits limits,
               PresentParameters params, u32 behaviorFlags,
               std::shared_ptr<dxmt9::Device> upperDevice,
               bool extendedDevice)
    : adapter_(std::move(adapter)), limits_(limits),
      caps_(makeDefaultCaps(limits_)),
      backend_(upperDevice),
      upperDevice_(std::move(upperDevice)),
      presentParameters_(normalizePresentParameters(adapter_, params)), behaviorFlags_(behaviorFlags),
      extendedDevice_(extendedDevice) {
  state_.reset();
  const u32 width = std::max(1u, presentParameters_.backBufferWidth);
  const u32 height = std::max(1u, presentParameters_.backBufferHeight);
  state_.viewport = {0, 0, width, height, 0.0f, 1.0f};
  deviceLost_ = false;
  maximumFrameLatency_ = kDefaultFrameLatency;
  if (backend_) {
    backend_->setMaxFrameLatency(maximumFrameLatency_);
  }
  experimentCapture_.path = getenvString("DXMT_EXPERIMENT_CAPTURE_PATH");
  experimentCapture_.frame = parseEnvU32("DXMT_CAPTURE_FRAME").value_or(0);
}

Device::~Device() {
  if (backend_) {
    upperDevice_->flush();
  }
  completeUpTo(submittedSequenceId_);
  // SeqIdSafety / drain-before-teardown: pending work is drained before the
  // default-pool resources are invalidated.
  DXMT_ASSERT(completedSequenceId_ == submittedSequenceId_);
  invalidateDefaultPoolResources();
}

HResult Device::testCooperativeLevel() const {
  return deviceLost_ ? D3DERR_DEVICELOST : D3D_OK;
}

HResult Device::checkDeviceState() const {
  if (deviceLost_) {
    return D3DERR_DEVICELOST;
  }
  if (presentOccluded_) {
    return S_PRESENT_OCCLUDED;
  }
  return D3D_OK;
}

std::shared_ptr<Buffer> Device::createBuffer(const BufferDesc& desc) {
  auto handle = upperDevice_ ? upperDevice_->createBuffer(desc) : BufferHandle{};
  if (!handle) {
    handle = Handle{nextHandle_++};
  }
  auto buffer = std::make_shared<Buffer>(shared_from_this(), handle, desc);
  registerBuffer(buffer);
  return buffer;
}

std::shared_ptr<Texture> Device::createTexture(const TextureDesc& desc) {
  auto handle = upperDevice_ ? upperDevice_->createTexture(desc) : TextureHandle{};
  if (!handle) {
    handle = Handle{nextHandle_++};
  }
  auto texture = std::make_shared<Texture>(shared_from_this(), handle, desc);
  registerTexture(texture);
  return texture;
}

std::shared_ptr<Surface> Device::createSurface(const SurfaceDesc& desc) {
  auto handle = upperDevice_ ? upperDevice_->createSurface(desc) : SurfaceHandle{};
  if (!handle) {
    handle = Handle{nextHandle_++};
  }
  auto surface = std::make_shared<Surface>(shared_from_this(), handle, desc);
  registerSurface(surface);
  return surface;
}

std::shared_ptr<Query> Device::createQuery(QueryType type) {
  auto query = std::make_shared<Query>(type);
  queries_.push_back(query);
  return query;
}

std::shared_ptr<StateBlock> Device::createStateBlock(StateBlockType type) const {
  auto block = std::make_shared<StateBlock>(type);
  block->capture(state_);
  return block;
}

std::shared_ptr<StateBlock> Device::captureStateBlock() const {
  return createStateBlock();
}

HResult Device::applyStateBlock(const StateBlock& block) {
  block.apply(*this);
  invalidateDrawStateCache();
  return D3D_OK;
}

std::shared_ptr<SwapChain> Device::createAdditionalSwapChain(const PresentParameters& params) {
  if (validatePresentParameters(params, extendedDevice_) != D3D_OK) {
    return {};
  }
  const auto normalized = normalizePresentParameters(adapter_, params);
  auto backBuffer = createSurface({std::max(1u, normalized.backBufferWidth),
                                   std::max(1u, normalized.backBufferHeight), normalized.backBufferFormat,
                                   Pool::Default, UsageRenderTarget, true, false, normalized.multiSampleType});
  std::shared_ptr<Surface> depth;
  if (normalized.enableAutoDepthStencil) {
    depth = createSurface({std::max(1u, normalized.backBufferWidth),
                           std::max(1u, normalized.backBufferHeight), normalized.autoDepthStencilFormat,
                           Pool::Default, UsageDepthStencil, false, true, normalized.multiSampleType});
  }
  auto swapChain = std::make_shared<SwapChain>(shared_from_this(), Handle{nextHandle_++}, normalized, backBuffer,
                                               depth);
  swapChains_.push_back(swapChain);
  return swapChain;
}

void Device::initializeDefaultSwapChain() {
  if (!swapChains_.empty()) {
    return;
  }
  const u32 width = std::max(1u, presentParameters_.backBufferWidth);
  const u32 height = std::max(1u, presentParameters_.backBufferHeight);
  auto backBuffer = createSurface({width, height, presentParameters_.backBufferFormat, Pool::Default,
                                   UsageRenderTarget, true, false, presentParameters_.multiSampleType});
  std::shared_ptr<Surface> depth;
  if (presentParameters_.enableAutoDepthStencil) {
    depth = createSurface({width, height, presentParameters_.autoDepthStencilFormat, Pool::Default,
                           UsageDepthStencil, false, true, presentParameters_.multiSampleType});
  }
  swapChains_.push_back(std::make_shared<SwapChain>(shared_from_this(), Handle{nextHandle_++},
                                                    presentParameters_, backBuffer, depth));
  state_.renderTargets[0] = backBuffer ? RenderTargetAttachment{backBuffer->handle(), 0, backBuffer->multiSampleCount()}
                                       : RenderTargetAttachment{};
  state_.depthStencil = depth ? RenderTargetAttachment{depth->handle(), 0, depth->multiSampleCount()}
                              : RenderTargetAttachment{};
  invalidateDrawStateCache();
}

std::shared_ptr<SwapChain> Device::swapChain(size_t index) const {
  if (index >= swapChains_.size()) {
    return {};
  }
  return swapChains_[index];
}

HResult Device::setRenderState(u32 key, u32 value) {
  state_.renderStates.set(key, value);
  if (key == RS_SCISSOR_TEST_ENABLE) {
    state_.scissorEnabled = value != 0;
  }
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setRenderStateFloat(u32 key, f32 value) {
  state_.renderStates.set(key, std::bit_cast<u32>(value));
  invalidateDrawStateCache();
  return D3D_OK;
}

u32 Device::getRenderState(u32 key) const {
  if (auto it = state_.renderStates.find(key); it != state_.renderStates.end()) {
    return it->second;
  }
  return 0;
}

f32 Device::getRenderStateFloat(u32 key, f32 defaultValue) const {
  if (auto it = state_.renderStates.find(key); it != state_.renderStates.end()) {
    return std::bit_cast<f32>(it->second);
  }
  return defaultValue;
}

HResult Device::setTextureStageState(u32 stage, u32 key, u32 value) {
  stage = std::min<u32>(stage, kMaxTextureStages - 1);
  key = std::min<u32>(key, kMaxTextureStageStates - 1);
  state_.textureStageStates[stage].set(key, value);
  invalidateDrawStateCache();
  return D3D_OK;
}

u32 Device::getTextureStageState(u32 stage, u32 key) const {
  if (stage >= kMaxTextureStages) {
    return 0;
  }
  const auto& map = state_.textureStageStates[stage];
  if (auto it = map.find(key); it != map.end()) {
    return it->second;
  }
  return 0;
}

HResult Device::setSamplerState(u32 sampler, u32 key, u32 value) {
  if (sampler >= kMaxSamplers) {
    return D3DERR_INVALIDCALL;
  }
  state_.samplerStates[sampler].set(key, value);
  invalidateDrawStateCache();
  return D3D_OK;
}

u32 Device::getSamplerState(u32 sampler, u32 key) const {
  if (sampler >= kMaxSamplers) {
    return 0;
  }
  const auto& map = state_.samplerStates[sampler];
  if (auto it = map.find(key); it != map.end()) {
    return it->second;
  }
  return 0;
}

HResult Device::setTransform(u32 key, const Matrix4x4& matrix) {
  state_.transforms.set(key, matrix);
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setLight(u32 index, const Light& light) {
  if (index >= kMaxLights) {
    return D3DERR_INVALIDCALL;
  }
  state_.lights[index] = light;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::lightEnable(u32 index, bool enable) {
  if (index >= kMaxLights) {
    return D3DERR_INVALIDCALL;
  }
  state_.lightEnabled[index] = enable;
  state_.lights[index].enabled = enable;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setMaterial(const Material& material) {
  state_.material = material;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setTexture(u32 stage, std::shared_ptr<Texture> texture) {
  if (stage >= kMaxTextures) {
    return D3DERR_INVALIDCALL;
  }
  state_.textures[stage] = std::move(texture);
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setStreamSource(u32 stream, std::shared_ptr<Buffer> buffer, u32 offset, u32 stride) {
  if (stream >= kMaxStreams) {
    return D3DERR_INVALIDCALL;
  }
  state_.streamBuffers[stream] = std::move(buffer);
  state_.streamOffsets[stream] = offset;
  state_.streamStrides[stream] = stride;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setIndices(std::shared_ptr<Buffer> buffer, IndexType indexType) {
  state_.indexBuffer = std::move(buffer);
  state_.indexType = indexType;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setFVF(u32 fvf) {
  state_.fvf = fvf;
  state_.vertexDecl.fvf = fvf;
  state_.vertexDecl.elements.clear();
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setVertexDeclaration(std::vector<VertexElement> elements) {
  state_.vertexDecl.elements = std::move(elements);
  state_.vertexDecl.fvf = state_.fvf;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setVertexShader(const ShaderRef& shader) {
  state_.vertexShader = shader;
  if (state_.vertexShader.hash == 0) {
    state_.vertexShader.hash = hashShaderRef(state_.vertexShader);
  }
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setPixelShader(const ShaderRef& shader) {
  state_.pixelShader = shader;
  if (state_.pixelShader.hash == 0) {
    state_.pixelShader.hash = hashShaderRef(state_.pixelShader);
  }
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setClipPlane(u32 index, const ClipPlane& plane) {
  if (index >= kMaxClipPlanes) {
    return D3DERR_INVALIDCALL;
  }
  state_.clipPlanes[index] = plane;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setViewport(const Viewport& viewport) {
  if (viewport.width == 0 || viewport.height == 0 || !std::isfinite(viewport.minZ) ||
      !std::isfinite(viewport.maxZ) || viewport.minZ < 0.0f || viewport.maxZ > 1.0f ||
      viewport.minZ > viewport.maxZ) {
    return D3DERR_INVALIDCALL;
  }
  state_.viewport = viewport;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setScissorRect(const Rect& rect) {
  state_.scissorRect = rect;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setRenderTarget(u32 index, std::shared_ptr<Surface> surface) {
  if (index >= kMaxRenderTargets) {
    return D3DERR_INVALIDCALL;
  }
  state_.renderTargets[index] = surface ? RenderTargetAttachment{surface->handle(), surface->level(),
                                                                surface->multiSampleCount()}
                                        : RenderTargetAttachment{};
  if (index == 0 && surface) {
    const auto& desc = surface->desc();
    state_.viewport = {0, 0, std::max(1u, desc.width), std::max(1u, desc.height), 0.0f, 1.0f};
  }
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setDepthStencilSurface(std::shared_ptr<Surface> surface) {
  state_.depthStencil = surface ? RenderTargetAttachment{surface->handle(), surface->level(),
                                                         surface->multiSampleCount()}
                                : RenderTargetAttachment{};
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::beginScene() {
  if (state_.inScene) {
    return D3DERR_INVALIDCALL;
  }
  state_.inScene = true;
  inScene_ = true;
  return D3D_OK;
}

HResult Device::endScene() {
  if (!state_.inScene) {
    return D3DERR_INVALIDCALL;
  }
  state_.inScene = false;
  inScene_ = false;
  return D3D_OK;
}

ClearDesc Device::snapshotClearDesc(const ClearDesc& desc) const {
  ClearDesc snapshot = desc;
  if (snapshot.clearColor) {
    bool hasExplicitColor = false;
    for (const auto& attachment : snapshot.colorAttachments) {
      if (attachment.handle) {
        hasExplicitColor = true;
        break;
      }
    }
    if (!hasExplicitColor) {
      snapshot.colorAttachments = state_.renderTargets;
    }
  }
  if ((snapshot.clearDepth || snapshot.clearStencil) && !snapshot.depthStencil.handle) {
    snapshot.depthStencil = state_.depthStencil;
  }
  return snapshot;
}

SwapDesc Device::snapshotSwapDesc() const {
  SwapDesc desc;
  desc.window = presentParameters_.deviceWindow;
  desc.width = std::max(1u, presentParameters_.backBufferWidth);
  desc.height = std::max(1u, presentParameters_.backBufferHeight);
  desc.format = presentParameters_.backBufferFormat;
  desc.interval = presentParameters_.presentationInterval;
  desc.windowed = presentParameters_.windowed;
  desc.backBufferCount = std::max(1u, presentParameters_.backBufferCount);
  desc.displaySyncEnabled = presentParameters_.presentationInterval != PresentInterval::Immediate;
  desc.multiSampleType = presentParameters_.multiSampleType;
  if (!swapChains_.empty()) {
    if (auto backBuffer = swapChains_[0]->backBuffer()) {
      desc.sourceSurface = backBuffer->handle();
    }
    desc.presenter = swapChains_[0]->presenter();
  }
  return desc;
}

PrimitiveType canonicalPrimitiveType(PrimitiveType primitiveType) {
  return primitiveType == PrimitiveType::TriangleFan ? PrimitiveType::TriangleList : primitiveType;
}

VertexDeclSnapshot makeVertexDeclSnapshotFromState(const DeviceState& state) {
  VertexDeclSnapshot decl = state.vertexDecl;
  decl.streams.fill({});
  for (size_t i = 0; i < kMaxStreams; ++i) {
    decl.streams[i].buffer = state.streamBuffers[i];
    decl.streams[i].offset = state.streamOffsets[i];
    decl.streams[i].stride = state.streamStrides[i];
  }
  return decl;
}

ViewportScissor makeViewportScissorFromState(const DeviceState& state) {
  ViewportScissor viewport{};
  viewport.viewport = state.viewport;
  viewport.scissor = state.scissorRect;
  viewport.scissorEnabled =
      state.renderStates.contains(RS_SCISSOR_TEST_ENABLE) && state.renderStates.at(RS_SCISSOR_TEST_ENABLE) != 0;
  return viewport;
}

u32 clipPlaneMaskFromState(const DeviceState& state) {
  return state.renderStates.contains(RS_CLIP_PLANE_ENABLE)
             ? state.renderStates.at(RS_CLIP_PLANE_ENABLE)
             : 0u;
}

std::array<Matrix4x4, kMaxTextureStages> makeTextureTransformsFromState(const DeviceState& state) {
  std::array<Matrix4x4, kMaxTextureStages> transforms{};
  for (size_t i = 0; i < kMaxTextureStages; ++i) {
    transforms[i] = lookupTransform(state, XFORM_TEXTURE_BASE + static_cast<u32>(i));
  }
  return transforms;
}

std::array<ClipPlane, kMaxClipPlanes> makeClipPlanesFromState(const DeviceState& state,
                                                              u32 clipPlaneMask,
                                                              const Matrix4x4& worldViewProj) {
  std::array<ClipPlane, kMaxClipPlanes> clipPlanes{};
  for (size_t i = 0; i < kMaxClipPlanes; ++i) {
    if ((clipPlaneMask & (1u << i)) != 0) {
      clipPlanes[i] = transformClipPlane(worldViewProj, state.clipPlanes[i]);
    }
  }
  return clipPlanes;
}

Matrix4x4 makeWorldViewProjFromState(const DeviceState& state) {
  const Matrix4x4 world = lookupTransform(state, XFORM_WORLD_BASE);
  const Matrix4x4 view = lookupTransform(state, XFORM_VIEW);
  const Matrix4x4 proj = lookupTransform(state, XFORM_PROJECTION);
  return multiplyMatrix(multiplyMatrix(world, view), proj);
}

ShaderRef makeVertexShaderRefFromState(const DeviceState& state) {
  if (state.vertexShader.kind == ShaderRef::Kind::Bytecode) {
    return state.vertexShader;
  }
  ShaderRef shader{};
  shader.kind = ShaderRef::Kind::FixedFunctionVertex;
  shader.vertexKey = makeFfpVertexKey(state);
  shader.hash = shader.vertexKey->hash;
  return shader;
}

ShaderRef makePixelShaderRefFromState(const DeviceState& state) {
  if (state.pixelShader.kind == ShaderRef::Kind::Bytecode) {
    return state.pixelShader;
  }
  ShaderRef shader{};
  shader.kind = ShaderRef::Kind::FixedFunctionPixel;
  shader.pixelKey = makeFfpPixelKey(state);
  shader.hash = shader.pixelKey->hash;
  return shader;
}

DrawShaderLayoutContext makeDrawShaderLayoutContextFromState(const DeviceState& state) {
  DrawShaderLayoutContext context{};
  context.vertexDecl = makeVertexDeclSnapshotFromState(state);
  context.vertexShader = makeVertexShaderRefFromState(state);
  context.pixelShader = makePixelShaderRefFromState(state);
  context.vsConst = state.vsConst;
  context.psConst = state.psConst;
  context.worldViewProj = makeWorldViewProjFromState(state);
  context.textureTransforms = makeTextureTransformsFromState(state);
  context.clipPlaneMask = clipPlaneMaskFromState(state);
  context.clipPlanes = makeClipPlanesFromState(state, context.clipPlaneMask, context.worldViewProj);
  return context;
}

namespace fixture {

DrawDesc makeDrawDescFromState(const DeviceState& state, const DrawCallArgs& args) {
  DrawDesc desc;
  const auto shaderLayout = makeDrawShaderLayoutContextFromState(state);
  desc.primitiveType = canonicalPrimitiveType(args.primitiveType);
  desc.primitiveCount = args.primitiveCount;
  desc.startVertex = args.startVertex;
  desc.baseVertexIndex = args.baseVertexIndex;
  desc.startIndex = args.startIndex;
  desc.indexType = args.indexType;
  desc.indexBuffer = state.indexBuffer ? state.indexBuffer->handle() : Handle{};
  desc.vertexDecl = shaderLayout.vertexDecl;
  desc.rs.values = state.renderStates.toMap();
  for (size_t i = 0; i < kMaxTextures; ++i) {
    desc.textures[i].handle = state.textures[i] ? state.textures[i]->handle() : Handle{};
    if (i < kMaxTextureStages) {
      desc.textures[i].stageStates = state.textureStageStates[i].toMap();
    } else {
      desc.textures[i].stageStates.clear();
    }
  }
  for (size_t i = 0; i < kMaxSamplers; ++i) {
    desc.samplers[i].states = state.samplerStates[i].toMap();
  }
  desc.rts.color = state.renderTargets;
  desc.rts.depthStencil = state.depthStencil;
  desc.viewport = makeViewportScissorFromState(state);
  desc.clipPlaneMask = shaderLayout.clipPlaneMask;
  desc.worldViewProj = shaderLayout.worldViewProj;
  desc.textureTransforms = shaderLayout.textureTransforms;
  desc.clipPlanes = shaderLayout.clipPlanes;
  desc.vertexShader = shaderLayout.vertexShader;
  desc.pixelShader = shaderLayout.pixelShader;
  desc.vsConst = shaderLayout.vsConst;
  desc.psConst = shaderLayout.psConst;
  return desc;
}

FlatDrawStateKey makeFlatDrawStateKey(const DrawDesc& desc) {
  FlatDrawStateKey key{};

  for (size_t i = 0; i < kMaxStreams; ++i) {
    const auto& stream = desc.vertexDecl.streams[i];
    key.streamBuffers[i] = stream.buffer ? stream.buffer->handle() : Handle{};
    key.streamOffsets[i] = stream.offset;
    key.streamStrides[i] = stream.stride;
    if (key.streamBuffers[i]) {
      key.streamMask |= 1u << i;
    }
  }

  key.indexBuffer = desc.indexBuffer;
  key.vertexElementCount = static_cast<u32>(desc.vertexDecl.elements.size());
  key.fvf = desc.vertexDecl.fvf;
  key.vertexDeclHash = hashVertexDeclElements(desc.vertexDecl);
  key.vertexShaderKind = desc.vertexShader.kind;
  key.pixelShaderKind = desc.pixelShader.kind;
  key.vertexShaderHash = hashShaderRefSummary(desc.vertexShader);
  key.pixelShaderHash = hashShaderRefSummary(desc.pixelShader);
  key.vertexConstantsHash = hashTrivial(desc.vsConst);
  key.pixelConstantsHash = hashTrivial(desc.psConst);

  for (size_t i = 0; i < kMaxTextures; ++i) {
    key.textures[i] = desc.textures[i].handle;
    if (key.textures[i]) {
      key.textureMask |= 1u << i;
    }
    if (i < kMaxTextureStages) {
      key.textureStageStateHashes[i] = hashStateMap(desc.textures[i].stageStates);
    }
  }

  for (size_t i = 0; i < kMaxSamplers; ++i) {
    key.samplerStateHashes[i] = hashStateMap(desc.samplers[i].states);
    if (!desc.samplers[i].states.empty()) {
      key.samplerStateMask |= 1u << i;
    }
  }

  key.renderStateHash = hashStateMap(desc.rs.values);
  key.colorAttachments = desc.rts.color;
  key.depthStencil = desc.rts.depthStencil;
  for (size_t i = 0; i < kMaxRenderTargets; ++i) {
    if (key.colorAttachments[i].handle) {
      key.renderTargetMask |= 1u << i;
    }
  }

  key.viewportHash = hashViewportScissor(desc.viewport);
  key.worldViewProjHash = hashTrivial(desc.worldViewProj);
  key.textureTransformsHash = hashTextureTransforms(desc.textureTransforms);
  key.clipPlaneMask = desc.clipPlaneMask;
  key.clipPlanesHash = hashClipPlanes(desc.clipPlanes);

  return key;
}

FlatDrawStateRecord makeFlatDrawStateRecord(const DrawDesc& desc) {
  FlatDrawStateRecord record{};
  record.key = makeFlatDrawStateKey(desc);
  record.streamBuffers = record.key.streamBuffers;
  record.streamOffsets = record.key.streamOffsets;
  record.streamStrides = record.key.streamStrides;
  record.streamMask = record.key.streamMask;
  record.indexBuffer = record.key.indexBuffer;
  record.textures = record.key.textures;
  record.textureMask = record.key.textureMask;
  record.renderStates = makeFlatStateSet<kMaxStateSlots>(desc.rs.values);
  for (size_t i = 0; i < kMaxTextureStages; ++i) {
    record.textureStageStates[i] =
        makeFlatStateSet<kMaxTextureStageStates>(desc.textures[i].stageStates);
  }
  for (size_t i = 0; i < kMaxSamplers; ++i) {
    record.samplerStates[i] =
        makeFlatStateSet<kMaxSamplerStates>(desc.samplers[i].states);
  }
  record.colorAttachments = record.key.colorAttachments;
  record.depthStencil = record.key.depthStencil;
  record.renderTargetMask = record.key.renderTargetMask;
  record.viewport = desc.viewport;
  record.vertexConstantsHash = record.key.vertexConstantsHash;
  record.pixelConstantsHash = record.key.pixelConstantsHash;
  record.worldViewProjHash = record.key.worldViewProjHash;
  record.textureTransformsHash = record.key.textureTransformsHash;
  record.clipPlaneMask = record.key.clipPlaneMask;
  record.clipPlanesHash = record.key.clipPlanesHash;
  return record;
}

}  // namespace fixture

namespace {

FlatDrawStateKey makeFlatDrawStateKeyFromState(const DeviceState& state,
                                               const DrawShaderLayoutContext& shaderLayout,
                                               const ViewportScissor& viewport) {
  FlatDrawStateKey key{};

  for (size_t i = 0; i < kMaxStreams; ++i) {
    key.streamBuffers[i] = state.streamBuffers[i] ? state.streamBuffers[i]->handle() : Handle{};
    key.streamOffsets[i] = state.streamOffsets[i];
    key.streamStrides[i] = state.streamStrides[i];
    if (key.streamBuffers[i]) {
      key.streamMask |= 1u << i;
    }
  }

  key.indexBuffer = state.indexBuffer ? state.indexBuffer->handle() : Handle{};
  key.vertexElementCount = static_cast<u32>(shaderLayout.vertexDecl.elements.size());
  key.fvf = shaderLayout.vertexDecl.fvf;
  key.vertexDeclHash = hashVertexDeclElements(shaderLayout.vertexDecl);
  key.vertexShaderKind = shaderLayout.vertexShader.kind;
  key.pixelShaderKind = shaderLayout.pixelShader.kind;
  key.vertexShaderHash = hashShaderRefSummary(shaderLayout.vertexShader);
  key.pixelShaderHash = hashShaderRefSummary(shaderLayout.pixelShader);
  key.vertexConstantsHash = hashTrivial(state.vsConst);
  key.pixelConstantsHash = hashTrivial(state.psConst);

  for (size_t i = 0; i < kMaxTextures; ++i) {
    key.textures[i] = state.textures[i] ? state.textures[i]->handle() : Handle{};
    if (key.textures[i]) {
      key.textureMask |= 1u << i;
    }
    if (i < kMaxTextureStages) {
      key.textureStageStateHashes[i] = hashStateMap(state.textureStageStates[i]);
    }
  }

  for (size_t i = 0; i < kMaxSamplers; ++i) {
    key.samplerStateHashes[i] = hashStateMap(state.samplerStates[i]);
    if (!state.samplerStates[i].empty()) {
      key.samplerStateMask |= 1u << i;
    }
  }

  key.renderStateHash = hashStateMap(state.renderStates);
  key.colorAttachments = state.renderTargets;
  key.depthStencil = state.depthStencil;
  for (size_t i = 0; i < kMaxRenderTargets; ++i) {
    if (key.colorAttachments[i].handle) {
      key.renderTargetMask |= 1u << i;
    }
  }

  key.viewportHash = hashViewportScissor(viewport);
  key.worldViewProjHash = hashTrivial(shaderLayout.worldViewProj);
  key.textureTransformsHash = hashTextureTransforms(shaderLayout.textureTransforms);
  key.clipPlaneMask = shaderLayout.clipPlaneMask;
  key.clipPlanesHash = hashClipPlanes(shaderLayout.clipPlanes);
  return key;
}

FlatDrawStateRecord makeFlatDrawStateRecordFromState(const DeviceState& state,
                                                     const DrawShaderLayoutContext& shaderLayout,
                                                     const ViewportScissor& viewport) {
  FlatDrawStateRecord record{};
  record.key = makeFlatDrawStateKeyFromState(state, shaderLayout, viewport);
  record.streamBuffers = record.key.streamBuffers;
  record.streamOffsets = record.key.streamOffsets;
  record.streamStrides = record.key.streamStrides;
  record.streamMask = record.key.streamMask;
  record.indexBuffer = record.key.indexBuffer;
  record.textures = record.key.textures;
  record.textureMask = record.key.textureMask;
  record.renderStates = makeFlatStateSet<kMaxStateSlots>(state.renderStates);
  for (size_t i = 0; i < kMaxTextureStages; ++i) {
    record.textureStageStates[i] =
        makeFlatStateSet<kMaxTextureStageStates>(state.textureStageStates[i]);
  }
  for (size_t i = 0; i < kMaxSamplers; ++i) {
    record.samplerStates[i] =
        makeFlatStateSet<kMaxSamplerStates>(state.samplerStates[i]);
  }
  record.colorAttachments = record.key.colorAttachments;
  record.depthStencil = record.key.depthStencil;
  record.renderTargetMask = record.key.renderTargetMask;
  record.viewport = viewport;
  record.vertexConstantsHash = record.key.vertexConstantsHash;
  record.pixelConstantsHash = record.key.pixelConstantsHash;
  record.worldViewProjHash = record.key.worldViewProjHash;
  record.textureTransformsHash = record.key.textureTransformsHash;
  record.clipPlaneMask = record.key.clipPlaneMask;
  record.clipPlanesHash = record.key.clipPlanesHash;
  return record;
}

}  // namespace

namespace fixture {

DrawShaderLayoutContext makeDrawShaderLayoutContext(const DrawDesc& desc) {
  DrawShaderLayoutContext context{};
  context.vertexDecl = desc.vertexDecl;
  context.vertexShader = desc.vertexShader;
  context.pixelShader = desc.pixelShader;
  context.vsConst = desc.vsConst;
  context.psConst = desc.psConst;
  context.worldViewProj = desc.worldViewProj;
  context.textureTransforms = desc.textureTransforms;
  context.clipPlaneMask = desc.clipPlaneMask;
  context.clipPlanes = desc.clipPlanes;
  return context;
}

DrawDebugSnapshot makeDrawDebugSnapshot(const DrawDesc& desc, const FlatDrawStateRecord& hot) {
  DrawDebugSnapshot snapshot{};
  snapshot.primitiveType = desc.primitiveType;
  snapshot.primitiveCount = desc.primitiveCount;
  snapshot.startVertex = desc.startVertex;
  snapshot.baseVertexIndex = desc.baseVertexIndex;
  snapshot.startIndex = desc.startIndex;
  snapshot.indexType = desc.indexType;
  snapshot.userVertexBytes = static_cast<u32>(
      std::min<std::size_t>(desc.userVertexData.size(), std::numeric_limits<u32>::max()));
  snapshot.userIndexBytes = static_cast<u32>(
      std::min<std::size_t>(desc.userIndexData.size(), std::numeric_limits<u32>::max()));
  snapshot.streamMask = hot.streamMask;
  snapshot.textureMask = hot.textureMask;
  snapshot.samplerStateMask = hot.key.samplerStateMask;
  snapshot.renderTargetMask = hot.renderTargetMask;
  snapshot.renderStateHash = hot.key.renderStateHash;
  snapshot.vertexDeclHash = hot.key.vertexDeclHash;
  snapshot.vertexShaderHash = hot.key.vertexShaderHash;
  snapshot.pixelShaderHash = hot.key.pixelShaderHash;
  return snapshot;
}

}  // namespace fixture

DrawDebugSnapshot makeDrawDebugSnapshot(const DrawCallArgs& args, const FlatDrawStateRecord& hot) {
  DrawDebugSnapshot snapshot{};
  snapshot.primitiveType = canonicalPrimitiveType(args.primitiveType);
  snapshot.primitiveCount = args.primitiveCount;
  snapshot.startVertex = args.startVertex;
  snapshot.baseVertexIndex = args.baseVertexIndex;
  snapshot.startIndex = args.startIndex;
  snapshot.indexType = args.indexType;
  snapshot.streamMask = hot.streamMask;
  snapshot.textureMask = hot.textureMask;
  snapshot.samplerStateMask = hot.key.samplerStateMask;
  snapshot.renderTargetMask = hot.renderTargetMask;
  snapshot.renderStateHash = hot.key.renderStateHash;
  snapshot.vertexDeclHash = hot.key.vertexDeclHash;
  snapshot.vertexShaderHash = hot.key.vertexShaderHash;
  snapshot.pixelShaderHash = hot.key.pixelShaderHash;
  return snapshot;
}

CanonicalDrawState makeCanonicalDrawStateFromState(const DeviceState& state, const DrawCallArgs& args) {
  auto shaderLayout = makeDrawShaderLayoutContextFromState(state);
  const auto viewport = makeViewportScissorFromState(state);
  auto hot = makeFlatDrawStateRecordFromState(state, shaderLayout, viewport);
  auto debug = makeDrawDebugSnapshot(args, hot);
  return CanonicalDrawState{std::move(hot), std::move(shaderLayout), std::move(debug)};
}

namespace {

std::span<const DrawParam> drawParamStorageSpan(const DrawParamInlineStorage& storage) noexcept {
  if (storage.overflowMode) {
    return std::span<const DrawParam>(storage.overflow.data(), storage.overflow.size());
  }
  return std::span<const DrawParam>(storage.inlineData.data(), storage.inlineSize);
}

std::span<const u8> drawPayloadStorageSpan(const DrawPayloadArenaStorage& storage) noexcept {
  if (storage.overflowMode) {
    return std::span<const u8>(storage.overflow.data(), storage.overflow.size());
  }
  return std::span<const u8>(storage.inlineData.data(), storage.inlineSize);
}

std::size_t drawParamStorageSize(const DrawParamInlineStorage& storage) noexcept {
  return storage.overflowMode ? storage.overflow.size() : storage.inlineSize;
}

std::size_t drawPayloadStorageSize(const DrawPayloadArenaStorage& storage) noexcept {
  return storage.overflowMode ? storage.overflow.size() : storage.inlineSize;
}

void reserveDrawParams(DrawParamInlineStorage& storage, std::size_t count) {
  if (count <= kDrawRunInlineParamCapacity || storage.overflowMode) {
    if (storage.overflowMode) storage.overflow.reserve(count);
    return;
  }
  storage.overflow.reserve(count);
  storage.overflow.insert(storage.overflow.end(), storage.inlineData.begin(),
                          storage.inlineData.begin() + static_cast<std::ptrdiff_t>(storage.inlineSize));
  storage.inlineSize = 0;
  storage.overflowMode = true;
}

void reserveDrawPayload(DrawPayloadArenaStorage& storage, std::size_t bytes) {
  if (bytes <= kDrawRunInlinePayloadCapacity || storage.overflowMode) {
    if (storage.overflowMode) storage.overflow.reserve(bytes);
    return;
  }
  storage.overflow.reserve(bytes);
  storage.overflow.insert(storage.overflow.end(), storage.inlineData.begin(),
                          storage.inlineData.begin() + static_cast<std::ptrdiff_t>(storage.inlineSize));
  storage.inlineSize = 0;
  storage.overflowMode = true;
}

void appendDrawParam(DrawParamInlineStorage& storage, DrawParam param) {
  if (storage.overflowMode) {
    storage.overflow.push_back(std::move(param));
    return;
  }
  if (storage.inlineSize < kDrawRunInlineParamCapacity) {
    storage.inlineData[storage.inlineSize++] = std::move(param);
    return;
  }
  reserveDrawParams(storage, kDrawRunInlineParamCapacity + 1);
  storage.overflow.push_back(std::move(param));
}

bool appendDrawPayload(DrawPayloadArenaStorage& storage, std::span<const u8> bytes,
                       DrawPayloadRange& range) {
  range = {};
  if (bytes.empty()) {
    return true;
  }

  constexpr auto kMaxRange = std::numeric_limits<u32>::max();
  const auto currentSize = drawPayloadStorageSize(storage);
  const std::uint64_t requiredSize =
      static_cast<std::uint64_t>(currentSize) + static_cast<std::uint64_t>(bytes.size());
  if (requiredSize > kMaxRange) {
    return false;
  }

  range = DrawPayloadRange{
      .offset = static_cast<u32>(currentSize),
      .size = static_cast<u32>(bytes.size()),
  };

  if (storage.overflowMode) {
    storage.overflow.insert(storage.overflow.end(), bytes.begin(), bytes.end());
    return true;
  }
  if (currentSize + bytes.size() <= kDrawRunInlinePayloadCapacity) {
    std::copy(bytes.begin(), bytes.end(), storage.inlineData.begin() +
                                           static_cast<std::ptrdiff_t>(storage.inlineSize));
    storage.inlineSize += bytes.size();
    return true;
  }
  reserveDrawPayload(storage, static_cast<std::size_t>(requiredSize));
  storage.overflow.insert(storage.overflow.end(), bytes.begin(), bytes.end());
  return true;
}

bool packDrawParamPayload(DrawParam& param, DrawPayloadArenaStorage& payloadArena,
                          DrawParamPayloadView payload) {
  const auto vertexBytes = payload.userVertexData;
  const auto indexBytes = payload.userIndexData;
  constexpr auto kMaxRange = std::numeric_limits<u32>::max();
  const std::uint64_t requiredSize =
      static_cast<std::uint64_t>(drawPayloadStorageSize(payloadArena)) +
      static_cast<std::uint64_t>(vertexBytes.size()) +
      static_cast<std::uint64_t>(indexBytes.size());
  if (requiredSize > kMaxRange) {
    return false;
  }

  return appendDrawPayload(payloadArena, vertexBytes, param.userVertexRange) &&
         appendDrawPayload(payloadArena, indexBytes, param.userIndexRange);
}

bool drawPayloadRangeValid(std::size_t payloadSize, DrawPayloadRange range) noexcept {
  return static_cast<std::uint64_t>(range.offset) + static_cast<std::uint64_t>(range.size) <=
         static_cast<std::uint64_t>(payloadSize);
}

}  // namespace

void drawRunClear(DrawRunDesc& run) {
  if (run.scratch_.draws.overflowMode) {
    run.scratch_.draws.overflow.clear();
  } else {
    run.scratch_.draws.inlineSize = 0;
  }
  if (run.scratch_.payload.overflowMode) {
    run.scratch_.payload.overflow.clear();
  } else {
    run.scratch_.payload.inlineSize = 0;
  }
}

void drawRunReserve(DrawRunDesc& run, std::size_t drawCount, std::size_t payloadBytes) {
  reserveDrawParams(run.scratch_.draws, drawCount);
  reserveDrawPayload(run.scratch_.payload, payloadBytes);
}

bool drawRunAppend(DrawRunDesc& run, DrawParam param, DrawParamPayloadView payload) {
  param.userVertexRange = {};
  param.userIndexRange = {};
  if (!packDrawParamPayload(param, run.scratch_.payload, payload)) {
    return false;
  }
  appendDrawParam(run.scratch_.draws, std::move(param));
  return true;
}

DrawRunView drawRunView(const DrawRunDesc& run) noexcept {
  return DrawRunView{
      .draws = drawParamStorageSpan(run.scratch_.draws),
      .payloadArena = drawPayloadStorageSpan(run.scratch_.payload),
  };
}

bool drawRunEmpty(const DrawRunDesc& run) noexcept {
  return drawParamStorageSize(run.scratch_.draws) == 0;
}

std::size_t drawRunDrawCount(const DrawRunDesc& run) noexcept {
  return drawParamStorageSize(run.scratch_.draws);
}

std::size_t drawRunPayloadSize(const DrawRunDesc& run) noexcept {
  return drawPayloadStorageSize(run.scratch_.payload);
}

std::span<const DrawParam> drawRunDraws(const DrawRunDesc& run) noexcept {
  return drawParamStorageSpan(run.scratch_.draws);
}

std::span<const u8> drawRunPayloadArena(const DrawRunDesc& run) noexcept {
  return drawPayloadStorageSpan(run.scratch_.payload);
}

std::span<const u8> drawRunPayloadBytes(const DrawRunDesc& run,
                                        DrawPayloadRange range) noexcept {
  return drawRunPayloadBytes(range, drawRunPayloadArena(run));
}

bool drawRunValidate(const DrawRunDesc& run) noexcept {
  const auto payloadSize = drawRunPayloadSize(run);
  for (const auto& param : drawRunDraws(run)) {
    if (!drawPayloadRangeValid(payloadSize, param.userVertexRange) ||
        !drawPayloadRangeValid(payloadSize, param.userIndexRange)) {
      return false;
    }
  }
  return true;
}

DrawParamPayloadView drawPayloadAt(std::span<const DrawParamPayloadView> payloads,
                                   std::size_t index) {
  if (index < payloads.size()) {
    return payloads[index];
  }
  return {};
}

bool drawRunUsesBoundIndexBuffer(std::span<const DrawParam> draws,
                                 std::span<const DrawParamPayloadView> payloads) {
  for (std::size_t i = 0; i < draws.size(); ++i) {
    const auto& draw = draws[i];
    if (draw.indexed && drawPayloadAt(payloads, i).userIndexData.empty()) {
      return true;
    }
  }
  return false;
}

DrawRunDesc makeDrawRunDescFromCanonical(CanonicalDrawState state,
                                         std::span<const DrawParam> draws,
                                         std::span<const DrawParamPayloadView> payloads) {
  DrawRunDesc run{};
  if (draws.empty()) {
    return run;
  }

  const auto& first = draws.front();
  const auto firstPayload = drawPayloadAt(payloads, 0);
  run.state = std::move(state);
  run.state.debug = makeDrawDebugSnapshot(
      DrawCallArgs{first.primitiveType, first.primitiveCount, first.startVertex,
                   first.baseVertexIndex, first.startIndex, first.indexType},
      run.state.hot);
  run.state.debug.userVertexBytes = static_cast<u32>(
      std::min<std::size_t>(firstPayload.userVertexData.size(), std::numeric_limits<u32>::max()));
  run.state.debug.userIndexBytes = static_cast<u32>(
      std::min<std::size_t>(firstPayload.userIndexData.size(), std::numeric_limits<u32>::max()));
  std::size_t payloadBytes = 0;
  for (std::size_t i = 0; i < draws.size(); ++i) {
    const auto payload = drawPayloadAt(payloads, i);
    payloadBytes += payload.userVertexData.size() + payload.userIndexData.size();
  }
  drawRunReserve(run, draws.size(), payloadBytes);

  for (std::size_t i = 0; i < draws.size(); ++i) {
    if (!drawRunAppend(run, draws[i], drawPayloadAt(payloads, i))) {
      return {};
    }
  }
  return run;
}

DrawRunDesc makeDrawRunDescFromState(DeviceState baseState,
                                     std::span<const DrawParam> draws,
                                     std::span<const DrawParamPayloadView> payloads = {}) {
  if (draws.empty()) {
    return {};
  }
  if (!drawRunUsesBoundIndexBuffer(draws, payloads)) {
    baseState.indexBuffer.reset();
  }
  auto state = makeCanonicalDrawStateFromState(
      baseState, DrawCallArgs{draws.front().primitiveType, draws.front().primitiveCount,
                              draws.front().startVertex, draws.front().baseVertexIndex,
                              draws.front().startIndex, draws.front().indexType});
  return makeDrawRunDescFromCanonical(std::move(state), draws, payloads);
}

void Device::invalidateDrawStateCache() noexcept {
  ++drawStateGeneration_;
  if (drawStateGeneration_ == 0) {
    drawStateGeneration_ = 1;
  }
}

const Device::CachedBaseDrawState& Device::cachedBaseDrawState(bool includeIndexBuffer) {
  auto& cache = includeIndexBuffer ? drawStateCacheWithIndex_ : drawStateCacheNoIndex_;
  if (cache.valid && cache.generation == drawStateGeneration_) {
    return cache;
  }

  DeviceState baseState = state_;
  if (!includeIndexBuffer) {
    baseState.indexBuffer.reset();
  }
  cache.shaderLayout = makeDrawShaderLayoutContextFromState(baseState);
  const auto viewport = makeViewportScissorFromState(baseState);
  cache.hot = makeFlatDrawStateRecordFromState(baseState, cache.shaderLayout, viewport);
  cache.generation = drawStateGeneration_;
  cache.valid = true;
  return cache;
}

DrawRunDesc Device::makeDrawRunDescFromCurrentState(
    std::span<const DrawParam> draws,
    std::span<const DrawParamPayloadView> payloads) {
  if (draws.empty()) {
    return {};
  }
  const auto& cached = cachedBaseDrawState(drawRunUsesBoundIndexBuffer(draws, payloads));
  CanonicalDrawState state{
      cached.hot,
      cached.shaderLayout,
      makeDrawDebugSnapshot(
          DrawCallArgs{draws.front().primitiveType, draws.front().primitiveCount,
                       draws.front().startVertex, draws.front().baseVertexIndex,
                       draws.front().startIndex, draws.front().indexType},
          cached.hot),
  };
  return makeDrawRunDescFromCanonical(std::move(state), draws, payloads);
}

HResult Device::clear(const ClearDesc& desc) {
  auto snapshot = snapshotClearDesc(desc);
  if (snapshot.clearColor) {
    for (const auto& attachment : snapshot.colorAttachments) {
      if (!attachment.handle) {
        continue;
      }
      for (auto& surface : surfaces_) {
        if (auto sp = surface.lock(); sp && sp->handle() == attachment.handle && sp->valid()) {
          if (canTrustGpuReadback(backend_) && backendOwnsSurfaceContents(sp->desc())) {
            continue;
          }
          if (snapshot.rects.empty()) {
            sp->fillColor(nullptr, snapshot.color);
          } else {
            for (const auto& rect : snapshot.rects) {
              sp->fillColor(&rect, snapshot.color);
            }
          }
        }
      }
      for (auto& texture : textures_) {
        if (auto tp = texture.lock(); tp && tp->handle() == attachment.handle && tp->valid()) {
          if (canTrustGpuReadback(backend_) && backendOwnsTextureContents(tp->desc())) {
            continue;
          }
          if (snapshot.rects.empty()) {
            tp->fillColor(nullptr, snapshot.color);
          } else {
            for (const auto& rect : snapshot.rects) {
              tp->fillColor(&rect, snapshot.color);
            }
          }
        }
      }
    }
  }

  if (snapshot.clearDepth || snapshot.clearStencil) {
    const auto applyDepthClear = [&](const std::shared_ptr<Surface>& surface) {
      if (!surface || !surface->valid()) {
        return;
      }
      const auto& surfaceDesc = surface->desc();
      if (canTrustGpuReadback(backend_) && backendOwnsSurfaceContents(surfaceDesc)) {
        return;
      }
      if (!surfaceDesc.depthStencil) {
        return;
      }
      if (snapshot.rects.empty()) {
        auto region = surface->lockRect(nullptr, 0);
        if (region.data) {
          std::vector<u8> scratch(static_cast<size_t>(region.pitch) * surfaceDesc.height, 0);
          fillDepthStencil(scratch, region.pitch, surfaceDesc.width, surfaceDesc.height, surfaceDesc.format,
                           nullptr, snapshot.clearDepth, snapshot.depth, snapshot.clearStencil,
                           snapshot.stencil);
          std::memcpy(region.data, scratch.data(), scratch.size());
        }
        surface->unlockRect();
      } else {
        for (const auto& rect : snapshot.rects) {
          auto region = surface->lockRect(&rect, 0);
          if (!region.data) {
            continue;
          }
          auto* bytes = static_cast<u8*>(region.data);
          const u32 rectWidth = static_cast<u32>(std::max(0, rect.right - rect.left));
          const u32 rectHeight = static_cast<u32>(std::max(0, rect.bottom - rect.top));
          std::vector<u8> scratch(static_cast<size_t>(region.pitch) * rectHeight, 0);
          // Fill a temporary region, then copy it into the locked surface area.
          fillDepthStencil(scratch, region.pitch, rectWidth, rectHeight, surfaceDesc.format, nullptr,
                           snapshot.clearDepth, snapshot.depth, snapshot.clearStencil, snapshot.stencil);
          for (u32 y = 0; y < rectHeight; ++y) {
            std::memcpy(bytes + static_cast<size_t>(y) * region.pitch,
                        scratch.data() + static_cast<size_t>(y) * region.pitch,
                        static_cast<size_t>(rectWidth) * bytesPerPixel(surfaceDesc.format));
          }
          surface->unlockRect();
        }
      }
    };

    if (snapshot.depthStencil.handle) {
      for (auto& surface : surfaces_) {
        if (auto sp = surface.lock(); sp && sp->handle() == snapshot.depthStencil.handle) {
          applyDepthClear(sp);
        }
      }
    }
  }
  submitClearInternal(snapshot);
  return D3D_OK;
}

HResult Device::drawPrimitiveRun(std::span<const DrawParam> draws) {
  if (draws.empty()) {
    return D3D_OK;
  }
  submitDrawRunInternal(makeDrawRunDescFromCurrentState(draws));
  return D3D_OK;
}

HResult Device::drawPrimitive(PrimitiveType type, u32 primitiveCount, u32 startVertex) {
  DrawParam draw{};
  draw.primitiveType = canonicalPrimitiveType(type);
  draw.primitiveCount = primitiveCount;
  draw.startVertex = startVertex;
  draw.indexType = state_.indexType;
  draw.indexed = false;
  submitDrawRunInternal(makeDrawRunDescFromCurrentState(std::span<const DrawParam>(&draw, 1)));
  if (state_.inScene) {
    // No-op; draw submission is immediate in the core harness.
  }
  return D3D_OK;
}

HResult Device::drawIndexedPrimitive(PrimitiveType type, u32 primitiveCount, u32 startVertex,
                                     i32 baseVertexIndex, u32 startIndex, IndexType indexType) {
  DrawParam draw{};
  draw.primitiveType = canonicalPrimitiveType(type);
  draw.primitiveCount = primitiveCount;
  draw.startVertex = startVertex;
  draw.baseVertexIndex = baseVertexIndex;
  draw.startIndex = startIndex;
  draw.indexType = indexType;
  draw.indexed = true;
  submitDrawRunInternal(makeDrawRunDescFromCurrentState(std::span<const DrawParam>(&draw, 1)));
  return D3D_OK;
}

HResult Device::drawPrimitiveUP(PrimitiveType type, u32 primitiveCount,
                                std::span<const u8> vertexData, u32 vertexStride) {
  upVertexScratch_.assign(vertexData.begin(), vertexData.end());
  DeviceState drawState = state_;
  // UP draws source stream 0 from caller memory, not the currently bound VB.
  drawState.streamBuffers[0].reset();
  drawState.streamOffsets[0] = 0;
  if (vertexStride != 0) {
    drawState.streamStrides[0] = vertexStride;
  }
  DrawParam draw{};
  draw.primitiveType = canonicalPrimitiveType(type);
  draw.primitiveCount = primitiveCount;
  draw.indexType = state_.indexType;
  draw.indexed = false;
  if (type == PrimitiveType::TriangleFan) {
    const u32 stride = vertexStride != 0 ? vertexStride : inferStreamZeroStride(makeVertexDeclSnapshotFromState(drawState));
    auto decomposed = decomposeTriangleFanVertices(vertexData, primitiveCount, stride);
    if (primitiveCount != 0 && decomposed.empty()) {
      return D3DERR_INVALIDCALL;
    }
    const DrawParamPayloadView payload{
        .userVertexData = std::span<const u8>(decomposed.data(), decomposed.size()),
    };
    submitDrawRunInternal(makeDrawRunDescFromState(
        drawState, std::span<const DrawParam>(&draw, 1),
        std::span<const DrawParamPayloadView>(&payload, 1)));
    return D3D_OK;
  } else {
    const DrawParamPayloadView payload{
        .userVertexData = std::span<const u8>(upVertexScratch_.data(), upVertexScratch_.size()),
    };
    submitDrawRunInternal(makeDrawRunDescFromState(
        drawState, std::span<const DrawParam>(&draw, 1),
        std::span<const DrawParamPayloadView>(&payload, 1)));
    return D3D_OK;
  }
}

HResult Device::drawIndexedPrimitiveUP(PrimitiveType type, u32 primitiveCount,
                                       std::span<const u8> vertexData, std::span<const u8> indexData,
                                       IndexType indexType, u32 vertexStride) {
  upVertexScratch_.assign(vertexData.begin(), vertexData.end());
  upIndexScratch_.assign(indexData.begin(), indexData.end());
  if (type == PrimitiveType::TriangleFan) {
    std::vector<u32> indices;
    indices.reserve(indexData.size() / sizeof(u32));
    for (size_t i = 0; i + sizeof(u32) <= indexData.size(); i += sizeof(u32)) {
      u32 value = 0;
      std::memcpy(&value, indexData.data() + i, sizeof(u32));
      indices.push_back(value);
    }
    const auto fan = decomposeTriangleFanIndices(indices);
    upIndexScratch_.resize(fan.size() * sizeof(u32));
    std::memcpy(upIndexScratch_.data(), fan.data(), upIndexScratch_.size());
    type = PrimitiveType::TriangleList;
  }
  DeviceState drawState = state_;
  // Indexed UP draws also source stream 0 from caller memory.
  drawState.streamBuffers[0].reset();
  drawState.streamOffsets[0] = 0;
  if (vertexStride != 0) {
    drawState.streamStrides[0] = vertexStride;
  }
  DrawParam draw{};
  draw.primitiveType = canonicalPrimitiveType(type);
  draw.primitiveCount = primitiveCount;
  draw.indexType = indexType;
  draw.indexed = true;
  const DrawParamPayloadView payload{
      .userVertexData = std::span<const u8>(upVertexScratch_.data(), upVertexScratch_.size()),
      .userIndexData = std::span<const u8>(upIndexScratch_.data(), upIndexScratch_.size()),
  };
  submitDrawRunInternal(makeDrawRunDescFromState(
      drawState, std::span<const DrawParam>(&draw, 1),
      std::span<const DrawParamPayloadView>(&payload, 1)));
  return D3D_OK;
}

HResult Device::present() {
  return presentEx();
}

HResult Device::presentEx(const Rect* sourceRect, const Rect* destRect, Handle destinationWindowOverride,
                          const void* dirtyRegion, u32 flags) {
  (void)sourceRect;
  (void)destRect;
  (void)destinationWindowOverride;
  (void)dirtyRegion;
  (void)flags;
  if (deviceLost_) {
    return D3DERR_DEVICELOST;
  }
  auto desc = snapshotSwapDesc();
  const bool synchronizePresent = desc.displaySyncEnabled;
  submitPresentInternal(desc);
  // Immediate presents must not synchronously wait for the Metal presenter:
  // some windowed apps submit before their message pump has made a drawable
  // available, and waiting here can deadlock that first frame.
  if (backend_ && synchronizePresent) {
    upperDevice_->flush();
  }
  completeUpTo(submittedSequenceId_);
  ++presentCount_;
  maybeCaptureExperimentFrame();
  // SeqIdSafety: a completed present must not outrun the submitted sequence.
  DXMT_ASSERT(completedSequenceId_ == submittedSequenceId_);
  return D3D_OK;
}

HResult Device::reset(const PresentParameters& params) {
  if (const auto hr = validatePresentParameters(params, false); hr != D3D_OK) {
    return hr;
  }
  return resetValidated(params);
}

HResult Device::resetEx(const PresentParameters& params, const DisplayModeEx* fullscreenMode) {
  if (const auto hr = validatePresentParameters(params, true); hr != D3D_OK) {
    return hr;
  }
  if (const auto hr = validateFullscreenModeRelation(params, fullscreenMode); hr != D3D_OK) {
    return hr;
  }
  return resetValidated(applyFullscreenMode(params, fullscreenMode));
}

HResult Device::resetValidated(const PresentParameters& params) {
  presentParameters_ = normalizePresentParameters(adapter_, params);
  deviceLost_ = false;
  presentOccluded_ = false;
  if (backend_) {
    upperDevice_->flush();
  }
  completeUpTo(submittedSequenceId_);
  // Drain-before-teardown: Reset waits for queued work to drain before
  // invalidating default-pool resources.
  DXMT_ASSERT(completedSequenceId_ == submittedSequenceId_);
  invalidateDefaultPoolResources();
  state_.reset();
  state_.viewport = {0, 0, std::max(1u, presentParameters_.backBufferWidth),
                     std::max(1u, presentParameters_.backBufferHeight), 0.0f,
                     1.0f};
  for (auto& chain : swapChains_) {
    if (chain) {
      chain->resize(presentParameters_);
    }
  }
  if (!swapChains_.empty() && swapChains_.front()) {
    const auto primary = swapChains_.front();
    state_.renderTargets[0] = primary->backBuffer()
                                  ? RenderTargetAttachment{primary->backBuffer()->handle(), 0,
                                                           primary->backBuffer()->multiSampleCount()}
                                  : RenderTargetAttachment{};
    state_.depthStencil = primary->depthStencilSurface()
                              ? RenderTargetAttachment{primary->depthStencilSurface()->handle(), 0,
                                                       primary->depthStencilSurface()->multiSampleCount()}
                              : RenderTargetAttachment{};
  }
  invalidateDrawStateCache();
  submittedSequenceId_ = 0;
  completedSequenceId_ = 0;
  presentCount_ = 0;
  experimentCapture_.captured = false;
  if (backend_) {
    backend_->setMaxFrameLatency(maximumFrameLatency_);
  }
  return D3D_OK;
}

HResult Device::setMaximumFrameLatency(u32 latency) {
  if (latency > kMaxFrameLatency) {
    return D3DERR_INVALIDCALL;
  }
  maximumFrameLatency_ = latency == 0 ? kDefaultFrameLatency : latency;
  if (backend_) {
    backend_->setMaxFrameLatency(maximumFrameLatency_);
  }
  return D3D_OK;
}

HResult Device::waitForVBlank(size_t swapChainIndex) {
  auto chain = swapChain(swapChainIndex);
  if (!chain) {
    return D3DERR_INVALIDCALL;
  }
  if (backend_) {
    auto vblankDesc = makeSwapDesc(chain->params());
    vblankDesc.presenter = chain->presenter();
    return upperDevice_->waitForVBlank(vblankDesc);
  }
  return D3D_OK;
}

HResult Device::checkResourceResidency(std::span<void* const> resources) const {
  (void)resources;
  return S_OK;
}

DisplayModeEx Device::getDisplayModeEx(size_t swapChainIndex) const {
  DisplayModeEx mode;
  const auto chain = swapChain(swapChainIndex);
  const auto& params = chain ? chain->params() : presentParameters_;
  mode.width = std::max(1u, params.backBufferWidth);
  mode.height = std::max(1u, params.backBufferHeight);
  mode.refreshRate = 60;
  mode.format = params.backBufferFormat;
  mode.scanLineOrdering = DisplayScanLineOrdering::Progressive;
  return mode;
}

HResult Device::getGPUThreadPriority(i32* priority) const {
  if (priority) {
    *priority = 0;
  }
  return D3D_OK;
}

HResult Device::setGPUThreadPriority(i32 priority) {
  (void)priority;
  return D3D_OK;
}

HResult Device::setConvolutionMonoKernel() {
  return E_NOTIMPL;
}

HResult Device::composeRects() {
  return E_NOTIMPL;
}

HResult Device::checkDeviceMultiSampleType(Format format, MultiSampleType type) const {
  if (type == MultiSampleType::None) {
    return D3D_OK;
  }

  const auto supportsCount = [this](u32 count) {
    switch (count) {
      case 2:
        return limits_.supportsSampleCount2;
      case 4:
        return limits_.supportsSampleCount4;
      case 8:
        return limits_.supportsSampleCount8;
      default:
        return false;
    }
  };

  const u32 count = dxmt9::core::sampleCount(type);
  if (!supportsCount(count)) {
    return D3DERR_NOTAVAILABLE;
  }
  if (!formatSupportsUsage(format, UsageRenderTarget, limits_) &&
      !formatSupportsUsage(format, UsageDepthStencil, limits_)) {
    return D3DERR_NOTAVAILABLE;
  }
  return D3D_OK;
}

HResult Device::issueQuery(const std::shared_ptr<Query>& query, bool begin) {
  if (!query) {
    return D3DERR_INVALIDCALL;
  }
  // TLA+: QuerySeqId / IssueQuery
  // The command stream assigns the next seqId, and D3DISSUE_END records that
  // seqId as qIssuedSeqId for the query fence.
  ++submittedSequenceId_;
  // TLA+: QuerySeqId / SeqIdMonotone
  // Queries advance the submission sequence but never allow the completed
  // sequence to move ahead of it.
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
  if (begin) {
    query->begin(submittedSequenceId_);
    if (query->type() == QueryType::Occlusion) {
      activeOcclusionQuery_ = query;
      activeOcclusionCount_ = 0;
    }
  } else {
    query->end(submittedSequenceId_);
    if (query->type() == QueryType::Occlusion) {
      query->resolve(activeOcclusionCount_);
      activeOcclusionQuery_.reset();
    } else if (query->type() == QueryType::Timestamp) {
      query->resolve(submittedSequenceId_);
    }
  }
  return D3D_OK;
}

HResult Device::getQueryData(const std::shared_ptr<Query>& query, void* output, size_t size,
                             u32 flags) const {
  if (!query) {
    return D3DERR_INVALIDCALL;
  }
  // TLA+: QuerySeqId / SeqIdMonotone
  // The completed sequence never exceeds submitted query/draw work.
  DXMT_ASSERT(completedSequenceId_ <= submittedSequenceId_);
  return query->getData(output, size, flags, completedSequenceId_);
}

void Device::completeUpTo(u64 sequenceId) {
  // TLA+: QuerySeqId / SeqIdMonotone
  // GPUComplete advances completedSeqId monotonically and keeps it bounded by
  // the submitted sequence cursor.
  DXMT_ASSERT(sequenceId >= completedSequenceId_);
  completedSequenceId_ = std::max(completedSequenceId_, sequenceId);
  DXMT_ASSERT(completedSequenceId_ <= submittedSequenceId_);
}

void Device::registerBuffer(const std::shared_ptr<Buffer>& buffer) {
  buffers_.push_back(buffer);
}

void Device::registerTexture(const std::shared_ptr<Texture>& texture) {
  textures_.push_back(texture);
}

void Device::registerSurface(const std::shared_ptr<Surface>& surface) {
  surfaces_.push_back(surface);
}

void Device::invalidateDefaultPoolResources() {
  auto invalidateWeak = [](auto& list) {
    list.erase(std::remove_if(list.begin(), list.end(), [](const auto& weak) {
                 if (auto ptr = weak.lock()) {
                   if (ptr->desc().pool == Pool::Default) {
                     ptr->invalidate();
                   }
                   return false;
                 }
                 return true;
               }),
               list.end());
  };
  invalidateWeak(buffers_);
  invalidateWeak(textures_);
  invalidateWeak(surfaces_);
}

void Device::submitClearInternal(const ClearDesc& desc) {
  if (renderTraceEnabled()) {
    emitRenderTrace("clear seq=%llu color=%d depth=%d stencil=%d color0=0x%llx depthStencil=0x%llx rects=%zu rgba=(%.3f,%.3f,%.3f,%.3f) depthValue=%.3f stencilValue=%u",
                    static_cast<unsigned long long>(submittedSequenceId_ + 1),
                    desc.clearColor ? 1 : 0,
                    desc.clearDepth ? 1 : 0,
                    desc.clearStencil ? 1 : 0,
                    static_cast<unsigned long long>(desc.colorAttachments[0].handle.value),
                    static_cast<unsigned long long>(desc.depthStencil.handle.value),
                    desc.rects.size(),
                    desc.color.r,
                    desc.color.g,
                    desc.color.b,
                    desc.color.a,
                    desc.depth,
                    desc.stencil);
  }
  upperDevice_->submitClear(desc);
  ++submittedSequenceId_;
  // SeqIdSafety: a submission can advance the current sequence, but never
  // below the completed sequence.
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
}

void Device::submitDrawRunInternal(DrawRunDesc desc) {
  if (drawRunEmpty(desc)) {
    return;
  }
  const auto draws = drawRunDraws(desc);
  const auto& hot = desc.state.hot;
  if (renderTraceEnabled()) {
    const auto& shader = desc.state.shaderLayout;
    const auto stageState = [&](size_t stageIndex, u32 key) -> u32 {
      if (stageIndex >= hot.textureStageStates.size()) {
        return 0u;
      }
      return flatStateOr(hot.textureStageStates[stageIndex], key, 0u);
    };
    const auto renderState = [&](u32 key, u32 fallback = 0u) -> u32 {
      return flatStateOr(hot.renderStates, key, fallback);
    };
    for (size_t i = 0; i < draws.size(); ++i) {
      const auto& draw = draws[i];
      emitRenderTrace("draw seq=%llu primType=%u primCount=%u startVertex=%u baseVertex=%d startIndex=%u rt0=0x%llx ds=0x%llx tex0=0x%llx vs=%u ps=%u vsHash=0x%llx psHash=0x%llx stateHash=0x%llx fvf=0x%x lighting=%u cull=%u alphaTest=%u alphaBlend=%u srcBlend=%u dstBlend=%u colorOp0=%u alphaOp0=%u tcIdx0=0x%x ttff0=0x%x colorOp1=%u alphaOp1=%u tcIdx1=0x%x ttff1=0x%x clipMask=0x%x indexed=%u",
                      static_cast<unsigned long long>(submittedSequenceId_ + 1 + i),
                      static_cast<unsigned>(draw.primitiveType),
                      draw.primitiveCount,
                      draw.startVertex,
                      draw.baseVertexIndex,
                      draw.startIndex,
                      static_cast<unsigned long long>(hot.colorAttachments[0].handle.value),
                      static_cast<unsigned long long>(hot.depthStencil.handle.value),
                      static_cast<unsigned long long>(hot.textures[0].value),
                      static_cast<unsigned>(shader.vertexShader.kind),
                      static_cast<unsigned>(shader.pixelShader.kind),
                      static_cast<unsigned long long>(hashShaderRef(shader.vertexShader)),
                      static_cast<unsigned long long>(hashShaderRef(shader.pixelShader)),
                      static_cast<unsigned long long>(hot.key.renderStateHash),
                      shader.vertexDecl.fvf,
                      renderState(RS_LIGHTING),
                      renderState(RS_CULL_MODE, static_cast<u32>(CullMode::Ccw)),
                      renderState(RS_ALPHA_TEST_ENABLE),
                      renderState(RS_ALPHABLEND_ENABLE),
                      renderState(RS_SRC_BLEND),
                      renderState(RS_DEST_BLEND),
                      stageState(0, TSS_COLOR_OP),
                      stageState(0, TSS_ALPHA_OP),
                      stageState(0, TSS_TEXCOORD_INDEX),
                      stageState(0, TSS_TEXTURE_TRANSFORM_FLAGS),
                      stageState(1, TSS_COLOR_OP),
                      stageState(1, TSS_ALPHA_OP),
                      stageState(1, TSS_TEXCOORD_INDEX),
                      stageState(1, TSS_TEXTURE_TRANSFORM_FLAGS),
                      hot.clipPlaneMask,
                      draw.indexed ? 1u : 0u);
    }
  }

  const auto drawCount = static_cast<u64>(draws.size());
  if (activeOcclusionQuery_) {
    for (const auto& draw : draws) {
      activeOcclusionCount_ += draw.primitiveCount;
    }
  }
  upperDevice_->submitDrawRun(std::move(desc));
  submittedSequenceId_ += drawCount;
  // SeqIdSafety: a submission can advance the current sequence, but never
  // below the completed sequence.
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
}

void Device::submitPresentInternal(const SwapDesc& desc) {
  if (renderTraceEnabled()) {
    emitRenderTrace("present seq=%llu window=0x%llx size=%ux%u fmt=%u windowed=%d interval=%u",
                    static_cast<unsigned long long>(submittedSequenceId_ + 1),
                    static_cast<unsigned long long>(desc.window.value),
                    desc.width,
                    desc.height,
                    static_cast<unsigned>(desc.format),
                    desc.windowed ? 1 : 0,
                    static_cast<unsigned>(desc.interval));
  }
  upperDevice_->present(desc);
  ++submittedSequenceId_;
  // SeqIdSafety: a submission can advance the current sequence, but never
  // below the completed sequence.
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
}

void Device::maybeCaptureExperimentFrame() {
  if (experimentCapture_.captured || experimentCapture_.frame == 0 || experimentCapture_.path.empty()) {
    return;
  }
  if (presentCount_ < experimentCapture_.frame) {
    return;
  }
  const bool trace = renderTraceEnabled();
  if (trace) {
    emitRenderTrace("capture frame=%u path=%s begin", presentCount_, experimentCapture_.path.c_str());
  }
  auto chain = swapChain(0);
  if (!chain) {
    if (trace) {
      emitRenderTrace("capture frame=%u aborted: no swap chain", presentCount_);
    }
    return;
  }
  auto backBuffer = chain->backBuffer();
  if (!backBuffer || !backBuffer->valid()) {
    if (trace) {
      emitRenderTrace("capture frame=%u aborted: invalid backbuffer", presentCount_);
    }
    return;
  }
  const auto& desc = backBuffer->desc();
  const u32 bpp = bytesPerPixel(desc.format);
  if (bpp == 0) {
    if (trace) {
      emitRenderTrace("capture frame=%u aborted: unsupported format=%u", presentCount_, static_cast<unsigned>(desc.format));
    }
    return;
  }
  auto scratch = createSurface(
      {desc.width, desc.height, desc.format, Pool::Scratch, 0, false, false, MultiSampleType::None});
  if (!scratch) {
    if (trace) {
      emitRenderTrace("capture frame=%u aborted: scratch alloc failed", presentCount_);
    }
    return;
  }
  const auto readbackHr = getRenderTargetData(backBuffer, scratch);
  if (readbackHr != D3D_OK) {
    if (trace) {
      emitRenderTrace("capture frame=%u aborted: getRenderTargetData hr=0x%08x", presentCount_,
                      static_cast<unsigned>(readbackHr));
    }
    return;
  }
  auto region = scratch->lockRect(nullptr, 0);
  if (!region.data) {
    if (trace) {
      emitRenderTrace("capture frame=%u aborted: lockRect failed", presentCount_);
    }
    return;
  }
  const size_t byteCount = static_cast<size_t>(region.pitch) * desc.height;
  const bool wrote = writeBmpScreenshot(experimentCapture_.path, desc.format, desc.width, desc.height,
                                        region.pitch,
                                        std::span<const u8>(static_cast<const u8*>(region.data), byteCount));
  scratch->unlockRect();
  if (wrote) {
    experimentCapture_.captured = true;
    if (trace) {
      emitRenderTrace("capture frame=%u wrote=%s", presentCount_, experimentCapture_.path.c_str());
    }
  } else {
    if (trace) {
      emitRenderTrace("capture frame=%u aborted: writeBmp failed", presentCount_);
    }
  }
}

HResult Device::fillSurface(const std::shared_ptr<Surface>& surface, const Rect* rect, ColorRGBA color) {
  if (!surface || !surface->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (backend_) {
    ColorFillDesc backendDesc;
    backendDesc.destination = surface->handle();
    if (rect) {
      backendDesc.rect = *rect;
      backendDesc.hasRect = true;
    }
    backendDesc.color = color;
    upperDevice_->submitColorFill(backendDesc);
  }
  surface->fillColor(rect, color);
  return D3D_OK;
}

HResult Device::stretchRect(const std::shared_ptr<Surface>& src, const Rect* srcRect,
                            const std::shared_ptr<Surface>& dst, const Rect* dstRect, bool linear) {
  if (!src || !dst || !src->valid() || !dst->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (src->desc().format != dst->desc().format) {
    return D3DERR_NOTAVAILABLE;
  }
  Rect srcArea = srcRect ? *srcRect
                         : Rect{0, 0, static_cast<i32>(src->desc().width), static_cast<i32>(src->desc().height)};
  Rect dstArea = dstRect ? *dstRect
                         : Rect{0, 0, static_cast<i32>(dst->desc().width), static_cast<i32>(dst->desc().height)};
  const i32 srcWidth = std::max(0, srcArea.right - srcArea.left);
  const i32 srcHeight = std::max(0, srcArea.bottom - srcArea.top);
  const i32 dstWidth = std::max(0, dstArea.right - dstArea.left);
  const i32 dstHeight = std::max(0, dstArea.bottom - dstArea.top);
  if (srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0) {
    return D3DERR_INVALIDCALL;
  }

  if (backend_) {
    StretchRectDesc backendDesc;
    backendDesc.source = src->handle();
    backendDesc.destination = dst->handle();
    backendDesc.sourceRect = srcArea;
    backendDesc.destinationRect = dstArea;
    backendDesc.linear = linear;
    upperDevice_->submitStretchRect(backendDesc);
    if (canTrustGpuReadback(backend_) &&
        (backendOwnsSurfaceContents(src->desc()) || backendOwnsSurfaceContents(dst->desc()))) {
      return D3D_OK;
    }
  }

  auto extractRegion = [&](const std::shared_ptr<Surface>& surface, const Rect& area) -> std::vector<u8> {
    const u32 bpp = bytesPerPixel(surface->desc().format);
    const u32 width = static_cast<u32>(std::max(0, area.right - area.left));
    const u32 height = static_cast<u32>(std::max(0, area.bottom - area.top));
    auto region = surface->lockRect(&area, 0);
    if (!region.data || bpp == 0) {
      if (region.data) {
        surface->unlockRect();
      }
      return {};
    }
    std::vector<u8> out(static_cast<size_t>(width) * height * bpp);
    const auto* srcBytes = static_cast<const u8*>(region.data);
    for (u32 y = 0; y < height; ++y) {
      std::memcpy(out.data() + static_cast<size_t>(y) * width * bpp,
                  srcBytes + static_cast<size_t>(y) * region.pitch,
                  static_cast<size_t>(width) * bpp);
    }
    surface->unlockRect();
    return out;
  };

  auto blitRegion = [&](const std::shared_ptr<Surface>& surface, const Rect& area, std::span<const u8> bytes,
                        u32 srcWidthPixels, u32 srcHeightPixels) -> HResult {
    const u32 bpp = bytesPerPixel(surface->desc().format);
    if (bpp == 0) {
      return D3DERR_INVALIDCALL;
    }
    auto region = surface->lockRect(&area, 0);
    if (!region.data) {
      return D3DERR_INVALIDCALL;
    }
    const u32 dstW = static_cast<u32>(std::max(0, area.right - area.left));
    const u32 dstH = static_cast<u32>(std::max(0, area.bottom - area.top));
    std::vector<u8> temp;
    if (srcWidthPixels == dstW && srcHeightPixels == dstH) {
      temp.assign(bytes.begin(), bytes.end());
    } else {
      temp.resize(static_cast<size_t>(dstW) * dstH * bpp);
      std::vector<u8> srcCopy(bytes.begin(), bytes.end());
      if (!stretchPixels(temp, dstW * bpp, dstW, dstH, surface->desc().format, srcCopy, srcWidthPixels * bpp,
                         srcWidthPixels, srcHeightPixels, surface->desc().format)) {
        surface->unlockRect();
        return D3DERR_INVALIDCALL;
      }
    }
    const auto* srcBytes = temp.data();
    for (u32 y = 0; y < dstH; ++y) {
      std::memcpy(static_cast<u8*>(region.data) + static_cast<size_t>(y) * region.pitch,
                  srcBytes + static_cast<size_t>(y) * dstW * bpp, static_cast<size_t>(dstW) * bpp);
    }
    surface->unlockRect();
    return D3D_OK;
  };

  const auto srcBytes = extractRegion(src, srcArea);
  if (srcBytes.empty()) {
    return D3DERR_INVALIDCALL;
  }

  const HResult result = blitRegion(dst, dstArea, std::span<const u8>(srcBytes.data(), srcBytes.size()),
                                    static_cast<u32>(srcWidth), static_cast<u32>(srcHeight));
  if (result != D3D_OK) {
    return result;
  }
  return D3D_OK;
}

HResult Device::updateSurface(const std::shared_ptr<Surface>& src, const std::shared_ptr<Surface>& dst) {
  if (!src || !dst || !src->valid() || !dst->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (src->desc().format != dst->desc().format) {
    return D3DERR_NOTAVAILABLE;
  }

  if (backend_) {
    SurfaceCopyDesc backendDesc;
    backendDesc.source = src->handle();
    backendDesc.destination = dst->handle();
    backendDesc.sourceRect = {0, 0, static_cast<i32>(src->desc().width), static_cast<i32>(src->desc().height)};
    backendDesc.destinationRect = {0, 0, static_cast<i32>(dst->desc().width), static_cast<i32>(dst->desc().height)};
    upperDevice_->submitSurfaceCopy(backendDesc);
  }

  auto srcRegion = src->lockRect(nullptr, 0);
  auto dstRegion = dst->lockRect(nullptr, 0);
  if (!srcRegion.data || !dstRegion.data) {
    if (srcRegion.data) {
      src->unlockRect();
    }
    if (dstRegion.data) {
      dst->unlockRect();
    }
    return D3DERR_INVALIDCALL;
  }

  const u32 bpp = bytesPerPixel(src->desc().format);
  const u32 width = std::min(src->desc().width, dst->desc().width);
  const u32 height = std::min(src->desc().height, dst->desc().height);
  for (u32 y = 0; y < height; ++y) {
    std::memcpy(static_cast<u8*>(dstRegion.data) + static_cast<size_t>(y) * dstRegion.pitch,
                static_cast<const u8*>(srcRegion.data) + static_cast<size_t>(y) * srcRegion.pitch,
                static_cast<size_t>(width) * bpp);
  }
  src->unlockRect();
  dst->unlockRect();
  return D3D_OK;
}

HResult Device::updateTexture(const std::shared_ptr<Texture>& src, const std::shared_ptr<Texture>& dst) {
  if (!src || !dst || !src->valid() || !dst->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (src->desc().format != dst->desc().format) {
    return D3DERR_NOTAVAILABLE;
  }
  const u32 levels = std::min(src->levelCount(), dst->levelCount());
  for (u32 level = 0; level < levels; ++level) {
    auto srcSurface = src->surfaceLevel(level);
    auto dstSurface = dst->surfaceLevel(level);
    if (!srcSurface || !dstSurface) {
      return D3DERR_INVALIDCALL;
    }
    if (backend_) {
      SurfaceCopyDesc backendDesc;
      backendDesc.source = srcSurface->handle();
      backendDesc.destination = dstSurface->handle();
      backendDesc.sourceLevel = 0;
      backendDesc.destinationLevel = 0;
      backendDesc.sourceRect = {0, 0, static_cast<i32>(srcSurface->desc().width),
                                static_cast<i32>(srcSurface->desc().height)};
      backendDesc.destinationRect = {0, 0, static_cast<i32>(dstSurface->desc().width),
                                     static_cast<i32>(dstSurface->desc().height)};
      upperDevice_->submitSurfaceCopy(backendDesc);
    }
  }
  dst->copyFrom(*src);
  return D3D_OK;
}

HResult Device::getRenderTargetData(const std::shared_ptr<Surface>& src, const std::shared_ptr<Surface>& dst) {
  if (!src || !dst || !src->valid() || !dst->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (backend_) {
    ReadbackDesc backendDesc;
    backendDesc.source = src->handle();
    backendDesc.destination = dst->handle();
    backendDesc.sourceRect = {0, 0, static_cast<i32>(src->desc().width), static_cast<i32>(src->desc().height)};
    upperDevice_->submitReadback(backendDesc);
    upperDevice_->flush();
    ReadbackPixels pixels;
    if (backend_->readbackSurface(backendDesc, pixels)) {
      auto dstRegion = dst->lockRect(nullptr, 0);
      if (!dstRegion.data) {
        return D3DERR_INVALIDCALL;
      }
      const u32 bpp = bytesPerPixel(src->desc().format);
      if (bpp == 0) {
        dst->unlockRect();
        return D3DERR_NOTAVAILABLE;
      }
      const u32 width = std::min(src->desc().width, dst->desc().width);
      const u32 height = std::min(src->desc().height, dst->desc().height);
      const size_t rowBytes = static_cast<size_t>(width) * bpp;
      if (pixels.pitch < rowBytes || pixels.bytes.size() < static_cast<size_t>(pixels.pitch) * height) {
        dst->unlockRect();
        return D3DERR_INVALIDCALL;
      }
      for (u32 y = 0; y < height; ++y) {
        std::memcpy(static_cast<u8*>(dstRegion.data) + static_cast<size_t>(y) * dstRegion.pitch,
                    pixels.bytes.data() + static_cast<size_t>(y) * pixels.pitch, rowBytes);
      }
      dst->unlockRect();
      return D3D_OK;
    }
  }
  return updateSurface(src, dst);
}

void Device::resetState() {
  state_.reset();
  invalidateDrawStateCache();
}

FfpVertexKey makeFfpVertexKey(const DeviceState& state) {
  FfpVertexKey key;
  key.lightingEnabled = state.renderStates.contains(RS_LIGHTING) && state.renderStates.at(RS_LIGHTING) != 0;
  key.specularEnabled = state.renderStates.contains(RS_SPECULAR_ENABLE) &&
                        state.renderStates.at(RS_SPECULAR_ENABLE) != 0;
  key.normalizeNormals = state.renderStates.contains(RS_NORMALIZE_NORMALS) &&
                         state.renderStates.at(RS_NORMALIZE_NORMALS) != 0;
  for (size_t i = 0; i < kMaxLights; ++i) {
    key.lightEnabled[i] = state.lightEnabled[i];
    key.lightType[i] = static_cast<u32>(state.lights[i].type);
  }
  key.colorMaterialMode[0] = state.renderStates.contains(RS_EMISSIVE_MATERIAL_SOURCE)
                                 ? state.renderStates.at(RS_EMISSIVE_MATERIAL_SOURCE)
                                 : 0;
  key.colorMaterialMode[1] = state.renderStates.contains(RS_AMBIENT_MATERIAL_SOURCE)
                                 ? state.renderStates.at(RS_AMBIENT_MATERIAL_SOURCE)
                                 : 0;
  key.colorMaterialMode[2] = state.renderStates.contains(RS_DIFFUSE_MATERIAL_SOURCE)
                                 ? state.renderStates.at(RS_DIFFUSE_MATERIAL_SOURCE)
                                 : 0;
  key.colorMaterialMode[3] = state.renderStates.contains(RS_SPECULAR_MATERIAL_SOURCE)
                                 ? state.renderStates.at(RS_SPECULAR_MATERIAL_SOURCE)
                                 : 0;
  key.fogMode = static_cast<FogMode>(state.renderStates.contains(RS_FOG_TABLE_MODE)
                                         ? state.renderStates.at(RS_FOG_TABLE_MODE)
                                         : 0);
  key.fogFromVertex = state.renderStates.contains(RS_FOG_FROM_VERTEX) &&
                      state.renderStates.at(RS_FOG_FROM_VERTEX) != 0;
  key.rangeFog = state.renderStates.contains(RS_RANGE_FOG) && state.renderStates.at(RS_RANGE_FOG) != 0;
  for (size_t i = 0; i < kMaxTextureStages; ++i) {
    key.texCoordGen[i] = state.textureStageStates[i].contains(TSS_TEXCOORD_INDEX)
                             ? state.textureStageStates[i].at(TSS_TEXCOORD_INDEX)
                             : 0;
    key.texTransformFlags[i] = state.textureStageStates[i].contains(TSS_TEXTURE_TRANSFORM_FLAGS)
                                   ? state.textureStageStates[i].at(TSS_TEXTURE_TRANSFORM_FLAGS)
                                   : 0;
  }
  key.vertexBlend = state.renderStates.contains(RS_VERTEX_BLEND) ? state.renderStates.at(RS_VERTEX_BLEND) : 0;
  key.indexedVertexBlend = key.vertexBlend != 0;
  key.clipPlaneMask = state.renderStates.contains(RS_CLIP_PLANE_ENABLE)
                          ? state.renderStates.at(RS_CLIP_PLANE_ENABLE)
                          : 0;
  key.hash = hashFfpVertexKey(key);
  return key;
}

FfpPixelKey makeFfpPixelKey(const DeviceState& state) {
  FfpPixelKey key;
  for (size_t stage = 0; stage < kMaxTextureStages; ++stage) {
    const auto& map = state.textureStageStates[stage];
    auto& out = key.stages[stage];
    out.colorOp = map.contains(TSS_COLOR_OP) ? map.at(TSS_COLOR_OP) : 0;
    out.colorArg1 = map.contains(TSS_COLOR_ARG1) ? map.at(TSS_COLOR_ARG1) : 0;
    out.colorArg2 = map.contains(TSS_COLOR_ARG2) ? map.at(TSS_COLOR_ARG2) : 0;
    out.alphaOp = map.contains(TSS_ALPHA_OP) ? map.at(TSS_ALPHA_OP) : 0;
    out.alphaArg1 = map.contains(TSS_ALPHA_ARG1) ? map.at(TSS_ALPHA_ARG1) : 0;
    out.alphaArg2 = map.contains(TSS_ALPHA_ARG2) ? map.at(TSS_ALPHA_ARG2) : 0;
    out.resultArg = map.contains(TSS_RESULT_ARG) ? map.at(TSS_RESULT_ARG) : 0;
    out.texType = map.contains(TSS_TEXTURE_TYPE) ? map.at(TSS_TEXTURE_TYPE) : 0;
    out.texCoordIndex = map.contains(TSS_TEXCOORD_INDEX) ? map.at(TSS_TEXCOORD_INDEX) : 0;
  }
  key.fogMode = static_cast<FogMode>(state.renderStates.contains(RS_FOG_TABLE_MODE)
                                         ? state.renderStates.at(RS_FOG_TABLE_MODE)
                                         : 0);
  key.alphaTestEnable = state.renderStates.contains(RS_ALPHA_TEST_ENABLE) &&
                        state.renderStates.at(RS_ALPHA_TEST_ENABLE) != 0;
  key.alphaTestFunc = state.renderStates.contains(RS_ALPHA_FUNC) ? state.renderStates.at(RS_ALPHA_FUNC) : 0;
  key.hash = hashFfpPixelKey(key);
  return key;
}

namespace {

// Test-only wrapper: builds a dxmt9::Device around an externally-provided
// BackendDevice. Returns a null WMT::Device — tests don't exercise the
// Metal surface, only the core object graph.
class StubDxmt9Device final : public dxmt9::Device {
 public:
  StubDxmt9Device(BackendLimits limits, std::shared_ptr<BackendDevice> backend)
      : limits_(limits), queue_(WMT::Device{NULL_OBJECT_HANDLE}, limits_), backend_(std::move(backend)) {}
  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  dxmt9::CommandQueue& queue() override { return queue_; }
  const BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<BackendDevice> backend() override { return backend_; }

  // Tests drive the backend's trigger* helpers directly, expecting the
  // observer wired by Factory::createDevice to fire. Forward through so the
  // mock backend stays the source of truth on the stub path.
  void setDeviceLostObserver(BackendDevice::DeviceLostObserver observer) override {
    if (backend_) backend_->setDeviceLostObserver(std::move(observer));
  }
  void setPresentationStatusObserver(BackendDevice::PresentationStatusObserver observer) override {
    if (backend_) backend_->setPresentationStatusObserver(std::move(observer));
  }
  void setMaxFrameLatency(std::uint32_t latency) override {
    if (backend_) backend_->setMaxFrameLatency(latency);
  }

  // Resource-ops + submit forwarding. Tests use mock backends as the source
  // of truth, while production submission stays on dxmt9::Device.
  BufferHandle createBuffer(const BufferDesc& desc) override {
    return backend_ ? backend_->createBuffer(desc) : BufferHandle{};
  }
  TextureHandle createTexture(const TextureDesc& desc) override {
    return backend_ ? backend_->createTexture(desc) : TextureHandle{};
  }
  SurfaceHandle createSurface(const SurfaceDesc& desc) override {
    return backend_ ? backend_->createSurface(desc) : SurfaceHandle{};
  }
  SurfaceHandle createSurfaceForTexture(TextureHandle handle, std::uint32_t level,
                                          const SurfaceDesc& desc) override {
    return backend_ ? backend_->createSurfaceForTexture(handle, level, desc) : SurfaceHandle{};
  }
  void destroyBuffer(BufferHandle handle) override {
    if (backend_) backend_->destroyBuffer(handle);
  }
  void destroyTexture(TextureHandle handle) override {
    if (backend_) backend_->destroyTexture(handle);
  }
  void destroySurface(SurfaceHandle handle) override {
    if (backend_) backend_->destroySurface(handle);
  }
  void* mapBuffer(BufferHandle handle, std::uint32_t flags) override {
    return backend_ ? backend_->mapBuffer(handle, flags) : nullptr;
  }
  void unmapBuffer(BufferHandle handle) override {
    if (backend_) backend_->unmapBuffer(handle);
  }
  void uploadBufferData(BufferHandle handle, std::span<const std::uint8_t> bytes) override {
    if (backend_) backend_->uploadBufferData(handle, bytes);
  }
  void uploadTextureLevel(TextureHandle handle, std::uint32_t level, std::uint32_t w,
                            std::uint32_t h, std::uint32_t pitch,
                            std::span<const std::uint8_t> bytes) override {
    if (backend_) backend_->uploadTextureLevel(handle, level, w, h, pitch, bytes);
  }
  void submitDrawRun(DrawRunDesc desc) override {
    if (!backend_) {
      return;
    }
    backend_->submitDrawRun(desc);
  }
  void submitClear(const ClearDesc& desc) override {
    if (backend_) backend_->submitClear(desc);
  }
  void submitSurfaceCopy(const SurfaceCopyDesc& desc) override {
    if (backend_) backend_->submitSurfaceCopy(desc);
  }
  void submitStretchRect(const StretchRectDesc& desc) override {
    if (backend_) backend_->submitStretchRect(desc);
  }
  void submitReadback(const ReadbackDesc& desc) override {
    if (backend_) backend_->submitReadback(desc);
  }
  void submitColorFill(const ColorFillDesc& desc) override {
    if (backend_) backend_->submitColorFill(desc);
  }
  void present(const SwapDesc& desc) override {
    if (backend_) backend_->present(desc);
  }
  void flush() override {
    if (backend_) backend_->flush();
  }
  HResult waitForVBlank(const SwapDesc& desc) override {
    return backend_ ? backend_->waitForVBlank(desc) : HResult{0};
  }
  bool readbackSurface(const ReadbackDesc& desc, ReadbackPixels& pixels) override {
    return backend_ && backend_->readbackSurface(desc, pixels);
  }

 private:
  BackendLimits limits_{};
  dxmt9::CommandQueue queue_;
  std::shared_ptr<BackendDevice> backend_;
};

}  // namespace

// Exposed for com.cpp's backward-compat Direct3DCreate9/Ex overloads.
std::shared_ptr<dxmt9::Device> makeStubDxmt9Device(BackendLimits limits,
                                                    std::shared_ptr<BackendDevice> backend) {
  if (!backend) {
    backend = std::make_shared<NullBackendDevice>();
  }
  return std::make_shared<StubDxmt9Device>(limits, std::move(backend));
}

namespace {

// Test convenience: build a real dxmt9::Device by selecting the first WMT
// device. Falls back to a NullBackendDevice wrapper if no WMT devices are
// available. Used by Factory(BackendLimits) for tests that exercise the
// real Metal backend without going through dxmt9c_factory_create().
std::shared_ptr<dxmt9::Device> bootstrapDeviceForTests(BackendLimits limits) {
  auto wmtDevices = WMT::CopyAllDevices();
  if (wmtDevices && wmtDevices.count() > 0) {
    dxmt9::DEVICE_DESC desc{};
    desc.device = WMT::Device{wmtDevices.object(0)};
    desc.limits = limits;
    if (auto upper = dxmt9::CreateDXMT9Device(desc)) {
      return std::shared_ptr<dxmt9::Device>(std::move(upper));
    }
  }
  return makeStubDxmt9Device(limits, std::make_shared<NullBackendDevice>());
}

}  // namespace

Factory::Factory(std::shared_ptr<dxmt9::Device> device)
    : device_(std::move(device)),
      limits_(device_ ? device_->limits() : BackendLimits{}) {
  AdapterInfo adapter;
  adapter.ordinal = 0;
  adapter.name = getenvString("DXMT_ADAPTER_NAME");
  if (adapter.name.empty()) {
    adapter.name = "NVIDIA GeForce 6800";
  }
  adapter.registryId = 1;
  adapter.displayId = 1;
  adapter.displayMode = {1920, 1080, 60, Format::A8R8G8B8};
  adapters_.push_back(std::move(adapter));
  adapterCaps_.push_back(makeDefaultCaps(limits_));
}

Factory::Factory(BackendLimits limits, std::shared_ptr<BackendDevice> backend)
    : Factory(makeStubDxmt9Device(
          limits, backend ? std::move(backend) : std::make_shared<NullBackendDevice>())) {}

Factory::Factory(BackendLimits limits) : Factory(bootstrapDeviceForTests(limits)) {}

const AdapterInfo& Factory::adapter(size_t index) const {
  if (index >= adapters_.size()) {
    throw std::out_of_range("adapter index out of range");
  }
  return adapters_[index];
}

const DeviceCaps& Factory::caps(size_t index) const {
  if (index >= adapterCaps_.size()) {
    throw std::out_of_range("caps index out of range");
  }
  return adapterCaps_[index];
}

AdapterIdentifier Factory::getAdapterIdentifier(size_t index) const {
  const auto& info = adapter(index);
  AdapterIdentifier identifier;
  identifier.description = info.name;
  identifier.deviceName = "\\\\.\\DISPLAY1";
  identifier.driver = getenvString("DXMT_ADAPTER_DRIVER");
  if (identifier.driver.empty()) {
    identifier.driver = "nvd3dum.dll";
  }
  identifier.driverVersion = info.registryId;
  identifier.vendorId = parseEnvU32Auto("DXMT_ADAPTER_VENDOR_ID").value_or(0x10deu);
  identifier.deviceId = parseEnvU32Auto("DXMT_ADAPTER_DEVICE_ID").value_or(0x0041u);
  identifier.subSysId = 0;
  identifier.revision = 0;
  identifier.monitor = info.displayId;
  return identifier;
}

std::vector<DisplayMode> Factory::enumAdapterModes(size_t index, Format format) const {
  if (index >= adapters_.size()) {
    return {};
  }
  return makeAdapterModes(format, limits_);
}

DisplayMode Factory::getAdapterDisplayMode(size_t index) const {
  return adapter(index).displayMode;
}

u32 Factory::getAdapterMonitor(size_t index) const {
  return adapter(index).displayId;
}

HRESULT Factory::checkDeviceType(size_t adapterIndex, DeviceType deviceType, Format adapterFormat,
                                 Format backBufferFormat, bool windowed) const {
  if (adapterIndex >= adapters_.size()) {
    return D3DERR_INVALIDCALL;
  }
  if (deviceType != DeviceType::Hal) {
    return D3DERR_NOTAVAILABLE;
  }
  if (windowed) {
    if (!isDisplayModeFormat(adapterFormat) || !isDisplayModeFormat(backBufferFormat)) {
      return D3DERR_NOTAVAILABLE;
    }
  } else {
    if (!isDisplayModeFormat(adapterFormat) || !isDisplayModeFormat(backBufferFormat)) {
      return D3DERR_NOTAVAILABLE;
    }
    if (adapterFormat != backBufferFormat) {
      return D3DERR_NOTAVAILABLE;
    }
    if (enumAdapterModes(adapterIndex, backBufferFormat).empty()) {
      return D3DERR_NOTAVAILABLE;
    }
  }
  if (!formatSupportsUsage(adapterFormat, UsageRenderTarget, limits_) ||
      !formatSupportsUsage(backBufferFormat, UsageRenderTarget, limits_)) {
    return D3DERR_NOTAVAILABLE;
  }
  return D3D_OK;
}

HRESULT Factory::checkDeviceFormat(size_t adapterIndex, Format format, u32 usage) const {
  if (adapterIndex >= adapters_.size()) {
    return D3DERR_INVALIDCALL;
  }
  return formatSupportsUsage(format, usage, limits_) ? D3D_OK : D3DERR_NOTAVAILABLE;
}

HRESULT Factory::checkDeviceMultiSampleType(size_t adapterIndex, Format format, MultiSampleType type) const {
  if (adapterIndex >= adapters_.size()) {
    return D3DERR_INVALIDCALL;
  }

  if (type == MultiSampleType::None) {
    return D3D_OK;
  }

  const auto supportsCount = [this](u32 count) {
    switch (count) {
      case 2:
        return limits_.supportsSampleCount2;
      case 4:
        return limits_.supportsSampleCount4;
      case 8:
        return limits_.supportsSampleCount8;
      default:
        return false;
    }
  };

  const u32 count = dxmt9::core::sampleCount(type);
  if (!supportsCount(count)) {
    return D3DERR_NOTAVAILABLE;
  }
  if (!formatSupportsUsage(format, UsageRenderTarget, limits_) &&
      !formatSupportsUsage(format, UsageDepthStencil, limits_)) {
    return D3DERR_NOTAVAILABLE;
  }
  return D3D_OK;
}

std::shared_ptr<Device> Factory::createDevice(size_t adapterIndex, const PresentParameters& params,
                                              u32 behaviorFlags) {
  if (validatePresentParameters(params, false) != D3D_OK) {
    return {};
  }
  return createDeviceValidated(adapterIndex, params, behaviorFlags, false);
}

std::shared_ptr<Device> Factory::createDeviceEx(size_t adapterIndex, const PresentParameters& params,
                                                const DisplayModeEx* fullscreenMode,
                                                u32 behaviorFlags) {
  if (const auto hr = validatePresentParameters(params, true); hr != D3D_OK) {
    return {};
  }
  if (const auto hr = validateFullscreenModeRelation(params, fullscreenMode); hr != D3D_OK) {
    return {};
  }
  return createDeviceValidated(adapterIndex, applyFullscreenMode(params, fullscreenMode), behaviorFlags, true);
}

std::shared_ptr<Device> Factory::createDeviceValidated(size_t adapterIndex, const PresentParameters& params,
                                                       u32 behaviorFlags, bool extendedDevice) {
  if (adapterIndex >= adapters_.size()) {
    return {};
  }
  const auto& adapterInfo = adapters_[adapterIndex];
  const auto normalized = normalizePresentParameters(adapterInfo, params);
  const auto fullscreenAdapterFormat =
      normalized.windowed ? adapterInfo.displayMode.format : normalized.backBufferFormat;
  if (checkDeviceType(adapterIndex, DeviceType::Hal, fullscreenAdapterFormat, normalized.backBufferFormat,
                      normalized.windowed) != D3D_OK) {
    return {};
  }
  if (!normalized.windowed) {
    const auto modes = enumAdapterModes(adapterIndex, normalized.backBufferFormat);
    const auto match = std::find_if(modes.begin(), modes.end(), [&](const DisplayMode& mode) {
      return mode.width == normalized.backBufferWidth && mode.height == normalized.backBufferHeight;
    });
    if (match == modes.end()) {
      return {};
    }
  }
  auto device = std::shared_ptr<Device>(
      new Device(adapterInfo, limits_, normalized, behaviorFlags, device_, extendedDevice));
  device->initializeDefaultSwapChain();
  if (device_) {
    std::weak_ptr<Device> weak = device;
    device_->setDeviceLostObserver([weak](bool lost) {
      if (auto locked = weak.lock()) {
        locked->setDeviceLost(lost);
      }
    });
    device_->setPresentationStatusObserver([weak](bool occluded) {
      if (auto locked = weak.lock()) {
        locked->setPresentOccluded(occluded);
      }
    });
    device_->setMaxFrameLatency(device->maximumFrameLatency());
  }
  return device;
}

}  // namespace dxmt9::core
