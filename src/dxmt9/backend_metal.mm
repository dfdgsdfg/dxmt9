#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <QuartzCore/CAMetalLayer.h>

#include "dxmt9/assert.hpp"
#include "dxmt9_backend_types.hpp"
#include "dxmt9_capture.hpp"
#include "dxmt9_compat.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9_hud.hpp"
#include "dxmt9_presenter_support.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9_shader_service.hpp"
#include "../winemetal/Metal.hpp"
#include "util/config/config.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <future>
#include <functional>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <thread>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dxmt9::core {

namespace {

using dxmt9::core::metalcapture::gpuDumpTextureHandle;
using dxmt9::core::metalcapture::gpuDumpTexturePath;
using dxmt9::core::metalcapture::writeTextureBmp;
using dxmt9::core::metalcompat::formatCompatFlags;
using dxmt9::core::metalcompat::isFloatRenderTargetFormat;
using dxmt9::core::metalcompat::matrixIsIdentity;
using dxmt9::core::metalhud::DeveloperHudController;
using dxmt9::core::metalqueue::CommandBufferDiagnostics;
using dxmt9::core::metalqueue::QueueSubmissionRecord;
using dxmt9::core::metalqueue::QueueTransitionRecord;
using dxmt9::core::metalqueue::emitQueueTraceLine;
using dxmt9::core::metalqueue::emitTextureTraceLine;
using dxmt9::core::metalqueue::queueTraceEnabled;
using dxmt9::core::metalqueue::queueTraceFilePath;
using dxmt9::core::metalqueue::queueTraceFromSeq;
using enum dxmt9::core::metalcompat::CompatFlagBits;

constexpr size_t kRingSize = 32;
constexpr size_t kMaxInflight = 3;

bool debugForceVisibleDraw() {
  static const bool enabled = dxmt9::util::getenvFlag("DXMT_DEBUG_FORCE_VISIBLE");
  return enabled;
}

bool debugSkipAllDraws() {
  static const bool enabled = dxmt9::util::getenvFlag("DXMT_SKIP_ALL_DRAWS");
  return enabled;
}

bool debugDisableScissor() {
  static const bool value = dxmt9::util::getenvFlag("DXMT_DISABLE_SCISSOR");
  return value;
}

bool debugDisableAlphaTest() {
  static const bool value = dxmt9::util::getenvFlag("DXMT_DISABLE_ALPHA_TEST");
  return value;
}

bool debugForceExpandIndexed() {
  static const bool value = dxmt9::util::getenvFlag("DXMT_FORCE_EXPAND_INDEXED");
  return value;
}


int fixedFunctionTraceBudget() {
  static const int budget = [] {
    const auto env = dxmt9::util::getenvString("DXMT_TRACE_FVF");
    if (env.empty()) {
      return 0;
    }
    return std::max(0, std::atoi(env.c_str()));
  }();
  return budget;
}

u64 fixedFunctionTraceTextureHandle() {
  static const u64 value = [] {
    const char* env = std::getenv("DXMT_TRACE_FVF_TEX0");
    if (!env || env[0] == '\0') {
      return 0ull;
    }
    return static_cast<u64>(std::strtoull(env, nullptr, 0));
  }();
  return value;
}

u64 textureTraceHandle() {
  static const u64 handle = [] {
    const char* env = std::getenv("DXMT_TRACE_TEXTURE_HANDLE");
    if (!env || env[0] == '\0') {
      return 0ull;
    }
    return static_cast<u64>(std::strtoull(env, nullptr, 0));
  }();
  return handle;
}

const char* shaderDumpDir() {
  static const char* path = std::getenv("DXMT_DUMP_SHADER_DIR");
  return path && path[0] != '\0' ? path : nullptr;
}

void maybeDumpShaderSource(const char* label, const std::string& source) {
  const char* dir = shaderDumpDir();
  if (!dir || !label) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const u64 hash = hashBytes(std::as_bytes(std::span(source)));
  const auto path = std::filesystem::path(dir) /
                    (std::string(label) + "-" + std::to_string(hash) + ".metal");
  if (std::filesystem::exists(path, ec)) {
    return;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return;
  }
  out.write(source.data(), static_cast<std::streamsize>(source.size()));
}

bool shouldDumpGpuTexture(Handle handle) {
  const u64 wanted = gpuDumpTextureHandle();
  return wanted != 0ull && handle.value == wanted;
}

u64 traceEncodeSeq() {
  static const u64 value = [] {
    const char* env = std::getenv("DXMT_TRACE_ENCODE_SEQ");
    if (!env || env[0] == '\0') {
      return 0ull;
    }
    return static_cast<u64>(std::strtoull(env, nullptr, 0));
  }();
  return value;
}

bool shouldTraceTexture(Handle handle) {
  const u64 wanted = textureTraceHandle();
  return wanted != 0ull && handle.value == wanted;
}

u64 skippedTextureHandle() {
  static const u64 value = [] {
    const char* env = std::getenv("DXMT_SKIP_TEXTURE_HANDLE");
    if (!env || env[0] == '\0') {
      return 0ull;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 0);
    if (end == env) {
      return 0ull;
    }
    return static_cast<u64>(parsed);
  }();
  return value;
}

u64 forcedPresentTextureHandle() {
  static const u64 value = [] {
    const char* env = std::getenv("DXMT_FORCE_PRESENT_TEXTURE_HANDLE");
    if (!env || env[0] == '\0') {
      return 0ull;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 0);
    if (end == env) {
      return 0ull;
    }
    return static_cast<u64>(parsed);
  }();
  return value;
}

bool shouldTraceEncode(const DrawDesc& draw, u64 seqId) {
  const u64 seq = traceEncodeSeq();
  if (seq != 0ull && seqId == seq) {
    return true;
  }
  const u64 wanted = textureTraceHandle();
  if (wanted != 0ull && draw.textures[0].handle && draw.textures[0].handle.value == wanted) {
    return true;
  }
  const u64 ffWanted = fixedFunctionTraceTextureHandle();
  return ffWanted != 0ull && draw.textures[0].handle && draw.textures[0].handle.value == ffWanted;
}

std::span<const u8> normalizeTextureUploadBytes(Format format, u32 width, u32 height, u32 pitch,
                                                std::span<const u8> bytes, std::vector<u8>& scratch) {
  if (bytes.empty()) {
    return bytes;
  }

  switch (format) {
    case Format::X8R8G8B8:
    case Format::X8B8G8R8: {
      const size_t rowBytes = static_cast<size_t>(pitch);
      const size_t expected = rowBytes * static_cast<size_t>(height);
      if (expected == 0 || bytes.size() < expected) {
        return bytes;
      }
      scratch.assign(bytes.begin(), bytes.begin() + expected);
      for (u32 y = 0; y < height; ++y) {
        u8* row = scratch.data() + static_cast<size_t>(y) * rowBytes;
        for (u32 x = 0; x < width; ++x) {
          row[static_cast<size_t>(x) * 4 + 3] = 0xffu;
        }
      }
      return scratch;
    }
    case Format::X1R5G5B5: {
      const size_t rowBytes = static_cast<size_t>(pitch);
      const size_t expected = rowBytes * static_cast<size_t>(height);
      if (expected == 0 || bytes.size() < expected) {
        return bytes;
      }
      scratch.assign(bytes.begin(), bytes.begin() + expected);
      for (u32 y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<u16*>(scratch.data() + static_cast<size_t>(y) * rowBytes);
        for (u32 x = 0; x < width; ++x) {
          row[x] |= 0x8000u;
        }
      }
      return scratch;
    }
    default:
      return bytes;
  }
}

template <typename T>
class ObjcPtr {
 public:
  struct AdoptTag {};
  struct RetainTag {};

  ObjcPtr() = default;
  explicit ObjcPtr(T object, AdoptTag) : object_(object) {}
  explicit ObjcPtr(T object, RetainTag) : object_(object) {
    if (object_) {
      [object_ retain];
    }
  }

  ObjcPtr(const ObjcPtr& other) : object_(other.object_) {
    if (object_) {
      [object_ retain];
    }
  }

  ObjcPtr(ObjcPtr&& other) noexcept : object_(other.object_) { other.object_ = nil; }

  ~ObjcPtr() {
    if (object_) {
      [object_ release];
    }
  }

  ObjcPtr& operator=(ObjcPtr other) noexcept {
    swap(other);
    return *this;
  }

  void swap(ObjcPtr& other) noexcept { std::swap(object_, other.object_); }

  T get() const noexcept { return object_; }
  explicit operator bool() const noexcept { return object_ != nil; }

  static ObjcPtr adopt(T object) { return ObjcPtr(object, AdoptTag{}); }
  static ObjcPtr retain(T object) { return ObjcPtr(object, RetainTag{}); }

 private:
  T object_ = nil;
};

template <typename T>
T* ptr(T object) {
  return object;
}

template <typename T>
obj_handle_t toWmtHandle(T object) {
  return static_cast<obj_handle_t>(reinterpret_cast<uintptr_t>(object));
}

template <typename T>
T fromWmtHandle(obj_handle_t handle) {
  return reinterpret_cast<T>(static_cast<uintptr_t>(handle));
}

WMT::Reference<WMT::Device> bootstrapWrappedDevice() {
  auto devices = WMT::CopyAllDevices();
  if (!devices || devices.count() == 0) {
    return {};
  }
  auto device = WMT::Device{devices.object(0)};
  if (!device) {
    return {};
  }
  return WMT::Reference<WMT::Device>(device);
}

WMT::Reference<WMT::CommandQueue> bootstrapWrappedCommandQueue(WMT::Device& device) {
  if (!device) {
    return {};
  }
  auto commandQueue = device.newCommandQueue(0);
  if (!commandQueue) {
    return {};
  }
  return commandQueue;
}

WMT::Reference<WMT::CommandBuffer> bootstrapCommandBuffer(WMT::CommandQueue& queue) {
  if (!queue) {
    return {};
  }
  return queue.commandBuffer();
}

u64 makeHash(const std::string& source) {
  return hashString(source);
}

WMTPixelFormat toPixelFormat(Format format, const BackendLimits& limits) {
  switch (format) {
    case Format::A8R8G8B8:
    case Format::X8R8G8B8:
      return WMTPixelFormatBGRA8Unorm;
    case Format::A8B8G8R8:
    case Format::X8B8G8R8:
      return WMTPixelFormatRGBA8Unorm;
    case Format::R5G6B5:
      return WMTPixelFormatB5G6R5Unorm;
    case Format::A1R5G5B5:
    case Format::X1R5G5B5:
      return WMTPixelFormatBGR5A1Unorm;
    case Format::A4R4G4B4:
      return WMTPixelFormatABGR4Unorm;
    case Format::A8:
      return WMTPixelFormatA8Unorm;
    case Format::A16B16G16R16F:
      return WMTPixelFormatRGBA16Float;
    case Format::A32B32G32R32F:
      return WMTPixelFormatRGBA32Float;
    case Format::G16R16F:
      return WMTPixelFormatRG16Float;
    case Format::R16F:
      return WMTPixelFormatR16Float;
    case Format::G32R32F:
      return WMTPixelFormatRG32Float;
    case Format::R32F:
      return WMTPixelFormatR32Float;
    case Format::A16B16G16R16:
      return WMTPixelFormatRGBA16Unorm;
    case Format::G16R16:
      return WMTPixelFormatRG16Unorm;
    case Format::A2R10G10B10:
      return WMTPixelFormatRGB10A2Unorm;
    case Format::A2B10G10R10:
      return WMTPixelFormatBGR10A2Unorm;
    case Format::L8:
      return WMTPixelFormatR8Unorm;
    case Format::L16:
      return WMTPixelFormatR16Unorm;
    case Format::A8L8:
      return WMTPixelFormatRG8Unorm;
    case Format::V8U8:
      return WMTPixelFormatRG8Snorm;
    case Format::Q8W8V8U8:
      return WMTPixelFormatRGBA8Snorm;
    case Format::V16U16:
      return WMTPixelFormatRG16Snorm;
    case Format::D24S8:
    case Format::D24X8:
      if (limits.supportsDepth24Stencil8) {
        return WMTPixelFormatDepth24Unorm_Stencil8;
      }
      return limits.supportsDepth32FloatStencil8 ? WMTPixelFormatDepth32Float_Stencil8
                                                 : WMTPixelFormatDepth32Float;
    case Format::D16:
    case Format::D16_LOCKABLE:
      return WMTPixelFormatDepth16Unorm;
    case Format::D32:
    case Format::D32F_LOCKABLE:
      return WMTPixelFormatDepth32Float;
    case Format::D24FS8:
      return WMTPixelFormatDepth32Float_Stencil8;
    default:
      return WMTPixelFormatBGRA8Unorm;
  }
}

bool formatHasStencilAspect(Format format) {
  switch (format) {
    case Format::D24S8:
    case Format::D24FS8:
      return true;
    default:
      return false;
  }
}

bool formatHasDepthAspect(Format format) {
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

WMTTextureType toTextureType(TextureType type, bool multisample) {
  switch (type) {
    case TextureType::TwoD:
      return multisample ? WMTTextureType2DMultisample : WMTTextureType2D;
    case TextureType::Cube:
      return WMTTextureTypeCube;
    case TextureType::Volume:
      return WMTTextureType3D;
    case TextureType::Array2D:
      return multisample ? WMTTextureType2DMultisampleArray : WMTTextureType2DArray;
  }
  return multisample ? WMTTextureType2DMultisample : WMTTextureType2D;
}

WMTResourceOptions toResourceOptions(Pool pool, u32 usage) {
  if (pool == Pool::SystemMem || pool == Pool::Scratch || pool == Pool::Managed) {
    return WMTResourceStorageModeShared;
  }
  return (usage & UsageDynamic) != 0 ? WMTResourceStorageModeShared : WMTResourceStorageModePrivate;
}

WMTTextureUsage toTextureUsage(const SurfaceDesc& desc) {
  WMTTextureUsage usage = WMTTextureUsageUnknown;
  if (desc.renderTarget || desc.depthStencil) {
    usage = static_cast<WMTTextureUsage>(usage | WMTTextureUsageRenderTarget);
  }
  usage = static_cast<WMTTextureUsage>(usage | WMTTextureUsageShaderRead);
  return usage;
}

WMTTextureUsage toTextureUsage(const TextureDesc& desc) {
  WMTTextureUsage usage = WMTTextureUsageShaderRead;
  if ((desc.usage & UsageRenderTarget) != 0 || (desc.usage & UsageDepthStencil) != 0) {
    usage = static_cast<WMTTextureUsage>(usage | WMTTextureUsageRenderTarget);
  }
  return usage;
}

WMTPrimitiveType toPrimitiveType(PrimitiveType type) {
  switch (type) {
    case PrimitiveType::PointList:
      return WMTPrimitiveTypePoint;
    case PrimitiveType::LineList:
      return WMTPrimitiveTypeLine;
    case PrimitiveType::LineStrip:
      return WMTPrimitiveTypeLineStrip;
    case PrimitiveType::TriangleList:
    case PrimitiveType::TriangleFan:
      return WMTPrimitiveTypeTriangle;
    case PrimitiveType::TriangleStrip:
      return WMTPrimitiveTypeTriangleStrip;
  }
  return WMTPrimitiveTypeTriangle;
}

WMTIndexType toIndexType(IndexType type) {
  return type == IndexType::UInt32 ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16;
}

WMTCompareFunction toCompareFunction(u32 value) {
  switch (static_cast<CompareFunc>(value)) {
    case CompareFunc::Never:      return WMTCompareFunctionNever;
    case CompareFunc::Less:       return WMTCompareFunctionLess;
    case CompareFunc::Equal:      return WMTCompareFunctionEqual;
    case CompareFunc::LessEqual:  return WMTCompareFunctionLessEqual;
    case CompareFunc::Greater:    return WMTCompareFunctionGreater;
    case CompareFunc::NotEqual:   return WMTCompareFunctionNotEqual;
    case CompareFunc::GreaterEqual: return WMTCompareFunctionGreaterEqual;
    case CompareFunc::Always:     return WMTCompareFunctionAlways;
  }
  return WMTCompareFunctionAlways;
}

[[maybe_unused]] WMTBlendOperation toBlendOperation(u32 value) {
  switch (static_cast<BlendOp>(value)) {
    case BlendOp::Add:          return WMTBlendOperationAdd;
    case BlendOp::Subtract:     return WMTBlendOperationSubtract;
    case BlendOp::RevSubtract:  return WMTBlendOperationReverseSubtract;
    case BlendOp::Min:          return WMTBlendOperationMin;
    case BlendOp::Max:          return WMTBlendOperationMax;
  }
  return WMTBlendOperationAdd;
}

[[maybe_unused]] WMTBlendFactor toBlendFactor(u32 value) {
  switch (static_cast<BlendFactor>(value)) {
    case BlendFactor::Zero:           return WMTBlendFactorZero;
    case BlendFactor::One:            return WMTBlendFactorOne;
    case BlendFactor::SrcColor:       return WMTBlendFactorSourceColor;
    case BlendFactor::InvSrcColor:    return WMTBlendFactorOneMinusSourceColor;
    case BlendFactor::SrcAlpha:       return WMTBlendFactorSourceAlpha;
    case BlendFactor::InvSrcAlpha:    return WMTBlendFactorOneMinusSourceAlpha;
    case BlendFactor::DestAlpha:      return WMTBlendFactorDestinationAlpha;
    case BlendFactor::InvDestAlpha:   return WMTBlendFactorOneMinusDestinationAlpha;
    case BlendFactor::DestColor:      return WMTBlendFactorDestinationColor;
    case BlendFactor::InvDestColor:   return WMTBlendFactorOneMinusDestinationColor;
    case BlendFactor::SrcAlphaSat:    return WMTBlendFactorSourceAlphaSaturated;
    case BlendFactor::BothSrcAlpha:   return WMTBlendFactorBlendAlpha;
    case BlendFactor::BothInvSrcAlpha: return WMTBlendFactorOneMinusBlendAlpha;
    case BlendFactor::BlendFactor:    return WMTBlendFactorBlendColor;
    case BlendFactor::InvBlendFactor: return WMTBlendFactorOneMinusBlendColor;
  }
  return WMTBlendFactorOne;
}

WMTCullMode toCullMode(u32 value) {
  switch (static_cast<CullMode>(value)) {
    case CullMode::None: return WMTCullModeNone;
    case CullMode::Cw:   return WMTCullModeBack;
    case CullMode::Ccw:  return WMTCullModeFront;
  }
  return WMTCullModeNone;
}

WMTStencilOperation toStencilOperation(u32 value) {
  switch (static_cast<StencilOp>(value)) {
    case StencilOp::Keep:    return WMTStencilOperationKeep;
    case StencilOp::Zero:    return WMTStencilOperationZero;
    case StencilOp::Replace: return WMTStencilOperationReplace;
    case StencilOp::IncrSat: return WMTStencilOperationIncrementClamp;
    case StencilOp::DecrSat: return WMTStencilOperationDecrementClamp;
    case StencilOp::Invert:  return WMTStencilOperationInvert;
    case StencilOp::Incr:    return WMTStencilOperationIncrementWrap;
    case StencilOp::Decr:    return WMTStencilOperationDecrementWrap;
  }
  return WMTStencilOperationKeep;
}

uint8_t toColorWriteMask(u32 value) {
  uint8_t mask = 0;
  if ((value & 0x1u) != 0) mask |= WMTColorWriteMaskRed;
  if ((value & 0x2u) != 0) mask |= WMTColorWriteMaskGreen;
  if ((value & 0x4u) != 0) mask |= WMTColorWriteMaskBlue;
  if ((value & 0x8u) != 0) mask |= WMTColorWriteMaskAlpha;
  return mask == 0 ? WMTColorWriteMaskAll : mask;
}

struct AttachmentKey {
  std::array<u64, kMaxRenderTargets> colorHandles{};
  u64 depthHandle = 0;
  u32 sampleCount = 1;

  friend bool operator==(const AttachmentKey&, const AttachmentKey&) = default;
};

struct AttachmentKeyHash {
  size_t operator()(const AttachmentKey& key) const noexcept {
    u64 hash = 1469598103934665603ull;
    for (auto handle : key.colorHandles) {
      hash ^= handle;
      hash *= 1099511628211ull;
    }
    hash ^= key.depthHandle;
    hash *= 1099511628211ull;
    hash ^= key.sampleCount;
    hash *= 1099511628211ull;
    return static_cast<size_t>(hash);
  }
};

u64 bloomMix64(u64 value, u64 salt) {
  u64 x = value + salt + 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

struct HazardBloom {
  std::array<u64, 2> bits{};

  void add(u64 value) {
    if (value == 0) {
      return;
    }
    const u64 hash0 = bloomMix64(value, 0x4d595df4d0f33173ull);
    const u64 hash1 = bloomMix64(value, 0x9e3779b97f4a7c15ull);
    bits[0] |= 1ull << (hash0 & 63u);
    bits[1] |= 1ull << (hash1 & 63u);
  }

  bool overlaps(const HazardBloom& other) const {
    return ((bits[0] & other.bits[0]) != 0) || ((bits[1] & other.bits[1]) != 0);
  }
};

HazardBloom makeAttachmentBloom(const RenderTargetSnapshot& rts) {
  HazardBloom bloom;
  for (const auto& attachment : rts.color) {
    bloom.add(attachment.handle.value);
  }
  bloom.add(rts.depthStencil.handle.value);
  return bloom;
}

HazardBloom makeAttachmentBloom(const ClearDesc& clear) {
  HazardBloom bloom;
  for (const auto& attachment : clear.colorAttachments) {
    bloom.add(attachment.handle.value);
  }
  bloom.add(clear.depthStencil.handle.value);
  return bloom;
}

HazardBloom makeDrawReadBloom(const DrawDesc& draw) {
  HazardBloom bloom;
  bloom.add(draw.indexBuffer.value);
  for (const auto& stream : draw.vertexDecl.streams) {
    if (stream.buffer) {
      bloom.add(stream.buffer->handle().value);
    }
  }
  for (const auto& texture : draw.textures) {
    bloom.add(texture.handle.value);
  }
  return bloom;
}

struct BlendAttachmentKey {
  bool blendingEnabled = false;
  u32 rgbBlendOperation = static_cast<u32>(BlendOp::Add);
  u32 alphaBlendOperation = static_cast<u32>(BlendOp::Add);
  u32 sourceRGBBlendFactor = static_cast<u32>(BlendFactor::One);
  u32 destinationRGBBlendFactor = static_cast<u32>(BlendFactor::Zero);
  u32 sourceAlphaBlendFactor = static_cast<u32>(BlendFactor::One);
  u32 destinationAlphaBlendFactor = static_cast<u32>(BlendFactor::Zero);
  u32 colorWriteMask = 0xfu;
  u32 pixelFormat = 0;

  friend bool operator==(const BlendAttachmentKey&, const BlendAttachmentKey&) = default;
};

struct BlendAttachmentKeyHash {
  size_t operator()(const BlendAttachmentKey& key) const noexcept {
    u64 hash = 1469598103934665603ull;
    hash ^= static_cast<u64>(key.blendingEnabled);
    hash *= 1099511628211ull;
    hash ^= key.rgbBlendOperation;
    hash *= 1099511628211ull;
    hash ^= key.alphaBlendOperation;
    hash *= 1099511628211ull;
    hash ^= key.sourceRGBBlendFactor;
    hash *= 1099511628211ull;
    hash ^= key.destinationRGBBlendFactor;
    hash *= 1099511628211ull;
    hash ^= key.sourceAlphaBlendFactor;
    hash *= 1099511628211ull;
    hash ^= key.destinationAlphaBlendFactor;
    hash *= 1099511628211ull;
    hash ^= key.colorWriteMask;
    hash *= 1099511628211ull;
    hash ^= key.pixelFormat;
    hash *= 1099511628211ull;
    return static_cast<size_t>(hash);
  }
};

struct StencilFaceKey {
  bool enabled = false;
  u32 compareFunction = static_cast<u32>(CompareFunc::Always);
  u32 failureOperation = static_cast<u32>(StencilOp::Keep);
  u32 depthFailureOperation = static_cast<u32>(StencilOp::Keep);
  u32 passOperation = static_cast<u32>(StencilOp::Keep);
  u32 readMask = 0xffu;
  u32 writeMask = 0xffu;

  friend bool operator==(const StencilFaceKey&, const StencilFaceKey&) = default;
};

struct StencilFaceKeyHash {
  size_t operator()(const StencilFaceKey& key) const noexcept {
    u64 hash = 1469598103934665603ull;
    hash ^= static_cast<u64>(key.enabled);
    hash *= 1099511628211ull;
    hash ^= key.compareFunction;
    hash *= 1099511628211ull;
    hash ^= key.failureOperation;
    hash *= 1099511628211ull;
    hash ^= key.depthFailureOperation;
    hash *= 1099511628211ull;
    hash ^= key.passOperation;
    hash *= 1099511628211ull;
    hash ^= key.readMask;
    hash *= 1099511628211ull;
    hash ^= key.writeMask;
    hash *= 1099511628211ull;
    return static_cast<size_t>(hash);
  }
};

AttachmentKey makeAttachmentKey(const RenderTargetSnapshot& rts) {
  AttachmentKey key;
  for (size_t i = 0; i < kMaxRenderTargets; ++i) {
    key.colorHandles[i] = rts.color[i].handle.value;
    key.sampleCount = std::max(key.sampleCount, rts.color[i].sampleCount);
  }
  key.depthHandle = rts.depthStencil.handle.value;
  key.sampleCount = std::max(key.sampleCount, rts.depthStencil.sampleCount);
  return key;
}

AttachmentKey makeAttachmentKey(const ClearDesc& clear) {
  AttachmentKey key;
  for (size_t i = 0; i < kMaxRenderTargets; ++i) {
    key.colorHandles[i] = clear.colorAttachments[i].handle.value;
    key.sampleCount = std::max(key.sampleCount, clear.colorAttachments[i].sampleCount);
  }
  key.depthHandle = clear.depthStencil.handle.value;
  key.sampleCount = std::max(key.sampleCount, clear.depthStencil.sampleCount);
  return key;
}

struct BufferRecord {
  BufferDesc desc{};
  WMT::Reference<WMT::Buffer> buffer;
  void* contents = nullptr;  // CPU-mapped pointer (shared mode only)
  std::vector<u8> shadow;
  bool destroyPending = false;
  u64 lastUsedSeqId = 0;
};

struct TextureRecord {
  TextureDesc desc{};
  WMT::Reference<WMT::Texture> texture;
  bool isPrivate = false;  // true if storage mode is private (no CPU access)
  bool destroyPending = false;
  u64 lastUsedSeqId = 0;
};

struct SurfaceRecord {
  SurfaceDesc desc{};
  WMT::Reference<WMT::Texture> texture;
  WMT::Reference<WMT::Texture> resolveTexture;
  TextureHandle aliasTexture{};
  u32 level = 0;
  bool destroyPending = false;
  u64 lastUsedSeqId = 0;
};

struct DrawUniforms {
  std::array<std::array<f32, 4>, kMaxVertexConstants> vsFloatConst{};
  std::array<std::array<i32, 4>, kMaxIntegerConstants> vsIntConst{};
  std::array<u32, kMaxBoolConstants> vsBoolConst{};
  std::array<std::array<f32, 4>, 4> ffpWorldViewProj{};
  std::array<std::array<std::array<f32, 4>, 4>, kMaxTextureStages> ffpTextureTransforms{};
  std::array<std::array<f32, 4>, kMaxPixelConstants> psFloatConst{};
  std::array<std::array<i32, 4>, kMaxIntegerConstants> psIntConst{};
  std::array<u32, kMaxBoolConstants> psBoolConst{};
  std::array<ClipPlane, kMaxClipPlanes> clipPlanes{};
  std::array<f32, 2> halfPixelFixup{};
  std::array<f32, 2> viewportOrigin{};
  std::array<f32, 2> viewportSize{};
  std::array<f32, 4> textureFactor{1.0f, 1.0f, 1.0f, 1.0f};
  f32 alphaRef = 0.0f;
  f32 fogStart = 1.0f;
  f32 fogEnd = 1.0f;
  f32 fogDensity = 1.0f;
  u32 vertexStreamOffset = 0;
  u32 vertexStreamStride = 0;
  i32 vertexBaseIndex = 0;
  u32 clipPlaneMask = 0;
  u32 alphaTestEnable = 0;
  u32 alphaTestFunc = static_cast<u32>(CompareFunc::Always);
  u32 fogMode = static_cast<u32>(FogMode::None);
};

class RingArena {
 public:
  explicit RingArena(size_t capacity = 1 << 20) : storage_(capacity) {}

  void reclaim(u64 completedSeqId) {
    while (!allocations_.empty() && allocations_.front().seqId <= completedSeqId) {
      cursor_ = std::max(cursor_, allocations_.front().offset + allocations_.front().size);
      allocations_.pop_front();
    }
    if (allocations_.empty()) {
      cursor_ = 0;
    }
  }

  std::byte* allocateBytes(size_t size, size_t alignment, u64 seqId) {
    if (size == 0 || storage_.empty()) {
      return nullptr;
    }
    const size_t alignedSize = alignUp(size, alignment);
    if (alignedSize > storage_.size()) {
      return nullptr;
    }

    auto canPlace = [&](size_t offset) {
      if (offset + alignedSize > storage_.size()) {
        return false;
      }
      for (const auto& allocation : allocations_) {
        const size_t begin = allocation.offset;
        const size_t end = allocation.offset + allocation.size;
        const size_t newBegin = offset;
        const size_t newEnd = offset + alignedSize;
        if (!(newEnd <= begin || newBegin >= end)) {
          return false;
        }
      }
      return true;
    };

    size_t offset = alignUp(cursor_, alignment);
    if (!canPlace(offset)) {
      offset = 0;
      if (!canPlace(offset)) {
        // TLA+: RingSafety
        DXMT_ASSERT(false && "ring arena exhausted");
        return nullptr;
      }
    }

    allocations_.push_back({offset, alignedSize, seqId});
    cursor_ = offset + alignedSize;
    return storage_.data() + offset;
  }

  template <typename T>
  T* allocate(u64 seqId, size_t count = 1) {
    return reinterpret_cast<T*>(allocateBytes(sizeof(T) * count, alignof(T), seqId));
  }

 private:
  struct Allocation {
    size_t offset = 0;
    size_t size = 0;
    u64 seqId = 0;
  };

  static size_t alignUp(size_t value, size_t alignment) {
    if (alignment <= 1) {
      return value;
    }
    return (value + alignment - 1) & ~(alignment - 1);
  }

  std::vector<std::byte> storage_;
  size_t cursor_ = 0;
  std::deque<Allocation> allocations_;
};

struct ShaderVariantKey {
  u64 hash = 0;
  bool textured = false;
  bool linear = false;
  bool clipPlanes = false;
  bool alphaTest = false;
  bool alphaToCoverage = false;
  u32 sampleCount = 1;
  std::array<u32, kMaxRenderTargets> colorFormats{};
  std::array<BlendAttachmentKey, kMaxRenderTargets> blend{};
  u32 depthFormat = 0;
  u32 stencilFormat = 0;

  friend bool operator==(const ShaderVariantKey&, const ShaderVariantKey&) = default;
};

struct ShaderVariantKeyHash {
  size_t operator()(const ShaderVariantKey& key) const noexcept {
    u64 hash = key.hash;
    hash ^= static_cast<u64>(key.textured);
    hash *= 1099511628211ull;
    hash ^= static_cast<u64>(key.linear);
    hash *= 1099511628211ull;
    hash ^= static_cast<u64>(key.clipPlanes);
    hash *= 1099511628211ull;
    hash ^= static_cast<u64>(key.alphaTest);
    hash *= 1099511628211ull;
    hash ^= static_cast<u64>(key.alphaToCoverage);
    hash *= 1099511628211ull;
    hash ^= key.sampleCount;
    hash *= 1099511628211ull;
    for (auto fmt : key.colorFormats) {
      hash ^= fmt;
      hash *= 1099511628211ull;
    }
    for (const auto& blend : key.blend) {
      hash ^= static_cast<u64>(blend.blendingEnabled);
      hash *= 1099511628211ull;
      hash ^= blend.rgbBlendOperation;
      hash *= 1099511628211ull;
      hash ^= blend.alphaBlendOperation;
      hash *= 1099511628211ull;
      hash ^= blend.sourceRGBBlendFactor;
      hash *= 1099511628211ull;
      hash ^= blend.destinationRGBBlendFactor;
      hash *= 1099511628211ull;
      hash ^= blend.sourceAlphaBlendFactor;
      hash *= 1099511628211ull;
      hash ^= blend.destinationAlphaBlendFactor;
      hash *= 1099511628211ull;
      hash ^= blend.colorWriteMask;
      hash *= 1099511628211ull;
      hash ^= blend.pixelFormat;
      hash *= 1099511628211ull;
    }
    hash ^= key.depthFormat;
    hash *= 1099511628211ull;
    hash ^= key.stencilFormat;
    hash *= 1099511628211ull;
    return static_cast<size_t>(hash);
  }
};

struct PipelineCacheEntry {
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>> future;
};

struct DepthStencilKey {
  bool depthEnable = false;
  bool depthWrite = false;
  u32 depthFunc = static_cast<u32>(CompareFunc::Always);
  StencilFaceKey front{};
  StencilFaceKey back{};

  friend bool operator==(const DepthStencilKey&, const DepthStencilKey&) = default;
};

struct DepthStencilKeyHash {
  size_t operator()(const DepthStencilKey& key) const noexcept {
    u64 hash = 1469598103934665603ull;
    hash ^= static_cast<u64>(key.depthEnable);
    hash *= 1099511628211ull;
    hash ^= static_cast<u64>(key.depthWrite);
    hash *= 1099511628211ull;
    hash ^= key.depthFunc;
    hash *= 1099511628211ull;
    hash ^= static_cast<u64>(key.front.enabled);
    hash *= 1099511628211ull;
    hash ^= key.front.compareFunction;
    hash *= 1099511628211ull;
    hash ^= key.front.failureOperation;
    hash *= 1099511628211ull;
    hash ^= key.front.depthFailureOperation;
    hash *= 1099511628211ull;
    hash ^= key.front.passOperation;
    hash *= 1099511628211ull;
    hash ^= key.front.readMask;
    hash *= 1099511628211ull;
    hash ^= key.front.writeMask;
    hash *= 1099511628211ull;
    hash ^= static_cast<u64>(key.back.enabled);
    hash *= 1099511628211ull;
    hash ^= key.back.compareFunction;
    hash *= 1099511628211ull;
    hash ^= key.back.failureOperation;
    hash *= 1099511628211ull;
    hash ^= key.back.depthFailureOperation;
    hash *= 1099511628211ull;
    hash ^= key.back.passOperation;
    hash *= 1099511628211ull;
    hash ^= key.back.readMask;
    hash *= 1099511628211ull;
    hash ^= key.back.writeMask;
    hash *= 1099511628211ull;
    return static_cast<size_t>(hash);
  }
};

NSString* makeNSString(const std::string& text) {
  return [[NSString alloc] initWithUTF8String:text.c_str()];
}

enum class D3DShaderStage { Vertex, Pixel };

struct D3DDecodedInstruction {
  u32 opcode = 0;
  u32 controls = 0;
  bool predicated = false;
  std::vector<u32> operands;
};

struct SpirvModule {
  std::vector<u32> words;
  u64 hash = 0;
  bool usesTexture = false;
  D3DShaderStage stage = D3DShaderStage::Vertex;
  u32 major = 0;
  u32 minor = 0;
  std::vector<D3DDecodedInstruction> instructions;
};

std::string makeShaderPrelude(bool withClipDistances) {
  std::ostringstream out;
  out << "#include <metal_stdlib>\nusing namespace metal;\n";
  out << "struct DrawUniforms {\n";
  out << "  float4 vsFloatConst[" << kMaxVertexConstants << "];\n";
  out << "  int4 vsIntConst[" << kMaxIntegerConstants << "];\n";
  out << "  uint vsBoolConst[" << kMaxBoolConstants << "];\n";
  out << "  float4 ffpWorldViewProj[4];\n";
  out << "  float4 ffpTextureTransforms[" << kMaxTextureStages << "][4];\n";
  out << "  float4 psFloatConst[" << kMaxPixelConstants << "];\n";
  out << "  int4 psIntConst[" << kMaxIntegerConstants << "];\n";
  out << "  uint psBoolConst[" << kMaxBoolConstants << "];\n";
  out << "  float4 clipPlanes[6];\n";
  out << "  float2 halfPixelFixup;\n";
  out << "  float2 viewportOrigin;\n";
  out << "  float2 viewportSize;\n";
  out << "  float4 textureFactor;\n";
  out << "  float alphaRef;\n";
  out << "  float fogStart;\n";
  out << "  float fogEnd;\n";
  out << "  float fogDensity;\n";
  out << "  uint vertexStreamOffset;\n";
  out << "  uint vertexStreamStride;\n";
  out << "  int vertexBaseIndex;\n";
  out << "  uint clipPlaneMask;\n";
  out << "  uint alphaTestEnable;\n";
  out << "  uint alphaTestFunc;\n";
  out << "  uint fogMode;\n";
  out << "};\n";
  out << "struct VSOut {\n";
  out << "  float4 position [[position]];\n";
  out << "  float4 color;\n";
  out << "  float4 secondaryColor;\n";
  for (size_t i = 0; i < kMaxTextureStages; ++i) {
    out << "  float4 texcoord" << i << ";\n";
  }
  out << "  float fogFactor;\n";
  out << "  float pointSize [[point_size]];\n";
  if (withClipDistances) {
    out << "  float clipDistance [[clip_distance]] [6];\n";
  }
  out << "};\n";
  out << "inline float4 dxmt9_merge(float4 current, float4 next, uint mask) {\n";
  out << "  return float4((mask & 1u) != 0u ? next.x : current.x,\n";
  out << "                  (mask & 2u) != 0u ? next.y : current.y,\n";
  out << "                  (mask & 4u) != 0u ? next.z : current.z,\n";
  out << "                  (mask & 8u) != 0u ? next.w : current.w);\n";
  out << "}\n";
  out << "inline float dxmt9_load_f32(const device uchar* base, uint offset) {\n";
  out << "  return as_type<float>(*reinterpret_cast<const device uint*>(base + offset));\n";
  out << "}\n";
  out << "inline float2 dxmt9_load_f32x2(const device uchar* base, uint offset) {\n";
  out << "  return float2(dxmt9_load_f32(base, offset), dxmt9_load_f32(base, offset + 4u));\n";
  out << "}\n";
  out << "inline float4 dxmt9_load_f32x4(const device uchar* base, uint offset) {\n";
  out << "  return float4(dxmt9_load_f32(base, offset), dxmt9_load_f32(base, offset + 4u),\n";
  out << "                dxmt9_load_f32(base, offset + 8u), dxmt9_load_f32(base, offset + 12u));\n";
  out << "}\n";
  out << "inline float3 dxmt9_load_f32x3(const device uchar* base, uint offset) {\n";
  out << "  return float3(dxmt9_load_f32(base, offset), dxmt9_load_f32(base, offset + 4u),\n";
  out << "                dxmt9_load_f32(base, offset + 8u));\n";
  out << "}\n";
  out << "inline float4 dxmt9_load_d3dcolor(const device uchar* base, uint offset) {\n";
  out << "  const uint raw = *reinterpret_cast<const device uint*>(base + offset);\n";
  out << "  return float4(float((raw >> 16) & 0xffu), float((raw >> 8) & 0xffu), float(raw & 0xffu),\n";
  out << "                float((raw >> 24) & 0xffu)) / 255.0f;\n";
  out << "}\n";
  out << "inline float4 dxmt9_apply_texture_arg_flags(float4 value, uint arg) {\n";
  out << "  if ((arg & 0x20u) != 0u) value = value.aaaa;\n";
  out << "  if ((arg & 0x10u) != 0u) value = float4(1.0f) - value;\n";
  out << "  return value;\n";
  out << "}\n";
  out << "inline float4 dxmt9_select_texture_arg(uint arg, float4 current, float4 diffuse,\n";
  out << "                                       float4 specular, float4 texture, float4 tfactor,\n";
  out << "                                       float4 temp) {\n";
  out << "  float4 value = current;\n";
  out << "  switch (arg & 0x0fu) {\n";
  out << "    case 0u: value = diffuse; break;\n";
  out << "    case 1u: value = current; break;\n";
  out << "    case 2u: value = texture; break;\n";
  out << "    case 3u: value = tfactor; break;\n";
  out << "    case 4u: value = specular; break;\n";
  out << "    case 5u: value = temp; break;\n";
  out << "    default: value = current; break;\n";
  out << "  }\n";
  out << "  return dxmt9_apply_texture_arg_flags(value, arg);\n";
  out << "}\n";
  out << "inline float4 dxmt9_select_texcoord(VSOut in, uint index) {\n";
  out << "  switch (index) {\n";
  for (size_t i = 0; i < kMaxTextureStages; ++i) {
    out << "    case " << i << "u: return in.texcoord" << i << ";\n";
  }
  out << "    default: return in.texcoord0;\n";
  out << "  }\n";
  out << "}\n";
  out << "inline float4 dxmt9_apply_texture_op(uint op, float4 arg1, float4 arg2, float4 current) {\n";
  out << "  switch (op) {\n";
  out << "    case 1u: return current;\n";
  out << "    case 2u: return arg1;\n";
  out << "    case 3u: return arg2;\n";
  out << "    case 4u: return arg1 * arg2;\n";
  out << "    case 5u: return saturate(arg1 * arg2 * 2.0f);\n";
  out << "    case 6u: return saturate(arg1 * arg2 * 4.0f);\n";
  out << "    case 7u: return saturate(arg1 + arg2);\n";
  out << "    case 8u: return saturate(arg1 + arg2 - float4(0.5f));\n";
  out << "    case 9u: return saturate((arg1 + arg2 - float4(0.5f)) * 2.0f);\n";
  out << "    case 10u: return saturate(arg1 - arg2);\n";
  out << "    case 11u: return saturate(arg1 + arg2 - arg1 * arg2);\n";
  out << "    case 26u: return mix(arg2, arg1, current);\n";
  out << "    default: return arg1;\n";
  out << "  }\n";
  out << "}\n";
  out << "inline float2 dxmt9_apply_texture_transform(float4 coord,\n";
  out << "                                            constant DrawUniforms& uniforms,\n";
  out << "                                            uint stage,\n";
  out << "                                            uint flags) {\n";
  out << "  const uint count = flags & 0xffu;\n";
  out << "  if (count == 0u) {\n";
  out << "    return coord.xy;\n";
  out << "  }\n";
  out << "  float4 transformed = float4(dot(uniforms.ffpTextureTransforms[stage][0], coord),\n";
  out << "                              dot(uniforms.ffpTextureTransforms[stage][1], coord),\n";
  out << "                              dot(uniforms.ffpTextureTransforms[stage][2], coord),\n";
  out << "                              dot(uniforms.ffpTextureTransforms[stage][3], coord));\n";
  out << "  if ((flags & 0x100u) != 0u && count >= 2u) {\n";
  out << "    const uint divisorIndex = min(count - 1u, 3u);\n";
  out << "    const float q = transformed[divisorIndex];\n";
  out << "    if (fabs(q) > 1.0e-8f) {\n";
  out << "      transformed.xy /= q;\n";
  out << "    } else {\n";
  out << "      transformed.xy = float2(0.0f);\n";
  out << "    }\n";
  out << "  }\n";
  out << "  if (count == 1u) {\n";
  out << "    return float2(transformed.x, 0.0f);\n";
  out << "  }\n";
  out << "  return transformed.xy;\n";
  out << "}\n";
  return out.str();
}

DrawUniforms buildDrawUniforms(const DrawDesc& desc) {
  DrawUniforms uniforms;
  uniforms.vsFloatConst = desc.vsConst.float4;
  uniforms.vsIntConst = desc.vsConst.int4;
  for (size_t i = 0; i < kMaxBoolConstants; ++i) {
    uniforms.vsBoolConst[i] = desc.vsConst.bools[i] ? 1u : 0u;
  }
  for (size_t row = 0; row < 4; ++row) {
    for (size_t col = 0; col < 4; ++col) {
      uniforms.ffpWorldViewProj[row][col] = desc.worldViewProj.m[row * 4 + col];
    }
  }
  for (size_t stage = 0; stage < kMaxTextureStages; ++stage) {
    for (size_t row = 0; row < 4; ++row) {
      for (size_t col = 0; col < 4; ++col) {
        uniforms.ffpTextureTransforms[stage][row][col] = desc.textureTransforms[stage].m[row * 4 + col];
      }
    }
  }
  uniforms.psFloatConst = desc.psConst.float4;
  uniforms.psIntConst = desc.psConst.int4;
  for (size_t i = 0; i < kMaxBoolConstants; ++i) {
    uniforms.psBoolConst[i] = desc.psConst.bools[i] ? 1u : 0u;
  }
  uniforms.halfPixelFixup = halfPixelFixup(desc.viewport.viewport);
  uniforms.viewportOrigin = {static_cast<f32>(desc.viewport.viewport.x), static_cast<f32>(desc.viewport.viewport.y)};
  uniforms.viewportSize = {static_cast<f32>(std::max(1u, desc.viewport.viewport.width)),
                           static_cast<f32>(std::max(1u, desc.viewport.viewport.height))};
  if (desc.rs.values.contains(RS_TEXTURE_FACTOR)) {
    const u32 raw = desc.rs.values.at(RS_TEXTURE_FACTOR);
    uniforms.textureFactor = {
        static_cast<f32>((raw >> 16) & 0xffu) / 255.0f,
        static_cast<f32>((raw >> 8) & 0xffu) / 255.0f,
        static_cast<f32>(raw & 0xffu) / 255.0f,
        static_cast<f32>((raw >> 24) & 0xffu) / 255.0f,
    };
  }
  uniforms.vertexStreamOffset = desc.vertexDecl.streams[0].offset;
  uniforms.vertexStreamStride = desc.vertexDecl.streams[0].stride;
  uniforms.vertexBaseIndex = 0;
  uniforms.clipPlaneMask = desc.clipPlaneMask;
  uniforms.alphaTestEnable = !debugDisableAlphaTest() && desc.rs.values.contains(RS_ALPHA_TEST_ENABLE) &&
                             desc.rs.values.at(RS_ALPHA_TEST_ENABLE) != 0;
  uniforms.alphaTestFunc = desc.rs.values.contains(RS_ALPHA_FUNC)
                               ? desc.rs.values.at(RS_ALPHA_FUNC)
                               : static_cast<u32>(CompareFunc::Always);
  uniforms.fogMode = desc.rs.values.contains(RS_FOG_TABLE_MODE) ? desc.rs.values.at(RS_FOG_TABLE_MODE)
                                                                 : static_cast<u32>(FogMode::None);
  uniforms.alphaRef = desc.rs.values.contains(RS_ALPHA_REF) ? static_cast<f32>(desc.rs.values.at(RS_ALPHA_REF)) / 255.0f
                                                             : 0.0f;
  uniforms.fogStart = desc.rs.values.contains(RS_FOG_START) ? std::bit_cast<f32>(desc.rs.values.at(RS_FOG_START))
                                                            : 1.0f;
  uniforms.fogEnd = desc.rs.values.contains(RS_FOG_END) ? std::bit_cast<f32>(desc.rs.values.at(RS_FOG_END)) : 1.0f;
  uniforms.fogDensity = desc.rs.values.contains(RS_FOG_DENSITY)
                            ? std::bit_cast<f32>(desc.rs.values.at(RS_FOG_DENSITY))
                            : 1.0f;
  for (size_t i = 0; i < kMaxClipPlanes; ++i) {
    uniforms.clipPlanes[i] = desc.clipPlanes[i];
  }
  return uniforms;
}

constexpr u32 kFvfPositionMask = 0x000eu;
constexpr u32 kFvfXyz = 0x0002u;
constexpr u32 kFvfXyzrhw = 0x0004u;
constexpr u32 kFvfNormal = 0x0010u;
constexpr u32 kFvfDiffuse = 0x0040u;
constexpr u32 kFvfSpecular = 0x0080u;
constexpr u32 kFvfTexCountMask = 0x0f00u;
constexpr u32 kFvfTexCountShift = 8u;

constexpr u32 kD3DDeclTypeFloat1 = 0u;
constexpr u32 kD3DDeclTypeFloat2 = 1u;
constexpr u32 kD3DDeclTypeFloat3 = 2u;
constexpr u32 kD3DDeclTypeFloat4 = 3u;
constexpr u32 kD3DDeclTypeD3DColor = 4u;
constexpr u32 kD3DDeclUsagePosition = 0u;
constexpr u32 kD3DDeclUsagePSize = 4u;
constexpr u32 kD3DDeclUsageTexcoord = 5u;
constexpr u32 kD3DDeclUsagePositionT = 9u;
constexpr u32 kD3DDeclUsageColor = 10u;
constexpr u32 kD3DDeclUsageFog = 11u;
constexpr u32 kD3DSP_DCL_USAGE_SHIFT = 0u;
constexpr u32 kD3DSP_DCL_USAGE_MASK = 0x0000000fu;
constexpr u32 kD3DSP_DCL_USAGEINDEX_SHIFT = 16u;
constexpr u32 kD3DSP_DCL_USAGEINDEX_MASK = 0x000f0000u;

struct FixedFunctionVertexLayout {
  bool valid = false;
  bool preTransformed = false;
  u32 positionComponents = 0;
  bool hasDiffuse = false;
  std::array<bool, kMaxTextureStages> hasTexcoord{};
  u32 stride = 0;
  u32 positionOffset = 0;
  u32 diffuseOffset = 0;
  std::array<u32, kMaxTextureStages> texcoordOffset{};
  u64 hash = 0;
};

struct VertexInputBinding {
  bool valid = false;
  u32 offset = 0;
  u32 type = 0;
  u32 usage = 0;
  u32 usageIndex = 0;
};

struct VertexShaderInputLayout {
  u32 stride = 0;
  std::array<VertexInputBinding, 16> inputs{};
  u64 hash = 0;
};

u32 declTypeSize(u32 type) {
  switch (type) {
    case kD3DDeclTypeFloat1:
      return 4;
    case kD3DDeclTypeFloat2:
      return 8;
    case kD3DDeclTypeFloat3:
      return 12;
    case kD3DDeclTypeFloat4:
      return 16;
    case kD3DDeclTypeD3DColor:
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

u64 hashFixedFunctionLayout(const FixedFunctionVertexLayout& layout) {
  u64 hash = 1469598103934665603ull;
  hash ^= static_cast<u64>(layout.valid);
  hash *= 1099511628211ull;
  hash ^= static_cast<u64>(layout.preTransformed);
  hash *= 1099511628211ull;
  hash ^= layout.positionComponents;
  hash *= 1099511628211ull;
  hash ^= static_cast<u64>(layout.hasDiffuse);
  hash *= 1099511628211ull;
  hash ^= layout.stride;
  hash *= 1099511628211ull;
  hash ^= layout.positionOffset;
  hash *= 1099511628211ull;
  hash ^= layout.diffuseOffset;
  hash *= 1099511628211ull;
  for (size_t i = 0; i < layout.hasTexcoord.size(); ++i) {
    hash ^= static_cast<u64>(layout.hasTexcoord[i]);
    hash *= 1099511628211ull;
    hash ^= layout.texcoordOffset[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

u64 hashVertexShaderInputLayout(const VertexShaderInputLayout& layout) {
  u64 hash = 1469598103934665603ull;
  hash ^= layout.stride;
  hash *= 1099511628211ull;
  for (const auto& input : layout.inputs) {
    hash ^= static_cast<u64>(input.valid);
    hash *= 1099511628211ull;
    hash ^= input.offset;
    hash *= 1099511628211ull;
    hash ^= input.type;
    hash *= 1099511628211ull;
    hash ^= input.usage;
    hash *= 1099511628211ull;
    hash ^= input.usageIndex;
    hash *= 1099511628211ull;
  }
  return hash;
}

u64 hashVertexDeclaration(const VertexDeclSnapshot& decl) {
  u64 hash = 1469598103934665603ull;
  hash ^= decl.fvf;
  hash *= 1099511628211ull;
  hash ^= decl.streams[0].stride;
  hash *= 1099511628211ull;
  for (const auto& element : decl.elements) {
    hash ^= element.stream;
    hash *= 1099511628211ull;
    hash ^= element.offset;
    hash *= 1099511628211ull;
    hash ^= element.type;
    hash *= 1099511628211ull;
    hash ^= element.method;
    hash *= 1099511628211ull;
    hash ^= element.usage;
    hash *= 1099511628211ull;
    hash ^= element.usageIndex;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::optional<FixedFunctionVertexLayout> decodeFixedFunctionVertexLayout(const DrawDesc& desc) {
  FixedFunctionVertexLayout layout;
  if (!desc.vertexDecl.elements.empty()) {
    u32 computedStride = 0;
    for (const auto& element : desc.vertexDecl.elements) {
      if (element.stream != 0) {
        continue;
      }
      const u32 size = declTypeSize(element.type);
      if (size == 0) {
        continue;
      }
      computedStride = std::max(computedStride, static_cast<u32>(element.offset + size));
      if (element.usage == kD3DDeclUsagePositionT && element.usageIndex == 0 &&
          element.type == kD3DDeclTypeFloat4) {
        layout.valid = true;
        layout.preTransformed = true;
        layout.positionComponents = 4;
        layout.positionOffset = element.offset;
      } else if (element.usage == kD3DDeclUsagePosition && element.usageIndex == 0 &&
                 (element.type == kD3DDeclTypeFloat3 || element.type == kD3DDeclTypeFloat4)) {
        layout.valid = true;
        layout.preTransformed = false;
        layout.positionComponents = element.type == kD3DDeclTypeFloat4 ? 4u : 3u;
        layout.positionOffset = element.offset;
      } else if (element.usage == kD3DDeclUsageColor && element.usageIndex == 0 &&
                 element.type == kD3DDeclTypeD3DColor) {
        layout.hasDiffuse = true;
        layout.diffuseOffset = element.offset;
      } else if (element.usage == kD3DDeclUsageTexcoord && element.usageIndex < kMaxTextureStages &&
                 element.type == kD3DDeclTypeFloat2) {
        layout.hasTexcoord[element.usageIndex] = true;
        layout.texcoordOffset[element.usageIndex] = element.offset;
      }
    }
    layout.stride = desc.vertexDecl.streams[0].stride ? desc.vertexDecl.streams[0].stride : computedStride;
    if (layout.valid) {
      layout.hash = hashFixedFunctionLayout(layout);
      return layout;
    }
    return std::nullopt;
  }

  const u32 fvf = desc.vertexDecl.fvf;
  const u32 position = fvf & kFvfPositionMask;
  if (position != kFvfXyzrhw && position != kFvfXyz) {
    return std::nullopt;
  }

  layout.valid = true;
  layout.preTransformed = position == kFvfXyzrhw;
  layout.positionComponents = layout.preTransformed ? 4u : 3u;
  u32 offset = 0;
  layout.positionOffset = offset;
  offset += layout.preTransformed ? 16u : 12u;

  if ((fvf & kFvfNormal) != 0) {
    offset += 12u;
  }

  if ((fvf & kFvfDiffuse) != 0) {
    layout.hasDiffuse = true;
    layout.diffuseOffset = offset;
    offset += 4;
  }
  if ((fvf & kFvfSpecular) != 0) {
    offset += 4;
  }

  const u32 texCount = (fvf & kFvfTexCountMask) >> kFvfTexCountShift;
  if (texCount > 0) {
    for (u32 i = 0; i < std::min<u32>(texCount, kMaxTextureStages); ++i) {
      if (fvfTexcoordSize(fvf, i) >= 2u) {
        layout.hasTexcoord[i] = true;
        layout.texcoordOffset[i] = offset;
      }
      offset += fvfTexcoordSize(fvf, i) * 4u;
    }
  } else {
    for (u32 i = 0; i < texCount; ++i) {
      offset += fvfTexcoordSize(fvf, i) * 4u;
    }
  }
  for (u32 i = std::min<u32>(texCount, kMaxTextureStages); i < texCount; ++i) {
    offset += fvfTexcoordSize(fvf, i) * 4u;
  }

  layout.stride = desc.vertexDecl.streams[0].stride ? desc.vertexDecl.streams[0].stride : offset;
  layout.hash = hashFixedFunctionLayout(layout);
  return layout;
}

u32 primitiveVertexCount(PrimitiveType type, u32 primitiveCount) {
  switch (type) {
    case PrimitiveType::PointList:
      return primitiveCount;
    case PrimitiveType::LineList:
      return primitiveCount * 2u;
    case PrimitiveType::LineStrip:
      return primitiveCount + 1u;
    case PrimitiveType::TriangleList:
    case PrimitiveType::TriangleFan:
      return primitiveCount * 3u;
    case PrimitiveType::TriangleStrip:
      return primitiveCount + 2u;
  }
  return primitiveCount * 3u;
}

u32 computeVertexDeclStride(const DrawDesc& desc) {
  if (desc.vertexDecl.streams[0].stride != 0) {
    return desc.vertexDecl.streams[0].stride;
  }
  u32 computedStride = 0;
  for (const auto& element : desc.vertexDecl.elements) {
    if (element.stream != 0) {
      continue;
    }
    computedStride = std::max(computedStride, static_cast<u32>(element.offset + declTypeSize(element.type)));
  }
  return computedStride;
}

u32 indexElementSize(IndexType type) {
  return type == IndexType::UInt32 ? 4u : 2u;
}

enum class D3DRegisterKind : u32 {
  Temp,
  Input,
  ConstFloat,
  Address,
  RastOut,
  AttrOut,
  TexCoordOut,
  ConstInt,
  ColorOut,
  DepthOut,
  Sampler,
  ConstBool,
  Loop,
  MiscType,
  Predicate,
  Unknown,
};

struct D3DRegisterRef {
  D3DRegisterKind kind = D3DRegisterKind::Unknown;
  u32 index = 0;
};

constexpr u32 kD3DSIO_NOP = 0;
constexpr u32 kD3DSIO_MOV = 1;
constexpr u32 kD3DSIO_ADD = 2;
constexpr u32 kD3DSIO_SUB = 3;
constexpr u32 kD3DSIO_MAD = 4;
constexpr u32 kD3DSIO_MUL = 5;
constexpr u32 kD3DSIO_RCP = 6;
constexpr u32 kD3DSIO_RSQ = 7;
constexpr u32 kD3DSIO_DP3 = 8;
constexpr u32 kD3DSIO_DP4 = 9;
constexpr u32 kD3DSIO_MIN = 10;
constexpr u32 kD3DSIO_MAX = 11;
constexpr u32 kD3DSIO_SLT = 12;
constexpr u32 kD3DSIO_SGE = 13;
constexpr u32 kD3DSIO_EXP = 14;
constexpr u32 kD3DSIO_LOG = 15;
constexpr u32 kD3DSIO_M4x4 = 20;
constexpr u32 kD3DSIO_M4x3 = 21;
constexpr u32 kD3DSIO_M3x4 = 22;
constexpr u32 kD3DSIO_M3x3 = 23;
constexpr u32 kD3DSIO_M3x2 = 24;
constexpr u32 kD3DSIO_CALL = 25;
constexpr u32 kD3DSIO_CALLNZ = 26;
constexpr u32 kD3DSIO_LOOP = 27;
constexpr u32 kD3DSIO_RET = 28;
constexpr u32 kD3DSIO_ENDLOOP = 29;
constexpr u32 kD3DSIO_LABEL = 30;
constexpr u32 kD3DSIO_LRP = 18;
constexpr u32 kD3DSIO_FRC = 19;
constexpr u32 kD3DSIO_DCL = 31;
constexpr u32 kD3DSIO_POW = 32;
constexpr u32 kD3DSIO_CRS = 33;
constexpr u32 kD3DSIO_SGN = 34;
constexpr u32 kD3DSIO_ABS = 35;
constexpr u32 kD3DSIO_NRM = 36;
constexpr u32 kD3DSIO_SINCOS = 37;
constexpr u32 kD3DSIO_REP = 38;
constexpr u32 kD3DSIO_ENDREP = 39;
constexpr u32 kD3DSIO_IF = 40;
constexpr u32 kD3DSIO_IFC = 41;
constexpr u32 kD3DSIO_ELSE = 42;
constexpr u32 kD3DSIO_ENDIF = 43;
constexpr u32 kD3DSIO_BREAK = 44;
constexpr u32 kD3DSIO_BREAKC = 45;
constexpr u32 kD3DSIO_MOVA = 46;
constexpr u32 kD3DSIO_DEFB = 47;
constexpr u32 kD3DSIO_DEFI = 48;
constexpr u32 kD3DSIO_TEXCOORD = 64;
constexpr u32 kD3DSIO_TEXKILL = 65;
constexpr u32 kD3DSIO_TEX = 66;
constexpr u32 kD3DSIO_TEXBEM = 67;
constexpr u32 kD3DSIO_TEXBEML = 68;
constexpr u32 kD3DSIO_TEXREG2AR = 69;
constexpr u32 kD3DSIO_TEXREG2GB = 70;
constexpr u32 kD3DSIO_TEXM3x2PAD = 71;
constexpr u32 kD3DSIO_TEXM3x2TEX = 72;
constexpr u32 kD3DSIO_TEXM3x3PAD = 73;
constexpr u32 kD3DSIO_TEXM3x3TEX = 74;
constexpr u32 kD3DSIO_TEXM3x3DIFF = 75;
constexpr u32 kD3DSIO_TEXM3x3SPEC = 76;
constexpr u32 kD3DSIO_TEXM3x3VSPEC = 77;
constexpr u32 kD3DSIO_EXPP = 78;
constexpr u32 kD3DSIO_LOGP = 79;
constexpr u32 kD3DSIO_CND = 80;
constexpr u32 kD3DSIO_DEF = 81;
constexpr u32 kD3DSIO_TEXREG2RGB = 82;
constexpr u32 kD3DSIO_TEXDP3TEX = 83;
constexpr u32 kD3DSIO_TEXM3x2DEPTH = 84;
constexpr u32 kD3DSIO_TEXDP3 = 85;
constexpr u32 kD3DSIO_TEXM3x3 = 86;
constexpr u32 kD3DSIO_TEXDEPTH = 87;
constexpr u32 kD3DSIO_CMP = 88;
constexpr u32 kD3DSIO_BEM = 89;
constexpr u32 kD3DSIO_DP2ADD = 90;
constexpr u32 kD3DSIO_DSX = 91;
constexpr u32 kD3DSIO_DSY = 92;
constexpr u32 kD3DSIO_TEXLDD = 93;
constexpr u32 kD3DSIO_SETP = 94;
constexpr u32 kD3DSIO_TEXLDL = 95;
constexpr u32 kD3DSIO_BREAKP = 96;
constexpr u32 kD3DSIO_PHASE = 0xfffdu;
constexpr u32 kD3DSIO_COMMENT = 0xfffeu;
constexpr u32 kD3DSIO_END = 0xffffu;

constexpr u32 kD3DSPR_TEMP = 0;
constexpr u32 kD3DSPR_INPUT = 1;
constexpr u32 kD3DSPR_CONST = 2;
constexpr u32 kD3DSPR_ADDR = 3;
constexpr u32 kD3DSPR_RASTOUT = 4;
constexpr u32 kD3DSPR_ATTROUT = 5;
constexpr u32 kD3DSPR_TEXCRDOUT = 6;
constexpr u32 kD3DSPR_CONSTINT = 7;
constexpr u32 kD3DSPR_COLOROUT = 8;
constexpr u32 kD3DSPR_DEPTHOUT = 9;
constexpr u32 kD3DSPR_SAMPLER = 10;
constexpr u32 kD3DSPR_CONSTBOOL = 14;
constexpr u32 kD3DSPR_LOOP = 15;
constexpr u32 kD3DSPR_MISCTYPE = 17;
constexpr u32 kD3DSPR_PREDICATE = 19;

std::string formatFloatLiteral(f32 value) {
  std::ostringstream out;
  out << std::setprecision(9) << value;
  std::string text = out.str();
  if (text.find_first_of(".eE") == std::string::npos) {
    text += ".0";
  }
  if (text == "-0" || text == "-0.0") {
    text = "0.0";
  }
  text += "f";
  return text;
}

std::string formatFloatVec4(const std::array<f32, 4>& values) {
  std::ostringstream out;
  out << "float4(";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << formatFloatLiteral(values[i]);
  }
  out << ")";
  return out.str();
}

std::string formatIntVec4(const std::array<i32, 4>& values) {
  std::ostringstream out;
  out << "int4(";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << values[i];
  }
  out << ")";
  return out.str();
}

std::string componentName(u32 component) {
  switch (component & 3u) {
    case 0:
      return "x";
    case 1:
      return "y";
    case 2:
      return "z";
    default:
      return "w";
  }
}

std::array<u8, 4> decodeSwizzle(u32 token) {
  return {static_cast<u8>((token >> 16) & 0x3u), static_cast<u8>((token >> 18) & 0x3u),
          static_cast<u8>((token >> 20) & 0x3u), static_cast<u8>((token >> 22) & 0x3u)};
}

u32 decodeRegisterType(u32 token) {
  return ((token >> 28) & 0x7u) | (((token >> 11) & 0x3u) << 3);
}

u32 decodeRegisterIndex(u32 token) {
  return token & 0x7ffu;
}

u32 decodeSourceModifier(u32 token) {
  return (token >> 24) & 0xfu;
}

u32 decodeDestModifier(u32 token) {
  return (token >> 20) & 0xfu;
}

u32 decodeWriteMask(u32 token) {
  return (token >> 16) & 0xfu;
}

bool tokenHasRelativeAddressing(u32 token) {
  return ((token >> 13) & 0x1u) != 0;
}

D3DRegisterKind decodeRegisterKind(u32 type, D3DShaderStage stage) {
  switch (type) {
    case kD3DSPR_TEMP:
      return D3DRegisterKind::Temp;
    case kD3DSPR_INPUT:
      return D3DRegisterKind::Input;
    case kD3DSPR_CONST:
      return D3DRegisterKind::ConstFloat;
    case kD3DSPR_ADDR:
      return stage == D3DShaderStage::Vertex ? D3DRegisterKind::Address : D3DRegisterKind::Input;
    case kD3DSPR_RASTOUT:
      return D3DRegisterKind::RastOut;
    case kD3DSPR_ATTROUT:
      return D3DRegisterKind::AttrOut;
    case kD3DSPR_TEXCRDOUT:
      return D3DRegisterKind::TexCoordOut;
    case kD3DSPR_CONSTINT:
      return D3DRegisterKind::ConstInt;
    case kD3DSPR_COLOROUT:
      return D3DRegisterKind::ColorOut;
    case kD3DSPR_DEPTHOUT:
      return D3DRegisterKind::DepthOut;
    case kD3DSPR_SAMPLER:
      return D3DRegisterKind::Sampler;
    case kD3DSPR_CONSTBOOL:
      return D3DRegisterKind::ConstBool;
    case kD3DSPR_LOOP:
      return D3DRegisterKind::Loop;
    case kD3DSPR_MISCTYPE:
      return D3DRegisterKind::MiscType;
    case kD3DSPR_PREDICATE:
      return D3DRegisterKind::Predicate;
    default:
      return D3DRegisterKind::Unknown;
  }
}

D3DRegisterRef decodeRegisterRef(u32 token, D3DShaderStage stage) {
  return {decodeRegisterKind(decodeRegisterType(token), stage), decodeRegisterIndex(token)};
}

std::string registerName(const D3DRegisterRef& reg, D3DShaderStage stage, bool dest = false) {
  std::ostringstream out;
  switch (reg.kind) {
    case D3DRegisterKind::Temp:
      out << "r" << reg.index;
      break;
    case D3DRegisterKind::Input:
      out << "v" << reg.index;
      break;
    case D3DRegisterKind::ConstFloat:
      out << "c" << reg.index;
      break;
    case D3DRegisterKind::Address:
      out << "a" << reg.index;
      break;
    case D3DRegisterKind::RastOut:
      if (stage == D3DShaderStage::Vertex) {
        if (reg.index == 0) {
          out << "oPos";
        } else if (reg.index == 1) {
          out << "oFog";
        } else if (reg.index == 2) {
          out << "oPts";
        } else {
          out << "oR" << reg.index;
        }
      } else {
        out << "rout" << reg.index;
      }
      break;
    case D3DRegisterKind::AttrOut:
      out << "oD" << reg.index;
      break;
    case D3DRegisterKind::TexCoordOut:
      if (stage == D3DShaderStage::Vertex) {
        out << "oT" << reg.index;
      } else {
        out << "vTex" << reg.index;
      }
      break;
    case D3DRegisterKind::ConstInt:
      out << "i" << reg.index;
      break;
    case D3DRegisterKind::ColorOut:
      out << "oC" << reg.index;
      break;
    case D3DRegisterKind::DepthOut:
      out << "oDepth";
      break;
    case D3DRegisterKind::Sampler:
      out << "s" << reg.index;
      break;
    case D3DRegisterKind::ConstBool:
      out << "b" << reg.index;
      break;
    case D3DRegisterKind::Loop:
      out << "aL";
      break;
    case D3DRegisterKind::MiscType:
      if (stage == D3DShaderStage::Pixel && reg.index == 0) {
        out << "vPos";
      } else if (stage == D3DShaderStage::Pixel && reg.index == 1) {
        out << "vFace";
      } else {
        out << "misc" << reg.index;
      }
      break;
    case D3DRegisterKind::Predicate:
      out << "p" << reg.index;
      break;
    case D3DRegisterKind::Unknown:
      out << (dest ? "dst" : "src") << reg.index;
      break;
  }
  return out.str();
}

std::string applySourceModifier(std::string expr, u32 modifier) {
  switch (modifier) {
    case 0:
      return expr;
    case 1:
      return "-(" + expr + ")";
    case 2:
      return "(" + expr + " - float4(0.5f))";
    case 3:
      return "-((" + expr + ") - float4(0.5f))";
    case 4:
      return "(" + expr + " * float4(2.0f) - float4(1.0f))";
    case 11:
      return "abs(" + expr + ")";
    case 12:
      return "-abs(" + expr + ")";
    default:
      throw std::runtime_error("unsupported D3D source modifier " + std::to_string(modifier));
  }
}

std::string applySwizzle(const std::string& expr, const std::array<u8, 4>& swizzle) {
  if (swizzle[0] == 0 && swizzle[1] == 1 && swizzle[2] == 2 && swizzle[3] == 3) {
    return expr;
  }
  std::ostringstream out;
  out << "float4(" << expr << "." << componentName(swizzle[0]) << ", " << expr << "." << componentName(swizzle[1])
      << ", " << expr << "." << componentName(swizzle[2]) << ", " << expr << "." << componentName(swizzle[3])
      << ")";
  return out.str();
}

std::string opcodeName(u32 opcode) {
  switch (opcode) {
    case kD3DSIO_NOP:
      return "nop";
    case kD3DSIO_MOV:
      return "mov";
    case kD3DSIO_ADD:
      return "add";
    case kD3DSIO_SUB:
      return "sub";
    case kD3DSIO_MAD:
      return "mad";
    case kD3DSIO_MUL:
      return "mul";
    case kD3DSIO_RCP:
      return "rcp";
    case kD3DSIO_RSQ:
      return "rsq";
    case kD3DSIO_DP3:
      return "dp3";
    case kD3DSIO_DP4:
      return "dp4";
    case kD3DSIO_MIN:
      return "min";
    case kD3DSIO_MAX:
      return "max";
    case kD3DSIO_SLT:
      return "slt";
    case kD3DSIO_SGE:
      return "sge";
    case kD3DSIO_EXP:
      return "exp";
    case kD3DSIO_LOG:
      return "log";
    case kD3DSIO_M4x4:
      return "m4x4";
    case kD3DSIO_M4x3:
      return "m4x3";
    case kD3DSIO_M3x4:
      return "m3x4";
    case kD3DSIO_M3x3:
      return "m3x3";
    case kD3DSIO_M3x2:
      return "m3x2";
    case kD3DSIO_CALL:
      return "call";
    case kD3DSIO_LRP:
      return "lrp";
    case kD3DSIO_FRC:
      return "frc";
    case kD3DSIO_SINCOS:
      return "sincos";
    case kD3DSIO_REP:
      return "rep";
    case kD3DSIO_ENDREP:
      return "endrep";
    case kD3DSIO_IF:
      return "if";
    case kD3DSIO_ELSE:
      return "else";
    case kD3DSIO_ENDIF:
      return "endif";
    case kD3DSIO_BREAK:
      return "break";
    case kD3DSIO_RET:
      return "ret";
    case kD3DSIO_LOOP:
      return "loop";
    case kD3DSIO_ENDLOOP:
      return "endloop";
    case kD3DSIO_LABEL:
      return "label";
    case kD3DSIO_DCL:
      return "dcl";
    case kD3DSIO_DEFB:
      return "defb";
    case kD3DSIO_DEFI:
      return "defi";
    case kD3DSIO_POW:
      return "pow";
    case kD3DSIO_CRS:
      return "crs";
    case kD3DSIO_SGN:
      return "sgn";
    case kD3DSIO_ABS:
      return "abs";
    case kD3DSIO_NRM:
      return "nrm";
    case kD3DSIO_CND:
      return "cnd";
    case kD3DSIO_DEF:
      return "def";
    case kD3DSIO_TEX:
      return "tex";
    case kD3DSIO_TEXDEPTH:
      return "texdepth";
    case kD3DSIO_CMP:
      return "cmp";
    case kD3DSIO_BEM:
      return "bem";
    case kD3DSIO_DP2ADD:
      return "dp2add";
    case kD3DSIO_DSX:
      return "dsx";
    case kD3DSIO_DSY:
      return "dsy";
    case kD3DSIO_TEXLDD:
      return "texldd";
    case kD3DSIO_SETP:
      return "setp";
    case kD3DSIO_TEXLDL:
      return "texldl";
    case kD3DSIO_BREAKP:
      return "breakp";
    case kD3DSIO_MOVA:
      return "mova";
    case kD3DSIO_EXPP:
      return "expp";
    case kD3DSIO_LOGP:
      return "logp";
    case kD3DSIO_PHASE:
      return "phase";
    case kD3DSIO_COMMENT:
      return "comment";
    case kD3DSIO_END:
      return "end";
    default:
      return "opcode_" + std::to_string(opcode);
  }
}

u32 fixedOperandCount(u32 opcode) {
  switch (opcode) {
    case kD3DSIO_NOP:
    case kD3DSIO_PHASE:
      return 0;
    case kD3DSIO_ELSE:
    case kD3DSIO_ENDIF:
    case kD3DSIO_ENDLOOP:
    case kD3DSIO_ENDREP:
    case kD3DSIO_RET:
    case kD3DSIO_BREAK:
      return 0;
    case kD3DSIO_MOV:
    case kD3DSIO_DEFB:
    case kD3DSIO_RCP:
    case kD3DSIO_RSQ:
    case kD3DSIO_FRC:
    case kD3DSIO_DSX:
    case kD3DSIO_DSY:
    case kD3DSIO_SETP:
    case kD3DSIO_BREAKP:
    case kD3DSIO_MOVA:
    case kD3DSIO_LOG:
    case kD3DSIO_LOGP:
    case kD3DSIO_EXP:
    case kD3DSIO_EXPP:
    case kD3DSIO_SGN:
    case kD3DSIO_ABS:
    case kD3DSIO_NRM:
      return 2;
    case kD3DSIO_LABEL:
    case kD3DSIO_CALL:
    case kD3DSIO_IF:
    case kD3DSIO_LOOP:
    case kD3DSIO_REP:
      return 1;
    case kD3DSIO_ADD:
    case kD3DSIO_SUB:
    case kD3DSIO_MUL:
    case kD3DSIO_DP3:
    case kD3DSIO_DP4:
    case kD3DSIO_MIN:
    case kD3DSIO_MAX:
    case kD3DSIO_POW:
    case kD3DSIO_CRS:
    case kD3DSIO_TEXLDD:
    case kD3DSIO_TEXLDL:
    case kD3DSIO_SLT:
    case kD3DSIO_SGE:
    case kD3DSIO_M4x4:
    case kD3DSIO_M4x3:
    case kD3DSIO_M3x4:
    case kD3DSIO_M3x3:
    case kD3DSIO_M3x2:
      return 3;
    case kD3DSIO_MAD:
    case kD3DSIO_LRP:
    case kD3DSIO_CND:
    case kD3DSIO_CMP:
    case kD3DSIO_DP2ADD:
      return 4;
    case kD3DSIO_DEF:
    case kD3DSIO_DEFI:
      return 5;
    case kD3DSIO_COMMENT:
    case kD3DSIO_END:
      return 0;
    default:
      throw std::runtime_error("unsupported SM1.x opcode");
  }
}

std::string readOperandExpression(const D3DDecodedInstruction& instruction, const D3DRegisterRef& reg,
                                  const std::string& vertexInputs, const std::string& pixelInputs,
                                  bool vertexStage, const std::string& outPosition, const std::string& outColor,
                                  const std::string& outSecondaryColor, const std::string& outTexcoord0,
                                  const std::string& outFogFactor, const std::string& outPointSize,
                                  const std::string& tempPrefix, const std::string& constPrefix,
                                  const std::string& intPrefix, const std::string& boolPrefix,
                                  const std::string& predicatePrefix) {
  (void)instruction;
  switch (reg.kind) {
    case D3DRegisterKind::Temp:
      return tempPrefix + "[" + std::to_string(reg.index) + "]";
    case D3DRegisterKind::ConstFloat:
      return constPrefix + "[" + std::to_string(reg.index) + "]";
    case D3DRegisterKind::ConstInt:
      return "float4(" + intPrefix + "[" + std::to_string(reg.index) + "])";
    case D3DRegisterKind::ConstBool:
      return "(" + boolPrefix + "[" + std::to_string(reg.index) + "] != 0u ? float4(1.0f) : float4(0.0f))";
    case D3DRegisterKind::Input:
      if (vertexStage) {
        return vertexInputs + "[" + std::to_string(reg.index) + "]";
      }
      if (reg.index == 0) {
        return "float4(" + pixelInputs + ".color)";
      }
      if (reg.index == 1) {
        return "float4(" + pixelInputs + ".texcoord0, 0.0f, 1.0f)";
      }
      if (reg.index == 2) {
        return "float4(" + pixelInputs + ".secondaryColor)";
      }
      if (reg.index == 3) {
        return "float4(" + pixelInputs + ".fogFactor)";
      }
      return "float4(0.0f)";
    case D3DRegisterKind::RastOut:
      if (!vertexStage) {
        return "float4(0.0f)";
      }
      if (reg.index == 0) {
        return outPosition;
      }
      if (reg.index == 1) {
        return "float4(" + outFogFactor + ")";
      }
      if (reg.index == 2) {
        return "float4(" + outPointSize + ")";
      }
      return "float4(0.0f)";
    case D3DRegisterKind::AttrOut:
      if (vertexStage) {
        return reg.index == 0 ? outColor : outSecondaryColor;
      }
      return "float4(0.0f)";
    case D3DRegisterKind::TexCoordOut:
      return outTexcoord0;
    case D3DRegisterKind::ColorOut:
      return outColor;
    case D3DRegisterKind::DepthOut:
      return "float4(0.0f)";
    case D3DRegisterKind::Address:
    case D3DRegisterKind::Loop:
      if (reg.kind == D3DRegisterKind::Address) {
        return "float4(a0)";
      }
      return "float4(aL)";
    case D3DRegisterKind::MiscType:
    case D3DRegisterKind::Sampler:
    case D3DRegisterKind::Unknown:
      return "float4(0.0f)";
    case D3DRegisterKind::Predicate:
      return "(" + predicatePrefix + "[" + std::to_string(reg.index) + "] ? float4(1.0f) : float4(0.0f))";
  }
  return "float4(0.0f)";
}

std::string decodeOperandToken(const u32 token, D3DShaderStage stage, bool destination) {
  if (destination && tokenHasRelativeAddressing(token)) {
    throw std::runtime_error("relative addressing is not supported yet");
  }
  D3DRegisterRef reg = decodeRegisterRef(token, stage);
  return registerName(reg, stage, destination);
}

u32 decodeLabelIndex(u32 token) {
  return token & 0x7ffu;
}

std::optional<VertexShaderInputLayout> decodeVertexShaderInputLayout(const SpirvModule& module, const DrawDesc& desc) {
  if (module.stage != D3DShaderStage::Vertex) {
    return std::nullopt;
  }

  VertexShaderInputLayout layout;
  layout.stride = computeVertexDeclStride(desc);
  bool hasBinding = false;
  for (const auto& instruction : module.instructions) {
    if (instruction.opcode != kD3DSIO_DCL || instruction.operands.size() < 2) {
      continue;
    }
    const u32 semanticToken = instruction.operands[0];
    const auto dst = decodeRegisterRef(instruction.operands[1], module.stage);
    if (dst.kind != D3DRegisterKind::Input || dst.index >= layout.inputs.size()) {
      continue;
    }
    const u32 usage = (semanticToken & kD3DSP_DCL_USAGE_MASK) >> kD3DSP_DCL_USAGE_SHIFT;
    const u32 usageIndex = (semanticToken & kD3DSP_DCL_USAGEINDEX_MASK) >> kD3DSP_DCL_USAGEINDEX_SHIFT;
    for (const auto& element : desc.vertexDecl.elements) {
      if (element.stream != 0) {
        continue;
      }
      if (element.usage == usage && element.usageIndex == usageIndex) {
        layout.inputs[dst.index] = VertexInputBinding{
            .valid = true,
            .offset = element.offset,
            .type = element.type,
            .usage = usage,
            .usageIndex = usageIndex,
        };
        hasBinding = true;
        break;
      }
    }
  }
  if (!hasBinding) {
    return std::nullopt;
  }
  layout.hash = hashVertexShaderInputLayout(layout);
  return layout;
}

std::string readPixelInputExpression(u32 token, const std::string& pixelInputs) {
  const u32 type = decodeRegisterType(token);
  const u32 index = decodeRegisterIndex(token);
  switch (type) {
    case kD3DSPR_INPUT:
      if (index == 0) {
        return "float4(" + pixelInputs + ".color)";
      }
      if (index == 1) {
        return "float4(" + pixelInputs + ".secondaryColor)";
      }
      break;
    case kD3DSPR_ADDR:
      return "dxmt9_select_texcoord(" + pixelInputs + ", " + std::to_string(index) + "u)";
    case kD3DSPR_RASTOUT:
      if (index == 0) {
        return pixelInputs + ".position";
      }
      if (index == 1) {
        return "float4(" + pixelInputs + ".fogFactor)";
      }
      if (index == 2) {
        return "float4(" + pixelInputs + ".pointSize)";
      }
      break;
    default:
      break;
  }

  if (index == 0) {
    return "float4(" + pixelInputs + ".color)";
  }
  if (index == 1) {
    return "dxmt9_select_texcoord(" + pixelInputs + ", 0u)";
  }
  if (index == 2) {
    return "float4(" + pixelInputs + ".secondaryColor)";
  }
  if (index == 3) {
    return "float4(" + pixelInputs + ".fogFactor)";
  }
  return "float4(0.0f)";
}

struct FlowBlock {
  u32 opcode = 0;
  bool sawElse = false;
};

SpirvModule translateD3DBytecodeToSpirv(const ShaderRef& shader, bool vertex, const DrawDesc& desc) {
  SpirvModule module;
  const auto& bytes = shader.bytecode.bytes;
  if (bytes.size() % sizeof(u32) != 0) {
    throw std::runtime_error("D3D bytecode size is not DWORD aligned");
  }

  const u64 bytecodeHash = shader.bytecode.hash ? shader.bytecode.hash : hashBytes(std::as_bytes(std::span(bytes)));
  module.hash = bytecodeHash ^ (vertex ? 0x5356505653455254ull : 0x5350465348454453ull) ^ desc.clipPlaneMask ^
                (static_cast<u64>(desc.rts.color[0].sampleCount) << 32);
  module.words.reserve(bytes.size() / sizeof(u32));
  module.stage = vertex ? D3DShaderStage::Vertex : D3DShaderStage::Pixel;

  auto readWord = [&](size_t offset) {
    u32 word = 0;
    std::memcpy(&word, bytes.data() + offset, sizeof(u32));
    return word;
  };

  if (bytes.empty()) {
    throw std::runtime_error("empty D3D bytecode");
  }

  const u32 versionToken = readWord(0);
  module.words.push_back(versionToken);
  const u32 shaderType = versionToken >> 16;
  if (shaderType == 0xfffeu) {
    module.stage = D3DShaderStage::Vertex;
  } else if (shaderType == 0xffffu) {
    module.stage = D3DShaderStage::Pixel;
  } else {
    throw std::runtime_error("invalid D3D shader version token");
  }
  module.major = (versionToken >> 8) & 0xffu;
  module.minor = versionToken & 0xffu;
  if (module.major < 2 || module.major > 3) {
    throw std::runtime_error("only SM 2.x and 3.x bytecode is supported");
  }

  size_t offset = sizeof(u32);
  while (offset < bytes.size()) {
    const u32 token = readWord(offset);
    module.words.push_back(token);
    offset += sizeof(u32);

    const u32 opcode = token & 0xffffu;
    if (opcode == kD3DSIO_END) {
      break;
    }
    if (opcode == kD3DSIO_COMMENT) {
      const u32 commentWords = (token >> 16) & 0x7fffu;
      const size_t commentBytes = static_cast<size_t>(commentWords) * sizeof(u32);
      if (offset + commentBytes > bytes.size()) {
        throw std::runtime_error("truncated D3D comment token");
      }
      offset += commentBytes;
      continue;
    }
    if (opcode == kD3DSIO_PHASE) {
      continue;
    }

    u32 operandCount = 0;
    try {
      operandCount = fixedOperandCount(opcode);
    } catch (const std::runtime_error&) {
      operandCount = (token >> 24) & 0xfu;
      if (operandCount == 0) {
        switch (opcode) {
          case kD3DSIO_NOP:
          case kD3DSIO_ELSE:
          case kD3DSIO_ENDIF:
          case kD3DSIO_ENDLOOP:
          case kD3DSIO_ENDREP:
          case kD3DSIO_RET:
          case kD3DSIO_BREAK:
          case kD3DSIO_PHASE:
          case kD3DSIO_COMMENT:
          case kD3DSIO_END:
            break;
          default: {
            std::ostringstream message;
            message << "missing D3D operand count"
                    << " opcode=" << opcodeName(opcode)
                    << " token=0x" << std::hex << token
                    << " offset=0x" << offset - sizeof(u32);
            if (!module.instructions.empty()) {
              const auto& previous = module.instructions.back();
              message << " prevOpcode=" << opcodeName(previous.opcode)
                      << " prevTokenCount=" << std::dec << previous.operands.size()
                      << " prevOperands=[";
              for (size_t i = 0; i < previous.operands.size(); ++i) {
                if (i != 0) {
                  message << ",";
                }
                message << "0x" << std::hex << previous.operands[i];
              }
              message << "]";
            }
            const size_t rawCount = module.words.size();
            const size_t rawStart = rawCount > 8 ? rawCount - 8 : 0;
            message << " rawWords=[";
            for (size_t i = rawStart; i < rawCount; ++i) {
              if (i != rawStart) {
                message << ",";
              }
              message << "0x" << std::hex << module.words[i];
            }
            message << "]";
            throw std::runtime_error(message.str());
          }
        }
      }
    }
    const size_t operandBytes = static_cast<size_t>(operandCount) * sizeof(u32);
    if (offset + operandBytes > bytes.size()) {
      throw std::runtime_error("truncated D3D instruction");
    }

    D3DDecodedInstruction instruction;
    instruction.opcode = opcode;
    instruction.controls = (token >> 16) & 0xffu;
    instruction.predicated = ((token >> 28) & 0x1u) != 0;
    instruction.operands.reserve(operandCount);
    for (u32 i = 0; i < operandCount; ++i) {
      instruction.operands.push_back(readWord(offset + static_cast<size_t>(i) * sizeof(u32)));
      module.words.push_back(instruction.operands.back());
    }
    if (opcode == kD3DSIO_TEX || opcode == kD3DSIO_TEXLDD || opcode == kD3DSIO_TEXLDL || opcode == kD3DSIO_TEXDEPTH ||
        opcode == kD3DSIO_TEXDP3 || opcode == kD3DSIO_TEXDP3TEX || opcode == kD3DSIO_TEXM3x2DEPTH ||
        opcode == kD3DSIO_TEXM3x3 || opcode == kD3DSIO_TEXREG2RGB || opcode == kD3DSIO_BEM) {
      module.usesTexture = true;
    }
    module.instructions.push_back(std::move(instruction));
    offset += operandBytes;
  }

  return module;
}

std::string translateSpirvToMsl(const SpirvModule& module, const DrawDesc& desc, bool vertex) {
  std::ostringstream out;
  out << makeShaderPrelude(desc.clipPlaneMask != 0);
  if (vertex) {
    const auto inputLayout = decodeVertexShaderInputLayout(module, desc);
    const bool traceShaderInputs = [] {
      const char* env = std::getenv("DXMT_TRACE_SHADER_INPUTS");
      return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
    }();
    if (traceShaderInputs) {
      std::ostringstream trace;
      trace << "[dxmt9-shader] vertex inputs";
      for (const auto& instruction : module.instructions) {
        if (instruction.opcode != kD3DSIO_DCL || instruction.operands.size() < 2) {
          continue;
        }
        const u32 semanticToken = instruction.operands[0];
        const auto dst = decodeRegisterRef(instruction.operands[1], module.stage);
        trace << " dcl(v" << dst.index
              << ":usage=" << ((semanticToken & kD3DSP_DCL_USAGE_MASK) >> kD3DSP_DCL_USAGE_SHIFT)
              << ",idx=" << ((semanticToken & kD3DSP_DCL_USAGEINDEX_MASK) >> kD3DSP_DCL_USAGEINDEX_SHIFT)
              << ",tok=0x" << std::hex << semanticToken << ",reg=0x" << instruction.operands[1] << std::dec << ")";
      }
      if (inputLayout) {
        trace << " mapped";
        for (size_t i = 0; i < inputLayout->inputs.size(); ++i) {
          const auto& binding = inputLayout->inputs[i];
          if (!binding.valid) {
            continue;
          }
          trace << " v" << i << "->off" << binding.offset << "/type" << binding.type
                << "/usage" << binding.usage << ":" << binding.usageIndex;
        }
      } else {
        trace << " mapped=none";
      }
      std::fprintf(stderr, "%s\n", trace.str().c_str());
      std::fflush(stderr);
    }
    out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]], constant DrawUniforms& uniforms [[buffer(0)]], "
           "device const uchar* stream0 [[buffer(1)]]) {\n";
    out << "  VSOut out;\n";
    out << "  float2 dxmt9_positions[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n";
    out << "  float4 outPosition = float4(dxmt9_positions[vid % 3], 0.0, 1.0);\n";
    out << "  float4 outColor = float4(1.0f);\n";
    out << "  float4 outSecondaryColor = float4(0.0f);\n";
    out << "  float4 outTexcoord0 = float4(0.0f);\n";
    out << "  float outFogFactor = 1.0f;\n";
    out << "  float outPointSize = 1.0f;\n";
    out << "  float4 vin[16];\n";
    out << "  for (uint i = 0; i < 16u; ++i) { vin[i] = float4(0.0f); }\n";
    out << "  vin[0] = float4(dxmt9_positions[vid % 3], 0.0f, 1.0f);\n";
    if (inputLayout) {
      out << "  const uint stride = uniforms.vertexStreamStride != 0u ? uniforms.vertexStreamStride : "
          << inputLayout->stride << "u;\n";
      out << "  const int vertexIndex = max(0, int(vid) + uniforms.vertexBaseIndex);\n";
      out << "  const uint base = uniforms.vertexStreamOffset + uint(vertexIndex) * stride;\n";
      for (size_t i = 0; i < inputLayout->inputs.size(); ++i) {
        const auto& binding = inputLayout->inputs[i];
        if (!binding.valid) {
          continue;
        }
        switch (binding.type) {
          case kD3DDeclTypeFloat1:
            out << "  vin[" << i << "] = float4(dxmt9_load_f32(stream0, base + " << binding.offset
                << "u), 0.0f, 0.0f, 1.0f);\n";
            break;
          case kD3DDeclTypeFloat2:
            out << "  vin[" << i << "] = float4(dxmt9_load_f32x2(stream0, base + " << binding.offset
                << "u), 0.0f, 1.0f);\n";
            break;
          case kD3DDeclTypeFloat3:
            out << "  vin[" << i << "] = float4(dxmt9_load_f32x3(stream0, base + " << binding.offset
                << "u), 1.0f);\n";
            break;
          case kD3DDeclTypeFloat4:
            out << "  vin[" << i << "] = dxmt9_load_f32x4(stream0, base + " << binding.offset << "u);\n";
            break;
          case kD3DDeclTypeD3DColor:
            out << "  vin[" << i << "] = dxmt9_load_d3dcolor(stream0, base + " << binding.offset << "u);\n";
            break;
          default:
            break;
        }
      }
    }
    out << "  int a0 = 0;\n";
    out << "  int aL = 0;\n";
    out << "  float4 r[32];\n";
    out << "  float4 cFloat[" << kMaxVertexConstants << "];\n";
    out << "  int4 cInt[" << kMaxIntegerConstants << "];\n";
    out << "  uint cBool[" << kMaxBoolConstants << "];\n";
    out << "  bool p[" << kMaxBoolConstants << "];\n";
    out << "  for (uint i = 0; i < " << kMaxVertexConstants << "; ++i) { cFloat[i] = uniforms.vsFloatConst[i]; }\n";
    out << "  for (uint i = 0; i < " << kMaxIntegerConstants << "; ++i) { cInt[i] = uniforms.vsIntConst[i]; }\n";
    out << "  for (uint i = 0; i < " << kMaxBoolConstants << "; ++i) { cBool[i] = uniforms.vsBoolConst[i]; }\n";
    out << "  for (uint i = 0; i < " << kMaxBoolConstants << "; ++i) { p[i] = false; }\n";
    std::vector<FlowBlock> controlStack;
    size_t callDepth = 0;
    for (size_t instructionIndex = 0; instructionIndex < module.instructions.size(); ++instructionIndex) {
      const auto& instruction = module.instructions[instructionIndex];
      if (instruction.opcode == kD3DSIO_COMMENT || instruction.opcode == kD3DSIO_PHASE) {
        continue;
      }
      out << "  // " << opcodeName(instruction.opcode);
      for (size_t i = 0; i < instruction.operands.size(); ++i) {
        const bool destination = i == 0;
        out << (i == 0 ? " " : ", ");
        if (instruction.opcode == kD3DSIO_DEF && i > 0) {
          out << formatFloatLiteral(std::bit_cast<f32>(instruction.operands[i]));
        } else if (instruction.opcode == kD3DSIO_DEFI && i > 0) {
          out << static_cast<i32>(instruction.operands[i]);
        } else if (instruction.opcode == kD3DSIO_DEFB && i > 0) {
          out << (instruction.operands[i] != 0u ? "true" : "false");
        } else if (instruction.opcode == kD3DSIO_LABEL || instruction.opcode == kD3DSIO_CALL) {
          out << "label" << decodeLabelIndex(instruction.operands[i]);
        } else {
          out << decodeOperandToken(instruction.operands[i], module.stage, destination);
        }
      }
      out << "\n";

      auto readSrc = [&](size_t index) {
        if (index >= instruction.operands.size()) {
          std::ostringstream message;
          message << "missing D3D source operand"
                  << " opcode=" << opcodeName(instruction.opcode)
                  << " requestedIndex=" << index
                  << " operandCount=" << instruction.operands.size();
          throw std::runtime_error(message.str());
        }
        const auto token = instruction.operands[index];
        const auto reg = decodeRegisterRef(token, module.stage);
        std::string expr = readOperandExpression(instruction, reg, "vin", "in", true,
                                                 "outPosition", "outColor", "outSecondaryColor", "outTexcoord0",
                                                 "outFogFactor", "outPointSize", "r", "cFloat", "cInt", "cBool",
                                                 "p");
        expr = applySwizzle(expr, decodeSwizzle(token));
        expr = applySourceModifier(std::move(expr), decodeSourceModifier(token));
        return expr;
      };

      auto emitMaskedAssign = [&](const std::string& target, const std::string& value, u32 mask, bool scalar = false) {
        if (scalar) {
          out << "  " << target << " = " << value << ".x;\n";
          return;
        }
        const std::string finalValue = decodeDestModifier(instruction.operands.empty() ? 0u : instruction.operands[0]) ==
                                               1u
                                           ? "clamp(" + value + ", float4(0.0f), float4(1.0f))"
                                           : value;
        if (mask == 0xfu) {
          out << "  " << target << " = " << finalValue << ";\n";
        } else {
          out << "  " << target << " = dxmt9_merge(" << target << ", " << finalValue << ", " << mask << "u);\n";
        }
      };

      if (instruction.opcode == kD3DSIO_LABEL) {
        if (instruction.operands.empty()) {
          throw std::runtime_error("LABEL requires a label operand");
        }
        out << "  // label " << decodeLabelIndex(instruction.operands[0]) << "\n";
        continue;
      }
      if (instruction.opcode == kD3DSIO_CALL) {
        if (instruction.operands.empty()) {
          throw std::runtime_error("CALL requires a label operand");
        }
        out << "  // call label " << decodeLabelIndex(instruction.operands[0]) << "\n";
        out << "  do {\n";
        ++callDepth;
        continue;
      }
      if (instruction.opcode == kD3DSIO_RET) {
        if (callDepth > 0) {
          --callDepth;
          out << "  break;\n";
          out << "  } while (false);\n";
        } else {
          out << "  return out;\n";
        }
        continue;
      }
      if (instruction.opcode == kD3DSIO_IF) {
        if (instruction.operands.empty()) {
          throw std::runtime_error("IF requires a condition operand");
        }
        out << "  if ((" << readSrc(0) << ").x != 0.0f) {\n";
        controlStack.push_back(FlowBlock{instruction.opcode, false});
        continue;
      }
      if (instruction.opcode == kD3DSIO_ELSE) {
        if (controlStack.empty() || controlStack.back().opcode != kD3DSIO_IF || controlStack.back().sawElse) {
          throw std::runtime_error("ELSE without matching IF");
        }
        controlStack.back().sawElse = true;
        out << "  } else {\n";
        continue;
      }
      if (instruction.opcode == kD3DSIO_ENDIF) {
        if (controlStack.empty() || controlStack.back().opcode != kD3DSIO_IF) {
          throw std::runtime_error("ENDIF without matching IF");
        }
        controlStack.pop_back();
        out << "  }\n";
        continue;
      }
      if (instruction.opcode == kD3DSIO_LOOP || instruction.opcode == kD3DSIO_REP) {
        if (instruction.operands.empty()) {
          throw std::runtime_error("loop requires a count operand");
        }
        const auto loopIndex = instructionIndex;
        const auto countExpr = "max(0, int(round(" + readSrc(0) + ".x)))";
        if (instruction.opcode == kD3DSIO_LOOP) {
          out << "  for (int dxmt9_loop_" << loopIndex << " = 0, dxmt9_loopCount_" << loopIndex << " = "
              << countExpr << "; dxmt9_loop_" << loopIndex << " < dxmt9_loopCount_" << loopIndex
              << "; ++dxmt9_loop_" << loopIndex << ") {\n";
        } else {
          out << "  for (int dxmt9_rep_" << loopIndex << " = 0, dxmt9_repCount_" << loopIndex << " = " << countExpr
              << "; dxmt9_rep_" << loopIndex << " < dxmt9_repCount_" << loopIndex << "; ++dxmt9_rep_"
              << loopIndex << ") {\n";
        }
        controlStack.push_back(FlowBlock{instruction.opcode, false});
        continue;
      }
      if (instruction.opcode == kD3DSIO_ENDLOOP || instruction.opcode == kD3DSIO_ENDREP) {
        if (controlStack.empty() ||
            (instruction.opcode == kD3DSIO_ENDLOOP && controlStack.back().opcode != kD3DSIO_LOOP) ||
            (instruction.opcode == kD3DSIO_ENDREP && controlStack.back().opcode != kD3DSIO_REP)) {
          throw std::runtime_error("loop end without matching opener");
        }
        controlStack.pop_back();
        out << "  }\n";
        continue;
      }
      if (instruction.opcode == kD3DSIO_BREAK) {
        out << "  break;\n";
        continue;
      }
      if (instruction.opcode == kD3DSIO_BREAKP) {
        if (instruction.operands.empty()) {
          throw std::runtime_error("BREAKP requires a predicate operand");
        }
        out << "  if ((" << readSrc(0) << ").x != 0.0f) { break; }\n";
        continue;
      }

      switch (instruction.opcode) {
        case kD3DSIO_NOP:
          break;
        case kD3DSIO_MOV: {
        if (instruction.operands.size() < 2) {
          throw std::runtime_error("MOV requires 2 operands");
        }
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
          const auto dstMask = decodeWriteMask(instruction.operands[0]);
          const auto value = readSrc(1);
          switch (dst.kind) {
            case D3DRegisterKind::Temp:
              emitMaskedAssign("r[" + std::to_string(dst.index) + "]", value, dstMask);
              break;
            case D3DRegisterKind::RastOut:
              if (dst.index == 0) {
                emitMaskedAssign("outPosition", value, dstMask);
              } else if (dst.index == 1) {
                emitMaskedAssign("outFogFactor", value, dstMask, true);
              } else if (dst.index == 2) {
                emitMaskedAssign("outPointSize", value, dstMask, true);
              } else {
                throw std::runtime_error("unsupported raster output register");
              }
              break;
            case D3DRegisterKind::AttrOut:
              if (dst.index == 0) {
                emitMaskedAssign("outColor", value, dstMask);
              } else if (dst.index == 1) {
                emitMaskedAssign("outSecondaryColor", value, dstMask);
              } else {
                throw std::runtime_error("unsupported attribute output register");
              }
              break;
            case D3DRegisterKind::TexCoordOut:
              if (dst.index == 0) {
                emitMaskedAssign("outTexcoord0", value, dstMask);
              } else {
                throw std::runtime_error("unsupported texcoord output register");
              }
              break;
            case D3DRegisterKind::ColorOut:
              if (dst.index == 0) {
                emitMaskedAssign("outColor", value, dstMask);
              } else {
                throw std::runtime_error("unsupported color output register");
              }
              break;
            case D3DRegisterKind::ConstFloat:
              out << "  cFloat[" << dst.index << "] = " << value << ";\n";
              break;
            case D3DRegisterKind::ConstInt:
              out << "  cInt[" << dst.index << "] = int4(" << value << ");\n";
              break;
            case D3DRegisterKind::ConstBool:
              out << "  cBool[" << dst.index << "] = " << value << ".x != 0.0f ? 1u : 0u;\n";
              break;
          default:
            throw std::runtime_error("unsupported MOV destination");
        }
        break;
      }
      case kD3DSIO_MOVA: {
        if (instruction.operands.size() < 2) {
          throw std::runtime_error("MOVA requires 2 operands");
        }
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        const auto value = readSrc(1);
        switch (dst.kind) {
          case D3DRegisterKind::Address:
            out << "  a0 = int(round(" << value << ".x));\n";
            break;
          case D3DRegisterKind::Loop:
            out << "  aL = int(round(" << value << ".x));\n";
            break;
          default:
            throw std::runtime_error("MOVA requires an address register destination");
        }
        break;
      }
      case kD3DSIO_SETP: {
        if (instruction.operands.size() < 2) {
          throw std::runtime_error("SETP requires 2 operands");
        }
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        if (dst.kind != D3DRegisterKind::Predicate) {
          throw std::runtime_error("SETP requires a predicate register destination");
        }
        out << "  p[" << dst.index << "] = (" << readSrc(1) << ").x != 0.0f;\n";
        break;
      }
        case kD3DSIO_ADD:
        case kD3DSIO_SUB:
        case kD3DSIO_MUL:
        case kD3DSIO_MAD:
      case kD3DSIO_MIN:
      case kD3DSIO_MAX:
      case kD3DSIO_SLT:
      case kD3DSIO_SGE:
      case kD3DSIO_EXP:
      case kD3DSIO_LOG:
      case kD3DSIO_EXPP:
      case kD3DSIO_LOGP:
      case kD3DSIO_SINCOS:
      case kD3DSIO_M4x4:
      case kD3DSIO_M4x3:
      case kD3DSIO_M3x4:
      case kD3DSIO_M3x3:
      case kD3DSIO_M3x2:
      case kD3DSIO_RCP:
      case kD3DSIO_RSQ:
      case kD3DSIO_FRC:
        case kD3DSIO_LRP:
        case kD3DSIO_DP3:
        case kD3DSIO_DP4:
        case kD3DSIO_CND:
        case kD3DSIO_CMP:
        case kD3DSIO_DP2ADD:
        case kD3DSIO_POW:
        case kD3DSIO_CRS:
        case kD3DSIO_SGN:
        case kD3DSIO_ABS:
        case kD3DSIO_NRM:
        case kD3DSIO_TEX:
        case kD3DSIO_DSX:
        case kD3DSIO_DSY:
        case kD3DSIO_TEXLDD:
        case kD3DSIO_TEXLDL: {
          if (instruction.operands.size() < 2) {
            throw std::runtime_error("missing D3D destination or source operand");
          }
          const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
          const auto dstMask = decodeWriteMask(instruction.operands[0]);
          std::string value;
          switch (instruction.opcode) {
            case kD3DSIO_ADD:
              value = "(" + readSrc(1) + " + " + readSrc(2) + ")";
              break;
            case kD3DSIO_SUB:
              value = "(" + readSrc(1) + " - " + readSrc(2) + ")";
              break;
            case kD3DSIO_MUL:
              value = "(" + readSrc(1) + " * " + readSrc(2) + ")";
              break;
            case kD3DSIO_MAD:
              value = "(" + readSrc(1) + " * " + readSrc(2) + " + " + readSrc(3) + ")";
              break;
            case kD3DSIO_MIN:
              value = "min(" + readSrc(1) + ", " + readSrc(2) + ")";
              break;
            case kD3DSIO_MAX:
              value = "max(" + readSrc(1) + ", " + readSrc(2) + ")";
              break;
            case kD3DSIO_SLT:
              value = "select(float4(0.0f), float4(1.0f), (" + readSrc(1) + ") < (" + readSrc(2) + "))";
              break;
            case kD3DSIO_SGE:
              value = "select(float4(0.0f), float4(1.0f), (" + readSrc(1) + ") >= (" + readSrc(2) + "))";
              break;
            case kD3DSIO_EXP:
            case kD3DSIO_EXPP:
              value = "float4(exp2(" + readSrc(1) + "))";
              break;
            case kD3DSIO_LOG:
            case kD3DSIO_LOGP:
              value = "float4(log2(max(" + readSrc(1) + ", float4(1.0e-8f))))";
              break;
            case kD3DSIO_SINCOS:
              value = "float4(sin(" + readSrc(1) + "), cos(" + readSrc(1) + "), 0.0f, 0.0f)";
              break;
            case kD3DSIO_M4x4: {
              const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
              if (base.kind != D3DRegisterKind::ConstFloat) {
                throw std::runtime_error("M4x4 requires a float constant matrix base");
              }
              const auto src = readSrc(1);
              value = "float4(dot(" + src + ", cFloat[" + std::to_string(base.index + 0) + "]), dot(" + src +
                      ", cFloat[" + std::to_string(base.index + 1) + "]), dot(" + src + ", cFloat[" +
                      std::to_string(base.index + 2) + "]), dot(" + src + ", cFloat[" +
                      std::to_string(base.index + 3) + "]))";
              break;
            }
            case kD3DSIO_M4x3: {
              const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
              if (base.kind != D3DRegisterKind::ConstFloat) {
                throw std::runtime_error("M4x3 requires a float constant matrix base");
              }
              const auto src = readSrc(1);
              value = "float4(dot(" + src + ", cFloat[" + std::to_string(base.index + 0) + "]), dot(" + src +
                      ", cFloat[" + std::to_string(base.index + 1) + "]), dot(" + src + ", cFloat[" +
                      std::to_string(base.index + 2) + "]), 0.0f)";
              break;
            }
            case kD3DSIO_M3x4: {
              const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
              if (base.kind != D3DRegisterKind::ConstFloat) {
                throw std::runtime_error("M3x4 requires a float constant matrix base");
              }
              const auto src = readSrc(1);
              value = "float4(dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 0) +
                      "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 1) +
                      "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 2) +
                      "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 3) + "].xyz))";
              break;
            }
            case kD3DSIO_M3x3: {
              const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
              if (base.kind != D3DRegisterKind::ConstFloat) {
                throw std::runtime_error("M3x3 requires a float constant matrix base");
              }
              const auto src = readSrc(1);
              value = "float4(dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 0) +
                      "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 1) +
                      "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 2) +
                      "].xyz), 0.0f)";
              break;
            }
            case kD3DSIO_M3x2: {
              const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
              if (base.kind != D3DRegisterKind::ConstFloat) {
                throw std::runtime_error("M3x2 requires a float constant matrix base");
              }
              const auto src = readSrc(1);
              value = "float4(dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 0) +
                      "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 1) +
                      "].xyz), 0.0f, 0.0f)";
              break;
            }
            case kD3DSIO_RCP:
              value = "float4(1.0f) / max(" + readSrc(1) + ", float4(1.0e-8f))";
              break;
            case kD3DSIO_RSQ:
              value = "rsqrt(max(" + readSrc(1) + ", float4(1.0e-8f)))";
              break;
            case kD3DSIO_FRC:
              value = "fract(" + readSrc(1) + ")";
              break;
            case kD3DSIO_LRP:
              value = "mix(" + readSrc(3) + ", " + readSrc(2) + ", " + readSrc(1) + ")";
              break;
            case kD3DSIO_DP3:
              value = "float4(dot((" + readSrc(1) + ").xyz, (" + readSrc(2) + ").xyz))";
              break;
            case kD3DSIO_DP4:
              value = "float4(dot(" + readSrc(1) + ", " + readSrc(2) + "))";
              break;
            case kD3DSIO_CND:
              value = "select(" + readSrc(3) + ", " + readSrc(2) + ", " + readSrc(1) + " > float4(0.5f))";
              break;
            case kD3DSIO_CMP:
              value = "select(" + readSrc(3) + ", " + readSrc(2) + ", " + readSrc(1) + " >= float4(0.0f))";
              break;
            case kD3DSIO_DP2ADD:
              value = "float4(dot((" + readSrc(1) + ").xy, (" + readSrc(2) + ").xy) + (" + readSrc(3) + ").x)";
              break;
            case kD3DSIO_POW:
              value = "pow(" + readSrc(1) + ", " + readSrc(2) + ")";
              break;
            case kD3DSIO_CRS:
              value = "float4(cross((" + readSrc(1) + ").xyz, (" + readSrc(2) + ").xyz), 0.0f)";
              break;
            case kD3DSIO_SGN:
              value = "sign(" + readSrc(1) + ")";
              break;
            case kD3DSIO_ABS:
              value = "abs(" + readSrc(1) + ")";
              break;
            case kD3DSIO_NRM:
              value = "float4(normalize((" + readSrc(1) + ").xyz), 0.0f)";
              break;
            case kD3DSIO_DSX:
              value = "dfdx(" + readSrc(1) + ")";
              break;
            case kD3DSIO_DSY:
              value = "dfdy(" + readSrc(1) + ")";
              break;
            case kD3DSIO_TEXLDD:
              value = "tex0.sample(samp0, " + readSrc(1) + ".xy)";
              break;
            case kD3DSIO_TEXLDL:
              value = "tex0.sample(samp0, " + readSrc(1) + ".xy, level(" + readSrc(1) + ".w))";
              break;
            case kD3DSIO_TEX:
              value = "tex0.sample(samp0, " + readSrc(1) + ".xy)";
              break;
            default:
              throw std::runtime_error("unsupported arithmetic opcode");
          }
          if (decodeDestModifier(instruction.operands[0]) == 1u) {
            value = "clamp(" + value + ", float4(0.0f), float4(1.0f))";
          }
          switch (dst.kind) {
            case D3DRegisterKind::Temp:
              emitMaskedAssign("r[" + std::to_string(dst.index) + "]", value, dstMask);
              break;
            case D3DRegisterKind::RastOut:
              if (dst.index == 0) {
                emitMaskedAssign("outPosition", value, dstMask);
              } else if (dst.index == 1) {
                emitMaskedAssign("outFogFactor", value, dstMask, true);
              } else if (dst.index == 2) {
                emitMaskedAssign("outPointSize", value, dstMask, true);
              } else {
                throw std::runtime_error("unsupported raster output register");
              }
              break;
            case D3DRegisterKind::AttrOut:
              if (dst.index == 0) {
                emitMaskedAssign("outColor", value, dstMask);
              } else if (dst.index == 1) {
                emitMaskedAssign("outSecondaryColor", value, dstMask);
              } else {
                throw std::runtime_error("unsupported attribute output register");
              }
              break;
            case D3DRegisterKind::TexCoordOut:
              if (dst.index == 0) {
                emitMaskedAssign("outTexcoord0", value, dstMask);
              } else {
                throw std::runtime_error("unsupported texcoord output register");
              }
              break;
            case D3DRegisterKind::ColorOut:
              if (dst.index == 0) {
                emitMaskedAssign("outColor", value, dstMask);
              } else {
                throw std::runtime_error("unsupported color output register");
              }
              break;
            case D3DRegisterKind::DepthOut:
              throw std::runtime_error("depth output is not supported yet");
            default:
              throw std::runtime_error("unsupported arithmetic destination");
          }
          break;
        }
        case kD3DSIO_DEF: {
          if (instruction.operands.size() < 5) {
            throw std::runtime_error("DEF requires 5 operands");
          }
          const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
          const auto values = std::array<f32, 4>{std::bit_cast<f32>(instruction.operands[1]),
                                                 std::bit_cast<f32>(instruction.operands[2]),
                                                 std::bit_cast<f32>(instruction.operands[3]),
                                                 std::bit_cast<f32>(instruction.operands[4])};
          if (dst.kind != D3DRegisterKind::ConstFloat) {
            std::ostringstream message;
            message << "DEF requires a float constant destination"
                    << " token=0x" << std::hex << instruction.operands[0]
                    << " regType=" << std::dec << decodeRegisterType(instruction.operands[0])
                    << " regIndex=" << dst.index
                    << " kind=" << static_cast<u32>(dst.kind);
            throw std::runtime_error(message.str());
          }
          out << "  cFloat[" << dst.index << "] = " << formatFloatVec4(values) << ";\n";
          break;
        }
        case kD3DSIO_DEFI: {
          if (instruction.operands.size() < 5) {
            throw std::runtime_error("DEFI requires 5 operands");
          }
          const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
          const auto values = std::array<i32, 4>{static_cast<i32>(instruction.operands[1]),
                                                 static_cast<i32>(instruction.operands[2]),
                                                 static_cast<i32>(instruction.operands[3]),
                                                 static_cast<i32>(instruction.operands[4])};
          if (dst.kind != D3DRegisterKind::ConstInt) {
            throw std::runtime_error("DEFI requires an integer constant destination");
          }
          out << "  cInt[" << dst.index << "] = " << formatIntVec4(values) << ";\n";
          break;
        }
        case kD3DSIO_DEFB: {
          if (instruction.operands.size() < 2) {
            throw std::runtime_error("DEFB requires 2 operands");
          }
          const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
          if (dst.kind != D3DRegisterKind::ConstBool) {
            throw std::runtime_error("DEFB requires a boolean constant destination");
          }
          out << "  cBool[" << dst.index << "] = " << (instruction.operands[1] != 0u ? "1u" : "0u") << ";\n";
          break;
        }
        case kD3DSIO_DCL:
          // No-op for now: DCL informs semantics, but the current translator maps outputs by register class.
          break;
        case kD3DSIO_BEM:
        case kD3DSIO_TEXDEPTH:
        case kD3DSIO_TEXREG2RGB:
        case kD3DSIO_TEXDP3TEX:
        case kD3DSIO_TEXM3x2DEPTH:
        case kD3DSIO_TEXDP3:
        case kD3DSIO_TEXM3x3:
          // Texture instructions are lowered through the supported TEXLD-style sample path above when present.
          break;
        default:
          throw std::runtime_error("unsupported D3D opcode: " + opcodeName(instruction.opcode));
      }
    }
    if (!controlStack.empty()) {
      throw std::runtime_error("unbalanced D3D control flow");
    }
    if (callDepth != 0) {
      throw std::runtime_error("unbalanced D3D CALL/RET");
    }

    out << "  out.position = outPosition;\n";
    out << "  out.color = outColor;\n";
    out << "  out.secondaryColor = outSecondaryColor;\n";
    out << "  out.texcoord0 = outTexcoord0;\n";
    for (size_t i = 1; i < kMaxTextureStages; ++i) {
      out << "  out.texcoord" << i << " = float4(0.0f, 0.0f, 0.0f, 1.0f);\n";
    }
    out << "  out.fogFactor = outFogFactor;\n";
    out << "  out.pointSize = outPointSize;\n";
    out << "  out.position.xy += uniforms.halfPixelFixup * out.position.w;\n";
    if (desc.clipPlaneMask != 0) {
      out << "  for (uint i = 0; i < 6; ++i) {\n";
      out << "    if ((uniforms.clipPlaneMask & (1u << i)) != 0u) {\n";
      out << "      out.clipDistance[i] = dot(uniforms.clipPlanes[i], out.position);\n";
      out << "    }\n";
      out << "  }\n";
    }
    out << "  return out;\n";
    out << "}\n";
    out << "// decoded d3d hash " << module.hash << "\n";
    return out.str();
  }

  const bool textured = module.usesTexture || desc.textures[0].handle != Handle{};
  const bool traceShaderInputs = [] {
    const char* env = std::getenv("DXMT_TRACE_SHADER_INPUTS");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  if (traceShaderInputs) {
    std::ostringstream trace;
    trace << "[dxmt9-shader] pixel inputs";
    for (const auto& instruction : module.instructions) {
      if (instruction.opcode != kD3DSIO_DCL || instruction.operands.empty()) {
        continue;
      }
      trace << " dcl(" << decodeOperandToken(instruction.operands[0], module.stage, true)
            << ",type=" << decodeRegisterType(instruction.operands[0])
            << ",tok=0x" << std::hex << instruction.operands[0] << std::dec << ")";
    }
    std::fprintf(stderr, "%s\n", trace.str().c_str());
    std::fflush(stderr);
  }
  if (textured) {
    out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], constant DrawUniforms& uniforms [[buffer(0)]], ";
    out << "texture2d<float> tex0 [[texture(0)]], sampler samp0 [[sampler(0)]]) {\n";
  } else {
    out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], constant DrawUniforms& uniforms [[buffer(0)]]) {\n";
  }
  out << "  float4 color = float4(1.0f);\n";
  out << "  float4 outColor = float4(1.0f);\n";
  out << "  float4 outSecondaryColor = float4(0.0f);\n";
  out << "  float4 outTexcoord0 = float4(0.0f);\n";
  out << "  float4 outPosition = float4(0.0f);\n";
  out << "  float outFogFactor = 1.0f;\n";
  out << "  float outPointSize = 1.0f;\n";
  out << "  int a0 = 0;\n";
  out << "  int aL = 0;\n";
  out << "  float4 r[32];\n";
  out << "  float4 cFloat[" << kMaxPixelConstants << "];\n";
  out << "  int4 cInt[" << kMaxIntegerConstants << "];\n";
  out << "  uint cBool[" << kMaxBoolConstants << "];\n";
  out << "  bool p[" << kMaxBoolConstants << "];\n";
  out << "  for (uint i = 0; i < " << kMaxPixelConstants << "; ++i) { cFloat[i] = uniforms.psFloatConst[i]; }\n";
  out << "  for (uint i = 0; i < " << kMaxIntegerConstants << "; ++i) { cInt[i] = uniforms.psIntConst[i]; }\n";
  out << "  for (uint i = 0; i < " << kMaxBoolConstants << "; ++i) { cBool[i] = uniforms.psBoolConst[i]; }\n";
  out << "  for (uint i = 0; i < " << kMaxBoolConstants << "; ++i) { p[i] = false; }\n";
    std::vector<FlowBlock> controlStack;
    size_t callDepth = 0;
    for (size_t instructionIndex = 0; instructionIndex < module.instructions.size(); ++instructionIndex) {
      const auto& instruction = module.instructions[instructionIndex];
    if (instruction.opcode == kD3DSIO_COMMENT || instruction.opcode == kD3DSIO_PHASE) {
      continue;
    }
    out << "  // " << opcodeName(instruction.opcode);
    for (size_t i = 0; i < instruction.operands.size(); ++i) {
      const bool destination = i == 0;
      out << (i == 0 ? " " : ", ");
      if (instruction.opcode == kD3DSIO_DEF && i > 0) {
        out << formatFloatLiteral(std::bit_cast<f32>(instruction.operands[i]));
      } else if (instruction.opcode == kD3DSIO_DEFI && i > 0) {
        out << static_cast<i32>(instruction.operands[i]);
      } else if (instruction.opcode == kD3DSIO_DEFB && i > 0) {
        out << (instruction.operands[i] != 0u ? "true" : "false");
      } else if (instruction.opcode == kD3DSIO_LABEL || instruction.opcode == kD3DSIO_CALL) {
        out << "label" << decodeLabelIndex(instruction.operands[i]);
      } else {
        out << decodeOperandToken(instruction.operands[i], module.stage, destination);
      }
    }
    out << "\n";

    auto readSrc = [&](size_t index) {
      if (index >= instruction.operands.size()) {
        std::ostringstream message;
        message << "missing D3D source operand"
                << " opcode=" << opcodeName(instruction.opcode)
                << " requestedIndex=" << index
                << " operandCount=" << instruction.operands.size();
        throw std::runtime_error(message.str());
      }
      const auto token = instruction.operands[index];
      const auto reg = decodeRegisterRef(token, module.stage);
      std::string expr;
      if (reg.kind == D3DRegisterKind::Input) {
        expr = readPixelInputExpression(token, "in");
      } else {
        expr = readOperandExpression(instruction, reg, "float4(0.0f)", "in", false, "outPosition",
                                     "outColor", "outSecondaryColor", "outTexcoord0", "outFogFactor",
                                     "outPointSize", "r", "cFloat", "cInt", "cBool", "p");
      }
      expr = applySwizzle(expr, decodeSwizzle(token));
      expr = applySourceModifier(std::move(expr), decodeSourceModifier(token));
      return expr;
    };

    auto emitMaskedAssign = [&](const std::string& target, const std::string& value, u32 mask, bool scalar = false) {
      if (scalar) {
        out << "  " << target << " = " << value << ".x;\n";
        return;
      }
      const std::string finalValue = decodeDestModifier(instruction.operands.empty() ? 0u : instruction.operands[0]) ==
                                             1u
                                         ? "clamp(" + value + ", float4(0.0f), float4(1.0f))"
                                         : value;
      if (mask == 0xfu) {
        out << "  " << target << " = " << finalValue << ";\n";
      } else {
        out << "  " << target << " = dxmt9_merge(" << target << ", " << finalValue << ", " << mask << "u);\n";
      }
    };

    if (instruction.opcode == kD3DSIO_LABEL) {
      if (instruction.operands.empty()) {
        throw std::runtime_error("LABEL requires a label operand");
      }
      out << "  // label " << decodeLabelIndex(instruction.operands[0]) << "\n";
      continue;
    }
    if (instruction.opcode == kD3DSIO_CALL) {
      if (instruction.operands.empty()) {
        throw std::runtime_error("CALL requires a label operand");
      }
      out << "  // call label " << decodeLabelIndex(instruction.operands[0]) << "\n";
      out << "  do {\n";
      ++callDepth;
      continue;
    }
    if (instruction.opcode == kD3DSIO_RET) {
      if (callDepth > 0) {
        --callDepth;
        out << "  break;\n";
        out << "  } while (false);\n";
      } else {
        out << "  return color;\n";
      }
      continue;
    }
    if (instruction.opcode == kD3DSIO_IF) {
      if (instruction.operands.empty()) {
        throw std::runtime_error("IF requires a condition operand");
      }
      out << "  if ((" << readSrc(0) << ").x != 0.0f) {\n";
      controlStack.push_back(FlowBlock{instruction.opcode, false});
      continue;
    }
    if (instruction.opcode == kD3DSIO_ELSE) {
      if (controlStack.empty() || controlStack.back().opcode != kD3DSIO_IF || controlStack.back().sawElse) {
        throw std::runtime_error("ELSE without matching IF");
      }
      controlStack.back().sawElse = true;
      out << "  } else {\n";
      continue;
    }
    if (instruction.opcode == kD3DSIO_ENDIF) {
      if (controlStack.empty() || controlStack.back().opcode != kD3DSIO_IF) {
        throw std::runtime_error("ENDIF without matching IF");
      }
      controlStack.pop_back();
      out << "  }\n";
      continue;
    }
    if (instruction.opcode == kD3DSIO_LOOP || instruction.opcode == kD3DSIO_REP) {
      if (instruction.operands.empty()) {
        throw std::runtime_error("loop requires a count operand");
      }
      const auto loopIndex = instructionIndex;
      const auto countExpr = "max(0, int(round(" + readSrc(0) + ".x)))";
      if (instruction.opcode == kD3DSIO_LOOP) {
        out << "  for (int dxmt9_loop_" << loopIndex << " = 0, dxmt9_loopCount_" << loopIndex << " = " << countExpr
            << "; dxmt9_loop_" << loopIndex << " < dxmt9_loopCount_" << loopIndex << "; ++dxmt9_loop_"
            << loopIndex << ") {\n";
      } else {
        out << "  for (int dxmt9_rep_" << loopIndex << " = 0, dxmt9_repCount_" << loopIndex << " = " << countExpr
            << "; dxmt9_rep_" << loopIndex << " < dxmt9_repCount_" << loopIndex << "; ++dxmt9_rep_"
            << loopIndex << ") {\n";
      }
      controlStack.push_back(FlowBlock{instruction.opcode, false});
      continue;
    }
    if (instruction.opcode == kD3DSIO_ENDLOOP || instruction.opcode == kD3DSIO_ENDREP) {
      if (controlStack.empty() ||
          (instruction.opcode == kD3DSIO_ENDLOOP && controlStack.back().opcode != kD3DSIO_LOOP) ||
          (instruction.opcode == kD3DSIO_ENDREP && controlStack.back().opcode != kD3DSIO_REP)) {
        throw std::runtime_error("loop end without matching opener");
      }
      controlStack.pop_back();
      out << "  }\n";
      continue;
    }
    if (instruction.opcode == kD3DSIO_BREAK) {
      out << "  break;\n";
      continue;
    }
    if (instruction.opcode == kD3DSIO_BREAKP) {
      if (instruction.operands.empty()) {
        throw std::runtime_error("BREAKP requires a predicate operand");
      }
      out << "  if ((" << readSrc(0) << ").x != 0.0f) { break; }\n";
      continue;
    }

    switch (instruction.opcode) {
      case kD3DSIO_NOP:
        break;
      case kD3DSIO_DEF: {
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        const auto values = std::array<f32, 4>{std::bit_cast<f32>(instruction.operands[1]),
                                               std::bit_cast<f32>(instruction.operands[2]),
                                               std::bit_cast<f32>(instruction.operands[3]),
                                               std::bit_cast<f32>(instruction.operands[4])};
        if (dst.kind != D3DRegisterKind::ConstFloat) {
          std::ostringstream message;
          message << "DEF requires a float constant destination"
                  << " token=0x" << std::hex << instruction.operands[0]
                  << " regType=" << std::dec << decodeRegisterType(instruction.operands[0])
                  << " regIndex=" << dst.index
                  << " kind=" << static_cast<u32>(dst.kind);
          throw std::runtime_error(message.str());
        }
        out << "  cFloat[" << dst.index << "] = " << formatFloatVec4(values) << ";\n";
        break;
      }
      case kD3DSIO_DEFI: {
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        const auto values = std::array<i32, 4>{static_cast<i32>(instruction.operands[1]),
                                               static_cast<i32>(instruction.operands[2]),
                                               static_cast<i32>(instruction.operands[3]),
                                               static_cast<i32>(instruction.operands[4])};
        if (dst.kind != D3DRegisterKind::ConstInt) {
          throw std::runtime_error("DEFI requires an integer constant destination");
        }
        out << "  cInt[" << dst.index << "] = " << formatIntVec4(values) << ";\n";
        break;
      }
      case kD3DSIO_DEFB: {
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        if (dst.kind != D3DRegisterKind::ConstBool) {
          throw std::runtime_error("DEFB requires a boolean constant destination");
        }
        out << "  cBool[" << dst.index << "] = " << (instruction.operands[1] != 0u ? "1u" : "0u") << ";\n";
        break;
      }
      case kD3DSIO_MOV: {
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        const auto dstMask = decodeWriteMask(instruction.operands[0]);
        const auto value = readSrc(1);
        switch (dst.kind) {
          case D3DRegisterKind::Temp:
            emitMaskedAssign("r[" + std::to_string(dst.index) + "]", value, dstMask);
            break;
          case D3DRegisterKind::ColorOut:
            if (dst.index == 0) {
              emitMaskedAssign("outColor", value, dstMask);
            } else {
              throw std::runtime_error("unsupported color output register");
            }
            break;
          case D3DRegisterKind::TexCoordOut:
            if (dst.index == 0) {
              emitMaskedAssign("outTexcoord0", value, dstMask);
            } else {
              throw std::runtime_error("unsupported texcoord output register");
            }
            break;
          case D3DRegisterKind::ConstFloat:
            out << "  cFloat[" << dst.index << "] = " << value << ";\n";
            break;
          case D3DRegisterKind::ConstInt:
            out << "  cInt[" << dst.index << "] = int4(" << value << ");\n";
            break;
          case D3DRegisterKind::ConstBool:
            out << "  cBool[" << dst.index << "] = " << value << ".x != 0.0f ? 1u : 0u;\n";
            break;
          default:
            throw std::runtime_error("unsupported MOV destination");
        }
        break;
      }
      case kD3DSIO_MOVA: {
        if (instruction.operands.size() < 2) {
          throw std::runtime_error("MOVA requires 2 operands");
        }
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        const auto value = readSrc(1);
        switch (dst.kind) {
          case D3DRegisterKind::Address:
            out << "  a0 = int(round(" << value << ".x));\n";
            break;
          case D3DRegisterKind::Loop:
            out << "  aL = int(round(" << value << ".x));\n";
            break;
          default:
            throw std::runtime_error("MOVA requires an address register destination");
        }
        break;
      }
      case kD3DSIO_SETP: {
        if (instruction.operands.size() < 2) {
          throw std::runtime_error("SETP requires 2 operands");
        }
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        if (dst.kind != D3DRegisterKind::Predicate) {
          throw std::runtime_error("SETP requires a predicate register destination");
        }
        out << "  p[" << dst.index << "] = (" << readSrc(1) << ").x != 0.0f;\n";
        break;
      }
      case kD3DSIO_ADD:
      case kD3DSIO_SUB:
      case kD3DSIO_MUL:
      case kD3DSIO_MAD:
      case kD3DSIO_MIN:
      case kD3DSIO_MAX:
      case kD3DSIO_SLT:
      case kD3DSIO_SGE:
      case kD3DSIO_EXP:
      case kD3DSIO_LOG:
      case kD3DSIO_EXPP:
      case kD3DSIO_LOGP:
      case kD3DSIO_SINCOS:
      case kD3DSIO_M4x4:
      case kD3DSIO_M4x3:
      case kD3DSIO_M3x4:
      case kD3DSIO_M3x3:
      case kD3DSIO_M3x2:
      case kD3DSIO_RCP:
      case kD3DSIO_RSQ:
      case kD3DSIO_FRC:
      case kD3DSIO_LRP:
      case kD3DSIO_DP3:
      case kD3DSIO_DP4:
      case kD3DSIO_CND:
      case kD3DSIO_CMP:
      case kD3DSIO_DP2ADD:
      case kD3DSIO_POW:
      case kD3DSIO_CRS:
      case kD3DSIO_SGN:
      case kD3DSIO_ABS:
      case kD3DSIO_NRM:
      case kD3DSIO_TEX:
      case kD3DSIO_DSX:
      case kD3DSIO_DSY:
      case kD3DSIO_TEXLDD:
      case kD3DSIO_TEXLDL: {
        const auto dst = decodeRegisterRef(instruction.operands[0], module.stage);
        const auto dstMask = decodeWriteMask(instruction.operands[0]);
        std::string value;
        switch (instruction.opcode) {
          case kD3DSIO_ADD:
            value = "(" + readSrc(1) + " + " + readSrc(2) + ")";
            break;
          case kD3DSIO_SUB:
            value = "(" + readSrc(1) + " - " + readSrc(2) + ")";
            break;
          case kD3DSIO_MUL:
            value = "(" + readSrc(1) + " * " + readSrc(2) + ")";
            break;
          case kD3DSIO_MAD:
            value = "(" + readSrc(1) + " * " + readSrc(2) + " + " + readSrc(3) + ")";
            break;
          case kD3DSIO_MIN:
            value = "min(" + readSrc(1) + ", " + readSrc(2) + ")";
            break;
          case kD3DSIO_MAX:
            value = "max(" + readSrc(1) + ", " + readSrc(2) + ")";
            break;
          case kD3DSIO_SLT:
            value = "select(float4(0.0f), float4(1.0f), (" + readSrc(1) + ") < (" + readSrc(2) + "))";
            break;
          case kD3DSIO_SGE:
            value = "select(float4(0.0f), float4(1.0f), (" + readSrc(1) + ") >= (" + readSrc(2) + "))";
            break;
          case kD3DSIO_EXP:
          case kD3DSIO_EXPP:
            value = "float4(exp2(" + readSrc(1) + "))";
            break;
          case kD3DSIO_LOG:
          case kD3DSIO_LOGP:
            value = "float4(log2(max(" + readSrc(1) + ", float4(1.0e-8f))))";
            break;
          case kD3DSIO_SINCOS:
            value = "float4(sin(" + readSrc(1) + "), cos(" + readSrc(1) + "), 0.0f, 0.0f)";
            break;
          case kD3DSIO_M4x4: {
            const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
            if (base.kind != D3DRegisterKind::ConstFloat) {
              throw std::runtime_error("M4x4 requires a float constant matrix base");
            }
            const auto src = readSrc(1);
            value = "float4(dot(" + src + ", cFloat[" + std::to_string(base.index + 0) + "]), dot(" + src +
                    ", cFloat[" + std::to_string(base.index + 1) + "]), dot(" + src + ", cFloat[" +
                    std::to_string(base.index + 2) + "]), dot(" + src + ", cFloat[" +
                    std::to_string(base.index + 3) + "]))";
            break;
          }
          case kD3DSIO_M4x3: {
            const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
            if (base.kind != D3DRegisterKind::ConstFloat) {
              throw std::runtime_error("M4x3 requires a float constant matrix base");
            }
            const auto src = readSrc(1);
            value = "float4(dot(" + src + ", cFloat[" + std::to_string(base.index + 0) + "]), dot(" + src +
                    ", cFloat[" + std::to_string(base.index + 1) + "]), dot(" + src + ", cFloat[" +
                    std::to_string(base.index + 2) + "]), 0.0f)";
            break;
          }
          case kD3DSIO_M3x4: {
            const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
            if (base.kind != D3DRegisterKind::ConstFloat) {
              throw std::runtime_error("M3x4 requires a float constant matrix base");
            }
            const auto src = readSrc(1);
            value = "float4(dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 0) +
                    "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 1) +
                    "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 2) +
                    "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 3) + "].xyz))";
            break;
          }
          case kD3DSIO_M3x3: {
            const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
            if (base.kind != D3DRegisterKind::ConstFloat) {
              throw std::runtime_error("M3x3 requires a float constant matrix base");
            }
            const auto src = readSrc(1);
            value = "float4(dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 0) +
                    "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 1) +
                    "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 2) +
                    "].xyz), 0.0f)";
            break;
          }
          case kD3DSIO_M3x2: {
            const auto base = decodeRegisterRef(instruction.operands[2], module.stage);
            if (base.kind != D3DRegisterKind::ConstFloat) {
              throw std::runtime_error("M3x2 requires a float constant matrix base");
            }
            const auto src = readSrc(1);
            value = "float4(dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 0) +
                    "].xyz), dot((" + src + ").xyz, cFloat[" + std::to_string(base.index + 1) +
                    "].xyz), 0.0f, 0.0f)";
            break;
          }
          case kD3DSIO_RCP:
            value = "float4(1.0f) / max(" + readSrc(1) + ", float4(1.0e-8f))";
            break;
          case kD3DSIO_RSQ:
            value = "rsqrt(max(" + readSrc(1) + ", float4(1.0e-8f)))";
            break;
          case kD3DSIO_FRC:
            value = "fract(" + readSrc(1) + ")";
            break;
          case kD3DSIO_LRP:
            value = "mix(" + readSrc(3) + ", " + readSrc(2) + ", " + readSrc(1) + ")";
            break;
          case kD3DSIO_DP3:
            value = "float4(dot((" + readSrc(1) + ").xyz, (" + readSrc(2) + ").xyz))";
            break;
          case kD3DSIO_DP4:
            value = "float4(dot(" + readSrc(1) + ", " + readSrc(2) + "))";
            break;
          case kD3DSIO_CND:
            value = "select(" + readSrc(3) + ", " + readSrc(2) + ", " + readSrc(1) + " > float4(0.5f))";
            break;
          case kD3DSIO_CMP:
            value = "select(" + readSrc(3) + ", " + readSrc(2) + ", " + readSrc(1) + " >= float4(0.0f))";
            break;
          case kD3DSIO_DP2ADD:
            value = "float4(dot((" + readSrc(1) + ").xy, (" + readSrc(2) + ").xy) + (" + readSrc(3) + ").x)";
            break;
          case kD3DSIO_POW:
            value = "pow(" + readSrc(1) + ", " + readSrc(2) + ")";
            break;
          case kD3DSIO_CRS:
            value = "float4(cross((" + readSrc(1) + ").xyz, (" + readSrc(2) + ").xyz), 0.0f)";
            break;
          case kD3DSIO_SGN:
            value = "sign(" + readSrc(1) + ")";
            break;
          case kD3DSIO_ABS:
            value = "abs(" + readSrc(1) + ")";
            break;
          case kD3DSIO_NRM:
            value = "float4(normalize((" + readSrc(1) + ").xyz), 0.0f)";
            break;
          case kD3DSIO_TEX:
            value = "tex0.sample(samp0, " + readSrc(1) + ".xy)";
            break;
          case kD3DSIO_DSX:
            value = "dfdx(" + readSrc(1) + ")";
            break;
          case kD3DSIO_DSY:
            value = "dfdy(" + readSrc(1) + ")";
            break;
          case kD3DSIO_TEXLDD:
            value = "tex0.sample(samp0, " + readSrc(1) + ".xy)";
            break;
          case kD3DSIO_TEXLDL:
            value = "tex0.sample(samp0, " + readSrc(1) + ".xy, level(" + readSrc(1) + ".w))";
            break;
          default:
            throw std::runtime_error("unsupported arithmetic opcode");
        }
        if (decodeDestModifier(instruction.operands[0]) == 1u) {
          value = "clamp(" + value + ", float4(0.0f), float4(1.0f))";
        }
        switch (dst.kind) {
          case D3DRegisterKind::Temp:
            emitMaskedAssign("r[" + std::to_string(dst.index) + "]", value, dstMask);
            break;
          case D3DRegisterKind::ColorOut:
            if (dst.index == 0) {
              emitMaskedAssign("outColor", value, dstMask);
            } else {
              throw std::runtime_error("unsupported color output register");
            }
            break;
          case D3DRegisterKind::TexCoordOut:
            if (dst.index == 0) {
              emitMaskedAssign("outTexcoord0", value, dstMask);
            } else {
              throw std::runtime_error("unsupported texcoord output register");
            }
            break;
          case D3DRegisterKind::ConstFloat:
            out << "  cFloat[" << dst.index << "] = " << value << ";\n";
            break;
          case D3DRegisterKind::ConstInt:
            out << "  cInt[" << dst.index << "] = int4(" << value << ");\n";
            break;
          case D3DRegisterKind::ConstBool:
            out << "  cBool[" << dst.index << "] = " << value << ".x != 0.0f ? 1u : 0u;\n";
            break;
          default:
            throw std::runtime_error("unsupported arithmetic destination");
        }
        break;
      }
      case kD3DSIO_DCL:
        break;
      case kD3DSIO_BEM:
      case kD3DSIO_TEXDEPTH:
      case kD3DSIO_TEXREG2RGB:
      case kD3DSIO_TEXDP3TEX:
      case kD3DSIO_TEXM3x2DEPTH:
      case kD3DSIO_TEXDP3:
      case kD3DSIO_TEXM3x3:
        break;
      default:
        throw std::runtime_error("unsupported D3D opcode: " + opcodeName(instruction.opcode));
    }
    }
    if (!controlStack.empty()) {
      throw std::runtime_error("unbalanced D3D control flow");
    }
    if (callDepth != 0) {
      throw std::runtime_error("unbalanced D3D CALL/RET");
    }
  out << "  color = outColor;\n";
  out << "  if (uniforms.alphaTestEnable != 0u) {\n";
  out << "    bool pass = true;\n";
  out << "    switch (uniforms.alphaTestFunc) {\n";
  out << "      case 2u: pass = color.a < uniforms.alphaRef; break;\n";
  out << "      case 3u: pass = color.a == uniforms.alphaRef; break;\n";
  out << "      case 4u: pass = color.a <= uniforms.alphaRef; break;\n";
  out << "      case 5u: pass = color.a > uniforms.alphaRef; break;\n";
  out << "      case 6u: pass = color.a != uniforms.alphaRef; break;\n";
  out << "      case 7u: pass = color.a >= uniforms.alphaRef; break;\n";
  out << "      case 8u: pass = true; break;\n";
  out << "      default: pass = true; break;\n";
  out << "    }\n";
  out << "    if (!pass) {\n";
  out << "      discard_fragment();\n";
  out << "    }\n";
  out << "  }\n";
  out << "  return color;\n";
  out << "}\n";
  out << "// decoded d3d hash " << module.hash << "\n";
  return out.str();
}

std::string makeTranslatedVertexSource(const ShaderRef& shader, const DrawDesc& desc) {
  return translateSpirvToMsl(translateD3DBytecodeToSpirv(shader, true, desc), desc, true);
}

std::string makeTranslatedFragmentSource(const ShaderRef& shader, const DrawDesc& desc) {
  return translateSpirvToMsl(translateD3DBytecodeToSpirv(shader, false, desc), desc, false);
}

std::string makeFfpVertexSource(const FfpVertexKey& key, const DrawDesc& desc) {
  std::ostringstream out;
  const auto layout = decodeFixedFunctionVertexLayout(desc);
  constexpr u32 kTciIndexMask = 0x0000ffffu;
  constexpr u32 kTciGenMask = 0xffff0000u;
  constexpr u32 kTciCameraSpacePosition = 0x00020000u;
  const auto emitStageTexcoords = [&](std::ostringstream& shader, const char* positionExpr) {
    for (size_t stage = 0; stage < kMaxTextureStages; ++stage) {
      const u32 texCoordIndex = key.texCoordGen[stage] & kTciIndexMask;
      const u32 texCoordGen = key.texCoordGen[stage] & kTciGenMask;
      shader << "  float4 dxmt9_texcoord" << stage << " = float4(0.0f, 0.0f, 1.0f, 1.0f);\n";
      if (layout && texCoordIndex < layout->hasTexcoord.size() && layout->hasTexcoord[texCoordIndex]) {
        shader << "  dxmt9_texcoord" << stage << " = float4(dxmt9_load_f32x2(stream0, base + "
               << layout->texcoordOffset[texCoordIndex] << "u), 1.0f, 1.0f);\n";
      }
      // D3D9 startup/UI paths sometimes leave camera-space texgen enabled on
      // XYZRHW draws. Feeding screen-space XYZRHW into projected texgen produces
      // the giant diagonal smears seen in Anno 1701. For pre-transformed
      // vertices, preserve the authored texcoords instead of treating screen
      // coordinates as camera-space positions.
      if (texCoordGen == kTciCameraSpacePosition && !(layout && layout->preTransformed)) {
        shader << "  dxmt9_texcoord" << stage << " = float4(" << positionExpr << ".xyz, 1.0f);\n";
      }
      // Legacy UI paths frequently leave texgen / projected texture-transform
      // state enabled while submitting XYZRHW quads with authored UVs. Applying
      // that state to pre-transformed vertices causes severe diagonal smearing
      // in Anno 1701's menu background. For XYZRHW draws, use the provided UVs
      // directly and ignore the stale transform state.
      if (layout && layout->preTransformed) {
        shader << "  out.texcoord" << stage << " = dxmt9_texcoord" << stage << ";\n";
      } else {
        shader << "  out.texcoord" << stage << " = float4(dxmt9_apply_texture_transform(dxmt9_texcoord" << stage
               << ", uniforms, " << stage << "u, " << key.texTransformFlags[stage]
               << "u), dxmt9_texcoord" << stage << ".zw);\n";
      }
    }
  };
  out << makeShaderPrelude(key.clipPlaneMask != 0);
  if (layout && layout->preTransformed) {
    out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]], constant DrawUniforms& uniforms [[buffer(0)]], "
           "device const uchar* stream0 [[buffer(1)]]) {\n";
    out << "  VSOut out;\n";
    out << "  const uint stride = uniforms.vertexStreamStride != 0u ? uniforms.vertexStreamStride : "
        << layout->stride << "u;\n";
    out << "  const int vertexIndex = max(0, int(vid) + uniforms.vertexBaseIndex);\n";
    out << "  const uint base = uniforms.vertexStreamOffset + uint(vertexIndex) * stride;\n";
    out << "  float4 inPosition = dxmt9_load_f32x4(stream0, base + " << layout->positionOffset << "u);\n";
    out << "  float clipW = fabs(inPosition.w) > 1.0e-8f ? (1.0f / inPosition.w) : 1.0f;\n";
    out << "  float2 viewportSize = max(uniforms.viewportSize, float2(1.0f));\n";
    out << "  float2 ndc = float2(((inPosition.x - uniforms.viewportOrigin.x) / viewportSize.x) * 2.0f - 1.0f,\n";
    out << "                     1.0f - ((inPosition.y - uniforms.viewportOrigin.y) / viewportSize.y) * 2.0f);\n";
    out << "  out.position = float4(ndc * clipW, inPosition.z * clipW, clipW);\n";
    out << "  out.position.xy += uniforms.halfPixelFixup * out.position.w;\n";
    if (layout->hasDiffuse) {
      out << "  out.color = dxmt9_load_d3dcolor(stream0, base + " << layout->diffuseOffset << "u);\n";
    } else {
      out << "  out.color = float4(1.0);\n";
    }
    out << "  out.secondaryColor = float4(0.0);\n";
    emitStageTexcoords(out, "inPosition");
    out << "  out.fogFactor = 1.0;\n";
    out << "  out.pointSize = 1.0;\n";
  } else if (layout) {
    out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]], constant DrawUniforms& uniforms [[buffer(0)]], "
           "device const uchar* stream0 [[buffer(1)]]) {\n";
    out << "  VSOut out;\n";
    out << "  const uint stride = uniforms.vertexStreamStride != 0u ? uniforms.vertexStreamStride : "
        << layout->stride << "u;\n";
    out << "  const int vertexIndex = max(0, int(vid) + uniforms.vertexBaseIndex);\n";
    out << "  const uint base = uniforms.vertexStreamOffset + uint(vertexIndex) * stride;\n";
    if (layout->positionComponents == 4) {
      out << "  float4 inPosition = dxmt9_load_f32x4(stream0, base + " << layout->positionOffset << "u);\n";
    } else {
      out << "  float4 inPosition = float4(dxmt9_load_f32x3(stream0, base + " << layout->positionOffset
          << "u), 1.0f);\n";
    }
    out << "  float4 clip;\n";
    out << "  bool identityWvp = all(uniforms.ffpWorldViewProj[0] == float4(1.0, 0.0, 0.0, 0.0)) &&\n";
    out << "                     all(uniforms.ffpWorldViewProj[1] == float4(0.0, 1.0, 0.0, 0.0)) &&\n";
    out << "                     all(uniforms.ffpWorldViewProj[2] == float4(0.0, 0.0, 1.0, 0.0)) &&\n";
    out << "                     all(uniforms.ffpWorldViewProj[3] == float4(0.0, 0.0, 0.0, 1.0));\n";
    out << "  bool pixelSpacePosition = identityWvp && (fabs(inPosition.x) > 2.0f || fabs(inPosition.y) > 2.0f);\n";
    out << "  if (pixelSpacePosition) {\n";
    out << "    float2 viewportSize = max(uniforms.viewportSize, float2(1.0f));\n";
    out << "    float2 ndc = float2(((inPosition.x - uniforms.viewportOrigin.x) / viewportSize.x) * 2.0f - 1.0f,\n";
    out << "                       1.0f - ((inPosition.y - uniforms.viewportOrigin.y) / viewportSize.y) * 2.0f);\n";
    out << "    clip = float4(ndc, inPosition.z, 1.0f);\n";
    out << "  } else {\n";
    out << "    clip.x = dot(float4(uniforms.ffpWorldViewProj[0].x, uniforms.ffpWorldViewProj[1].x,\n";
    out << "                           uniforms.ffpWorldViewProj[2].x, uniforms.ffpWorldViewProj[3].x), inPosition);\n";
    out << "    clip.y = dot(float4(uniforms.ffpWorldViewProj[0].y, uniforms.ffpWorldViewProj[1].y,\n";
    out << "                           uniforms.ffpWorldViewProj[2].y, uniforms.ffpWorldViewProj[3].y), inPosition);\n";
    out << "    clip.z = dot(float4(uniforms.ffpWorldViewProj[0].z, uniforms.ffpWorldViewProj[1].z,\n";
    out << "                           uniforms.ffpWorldViewProj[2].z, uniforms.ffpWorldViewProj[3].z), inPosition);\n";
    out << "    clip.w = dot(float4(uniforms.ffpWorldViewProj[0].w, uniforms.ffpWorldViewProj[1].w,\n";
    out << "                           uniforms.ffpWorldViewProj[2].w, uniforms.ffpWorldViewProj[3].w), inPosition);\n";
    out << "  }\n";
    out << "  out.position = clip;\n";
    out << "  out.position.xy += uniforms.halfPixelFixup * out.position.w;\n";
    if (layout->hasDiffuse) {
      out << "  out.color = dxmt9_load_d3dcolor(stream0, base + " << layout->diffuseOffset << "u);\n";
    } else {
      out << "  out.color = float4(1.0);\n";
    }
    out << "  out.secondaryColor = float4(0.0);\n";
    emitStageTexcoords(out, "inPosition");
    out << "  out.fogFactor = 1.0;\n";
    out << "  out.pointSize = 1.0;\n";
  } else {
    out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]], constant DrawUniforms& uniforms [[buffer(0)]]) {\n";
    out << "  VSOut out;\n";
    out << "  float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n";
    out << "  out.position = float4(p[vid % 3], 0.0, 1.0);\n";
    out << "  out.position.xy += uniforms.halfPixelFixup * out.position.w;\n";
    out << "  out.color = float4(1.0);\n";
    out << "  out.secondaryColor = float4(0.0);\n";
    out << "  out.texcoord0 = float4(float2(vid & 1u, (vid >> 1u) & 1u), 0.0f, 1.0f);\n";
    for (size_t i = 1; i < kMaxTextureStages; ++i) {
      out << "  out.texcoord" << i << " = out.texcoord0;\n";
    }
    out << "  out.fogFactor = 1.0;\n";
    out << "  out.pointSize = 1.0;\n";
  }
  out << "  if (" << (key.lightingEnabled ? "true" : "false") << ") {\n";
  out << "    out.color.rgb *= 1.0;\n";
  out << "  }\n";
  if (key.clipPlaneMask != 0 || desc.clipPlaneMask != 0) {
    out << "  for (uint i = 0; i < 6; ++i) {\n";
    out << "    if ((uniforms.clipPlaneMask & (1u << i)) != 0u) {\n";
      out << "      out.clipDistance[i] = dot(uniforms.clipPlanes[i], out.position);\n";
    out << "    }\n";
    out << "  }\n";
  }
  out << "  return out;\n";
  out << "}\n";
  out << "// ffp vertex hash " << key.hash << "\n";
  return out.str();
}

std::string makeFfpPixelSource(const FfpPixelKey& key, const DrawDesc& desc) {
  std::ostringstream out;
  std::vector<size_t> activeStages;
  activeStages.reserve(kMaxTextureStages);
  for (size_t stage = 0; stage < kMaxTextureStages; ++stage) {
    const bool stageEnabled =
        key.stages[stage].colorOp != static_cast<u32>(TextureOp::Disable) ||
        key.stages[stage].alphaOp != static_cast<u32>(TextureOp::Disable);
    if (stageEnabled && desc.textures[stage].handle != Handle{}) {
      activeStages.push_back(stage);
    }
  }
  const bool textured = !activeStages.empty();
  const bool debugFfpUv = [] {
    const char* env = std::getenv("DXMT_DEBUG_FFP_UV");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  const bool debugFfpTexture = [] {
    const char* env = std::getenv("DXMT_DEBUG_FFP_TEXTURE");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  const bool debugFfpAlpha = [] {
    const char* env = std::getenv("DXMT_DEBUG_FFP_ALPHA");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  out << makeShaderPrelude(desc.clipPlaneMask != 0);
  if (textured) {
    out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], constant DrawUniforms& uniforms [[buffer(0)]], ";
    for (size_t i = 0; i < activeStages.size(); ++i) {
      const size_t stage = activeStages[i];
      if (i != 0) {
        out << ", ";
      }
      out << "texture2d<float> tex" << stage << " [[texture(" << stage << ")]], sampler samp" << stage
          << " [[sampler(" << stage << ")]]";
    }
    out << ") {\n";
  } else {
    out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], constant DrawUniforms& uniforms [[buffer(0)]]) {\n";
  }
  out << "  float4 color = in.color;\n";
  out << "  float4 current = color;\n";
  out << "  float4 diffuse = in.color;\n";
  out << "  float4 specular = in.secondaryColor;\n";
  out << "  float4 tfactor = uniforms.textureFactor;\n";
  out << "  float4 temp = float4(0.0);\n";
  if (textured) {
    if (debugFfpUv) {
      out << "  return float4(fract(in.texcoord0.x), fract(in.texcoord0.y), 0.0, 1.0);\n";
      out << "}\n";
      out << "// ffp pixel hash " << key.hash << "\n";
      return out.str();
    }
    if (debugFfpTexture) {
      const size_t stage = activeStages.front();
      const u32 coordIndex = key.stages[stage].texCoordIndex & 0xffffu;
      out << "  return tex" << stage << ".sample(samp" << stage
          << ", dxmt9_select_texcoord(in, " << coordIndex << "u).xy);\n";
      out << "}\n";
      out << "// ffp pixel hash " << key.hash << "\n";
      return out.str();
    }
    if (debugFfpAlpha) {
      const size_t stage = activeStages.front();
      const u32 coordIndex = key.stages[stage].texCoordIndex & 0xffffu;
      out << "  float alpha = tex" << stage << ".sample(samp" << stage
          << ", dxmt9_select_texcoord(in, " << coordIndex << "u).xy).a;\n";
      out << "  return float4(alpha, alpha, alpha, 1.0);\n";
      out << "}\n";
      out << "// ffp pixel hash " << key.hash << "\n";
      return out.str();
    }
    for (size_t stage = 0; stage < kMaxTextureStages; ++stage) {
      const auto& stageKey = key.stages[stage];
      const bool stageEnabled =
          stageKey.colorOp != static_cast<u32>(TextureOp::Disable) ||
          stageKey.alphaOp != static_cast<u32>(TextureOp::Disable);
      if (!stageEnabled) {
        continue;
      }
      const bool hasTexture = desc.textures[stage].handle != Handle{};
      const u32 coordIndex = stageKey.texCoordIndex & 0xffffu;
      if (hasTexture) {
        out << "  float4 texColor" << stage << " = tex" << stage << ".sample(samp" << stage
            << ", dxmt9_select_texcoord(in, " << coordIndex << "u).xy);\n";
      } else {
        out << "  float4 texColor" << stage << " = float4(1.0f);\n";
      }
      out << "  float4 colorArg1_" << stage << " = dxmt9_select_texture_arg(" << stageKey.colorArg1
          << "u, current, diffuse, specular, texColor" << stage << ", tfactor, temp);\n";
      out << "  float4 colorArg2_" << stage << " = dxmt9_select_texture_arg(" << stageKey.colorArg2
          << "u, current, diffuse, specular, texColor" << stage << ", tfactor, temp);\n";
      out << "  float4 stageResult" << stage << " = dxmt9_apply_texture_op(" << stageKey.colorOp
          << "u, colorArg1_" << stage << ", colorArg2_" << stage << ", current);\n";
      out << "  float4 alphaArg1_" << stage << " = dxmt9_select_texture_arg(" << stageKey.alphaArg1
          << "u, current, diffuse, specular, texColor" << stage << ", tfactor, temp);\n";
      out << "  float4 alphaArg2_" << stage << " = dxmt9_select_texture_arg(" << stageKey.alphaArg2
          << "u, current, diffuse, specular, texColor" << stage << ", tfactor, temp);\n";
      out << "  stageResult" << stage << ".a = dxmt9_apply_texture_op(" << stageKey.alphaOp
          << "u, alphaArg1_" << stage << ", alphaArg2_" << stage << ", current).a;\n";
      out << "  current = stageResult" << stage << ";\n";
      if (stageKey.resultArg == 5u) {
        out << "  temp = stageResult" << stage << ";\n";
      }
    }
    out << "  color = current;\n";
  }
  if (key.alphaTestEnable) {
    out << "  bool pass = true;\n";
    out << "  switch (uniforms.alphaTestFunc) {\n";
    out << "    case 2u: pass = color.a < uniforms.alphaRef; break;\n";
    out << "    case 3u: pass = color.a == uniforms.alphaRef; break;\n";
    out << "    case 4u: pass = color.a <= uniforms.alphaRef; break;\n";
    out << "    case 5u: pass = color.a > uniforms.alphaRef; break;\n";
    out << "    case 6u: pass = color.a != uniforms.alphaRef; break;\n";
    out << "    case 7u: pass = color.a >= uniforms.alphaRef; break;\n";
    out << "    case 8u: pass = true; break;\n";
    out << "    default: pass = true; break;\n";
    out << "  }\n";
    out << "  if (!pass) { discard_fragment(); }\n";
  }
  if (key.fogMode != FogMode::None) {
    out << "  float fog = clamp(in.fogFactor, 0.0, 1.0);\n";
    out << "  float4 fogColor = float4(0.5, 0.5, 0.5, 1.0);\n";
    out << "  color = mix(fogColor, color, fog);\n";
  }
  out << "  return color;\n";
  out << "}\n";
  out << "// ffp pixel hash " << key.hash << "\n";
  return out.str();
}

std::string makeGenericVertexSource(u64 variantHash) {
  std::ostringstream out;
  out << "#include <metal_stdlib>\nusing namespace metal;\n";
  out << "struct VSOut { float4 position [[position]]; };\n";
  out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]]) {\n";
  out << "  VSOut out;\n";
  out << "  float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n";
  out << "  out.position = float4(p[vid % 3], 0.0, 1.0);\n";
  out << "  return out;\n";
  out << "}\n";
  out << "// variant " << variantHash << "\n";
  return out.str();
}

std::string makeGenericFragmentSource(ColorRGBA color, u64 variantHash) {
  std::ostringstream out;
  out << "#include <metal_stdlib>\nusing namespace metal;\n";
  out << "struct VSOut { float4 position [[position]]; };\n";
  out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]]) {\n";
  out << "  (void)in;\n";
  out << "  return float4(" << color.r << "f, " << color.g << "f, " << color.b << "f, " << color.a << "f);\n";
  out << "}\n";
  out << "// variant " << variantHash << "\n";
  return out.str();
}

std::string makeTexturedVertexSource(u64 variantHash) {
  std::ostringstream out;
  out << "#include <metal_stdlib>\nusing namespace metal;\n";
  out << "struct VSOut { float4 position [[position]]; float2 uv; };\n";
  out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]]) {\n";
  out << "  VSOut out;\n";
  out << "  float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n";
  out << "  float2 uv[3] = { float2(0.0, 1.0), float2(2.0, 1.0), float2(0.0, -1.0) };\n";
  out << "  out.position = float4(p[vid % 3], 0.0, 1.0);\n";
  out << "  out.uv = uv[vid % 3];\n";
  out << "  return out;\n";
  out << "}\n";
  out << "// variant " << variantHash << "\n";
  return out.str();
}

std::string makeTexturedFragmentSource(u64 variantHash, bool forceOpaqueAlpha = false) {
  std::ostringstream out;
  out << "#include <metal_stdlib>\nusing namespace metal;\n";
  out << "struct VSOut { float4 position [[position]]; float2 uv; };\n";
  out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], texture2d<float> tex0 [[texture(0)]], sampler samp0 [[sampler(0)]]) {\n";
  out << "  float4 color = tex0.sample(samp0, in.uv);\n";
  if (forceOpaqueAlpha) {
    out << "  color.a = 1.0;\n";
  }
  out << "  return color;\n";
  out << "}\n";
  out << "// variant " << variantHash << "\n";
  return out.str();
}

WMT::Reference<WMT::Library> makeLibraryWMT(WMT::Device& device, const std::string& source) {
  WMT::Error error{};
  auto lib = device.newLibraryFromSource(source.c_str(), error);
  if (!lib) {
    return {};
  }
  return lib;
}

void initShaderArchive(WMT::Device& device, const std::string& path,
                       WMT::Reference<WMT::BinaryArchive>& archiveOut) {
  WMT::Error err{};
  auto archive = device.newBinaryArchive(path.c_str(), err);
  archiveOut = std::move(archive);
}

void persistShaderArchiveWMT(WMT::BinaryArchive& archive, const std::string& path) {
  if (!archive || path.empty()) {
    return;
  }
  WMT::Error err{};
  archive.serialize(path.c_str(), err);
}

std::string makeDrawShaderSource(const DrawDesc& desc, bool vertex);

std::string makeShaderSourceFromRequestInternal(const WinemetalShaderCompileRequest& request) {
  DrawDesc desc;
  desc.rts.color[0].sampleCount = std::max<u32>(1u, request.sampleCount);
  desc.clipPlaneMask = request.clipPlaneMask;
  desc.textures[0].handle = request.textured ? Handle{1} : Handle{};
  if (request.alphaTestEnable != 0) {
    desc.rs.values[RS_ALPHA_TEST_ENABLE] = request.alphaTestEnable;
    desc.rs.values[RS_ALPHA_FUNC] = request.alphaTestFunc;
    desc.rs.values[RS_ALPHA_REF] = static_cast<u32>(std::clamp(request.alphaRef, 0.0f, 1.0f) * 255.0f + 0.5f);
  }
  if (request.fogMode != static_cast<u32>(FogMode::None)) {
    desc.rs.values[RS_FOG_TABLE_MODE] = request.fogMode;
  }

  std::vector<u8> bytecode;
  if (request.bytecode && request.bytecodeSize > 0) {
    const auto* bytes = static_cast<const u8*>(request.bytecode);
    bytecode.assign(bytes, bytes + request.bytecodeSize);
  }

  switch (request.kind) {
    case WinemetalShaderKind_D3DBytecodeVertex:
      desc.vertexShader.kind = ShaderRef::Kind::Bytecode;
      desc.vertexShader.bytecode.bytes = std::move(bytecode);
      desc.vertexShader.bytecode.hash = request.bytecodeHash;
      return makeDrawShaderSource(desc, true);
    case WinemetalShaderKind_D3DBytecodePixel:
      desc.pixelShader.kind = ShaderRef::Kind::Bytecode;
      desc.pixelShader.bytecode.bytes = std::move(bytecode);
      desc.pixelShader.bytecode.hash = request.bytecodeHash;
      return makeDrawShaderSource(desc, false);
    case WinemetalShaderKind_FfpVertex:
      desc.vertexShader.kind = ShaderRef::Kind::FixedFunctionVertex;
      if (request.variantKey) {
        desc.vertexShader.vertexKey = *reinterpret_cast<const FfpVertexKey*>(request.variantKey);
      }
      return makeDrawShaderSource(desc, true);
    case WinemetalShaderKind_FfpPixel:
      desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
      if (request.variantKey) {
        desc.pixelShader.pixelKey = *reinterpret_cast<const FfpPixelKey*>(request.variantKey);
      }
      return makeDrawShaderSource(desc, false);
  }
  return {};
}

std::string makeDrawShaderSource(const DrawDesc& desc, bool vertex) {
  if (vertex) {
    if (desc.vertexShader.kind == ShaderRef::Kind::Bytecode) {
      auto source = makeTranslatedVertexSource(desc.vertexShader, desc);
      maybeDumpShaderSource("translated-vs", source);
      return source;
    }
    if (desc.vertexShader.kind == ShaderRef::Kind::FixedFunctionVertex && desc.vertexShader.vertexKey) {
      auto source = makeFfpVertexSource(*desc.vertexShader.vertexKey, desc);
      maybeDumpShaderSource("ffp-vs", source);
      return source;
    }
    const u64 variantHash = desc.vertexShader.hash ^ desc.clipPlaneMask ^ desc.rts.color[0].sampleCount;
    auto source = desc.textures[0].handle ? makeTexturedVertexSource(variantHash)
                                          : makeGenericVertexSource(variantHash);
    maybeDumpShaderSource("builtin-vs", source);
    return source;
  }

  if (desc.pixelShader.kind == ShaderRef::Kind::Bytecode) {
    auto source = makeTranslatedFragmentSource(desc.pixelShader, desc);
    maybeDumpShaderSource("translated-fs", source);
    return source;
  }
  if (desc.pixelShader.kind == ShaderRef::Kind::FixedFunctionPixel && desc.pixelShader.pixelKey) {
    auto source = makeFfpPixelSource(*desc.pixelShader.pixelKey, desc);
    maybeDumpShaderSource("ffp-fs", source);
    return source;
  }
  const u64 variantHash = desc.pixelShader.hash ^ desc.clipPlaneMask ^ desc.rts.color[0].sampleCount;
  auto source = desc.textures[0].handle ? makeTexturedFragmentSource(variantHash)
                                        : makeGenericFragmentSource({1.0f, 1.0f, 1.0f, 1.0f}, variantHash);
  maybeDumpShaderSource("builtin-fs", source);
  return source;
}

ShaderVariantKey makeShaderVariantKey(const DrawDesc& desc, std::span<const u32> colorFormats,
                                      std::span<const BlendAttachmentKey> blendAttachments, u32 depthFormat,
                                      u32 stencilFormat) {
  ShaderVariantKey key;
  const auto layout = decodeFixedFunctionVertexLayout(desc);
  const u64 layoutHash = layout ? layout->hash : hashVertexDeclaration(desc.vertexDecl);
  key.hash = desc.vertexShader.hash ^ (desc.pixelShader.hash << 1) ^ desc.clipPlaneMask ^ depthFormat ^
             (stencilFormat << 1) ^ (layoutHash << 1) ^ desc.vertexDecl.fvf;
  key.textured = desc.textures[0].handle != Handle{};
  const auto minFilterIt = desc.samplers[0].states.find(SAMP_MIN_FILTER);
  const auto magFilterIt = desc.samplers[0].states.find(SAMP_MAG_FILTER);
  key.linear = (minFilterIt != desc.samplers[0].states.end() && minFilterIt->second == 2u) ||
               (magFilterIt != desc.samplers[0].states.end() && magFilterIt->second == 2u);
  key.clipPlanes = desc.clipPlaneMask != 0;
  key.alphaTest = desc.rs.values.contains(RS_ALPHA_TEST_ENABLE) && desc.rs.values.at(RS_ALPHA_TEST_ENABLE) != 0;
  key.alphaToCoverage = false;
  key.sampleCount = std::max(1u, desc.rts.color[0].sampleCount);
  for (size_t i = 0; i < kMaxRenderTargets; ++i) {
    key.colorFormats[i] = i < colorFormats.size() ? colorFormats[i] : 0u;
    if (i < blendAttachments.size()) {
      key.blend[i] = blendAttachments[i];
    }
  }
  key.depthFormat = depthFormat;
  key.stencilFormat = stencilFormat;
  return key;
}

DepthStencilKey makeDepthStencilKey(const DrawDesc& desc) {
  DepthStencilKey key;
  if (debugForceVisibleDraw()) {
    return key;
  }
  const auto it = desc.rs.values.find(RS_Z_ENABLE);
  key.depthEnable = it != desc.rs.values.end() && it->second != 0;
  const auto writeIt = desc.rs.values.find(RS_Z_WRITE_ENABLE);
  key.depthWrite = writeIt != desc.rs.values.end() && writeIt->second != 0;
  const auto funcIt = desc.rs.values.find(RS_Z_FUNC);
  key.depthFunc = funcIt != desc.rs.values.end() ? funcIt->second : static_cast<u32>(CompareFunc::Always);
  const auto stencilEnableIt = desc.rs.values.find(RS_STENCIL_ENABLE);
  key.front.enabled = stencilEnableIt != desc.rs.values.end() && stencilEnableIt->second != 0;
  const auto stencilFuncIt = desc.rs.values.find(RS_STENCIL_FUNC);
  key.front.compareFunction = stencilFuncIt != desc.rs.values.end() ? stencilFuncIt->second
                                                                    : static_cast<u32>(CompareFunc::Always);
  const auto stencilFailIt = desc.rs.values.find(RS_STENCIL_FAIL);
  key.front.failureOperation = stencilFailIt != desc.rs.values.end() ? stencilFailIt->second
                                                                     : static_cast<u32>(StencilOp::Keep);
  const auto stencilZFailIt = desc.rs.values.find(RS_STENCIL_ZFAIL);
  key.front.depthFailureOperation = stencilZFailIt != desc.rs.values.end() ? stencilZFailIt->second
                                                                           : static_cast<u32>(StencilOp::Keep);
  const auto stencilPassIt = desc.rs.values.find(RS_STENCIL_PASS);
  key.front.passOperation = stencilPassIt != desc.rs.values.end() ? stencilPassIt->second
                                                                  : static_cast<u32>(StencilOp::Keep);
  const auto stencilMaskIt = desc.rs.values.find(RS_STENCIL_MASK);
  key.front.readMask = stencilMaskIt != desc.rs.values.end() ? stencilMaskIt->second : 0xffu;
  const auto stencilWriteMaskIt = desc.rs.values.find(RS_STENCIL_WRITEMASK);
  key.front.writeMask = stencilWriteMaskIt != desc.rs.values.end() ? stencilWriteMaskIt->second : 0xffu;
  const auto ccwFuncIt = desc.rs.values.find(RS_STENCIL_CCW_FUNC);
  key.back.compareFunction = ccwFuncIt != desc.rs.values.end() ? ccwFuncIt->second : key.front.compareFunction;
  const auto ccwFailIt = desc.rs.values.find(RS_STENCIL_CCW_FAIL);
  key.back.failureOperation = ccwFailIt != desc.rs.values.end() ? ccwFailIt->second : key.front.failureOperation;
  const auto ccwZFailIt = desc.rs.values.find(RS_STENCIL_CCW_ZFAIL);
  key.back.depthFailureOperation = ccwZFailIt != desc.rs.values.end() ? ccwZFailIt->second
                                                                       : key.front.depthFailureOperation;
  const auto ccwPassIt = desc.rs.values.find(RS_STENCIL_CCW_PASS);
  key.back.passOperation = ccwPassIt != desc.rs.values.end() ? ccwPassIt->second : key.front.passOperation;
  const auto ccwMaskIt = desc.rs.values.find(RS_STENCIL_CCW_MASK);
  key.back.readMask = ccwMaskIt != desc.rs.values.end() ? ccwMaskIt->second : key.front.readMask;
  const auto ccwWriteMaskIt = desc.rs.values.find(RS_STENCIL_CCW_WRITEMASK);
  key.back.writeMask = ccwWriteMaskIt != desc.rs.values.end() ? ccwWriteMaskIt->second : key.front.writeMask;
  key.back.enabled = key.front.enabled;
  return key;
}

class MetalBackendDevice final : public BackendDevice {
 public:
  explicit MetalBackendDevice(const BackendLimits& limits) : limits_(limits) {
    wrappedDevice_ = bootstrapWrappedDevice();
    if (!wrappedDevice_) {
      return;
    }
    limits_.supportsDepth24Stencil8 = wrappedDevice_.supportsDepth24Stencil8();
    wrappedCommandQueue_ = bootstrapWrappedCommandQueue(wrappedDevice_);
    if (!wrappedCommandQueue_) {
      return;
    }
    char cachePath[4096]{};
    WMTGetShaderCachePath(cachePath, sizeof(cachePath));
    shaderArchivePath_ = cachePath;
    initShaderArchive(wrappedDevice_, shaderArchivePath_, shaderArchive_);

    queueLifecycle_.bindTrackedSubmissionState({
        .writingSlot = &writingSlot_,
        .writeIndex = &writeIndex_,
        .nextSeqId = &nextSeqId_,
        .readySlots = &readySlots_,
        .completedSeqQueue = &completedSeqQueue_,
        .inflightCount = &inflightCount_,
        .completedSeqId = &completedSeqId_,
        .lastCommittedSeqId = &lastCommittedSeqId_,
        .slots = std::span<ChunkSlot>(slots_.data(), slots_.size()),
        .mutex = &mutex_,
        .writeCv = &writeCv_,
        .encodeCv = &encodeCv_,
        .finishCv = &finishCv_,
        .stop = &stop_,
        .submissionDiagnostics = &submissionDiagnostics_,
        .resolveSurfaceFlags = [this](Handle handle) { return compatFlagsForSurfaceUnlocked(handle); },
    });

    stop_ = false;
    encodeThread_ = std::thread([this] { encodeLoop(); });
    finishThread_ = std::thread([this] { finishLoop(); });
    ready_ = true;
  }

  ~MetalBackendDevice() override {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
      encodeCv_.notify_all();
      finishCv_.notify_all();
      writeCv_.notify_all();
    }
    if (encodeThread_.joinable()) {
      encodeThread_.join();
    }
    if (finishThread_.joinable()) {
      finishThread_.join();
    }
    std::lock_guard lock(mutex_);
    if (shaderArchive_) {
      persistShaderArchiveWMT(shaderArchive_, shaderArchivePath_);
    }
    purgeResourcesUnlocked();
  }

  void setDeviceLostObserver(DeviceLostObserver observer) override {
    std::lock_guard lock(mutex_);
    deviceLostObserver_ = std::move(observer);
  }

  void setPresentationStatusObserver(PresentationStatusObserver observer) override {
    std::lock_guard lock(mutex_);
    presentationStatusObserver_ = std::move(observer);
  }

  void setMaxFrameLatency(u32 latency) override {
    std::lock_guard lock(mutex_);
    maxFrameLatency_ = std::clamp(latency, 1u, 3u);
  }

  HResult waitForVBlank(const SwapDesc& desc) override {
    (void)desc;
    flush();
    return D3D_OK;
  }

  bool readbackSurface(const ReadbackDesc& desc, ReadbackPixels& pixels) override {
    WMT::Reference<WMT::Texture> sourceTexture;
    Format format = Format::Unknown;
    Rect sourceRect{};
    u32 sourceLevel = desc.sourceLevel;
    {
      std::lock_guard lock(mutex_);
      auto* surface = findSurfaceUnlocked(desc.source.value);
      if (!surface || !surface->texture) {
        return false;
      }
      sourceTexture = surface->resolveTexture ? surface->resolveTexture : surface->texture;
      format = surface->desc.format;
      sourceRect = desc.sourceRect;
      if (sourceRect.right <= sourceRect.left || sourceRect.bottom <= sourceRect.top) {
        sourceRect.right = static_cast<i32>(surface->desc.width);
        sourceRect.bottom = static_cast<i32>(surface->desc.height);
      }
    }

    const u32 width = static_cast<u32>(std::max(1, sourceRect.right - sourceRect.left));
    const u32 height = static_cast<u32>(std::max(1, sourceRect.bottom - sourceRect.top));
    const u32 bpp = bytesPerPixel(format);
    if (!sourceTexture || bpp == 0) {
      return false;
    }

    {
      WMTTextureInfo stagingInfo{};
      stagingInfo.type = WMTTextureType2D;
      stagingInfo.pixel_format = toPixelFormat(format, limits_);
      stagingInfo.width = width;
      stagingInfo.height = height;
      stagingInfo.depth = 1;
      stagingInfo.mipmap_level_count = 1;
      stagingInfo.sample_count = 1;
      stagingInfo.array_length = 1;
      stagingInfo.options = WMTResourceStorageModeShared;
      stagingInfo.usage = WMTTextureUsageShaderRead;
      auto stagingTexture = wrappedDevice_.newTexture(stagingInfo);
      if (!stagingTexture) {
        return false;
      }

      auto commandBuffer = bootstrapCommandBuffer(wrappedCommandQueue_);
      if (!commandBuffer) {
        return false;
      }
      auto blit = commandBuffer.blitCommandEncoder();
      if (!blit) {
        return false;
      }
      WMTOrigin srcOrigin{(uint64_t)sourceRect.left, (uint64_t)sourceRect.top, 0};
      WMTSize srcSize{width, height, 1};
      WMTOrigin dstOrigin{0, 0, 0};
      blit.copyFromTextureToTexture(WMT::Texture{sourceTexture.handle}, 0, sourceLevel,
                                    srcOrigin, srcSize,
                                    WMT::Texture{stagingTexture.handle}, 0, 0, dstOrigin);
      blit.endEncoding();
      commandBuffer.commit();
      commandBuffer.waitUntilCompleted();

      pixels.pitch = width * bpp;
      pixels.bytes.resize(static_cast<size_t>(pixels.pitch) * height);
      // Read back from staging texture by mapping it via replaceRegion in reverse:
      // stagingTexture is shared mode, so we can read its contents
      // Use WMT::Texture::replaceRegion in REVERSE is not right.
      // Instead, use a buffer-based readback: create buffer, blit texture to buffer, read buffer.
      // For simplicity: use a staging buffer + blit texture-to-buffer pattern.
      WMTBufferInfo bufInfo{};
      bufInfo.length = static_cast<uint64_t>(pixels.pitch) * height;
      bufInfo.options = WMTResourceStorageModeShared;
      auto readbackBuf = wrappedDevice_.newBuffer(bufInfo);
      if (readbackBuf) {
        auto cmdBuf2 = bootstrapCommandBuffer(wrappedCommandQueue_);
        if (cmdBuf2) {
          auto blit2 = cmdBuf2.blitCommandEncoder();
          if (blit2) {
            WMTOrigin origin{0, 0, 0};
            WMTSize size{width, height, 1};
            blit2.copyFromTextureToBuffer(WMT::Texture{stagingTexture.handle}, 0, 0,
                                          origin, size,
                                          WMT::Buffer{readbackBuf.handle},
                                          0, pixels.pitch, 0);
            blit2.endEncoding();
          }
          cmdBuf2.commit();
          cmdBuf2.waitUntilCompleted();
        }
        if (bufInfo.memory.ptr) {
          std::memcpy(pixels.bytes.data(), bufInfo.memory.ptr, pixels.bytes.size());
        }
      }
      return true;
    }
  }

  bool ready() const noexcept { return ready_; }

  BufferHandle createBuffer(const BufferDesc& desc) override {
    std::lock_guard lock(mutex_);
    const Handle handle{nextHandle_++};
    BufferRecord record;
    record.desc = desc;
    record.shadow.resize(static_cast<size_t>(desc.size));
    if (desc.pool != Pool::SystemMem && desc.pool != Pool::Scratch) {
      WMTBufferInfo info{};
      info.length = desc.size;
      info.options = WMTResourceStorageModeShared;
      record.buffer = wrappedDevice_.newBuffer(info);
      record.contents = info.memory.ptr;  // shared mode: contents ptr returned in info
    }
    buffers_[handle.value] = std::move(record);
    return handle;
  }

  TextureHandle createTexture(const TextureDesc& desc) override {
    std::lock_guard lock(mutex_);
    const Handle handle{nextHandle_++};
    TextureRecord record;
    record.desc = desc;
    if (desc.pool != Pool::SystemMem && desc.pool != Pool::Scratch) {
      WMTTextureInfo info{};
      info.type = toTextureType(desc.type, false);
      info.pixel_format = toPixelFormat(desc.format, limits_);
      info.width = std::max(1u, desc.width);
      info.height = std::max(1u, desc.height);
      info.depth = std::max(1u, desc.depth);
      info.mipmap_level_count = std::max(1u, desc.levels);
      info.sample_count = 1;
      info.array_length = 1;
      info.options = toResourceOptions(desc.pool, desc.usage);
      info.usage = toTextureUsage(desc);
      record.texture = wrappedDevice_.newTexture(info);
      record.isPrivate = (info.options == WMTResourceStorageModePrivate);
    }
    textures_[handle.value] = std::move(record);
    if (shouldTraceTexture(handle)) {
      std::ostringstream out;
      out << "[dxmt9-texture] create handle=0x" << std::hex << handle.value << std::dec
          << " format=" << static_cast<unsigned>(desc.format)
          << " type=" << static_cast<unsigned>(desc.type)
          << " size=" << desc.width << "x" << desc.height
          << " levels=" << desc.levels
          << " usage=0x" << std::hex << desc.usage << std::dec
          << " pool=" << static_cast<unsigned>(desc.pool);
      emitTextureTraceLine(out.str());
    }
    return handle;
  }

  SurfaceHandle createSurface(const SurfaceDesc& desc) override {
    std::lock_guard lock(mutex_);
    const Handle handle{nextHandle_++};
    SurfaceRecord record;
    record.desc = desc;
    if (desc.pool != Pool::SystemMem && desc.pool != Pool::Scratch) {
      const uint32_t sc = std::max(1u, sampleCount(desc.multiSampleType));
      WMTTextureInfo info{};
      info.type = toTextureType(TextureType::TwoD, desc.multiSampleType != MultiSampleType::None);
      info.pixel_format = toPixelFormat(desc.format, limits_);
      info.width = std::max(1u, desc.width);
      info.height = std::max(1u, desc.height);
      info.depth = 1;
      info.mipmap_level_count = 1;
      info.sample_count = sc;
      info.array_length = 1;
      info.options = toResourceOptions(desc.pool, desc.usage);
      info.usage = toTextureUsage(desc);
      record.texture = wrappedDevice_.newTexture(info);
      if (sc > 1) {
        WMTTextureInfo resolveInfo = info;
        resolveInfo.sample_count = 1;
        resolveInfo.type = WMTTextureType2D;
        resolveInfo.usage = static_cast<WMTTextureUsage>(WMTTextureUsageShaderRead | WMTTextureUsageRenderTarget);
        record.resolveTexture = wrappedDevice_.newTexture(resolveInfo);
      }
    }
    surfaces_[handle.value] = std::move(record);
    return handle;
  }

  SurfaceHandle createSurfaceForTexture(TextureHandle textureHandle, u32 level, const SurfaceDesc& desc) override {
    std::lock_guard lock(mutex_);
    auto textureIt = textures_.find(textureHandle.value);
    if (textureIt == textures_.end() || !textureIt->second.texture) {
      return {};
    }

    const Handle handle{nextHandle_++};
    SurfaceRecord record;
    record.desc = desc;
    record.aliasTexture = textureHandle;
    record.level = level;

    {
      WMT::Texture parentTexture{textureIt->second.texture.handle};
      if (level == 0 && desc.width == textureIt->second.desc.width &&
          desc.height == textureIt->second.desc.height) {
        record.texture = WMT::Reference<WMT::Texture>(parentTexture);
      } else {
        WMTTextureSwizzleChannels swizzle{
            WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
            WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha};
        uint64_t gpuId = 0;
        auto view = parentTexture.newTextureView(
            parentTexture.pixelFormat(), parentTexture.textureType(),
            level, 1, 0, 1, swizzle, gpuId);
        record.texture = view ? std::move(view) : WMT::Reference<WMT::Texture>(parentTexture);
      }
    }

    surfaces_[handle.value] = std::move(record);
    return handle;
  }

  void destroyBuffer(BufferHandle handle) override {
    std::lock_guard lock(mutex_);
    if (auto it = buffers_.find(handle.value); it != buffers_.end()) {
      it->second.destroyPending = true;
      tryGarbageCollectUnlocked();
    }
  }

  void destroyTexture(TextureHandle handle) override {
    std::lock_guard lock(mutex_);
    if (auto it = textures_.find(handle.value); it != textures_.end()) {
      it->second.destroyPending = true;
      tryGarbageCollectUnlocked();
    }
  }

  void destroySurface(SurfaceHandle handle) override {
    std::lock_guard lock(mutex_);
    if (auto it = surfaces_.find(handle.value); it != surfaces_.end()) {
      it->second.destroyPending = true;
      tryGarbageCollectUnlocked();
    }
  }

  void* mapBuffer(BufferHandle handle, u32 flags) override {
    std::unique_lock lock(mutex_);
    auto it = buffers_.find(handle.value);
    if (it == buffers_.end()) {
      return nullptr;
    }
    if ((flags & UsageDiscard) == 0 && (flags & UsageNoOverwrite) == 0 &&
        it->second.lastUsedSeqId > completedSeqId_) {
      waitForSequenceUnlocked(it->second.lastUsedSeqId, lock);
    }
    BufferRecord& record = it->second;
    if ((flags & UsageDiscard) != 0) {
      std::fill(record.shadow.begin(), record.shadow.end(), 0);
      if (record.contents) {
        std::memset(record.contents, 0, record.shadow.size());
      }
    }
    if (record.contents) {
      return record.contents;
    }
    return record.shadow.empty() ? nullptr : record.shadow.data();
  }

  void unmapBuffer(BufferHandle handle) override {
    std::lock_guard lock(mutex_);
    (void)handle;
  }

  void uploadBufferData(BufferHandle handle, std::span<const u8> bytes) override {
    std::lock_guard lock(mutex_);
    auto it = buffers_.find(handle.value);
    if (it == buffers_.end()) {
      return;
    }
    it->second.shadow.assign(bytes.begin(), bytes.end());
    if (!it->second.buffer || bytes.empty() || !it->second.contents) {
      return;
    }
    const size_t copySize = std::min(bytes.size(), static_cast<size_t>(it->second.desc.size));
    std::memcpy(it->second.contents, bytes.data(), copySize);
  }

  void uploadTextureLevel(TextureHandle handle, u32 level, u32 width, u32 height, u32 pitch,
                          std::span<const u8> bytes) override {
    std::lock_guard lock(mutex_);
    auto it = textures_.find(handle.value);
    if (it == textures_.end() || !it->second.texture || bytes.empty()) {
      return;
    }
    if (shouldTraceTexture(handle)) {
      u32 minAlpha = 255u;
      u32 maxAlpha = 0u;
      u64 nonZeroAlpha = 0u;
      u64 nonZeroRgb = 0u;
      if ((it->second.desc.format == Format::A8R8G8B8 || it->second.desc.format == Format::A8B8G8R8 ||
           it->second.desc.format == Format::X8R8G8B8 || it->second.desc.format == Format::X8B8G8R8) &&
          pitch >= width * 4u) {
        for (u32 y = 0; y < height; ++y) {
          const u8* row = bytes.data() + static_cast<size_t>(y) * pitch;
          for (u32 x = 0; x < width; ++x) {
            const u8 b = row[static_cast<size_t>(x) * 4u + 0u];
            const u8 g = row[static_cast<size_t>(x) * 4u + 1u];
            const u8 r = row[static_cast<size_t>(x) * 4u + 2u];
            const u8 a = row[static_cast<size_t>(x) * 4u + 3u];
            minAlpha = std::min<u32>(minAlpha, a);
            maxAlpha = std::max<u32>(maxAlpha, a);
            nonZeroAlpha += (a != 0u) ? 1u : 0u;
            nonZeroRgb += (r != 0u || g != 0u || b != 0u) ? 1u : 0u;
          }
        }
      }
      std::ostringstream out;
      out << "[dxmt9-texture] upload handle=0x" << std::hex << handle.value << std::dec
          << " level=" << level
          << " size=" << width << "x" << height
          << " pitch=" << pitch
          << " bytes=" << bytes.size()
          << " alphaMin=" << minAlpha
          << " alphaMax=" << maxAlpha
          << " nonZeroAlpha=" << nonZeroAlpha
          << " nonZeroRgb=" << nonZeroRgb
          << " head=";
      const size_t preview = std::min<size_t>(16, bytes.size());
      for (size_t i = 0; i < preview; ++i) {
        if (i) {
          out << ',';
        }
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[i]) << std::dec;
      }
      emitTextureTraceLine(out.str());
    }

    std::vector<u8> normalizedStorage;
    const auto normalizedBytes =
        normalizeTextureUploadBytes(it->second.desc.format, width, height, pitch, bytes, normalizedStorage);

    WMT::Texture texture{it->second.texture.handle};
    const uint32_t mipLevel = level;
    const uint32_t mipWidth = std::max(1u, width);
    const uint32_t mipHeight = std::max(1u, height);

    if (!it->second.isPrivate) {
      WMTOrigin origin{0, 0, 0};
      WMTSize size{mipWidth, mipHeight, 1};
      texture.replaceRegion(origin, size, mipLevel, 0, normalizedBytes.data(), pitch, 0);
    } else {
      WMTTextureInfo stagingInfo{};
      stagingInfo.type = WMTTextureType2D;
      stagingInfo.pixel_format = texture.pixelFormat();
      stagingInfo.width = mipWidth;
      stagingInfo.height = mipHeight;
      stagingInfo.depth = 1;
      stagingInfo.mipmap_level_count = 1;
      stagingInfo.sample_count = 1;
      stagingInfo.array_length = 1;
      stagingInfo.options = WMTResourceStorageModeShared;
      stagingInfo.usage = WMTTextureUsageShaderRead;
      auto stagingTexture = wrappedDevice_.newTexture(stagingInfo);
      if (!stagingTexture) {
        return;
      }
      {
        WMTOrigin origin{0, 0, 0};
        WMTSize size{mipWidth, mipHeight, 1};
        WMT::Texture{stagingTexture.handle}.replaceRegion(origin, size, 0, 0,
                                                          normalizedBytes.data(), pitch, 0);
      }
      auto commandBuffer = bootstrapCommandBuffer(wrappedCommandQueue_);
      if (!commandBuffer) {
        return;
      }
      auto blit = commandBuffer.blitCommandEncoder();
      if (!blit) {
        return;
      }
      WMTOrigin origin{0, 0, 0};
      WMTSize size{mipWidth, mipHeight, 1};
      blit.copyFromTextureToTexture(WMT::Texture{stagingTexture.handle}, 0, 0,
                                    origin, size, texture, 0, mipLevel, origin);
      blit.endEncoding();
      commandBuffer.commit();
      commandBuffer.waitUntilCompleted();
    }

    if (level == 0 && shouldDumpGpuTexture(handle)) {
      dumpTextureSnapshotUnlocked(handle, it->second.desc, it->second.texture.handle);
    }
  }

  void submitDraw(const DrawDesc& desc) override {
    std::unique_lock lock(mutex_);
    // TLA+: WineCommit
    ensureWritingSlotUnlocked(lock);
    currentSlot().commands.push_back(makeDrawCommand(desc));
    currentBackBuffer_ = desc.rts.color[0].handle;
    markDrawResourcesUnlocked(desc);
  }

  void submitClear(const ClearDesc& desc) override {
    std::unique_lock lock(mutex_);
    // TLA+: WineCommit
    ensureWritingSlotUnlocked(lock);
    currentSlot().commands.push_back(makeClearCommand(desc));
    if (desc.colorAttachments[0].handle) {
      currentBackBuffer_ = desc.colorAttachments[0].handle;
    }
    markClearResourcesUnlocked(desc);
  }

  void submitSurfaceCopy(const SurfaceCopyDesc& desc) override {
    std::unique_lock lock(mutex_);
    // TLA+: WineCommit
    ensureWritingSlotUnlocked(lock);
    currentSlot().commands.push_back(makeSurfaceCopyCommand(desc));
    markSurfaceCopyResourcesUnlocked(desc);
  }

  void submitStretchRect(const StretchRectDesc& desc) override {
    std::unique_lock lock(mutex_);
    // TLA+: WineCommit
    ensureWritingSlotUnlocked(lock);
    currentSlot().commands.push_back(makeStretchRectCommand(desc));
    markStretchResourcesUnlocked(desc);
  }

  void submitReadback(const ReadbackDesc& desc) override {
    std::lock_guard lock(mutex_);
    // Readback is satisfied by the synchronous staging copy in readbackSurface().
    // We still track resource liveness so NoUseAfterFree remains meaningful.
    markReadbackResourcesUnlocked(desc);
  }

  void submitColorFill(const ColorFillDesc& desc) override {
    std::unique_lock lock(mutex_);
    // TLA+: WineCommit
    ensureWritingSlotUnlocked(lock);
    currentSlot().commands.push_back(makeColorFillCommand(desc));
    currentBackBuffer_ = desc.destination;
    markColorFillResourcesUnlocked(desc);
  }

  void present(const SwapDesc& desc) override {
    std::unique_lock lock(mutex_);
    // TLA+: WineCommit
    queueLifecycle_.presentAndCommit(lock, kMaxInflight, desc, currentBackBuffer_, [this](const ChunkSlot& slot) {
      updateLastUsedSeqIdsUnlocked(slot);
    });
  }

 void flush() override {
    std::unique_lock lock(mutex_);
    // TLA+: WineCommit
    queueLifecycle_.flushAndWait(lock, kMaxInflight, [this](const ChunkSlot& slot) {
      updateLastUsedSeqIdsUnlocked(slot);
    });
  }

 private:
  u32 compatFlagsForSurfaceUnlocked(Handle handle) const {
    if (!handle) {
      return 0;
    }
    const auto* surface = findSurfaceUnlocked(handle.value);
    if (!surface) {
      return 0;
    }
    return isFloatRenderTargetFormat(surface->desc.format) ? CompatFlagFp16 : 0u;
  }

  BufferRecord* findBufferUnlocked(u64 handle) {
    auto it = buffers_.find(handle);
    return it == buffers_.end() ? nullptr : &it->second;
  }

  const BufferRecord* findBufferUnlocked(u64 handle) const {
    auto it = buffers_.find(handle);
    return it == buffers_.end() ? nullptr : &it->second;
  }

  TextureRecord* findTextureUnlocked(u64 handle) {
    auto it = textures_.find(handle);
    return it == textures_.end() ? nullptr : &it->second;
  }

  const TextureRecord* findTextureUnlocked(u64 handle) const {
    auto it = textures_.find(handle);
    return it == textures_.end() ? nullptr : &it->second;
  }

  SurfaceRecord* findSurfaceUnlocked(u64 handle) {
    auto it = surfaces_.find(handle);
    return it == surfaces_.end() ? nullptr : &it->second;
  }

  const SurfaceRecord* findSurfaceUnlocked(u64 handle) const {
    auto it = surfaces_.find(handle);
    return it == surfaces_.end() ? nullptr : &it->second;
  }

  ChunkSlot& currentSlot() {
    // TLA+: RingSafety
    DXMT_ASSERT(writingSlot_.has_value());
    return slots_[*writingSlot_];
  }

  void ensureWritingSlotUnlocked(std::unique_lock<std::mutex>& lock) {
    (void)queueLifecycle_.ensureWriterSlot(lock, kMaxInflight);
  }

  void updateLastUsedSeqIdsUnlocked(const ChunkSlot& slot) {
    for (const auto& command : slot.commands) {
      switch (command.kind) {
        case MetalCommandRecord::Kind::Draw:
          markDrawResourcesUnlocked(command.draw, slot.seqId);
          break;
        case MetalCommandRecord::Kind::Clear:
          markClearResourcesUnlocked(command.clear, slot.seqId);
          break;
        case MetalCommandRecord::Kind::SurfaceCopy:
          markSurfaceCopyResourcesUnlocked(command.surfaceCopy, slot.seqId);
          break;
        case MetalCommandRecord::Kind::StretchRect:
          markStretchResourcesUnlocked(command.stretchRect, slot.seqId);
          break;
        case MetalCommandRecord::Kind::Readback:
          markReadbackResourcesUnlocked(command.readback, slot.seqId);
          break;
        case MetalCommandRecord::Kind::ColorFill:
          markColorFillResourcesUnlocked(command.colorFill, slot.seqId);
          break;
        case MetalCommandRecord::Kind::Present:
          if (command.presentSource) {
            if (auto* surface = findSurfaceUnlocked(command.presentSource.value)) {
              surface->lastUsedSeqId = std::max(surface->lastUsedSeqId, slot.seqId);
            }
          }
          break;
      }
    }
  }

  void markBufferUseUnlocked(Handle handle, u64 seqId) {
    if (!handle) {
      return;
    }
    if (auto* buffer = findBufferUnlocked(handle.value)) {
      buffer->lastUsedSeqId = std::max(buffer->lastUsedSeqId, seqId);
    }
  }

  void markTextureUseUnlocked(Handle handle, u64 seqId) {
    if (!handle) {
      return;
    }
    if (auto* texture = findTextureUnlocked(handle.value)) {
      texture->lastUsedSeqId = std::max(texture->lastUsedSeqId, seqId);
    }
  }

  void markSurfaceUseUnlocked(Handle handle, u64 seqId) {
    if (!handle) {
      return;
    }
    if (auto* surface = findSurfaceUnlocked(handle.value)) {
      surface->lastUsedSeqId = std::max(surface->lastUsedSeqId, seqId);
    }
  }

  void markDrawResourcesUnlocked(const DrawDesc& desc, u64 seqId = 0) {
    if (seqId == 0) {
      seqId = nextSeqId_;
    }
    markBufferUseUnlocked(desc.indexBuffer, seqId);
    for (const auto& stream : desc.vertexDecl.streams) {
      if (stream.buffer) {
        markBufferUseUnlocked(stream.buffer->handle(), seqId);
      }
    }
    for (const auto& texture : desc.textures) {
      markTextureUseUnlocked(texture.handle, seqId);
    }
    for (const auto& rt : desc.rts.color) {
      markSurfaceUseUnlocked(rt.handle, seqId);
    }
    markSurfaceUseUnlocked(desc.rts.depthStencil.handle, seqId);
  }

  void markClearResourcesUnlocked(const ClearDesc& desc, u64 seqId = 0) {
    if (seqId == 0) {
      seqId = nextSeqId_;
    }
    for (const auto& attachment : desc.colorAttachments) {
      markSurfaceUseUnlocked(attachment.handle, seqId);
    }
    markSurfaceUseUnlocked(desc.depthStencil.handle, seqId);
  }

  void markSurfaceCopyResourcesUnlocked(const SurfaceCopyDesc& desc, u64 seqId = 0) {
    if (seqId == 0) {
      seqId = nextSeqId_;
    }
    markSurfaceUseUnlocked(desc.source, seqId);
    markSurfaceUseUnlocked(desc.destination, seqId);
  }

  void markStretchResourcesUnlocked(const StretchRectDesc& desc, u64 seqId = 0) {
    if (seqId == 0) {
      seqId = nextSeqId_;
    }
    markSurfaceUseUnlocked(desc.source, seqId);
    markSurfaceUseUnlocked(desc.destination, seqId);
  }

  void markReadbackResourcesUnlocked(const ReadbackDesc& desc, u64 seqId = 0) {
    if (seqId == 0) {
      seqId = nextSeqId_;
    }
    markSurfaceUseUnlocked(desc.source, seqId);
    markSurfaceUseUnlocked(desc.destination, seqId);
  }

  void markColorFillResourcesUnlocked(const ColorFillDesc& desc, u64 seqId = 0) {
    if (seqId == 0) {
      seqId = nextSeqId_;
    }
    markSurfaceUseUnlocked(desc.destination, seqId);
  }

  MetalCommandRecord makeDrawCommand(const DrawDesc& desc) {
    MetalCommandRecord op;
    op.kind = MetalCommandRecord::Kind::Draw;
    op.draw = desc;
    return op;
  }

  MetalCommandRecord makeClearCommand(const ClearDesc& desc) {
    MetalCommandRecord op;
    op.kind = MetalCommandRecord::Kind::Clear;
    op.clear = desc;
    return op;
  }

  MetalCommandRecord makeSurfaceCopyCommand(const SurfaceCopyDesc& desc) {
    MetalCommandRecord op;
    op.kind = MetalCommandRecord::Kind::SurfaceCopy;
    op.surfaceCopy = desc;
    return op;
  }

  MetalCommandRecord makeStretchRectCommand(const StretchRectDesc& desc) {
    MetalCommandRecord op;
    op.kind = MetalCommandRecord::Kind::StretchRect;
    op.stretchRect = desc;
    return op;
  }

  MetalCommandRecord makeReadbackCommand(const ReadbackDesc& desc) {
    MetalCommandRecord op;
    op.kind = MetalCommandRecord::Kind::Readback;
    op.readback = desc;
    return op;
  }

  MetalCommandRecord makeColorFillCommand(const ColorFillDesc& desc) {
    MetalCommandRecord op;
    op.kind = MetalCommandRecord::Kind::ColorFill;
    op.colorFill = desc;
    return op;
  }

  void encodeLoop() {
    @autoreleasepool {
      while (true) {
        std::unique_lock lock(mutex_);
        if (!queueLifecycle_.runEncodeIteration(
                lock,
                [this](size_t slotIndex, const ChunkSlot& slot) {
                  return encodeChunk(slotIndex, slot);
                },
                [this](u64) {
                  argbufArena_.reclaim(completedSeqId_);
                  lambdaStoreArena_.reclaim(completedSeqId_);
                  stagingArena_.reclaim(completedSeqId_);
                  copyTempArena_.reclaim(completedSeqId_);
                })) {
          return;
        }
      }
    }
  }

  std::optional<metalqueue::QueueSubmissionRecord> encodeChunk(size_t slotIndex, const ChunkSlot& slot) {
    {
      if (!wrappedDevice_ || !wrappedCommandQueue_) {
        return std::nullopt;
      }

      auto ownedCommandBuffer = bootstrapCommandBuffer(wrappedCommandQueue_);
      if (!ownedCommandBuffer) {
        return std::nullopt;
      }
      auto commandBuffer = ownedCommandBuffer;

      WMT::Reference<WMT::RenderCommandEncoder> activeRenderEncoder{};
      WMT::Reference<WMT::BlitCommandEncoder> activeBlitEncoder{};
      AttachmentKey activeKey{};
      HazardBloom activeWriteBloom{};
      bool hasActiveRender = false;
      std::optional<ClearDesc> pendingClear;

      auto flushRender = [&] {
        if (activeRenderEncoder) {
          activeRenderEncoder.endEncoding();
          activeRenderEncoder = {};
          hasActiveRender = false;
        }
      };

      auto flushBlit = [&] {
        if (activeBlitEncoder) {
          activeBlitEncoder.endEncoding();
          activeBlitEncoder = {};
        }
      };

      auto startRenderPass = [&](const DrawDesc& draw, const std::optional<ClearDesc>& clear) {
        activeRenderEncoder = beginRenderPass(commandBuffer, draw, clear);
        hasActiveRender = static_cast<bool>(activeRenderEncoder);
        activeKey = makeAttachmentKey(draw.rts);
        activeWriteBloom = makeAttachmentBloom(draw.rts);
      };

      auto flushPendingClear = [&] {
        if (!pendingClear.has_value()) {
          return;
        }
        encodeClearPass(commandBuffer, *pendingClear);
        pendingClear.reset();
      };

      for (const auto& command : slot.commands) {
        switch (command.kind) {
          case MetalCommandRecord::Kind::Clear:
            flushRender();
            flushBlit();
            if (command.clear.rects.empty()) {
              pendingClear = command.clear;
            } else {
              encodeColorFillPass(commandBuffer, command.clear);
            }
            break;
          case MetalCommandRecord::Kind::Draw: {
            flushBlit();
            const auto drawKey = makeAttachmentKey(command.draw.rts);
            const auto drawReadBloom = makeDrawReadBloom(command.draw);
            if (pendingClear.has_value()) {
              const auto clearKey = makeAttachmentKey(*pendingClear);
              const auto clearBloom = makeAttachmentBloom(*pendingClear);
              if (clearKey == drawKey && !clearBloom.overlaps(drawReadBloom)) {
                startRenderPass(command.draw, pendingClear);
                pendingClear.reset();
              } else {
                flushPendingClear();
                if (!hasActiveRender || activeKey != drawKey || activeWriteBloom.overlaps(drawReadBloom)) {
                  flushRender();
                  startRenderPass(command.draw, std::nullopt);
                }
              }
            } else if (!hasActiveRender || activeKey != drawKey || activeWriteBloom.overlaps(drawReadBloom)) {
              flushRender();
              startRenderPass(command.draw, std::nullopt);
            }
            encodeDraw(commandBuffer, activeRenderEncoder, command.draw, slot.seqId);
            break;
          }
          case MetalCommandRecord::Kind::SurfaceCopy:
            flushPendingClear();
            flushRender();
            encodeSurfaceCopy(commandBuffer, command.surfaceCopy);
            break;
          case MetalCommandRecord::Kind::StretchRect:
            flushPendingClear();
            flushRender();
            encodeStretchRect(commandBuffer, command.stretchRect);
            break;
          case MetalCommandRecord::Kind::Readback:
            flushPendingClear();
            flushRender();
            encodeReadback(commandBuffer, command.readback);
            break;
          case MetalCommandRecord::Kind::ColorFill:
            flushPendingClear();
            flushRender();
            encodeColorFill(commandBuffer, command.colorFill);
            break;
          case MetalCommandRecord::Kind::Present:
            flushPendingClear();
            flushRender();
            flushBlit();
            encodePresent(commandBuffer, command.present, command.presentSource, slot.seqId);
            break;
        }
      }

      flushPendingClear();
      flushRender();
      flushBlit();

      const u64 seqId = slot.seqId;
      // Retain ownership for the submission record; the queue will release after commit.
      NSObject_retain(commandBuffer.handle);
      return metalqueue::QueueSubmissionRecord{
          .commandBuffer = commandBuffer.handle,
          .slotIndex = slotIndex,
          .seqId = seqId,
          .commands = std::span<const MetalCommandRecord>(slot.commands.data(), slot.commands.size()),
          .context = "queue",
      };
    }
  }

  WMT::Reference<WMT::RenderCommandEncoder> beginRenderPass(WMT::CommandBuffer& commandBuffer,
                                                             const DrawDesc& draw,
                                                             const std::optional<ClearDesc>& clear) {
    auto* surface = findSurfaceUnlocked(draw.rts.color[0].handle.value);
    if (!surface || !surface->texture) {
      return {};
    }
    {
      WMTRenderPassInfo passInfo{};
      auto& attachment = passInfo.colors[0];
      attachment.texture = surface->texture.handle;
      const bool discardAfterPresent = !clear.has_value() && backBufferDiscardAfterPresent_ &&
                                       draw.rts.color[0].handle == currentBackBuffer_;
      attachment.load_action = clear.has_value() ? WMTLoadActionClear
                                                  : (discardAfterPresent ? WMTLoadActionDontCare
                                                                         : WMTLoadActionLoad);
      attachment.store_action = WMTStoreActionStore;
      if (surface->resolveTexture) {
        attachment.resolve_texture = surface->resolveTexture.handle;
        attachment.store_action = WMTStoreActionMultisampleResolve;
      }
      if (clear.has_value()) {
        attachment.clear_color = WMTClearColor{clear->color.r, clear->color.g,
                                               clear->color.b, clear->color.a};
      }

      if (auto* depthSurface = findSurfaceUnlocked(draw.rts.depthStencil.handle.value);
          depthSurface && depthSurface->texture && depthSurface->desc.depthStencil) {
        if (formatHasDepthAspect(depthSurface->desc.format)) {
          passInfo.depth.texture = depthSurface->texture.handle;
          passInfo.depth.load_action = (clear.has_value() && clear->clearDepth)
                                           ? WMTLoadActionClear : WMTLoadActionLoad;
          passInfo.depth.store_action = WMTStoreActionStore;
          if (clear.has_value()) {
            passInfo.depth.clear_depth = clear->depth;
          }
        }
        if (formatHasStencilAspect(depthSurface->desc.format)) {
          passInfo.stencil.texture = depthSurface->texture.handle;
          passInfo.stencil.load_action = (clear.has_value() && clear->clearStencil)
                                             ? WMTLoadActionClear : WMTLoadActionLoad;
          passInfo.stencil.store_action = WMTStoreActionStore;
          if (clear.has_value()) {
            passInfo.stencil.clear_stencil = clear->stencil;
          }
        }
      }

      auto encoder = commandBuffer.renderCommandEncoder(passInfo);
      if (!encoder) {
        return {};
      }
      if (discardAfterPresent) {
        backBufferDiscardAfterPresent_ = false;
      }
      const auto ffLayout = decodeFixedFunctionVertexLayout(draw);
      double viewportWidth = static_cast<double>(std::max(1u, draw.viewport.viewport.width));
      double viewportHeight = static_cast<double>(std::max(1u, draw.viewport.viewport.height));
      double viewportOriginX = 0.0;
      double viewportOriginY = 0.0;
      if (ffLayout && ffLayout->preTransformed) {
        viewportWidth = static_cast<double>(std::max(1u, surface->desc.width));
        viewportHeight = static_cast<double>(std::max(1u, surface->desc.height));
      }
      WMTViewport vp{viewportOriginX, viewportOriginY, viewportWidth, viewportHeight,
                     static_cast<double>(draw.viewport.viewport.minZ),
                     static_cast<double>(draw.viewport.viewport.maxZ)};
      encoder.setViewport(vp);
      encoder.setRasterizerState(WMTTriangleFillModeFill, WMTCullModeNone,
                                 WMTDepthClipModeClip, WMTWindingClockwise,
                                 0.0f, 0.0f, 0.0f);
      return WMT::Reference<WMT::RenderCommandEncoder>(encoder);
    }
  }

  void encodeDraw(WMT::CommandBuffer& commandBuffer, WMT::RenderCommandEncoder& encoder,
                  const DrawDesc& draw, u64 seqId) {
    (void)commandBuffer;
    if (debugSkipAllDraws()) {
      if (queueTraceEnabled()) {
        std::ostringstream out;
        out << "[dxmt9-debug] skip all draws seq=" << static_cast<unsigned long long>(seqId)
            << " tex0=" << static_cast<unsigned long long>(draw.textures[0].handle.value);
        emitQueueTraceLine(out.str());
      }
      return;
    }
    const bool traceEncode = shouldTraceEncode(draw, seqId);
    if (!encoder) {
      if (traceEncode) {
        emitQueueTraceLine("[dxmt9-encode] seq=" + std::to_string(seqId) + " skipped reason=no-encoder");
      }
      return;
    }
    const auto depthKey = makeDepthStencilKey(draw);
    auto pipeline = pipelineForDraw(draw).get();
    if (!pipeline) {
      if (traceEncode) {
        std::ostringstream out;
        out << "[dxmt9-encode] seq=" << static_cast<unsigned long long>(seqId)
            << " skipped reason=no-pipeline"
            << " rt0=" << static_cast<unsigned long long>(draw.rts.color[0].handle.value)
            << " ds=" << static_cast<unsigned long long>(draw.rts.depthStencil.handle.value)
            << " tex0=" << static_cast<unsigned long long>(draw.textures[0].handle.value)
            << " fvf=0x" << std::hex << draw.vertexDecl.fvf << std::dec
            << " alphaBlend="
            << (draw.rs.values.contains(RS_ALPHABLEND_ENABLE) ? draw.rs.values.at(RS_ALPHABLEND_ENABLE) : 0u)
            << " colorWrite="
            << (draw.rs.values.contains(RS_COLOR_WRITE_ENABLE) ? draw.rs.values.at(RS_COLOR_WRITE_ENABLE) : 0xfu);
        emitQueueTraceLine(out.str());
      }
      return;
    }
    auto depthState = depthStencilStateFor(depthKey);
    if (depthState) {
      encoder.setDepthStencilState(depthState);
    }
    encoder.setRenderPipelineState(pipeline);
    auto* uniforms = argbufArena_.allocate<DrawUniforms>(seqId);
    DrawUniforms fallbackUniforms{};
    if (!uniforms) {
      uniforms = &fallbackUniforms;
    }
    *uniforms = buildDrawUniforms(draw);
    WMTBufferInfo uniformInfo{};
    uniformInfo.length = sizeof(DrawUniforms);
    uniformInfo.options = WMTResourceStorageModeShared;
    uniformInfo.memory.set((void *)uniforms);
    auto transientUniformBuffer = wrappedDevice_.newBuffer(uniformInfo);
    if (!transientUniformBuffer) {
      return;
    }
    encoder.setVertexBuffer(transientUniformBuffer, 0, 0);
    encoder.setFragmentBuffer(transientUniformBuffer, 0, 0);
    const auto ffLayout = decodeFixedFunctionVertexLayout(draw);
    if (auto* surface = findSurfaceUnlocked(draw.rts.color[0].handle.value); surface && surface->texture) {
      double viewportWidth = static_cast<double>(std::max(1u, draw.viewport.viewport.width));
      double viewportHeight = static_cast<double>(std::max(1u, draw.viewport.viewport.height));
      double viewportOriginX = 0.0;
      double viewportOriginY = 0.0;
      if (ffLayout && ffLayout->preTransformed) {
        viewportWidth = static_cast<double>(std::max(1u, surface->desc.width));
        viewportHeight = static_cast<double>(std::max(1u, surface->desc.height));
      }
      encoder.setViewport(WMTViewport{viewportOriginX, viewportOriginY, viewportWidth, viewportHeight,
                                      static_cast<double>(draw.viewport.viewport.minZ),
                                      static_cast<double>(draw.viewport.viewport.maxZ)});
      WMTScissorRect scissor{};
      if (draw.viewport.scissorEnabled && !debugDisableScissor()) {
        scissor.x = static_cast<uint64_t>(std::max(0, draw.viewport.scissor.left));
        scissor.y = static_cast<uint64_t>(std::max(0, draw.viewport.scissor.top));
        scissor.width = static_cast<uint64_t>(std::max(0, draw.viewport.scissor.right - draw.viewport.scissor.left));
        scissor.height =
            static_cast<uint64_t>(std::max(0, draw.viewport.scissor.bottom - draw.viewport.scissor.top));
      } else {
        scissor.x = 0;
        scissor.y = 0;
        scissor.width = static_cast<uint64_t>(std::max(1u, surface->desc.width));
        scissor.height = static_cast<uint64_t>(std::max(1u, surface->desc.height));
      }
      encoder.setScissorRect(scissor);
      if (ffLayout && ffLayout->preTransformed) {
        encoder.setCullMode(WMTCullModeNone);
      } else {
        encoder.setCullMode(static_cast<WMTCullMode>(toCullMode(
            draw.rs.values.contains(RS_CULL_MODE) ? draw.rs.values.at(RS_CULL_MODE) : 1u)));
      }
    }
    static std::atomic<int> ffTraceRemaining{fixedFunctionTraceBudget()};
    const u32 primitiveCount = std::max<u32>(1, draw.primitiveCount);
    const uint64_t vertexCount =
        static_cast<uint64_t>(std::max(1u, primitiveVertexCount(draw.primitiveType, primitiveCount)));
    const bool indexedDraw = draw.indexBuffer || !draw.userIndexData.empty();
    WMT::Reference<WMT::Buffer> transientVertexBuffer;
    std::span<const u8> vertexBytes;
    WMT::Buffer vertexBuffer{};
    uint64_t vertexBufferOffset = 0;
    auto makeTransientBuffer = [&](const void* data, size_t len) -> WMT::Reference<WMT::Buffer> {
      WMTBufferInfo bi{};
      bi.length = len;
      bi.options = WMTResourceStorageModeShared;
      bi.memory.set((void *)data);
      return wrappedDevice_.newBuffer(bi);
    };
    if (!draw.userVertexData.empty()) {
      transientVertexBuffer = makeTransientBuffer(draw.userVertexData.data(),
                                                   draw.userVertexData.size());
      vertexBuffer = transientVertexBuffer;
      vertexBufferOffset = draw.vertexDecl.streams[0].offset;
      vertexBytes = draw.userVertexData;
    } else if (draw.vertexDecl.streams[0].buffer) {
      if (auto* buffer = findBufferUnlocked(draw.vertexDecl.streams[0].buffer->handle().value);
          buffer && buffer->buffer) {
        vertexBuffer = WMT::Buffer{buffer->buffer.handle};
        vertexBufferOffset = draw.vertexDecl.streams[0].offset;
        if (!buffer->shadow.empty()) {
          vertexBytes = buffer->shadow;
        } else if (buffer->contents) {
          vertexBytes = std::span<const u8>(static_cast<const u8*>(buffer->contents),
                                            static_cast<size_t>(buffer->desc.size));
        }
      } else {
        const auto bytes = draw.vertexDecl.streams[0].buffer->bytes();
        if (!bytes.empty()) {
          transientVertexBuffer = makeTransientBuffer(bytes.data(), bytes.size());
          vertexBuffer = transientVertexBuffer;
          vertexBufferOffset = draw.vertexDecl.streams[0].offset;
          vertexBytes = bytes;
        }
      }
    }
    if (traceEncode && !ffLayout && !vertexBytes.empty() && !draw.vertexDecl.elements.empty()) {
      auto readF32 = [&](size_t absoluteOffset) {
        float value = 0.0f;
        if (absoluteOffset + sizeof(float) <= vertexBytes.size()) {
          std::memcpy(&value, vertexBytes.data() + absoluteOffset, sizeof(float));
        }
        return value;
      };
      auto readU32 = [&](size_t absoluteOffset) {
        u32 value = 0;
        if (absoluteOffset + sizeof(u32) <= vertexBytes.size()) {
          std::memcpy(&value, vertexBytes.data() + absoluteOffset, sizeof(u32));
        }
        return value;
      };

      std::optional<u32> positionOffset;
      std::optional<u32> colorOffset;
      std::optional<u32> texcoord0Offset;
      for (const auto& element : draw.vertexDecl.elements) {
        if (!positionOffset && element.usage == kD3DDeclUsagePosition && element.usageIndex == 0 &&
            element.type == kD3DDeclTypeFloat4) {
          positionOffset = element.offset;
        } else if (!colorOffset && element.usage == kD3DDeclUsageColor && element.usageIndex == 0 &&
                   element.type == kD3DDeclTypeD3DColor) {
          colorOffset = element.offset;
        } else if (!texcoord0Offset && element.usage == kD3DDeclUsageTexcoord && element.usageIndex == 0 &&
                   element.type == kD3DDeclTypeFloat4) {
          texcoord0Offset = element.offset;
        }
      }

      if (positionOffset && texcoord0Offset) {
        const size_t stride = static_cast<size_t>(computeVertexDeclStride(draw));
        const size_t streamBase = static_cast<size_t>(draw.vertexDecl.streams[0].offset);
        std::ostringstream trace;
        trace << "[dxmt9-encode-verts] seq=" << static_cast<unsigned long long>(seqId)
              << " startVertex=" << draw.startVertex
              << " baseVertex=" << draw.baseVertexIndex
              << " stride=" << stride
              << " bytes=" << vertexBytes.size();
        const u32 tracedVertexCount = std::min<u32>(static_cast<u32>(vertexCount), 6u);
        for (u32 i = 0; i < tracedVertexCount; ++i) {
          const size_t base = streamBase +
                              static_cast<size_t>(draw.startVertex + i) * stride;
          trace << " v" << i << "=("
                << readF32(base + *positionOffset + 0) << ","
                << readF32(base + *positionOffset + 4) << ","
                << readF32(base + *positionOffset + 8) << ","
                << readF32(base + *positionOffset + 12) << ")";
          if (colorOffset) {
            trace << " c=0x" << std::hex << readU32(base + *colorOffset) << std::dec;
          }
          trace << " uv=("
                << readF32(base + *texcoord0Offset + 0) << ","
                << readF32(base + *texcoord0Offset + 4) << ","
                << readF32(base + *texcoord0Offset + 8) << ","
                << readF32(base + *texcoord0Offset + 12) << ")";
        }
        emitQueueTraceLine(trace.str());
      }
    }
    if (ffLayout) {
      if (!vertexBuffer) {
        if (traceEncode) {
          emitQueueTraceLine("[dxmt9-encode] seq=" + std::to_string(seqId) + " skipped reason=no-vertex-buffer");
        }
        return;
      }
      uniforms->vertexStreamOffset = 0;
      uniforms->vertexStreamStride =
          draw.vertexDecl.streams[0].stride ? draw.vertexDecl.streams[0].stride : ffLayout->stride;
      if (!indexedDraw && uniforms->vertexStreamStride != 0u) {
        vertexBufferOffset += static_cast<uint64_t>(draw.startVertex) *
                              static_cast<uint64_t>(uniforms->vertexStreamStride);
        uniforms->vertexBaseIndex = 0;
      } else {
        uniforms->vertexBaseIndex = indexedDraw ? draw.baseVertexIndex : static_cast<i32>(draw.startVertex);
      }
      if (ffLayout->preTransformed) {
        if (auto* targetSurface = findSurfaceUnlocked(draw.rts.color[0].handle.value); targetSurface) {
          uniforms->viewportOrigin = {0.0f, 0.0f};
          uniforms->viewportSize = {static_cast<f32>(std::max(1u, targetSurface->desc.width)),
                                    static_cast<f32>(std::max(1u, targetSurface->desc.height))};
        }
      }
      {
        WMTBufferInfo bi{}; bi.length = sizeof(DrawUniforms);
        bi.options = WMTResourceStorageModeShared; bi.memory.set((void *)uniforms);
        transientUniformBuffer = wrappedDevice_.newBuffer(bi);
      }
      if (!transientUniformBuffer) {
        return;
      }
      encoder.setVertexBuffer(transientUniformBuffer, 0, 0);
      encoder.setFragmentBuffer(transientUniformBuffer, 0, 0);
      encoder.setVertexBuffer(vertexBuffer, vertexBufferOffset, 1);

      const u64 ffTraceTex0 = fixedFunctionTraceTextureHandle();
      const bool forceTrace =
          ffTraceTex0 != 0 && draw.textures[0].handle && draw.textures[0].handle.value == ffTraceTex0;
      if ((forceTrace || ffTraceRemaining.load(std::memory_order_relaxed) > 0) && !vertexBytes.empty()) {
        bool shouldTrace = forceTrace;
        if (!shouldTrace) {
          int expected = ffTraceRemaining.load(std::memory_order_relaxed);
          while (expected > 0 &&
                 !ffTraceRemaining.compare_exchange_weak(expected, expected - 1, std::memory_order_relaxed)) {
          }
          shouldTrace = expected > 0;
        }
        if (shouldTrace) {
          std::ostringstream trace;
          const auto stageStateValue = [&](u32 key, u32 fallback) -> u32 {
            const auto it = draw.textures[0].stageStates.find(key);
            return it != draw.textures[0].stageStates.end() ? it->second : fallback;
          };
          const auto stageStateValueAt = [&](size_t stageIndex, u32 key, u32 fallback) -> u32 {
            if (stageIndex >= draw.textures.size()) {
              return fallback;
            }
            const auto it = draw.textures[stageIndex].stageStates.find(key);
            return it != draw.textures[stageIndex].stageStates.end() ? it->second : fallback;
          };
          trace << "[dxmt9-ffp] seq=" << static_cast<unsigned long long>(seqId)
                << " fvf=0x" << std::hex << draw.vertexDecl.fvf << std::dec
                << " ffLayout=1"
                << " preT=" << (ffLayout->preTransformed ? 1 : 0)
                << " baseVertex=" << draw.baseVertexIndex
                << " startIndex=" << draw.startIndex
                << " primCount=" << draw.primitiveCount
                << " stride=" << uniforms->vertexStreamStride
                << " viewport=(" << uniforms->viewportOrigin[0] << "," << uniforms->viewportOrigin[1]
                << " " << uniforms->viewportSize[0] << "x" << uniforms->viewportSize[1] << ")"
                << " zEnable=" << (draw.rs.values.contains(RS_Z_ENABLE) ? draw.rs.values.at(RS_Z_ENABLE) : 0u)
                << " zFunc=" << (draw.rs.values.contains(RS_Z_FUNC) ? draw.rs.values.at(RS_Z_FUNC) : 0u)
                << " alphaTest="
                << (draw.rs.values.contains(RS_ALPHA_TEST_ENABLE) ? draw.rs.values.at(RS_ALPHA_TEST_ENABLE) : 0u)
                << " alphaFunc="
                << (draw.rs.values.contains(RS_ALPHA_FUNC) ? draw.rs.values.at(RS_ALPHA_FUNC)
                                                           : static_cast<u32>(CompareFunc::Always))
                << " alphaRef="
                << (draw.rs.values.contains(RS_ALPHA_REF) ? draw.rs.values.at(RS_ALPHA_REF) : 0u)
                << " alphaBlend="
                << (draw.rs.values.contains(RS_ALPHABLEND_ENABLE) ? draw.rs.values.at(RS_ALPHABLEND_ENABLE) : 0u)
                << " srcBlend=" << (draw.rs.values.contains(RS_SRC_BLEND) ? draw.rs.values.at(RS_SRC_BLEND) : 0u)
                << " dstBlend=" << (draw.rs.values.contains(RS_DEST_BLEND) ? draw.rs.values.at(RS_DEST_BLEND) : 0u)
                << " tci0=0x" << std::hex
                << stageStateValue(TSS_TEXCOORD_INDEX, 0u)
                << std::dec
                << " ttff0=0x" << std::hex
                << stageStateValue(TSS_TEXTURE_TRANSFORM_FLAGS, 0u)
                << std::dec
                << " colorOp0=" << stageStateValue(TSS_COLOR_OP, static_cast<u32>(TextureOp::Disable))
                << " colorArg10=" << stageStateValue(TSS_COLOR_ARG1, 0u)
                << " colorArg20=" << stageStateValue(TSS_COLOR_ARG2, 0u)
                << " alphaOp0=" << stageStateValue(TSS_ALPHA_OP, static_cast<u32>(TextureOp::Disable))
                << " alphaArg10=" << stageStateValue(TSS_ALPHA_ARG1, 0u)
                << " alphaArg20=" << stageStateValue(TSS_ALPHA_ARG2, 0u)
                << " colorOp1=" << stageStateValueAt(1, TSS_COLOR_OP, static_cast<u32>(TextureOp::Disable))
                << " colorArg11=" << stageStateValueAt(1, TSS_COLOR_ARG1, 0u)
                << " colorArg21=" << stageStateValueAt(1, TSS_COLOR_ARG2, 0u)
                << " alphaOp1=" << stageStateValueAt(1, TSS_ALPHA_OP, static_cast<u32>(TextureOp::Disable))
                << " alphaArg11=" << stageStateValueAt(1, TSS_ALPHA_ARG1, 0u)
                << " alphaArg21=" << stageStateValueAt(1, TSS_ALPHA_ARG2, 0u)
                << " elems=" << draw.vertexDecl.elements.size()
                << " tfactor=0x"
                << std::hex
                << (draw.rs.values.contains(RS_TEXTURE_FACTOR) ? draw.rs.values.at(RS_TEXTURE_FACTOR) : 0u)
                << std::dec;
          trace << " texM0=["
                << uniforms->ffpTextureTransforms[0][0][0] << "," << uniforms->ffpTextureTransforms[0][0][1] << ","
                << uniforms->ffpTextureTransforms[0][0][2] << "," << uniforms->ffpTextureTransforms[0][0][3] << ";"
                << uniforms->ffpTextureTransforms[0][1][0] << "," << uniforms->ffpTextureTransforms[0][1][1] << ","
                << uniforms->ffpTextureTransforms[0][1][2] << "," << uniforms->ffpTextureTransforms[0][1][3] << ";"
                << uniforms->ffpTextureTransforms[0][2][0] << "," << uniforms->ffpTextureTransforms[0][2][1] << ","
                << uniforms->ffpTextureTransforms[0][2][2] << "," << uniforms->ffpTextureTransforms[0][2][3] << ";"
                << uniforms->ffpTextureTransforms[0][3][0] << "," << uniforms->ffpTextureTransforms[0][3][1] << ","
                << uniforms->ffpTextureTransforms[0][3][2] << "," << uniforms->ffpTextureTransforms[0][3][3] << "]";
          for (size_t i = 0; i < draw.vertexDecl.elements.size(); ++i) {
            const auto& e = draw.vertexDecl.elements[i];
            trace << " e" << i << "={s=" << e.stream
                  << ",off=" << e.offset
                  << ",type=" << e.type
                  << ",usage=" << e.usage
                  << ",idx=" << e.usageIndex
                  << "}";
          }

          auto readF32 = [&](size_t absoluteOffset) {
            float value = 0.0f;
            if (absoluteOffset + sizeof(float) <= vertexBytes.size()) {
              std::memcpy(&value, vertexBytes.data() + absoluteOffset, sizeof(float));
            }
            return value;
          };
          auto readU32 = [&](size_t absoluteOffset) {
            u32 value = 0;
            if (absoluteOffset + sizeof(u32) <= vertexBytes.size()) {
              std::memcpy(&value, vertexBytes.data() + absoluteOffset, sizeof(u32));
            }
            return value;
          };

          const size_t stride = static_cast<size_t>(uniforms->vertexStreamStride ? uniforms->vertexStreamStride
                                                                                : ffLayout->stride);
          const u32 tracedVertexCount = std::min<u32>(static_cast<u32>(vertexCount), 24u);
          for (u32 i = 0; i < tracedVertexCount; ++i) {
            const size_t base = static_cast<size_t>(draw.vertexDecl.streams[0].offset) +
                                static_cast<size_t>(draw.baseVertexIndex + static_cast<int>(i)) * stride;
            trace << " v" << i << "=("
                  << readF32(base + ffLayout->positionOffset + 0) << ","
                  << readF32(base + ffLayout->positionOffset + 4) << ","
                  << readF32(base + ffLayout->positionOffset + 8) << ","
                  << readF32(base + ffLayout->positionOffset + 12) << ")";
            if (ffLayout->hasDiffuse) {
              const u32 rgba = readU32(base + ffLayout->diffuseOffset);
              trace << " c" << i << "=0x" << std::hex << rgba << std::dec;
            }
            if (ffLayout->hasTexcoord[0]) {
              trace << " uv" << i << "=("
                    << readF32(base + ffLayout->texcoordOffset[0] + 0) << ","
                    << readF32(base + ffLayout->texcoordOffset[0] + 4) << ")";
            }
          }

          if (draw.indexBuffer) {
            const auto* indexRecord = findBufferUnlocked(draw.indexBuffer.value);
            std::span<const u8> indexBytes;
            if (indexRecord && !indexRecord->shadow.empty()) {
              indexBytes = indexRecord->shadow;
            } else if (indexRecord && indexRecord->buffer && indexRecord->contents) {
              indexBytes = std::span<const u8>(static_cast<const u8*>(indexRecord->contents),
                                               static_cast<size_t>(indexRecord->desc.size));
            }
            if (!indexBytes.empty()) {
              trace << " idx=";
              const size_t start = static_cast<size_t>(draw.startIndex) * indexElementSize(draw.indexType);
              const u32 tracedIndexCount =
                  std::min<u32>(primitiveCount * 3u, 36u);
              for (u32 i = 0; i < tracedIndexCount; ++i) {
                if (i) {
                  trace << ",";
                }
                if (draw.indexType == IndexType::UInt16 &&
                    start + static_cast<size_t>(i + 1) * sizeof(u16) <= indexBytes.size()) {
                  u16 index = 0;
                  std::memcpy(&index, indexBytes.data() + start + static_cast<size_t>(i) * sizeof(u16),
                              sizeof(u16));
                  trace << index;
                } else if (draw.indexType == IndexType::UInt32 &&
                           start + static_cast<size_t>(i + 1) * sizeof(u32) <= indexBytes.size()) {
                  u32 index = 0;
                  std::memcpy(&index, indexBytes.data() + start + static_cast<size_t>(i) * sizeof(u32),
                              sizeof(u32));
                  trace << index;
                } else {
                  trace << '?';
                }
              }
              trace << " ref=";
              const u32 tracedRefs = std::min<u32>(12u, tracedIndexCount);
              for (u32 i = 0; i < tracedRefs; ++i) {
                u32 vertexIndex = 0;
                bool haveIndex = false;
                if (draw.indexType == IndexType::UInt16 &&
                    start + static_cast<size_t>(i + 1) * sizeof(u16) <= indexBytes.size()) {
                  u16 index = 0;
                  std::memcpy(&index, indexBytes.data() + start + static_cast<size_t>(i) * sizeof(u16),
                              sizeof(u16));
                  vertexIndex = static_cast<u32>(index);
                  haveIndex = true;
                } else if (draw.indexType == IndexType::UInt32 &&
                           start + static_cast<size_t>(i + 1) * sizeof(u32) <= indexBytes.size()) {
                  std::memcpy(&vertexIndex, indexBytes.data() + start + static_cast<size_t>(i) * sizeof(u32),
                              sizeof(u32));
                  haveIndex = true;
                }
                if (!haveIndex) {
                  break;
                }
                const size_t refBase = static_cast<size_t>(draw.vertexDecl.streams[0].offset) +
                                       static_cast<size_t>(draw.baseVertexIndex + static_cast<int>(vertexIndex)) *
                                           stride;
                trace << " r" << i << "#" << vertexIndex << "=("
                      << readF32(refBase + ffLayout->positionOffset + 0) << ","
                      << readF32(refBase + ffLayout->positionOffset + 4) << ","
                      << readF32(refBase + ffLayout->positionOffset + 8) << ","
                      << readF32(refBase + ffLayout->positionOffset + 12) << ")";
                if (ffLayout->hasTexcoord[0]) {
                  trace << " uv=("
                        << readF32(refBase + ffLayout->texcoordOffset[0] + 0) << ","
                        << readF32(refBase + ffLayout->texcoordOffset[0] + 4) << ")";
                }
                if (ffLayout->hasDiffuse) {
                  const u32 rgba = readU32(refBase + ffLayout->diffuseOffset);
                  trace << " c=0x" << std::hex << rgba << std::dec;
                }
              }
            }
          }
          trace << " tex0=";
          if (draw.textures[0].handle) {
            trace << static_cast<unsigned long long>(draw.textures[0].handle.value);
          } else {
            trace << 0;
          }
          trace << " tex1=";
          if (draw.textures.size() > 1 && draw.textures[1].handle) {
            trace << static_cast<unsigned long long>(draw.textures[1].handle.value);
          } else {
            trace << 0;
          }
          emitQueueTraceLine(trace.str());
        }
      }
    }
    if (vertexBuffer && !ffLayout) {
      const u64 ffTraceTex0 = fixedFunctionTraceTextureHandle();
      const bool forceTrace =
          ffTraceTex0 != 0 && draw.textures[0].handle && draw.textures[0].handle.value == ffTraceTex0;
      if (forceTrace) {
        std::ostringstream trace;
        trace << "[dxmt9-ffp] seq=" << static_cast<unsigned long long>(seqId)
              << " fvf=0x" << std::hex << draw.vertexDecl.fvf << std::dec
              << " ffLayout=" << (ffLayout ? 1 : 0)
              << " baseVertex=" << draw.baseVertexIndex
              << " startIndex=" << draw.startIndex
              << " primCount=" << draw.primitiveCount
              << " stride="
              << (ffLayout ? (draw.vertexDecl.streams[0].stride ? draw.vertexDecl.streams[0].stride : ffLayout->stride)
                           : computeVertexDeclStride(draw))
              << " elems=" << draw.vertexDecl.elements.size();
        for (size_t i = 0; i < draw.vertexDecl.elements.size(); ++i) {
          const auto& e = draw.vertexDecl.elements[i];
          trace << " e" << i << "={s=" << e.stream
                << ",off=" << e.offset
                << ",type=" << e.type
                << ",usage=" << e.usage
                << ",idx=" << e.usageIndex
                << "}";
        }
        emitQueueTraceLine(trace.str());
      }
      uniforms->vertexStreamOffset = 0;
      uniforms->vertexStreamStride =
          ffLayout ? (draw.vertexDecl.streams[0].stride ? draw.vertexDecl.streams[0].stride : ffLayout->stride)
                   : computeVertexDeclStride(draw);
      if (!indexedDraw && uniforms->vertexStreamStride != 0u) {
        vertexBufferOffset += static_cast<uint64_t>(draw.startVertex) *
                              static_cast<uint64_t>(uniforms->vertexStreamStride);
        uniforms->vertexBaseIndex = 0;
      } else {
        uniforms->vertexBaseIndex = indexedDraw ? draw.baseVertexIndex : static_cast<i32>(draw.startVertex);
      }
      {
        WMTBufferInfo bi{}; bi.length = sizeof(DrawUniforms);
        bi.options = WMTResourceStorageModeShared; bi.memory.set((void *)uniforms);
        transientUniformBuffer = wrappedDevice_.newBuffer(bi);
      }
      if (!transientUniformBuffer) {
        return;
      }
      encoder.setVertexBuffer(transientUniformBuffer, 0, 0);
      encoder.setFragmentBuffer(transientUniformBuffer, 0, 0);
      encoder.setVertexBuffer(vertexBuffer, vertexBufferOffset, 1);
    }
    for (size_t stage = 0; stage < kMaxTextureStages; ++stage) {
      if (!draw.textures[stage].handle) {
        continue;
      }
      if (const u64 skipped = skippedTextureHandle();
          skipped != 0ull && draw.textures[stage].handle.value == skipped) {
        if (traceEncode || shouldTraceTexture(draw.textures[stage].handle)) {
          std::ostringstream out;
          out << "[dxmt9-debug] skip draw seq=" << static_cast<unsigned long long>(seqId)
              << " tex" << stage << "=" << static_cast<unsigned long long>(draw.textures[stage].handle.value);
          emitQueueTraceLine(out.str());
        }
        return;
      }
      if (auto* texture = findTextureUnlocked(draw.textures[stage].handle.value); texture && texture->texture) {
        if (shouldTraceTexture(draw.textures[stage].handle)) {
          std::ostringstream out;
          out << "[dxmt9-texture] bind stage=" << stage
              << " handle=0x" << std::hex << draw.textures[stage].handle.value << std::dec
              << " format=" << static_cast<unsigned>(texture->desc.format)
              << " size=" << texture->desc.width << "x" << texture->desc.height
              << " levels=" << texture->desc.levels;
          emitTextureTraceLine(out.str());
        }
        encoder.setFragmentTexture(WMT::Texture{texture->texture.handle}, (uint8_t)stage);
      }
      auto sampler = makeSampler(draw.samplers[stage]);
      if (sampler) {
        encoder.setFragmentSamplerState(sampler, (uint8_t)stage);
      }
    }
    const auto primitiveType = toPrimitiveType(draw.primitiveType);
    bool expandedIndexedDraw = false;
    if (traceEncode) {
      std::ostringstream out;
      out << "[dxmt9-encode] seq=" << static_cast<unsigned long long>(seqId)
          << " draw rt0=" << static_cast<unsigned long long>(draw.rts.color[0].handle.value)
          << " ds=" << static_cast<unsigned long long>(draw.rts.depthStencil.handle.value)
          << " tex0=" << static_cast<unsigned long long>(draw.textures[0].handle.value)
          << " ffLayout=" << (ffLayout ? 1 : 0)
          << " indexed=" << (indexedDraw ? 1 : 0)
          << " primType=" << static_cast<unsigned>(draw.primitiveType)
          << " primCount=" << draw.primitiveCount
          << " vertexCount=" << static_cast<unsigned long long>(vertexCount)
          << " vertexStreamStride=" << uniforms->vertexStreamStride
          << " vertexBufferOffset=" << vertexBufferOffset
          << " vertexStreamOffset=" << uniforms->vertexStreamOffset
          << " vertexBaseIndex=" << uniforms->vertexBaseIndex
          << " colorWrite="
          << (draw.rs.values.contains(RS_COLOR_WRITE_ENABLE) ? draw.rs.values.at(RS_COLOR_WRITE_ENABLE) : 0xfu)
          << " zEnable=" << (draw.rs.values.contains(RS_Z_ENABLE) ? draw.rs.values.at(RS_Z_ENABLE) : 0u)
          << " zWrite=" << (draw.rs.values.contains(RS_Z_WRITE_ENABLE) ? draw.rs.values.at(RS_Z_WRITE_ENABLE) : 0u)
          << " zFunc=" << (draw.rs.values.contains(RS_Z_FUNC) ? draw.rs.values.at(RS_Z_FUNC) : 0u)
          << " alphaBlend="
          << (draw.rs.values.contains(RS_ALPHABLEND_ENABLE) ? draw.rs.values.at(RS_ALPHABLEND_ENABLE) : 0u)
          << " srcBlend=" << (draw.rs.values.contains(RS_SRC_BLEND) ? draw.rs.values.at(RS_SRC_BLEND) : 0u)
          << " dstBlend=" << (draw.rs.values.contains(RS_DEST_BLEND) ? draw.rs.values.at(RS_DEST_BLEND) : 0u)
          << " forceVisible=" << (debugForceVisibleDraw() ? 1 : 0);
      emitQueueTraceLine(out.str());
    }
    if (indexedDraw) {
      std::span<const u8> indexBytes;
      if (!draw.userIndexData.empty()) {
        indexBytes = draw.userIndexData;
      } else {
        auto* indexRecord = findBufferUnlocked(draw.indexBuffer.value);
        if (indexRecord && !indexRecord->shadow.empty()) {
          indexBytes = indexRecord->shadow;
        } else if (indexRecord && indexRecord->buffer && indexRecord->contents) {
          indexBytes = std::span<const u8>(static_cast<const u8*>(indexRecord->contents),
                                           static_cast<size_t>(indexRecord->desc.size));
        }
      }
      const size_t stride = static_cast<size_t>(ffLayout ? (uniforms->vertexStreamStride ? uniforms->vertexStreamStride
                                                                                         : ffLayout->stride)
                                                         : computeVertexDeclStride(draw));
      const size_t streamBase = static_cast<size_t>(draw.vertexDecl.streams[0].offset);
      const size_t firstIndexByte = static_cast<size_t>(draw.startIndex) * indexElementSize(draw.indexType);
      if (debugForceExpandIndexed()) {
        std::ostringstream out;
        out << "[dxmt9-expanded-check] seq=" << static_cast<unsigned long long>(seqId)
            << " tex0=" << static_cast<unsigned long long>(draw.textures[0].handle.value)
            << " ff=" << (ffLayout ? 1 : 0)
            << " vertexBytes=" << vertexBytes.size()
            << " indexBytes=" << indexBytes.size()
            << " stride=" << stride
            << " startIndex=" << draw.startIndex
            << " baseVertex=" << draw.baseVertexIndex;
        emitQueueTraceLine(out.str());
      }
      if (!vertexBytes.empty() && !indexBytes.empty() && stride != 0) {
        std::vector<u8> expandedVertices(static_cast<size_t>(vertexCount) * stride, 0);
        for (uint64_t i = 0; i < vertexCount; ++i) {
          i32 vertexIndex = draw.baseVertexIndex;
          bool haveIndex = false;
          if (draw.indexType == IndexType::UInt16 &&
              firstIndexByte + static_cast<size_t>(i + 1) * sizeof(u16) <= indexBytes.size()) {
            u16 index = 0;
            std::memcpy(&index, indexBytes.data() + firstIndexByte + static_cast<size_t>(i) * sizeof(u16),
                        sizeof(u16));
            vertexIndex += static_cast<i32>(index);
            haveIndex = true;
          } else if (draw.indexType == IndexType::UInt32 &&
                     firstIndexByte + static_cast<size_t>(i + 1) * sizeof(u32) <= indexBytes.size()) {
            u32 index = 0;
            std::memcpy(&index, indexBytes.data() + firstIndexByte + static_cast<size_t>(i) * sizeof(u32),
                        sizeof(u32));
            vertexIndex += static_cast<i32>(index);
            haveIndex = true;
          }
          if (!haveIndex || vertexIndex < 0) {
            continue;
          }
          const size_t sourceOffset = streamBase + static_cast<size_t>(vertexIndex) * stride;
          if (sourceOffset + stride > vertexBytes.size()) {
            continue;
          }
          std::memcpy(expandedVertices.data() + static_cast<size_t>(i) * stride,
                      vertexBytes.data() + sourceOffset, stride);
        }
        {
          WMTBufferInfo bi{};
          bi.length = expandedVertices.size();
          bi.options = WMTResourceStorageModeShared;
          bi.memory.set((void *)expandedVertices.data());
          transientVertexBuffer = wrappedDevice_.newBuffer(bi);
        }
        if (transientVertexBuffer) {
          encoder.setVertexBuffer(transientVertexBuffer, 0, 1);
          if (ffLayout && ffLayout->preTransformed && vertexCount >= 6 && draw.textures[0].handle != Handle{}) {
            const bool traceExpanded = [] {
              const char* env = std::getenv("DXMT_TRACE_FVF_EXPANDED");
              return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
            }();
            if (traceExpanded) {
              auto readExpandedF32 = [&](size_t absoluteOffset) {
                float value = 0.0f;
                if (absoluteOffset + sizeof(float) <= expandedVertices.size()) {
                  std::memcpy(&value, expandedVertices.data() + absoluteOffset, sizeof(float));
                }
                return value;
              };
              std::ostringstream trace;
              trace << "[dxmt9-expanded] seq=" << static_cast<unsigned long long>(seqId)
                    << " tex0=" << static_cast<unsigned long long>(draw.textures[0].handle.value)
                    << " stride=" << stride;
              for (uint64_t i = 0; i < std::min<uint64_t>(vertexCount, 6); ++i) {
                const size_t base = static_cast<size_t>(i) * stride;
                trace << " v" << i << "=("
                      << readExpandedF32(base + ffLayout->positionOffset + 0) << ","
                      << readExpandedF32(base + ffLayout->positionOffset + 4) << ","
                      << readExpandedF32(base + ffLayout->positionOffset + 8) << ","
                      << readExpandedF32(base + ffLayout->positionOffset + 12) << ")";
                if (ffLayout->hasTexcoord[0]) {
                  trace << " uv" << i << "=("
                        << readExpandedF32(base + ffLayout->texcoordOffset[0] + 0) << ","
                        << readExpandedF32(base + ffLayout->texcoordOffset[0] + 4) << ")";
                }
              }
              emitQueueTraceLine(trace.str());
            }
          }
          vertexBytes = std::span<const u8>(expandedVertices.data(), expandedVertices.size());
          uniforms->vertexStreamOffset = 0;
          uniforms->vertexBaseIndex = 0;
          {
            WMTBufferInfo bi{}; bi.length = sizeof(DrawUniforms);
            bi.options = WMTResourceStorageModeShared; bi.memory.set((void *)uniforms);
            transientUniformBuffer = wrappedDevice_.newBuffer(bi);
          }
          if (!transientUniformBuffer) {
            return;
          }
          encoder.setVertexBuffer(transientUniformBuffer, 0, 0);
          encoder.setFragmentBuffer(transientUniformBuffer, 0, 0);
          expandedIndexedDraw = true;
        }
      }
      if (debugForceExpandIndexed()) {
        std::ostringstream out;
        out << "[dxmt9-expanded-check] seq=" << static_cast<unsigned long long>(seqId)
            << " tex0=" << static_cast<unsigned long long>(draw.textures[0].handle.value)
            << " expanded=" << (expandedIndexedDraw ? 1 : 0);
        emitQueueTraceLine(out.str());
      }
      if (expandedIndexedDraw) {
        encoder.drawPrimitives(primitiveType, 0, (uint64_t)vertexCount);
        return;
      }
      WMT::Reference<WMT::Buffer> transientIndexBuffer;
      WMT::Buffer indexBuffer{};
      uint64_t indexBufferOffset = static_cast<uint64_t>(draw.startIndex) * indexElementSize(draw.indexType);
      if (!draw.userIndexData.empty()) {
        WMTBufferInfo bi{}; bi.length = draw.userIndexData.size();
        bi.options = WMTResourceStorageModeShared;
        bi.memory.set((void *)draw.userIndexData.data());
        transientIndexBuffer = wrappedDevice_.newBuffer(bi);
        indexBuffer = transientIndexBuffer;
      } else {
        auto* buffer = findBufferUnlocked(draw.indexBuffer.value);
        if (buffer && buffer->buffer) {
          indexBuffer = WMT::Buffer{buffer->buffer.handle};
        } else if (buffer && !buffer->shadow.empty()) {
          WMTBufferInfo bi{}; bi.length = buffer->shadow.size();
          bi.options = WMTResourceStorageModeShared;
          bi.memory.set((void *)buffer->shadow.data());
          transientIndexBuffer = wrappedDevice_.newBuffer(bi);
          indexBuffer = transientIndexBuffer;
        }
      }
      if (indexBuffer) {
        encoder.drawIndexedPrimitives(primitiveType, toIndexType(draw.indexType),
                                      (uint64_t)vertexCount, indexBuffer, indexBufferOffset,
                                      1, 0, 0);
        return;
      }
    }
    encoder.drawPrimitives(primitiveType, 0, (uint64_t)vertexCount);
  }

  void encodeClearPass(WMT::CommandBuffer& commandBuffer, const ClearDesc& clear) {
    if (clear.colorAttachments[0].handle == Handle{} && clear.depthStencil.handle == Handle{}) {
      return;
    }
    auto* surface = findSurfaceUnlocked(clear.colorAttachments[0].handle.value);
    if (!surface || !surface->texture) {
      return;
    }
    WMTRenderPassInfo passInfo{};
    passInfo.colors[0].texture = surface->texture.handle;
    passInfo.colors[0].load_action = WMTLoadActionClear;
    passInfo.colors[0].store_action = WMTStoreActionStore;
    if (surface->resolveTexture) {
      passInfo.colors[0].resolve_texture = surface->resolveTexture.handle;
      passInfo.colors[0].store_action = WMTStoreActionMultisampleResolve;
    }
    passInfo.colors[0].clear_color = WMTClearColor{clear.color.r, clear.color.g,
                                                   clear.color.b, clear.color.a};
    auto encoder = commandBuffer.renderCommandEncoder(passInfo);
    if (encoder) {
      encoder.endEncoding();
    }
  }

  void encodeColorFillPass(WMT::CommandBuffer& commandBuffer, const ClearDesc& clear) {
    encodeClearPass(commandBuffer, clear);
  }

  void encodeColorFill(WMT::CommandBuffer& commandBuffer, const ColorFillDesc& fill) {
    auto* surface = findSurfaceUnlocked(fill.destination.value);
    if (!surface || !surface->texture) {
      return;
    }
    WMTRenderPassInfo passInfo{};
    passInfo.colors[0].texture = surface->texture.handle;
    passInfo.colors[0].load_action = fill.hasRect ? WMTLoadActionLoad : WMTLoadActionClear;
    passInfo.colors[0].store_action = WMTStoreActionStore;
    if (surface->resolveTexture) {
      passInfo.colors[0].resolve_texture = surface->resolveTexture.handle;
      passInfo.colors[0].store_action = WMTStoreActionMultisampleResolve;
    }
    if (!fill.hasRect) {
      passInfo.colors[0].clear_color = WMTClearColor{fill.color.r, fill.color.g,
                                                     fill.color.b, fill.color.a};
    }
    auto encoder = commandBuffer.renderCommandEncoder(passInfo);
    if (!encoder) {
      return;
    }
    if (fill.hasRect) {
      WMTScissorRect rect{};
      rect.x = static_cast<uint64_t>(std::max(0, fill.rect.left));
      rect.y = static_cast<uint64_t>(std::max(0, fill.rect.top));
      rect.width = static_cast<uint64_t>(std::max(0, fill.rect.right - fill.rect.left));
      rect.height = static_cast<uint64_t>(std::max(0, fill.rect.bottom - fill.rect.top));
      encoder.setScissorRect(rect);
      auto pipeline = pipelineForColorFill(fill.color,
                        static_cast<u32>(toPixelFormat(surface->desc.format, limits_))).get();
      if (pipeline) {
        encoder.setRenderPipelineState(pipeline);
        encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
      }
    }
    encoder.endEncoding();
  }

  void encodeSurfaceCopy(WMT::CommandBuffer& commandBuffer, const SurfaceCopyDesc& copy) {
    auto* src = findSurfaceUnlocked(copy.source.value);
    auto* dst = findSurfaceUnlocked(copy.destination.value);
    if (!src || !dst || !src->texture || !dst->texture) {
      return;
    }
    const uint32_t srcW = static_cast<uint32_t>(std::max(1, copy.sourceRect.right - copy.sourceRect.left));
    const uint32_t srcH = static_cast<uint32_t>(std::max(1, copy.sourceRect.bottom - copy.sourceRect.top));
    const uint32_t dstW = static_cast<uint32_t>(std::max(1, copy.destinationRect.right - copy.destinationRect.left));
    const uint32_t dstH = static_cast<uint32_t>(std::max(1, copy.destinationRect.bottom - copy.destinationRect.top));
    if (srcW == dstW && srcH == dstH) {
      auto blit = commandBuffer.blitCommandEncoder();
      if (!blit) return;
      WMTOrigin srcOrigin{(uint64_t)copy.sourceRect.left, (uint64_t)copy.sourceRect.top, 0};
      WMTSize srcSize{srcW, srcH, 1};
      WMTOrigin dstOrigin{(uint64_t)copy.destinationRect.left, (uint64_t)copy.destinationRect.top, 0};
      blit.copyFromTextureToTexture(WMT::Texture{src->texture.handle}, 0, copy.sourceLevel,
                                    srcOrigin, srcSize,
                                    WMT::Texture{dst->texture.handle}, 0, copy.destinationLevel,
                                    dstOrigin);
      blit.endEncoding();
    } else {
      encodeStretchRect(commandBuffer, {
          .source = copy.source,
          .destination = copy.destination,
          .sourceRect = copy.sourceRect,
          .destinationRect = copy.destinationRect,
          .linear = true,
          .sourceSampleCount = src->desc.multiSampleType == MultiSampleType::None ? 1u : sampleCount(src->desc.multiSampleType),
          .destinationSampleCount = dst->desc.multiSampleType == MultiSampleType::None ? 1u : sampleCount(dst->desc.multiSampleType),
      });
    }
  }

  void encodeStretchRect(WMT::CommandBuffer& commandBuffer, const StretchRectDesc& stretch) {
    auto* src = findSurfaceUnlocked(stretch.source.value);
    auto* dst = findSurfaceUnlocked(stretch.destination.value);
    if (!src || !dst || !src->texture || !dst->texture) {
      return;
    }
    {
      WMTRenderPassInfo passInfo{};
      passInfo.colors[0].texture = dst->texture.handle;
      passInfo.colors[0].load_action = WMTLoadActionLoad;
      passInfo.colors[0].store_action = WMTStoreActionStore;
      if (dst->resolveTexture) {
        passInfo.colors[0].resolve_texture = dst->resolveTexture.handle;
        passInfo.colors[0].store_action = WMTStoreActionMultisampleResolve;
      }
      auto encoder = commandBuffer.renderCommandEncoder(passInfo);
      if (!encoder) return;
      auto pipeline = pipelineForStretchRect(stretch,
                        static_cast<u32>(toPixelFormat(dst->desc.format, limits_))).get();
      if (!pipeline) {
        encoder.endEncoding();
        return;
      }
      encoder.setRenderPipelineState(pipeline);
      encoder.setFragmentTexture(WMT::Texture{src->texture.handle}, 0);
      auto sampler = makeSampler(stretch.linear);
      if (sampler) encoder.setFragmentSamplerState(sampler, 0);
      encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
      encoder.endEncoding();
    }
  }

  void encodeReadback(WMT::CommandBuffer& commandBuffer, const ReadbackDesc& readback) {
    auto* src = findSurfaceUnlocked(readback.source.value);
    auto* dst = findSurfaceUnlocked(readback.destination.value);
    if (!src || !dst || !src->texture) {
      return;
    }
    {
      auto blit = commandBuffer.blitCommandEncoder();
      if (!blit) return;
      WMT::Texture sourceTexture{src->resolveTexture ? src->resolveTexture.handle : src->texture.handle};
      const uint32_t w = static_cast<uint32_t>(std::max(1, readback.sourceRect.right - readback.sourceRect.left));
      const uint32_t h = static_cast<uint32_t>(std::max(1, readback.sourceRect.bottom - readback.sourceRect.top));
      if (!dst->texture) {
        blit.endEncoding();
        return;
      }
      WMTOrigin srcOrigin{(uint64_t)readback.sourceRect.left, (uint64_t)readback.sourceRect.top, 0};
      WMTSize srcSize{w, h, 1};
      WMTOrigin dstOrigin{0, 0, 0};
      blit.copyFromTextureToTexture(sourceTexture, 0, readback.sourceLevel,
                                    srcOrigin, srcSize,
                                    WMT::Texture{dst->texture.handle}, 0, 0, dstOrigin);
      blit.endEncoding();
    }
  }

  void encodePresent(WMT::CommandBuffer& commandBuffer, const SwapDesc& present, Handle sourceHandle, u64 seqId) {
    presenterState_.traceEvent("begin", seqId, present.window.value);
    if (queueTraceEnabled()) {
      std::ostringstream out;
      out << "[dxmt9-present] source"
          << " seq=" << static_cast<unsigned long long>(seqId)
          << " hwnd=" << static_cast<unsigned long long>(present.window.value)
          << " handle=0x" << std::hex << static_cast<unsigned long long>(sourceHandle.value) << std::dec;
      emitQueueTraceLine(out.str());
    }
    auto* source = findSurfaceUnlocked(sourceHandle.value);
    if (!source || !source->texture) {
      presenterState_.traceEvent("missing-source", seqId, present.window.value);
      return;
    }
    obj_handle_t sourceTextureHandle = source->resolveTexture ? source->resolveTexture.handle : source->texture.handle;
    u64 forcedTextureHandle = forcedPresentTextureHandle();
    if (forcedTextureHandle != 0ull) {
      if (auto* forced = findTextureUnlocked(forcedTextureHandle); forced && forced->texture) {
        sourceTextureHandle = forced->texture.handle;
        if (queueTraceEnabled()) {
          std::ostringstream out;
          out << "[dxmt9-present] force-texture"
              << " seq=" << static_cast<unsigned long long>(seqId)
              << " hwnd=" << static_cast<unsigned long long>(present.window.value)
              << " handle=0x" << std::hex << forcedTextureHandle << std::dec
              << " size=" << forced->desc.width << "x" << forced->desc.height
              << " fmt=" << static_cast<unsigned>(forced->desc.format);
          emitQueueTraceLine(out.str());
        }
      } else if (queueTraceEnabled()) {
        std::ostringstream out;
        out << "[dxmt9-present] force-texture-missing"
            << " seq=" << static_cast<unsigned long long>(seqId)
            << " hwnd=" << static_cast<unsigned long long>(present.window.value)
            << " handle=0x" << std::hex << forcedTextureHandle << std::dec;
        emitQueueTraceLine(out.str());
      }
    }
    if (queueTraceEnabled()) {
      std::ostringstream out;
      out << "[dxmt9-present] source.info"
          << " seq=" << static_cast<unsigned long long>(seqId)
          << " hwnd=" << static_cast<unsigned long long>(present.window.value)
          << " size=" << source->desc.width << "x" << source->desc.height
          << " fmt=" << static_cast<unsigned>(source->desc.format)
          << " sampleCount="
          << (source->desc.multiSampleType == MultiSampleType::None ? 1u : sampleCount(source->desc.multiSampleType));
      emitQueueTraceLine(out.str());
    }

    CAMetalLayer* layer = nullptr;
    if (present.window.value != 0) {
      layer = presenterState_.lookupLayer(present.window.value);
      if (!layer) {
        layer = presenterState_.ensureLayer(present.window.value, seqId);
      }
    }
    if (!layer) {
      presenterState_.traceEvent("missing-layer", seqId, present.window.value);
      return;
    }

    {
      WMTLayerProps props{};
      props.device = wrappedDevice_.handle;
      props.pixel_format = WMTPixelFormatBGRA8Unorm;
      props.opaque = true;
      props.framebuffer_only = false;
      props.drawable_width = std::max(1u, present.width);
      props.drawable_height = std::max(1u, present.height);
      props.display_sync_enabled = present.displaySyncEnabled;
      props.contents_scale = 1.0;
      MetalLayer_setProps((obj_handle_t)layer, &props);
      MetalLayer_setMaximumDrawableCount((obj_handle_t)layer, std::clamp(maxFrameLatency_, 1u, 3u));
    }

    presenterState_.traceEvent("nextDrawable.begin", seqId, present.window.value);
    WMT::MetalLayer wmtLayer{(obj_handle_t)layer};
    auto drawable = wmtLayer.nextDrawable();
    if (!drawable) {
      if (presentationStatusObserver_) presentationStatusObserver_(true);
      presenterState_.traceEvent("nextDrawable.nil", seqId, present.window.value);
      return;
    }
    if (presentationStatusObserver_) presentationStatusObserver_(false);
    presenterState_.traceEvent("nextDrawable.ok", seqId, present.window.value);

    auto drawableTex = drawable.texture();
    WMTRenderPassInfo passInfo{};
    passInfo.colors[0].texture = drawableTex.handle;
    passInfo.colors[0].load_action = WMTLoadActionDontCare;
    passInfo.colors[0].store_action = WMTStoreActionStore;
    auto encoder = commandBuffer.renderCommandEncoder(passInfo);
    if (!encoder) {
      presenterState_.traceEvent("encoder.nil", seqId, present.window.value);
      return;
    }
    auto pipeline = pipelineForPresent(source->desc.format).get();
    if (!pipeline) {
      encoder.endEncoding();
      presenterState_.traceEvent("pipeline.nil", seqId, present.window.value);
      return;
    }
    encoder.setRenderPipelineState(pipeline);
    encoder.setFragmentTexture(WMT::Texture{sourceTextureHandle}, 0);
    auto sampler = makeSampler(false);
    if (sampler) encoder.setFragmentSamplerState(sampler, 0);
    const double width = std::max(1u, present.width);
    const double height = std::max(1u, present.height);
    encoder.setViewport(WMTViewport{0.0, 0.0, width, height, 0.0, 1.0});
    encoder.setScissorRect(WMTScissorRect{0, 0, (uint64_t)width, (uint64_t)height});
    encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
    encoder.endEncoding();
    commandBuffer.presentDrawable(drawable);
    presenterState_.traceEvent("scheduled", seqId, present.window.value);
    backBufferDiscardAfterPresent_ = true;
  }

  void dumpTextureSnapshotUnlocked(Handle handle, const TextureDesc& desc,
                                   obj_handle_t sourceTextureHandle) {
    if (!sourceTextureHandle || !shouldDumpGpuTexture(handle) ||
        dumpedGpuTextures_.contains(handle.value)) {
      return;
    }
    if (desc.levels == 0 || desc.width == 0 || desc.height == 0) return;
    const char* path = gpuDumpTexturePath();
    if (!path || path[0] == '\0') return;
    if (desc.format != Format::A8R8G8B8 && desc.format != Format::X8R8G8B8 &&
        desc.format != Format::A8B8G8R8 && desc.format != Format::X8B8G8R8) {
      std::ostringstream out;
      out << "[dxmt9-texture] gpu-dump skip handle=0x" << std::hex << handle.value << std::dec
          << " unsupported-format=" << static_cast<unsigned>(desc.format);
      emitTextureTraceLine(out.str());
      dumpedGpuTextures_.insert(handle.value);
      return;
    }

    WMT::Texture srcTex{sourceTextureHandle};
    WMTTextureInfo stagingInfo{};
    stagingInfo.type = WMTTextureType2D;
    stagingInfo.pixel_format = srcTex.pixelFormat();
    stagingInfo.width = std::max(1u, desc.width);
    stagingInfo.height = std::max(1u, desc.height);
    stagingInfo.depth = 1; stagingInfo.mipmap_level_count = 1;
    stagingInfo.sample_count = 1; stagingInfo.array_length = 1;
    stagingInfo.options = WMTResourceStorageModeShared;
    stagingInfo.usage = WMTTextureUsageShaderRead;
    auto stagingTexture = wrappedDevice_.newTexture(stagingInfo);
    if (!stagingTexture) return;

    auto commandBuffer = bootstrapCommandBuffer(wrappedCommandQueue_);
    if (!commandBuffer) return;
    auto blit = commandBuffer.blitCommandEncoder();
    if (!blit) return;
    WMTOrigin origin{0,0,0};
    WMTSize size{std::max(1u,desc.width), std::max(1u,desc.height), 1};
    blit.copyFromTextureToTexture(srcTex, 0, 0, origin, size,
                                  WMT::Texture{stagingTexture.handle}, 0, 0, origin);
    blit.endEncoding();
    commandBuffer.commit();
    commandBuffer.waitUntilCompleted();
    {
      std::lock_guard lock(mutex_);
      CommandBufferDiagnostics diagnostics;
      diagnostics.hasBlit = true;
      submissionDiagnostics_.inspect(commandBuffer.handle, diagnostics, "gpu-dump");
    }

    // Read back via a buffer blit
    const u32 pitch = std::max(1u, desc.width) * 4u;
    std::vector<u8> bytes(static_cast<size_t>(pitch) * std::max(1u, desc.height));
    WMTBufferInfo bufInfo{};
    bufInfo.length = bytes.size();
    bufInfo.options = WMTResourceStorageModeShared;
    auto readBuf = wrappedDevice_.newBuffer(bufInfo);
    if (readBuf && bufInfo.memory.ptr) {
      auto cmdBuf2 = bootstrapCommandBuffer(wrappedCommandQueue_);
      if (cmdBuf2) {
        auto blit2 = cmdBuf2.blitCommandEncoder();
        if (blit2) {
          blit2.copyFromTextureToBuffer(WMT::Texture{stagingTexture.handle}, 0, 0,
                                        origin, size, WMT::Buffer{readBuf.handle},
                                        0, pitch, 0);
          blit2.endEncoding();
        }
        cmdBuf2.commit(); cmdBuf2.waitUntilCompleted();
      }
      std::memcpy(bytes.data(), bufInfo.memory.ptr, bytes.size());
    }
    const bool wrote = writeTextureBmp(path, desc.format, std::max(1u, desc.width),
                                       std::max(1u, desc.height), pitch, bytes);
    std::ostringstream out;
    out << "[dxmt9-texture] gpu-dump handle=0x" << std::hex << handle.value << std::dec
        << " size=" << desc.width << "x" << desc.height
        << " format=" << static_cast<unsigned>(desc.format)
        << " path=" << path << " wrote=" << (wrote ? 1 : 0);
    emitTextureTraceLine(out.str());
    dumpedGpuTextures_.insert(handle.value);
  }

  WMT::Reference<WMT::SamplerState> makeSampler(bool linear) {
    WMTSamplerInfo info{};
    auto f = linear ? WMTSamplerMinMagFilterLinear : WMTSamplerMinMagFilterNearest;
    info.min_filter = f;
    info.mag_filter = f;
    info.mip_filter = WMTSamplerMipFilterNotMipmapped;
    info.s_address_mode = WMTSamplerAddressModeClampToEdge;
    info.t_address_mode = WMTSamplerAddressModeClampToEdge;
    info.r_address_mode = WMTSamplerAddressModeClampToEdge;
    info.normalized_coords = true;
    return wrappedDevice_.newSamplerState(info);
  }

  WMT::Reference<WMT::SamplerState> makeSampler(const SamplerSnapshot& snapshot) {
    const auto minFilter = snapshot.states.contains(SAMP_MIN_FILTER) ? snapshot.states.at(SAMP_MIN_FILTER) : 0u;
    const auto magFilter = snapshot.states.contains(SAMP_MAG_FILTER) ? snapshot.states.at(SAMP_MAG_FILTER) : 0u;
    const auto mipFilter = snapshot.states.contains(SAMP_MIP_FILTER) ? snapshot.states.at(SAMP_MIP_FILTER) : 0u;
    const auto addressU = snapshot.states.contains(SAMP_ADDRESS_U) ? snapshot.states.at(SAMP_ADDRESS_U) : 1u;
    const auto addressV = snapshot.states.contains(SAMP_ADDRESS_V) ? snapshot.states.at(SAMP_ADDRESS_V) : 1u;
    const auto addressW = snapshot.states.contains(SAMP_ADDRESS_W) ? snapshot.states.at(SAMP_ADDRESS_W) : 1u;
    const auto borderColor = snapshot.states.contains(SAMP_BORDER_COLOR) ? snapshot.states.at(SAMP_BORDER_COLOR) : 0u;
    auto resolveAddressMode = [](u32 value) -> WMTSamplerAddressMode {
      switch (value) {
        case 1u: return WMTSamplerAddressModeRepeat;
        case 2u: return WMTSamplerAddressModeMirrorRepeat;
        case 4u: return WMTSamplerAddressModeClampToBorderColor;
        case 3u:
        default: return WMTSamplerAddressModeClampToEdge;
      }
    };
    auto resolveBorderColor = [](u32 value) -> WMTSamplerBorderColor {
      switch (value) {
        case 0x00000000u: return WMTSamplerBorderColorTransparentBlack;
        case 0xff000000u: return WMTSamplerBorderColorOpaqueBlack;
        case 0xffffffffu: return WMTSamplerBorderColorOpaqueWhite;
        default: return (value >> 24) == 0u ? WMTSamplerBorderColorTransparentBlack : WMTSamplerBorderColorOpaqueBlack;
      }
    };

    WMTSamplerInfo info{};
    info.min_filter = minFilter == 2u ? WMTSamplerMinMagFilterLinear : WMTSamplerMinMagFilterNearest;
    info.mag_filter = magFilter == 2u ? WMTSamplerMinMagFilterLinear : WMTSamplerMinMagFilterNearest;
    switch (mipFilter) {
      case 2u: info.mip_filter = WMTSamplerMipFilterLinear; break;
      case 1u: info.mip_filter = WMTSamplerMipFilterNearest; break;
      default: info.mip_filter = WMTSamplerMipFilterNotMipmapped; break;
    }
    info.s_address_mode = resolveAddressMode(addressU);
    info.t_address_mode = resolveAddressMode(addressV);
    info.r_address_mode = resolveAddressMode(addressW);
    if (info.s_address_mode == WMTSamplerAddressModeClampToBorderColor ||
        info.t_address_mode == WMTSamplerAddressModeClampToBorderColor ||
        info.r_address_mode == WMTSamplerAddressModeClampToBorderColor) {
      info.border_color = resolveBorderColor(borderColor);
    }
    info.normalized_coords = true;
    return wrappedDevice_.newSamplerState(info);
  }

  std::shared_future<WMT::Reference<WMT::RenderPipelineState>> pipelineForDraw(const DrawDesc& draw) {
    auto resolvePixelFormat = [this](Handle handle) -> u32 {
      if (!handle) {
        return 0;
      }
      if (auto* surface = findSurfaceUnlocked(handle.value); surface) {
        return static_cast<u32>(toPixelFormat(surface->desc.format, limits_));
      }
      return 0;
    };

    std::array<u32, kMaxRenderTargets> colorFormats{};
    std::array<BlendAttachmentKey, kMaxRenderTargets> blendAttachments{};
    const auto& rs = draw.rs.values;
    const bool blendEnabled =
        !debugForceVisibleDraw() && rs.contains(RS_ALPHABLEND_ENABLE) && rs.at(RS_ALPHABLEND_ENABLE) != 0;
    for (size_t i = 0; i < kMaxRenderTargets; ++i) {
      colorFormats[i] = resolvePixelFormat(draw.rts.color[i].handle);
      auto& blend = blendAttachments[i];
      blend.pixelFormat = colorFormats[i];
      blend.blendingEnabled = blendEnabled;
      blend.rgbBlendOperation = rs.contains(RS_BLEND_OP) ? rs.at(RS_BLEND_OP) : static_cast<u32>(BlendOp::Add);
      blend.alphaBlendOperation = rs.contains(RS_BLEND_OP_ALPHA)
                                      ? rs.at(RS_BLEND_OP_ALPHA)
                                      : blend.rgbBlendOperation;
      blend.sourceRGBBlendFactor = rs.contains(RS_SRC_BLEND) ? rs.at(RS_SRC_BLEND)
                                                              : static_cast<u32>(BlendFactor::One);
      blend.destinationRGBBlendFactor = rs.contains(RS_DEST_BLEND) ? rs.at(RS_DEST_BLEND)
                                                                   : static_cast<u32>(BlendFactor::Zero);
      blend.sourceAlphaBlendFactor = rs.contains(RS_SRC_BLEND_ALPHA) ? rs.at(RS_SRC_BLEND_ALPHA)
                                                                      : blend.sourceRGBBlendFactor;
      blend.destinationAlphaBlendFactor = rs.contains(RS_DEST_BLEND_ALPHA)
                                              ? rs.at(RS_DEST_BLEND_ALPHA)
                                              : blend.destinationRGBBlendFactor;
      blend.colorWriteMask = debugForceVisibleDraw()
                                 ? 0xfu
                                 : (rs.contains(RS_COLOR_WRITE_ENABLE) ? rs.at(RS_COLOR_WRITE_ENABLE) : 0xfu);
    }
    u32 depthFormat = 0;
    u32 stencilFormat = 0;
    if (draw.rts.depthStencil.handle) {
      if (auto* surface = findSurfaceUnlocked(draw.rts.depthStencil.handle.value);
          surface && surface->desc.depthStencil) {
        const auto pixelFormat = static_cast<u32>(toPixelFormat(surface->desc.format, limits_));
        depthFormat = formatHasDepthAspect(surface->desc.format) ? pixelFormat : 0u;
        stencilFormat = formatHasStencilAspect(surface->desc.format) ? pixelFormat : 0u;
      }
    }
    const auto key = makeShaderVariantKey(draw, colorFormats, blendAttachments, depthFormat, stencilFormat);
    {
      std::lock_guard lock(cacheMutex_);
      if (auto it = drawPipelineCache_.find(key); it != drawPipelineCache_.end()) {
        return it->second.future;
      }
      auto future = std::async(std::launch::async, [this, draw, key]() {
        auto vsSource = makeDrawShaderSource(draw, true);
        auto fsSource = makeDrawShaderSource(draw, false);
        auto vsLib = makeLibraryWMT(wrappedDevice_, vsSource);
        auto fsLib = makeLibraryWMT(wrappedDevice_, fsSource);
        if (!vsLib || !fsLib) {
          return WMT::Reference<WMT::RenderPipelineState>{};
        }
        auto vs = vsLib.newFunction("dxmt9_vs");
        auto fs = fsLib.newFunction("dxmt9_fs");
        if (!vs || !fs) {
          return WMT::Reference<WMT::RenderPipelineState>{};
        }
        WMTRenderPipelineInfo info{};
        info.vertex_function = vs.handle;
        info.fragment_function = fs.handle;
        info.raster_sample_count = std::max(1u, key.sampleCount);
        info.alpha_to_coverage_enabled = key.alphaToCoverage;
        info.depth_pixel_format = static_cast<WMTPixelFormat>(key.depthFormat);
        info.stencil_pixel_format = static_cast<WMTPixelFormat>(key.stencilFormat);
        info.rasterization_enabled = true;
        if (shaderArchive_) {
          info.binary_archive_for_serialization = shaderArchive_.handle;
        }
        for (size_t i = 0; i < kMaxRenderTargets; ++i) {
          auto& ca = info.colors[i];
          ca.pixel_format = static_cast<WMTPixelFormat>(key.colorFormats[i]);
          ca.blending_enabled = key.blend[i].blendingEnabled;
          ca.rgb_blend_operation = toBlendOperation(key.blend[i].rgbBlendOperation);
          ca.alpha_blend_operation = toBlendOperation(key.blend[i].alphaBlendOperation);
          ca.src_rgb_blend_factor = toBlendFactor(key.blend[i].sourceRGBBlendFactor);
          ca.dst_rgb_blend_factor = toBlendFactor(key.blend[i].destinationRGBBlendFactor);
          ca.src_alpha_blend_factor = toBlendFactor(key.blend[i].sourceAlphaBlendFactor);
          ca.dst_alpha_blend_factor = toBlendFactor(key.blend[i].destinationAlphaBlendFactor);
          ca.write_mask = toColorWriteMask(key.blend[i].colorWriteMask);
        }
        WMT::Error err{};
        auto pso = wrappedDevice_.newRenderPipelineState(info, err);
        if (pso && shaderArchive_) {
          persistShaderArchiveWMT(shaderArchive_, shaderArchivePath_);
        }
        return pso;
      });
      auto shared = future.share();
      drawPipelineCache_.emplace(key, PipelineCacheEntry{shared});
      return shared;
    }
  }

  std::shared_future<WMT::Reference<WMT::RenderPipelineState>> pipelineForColorFill(const ColorRGBA& color,
                                                                                     u32 pixelFormat) {
    ShaderVariantKey key;
    key.hash = static_cast<u64>(std::bit_cast<u32>(color.r)) ^
               (static_cast<u64>(std::bit_cast<u32>(color.g)) << 1) ^ pixelFormat;
    key.colorFormats[0] = pixelFormat;
    key.blend[0].pixelFormat = pixelFormat;
    std::lock_guard lock(cacheMutex_);
    if (auto it = fillPipelineCache_.find(key); it != fillPipelineCache_.end()) {
      return it->second.future;
    }
    auto future = std::async(std::launch::async, [this, color, pixelFormat]() {
      auto vsLib = makeLibraryWMT(wrappedDevice_, makeGenericVertexSource(makeHash("fill")));
      auto fsLib = makeLibraryWMT(wrappedDevice_, makeGenericFragmentSource(color, makeHash("fill")));
      if (!vsLib || !fsLib) {
        return WMT::Reference<WMT::RenderPipelineState>{};
      }
      auto vs = vsLib.newFunction("dxmt9_vs");
      auto fs = fsLib.newFunction("dxmt9_fs");
      WMTRenderPipelineInfo info{};
      info.vertex_function = vs.handle;
      info.fragment_function = fs.handle;
      info.colors[0].pixel_format = static_cast<WMTPixelFormat>(pixelFormat);
      info.colors[0].write_mask = WMTColorWriteMaskAll;
      info.rasterization_enabled = true;
      info.raster_sample_count = 1;
      if (shaderArchive_) info.binary_archive_for_serialization = shaderArchive_.handle;
      WMT::Error err{};
      auto pso = wrappedDevice_.newRenderPipelineState(info, err);
      if (pso && shaderArchive_) persistShaderArchiveWMT(shaderArchive_, shaderArchivePath_);
      return pso;
    });
    auto shared = future.share();
    fillPipelineCache_.emplace(key, PipelineCacheEntry{shared});
    return shared;
  }

  std::shared_future<WMT::Reference<WMT::RenderPipelineState>> pipelineForStretchRect(const StretchRectDesc& stretch,
                                                                                       u32 pixelFormat) {
    ShaderVariantKey key;
    key.hash = stretch.linear ? 1u : 0u;
    key.textured = true;
    key.linear = stretch.linear;
    key.sampleCount = std::max(1u, stretch.destinationSampleCount);
    key.colorFormats[0] = pixelFormat;
    key.blend[0].pixelFormat = pixelFormat;
    std::lock_guard lock(cacheMutex_);
    if (auto it = stretchPipelineCache_.find(key); it != stretchPipelineCache_.end()) {
      return it->second.future;
    }
    auto future = std::async(std::launch::async, [this, stretch, pixelFormat]() {
      auto vsLib = makeLibraryWMT(wrappedDevice_, makeTexturedVertexSource(makeHash("stretch")));
      auto fsLib = makeLibraryWMT(wrappedDevice_, makeTexturedFragmentSource(makeHash("stretch")));
      if (!vsLib || !fsLib) return WMT::Reference<WMT::RenderPipelineState>{};
      auto vs = vsLib.newFunction("dxmt9_vs");
      auto fs = fsLib.newFunction("dxmt9_fs");
      WMTRenderPipelineInfo info{};
      info.vertex_function = vs.handle;
      info.fragment_function = fs.handle;
      info.raster_sample_count = std::max(1u, stretch.destinationSampleCount);
      info.colors[0].pixel_format = static_cast<WMTPixelFormat>(pixelFormat);
      info.colors[0].write_mask = WMTColorWriteMaskAll;
      info.rasterization_enabled = true;
      if (shaderArchive_) info.binary_archive_for_serialization = shaderArchive_.handle;
      WMT::Error err{};
      auto pso = wrappedDevice_.newRenderPipelineState(info, err);
      if (pso && shaderArchive_) persistShaderArchiveWMT(shaderArchive_, shaderArchivePath_);
      return pso;
    });
    auto shared = future.share();
    stretchPipelineCache_.emplace(key, PipelineCacheEntry{shared});
    return shared;
  }

  std::shared_future<WMT::Reference<WMT::RenderPipelineState>> pipelineForPresent(Format sourceFormat) {
    ShaderVariantKey key;
    key.hash = sourceFormat == Format::X8R8G8B8 || sourceFormat == Format::X8B8G8R8 ? 1u : 0u;
    key.textured = true;
    key.sampleCount = 1u;
    key.colorFormats[0] = static_cast<u32>(WMTPixelFormatBGRA8Unorm);
    key.blend[0].pixelFormat = static_cast<u32>(WMTPixelFormatBGRA8Unorm);
    std::lock_guard lock(cacheMutex_);
    if (auto it = presentPipelineCache_.find(key); it != presentPipelineCache_.end()) {
      return it->second.future;
    }
    const bool forceOpaqueAlpha = sourceFormat == Format::X8R8G8B8 || sourceFormat == Format::X8B8G8R8;
    auto future = std::async(std::launch::async, [this, forceOpaqueAlpha]() {
      auto vsLib = makeLibraryWMT(wrappedDevice_, makeTexturedVertexSource(makeHash("present")));
      auto fsLib = makeLibraryWMT(wrappedDevice_, makeTexturedFragmentSource(
                                       makeHash(forceOpaqueAlpha ? "present-opaque" : "present"), forceOpaqueAlpha));
      if (!vsLib || !fsLib) return WMT::Reference<WMT::RenderPipelineState>{};
      auto vs = vsLib.newFunction("dxmt9_vs");
      auto fs = fsLib.newFunction("dxmt9_fs");
      WMTRenderPipelineInfo info{};
      info.vertex_function = vs.handle;
      info.fragment_function = fs.handle;
      info.raster_sample_count = 1;
      info.colors[0].pixel_format = WMTPixelFormatBGRA8Unorm;
      info.colors[0].write_mask = WMTColorWriteMaskAll;
      info.rasterization_enabled = true;
      if (shaderArchive_) info.binary_archive_for_serialization = shaderArchive_.handle;
      WMT::Error err{};
      auto pso = wrappedDevice_.newRenderPipelineState(info, err);
      if (pso && shaderArchive_) persistShaderArchiveWMT(shaderArchive_, shaderArchivePath_);
      return pso;
    });
    auto shared = future.share();
    presentPipelineCache_.emplace(key, PipelineCacheEntry{shared});
    return shared;
  }

  WMT::Reference<WMT::DepthStencilState> depthStencilStateFor(const DepthStencilKey& key) {
    std::lock_guard lock(cacheMutex_);
    if (auto it = depthCache_.find(key); it != depthCache_.end()) {
      return it->second;
    }
    WMTDepthStencilInfo info{};
    info.depth_compare_function = static_cast<WMTCompareFunction>(toCompareFunction(key.depthFunc));
    info.depth_write_enabled = key.depthEnable && key.depthWrite;
    auto applyFace = [](WMTStencilInfo& stencilInfo, const StencilFaceKey& face) {
      stencilInfo.enabled = face.enabled;
      stencilInfo.stencil_compare_function = static_cast<WMTCompareFunction>(toCompareFunction(face.compareFunction));
      stencilInfo.stencil_fail_op = static_cast<WMTStencilOperation>(toStencilOperation(face.failureOperation));
      stencilInfo.depth_fail_op = static_cast<WMTStencilOperation>(toStencilOperation(face.depthFailureOperation));
      stencilInfo.depth_stencil_pass_op = static_cast<WMTStencilOperation>(toStencilOperation(face.passOperation));
      stencilInfo.read_mask = static_cast<uint8_t>(face.readMask);
      stencilInfo.write_mask = static_cast<uint8_t>(face.writeMask);
    };
    if (key.front.enabled || key.back.enabled) {
      applyFace(info.front_stencil, key.front);
      applyFace(info.back_stencil, key.back.enabled ? key.back : key.front);
    }
    auto state = wrappedDevice_.newDepthStencilState(info);
    depthCache_.emplace(key, state);
    return state;
  }

  void finishLoop() {
    @autoreleasepool {
      while (true) {
        std::unique_lock lock(mutex_);
        if (!queueLifecycle_.runFinishIteration(lock, [this](u64) {
              argbufArena_.reclaim(completedSeqId_);
              lambdaStoreArena_.reclaim(completedSeqId_);
              stagingArena_.reclaim(completedSeqId_);
              copyTempArena_.reclaim(completedSeqId_);
              tryGarbageCollectUnlocked();
            })) {
          return;
        }
      }
    }
  }

  void waitForSequenceUnlocked(u64 seqId, std::unique_lock<std::mutex>& lock) {
    queueLifecycle_.waitForSequence(lock, seqId);
  }

  void tryGarbageCollectUnlocked() {
    auto gcMap = [](auto& map, u64 completed) {
      for (auto it = map.begin(); it != map.end();) {
        auto& record = it->second;
        if (record.destroyPending && record.lastUsedSeqId <= completed) {
          // TLA+: NoUseAfterFree
          DXMT_ASSERT(record.lastUsedSeqId <= completed);
          it = map.erase(it);
        } else {
          ++it;
        }
      }
    };
    gcMap(buffers_, completedSeqId_);
    gcMap(textures_, completedSeqId_);
    gcMap(surfaces_, completedSeqId_);
  }

  void purgeResourcesUnlocked() {
    buffers_.clear();
    textures_.clear();
    surfaces_.clear();
  }

  BackendLimits limits_{};
  WMT::Reference<WMT::Device> wrappedDevice_{};
  WMT::Reference<WMT::CommandQueue> wrappedCommandQueue_{};
  std::thread encodeThread_;
  std::thread finishThread_;
  std::mutex mutex_;
  std::condition_variable encodeCv_;
  std::condition_variable finishCv_;
  std::condition_variable writeCv_;
  bool stop_ = true;
  std::array<ChunkSlot, kRingSize> slots_{};
  std::optional<size_t> writingSlot_;
  size_t writeIndex_ = 0;
  u64 nextSeqId_ = 1;
  u64 lastCommittedSeqId_ = 0;
  u64 completedSeqId_ = 0;
  size_t inflightCount_ = 0;
  std::deque<size_t> readySlots_;
  std::deque<u64> completedSeqQueue_;
  u64 nextHandle_ = 1;
  Handle currentBackBuffer_{};
  bool backBufferDiscardAfterPresent_ = false;
  DeviceLostObserver deviceLostObserver_;
  PresentationStatusObserver presentationStatusObserver_;
  u32 maxFrameLatency_ = 3;
  std::unordered_map<u64, BufferRecord> buffers_;
  std::unordered_map<u64, TextureRecord> textures_;
  std::unordered_map<u64, SurfaceRecord> surfaces_;
  std::unordered_set<u64> dumpedGpuTextures_;
  std::mutex cacheMutex_;
  std::unordered_map<ShaderVariantKey, PipelineCacheEntry, ShaderVariantKeyHash> drawPipelineCache_;
  std::unordered_map<ShaderVariantKey, PipelineCacheEntry, ShaderVariantKeyHash> fillPipelineCache_;
  std::unordered_map<ShaderVariantKey, PipelineCacheEntry, ShaderVariantKeyHash> stretchPipelineCache_;
  std::unordered_map<ShaderVariantKey, PipelineCacheEntry, ShaderVariantKeyHash> presentPipelineCache_;
  std::unordered_map<DepthStencilKey, WMT::Reference<WMT::DepthStencilState>, DepthStencilKeyHash> depthCache_;
  metalpresent::PresenterState presenterState_{};
  RingArena argbufArena_{1 << 20};
  RingArena lambdaStoreArena_{1 << 18};
  RingArena stagingArena_{1 << 20};
  RingArena copyTempArena_{1 << 20};
  std::string shaderArchivePath_{};
  WMT::Reference<WMT::BinaryArchive> shaderArchive_{};
  metalqueue::QueueLifecycleController queueLifecycle_{};
  metalhud::SubmissionDiagnosticsController submissionDiagnostics_{};
  bool ready_ = false;
};

}  // namespace

std::string makeShaderSourceFromRequest(const WinemetalShaderCompileRequest& request) {
  return makeShaderSourceFromRequestInternal(request);
}

std::shared_ptr<BackendDevice> makeMetalBackendDevice(const BackendLimits& limits) {
  auto backend = std::make_shared<MetalBackendDevice>(limits);
  return backend->ready() ? std::static_pointer_cast<BackendDevice>(std::move(backend)) : std::shared_ptr<BackendDevice>{};
}

}  // namespace dxmt9::core
