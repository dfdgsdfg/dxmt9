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
#include "dxmt9_presenter.hpp"
#include "dxmt9_presenter_macdrv.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9_blit_encoders.hpp"
#include "dxmt9_d3d9_bytecode.hpp"
#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_shader_translator.hpp"
#include "dxmt9_ring_arena.hpp"
#include "dxmt9_shader_service.hpp"
#include "dxmt9_shader_sources.hpp"
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

WMT::Reference<WMT::CommandBuffer> bootstrapCommandBuffer(WMT::CommandQueue& queue) {
  if (!queue) {
    return {};
  }
  return queue.commandBuffer();
}

// makeHash + makeLibraryWMT + shader source helpers moved to
// dxmt9_shader_sources.{hpp,cpp}; callers use dxmt9::shaders:: qualified.
using dxmt9::shaders::makeHash;
using dxmt9::shaders::makeLibrary;
using dxmt9::shaders::makeGenericVertexSource;
using dxmt9::shaders::makeGenericFragmentSource;
using dxmt9::shaders::makeTexturedVertexSource;
using dxmt9::shaders::makeTexturedFragmentSource;
using dxmt9::shaders::initShaderArchive;
using dxmt9::shaders::persistShaderArchive;
using dxmt9::shaders::makeShaderPrelude;

// Type conversion helpers moved to dxmt9_format_convert.{hpp,cpp}
// (dxmt9::convert:: namespace). Imported via `using` declarations below.
using dxmt9::convert::toPixelFormat;
using dxmt9::convert::formatHasStencilAspect;
using dxmt9::convert::formatHasDepthAspect;
using dxmt9::convert::toTextureType;
using dxmt9::convert::toResourceOptions;
using dxmt9::convert::toTextureUsage;
using dxmt9::convert::toPrimitiveType;
using dxmt9::convert::toIndexType;
using dxmt9::convert::toCompareFunction;
using dxmt9::convert::toBlendOperation;
using dxmt9::convert::toBlendFactor;
using dxmt9::convert::toCullMode;
using dxmt9::convert::toStencilOperation;
using dxmt9::convert::toColorWriteMask;

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

// Pipeline-cache key types + Cache class moved to dxmt9_pipeline_cache.{hpp,cpp}
// (dxmt9::pipeline:: namespace). Imported as aliases so the rest of this TU
// keeps using the short names.
using BlendAttachmentKey = dxmt9::pipeline::BlendAttachmentKey;
using BlendAttachmentKeyHash = dxmt9::pipeline::BlendAttachmentKeyHash;
using StencilFaceKey = dxmt9::pipeline::StencilFaceKey;
using StencilFaceKeyHash = dxmt9::pipeline::StencilFaceKeyHash;
using ShaderVariantKey = dxmt9::pipeline::ShaderVariantKey;
using ShaderVariantKeyHash = dxmt9::pipeline::ShaderVariantKeyHash;
using DepthStencilKey = dxmt9::pipeline::DepthStencilKey;
using DepthStencilKeyHash = dxmt9::pipeline::DepthStencilKeyHash;
using PipelineCacheEntry = dxmt9::pipeline::Entry;
using PipelineCache = dxmt9::pipeline::Cache;

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

// Resource record types + Pool moved to dxmt9_resource_pool.{hpp,cpp}
// (dxmt9::resources:: namespace). Aliased so the rest of this TU keeps
// the short names.
using BufferRecord = dxmt9::resources::BufferRecord;
using TextureRecord = dxmt9::resources::TextureRecord;
using SurfaceRecord = dxmt9::resources::SurfaceRecord;

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

// RingArena + FrameAllocators moved to dxmt9_ring_arena.hpp
// (dxmt9::scratch:: namespace). Alias keeps short names here.
using RingArena = dxmt9::scratch::RingArena;

// ShaderVariantKey / DepthStencilKey / PipelineCache all live in
// dxmt9_pipeline_cache.{hpp,cpp}; aliased above.

// Container for the three resource pools + the gpu-texture-dump dedup set +
// the handle allocator. Grouped per C6 as a named structure; the underlying
// mutex is still commandQueue_->mutex_ (shared with queue state) pending a
// mutex split when the pools move out of MetalBackendDevice entirely.
using ResourcePool = dxmt9::resources::Pool;

// Per-frame bump-ring allocators used by the encode thread for transient
using FrameAllocators = dxmt9::scratch::FrameAllocators;

NSString* makeNSString(const std::string& text) {
  return [[NSString alloc] initWithUTF8String:text.c_str()];
}

// D3DShaderStage / D3DDecodedInstruction / SpirvModule moved to
// dxmt9_d3d9_bytecode.hpp (dxmt9::d3d9bc:: namespace). Aliased below.

// makeShaderPrelude moved to dxmt9_shader_sources.{hpp,cpp}
// (dxmt9::shaders:: namespace). Keeping a dead-code-to-be-removed marker:

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

// FVF / D3D-decl constants moved to dxmt9_ffp_shaders.hpp
// (dxmt9::ffp:: namespace). Aliased into this TU below.
using dxmt9::ffp::kFvfPositionMask;
using dxmt9::ffp::kFvfXyz;
using dxmt9::ffp::kFvfXyzrhw;
using dxmt9::ffp::kFvfNormal;
using dxmt9::ffp::kFvfDiffuse;
using dxmt9::ffp::kFvfSpecular;
using dxmt9::ffp::kFvfTexCountMask;
using dxmt9::ffp::kFvfTexCountShift;
using dxmt9::ffp::kD3DDeclTypeFloat1;
using dxmt9::ffp::kD3DDeclTypeFloat2;
using dxmt9::ffp::kD3DDeclTypeFloat3;
using dxmt9::ffp::kD3DDeclTypeFloat4;
using dxmt9::ffp::kD3DDeclTypeD3DColor;
using dxmt9::ffp::kD3DDeclUsagePosition;
using dxmt9::ffp::kD3DDeclUsagePSize;
using dxmt9::ffp::kD3DDeclUsageTexcoord;
using dxmt9::ffp::kD3DDeclUsagePositionT;
using dxmt9::ffp::kD3DDeclUsageColor;
using dxmt9::ffp::kD3DDeclUsageFog;
using dxmt9::ffp::declTypeSize;
// kD3DSP_DCL_* now in dxmt9_d3d9_bytecode.hpp (accessed via the
// `using namespace dxmt9::d3d9bc;` introduced below alongside the opcodes).

// FixedFunctionVertexLayout moved to dxmt9_ffp_shaders.{hpp,cpp}
using FixedFunctionVertexLayout = dxmt9::ffp::FixedFunctionVertexLayout;

// VertexInputBinding / VertexShaderInputLayout moved to dxmt9_ffp_shaders.hpp.
using VertexInputBinding = dxmt9::ffp::VertexInputBinding;
using VertexShaderInputLayout = dxmt9::ffp::VertexShaderInputLayout;
using dxmt9::ffp::computeVertexDeclStride;
using dxmt9::ffp::hashVertexShaderInputLayout;
using dxmt9::ffp::decodeFixedFunctionVertexLayout;

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

// declTypeSize + fvfTexcoordSize moved to dxmt9_ffp_shaders.cpp (anonymous).

// hashFixedFunctionLayout moved to dxmt9_ffp_shaders.cpp (anonymous).


u32 indexElementSize(IndexType type) {
  return type == IndexType::UInt32 ? 4u : 2u;
}

// D3DRegisterKind / D3DRegisterRef + kD3DSIO_* + kD3DSPR_* + kD3DSP_DCL_*
// moved to dxmt9_d3d9_bytecode.hpp (dxmt9::d3d9bc:: namespace). Aliased below.
using dxmt9::d3d9bc::D3DShaderStage;
using dxmt9::d3d9bc::D3DDecodedInstruction;
using dxmt9::d3d9bc::SpirvModule;
using dxmt9::d3d9bc::D3DRegisterKind;
using dxmt9::d3d9bc::D3DRegisterRef;
using namespace dxmt9::d3d9bc;  // pulls in all kD3DSIO_* / kD3DSPR_* / kD3DSP_DCL_*




// makeGenericVertexSource/makeGenericFragmentSource/makeTexturedVertexSource/
// makeTexturedFragmentSource/makeLibraryWMT/initShaderArchive/persistShaderArchiveWMT
// + makeShaderPrelude moved to dxmt9_shader_sources.{hpp,cpp}.
// makeFfpVertexSource / makeFfpPixelSource moved to dxmt9_ffp_shaders.{hpp,cpp}.
using dxmt9::ffp::makeFfpVertexSource;
using dxmt9::ffp::makeFfpPixelSource;
using dxmt9::translator::makeTranslatedVertexSource;
using dxmt9::translator::makeTranslatedFragmentSource;


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
  MetalBackendDevice(const BackendLimits& limits, WMT::Reference<WMT::Device> wmtDevice,
                     dxmt9::CommandQueue& commandQueue,
                     WMT::Reference<WMT::BinaryArchive>& shaderArchive,
                     const std::string& shaderArchivePath,
                     dxmt9::Device& upperDevice)
      : limits_(limits), commandQueue_(&commandQueue), upperDevice_(&upperDevice),
        shaderArchive_(&shaderArchive), shaderArchivePath_(&shaderArchivePath) {
    wrappedDevice_ = std::move(wmtDevice);
    if (!wrappedDevice_ || !commandQueue_->valid()) {
      return;
    }
    limits_.supportsDepth24Stencil8 = wrappedDevice_.supportsDepth24Stencil8();
    // Shader archive is owned by DeviceImpl; we just reference it.

    commandQueue_->queueLifecycle_.bindTrackedSubmissionState({
        .writingSlot = &commandQueue_->writingSlot_,
        .writeIndex = &commandQueue_->writeIndex_,
        .nextSeqId = &commandQueue_->nextSeqId_,
        .readySlots = &commandQueue_->readySlots_,
        .completedSeqQueue = &commandQueue_->completedSeqQueue_,
        .inflightCount = &commandQueue_->inflightCount_,
        .completedSeqId = &commandQueue_->completedSeqId_,
        .lastCommittedSeqId = &commandQueue_->lastCommittedSeqId_,
        .slots = std::span<ChunkSlot>(commandQueue_->slots_.data(), commandQueue_->slots_.size()),
        .mutex = &commandQueue_->mutex_,
        .writeCv = &commandQueue_->writeCv_,
        .encodeCv = &commandQueue_->encodeCv_,
        .finishCv = &commandQueue_->finishCv_,
        .stop = &commandQueue_->stop_,
        .submissionDiagnostics = &commandQueue_->submissionDiagnostics_,
        .resolveSurfaceFlags = [this](Handle handle) { return compatFlagsForSurfaceUnlocked(handle); },
    });

    commandQueue_->startThreads(
        [this] { encodeLoop(); },
        [this] { finishLoop(); },
        [this] { completionWatcherLoop(); });
    ready_ = true;
  }

  ~MetalBackendDevice() override {
    // Threads are owned by CommandQueue but run bodies on *this; stop them
    // before tearing down our own state.
    if (commandQueue_) {
      commandQueue_->stopThreads();
    }
    std::lock_guard lock(commandQueue_->mutex_);
    if (*shaderArchive_) {
      persistShaderArchive(*shaderArchive_, *shaderArchivePath_);
    }
    purgeResourcesUnlocked();
  }

  void completionWatcherLoop() {
    while (commandQueue_->queueLifecycle_.processOnePendingCompletion(commandQueue_->stop_)) {
      // continue until processOnePendingCompletion returns false (stop)
    }
  }

  // Observer setters + setMaxFrameLatency: no overrides here. Factory wires
  // observers directly on the upper dxmt9::Device in createDevice (task 3).
  // BackendDevice's default no-op bodies are correct for this class since
  // production never calls backend->setDeviceLostObserver anymore — the
  // invocation path goes through upperDevice_->notifyPresentationStatus
  // from encodePresent, with storage on DeviceImpl.

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
      std::lock_guard lock(commandQueue_->mutex_);
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

      auto commandBuffer = bootstrapCommandBuffer(commandQueue_->raw());
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
        auto cmdBuf2 = bootstrapCommandBuffer(commandQueue_->raw());
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
    std::lock_guard lock(commandQueue_->mutex_);
    const Handle handle{pool_.nextHandle++};
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
    pool_.buffers[handle.value] = std::move(record);
    return handle;
  }

  TextureHandle createTexture(const TextureDesc& desc) override {
    std::lock_guard lock(commandQueue_->mutex_);
    const Handle handle{pool_.nextHandle++};
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
    pool_.textures[handle.value] = std::move(record);
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
    std::lock_guard lock(commandQueue_->mutex_);
    const Handle handle{pool_.nextHandle++};
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
    pool_.surfaces[handle.value] = std::move(record);
    return handle;
  }

  SurfaceHandle createSurfaceForTexture(TextureHandle textureHandle, u32 level, const SurfaceDesc& desc) override {
    std::lock_guard lock(commandQueue_->mutex_);
    auto textureIt = pool_.textures.find(textureHandle.value);
    if (textureIt == pool_.textures.end() || !textureIt->second.texture) {
      return {};
    }

    const Handle handle{pool_.nextHandle++};
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

    pool_.surfaces[handle.value] = std::move(record);
    return handle;
  }

  void destroyBuffer(BufferHandle handle) override {
    std::lock_guard lock(commandQueue_->mutex_);
    if (auto it = pool_.buffers.find(handle.value); it != pool_.buffers.end()) {
      it->second.destroyPending = true;
      tryGarbageCollectUnlocked();
    }
  }

  void destroyTexture(TextureHandle handle) override {
    std::lock_guard lock(commandQueue_->mutex_);
    if (auto it = pool_.textures.find(handle.value); it != pool_.textures.end()) {
      it->second.destroyPending = true;
      tryGarbageCollectUnlocked();
    }
  }

  void destroySurface(SurfaceHandle handle) override {
    std::lock_guard lock(commandQueue_->mutex_);
    if (auto it = pool_.surfaces.find(handle.value); it != pool_.surfaces.end()) {
      it->second.destroyPending = true;
      tryGarbageCollectUnlocked();
    }
  }

  void* mapBuffer(BufferHandle handle, u32 flags) override {
    std::unique_lock lock(commandQueue_->mutex_);
    auto it = pool_.buffers.find(handle.value);
    if (it == pool_.buffers.end()) {
      return nullptr;
    }
    if ((flags & UsageDiscard) == 0 && (flags & UsageNoOverwrite) == 0 &&
        it->second.lastUsedSeqId > commandQueue_->completedSeqId_) {
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
    std::lock_guard lock(commandQueue_->mutex_);
    (void)handle;
  }

  void uploadBufferData(BufferHandle handle, std::span<const u8> bytes) override {
    std::lock_guard lock(commandQueue_->mutex_);
    auto it = pool_.buffers.find(handle.value);
    if (it == pool_.buffers.end()) {
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
    std::lock_guard lock(commandQueue_->mutex_);
    auto it = pool_.textures.find(handle.value);
    if (it == pool_.textures.end() || !it->second.texture || bytes.empty()) {
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
      auto commandBuffer = bootstrapCommandBuffer(commandQueue_->raw());
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
    std::unique_lock lock(commandQueue_->mutex_);
    // TLA+: WineCommit
    ensureWritingSlotUnlocked(lock);
    currentSlot().commands.push_back(makeDrawCommand(desc));
    currentBackBuffer_ = desc.rts.color[0].handle;
    markDrawResourcesUnlocked(desc);
  }

  void submitClear(const ClearDesc& desc) override {
    std::unique_lock lock(commandQueue_->mutex_);
    // TLA+: WineCommit
    ensureWritingSlotUnlocked(lock);
    currentSlot().commands.push_back(makeClearCommand(desc));
    if (desc.colorAttachments[0].handle) {
      currentBackBuffer_ = desc.colorAttachments[0].handle;
    }
    markClearResourcesUnlocked(desc);
  }

  void submitSurfaceCopy(const SurfaceCopyDesc& desc) override {
    std::unique_lock lock(commandQueue_->mutex_);
    // TLA+: WineCommit
    ensureWritingSlotUnlocked(lock);
    currentSlot().commands.push_back(makeSurfaceCopyCommand(desc));
    markSurfaceCopyResourcesUnlocked(desc);
  }

  void submitStretchRect(const StretchRectDesc& desc) override {
    std::unique_lock lock(commandQueue_->mutex_);
    // TLA+: WineCommit
    ensureWritingSlotUnlocked(lock);
    currentSlot().commands.push_back(makeStretchRectCommand(desc));
    markStretchResourcesUnlocked(desc);
  }

  void submitReadback(const ReadbackDesc& desc) override {
    std::lock_guard lock(commandQueue_->mutex_);
    // Readback is satisfied by the synchronous staging copy in readbackSurface().
    // We still track resource liveness so NoUseAfterFree remains meaningful.
    markReadbackResourcesUnlocked(desc);
  }

  void submitColorFill(const ColorFillDesc& desc) override {
    std::unique_lock lock(commandQueue_->mutex_);
    // TLA+: WineCommit
    ensureWritingSlotUnlocked(lock);
    currentSlot().commands.push_back(makeColorFillCommand(desc));
    currentBackBuffer_ = desc.destination;
    markColorFillResourcesUnlocked(desc);
  }

  void present(const SwapDesc& desc) override {
    std::unique_lock lock(commandQueue_->mutex_);
    // TLA+: WineCommit
    commandQueue_->queueLifecycle_.presentAndCommit(lock, kMaxInflight, desc, currentBackBuffer_, [this](const ChunkSlot& slot) {
      updateLastUsedSeqIdsUnlocked(slot);
    });
  }

 void flush() override {
    std::unique_lock lock(commandQueue_->mutex_);
    // TLA+: WineCommit
    commandQueue_->queueLifecycle_.flushAndWait(lock, kMaxInflight, [this](const ChunkSlot& slot) {
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

  BufferRecord* findBufferUnlocked(u64 handle) { return pool_.findBuffer(handle); }
  const BufferRecord* findBufferUnlocked(u64 handle) const { return pool_.findBuffer(handle); }
  TextureRecord* findTextureUnlocked(u64 handle) { return pool_.findTexture(handle); }
  const TextureRecord* findTextureUnlocked(u64 handle) const { return pool_.findTexture(handle); }
  SurfaceRecord* findSurfaceUnlocked(u64 handle) { return pool_.findSurface(handle); }
  const SurfaceRecord* findSurfaceUnlocked(u64 handle) const { return pool_.findSurface(handle); }

  ChunkSlot& currentSlot() {
    // TLA+: RingSafety
    DXMT_ASSERT(commandQueue_->writingSlot_.has_value());
    return commandQueue_->slots_[*commandQueue_->writingSlot_];
  }

  void ensureWritingSlotUnlocked(std::unique_lock<std::mutex>& lock) {
    (void)commandQueue_->queueLifecycle_.ensureWriterSlot(lock, kMaxInflight);
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
      seqId = commandQueue_->nextSeqId_;
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
      seqId = commandQueue_->nextSeqId_;
    }
    for (const auto& attachment : desc.colorAttachments) {
      markSurfaceUseUnlocked(attachment.handle, seqId);
    }
    markSurfaceUseUnlocked(desc.depthStencil.handle, seqId);
  }

  void markSurfaceCopyResourcesUnlocked(const SurfaceCopyDesc& desc, u64 seqId = 0) {
    if (seqId == 0) {
      seqId = commandQueue_->nextSeqId_;
    }
    markSurfaceUseUnlocked(desc.source, seqId);
    markSurfaceUseUnlocked(desc.destination, seqId);
  }

  void markStretchResourcesUnlocked(const StretchRectDesc& desc, u64 seqId = 0) {
    if (seqId == 0) {
      seqId = commandQueue_->nextSeqId_;
    }
    markSurfaceUseUnlocked(desc.source, seqId);
    markSurfaceUseUnlocked(desc.destination, seqId);
  }

  void markReadbackResourcesUnlocked(const ReadbackDesc& desc, u64 seqId = 0) {
    if (seqId == 0) {
      seqId = commandQueue_->nextSeqId_;
    }
    markSurfaceUseUnlocked(desc.source, seqId);
    markSurfaceUseUnlocked(desc.destination, seqId);
  }

  void markColorFillResourcesUnlocked(const ColorFillDesc& desc, u64 seqId = 0) {
    if (seqId == 0) {
      seqId = commandQueue_->nextSeqId_;
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
        std::unique_lock lock(commandQueue_->mutex_);
        if (!commandQueue_->queueLifecycle_.runEncodeIteration(
                lock,
                [this](size_t slotIndex, const ChunkSlot& slot) {
                  return encodeChunk(slotIndex, slot);
                },
                [this](u64) {
                  allocators_.reclaim(commandQueue_->completedSeqId_);
                })) {
          return;
        }
      }
    }
  }

  std::optional<metalqueue::QueueSubmissionRecord> encodeChunk(size_t slotIndex, const ChunkSlot& slot) {
    {
      if (!wrappedDevice_ || !commandQueue_ || !commandQueue_->valid()) {
        return std::nullopt;
      }

      auto ownedCommandBuffer = bootstrapCommandBuffer(commandQueue_->raw());
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
      // Hand the retained WMT::CommandBuffer to the queue. `commandBuffer`
      // (local WMT::Reference<WMT::CommandBuffer> in encodeChunk) is moved
      // out; the completion-watcher thread owns it from here and releases
      // it once the GPU finishes waitUntilCompleted().
      metalqueue::QueueSubmissionRecord record;
      record.commandBuffer = std::move(commandBuffer);
      record.slotIndex = slotIndex;
      record.seqId = seqId;
      record.commands = std::span<const MetalCommandRecord>(slot.commands.data(), slot.commands.size());
      record.context = "queue";
      return record;
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
    auto* uniforms = allocators_.argbuf.allocate<DrawUniforms>(seqId);
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
    dxmt9::encoders::encodeColorFill(commandBuffer, pool_, pipelineCache_, wrappedDevice_,
                                       limits_, shaderArchive_, shaderArchivePath_, fill);
  }

  void encodeSurfaceCopy(WMT::CommandBuffer& commandBuffer, const SurfaceCopyDesc& copy) {
    dxmt9::encoders::encodeSurfaceCopy(commandBuffer, pool_, pipelineCache_, wrappedDevice_,
                                         limits_, shaderArchive_, shaderArchivePath_, copy);
  }

  void encodeStretchRect(WMT::CommandBuffer& commandBuffer, const StretchRectDesc& stretch) {
    dxmt9::encoders::encodeStretchRect(commandBuffer, pool_, pipelineCache_, wrappedDevice_,
                                         limits_, shaderArchive_, shaderArchivePath_, stretch);
  }

  void encodeReadback(WMT::CommandBuffer& commandBuffer, const ReadbackDesc& readback) {
    dxmt9::encoders::encodeReadback(commandBuffer, pool_, readback);
  }

  void encodePresent(WMT::CommandBuffer& commandBuffer, const SwapDesc& present, Handle sourceHandle, u64 seqId) {
    dxmt9::presentimpl::traceEvent("begin", seqId, present.window.value);
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
      dxmt9::presentimpl::traceEvent("missing-source", seqId, present.window.value);
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

    // The originating core::SwapChain owns the Presenter and passes it via
    // SwapDesc. Missing presenter = no layer available (hwnd=0 or failed
    // acquisition in SwapChain::ensurePresenter).
    dxmt9::Presenter* presenter = present.presenter;
    if (!presenter) {
      dxmt9::presentimpl::traceEvent("missing-layer", seqId, present.window.value);
      return;
    }

    const bool opaqueAlpha =
        source->desc.format == Format::X8R8G8B8 || source->desc.format == Format::X8B8G8R8;

    dxmt9::Presenter::EncodeParams params{};
    params.source = WMT::Texture{sourceTextureHandle};
    params.width = present.width;
    params.height = present.height;
    params.displaySyncEnabled = present.displaySyncEnabled;
    params.contentsScale = 1.0;
    params.maxDrawableCount = upperDevice_ ? upperDevice_->maxFrameLatency() : 3u;
    params.opaqueAlpha = opaqueAlpha;
    params.seqId = seqId;

    const auto presentResult = presenter->encodeCommands(commandBuffer, params);
    if (!presentResult.acquired) {
      if (upperDevice_) upperDevice_->notifyPresentationStatus(true);
      return;
    }
    if (upperDevice_) upperDevice_->notifyPresentationStatus(false);
    if (!presentResult.encoded) {
      return;
    }
    backBufferDiscardAfterPresent_ = true;
  }

  void dumpTextureSnapshotUnlocked(Handle handle, const TextureDesc& desc,
                                   obj_handle_t sourceTextureHandle) {
    if (!sourceTextureHandle || !shouldDumpGpuTexture(handle) ||
        pool_.dumpedGpuTextures.contains(handle.value)) {
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
      pool_.dumpedGpuTextures.insert(handle.value);
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

    auto commandBuffer = bootstrapCommandBuffer(commandQueue_->raw());
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
      std::lock_guard lock(commandQueue_->mutex_);
      CommandBufferDiagnostics diagnostics;
      diagnostics.hasBlit = true;
      commandQueue_->submissionDiagnostics_.inspect(commandBuffer.handle, diagnostics, "gpu-dump");
    }

    // Read back via a buffer blit
    const u32 pitch = std::max(1u, desc.width) * 4u;
    std::vector<u8> bytes(static_cast<size_t>(pitch) * std::max(1u, desc.height));
    WMTBufferInfo bufInfo{};
    bufInfo.length = bytes.size();
    bufInfo.options = WMTResourceStorageModeShared;
    auto readBuf = wrappedDevice_.newBuffer(bufInfo);
    if (readBuf && bufInfo.memory.ptr) {
      auto cmdBuf2 = bootstrapCommandBuffer(commandQueue_->raw());
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
    pool_.dumpedGpuTextures.insert(handle.value);
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
      std::lock_guard lock(pipelineCache_.mutex);
      if (auto it = pipelineCache_.draw.find(key); it != pipelineCache_.draw.end()) {
        return it->second.future;
      }
      auto future = std::async(std::launch::async, [this, draw, key]() {
        auto vsSource = makeDrawShaderSource(draw, true);
        auto fsSource = makeDrawShaderSource(draw, false);
        auto vsLib = makeLibrary(wrappedDevice_, vsSource);
        auto fsLib = makeLibrary(wrappedDevice_, fsSource);
        if (!vsLib || !fsLib) {
          return WMT::Reference<WMT::RenderPipelineState>{};
        }
        auto vs = vsLib.newFunction("dxmt9_vs");
        auto fs = fsLib.newFunction("dxmt9_fs");
        if (!vs || !fs) {
          return WMT::Reference<WMT::RenderPipelineState>{};
        }
        WMTRenderPipelineInfo info{};
      info.max_tessellation_factor = 1;
        info.max_tessellation_factor = 1;
        info.vertex_function = vs.handle;
        info.fragment_function = fs.handle;
        info.raster_sample_count = std::max(1u, key.sampleCount);
        info.alpha_to_coverage_enabled = key.alphaToCoverage;
        info.depth_pixel_format = static_cast<WMTPixelFormat>(key.depthFormat);
        info.stencil_pixel_format = static_cast<WMTPixelFormat>(key.stencilFormat);
        info.rasterization_enabled = true;
        if (*shaderArchive_) {
          info.binary_archive_for_serialization = (*shaderArchive_).handle;
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
        if (pso && *shaderArchive_) {
          persistShaderArchive(*shaderArchive_, *shaderArchivePath_);
        }
        return pso;
      });
      auto shared = future.share();
      pipelineCache_.draw.emplace(key, PipelineCacheEntry{shared});
      return shared;
    }
  }


  // pipelineForPresent moved to dxmt9::Presenter (C4); each Presenter caches
  // its own pipeline per (opaqueAlpha) variant using buildPresentPipeline.

  WMT::Reference<WMT::DepthStencilState> depthStencilStateFor(const DepthStencilKey& key) {
    return pipelineCache_.depthStencilStateFor(wrappedDevice_, key);
  }

  void finishLoop() {
    @autoreleasepool {
      while (true) {
        std::unique_lock lock(commandQueue_->mutex_);
        if (!commandQueue_->queueLifecycle_.runFinishIteration(lock, [this](u64) {
              allocators_.reclaim(commandQueue_->completedSeqId_);
              tryGarbageCollectUnlocked();
            })) {
          return;
        }
      }
    }
  }

  void waitForSequenceUnlocked(u64 seqId, std::unique_lock<std::mutex>& lock) {
    commandQueue_->queueLifecycle_.waitForSequence(lock, seqId);
  }

  void tryGarbageCollectUnlocked() {
    pool_.reclaimCompleted(commandQueue_->completedSeqId_);
  }

  void purgeResourcesUnlocked() { pool_.purgeAll(); }

  BackendLimits limits_{};
  WMT::Reference<WMT::Device> wrappedDevice_{};
  dxmt9::CommandQueue* commandQueue_ = nullptr;
  dxmt9::Device* upperDevice_ = nullptr;  // observers + maxFrameLatency live here (task 3)
  // Threads moved to CommandQueue (C7c). Loop bodies supplied via
  // commandQueue_->startThreads() in the ctor; joined via stopThreads() in
  // the dtor before any other teardown.
  // mutex_, condition variables, stop_ all moved to dxmt9::CommandQueue (C7b).
  // Accessed via commandQueue_->. Threads remain here because their bodies
  // call into this class's methods (encodeChunk, tryGarbageCollectUnlocked,
  // etc.) and must be joined before *this tears down.
  // Chunk-ring state + seqId counters + queueLifecycle + diagnostics moved
  // to dxmt9::CommandQueue (C1, C7a). Accessed via commandQueue_->.
  // pool_.nextHandle, resource maps, and pool_.dumpedGpuTextures grouped into pool_ (C6).
  Handle currentBackBuffer_{};
  bool backBufferDiscardAfterPresent_ = false;
  // deviceLostObserver_/presentationStatusObserver_/maxFrameLatency_ moved
  // to dxmt9::Device (task 3). Accessed via upperDevice_->notify*/... or
  // upperDevice_->maxFrameLatency().
  ResourcePool pool_{};
  PipelineCache pipelineCache_{};
  // presenters_ map removed — Presenter ownership lives on core::SwapChain.
  FrameAllocators allocators_{};
  // shaderArchive_/shaderArchivePath_ are owned by DeviceImpl (C2). We keep
  // pointers to those fields; lifetime-safe because DeviceImpl destroys the
  // backend (which joins threads) before the archive is released.
  WMT::Reference<WMT::BinaryArchive>* shaderArchive_ = nullptr;
  const std::string* shaderArchivePath_ = nullptr;
  bool ready_ = false;
};

}  // namespace

std::string makeShaderSourceFromRequest(const WinemetalShaderCompileRequest& request) {
  return makeShaderSourceFromRequestInternal(request);
}

// buildPresentPipeline moved to dxmt9_pipeline_cache.{hpp,cpp}
// (dxmt9::pipeline:: namespace). Presenter calls it directly.

std::shared_ptr<BackendDevice> makeMetalBackendDevice(const BackendLimits& limits,
                                                      WMT::Reference<WMT::Device> wmtDevice,
                                                      dxmt9::CommandQueue& commandQueue,
                                                      WMT::Reference<WMT::BinaryArchive>& shaderArchive,
                                                      const std::string& shaderArchivePath,
                                                      dxmt9::Device& upperDevice) {
  auto backend = std::make_shared<MetalBackendDevice>(limits, std::move(wmtDevice), commandQueue,
                                                       shaderArchive, shaderArchivePath, upperDevice);
  return backend->ready() ? std::static_pointer_cast<BackendDevice>(std::move(backend))
                          : std::shared_ptr<BackendDevice>{};
}

}  // namespace dxmt9::core
