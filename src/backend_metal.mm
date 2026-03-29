#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "dxmt9/assert.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/winemetal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <deque>
#include <iomanip>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <thread>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dxmt9::core {

namespace {

constexpr size_t kRingSize = 32;
constexpr size_t kMaxInflight = 3;

std::mutex gLayerRegistryMutex;
std::unordered_map<u64, CAMetalLayer*> gLayerRegistry;

CAMetalLayer* lookupLayerHandle(u64 handle) {
  std::lock_guard lock(gLayerRegistryMutex);
  if (auto it = gLayerRegistry.find(handle); it != gLayerRegistry.end()) {
    return it->second;
  }
  return nullptr;
}

void registerLayerHandle(u64 handle, CAMetalLayer* layer) {
  std::lock_guard lock(gLayerRegistryMutex);
  gLayerRegistry[handle] = layer;
}

void unregisterLayerHandle(u64 handle) {
  std::lock_guard lock(gLayerRegistryMutex);
  gLayerRegistry.erase(handle);
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

u64 makeHash(const std::string& source) {
  return hashString(source);
}

MTLPixelFormat toPixelFormat(Format format) {
  switch (format) {
    case Format::A8R8G8B8:
    case Format::X8R8G8B8:
      return MTLPixelFormatBGRA8Unorm;
    case Format::A8B8G8R8:
    case Format::X8B8G8R8:
      return MTLPixelFormatRGBA8Unorm;
    case Format::R5G6B5:
      return MTLPixelFormatB5G6R5Unorm;
    case Format::A1R5G5B5:
    case Format::X1R5G5B5:
      return MTLPixelFormatBGR5A1Unorm;
    case Format::A4R4G4B4:
      return MTLPixelFormatABGR4Unorm;
    case Format::A8:
      return MTLPixelFormatA8Unorm;
    case Format::A16B16G16R16F:
      return MTLPixelFormatRGBA16Float;
    case Format::A32B32G32R32F:
      return MTLPixelFormatRGBA32Float;
    case Format::G16R16F:
      return MTLPixelFormatRG16Float;
    case Format::R16F:
      return MTLPixelFormatR16Float;
    case Format::G32R32F:
      return MTLPixelFormatRG32Float;
    case Format::R32F:
      return MTLPixelFormatR32Float;
    case Format::A16B16G16R16:
      return MTLPixelFormatRGBA16Unorm;
    case Format::G16R16:
      return MTLPixelFormatRG16Unorm;
    case Format::A2R10G10B10:
      return MTLPixelFormatRGB10A2Unorm;
    case Format::A2B10G10R10:
      return MTLPixelFormatBGR10A2Unorm;
    case Format::L8:
      return MTLPixelFormatR8Unorm;
    case Format::L16:
      return MTLPixelFormatR16Unorm;
    case Format::A8L8:
      return MTLPixelFormatRG8Unorm;
    case Format::V8U8:
      return MTLPixelFormatRG8Snorm;
    case Format::Q8W8V8U8:
      return MTLPixelFormatRGBA8Snorm;
    case Format::V16U16:
      return MTLPixelFormatRG16Snorm;
    case Format::D24S8:
    case Format::D24X8:
      return MTLPixelFormatDepth24Unorm_Stencil8;
    case Format::D16:
    case Format::D16_LOCKABLE:
      return MTLPixelFormatDepth16Unorm;
    case Format::D32:
    case Format::D32F_LOCKABLE:
      return MTLPixelFormatDepth32Float;
    case Format::D24FS8:
      return MTLPixelFormatDepth32Float_Stencil8;
    default:
      return MTLPixelFormatBGRA8Unorm;
  }
}

MTLTextureType toTextureType(TextureType type, bool multisample) {
  switch (type) {
    case TextureType::TwoD:
      return multisample ? MTLTextureType2DMultisample : MTLTextureType2D;
    case TextureType::Cube:
      return MTLTextureTypeCube;
    case TextureType::Volume:
      return MTLTextureType3D;
    case TextureType::Array2D:
      return multisample ? MTLTextureType2DMultisampleArray : MTLTextureType2DArray;
  }
  return multisample ? MTLTextureType2DMultisample : MTLTextureType2D;
}

MTLStorageMode toStorageMode(Pool pool, u32 usage) {
  if (pool == Pool::SystemMem || pool == Pool::Scratch) {
    return MTLStorageModeShared;
  }
  if (pool == Pool::Managed) {
    return MTLStorageModeShared;
  }
  return (usage & UsageDynamic) != 0 ? MTLStorageModeShared : MTLStorageModePrivate;
}

MTLResourceOptions toResourceOptions(Pool pool, u32 usage) {
  const auto storageMode = toStorageMode(pool, usage);
  MTLResourceOptions options = 0;
  switch (storageMode) {
    case MTLStorageModeShared:
      options |= MTLResourceStorageModeShared;
      break;
    case MTLStorageModePrivate:
      options |= MTLResourceStorageModePrivate;
      break;
    default:
      options |= MTLResourceStorageModeShared;
      break;
  }
  return options;
}

MTLTextureUsage toTextureUsage(const SurfaceDesc& desc) {
  MTLTextureUsage usage = MTLTextureUsageUnknown;
  if (desc.renderTarget || desc.depthStencil) {
    usage |= MTLTextureUsageRenderTarget;
  }
  usage |= MTLTextureUsageShaderRead;
  return usage;
}

MTLTextureUsage toTextureUsage(const TextureDesc& desc) {
  MTLTextureUsage usage = MTLTextureUsageShaderRead;
  if ((desc.usage & UsageRenderTarget) != 0 || (desc.usage & UsageDepthStencil) != 0) {
    usage |= MTLTextureUsageRenderTarget;
  }
  return usage;
}

MTLPrimitiveType toPrimitiveType(PrimitiveType type) {
  switch (type) {
    case PrimitiveType::PointList:
      return MTLPrimitiveTypePoint;
    case PrimitiveType::LineList:
      return MTLPrimitiveTypeLine;
    case PrimitiveType::LineStrip:
      return MTLPrimitiveTypeLineStrip;
    case PrimitiveType::TriangleList:
    case PrimitiveType::TriangleFan:
      return MTLPrimitiveTypeTriangle;
    case PrimitiveType::TriangleStrip:
      return MTLPrimitiveTypeTriangleStrip;
  }
  return MTLPrimitiveTypeTriangle;
}

MTLIndexType toIndexType(IndexType type) {
  return type == IndexType::UInt32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
}

MTLCompareFunction toCompareFunction(u32 value) {
  switch (static_cast<CompareFunc>(value)) {
    case CompareFunc::Never:
      return MTLCompareFunctionNever;
    case CompareFunc::Less:
      return MTLCompareFunctionLess;
    case CompareFunc::Equal:
      return MTLCompareFunctionEqual;
    case CompareFunc::LessEqual:
      return MTLCompareFunctionLessEqual;
    case CompareFunc::Greater:
      return MTLCompareFunctionGreater;
    case CompareFunc::NotEqual:
      return MTLCompareFunctionNotEqual;
    case CompareFunc::GreaterEqual:
      return MTLCompareFunctionGreaterEqual;
    case CompareFunc::Always:
      return MTLCompareFunctionAlways;
  }
  return MTLCompareFunctionAlways;
}

[[maybe_unused]] MTLBlendOperation toBlendOperation(u32 value) {
  switch (static_cast<BlendOp>(value)) {
    case BlendOp::Add:
      return MTLBlendOperationAdd;
    case BlendOp::Subtract:
      return MTLBlendOperationSubtract;
    case BlendOp::RevSubtract:
      return MTLBlendOperationReverseSubtract;
    case BlendOp::Min:
      return MTLBlendOperationMin;
    case BlendOp::Max:
      return MTLBlendOperationMax;
  }
  return MTLBlendOperationAdd;
}

[[maybe_unused]] MTLBlendFactor toBlendFactor(u32 value) {
  switch (static_cast<BlendFactor>(value)) {
    case BlendFactor::Zero:
      return MTLBlendFactorZero;
    case BlendFactor::One:
      return MTLBlendFactorOne;
    case BlendFactor::SrcColor:
      return MTLBlendFactorSourceColor;
    case BlendFactor::InvSrcColor:
      return MTLBlendFactorOneMinusSourceColor;
    case BlendFactor::SrcAlpha:
      return MTLBlendFactorSourceAlpha;
    case BlendFactor::InvSrcAlpha:
      return MTLBlendFactorOneMinusSourceAlpha;
    case BlendFactor::DestAlpha:
      return MTLBlendFactorDestinationAlpha;
    case BlendFactor::InvDestAlpha:
      return MTLBlendFactorOneMinusDestinationAlpha;
    case BlendFactor::DestColor:
      return MTLBlendFactorDestinationColor;
    case BlendFactor::InvDestColor:
      return MTLBlendFactorOneMinusDestinationColor;
    case BlendFactor::SrcAlphaSat:
      return MTLBlendFactorSourceAlphaSaturated;
    case BlendFactor::BothSrcAlpha:
      return MTLBlendFactorBlendAlpha;
    case BlendFactor::BothInvSrcAlpha:
      return MTLBlendFactorOneMinusBlendAlpha;
    case BlendFactor::BlendFactor:
      return MTLBlendFactorBlendColor;
    case BlendFactor::InvBlendFactor:
      return MTLBlendFactorOneMinusBlendColor;
  }
  return MTLBlendFactorOne;
}

MTLCullMode toCullMode(u32 value) {
  switch (static_cast<CullMode>(value)) {
    case CullMode::None:
      return MTLCullModeNone;
    case CullMode::Cw:
      return MTLCullModeBack;
    case CullMode::Ccw:
      return MTLCullModeFront;
  }
  return MTLCullModeNone;
}

MTLStencilOperation toStencilOperation(u32 value) {
  switch (static_cast<StencilOp>(value)) {
    case StencilOp::Keep:
      return MTLStencilOperationKeep;
    case StencilOp::Zero:
      return MTLStencilOperationZero;
    case StencilOp::Replace:
      return MTLStencilOperationReplace;
    case StencilOp::IncrSat:
      return MTLStencilOperationIncrementClamp;
    case StencilOp::DecrSat:
      return MTLStencilOperationDecrementClamp;
    case StencilOp::Invert:
      return MTLStencilOperationInvert;
    case StencilOp::Incr:
      return MTLStencilOperationIncrementWrap;
    case StencilOp::Decr:
      return MTLStencilOperationDecrementWrap;
  }
  return MTLStencilOperationKeep;
}

MTLColorWriteMask toColorWriteMask(u32 value) {
  MTLColorWriteMask mask = 0;
  if ((value & 0x1u) != 0) {
    mask |= MTLColorWriteMaskRed;
  }
  if ((value & 0x2u) != 0) {
    mask |= MTLColorWriteMaskGreen;
  }
  if ((value & 0x4u) != 0) {
    mask |= MTLColorWriteMaskBlue;
  }
  if ((value & 0x8u) != 0) {
    mask |= MTLColorWriteMaskAlpha;
  }
  return mask == 0 ? MTLColorWriteMaskAll : mask;
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
  ObjcPtr<id<MTLBuffer>> buffer;
  std::vector<u8> shadow;
  bool destroyPending = false;
  u64 lastUsedSeqId = 0;
};

struct TextureRecord {
  TextureDesc desc{};
  ObjcPtr<id<MTLTexture>> texture;
  bool destroyPending = false;
  u64 lastUsedSeqId = 0;
};

struct SurfaceRecord {
  SurfaceDesc desc{};
  ObjcPtr<id<MTLTexture>> texture;
  ObjcPtr<id<MTLTexture>> resolveTexture;
  bool destroyPending = false;
  u64 lastUsedSeqId = 0;
};

struct MetalCommandRecord {
  enum class Kind {
    Draw,
    Clear,
    SurfaceCopy,
    StretchRect,
    Readback,
    ColorFill,
    Present,
  };

  Kind kind = Kind::Draw;
  DrawDesc draw{};
  ClearDesc clear{};
  SurfaceCopyDesc surfaceCopy{};
  StretchRectDesc stretchRect{};
  ReadbackDesc readback{};
  ColorFillDesc colorFill{};
  SwapDesc present{};
  Handle presentSource{};
};

struct DrawUniforms {
  std::array<std::array<f32, 4>, kMaxVertexConstants> vsFloatConst{};
  std::array<std::array<i32, 4>, kMaxIntegerConstants> vsIntConst{};
  std::array<u32, kMaxBoolConstants> vsBoolConst{};
  std::array<std::array<f32, 4>, kMaxPixelConstants> psFloatConst{};
  std::array<std::array<i32, 4>, kMaxIntegerConstants> psIntConst{};
  std::array<u32, kMaxBoolConstants> psBoolConst{};
  std::array<ClipPlane, kMaxClipPlanes> clipPlanes{};
  std::array<f32, 2> halfPixelFixup{};
  f32 alphaRef = 0.0f;
  f32 fogStart = 1.0f;
  f32 fogEnd = 1.0f;
  f32 fogDensity = 1.0f;
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

struct ChunkSlot {
  enum class State { Free, Writing, Pending, Encoding, GPU };

  State state = State::Free;
  u64 seqId = 0;
  std::vector<MetalCommandRecord> commands;
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
  std::shared_future<ObjcPtr<id<MTLRenderPipelineState>>> future;
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
  out << "  float4 psFloatConst[" << kMaxPixelConstants << "];\n";
  out << "  int4 psIntConst[" << kMaxIntegerConstants << "];\n";
  out << "  uint psBoolConst[" << kMaxBoolConstants << "];\n";
  out << "  float4 clipPlanes[6];\n";
  out << "  float2 halfPixelFixup;\n";
  out << "  float alphaRef;\n";
  out << "  float fogStart;\n";
  out << "  float fogEnd;\n";
  out << "  float fogDensity;\n";
  out << "  uint clipPlaneMask;\n";
  out << "  uint alphaTestEnable;\n";
  out << "  uint alphaTestFunc;\n";
  out << "  uint fogMode;\n";
  out << "};\n";
  out << "struct VSOut {\n";
  out << "  float4 position [[position]];\n";
  out << "  float4 color;\n";
  out << "  float4 secondaryColor;\n";
  out << "  float2 texcoord0;\n";
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
  return out.str();
}

DrawUniforms buildDrawUniforms(const DrawDesc& desc) {
  DrawUniforms uniforms;
  uniforms.vsFloatConst = desc.vsConst.float4;
  uniforms.vsIntConst = desc.vsConst.int4;
  for (size_t i = 0; i < kMaxBoolConstants; ++i) {
    uniforms.vsBoolConst[i] = desc.vsConst.bools[i] ? 1u : 0u;
  }
  uniforms.psFloatConst = desc.psConst.float4;
  uniforms.psIntConst = desc.psConst.int4;
  for (size_t i = 0; i < kMaxBoolConstants; ++i) {
    uniforms.psBoolConst[i] = desc.psConst.bools[i] ? 1u : 0u;
  }
  uniforms.halfPixelFixup = halfPixelFixup(desc.viewport.viewport);
  uniforms.clipPlaneMask = desc.clipPlaneMask;
  uniforms.alphaTestEnable = desc.rs.values.contains(RS_ALPHA_TEST_ENABLE) &&
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
constexpr u32 kD3DSIO_LRP = 18;
constexpr u32 kD3DSIO_FRC = 19;
constexpr u32 kD3DSIO_DCL = 31;
constexpr u32 kD3DSIO_POW = 32;
constexpr u32 kD3DSIO_CRS = 33;
constexpr u32 kD3DSIO_SGN = 34;
constexpr u32 kD3DSIO_ABS = 35;
constexpr u32 kD3DSIO_NRM = 36;
constexpr u32 kD3DSIO_DEFB = 47;
constexpr u32 kD3DSIO_DEFI = 48;
constexpr u32 kD3DSIO_TEXCOORD = 49;
constexpr u32 kD3DSIO_TEXKILL = 50;
constexpr u32 kD3DSIO_CND = 65;
constexpr u32 kD3DSIO_DEF = 66;
constexpr u32 kD3DSIO_TEXREG2RGB = 67;
constexpr u32 kD3DSIO_TEXDP3TEX = 68;
constexpr u32 kD3DSIO_TEXM3x2DEPTH = 69;
constexpr u32 kD3DSIO_TEXDP3 = 70;
constexpr u32 kD3DSIO_TEXM3x3 = 71;
constexpr u32 kD3DSIO_TEX = 51;
constexpr u32 kD3DSIO_TEXDEPTH = 72;
constexpr u32 kD3DSIO_CMP = 73;
constexpr u32 kD3DSIO_BEM = 74;
constexpr u32 kD3DSIO_DP2ADD = 75;
constexpr u32 kD3DSIO_TEXBEM = 52;
constexpr u32 kD3DSIO_TEXBEML = 53;
constexpr u32 kD3DSIO_TEXREG2AR = 54;
constexpr u32 kD3DSIO_TEXREG2GB = 55;
constexpr u32 kD3DSIO_TEXM3x2PAD = 56;
constexpr u32 kD3DSIO_TEXM3x2TEX = 57;
constexpr u32 kD3DSIO_TEXM3x3PAD = 58;
constexpr u32 kD3DSIO_TEXM3x3TEX = 59;
constexpr u32 kD3DSIO_RESERVED0 = 60;
constexpr u32 kD3DSIO_TEXM3x3SPEC = 61;
constexpr u32 kD3DSIO_TEXM3x3VSPEC = 62;
constexpr u32 kD3DSIO_EXPP = 63;
constexpr u32 kD3DSIO_LOGP = 64;
constexpr u32 kD3DSIO_DSX = 76;
constexpr u32 kD3DSIO_DSY = 77;
constexpr u32 kD3DSIO_TEXLDD = 78;
constexpr u32 kD3DSIO_SETP = 79;
constexpr u32 kD3DSIO_TEXLDL = 80;
constexpr u32 kD3DSIO_BREAKP = 81;
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
      throw std::runtime_error("unsupported D3D source modifier");
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
    case kD3DSIO_LRP:
      return "lrp";
    case kD3DSIO_FRC:
      return "frc";
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
    case kD3DSIO_MOV:
    case kD3DSIO_DEFB:
    case kD3DSIO_DEFI:
    case kD3DSIO_RCP:
    case kD3DSIO_RSQ:
    case kD3DSIO_FRC:
    case kD3DSIO_DCL:
    case kD3DSIO_DSX:
    case kD3DSIO_DSY:
    case kD3DSIO_SETP:
    case kD3DSIO_BREAKP:
      return 2;
    case kD3DSIO_ADD:
    case kD3DSIO_SUB:
    case kD3DSIO_MUL:
    case kD3DSIO_DP3:
    case kD3DSIO_DP4:
    case kD3DSIO_MIN:
    case kD3DSIO_MAX:
    case kD3DSIO_POW:
    case kD3DSIO_CRS:
    case kD3DSIO_SGN:
    case kD3DSIO_ABS:
    case kD3DSIO_NRM:
    case kD3DSIO_CND:
    case kD3DSIO_CMP:
    case kD3DSIO_DP2ADD:
    case kD3DSIO_TEXLDD:
    case kD3DSIO_TEXLDL:
      return 3;
    case kD3DSIO_MAD:
    case kD3DSIO_LRP:
      return 4;
    case kD3DSIO_DEF:
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
                                  const std::string& intPrefix, const std::string& boolPrefix) {
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
        if (reg.index == 0) {
          return vertexInputs;
        }
        return "float4(0.0f)";
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
    case D3DRegisterKind::MiscType:
    case D3DRegisterKind::Predicate:
    case D3DRegisterKind::Sampler:
    case D3DRegisterKind::Unknown:
      return "float4(0.0f)";
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

    const u32 operandCount = (token >> 24) & 0xfu;
    if (operandCount == 0 && opcode != kD3DSIO_NOP) {
      throw std::runtime_error("missing D3D operand count");
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
    out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]], constant DrawUniforms& uniforms [[buffer(0)]]) {\n";
    out << "  VSOut out;\n";
    out << "  float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n";
    out << "  float4 outPosition = float4(p[vid % 3], 0.0, 1.0);\n";
    out << "  float4 outColor = float4(1.0f);\n";
    out << "  float4 outSecondaryColor = float4(0.0f);\n";
    out << "  float4 outTexcoord0 = float4(0.0f);\n";
    out << "  float outFogFactor = 1.0f;\n";
    out << "  float outPointSize = 1.0f;\n";
    out << "  float4 r[32];\n";
    out << "  float4 cFloat[" << kMaxVertexConstants << "];\n";
    out << "  int4 cInt[" << kMaxIntegerConstants << "];\n";
    out << "  uint cBool[" << kMaxBoolConstants << "];\n";
    out << "  for (uint i = 0; i < " << kMaxVertexConstants << "; ++i) { cFloat[i] = uniforms.vsFloatConst[i]; }\n";
    out << "  for (uint i = 0; i < " << kMaxIntegerConstants << "; ++i) { cInt[i] = uniforms.vsIntConst[i]; }\n";
    out << "  for (uint i = 0; i < " << kMaxBoolConstants << "; ++i) { cBool[i] = uniforms.vsBoolConst[i]; }\n";
    for (const auto& instruction : module.instructions) {
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
        } else {
          out << decodeOperandToken(instruction.operands[i], module.stage, destination);
        }
      }
      out << "\n";

      auto readSrc = [&](size_t index) {
        if (index >= instruction.operands.size()) {
          throw std::runtime_error("missing D3D source operand");
        }
        const auto token = instruction.operands[index];
        const auto reg = decodeRegisterRef(token, module.stage);
        std::string expr = readOperandExpression(instruction, reg, "float4(p[vid % 3], 0.0f, 1.0f)", "in", true,
                                                 "outPosition", "outColor", "outSecondaryColor", "outTexcoord0",
                                                 "outFogFactor", "outPointSize", "r", "cFloat", "cInt", "cBool");
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
        case kD3DSIO_ADD:
        case kD3DSIO_SUB:
        case kD3DSIO_MUL:
        case kD3DSIO_MAD:
        case kD3DSIO_MIN:
        case kD3DSIO_MAX:
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
            throw std::runtime_error("DEF requires a float constant destination");
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
          throw std::runtime_error("unsupported D3D opcode");
      }
    }

    out << "  out.position = outPosition;\n";
    out << "  out.color = outColor;\n";
    out << "  out.secondaryColor = outSecondaryColor;\n";
    out << "  out.texcoord0 = outTexcoord0.xy;\n";
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
  out << "  float4 r[32];\n";
  out << "  float4 cFloat[" << kMaxPixelConstants << "];\n";
  out << "  int4 cInt[" << kMaxIntegerConstants << "];\n";
  out << "  uint cBool[" << kMaxBoolConstants << "];\n";
  out << "  for (uint i = 0; i < " << kMaxPixelConstants << "; ++i) { cFloat[i] = uniforms.psFloatConst[i]; }\n";
  out << "  for (uint i = 0; i < " << kMaxIntegerConstants << "; ++i) { cInt[i] = uniforms.psIntConst[i]; }\n";
  out << "  for (uint i = 0; i < " << kMaxBoolConstants << "; ++i) { cBool[i] = uniforms.psBoolConst[i]; }\n";
  for (const auto& instruction : module.instructions) {
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
      } else {
        out << decodeOperandToken(instruction.operands[i], module.stage, destination);
      }
    }
    out << "\n";

    auto readSrc = [&](size_t index) {
      if (index >= instruction.operands.size()) {
        throw std::runtime_error("missing D3D source operand");
      }
      const auto token = instruction.operands[index];
      const auto reg = decodeRegisterRef(token, module.stage);
      std::string expr = readOperandExpression(instruction, reg, "float4(0.0f)", "in", false, "outPosition",
                                               "outColor", "outSecondaryColor", "outTexcoord0", "outFogFactor",
                                               "outPointSize", "r", "cFloat", "cInt", "cBool");
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
          throw std::runtime_error("DEF requires a float constant destination");
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
      case kD3DSIO_ADD:
      case kD3DSIO_SUB:
      case kD3DSIO_MUL:
      case kD3DSIO_MAD:
      case kD3DSIO_MIN:
      case kD3DSIO_MAX:
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
        throw std::runtime_error("unsupported D3D opcode");
    }
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
  out << makeShaderPrelude(key.clipPlaneMask != 0);
  out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]], constant DrawUniforms& uniforms [[buffer(0)]]) {\n";
  out << "  VSOut out;\n";
  out << "  float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n";
  out << "  out.position = float4(p[vid % 3], 0.0, 1.0);\n";
  out << "  out.position.xy += uniforms.halfPixelFixup * out.position.w;\n";
  out << "  out.color = float4(1.0);\n";
  out << "  out.texcoord0 = float2(vid & 1u, (vid >> 1u) & 1u);\n";
  out << "  out.fogFactor = 1.0;\n";
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
  const bool textured = desc.textures[0].handle != Handle{};
  out << makeShaderPrelude(desc.clipPlaneMask != 0);
  if (textured) {
    out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], constant DrawUniforms& uniforms [[buffer(0)]], ";
    out << "texture2d<float> tex0 [[texture(0)]], sampler samp0 [[sampler(0)]]) {\n";
  } else {
    out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], constant DrawUniforms& uniforms [[buffer(0)]]) {\n";
  }
  out << "  float4 color = in.color;\n";
  if (textured) {
    out << "  float4 texColor = tex0.sample(samp0, in.texcoord0);\n";
    switch (key.stages[0].colorOp) {
      case static_cast<u32>(TextureOp::SelectArg1):
        out << "  color = color;\n";
        break;
      case static_cast<u32>(TextureOp::Add):
        out << "  color += texColor;\n";
        break;
      case static_cast<u32>(TextureOp::Modulate):
      default:
        out << "  color *= texColor;\n";
        break;
    }
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

std::string makeTexturedFragmentSource(u64 variantHash) {
  std::ostringstream out;
  out << "#include <metal_stdlib>\nusing namespace metal;\n";
  out << "struct VSOut { float4 position [[position]]; float2 uv; };\n";
  out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], texture2d<float> tex0 [[texture(0)]], sampler samp0 [[sampler(0)]]) {\n";
  out << "  return tex0.sample(samp0, in.uv);\n";
  out << "}\n";
  out << "// variant " << variantHash << "\n";
  return out.str();
}

ObjcPtr<id<MTLLibrary>> makeLibrary(id<MTLDevice> device, const std::string& source) {
  NSError* error = nil;
  NSString* nsSource = makeNSString(source);
  id<MTLLibrary> library = [device newLibraryWithSource:nsSource options:nil error:&error];
  [nsSource release];
  if (!library) {
    NSLog(@"dxmt9: Metal shader compile failed: %@", error);
    return {};
  }
  return ObjcPtr<id<MTLLibrary>>::adopt(library);
}

ObjcPtr<NSURL*> makeShaderArchiveURL() {
  @autoreleasepool {
    NSArray<NSString*>* caches = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    NSString* basePath = caches.count > 0 ? caches[0] : NSTemporaryDirectory();
    NSString* dir = [basePath stringByAppendingPathComponent:@"dxmt9"];
    NSError* error = nil;
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:&error];
    if (error) {
      NSLog(@"dxmt9: shader cache directory creation failed: %@", error);
    }
    NSString* archivePath = [dir stringByAppendingPathComponent:@"shader-archive.metallib"];
    return ObjcPtr<NSURL*>::retain([NSURL fileURLWithPath:archivePath]);
  }
}

ObjcPtr<id<MTLBinaryArchive>> loadShaderArchive(id<MTLDevice> device, NSURL* url) {
  if (!device || !url) {
    return {};
  }
  @autoreleasepool {
    auto desc = [MTLBinaryArchiveDescriptor new];
    desc.url = url;
    NSError* error = nil;
    id<MTLBinaryArchive> archive = [device newBinaryArchiveWithDescriptor:desc error:&error];
    if (!archive) {
      desc.url = nil;
      archive = [device newBinaryArchiveWithDescriptor:desc error:&error];
    }
    [desc release];
    if (!archive) {
      if (error) {
        NSLog(@"dxmt9: binary archive creation failed: %@", error);
      }
      return {};
    }
    return ObjcPtr<id<MTLBinaryArchive>>::adopt(archive);
  }
}

void persistShaderArchive(id<MTLBinaryArchive> archive, NSURL* url) {
  if (!archive || !url) {
    return;
  }
  @autoreleasepool {
    NSError* error = nil;
    if (![archive serializeToURL:url error:&error] && error) {
      NSLog(@"dxmt9: binary archive write failed: %@", error);
    }
  }
}

std::string makeDrawShaderSource(const DrawDesc& desc, bool vertex);

struct ShaderBlob {
  std::string source;
};

std::mutex gShaderBlobMutex;
std::unordered_map<u64, std::unique_ptr<ShaderBlob>> gShaderBlobRegistry;
u64 gNextShaderBlobHandle = 1;

u64 registerShaderBlob(std::string source) {
  auto blob = std::make_unique<ShaderBlob>();
  blob->source = std::move(source);
  std::lock_guard lock(gShaderBlobMutex);
  const u64 handle = gNextShaderBlobHandle++;
  gShaderBlobRegistry.emplace(handle, std::move(blob));
  return handle;
}

ShaderBlob* findShaderBlob(u64 handle) {
  std::lock_guard lock(gShaderBlobMutex);
  if (auto it = gShaderBlobRegistry.find(handle); it != gShaderBlobRegistry.end()) {
    return it->second.get();
  }
  return nullptr;
}

std::string makeShaderSourceFromRequest(const WinemetalShaderCompileRequest& request) {
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
      return makeTranslatedVertexSource(desc.vertexShader, desc);
    }
    if (desc.vertexShader.kind == ShaderRef::Kind::FixedFunctionVertex && desc.vertexShader.vertexKey) {
      return makeFfpVertexSource(*desc.vertexShader.vertexKey, desc);
    }
    const u64 variantHash = desc.vertexShader.hash ^ desc.clipPlaneMask ^ desc.rts.color[0].sampleCount;
    return desc.textures[0].handle ? makeTexturedVertexSource(variantHash)
                                   : makeGenericVertexSource(variantHash);
  }

  if (desc.pixelShader.kind == ShaderRef::Kind::Bytecode) {
    return makeTranslatedFragmentSource(desc.pixelShader, desc);
  }
  if (desc.pixelShader.kind == ShaderRef::Kind::FixedFunctionPixel && desc.pixelShader.pixelKey) {
    return makeFfpPixelSource(*desc.pixelShader.pixelKey, desc);
  }
  const u64 variantHash = desc.pixelShader.hash ^ desc.clipPlaneMask ^ desc.rts.color[0].sampleCount;
  return desc.textures[0].handle ? makeTexturedFragmentSource(variantHash)
                                 : makeGenericFragmentSource({1.0f, 1.0f, 1.0f, 1.0f}, variantHash);
}

ShaderVariantKey makeShaderVariantKey(const DrawDesc& desc, std::span<const u32> colorFormats,
                                      std::span<const BlendAttachmentKey> blendAttachments, u32 depthFormat,
                                      u32 stencilFormat) {
  ShaderVariantKey key;
  key.hash = desc.vertexShader.hash ^ (desc.pixelShader.hash << 1) ^ desc.clipPlaneMask ^ depthFormat ^
             (stencilFormat << 1);
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

struct LayerRecord {
  ObjcPtr<CAMetalLayer*> layer;
  Handle window{};
};

class MetalBackendDevice final : public BackendDevice {
 public:
  explicit MetalBackendDevice(const BackendLimits& limits) : limits_(limits) {
    @autoreleasepool {
      device_ = ObjcPtr<id<MTLDevice>>::adopt(MTLCreateSystemDefaultDevice());
      if (!device_) {
        return;
      }
      commandQueue_ = ObjcPtr<id<MTLCommandQueue>>::adopt([device_.get() newCommandQueue]);
      shaderArchiveURL_ = makeShaderArchiveURL();
      shaderArchive_ = loadShaderArchive(device_.get(), shaderArchiveURL_.get());
    }

    if (!device_ || !commandQueue_) {
      return;
    }

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
      persistShaderArchive(shaderArchive_.get(), shaderArchiveURL_.get());
    }
    purgeResourcesUnlocked();
    layers_.clear();
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

  bool ready() const noexcept { return ready_; }

  BufferHandle createBuffer(const BufferDesc& desc) override {
    std::lock_guard lock(mutex_);
    const Handle handle{nextHandle_++};
    BufferRecord record;
    record.desc = desc;
    record.shadow.resize(static_cast<size_t>(desc.size));
    if (desc.pool != Pool::SystemMem && desc.pool != Pool::Scratch) {
      @autoreleasepool {
        const auto options = toResourceOptions(desc.pool, desc.usage);
        id<MTLBuffer> buffer = [device_.get() newBufferWithLength:static_cast<NSUInteger>(desc.size)
                                                          options:options];
        record.buffer = ObjcPtr<id<MTLBuffer>>::adopt(buffer);
      }
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
      @autoreleasepool {
        auto descriptor = [MTLTextureDescriptor new];
        descriptor.textureType = toTextureType(desc.type, false);
        descriptor.pixelFormat = toPixelFormat(desc.format);
        descriptor.width = std::max(1u, desc.width);
        descriptor.height = std::max(1u, desc.height);
        descriptor.depth = std::max(1u, desc.depth);
        descriptor.mipmapLevelCount = std::max(1u, desc.levels);
        descriptor.sampleCount = 1;
        descriptor.arrayLength = 1;
        descriptor.storageMode = toStorageMode(desc.pool, desc.usage);
        descriptor.usage = toTextureUsage(desc);
        id<MTLTexture> texture = [device_.get() newTextureWithDescriptor:descriptor];
        record.texture = ObjcPtr<id<MTLTexture>>::adopt(texture);
        [descriptor release];
      }
    }
    textures_[handle.value] = std::move(record);
    return handle;
  }

  SurfaceHandle createSurface(const SurfaceDesc& desc) override {
    std::lock_guard lock(mutex_);
    const Handle handle{nextHandle_++};
    SurfaceRecord record;
    record.desc = desc;
    if (desc.pool != Pool::SystemMem && desc.pool != Pool::Scratch) {
      @autoreleasepool {
        auto descriptor = [MTLTextureDescriptor new];
        descriptor.textureType = toTextureType(TextureType::TwoD, desc.multiSampleType != MultiSampleType::None);
        descriptor.pixelFormat = toPixelFormat(desc.format);
        descriptor.width = std::max(1u, desc.width);
        descriptor.height = std::max(1u, desc.height);
        descriptor.depth = 1;
        descriptor.mipmapLevelCount = 1;
        descriptor.sampleCount = std::max(1u, sampleCount(desc.multiSampleType));
        descriptor.storageMode = toStorageMode(desc.pool, desc.usage);
        descriptor.usage = toTextureUsage(desc);
        id<MTLTexture> texture = [device_.get() newTextureWithDescriptor:descriptor];
        record.texture = ObjcPtr<id<MTLTexture>>::adopt(texture);
        if (descriptor.sampleCount > 1) {
          MTLTextureDescriptor* resolve = [descriptor copy];
          resolve.sampleCount = 1;
          resolve.textureType = MTLTextureType2D;
          resolve.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
          id<MTLTexture> resolveTexture = [device_.get() newTextureWithDescriptor:resolve];
          record.resolveTexture = ObjcPtr<id<MTLTexture>>::adopt(resolveTexture);
          [resolve release];
        }
        [descriptor release];
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
      if (record.buffer) {
        std::memset([record.buffer.get() contents], 0, record.shadow.size());
      }
    }
    if (record.buffer) {
      return [record.buffer.get() contents];
    }
    return record.shadow.empty() ? nullptr : record.shadow.data();
  }

  void unmapBuffer(BufferHandle handle) override {
    std::lock_guard lock(mutex_);
    (void)handle;
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
    if (!desc.colorAttachments.empty()) {
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
    std::unique_lock lock(mutex_);
    // TLA+: WineCommit
    ensureWritingSlotUnlocked(lock);
    currentSlot().commands.push_back(makeReadbackCommand(desc));
    markReadbackResourcesUnlocked(desc);
    commitCurrentChunkUnlocked(lock);
    waitForSequenceUnlocked(lastCommittedSeqId_, lock);
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
    ensureWritingSlotUnlocked(lock);
    MetalCommandRecord op;
    op.kind = MetalCommandRecord::Kind::Present;
    op.present = desc;
    op.presentSource = currentBackBuffer_;
    currentSlot().commands.push_back(std::move(op));
    commitCurrentChunkUnlocked(lock);
  }

  void flush() override {
    std::unique_lock lock(mutex_);
    // TLA+: WineCommit
    commitCurrentChunkUnlocked(lock);
    waitForSequenceUnlocked(nextSeqId_ == 0 ? 0 : nextSeqId_ - 1, lock);
  }

 private:
  BufferRecord* findBufferUnlocked(u64 handle) {
    auto it = buffers_.find(handle);
    return it == buffers_.end() ? nullptr : &it->second;
  }

  TextureRecord* findTextureUnlocked(u64 handle) {
    auto it = textures_.find(handle);
    return it == textures_.end() ? nullptr : &it->second;
  }

  SurfaceRecord* findSurfaceUnlocked(u64 handle) {
    auto it = surfaces_.find(handle);
    return it == surfaces_.end() ? nullptr : &it->second;
  }

  ChunkSlot& currentSlot() {
    // TLA+: RingSafety
    DXMT_ASSERT(writingSlot_.has_value());
    return slots_[*writingSlot_];
  }

  void ensureWritingSlotUnlocked(std::unique_lock<std::mutex>& lock) {
    if (writingSlot_) {
      // TLA+: RingSafety
      DXMT_ASSERT(slots_[*writingSlot_].state == ChunkSlot::State::Writing);
      return;
    }
    writeCv_.wait(lock, [this] {
      return stop_ || (slots_[writeIndex_].state == ChunkSlot::State::Free &&
                       inflightCount_ < kMaxInflight);
    });
    if (stop_) {
      return;
    }
    // TLA+: RingSafety
    DXMT_ASSERT(slots_[writeIndex_].state == ChunkSlot::State::Free);
    slots_[writeIndex_].state = ChunkSlot::State::Writing;
    slots_[writeIndex_].seqId = 0;
    slots_[writeIndex_].commands.clear();
    writingSlot_ = writeIndex_;
  }

  void commitCurrentChunkUnlocked(std::unique_lock<std::mutex>& lock) {
    (void)lock;
    if (!writingSlot_) {
      return;
    }
    ChunkSlot& slot = slots_[*writingSlot_];
    if (slot.commands.empty()) {
      slot.state = ChunkSlot::State::Free;
      writingSlot_.reset();
      return;
    }
    writeCv_.wait(lock, [this] { return stop_ || inflightCount_ < kMaxInflight; });
    if (stop_) {
      return;
    }
    // TLA+: WineCommit
    slot.seqId = nextSeqId_++;
    slot.state = ChunkSlot::State::Pending;
    lastCommittedSeqId_ = slot.seqId;
    ++inflightCount_;
    updateLastUsedSeqIdsUnlocked(slot);
    readySlots_.push_back(*writingSlot_);
    writingSlot_.reset();
    writeIndex_ = (writeIndex_ + 1) % kRingSize;
    // TLA+: BoundedInflight
    DXMT_ASSERT(inflightCount_ <= kMaxInflight);
    // TLA+: RingSafety
    DXMT_ASSERT(slots_[writeIndex_].state == ChunkSlot::State::Free);
    encodeCv_.notify_one();
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
        size_t slotIndex = 0;
        ChunkSlot slotCopy;
        {
          std::unique_lock lock(mutex_);
          encodeCv_.wait(lock, [this] { return stop_ || !readySlots_.empty(); });
          if (stop_ && readySlots_.empty()) {
            return;
          }
          slotIndex = readySlots_.front();
          readySlots_.pop_front();
          auto& slot = slots_[slotIndex];
          // TLA+: CommandQueue
          // TLA+: EncodeSafety
          DXMT_ASSERT(slot.state == ChunkSlot::State::Pending);
          slot.state = ChunkSlot::State::Encoding;
          slotCopy = slot;
        }

        encodeChunk(slotIndex, slotCopy);
      }
    }
  }

  void encodeChunk(size_t slotIndex, const ChunkSlot& slot) {
    @autoreleasepool {
      if (!device_ || !commandQueue_) {
        finishChunk(slotIndex, slot.seqId);
        return;
      }

      id<MTLCommandBuffer> commandBuffer = [commandQueue_.get() commandBuffer];
      if (!commandBuffer) {
        finishChunk(slotIndex, slot.seqId);
        return;
      }

      id<MTLRenderCommandEncoder> activeRenderEncoder = nil;
      id<MTLBlitCommandEncoder> activeBlitEncoder = nil;
      AttachmentKey activeKey{};
      HazardBloom activeWriteBloom{};
      bool hasActiveRender = false;
      std::optional<ClearDesc> pendingClear;

      auto flushRender = [&] {
        if (activeRenderEncoder) {
          [activeRenderEncoder endEncoding];
          activeRenderEncoder = nil;
          hasActiveRender = false;
        }
      };

      auto flushBlit = [&] {
        if (activeBlitEncoder) {
          [activeBlitEncoder endEncoding];
          activeBlitEncoder = nil;
        }
      };

      auto startRenderPass = [&](const DrawDesc& draw, const std::optional<ClearDesc>& clear) {
        activeRenderEncoder = beginRenderPass(commandBuffer, draw, clear);
        hasActiveRender = activeRenderEncoder != nil;
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
            encodePresent(commandBuffer, command.present, command.presentSource);
            break;
        }
      }

      flushPendingClear();
      flushRender();
      flushBlit();

      const u64 seqId = slot.seqId;
      [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
        (void)buffer;
        std::lock_guard completionLock(mutex_);
        completedSeqQueue_.push_back(seqId);
        finishCv_.notify_all();
      }];
      [commandBuffer commit];

      {
        std::lock_guard lock(mutex_);
        slots_[slotIndex].state = ChunkSlot::State::GPU;
      }
    }
  }

  void finishChunk(size_t slotIndex, u64 seqId) {
    std::lock_guard lock(mutex_);
    auto& slot = slots_[slotIndex];
    slot.state = ChunkSlot::State::Free;
    slot.seqId = seqId;
    slot.commands.clear();
    if (inflightCount_ > 0) {
      --inflightCount_;
    }
    completedSeqId_ = std::max(completedSeqId_, seqId);
    argbufArena_.reclaim(completedSeqId_);
    lambdaStoreArena_.reclaim(completedSeqId_);
    stagingArena_.reclaim(completedSeqId_);
    copyTempArena_.reclaim(completedSeqId_);
    completedSeqQueue_.push_back(seqId);
    writeCv_.notify_all();
    finishCv_.notify_all();
  }

  id<MTLRenderCommandEncoder> beginRenderPass(id<MTLCommandBuffer> commandBuffer, const DrawDesc& draw,
                                              const std::optional<ClearDesc>& clear) {
    auto* surface = findSurfaceUnlocked(draw.rts.color[0].handle.value);
    if (!surface || !surface->texture) {
      return nil;
    }
    @autoreleasepool {
      auto desc = [MTLRenderPassDescriptor renderPassDescriptor];
      auto attachment = desc.colorAttachments[0];
      attachment.texture = surface->texture.get();
      const bool discardAfterPresent = !clear.has_value() && backBufferDiscardAfterPresent_ &&
                                       draw.rts.color[0].handle == currentBackBuffer_;
      attachment.loadAction = clear.has_value() ? MTLLoadActionClear
                                                : (discardAfterPresent ? MTLLoadActionDontCare
                                                                       : MTLLoadActionLoad);
      attachment.storeAction = MTLStoreActionStore;
      if (surface->resolveTexture) {
        attachment.resolveTexture = surface->resolveTexture.get();
        attachment.storeAction = MTLStoreActionMultisampleResolve;
      }
      if (clear.has_value()) {
        attachment.clearColor = MTLClearColorMake(clear->color.r, clear->color.g, clear->color.b, clear->color.a);
      }

      if (auto* depthSurface = findSurfaceUnlocked(draw.rts.depthStencil.handle.value);
          depthSurface && depthSurface->texture) {
        auto depth = desc.depthAttachment;
        depth.texture = depthSurface->texture.get();
        depth.loadAction = clear.has_value() ? MTLLoadActionClear : MTLLoadActionLoad;
        depth.storeAction = MTLStoreActionStore;
        if (clear.has_value()) {
          depth.clearDepth = clear->depth;
        }
      }

      id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:desc];
      if (!encoder) {
        return nil;
      }
      if (discardAfterPresent) {
        backBufferDiscardAfterPresent_ = false;
      }
      [encoder setViewport:MTLViewport{0, 0, static_cast<double>(std::max(1u, draw.viewport.viewport.width)),
                                       static_cast<double>(std::max(1u, draw.viewport.viewport.height)),
                                       static_cast<double>(draw.viewport.viewport.minZ),
                                       static_cast<double>(draw.viewport.viewport.maxZ)}];
      [encoder setCullMode:toCullMode(draw.rs.values.contains(RS_CULL_MODE) ? draw.rs.values.at(RS_CULL_MODE) : 1)];
      return encoder;
    }
  }

  void encodeDraw(id<MTLCommandBuffer> commandBuffer, id<MTLRenderCommandEncoder> encoder, const DrawDesc& draw,
                  u64 seqId) {
    (void)commandBuffer;
    if (!encoder) {
      return;
    }
    const auto depthKey = makeDepthStencilKey(draw);
    auto pipeline = pipelineForDraw(draw).get();
    if (!pipeline) {
      return;
    }
    auto depthState = depthStencilStateFor(depthKey);
    if (depthState) {
      [encoder setDepthStencilState:depthState.get()];
    }
    [encoder setRenderPipelineState:pipeline.get()];
    auto* uniforms = argbufArena_.allocate<DrawUniforms>(seqId);
    DrawUniforms fallbackUniforms{};
    if (!uniforms) {
      uniforms = &fallbackUniforms;
    }
    *uniforms = buildDrawUniforms(draw);
    [encoder setVertexBytes:uniforms length:sizeof(DrawUniforms) atIndex:0];
    [encoder setFragmentBytes:uniforms length:sizeof(DrawUniforms) atIndex:0];
    if (draw.textures[0].handle) {
      if (auto* texture = findTextureUnlocked(draw.textures[0].handle.value); texture && texture->texture) {
        [encoder setFragmentTexture:texture->texture.get() atIndex:0];
      }
      auto sampler = makeSampler(draw.samplers[0]);
      if (sampler) {
        [encoder setFragmentSamplerState:sampler.get() atIndex:0];
      }
    }
    const auto primitiveType = toPrimitiveType(draw.primitiveType);
    const u64 primitiveCount = std::max<u32>(1, draw.primitiveCount);
    if (draw.indexBuffer) {
      auto* buffer = findBufferUnlocked(draw.indexBuffer.value);
      if (buffer && buffer->buffer) {
        [encoder drawIndexedPrimitives:primitiveType
                            indexCount:static_cast<NSUInteger>(primitiveCount * 3)
                             indexType:toIndexType(draw.indexType)
                           indexBuffer:buffer->buffer.get()
                     indexBufferOffset:0];
        return;
      }
    }
    [encoder drawPrimitives:primitiveType vertexStart:0 vertexCount:static_cast<NSUInteger>(primitiveCount * 3)];
  }

  void encodeClearPass(id<MTLCommandBuffer> commandBuffer, const ClearDesc& clear) {
    if (clear.colorAttachments[0].handle == Handle{} && clear.depthStencil.handle == Handle{}) {
      return;
    }
    auto* surface = findSurfaceUnlocked(clear.colorAttachments[0].handle.value);
    if (!surface || !surface->texture) {
      return;
    }
    @autoreleasepool {
      auto desc = [MTLRenderPassDescriptor renderPassDescriptor];
      auto attachment = desc.colorAttachments[0];
      attachment.texture = surface->texture.get();
      attachment.loadAction = MTLLoadActionClear;
      attachment.storeAction = MTLStoreActionStore;
      if (surface->resolveTexture) {
        attachment.resolveTexture = surface->resolveTexture.get();
        attachment.storeAction = MTLStoreActionMultisampleResolve;
      }
      attachment.clearColor = MTLClearColorMake(clear.color.r, clear.color.g, clear.color.b, clear.color.a);
      auto encoder = [commandBuffer renderCommandEncoderWithDescriptor:desc];
      if (encoder) {
        [encoder endEncoding];
      }
    }
  }

  void encodeColorFillPass(id<MTLCommandBuffer> commandBuffer, const ClearDesc& clear) {
    encodeClearPass(commandBuffer, clear);
  }

  void encodeColorFill(id<MTLCommandBuffer> commandBuffer, const ColorFillDesc& fill) {
    auto* surface = findSurfaceUnlocked(fill.destination.value);
    if (!surface || !surface->texture) {
      return;
    }
    @autoreleasepool {
      auto desc = [MTLRenderPassDescriptor renderPassDescriptor];
      auto attachment = desc.colorAttachments[0];
      attachment.texture = surface->texture.get();
      attachment.loadAction = fill.hasRect ? MTLLoadActionLoad : MTLLoadActionClear;
      attachment.storeAction = MTLStoreActionStore;
      if (surface->resolveTexture) {
        attachment.resolveTexture = surface->resolveTexture.get();
        attachment.storeAction = MTLStoreActionMultisampleResolve;
      }
      if (!fill.hasRect) {
        attachment.clearColor = MTLClearColorMake(fill.color.r, fill.color.g, fill.color.b, fill.color.a);
      }
      auto encoder = [commandBuffer renderCommandEncoderWithDescriptor:desc];
      if (!encoder) {
        return;
      }
      if (fill.hasRect) {
        MTLScissorRect rect{};
        rect.x = static_cast<NSUInteger>(std::max(0, fill.rect.left));
        rect.y = static_cast<NSUInteger>(std::max(0, fill.rect.top));
        rect.width = static_cast<NSUInteger>(std::max(0, fill.rect.right - fill.rect.left));
        rect.height = static_cast<NSUInteger>(std::max(0, fill.rect.bottom - fill.rect.top));
        [encoder setScissorRect:rect];
        auto pipeline = pipelineForColorFill(fill.color, static_cast<u32>(toPixelFormat(surface->desc.format))).get();
        if (pipeline) {
          [encoder setRenderPipelineState:pipeline.get()];
          [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
      }
      [encoder endEncoding];
    }
  }

  void encodeSurfaceCopy(id<MTLCommandBuffer> commandBuffer, const SurfaceCopyDesc& copy) {
    auto* src = findSurfaceUnlocked(copy.source.value);
    auto* dst = findSurfaceUnlocked(copy.destination.value);
    if (!src || !dst || !src->texture || !dst->texture) {
      return;
    }
    @autoreleasepool {
      auto blit = [commandBuffer blitCommandEncoder];
      if (!blit) {
        return;
      }
      const NSUInteger srcLevel = copy.sourceLevel;
      const NSUInteger dstLevel = copy.destinationLevel;
      const NSUInteger srcW = std::max(1, copy.sourceRect.right - copy.sourceRect.left);
      const NSUInteger srcH = std::max(1, copy.sourceRect.bottom - copy.sourceRect.top);
      const NSUInteger dstW = std::max(1, copy.destinationRect.right - copy.destinationRect.left);
      const NSUInteger dstH = std::max(1, copy.destinationRect.bottom - copy.destinationRect.top);
      if (srcW == dstW && srcH == dstH) {
        [blit copyFromTexture:src->texture.get()
                  sourceSlice:0
                  sourceLevel:srcLevel
                 sourceOrigin:MTLOriginMake(copy.sourceRect.left, copy.sourceRect.top, 0)
                   sourceSize:MTLSizeMake(srcW, srcH, 1)
                    toTexture:dst->texture.get()
             destinationSlice:0
             destinationLevel:dstLevel
            destinationOrigin:MTLOriginMake(copy.destinationRect.left, copy.destinationRect.top, 0)];
      } else {
        [blit endEncoding];
        encodeStretchRect(commandBuffer, {
                                    .source = copy.source,
                                    .destination = copy.destination,
                                    .sourceRect = copy.sourceRect,
                                    .destinationRect = copy.destinationRect,
                                    .linear = true,
                                    .sourceSampleCount = src->desc.multiSampleType == MultiSampleType::None ? 1u : sampleCount(src->desc.multiSampleType),
                                    .destinationSampleCount = dst->desc.multiSampleType == MultiSampleType::None ? 1u : sampleCount(dst->desc.multiSampleType),
                                });
        return;
      }
      [blit endEncoding];
    }
  }

  void encodeStretchRect(id<MTLCommandBuffer> commandBuffer, const StretchRectDesc& stretch) {
    auto* src = findSurfaceUnlocked(stretch.source.value);
    auto* dst = findSurfaceUnlocked(stretch.destination.value);
    if (!src || !dst || !src->texture || !dst->texture) {
      return;
    }
    @autoreleasepool {
      auto desc = [MTLRenderPassDescriptor renderPassDescriptor];
      auto attachment = desc.colorAttachments[0];
      attachment.texture = dst->texture.get();
      attachment.loadAction = MTLLoadActionLoad;
      attachment.storeAction = MTLStoreActionStore;
      if (dst->resolveTexture) {
        attachment.resolveTexture = dst->resolveTexture.get();
        attachment.storeAction = MTLStoreActionMultisampleResolve;
      }
      auto encoder = [commandBuffer renderCommandEncoderWithDescriptor:desc];
      if (!encoder) {
        return;
      }
      auto pipeline = pipelineForStretchRect(stretch, static_cast<u32>(toPixelFormat(dst->desc.format))).get();
      if (!pipeline) {
        [encoder endEncoding];
        return;
      }
      [encoder setRenderPipelineState:pipeline.get()];
      [encoder setFragmentTexture:src->texture.get() atIndex:0];
      auto sampler = makeSampler(stretch.linear);
      if (sampler) {
        [encoder setFragmentSamplerState:sampler.get() atIndex:0];
      }
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [encoder endEncoding];
    }
  }

  void encodeReadback(id<MTLCommandBuffer> commandBuffer, const ReadbackDesc& readback) {
    auto* src = findSurfaceUnlocked(readback.source.value);
    auto* dst = findSurfaceUnlocked(readback.destination.value);
    if (!src || !dst || !src->texture) {
      return;
    }
    @autoreleasepool {
      auto blit = [commandBuffer blitCommandEncoder];
      if (!blit) {
        return;
      }
      id<MTLTexture> sourceTexture = src->resolveTexture ? src->resolveTexture.get() : src->texture.get();
      auto region = MTLRegionMake2D(readback.sourceRect.left, readback.sourceRect.top,
                                    std::max(1, readback.sourceRect.right - readback.sourceRect.left),
                                    std::max(1, readback.sourceRect.bottom - readback.sourceRect.top));
      auto* dstSurface = findSurfaceUnlocked(readback.destination.value);
      if (!dstSurface || !dstSurface->texture) {
        [blit endEncoding];
        return;
      }
      [blit copyFromTexture:sourceTexture
                sourceSlice:0
                sourceLevel:readback.sourceLevel
               sourceOrigin:region.origin
                 sourceSize:region.size
                  toTexture:dstSurface->texture.get()
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
      [blit endEncoding];
    }
  }

  void encodePresent(id<MTLCommandBuffer> commandBuffer, const SwapDesc& present, Handle sourceHandle) {
    auto* source = findSurfaceUnlocked(sourceHandle.value);
    if (!source || !source->texture) {
      return;
    }

    CAMetalLayer* layer = nullptr;
    if (present.window.value != 0) {
      layer = lookupLayerHandle(present.window.value);
    }
    if (!layer) {
      return;
    }

    layer.device = device_.get();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.drawableSize = CGSizeMake(std::max(1u, present.width), std::max(1u, present.height));
    layer.displaySyncEnabled = present.displaySyncEnabled;
    layer.maximumDrawableCount = std::clamp(maxFrameLatency_, 1u, 3u);
    layer.framebufferOnly = YES;

    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) {
      if (presentationStatusObserver_) {
        presentationStatusObserver_(true);
      }
      return;
    }
    if (presentationStatusObserver_) {
      presentationStatusObserver_(false);
    }

    auto blit = [commandBuffer blitCommandEncoder];
    if (blit) {
      id<MTLTexture> sourceTexture = source->resolveTexture ? source->resolveTexture.get() : source->texture.get();
      [blit copyFromTexture:sourceTexture
                sourceSlice:0
                sourceLevel:0
               sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(std::max(1u, present.width), std::max(1u, present.height), 1)
                  toTexture:drawable.texture
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
      [blit endEncoding];
    }
    [commandBuffer presentDrawable:drawable];
    backBufferDiscardAfterPresent_ = true;
  }

  ObjcPtr<id<MTLSamplerState>> makeSampler(bool linear) {
    @autoreleasepool {
      auto descriptor = [MTLSamplerDescriptor new];
      descriptor.minFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
      descriptor.magFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
      descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
      descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
      descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
      id<MTLSamplerState> sampler = [device_.get() newSamplerStateWithDescriptor:descriptor];
      [descriptor release];
      return sampler ? ObjcPtr<id<MTLSamplerState>>::adopt(sampler) : ObjcPtr<id<MTLSamplerState>>{};
    }
  }

  ObjcPtr<id<MTLSamplerState>> makeSampler(const SamplerSnapshot& snapshot) {
    const auto minFilter = snapshot.states.contains(SAMP_MIN_FILTER) ? snapshot.states.at(SAMP_MIN_FILTER) : 0u;
    const auto magFilter = snapshot.states.contains(SAMP_MAG_FILTER) ? snapshot.states.at(SAMP_MAG_FILTER) : 0u;
    return makeSampler(minFilter == 2u || magFilter == 2u);
  }

  std::shared_future<ObjcPtr<id<MTLRenderPipelineState>>> pipelineForDraw(const DrawDesc& draw) {
    auto resolvePixelFormat = [this](Handle handle) -> u32 {
      if (!handle) {
        return 0;
      }
      if (auto* surface = findSurfaceUnlocked(handle.value); surface) {
        return static_cast<u32>(toPixelFormat(surface->desc.format));
      }
      return 0;
    };

    std::array<u32, kMaxRenderTargets> colorFormats{};
    std::array<BlendAttachmentKey, kMaxRenderTargets> blendAttachments{};
    const auto& rs = draw.rs.values;
    const bool blendEnabled = rs.contains(RS_ALPHABLEND_ENABLE) && rs.at(RS_ALPHABLEND_ENABLE) != 0;
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
      blend.colorWriteMask = rs.contains(RS_COLOR_WRITE_ENABLE) ? rs.at(RS_COLOR_WRITE_ENABLE) : 0xfu;
    }
    const u32 depthFormat = resolvePixelFormat(draw.rts.depthStencil.handle);
    const u32 stencilFormat = depthFormat;
    const auto key = makeShaderVariantKey(draw, colorFormats, blendAttachments, depthFormat, stencilFormat);
    {
      std::lock_guard lock(cacheMutex_);
      if (auto it = drawPipelineCache_.find(key); it != drawPipelineCache_.end()) {
        return it->second.future;
      }
      auto future = std::async(std::launch::async, [this, draw, key]() {
        @autoreleasepool {
          auto vsSource = makeDrawShaderSource(draw, true);
          auto fsSource = makeDrawShaderSource(draw, false);
          auto vsLib = makeLibrary(device_.get(), vsSource);
          auto fsLib = makeLibrary(device_.get(), fsSource);
          if (!vsLib || !fsLib) {
            return ObjcPtr<id<MTLRenderPipelineState>>{};
          }
          auto vs = [vsLib.get() newFunctionWithName:@"dxmt9_vs"];
          auto fs = [fsLib.get() newFunctionWithName:@"dxmt9_fs"];
          if (!vs || !fs) {
            return ObjcPtr<id<MTLRenderPipelineState>>{};
          }
          auto desc = [MTLRenderPipelineDescriptor new];
          desc.vertexFunction = vs;
          desc.fragmentFunction = fs;
          desc.rasterSampleCount = std::max(1u, key.sampleCount);
          desc.alphaToCoverageEnabled = key.alphaToCoverage;
          desc.depthAttachmentPixelFormat = static_cast<MTLPixelFormat>(key.depthFormat);
          desc.stencilAttachmentPixelFormat = static_cast<MTLPixelFormat>(key.stencilFormat);
          if (shaderArchive_) {
            auto archive = shaderArchive_.get();
            NSArray<id<MTLBinaryArchive>>* archives = @[archive];
            desc.binaryArchives = archives;
            NSError* archiveError = nil;
            if (![archive addRenderPipelineFunctionsWithDescriptor:desc error:&archiveError] && archiveError) {
              NSLog(@"dxmt9: binary archive update failed: %@", archiveError);
            }
          }
          for (size_t i = 0; i < kMaxRenderTargets; ++i) {
            auto* colorAttachment = desc.colorAttachments[i];
            colorAttachment.pixelFormat = static_cast<MTLPixelFormat>(key.colorFormats[i]);
            colorAttachment.blendingEnabled = key.blend[i].blendingEnabled;
            colorAttachment.rgbBlendOperation = toBlendOperation(key.blend[i].rgbBlendOperation);
            colorAttachment.alphaBlendOperation = toBlendOperation(key.blend[i].alphaBlendOperation);
            colorAttachment.sourceRGBBlendFactor = toBlendFactor(key.blend[i].sourceRGBBlendFactor);
            colorAttachment.destinationRGBBlendFactor = toBlendFactor(key.blend[i].destinationRGBBlendFactor);
            colorAttachment.sourceAlphaBlendFactor = toBlendFactor(key.blend[i].sourceAlphaBlendFactor);
            colorAttachment.destinationAlphaBlendFactor = toBlendFactor(key.blend[i].destinationAlphaBlendFactor);
            colorAttachment.writeMask = toColorWriteMask(key.blend[i].colorWriteMask);
          }
          NSError* error = nil;
          id<MTLRenderPipelineState> pipeline = [device_.get() newRenderPipelineStateWithDescriptor:desc error:&error];
          if (!pipeline) {
            NSLog(@"dxmt9: pipeline compile failed: %@", error);
          } else if (shaderArchive_) {
            persistShaderArchive(shaderArchive_.get(), shaderArchiveURL_.get());
          }
          [desc release];
          return ObjcPtr<id<MTLRenderPipelineState>>::adopt(pipeline);
        }
      });
      auto shared = future.share();
      drawPipelineCache_.emplace(key, PipelineCacheEntry{shared});
      return shared;
    }
  }

  std::shared_future<ObjcPtr<id<MTLRenderPipelineState>>> pipelineForColorFill(const ColorRGBA& color,
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
      @autoreleasepool {
        auto vsLib = makeLibrary(device_.get(), makeGenericVertexSource(makeHash("fill")));
        auto fsLib = makeLibrary(device_.get(), makeGenericFragmentSource(color, makeHash("fill")));
        if (!vsLib || !fsLib) {
          return ObjcPtr<id<MTLRenderPipelineState>>{};
        }
        auto vs = [vsLib.get() newFunctionWithName:@"dxmt9_vs"];
        auto fs = [fsLib.get() newFunctionWithName:@"dxmt9_fs"];
        auto desc = [MTLRenderPipelineDescriptor new];
        desc.vertexFunction = vs;
        desc.fragmentFunction = fs;
        desc.colorAttachments[0].pixelFormat = static_cast<MTLPixelFormat>(pixelFormat);
        if (shaderArchive_) {
          auto archive = shaderArchive_.get();
          NSArray<id<MTLBinaryArchive>>* archives = @[archive];
          desc.binaryArchives = archives;
          NSError* archiveError = nil;
          if (![archive addRenderPipelineFunctionsWithDescriptor:desc error:&archiveError] && archiveError) {
            NSLog(@"dxmt9: binary archive update failed: %@", archiveError);
          }
        }
        NSError* error = nil;
        id<MTLRenderPipelineState> pipeline = [device_.get() newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!pipeline) {
          NSLog(@"dxmt9: color fill pipeline compile failed: %@", error);
        } else if (shaderArchive_) {
          persistShaderArchive(shaderArchive_.get(), shaderArchiveURL_.get());
        }
        [desc release];
        return ObjcPtr<id<MTLRenderPipelineState>>::adopt(pipeline);
      }
    });
    auto shared = future.share();
    fillPipelineCache_.emplace(key, PipelineCacheEntry{shared});
    return shared;
  }

  std::shared_future<ObjcPtr<id<MTLRenderPipelineState>>> pipelineForStretchRect(const StretchRectDesc& stretch,
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
      @autoreleasepool {
        auto vsLib = makeLibrary(device_.get(), makeTexturedVertexSource(makeHash("stretch")));
        auto fsLib = makeLibrary(device_.get(), makeTexturedFragmentSource(makeHash("stretch")));
        if (!vsLib || !fsLib) {
          return ObjcPtr<id<MTLRenderPipelineState>>{};
        }
        auto vs = [vsLib.get() newFunctionWithName:@"dxmt9_vs"];
        auto fs = [fsLib.get() newFunctionWithName:@"dxmt9_fs"];
        auto desc = [MTLRenderPipelineDescriptor new];
        desc.vertexFunction = vs;
        desc.fragmentFunction = fs;
        desc.rasterSampleCount = std::max(1u, stretch.destinationSampleCount);
        desc.colorAttachments[0].pixelFormat = static_cast<MTLPixelFormat>(pixelFormat);
        if (shaderArchive_) {
          auto archive = shaderArchive_.get();
          NSArray<id<MTLBinaryArchive>>* archives = @[archive];
          desc.binaryArchives = archives;
          NSError* archiveError = nil;
          if (![archive addRenderPipelineFunctionsWithDescriptor:desc error:&archiveError] && archiveError) {
            NSLog(@"dxmt9: binary archive update failed: %@", archiveError);
          }
        }
        NSError* error = nil;
        id<MTLRenderPipelineState> pipeline = [device_.get() newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!pipeline) {
          NSLog(@"dxmt9: stretch pipeline compile failed: %@", error);
        } else if (shaderArchive_) {
          persistShaderArchive(shaderArchive_.get(), shaderArchiveURL_.get());
        }
        [desc release];
        return ObjcPtr<id<MTLRenderPipelineState>>::adopt(pipeline);
      }
    });
    auto shared = future.share();
    stretchPipelineCache_.emplace(key, PipelineCacheEntry{shared});
    return shared;
  }

  ObjcPtr<id<MTLDepthStencilState>> depthStencilStateFor(const DepthStencilKey& key) {
    std::lock_guard lock(cacheMutex_);
    if (auto it = depthCache_.find(key); it != depthCache_.end()) {
      return it->second;
    }
    @autoreleasepool {
      auto desc = [MTLDepthStencilDescriptor new];
      desc.depthCompareFunction = toCompareFunction(key.depthFunc);
      desc.depthWriteEnabled = key.depthEnable && key.depthWrite;
      auto applyFace = [](MTLStencilDescriptor* stencil, const StencilFaceKey& face) {
        if (!stencil) {
          return;
        }
        stencil.stencilCompareFunction = toCompareFunction(face.compareFunction);
        stencil.stencilFailureOperation = toStencilOperation(face.failureOperation);
        stencil.depthFailureOperation = toStencilOperation(face.depthFailureOperation);
        stencil.depthStencilPassOperation = toStencilOperation(face.passOperation);
        stencil.readMask = face.readMask;
        stencil.writeMask = face.writeMask;
      };
      if (key.front.enabled || key.back.enabled) {
        auto front = [MTLStencilDescriptor new];
        auto back = [MTLStencilDescriptor new];
        applyFace(front, key.front);
        applyFace(back, key.back.enabled ? key.back : key.front);
        desc.frontFaceStencil = front;
        desc.backFaceStencil = back;
        [front release];
        [back release];
      }
      id<MTLDepthStencilState> state = [device_.get() newDepthStencilStateWithDescriptor:desc];
      [desc release];
      auto wrapped = ObjcPtr<id<MTLDepthStencilState>>::adopt(state);
      depthCache_.emplace(key, wrapped);
      return wrapped;
    }
  }

  void finishLoop() {
    @autoreleasepool {
      while (true) {
        u64 seqId = 0;
        {
          std::unique_lock lock(mutex_);
          finishCv_.wait(lock, [this] { return stop_ || !completedSeqQueue_.empty(); });
          if (stop_ && completedSeqQueue_.empty()) {
            return;
          }
          seqId = completedSeqQueue_.front();
          completedSeqQueue_.pop_front();
          completedSeqId_ = std::max(completedSeqId_, seqId);
          if (inflightCount_ > 0) {
            --inflightCount_;
          }
          reclaimCompletedSlotsUnlocked(seqId);
          argbufArena_.reclaim(completedSeqId_);
          lambdaStoreArena_.reclaim(completedSeqId_);
          stagingArena_.reclaim(completedSeqId_);
          copyTempArena_.reclaim(completedSeqId_);
          tryGarbageCollectUnlocked();
          finishCv_.notify_all();
          writeCv_.notify_all();
        }
      }
    }
  }

  void reclaimCompletedSlotsUnlocked(u64 seqId) {
    for (auto& slot : slots_) {
      if (slot.state == ChunkSlot::State::GPU && slot.seqId == seqId) {
        slot.state = ChunkSlot::State::Free;
        slot.commands.clear();
        slot.seqId = 0;
        // TLA+: SeqIdSafety
        DXMT_ASSERT(completedSeqId_ <= nextSeqId_);
      }
    }
  }

  void waitForSequenceUnlocked(u64 seqId, std::unique_lock<std::mutex>& lock) {
    finishCv_.wait(lock, [this, seqId] { return stop_ || completedSeqId_ >= seqId; });
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
  ObjcPtr<id<MTLDevice>> device_;
  ObjcPtr<id<MTLCommandQueue>> commandQueue_;
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
  std::mutex cacheMutex_;
  std::unordered_map<ShaderVariantKey, PipelineCacheEntry, ShaderVariantKeyHash> drawPipelineCache_;
  std::unordered_map<ShaderVariantKey, PipelineCacheEntry, ShaderVariantKeyHash> fillPipelineCache_;
  std::unordered_map<ShaderVariantKey, PipelineCacheEntry, ShaderVariantKeyHash> stretchPipelineCache_;
  std::unordered_map<DepthStencilKey, ObjcPtr<id<MTLDepthStencilState>>, DepthStencilKeyHash> depthCache_;
  std::unordered_map<u64, LayerRecord> layers_;
  RingArena argbufArena_{1 << 20};
  RingArena lambdaStoreArena_{1 << 18};
  RingArena stagingArena_{1 << 20};
  RingArena copyTempArena_{1 << 20};
  ObjcPtr<NSURL*> shaderArchiveURL_;
  ObjcPtr<id<MTLBinaryArchive>> shaderArchive_;
  bool ready_ = false;
};

}  // namespace

std::shared_ptr<BackendDevice> makeMetalBackendDevice(const BackendLimits& limits) {
  auto backend = std::make_shared<MetalBackendDevice>(limits);
  return backend->ready() ? std::static_pointer_cast<BackendDevice>(std::move(backend)) : std::shared_ptr<BackendDevice>{};
}

}  // namespace dxmt9::core

extern "C" {

using dxmt9::core::u32;
using dxmt9::core::u64;

u64 winemetal_get_view_for_hwnd(u64 hwnd) {
  return hwnd;
}

u64 winemetal_create_metal_layer(u64 viewHandle, u64 deviceHandle, const WinemetalPresentParams* params) {
  @autoreleasepool {
    auto* view = reinterpret_cast<NSView*>(static_cast<uintptr_t>(viewHandle));
    auto* device = reinterpret_cast<id<MTLDevice>>(static_cast<uintptr_t>(deviceHandle));
    auto* layer = [CAMetalLayer layer];
    if (device) {
      layer.device = device;
    }
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.drawableSize = CGSizeMake(params ? std::max(1u, params->width) : 1u,
                                    params ? std::max(1u, params->height) : 1u);
    layer.displaySyncEnabled = params ? params->displaySyncEnabled : YES;
    layer.maximumDrawableCount = 3;
    layer.framebufferOnly = YES;
    if (view) {
      view.wantsLayer = YES;
      view.layer = layer;
    }
    auto handle = static_cast<u64>(reinterpret_cast<uintptr_t>([layer retain]));
    dxmt9::core::registerLayerHandle(handle, layer);
    return handle;
  }
}

void winemetal_resize_metal_layer(u64 layerHandle, u32 width, u32 height) {
  auto* layer = reinterpret_cast<CAMetalLayer*>(static_cast<uintptr_t>(layerHandle));
  if (layer) {
    layer.drawableSize = CGSizeMake(std::max(1u, width), std::max(1u, height));
  }
}

void winemetal_set_sync_enabled(u64 layerHandle, bool enabled) {
  auto* layer = reinterpret_cast<CAMetalLayer*>(static_cast<uintptr_t>(layerHandle));
  if (layer) {
    layer.displaySyncEnabled = enabled ? YES : NO;
  }
}

void winemetal_destroy_metal_layer(u64 layerHandle) {
  auto* layer = reinterpret_cast<CAMetalLayer*>(static_cast<uintptr_t>(layerHandle));
  if (!layer) {
    return;
  }
  dxmt9::core::unregisterLayerHandle(layerHandle);
  [layer release];
}

u64 winemetal_next_drawable(u64 layerHandle) {
  @autoreleasepool {
    auto* layer = reinterpret_cast<CAMetalLayer*>(static_cast<uintptr_t>(layerHandle));
    if (!layer) {
      return 0;
    }
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) {
      return 0;
    }
    return static_cast<u64>(reinterpret_cast<uintptr_t>([drawable retain]));
  }
}

void winemetal_present_drawable(u64 commandBufferHandle, u64 drawableHandle) {
  auto* commandBuffer = reinterpret_cast<id<MTLCommandBuffer>>(static_cast<uintptr_t>(commandBufferHandle));
  auto* drawable = reinterpret_cast<id<CAMetalDrawable>>(static_cast<uintptr_t>(drawableHandle));
  if (!commandBuffer || !drawable) {
    return;
  }
  [commandBuffer presentDrawable:drawable];
  [drawable release];
}

u64 winemetal_compile_shader(const WinemetalShaderCompileRequest* request) {
  if (!request) {
    return 0;
  }
  try {
    return dxmt9::core::registerShaderBlob(dxmt9::core::makeShaderSourceFromRequest(*request));
  } catch (const std::exception& e) {
    NSLog(@"dxmt9: shader compilation request failed: %s", e.what());
    return 0;
  }
}

const char* winemetal_shader_source(u64 shaderHandle) {
  auto* blob = dxmt9::core::findShaderBlob(shaderHandle);
  return blob ? blob->source.c_str() : nullptr;
}

u64 winemetal_shader_source_size(u64 shaderHandle) {
  auto* blob = dxmt9::core::findShaderBlob(shaderHandle);
  return blob ? static_cast<u64>(blob->source.size()) : 0;
}

void winemetal_destroy_shader(u64 shaderHandle) {
  std::lock_guard lock(dxmt9::core::gShaderBlobMutex);
  dxmt9::core::gShaderBlobRegistry.erase(shaderHandle);
}

}  // extern "C"
