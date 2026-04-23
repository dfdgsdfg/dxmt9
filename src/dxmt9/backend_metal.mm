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
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_draw_shader.hpp"
#include "dxmt9_draw_state.hpp"
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

// Env-gated debug knobs moved to dxmt9::debug (Step 3d). Thin aliases
// keep the file-local call sites unchanged.
inline bool debugForceVisibleDraw() { return dxmt9::debug::forceVisibleDraw(); }
inline bool debugSkipAllDraws() { return dxmt9::debug::skipAllDraws(); }
inline bool debugDisableScissor() { return dxmt9::debug::disableScissor(); }
inline bool debugDisableAlphaTest() { return dxmt9::debug::disableAlphaTest(); }
inline bool debugForceExpandIndexed() { return dxmt9::debug::forceExpandIndexed(); }
inline int fixedFunctionTraceBudget() { return dxmt9::debug::fixedFunctionTraceBudget(); }
inline u64 fixedFunctionTraceTextureHandle() { return dxmt9::debug::fixedFunctionTraceTextureHandle(); }
inline u64 textureTraceHandle() { return dxmt9::debug::traceTextureHandle(); }

// shaderDumpDir + maybeDumpShaderSource moved to dxmt9_draw_shader.cpp
// (file-private there). Only the makeDrawShaderSource dispatch calls them.

bool shouldDumpGpuTexture(Handle handle) {
  const u64 wanted = gpuDumpTextureHandle();
  return wanted != 0ull && handle.value == wanted;
}

inline u64 traceEncodeSeq() { return dxmt9::debug::traceEncodeSeq(); }
inline bool shouldTraceTexture(Handle handle) { return dxmt9::debug::shouldTraceTexture(handle); }
inline u64 skippedTextureHandle() { return dxmt9::debug::skippedTextureHandle(); }
inline bool shouldTraceEncode(const DrawDesc& draw, u64 seqId) {
  return dxmt9::debug::shouldTraceEncode(draw, seqId);
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

// DrawUniforms + buildDrawUniforms + makeDepthStencilKey moved to
// dxmt9_draw_state.{hpp,cpp} (Step 3d-3). Aliased here so the rest of
// this TU keeps the short names.
using DrawUniforms = dxmt9::state::DrawUniforms;
using dxmt9::state::buildDrawUniforms;
using dxmt9::state::makeDepthStencilKey;

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

// buildDrawUniforms moved to dxmt9::state (Step 3d-3).

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

using dxmt9::ffp::hashVertexDeclaration;

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


// makeDrawShaderSource moved to dxmt9_draw_shader.{hpp,cpp}
using dxmt9::drawshader::makeDrawShaderSource;

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


using dxmt9::pipeline::makeShaderVariantKey;

// makeDepthStencilKey moved to dxmt9::state (Step 3d-3).

class MetalBackendDevice final : public BackendDevice {
 public:
  MetalBackendDevice(const BackendLimits& limits, WMT::Reference<WMT::Device> wmtDevice,
                     dxmt9::CommandQueue& commandQueue,
                     WMT::Reference<WMT::BinaryArchive>& shaderArchive,
                     const std::string& shaderArchivePath,
                     dxmt9::Device& upperDevice,
                     dxmt9::resources::Pool& pool,
                     dxmt9::pipeline::Cache& pipelineCache,
                     dxmt9::scratch::FrameAllocators& allocators)
      : limits_(limits), commandQueue_(&commandQueue), upperDevice_(&upperDevice),
        shaderArchive_(&shaderArchive), shaderArchivePath_(&shaderArchivePath),
        pool_(&pool), pipelineCache_(&pipelineCache), allocators_(&allocators) {
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

    // encodeLoop stays on backend (owns encodeChunk/encodeDraw). finish +
    // completion loops moved to CommandQueue (Step 3c).
    commandQueue_->startThreads(
        [this] { encodeLoop(); },
        [q = commandQueue_, p = pool_, a = allocators_] { q->runFinishLoop(*p, *a); },
        [q = commandQueue_] { q->runCompletionWatcherLoop(); });
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

  // completionWatcherLoop moved to CommandQueue::runCompletionWatcherLoop
  // (Step 3c).

  // Observer setters + setMaxFrameLatency: no overrides here. Factory wires
  // observers directly on the upper dxmt9::Device in createDevice (task 3).
  // BackendDevice's default no-op bodies are correct for this class since
  // production never calls backend->setDeviceLostObserver anymore — the
  // invocation path goes through upperDevice_->notifyPresentationStatus
  // from encodePresent, with storage on DeviceImpl.

  // waitForVBlank lives on CommandQueue (Step 3b).

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

  // Resource CRUD overrides removed (Step 3a). DeviceImpl implements these
  // directly against pool_ after Step 2a. Production never routes back
  // through MetalBackendDevice for resource lifecycle; tests use
  // MockBackendDevice which carries its own overrides.

  void* mapBuffer(BufferHandle handle, u32 flags) override {
    std::unique_lock lock(commandQueue_->mutex_);
    auto it = pool_->buffers.find(handle.value);
    if (it == pool_->buffers.end()) {
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

  // unmapBuffer + uploadBufferData overrides removed (Step 3e).
  // DeviceImpl implements these directly against pool_; production never
  // routes through MetalBackendDevice's BackendDevice overrides.

  void uploadTextureLevel(TextureHandle handle, u32 level, u32 width, u32 height, u32 pitch,
                          std::span<const u8> bytes) override {
    std::lock_guard lock(commandQueue_->mutex_);
    if (shouldTraceTexture(handle)) {
      emitUploadTrace(handle, level, width, height, pitch, bytes);
    }
    pool_->uploadTextureLevel(wrappedDevice_, commandQueue_->raw(), handle, level,
                              width, height, pitch, bytes.data(), bytes.size());
    if (level == 0 && shouldDumpGpuTexture(handle)) {
      if (auto* rec = pool_->findTexture(handle.value); rec && rec->texture) {
        dumpTextureSnapshotUnlocked(handle, rec->desc, rec->texture.handle);
      }
    }
  }

  // Debug-only upload trace (kept on backend because it's wired to file-local
  // shouldTraceTexture + emitTextureTraceLine sinks).
  void emitUploadTrace(TextureHandle handle, u32 level, u32 width, u32 height,
                        u32 pitch, std::span<const u8> bytes) {
    u32 minAlpha = 255u;
    u32 maxAlpha = 0u;
    u64 nonZeroAlpha = 0u;
    u64 nonZeroRgb = 0u;
    auto* rec = pool_->findTexture(handle.value);
    if (rec && (rec->desc.format == Format::A8R8G8B8 || rec->desc.format == Format::A8B8G8R8 ||
                 rec->desc.format == Format::X8R8G8B8 || rec->desc.format == Format::X8B8G8R8) &&
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

  // Step 3b: submit{Draw,Clear,SurfaceCopy,StretchRect,Readback,ColorFill},
  // present, and flush moved to dxmt9::CommandQueue. DeviceImpl dispatches
  // directly through queue_. Backend no longer implements these overrides —
  // the defaulted no-op on BackendDevice is correct because production
  // never invokes them via the backend pointer anymore.

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

  BufferRecord* findBufferUnlocked(u64 handle) { return pool_->findBuffer(handle); }
  const BufferRecord* findBufferUnlocked(u64 handle) const { return pool_->findBuffer(handle); }
  TextureRecord* findTextureUnlocked(u64 handle) { return pool_->findTexture(handle); }
  const TextureRecord* findTextureUnlocked(u64 handle) const { return pool_->findTexture(handle); }
  SurfaceRecord* findSurfaceUnlocked(u64 handle) { return pool_->findSurface(handle); }
  const SurfaceRecord* findSurfaceUnlocked(u64 handle) const { return pool_->findSurface(handle); }

  // currentSlot / ensureWritingSlotUnlocked / updateLastUsedSeqIdsUnlocked /
  // markXResourcesUnlocked / seqIdForMark / makeXCommand moved to
  // dxmt9::CommandQueue (Step 3b). Backend retains only encodeChunk paths.

  void encodeLoop() {
    @autoreleasepool {
      while (true) {
        std::unique_lock lock(commandQueue_->mutex_);
        if (!commandQueue_->queueLifecycle_.runEncodeIteration(
                lock,
                [this](size_t slotIndex, const ChunkSlot& slot) {
                  dxmt9::encoders::EncodeContext ctx{
                      wrappedDevice_, limits_, *pool_, *pipelineCache_, *allocators_,
                      shaderArchive_, shaderArchivePath_, *commandQueue_, upperDevice_,
                  };
                  return dxmt9::encoders::encodeChunk(ctx, slotIndex, slot);
                },
                [this](u64) {
                  allocators_->reclaim(commandQueue_->completedSeqId_);
                })) {
          return;
        }
      }
    }
  }

  void dumpTextureSnapshotUnlocked(Handle handle, const TextureDesc& desc,
                                   obj_handle_t sourceTextureHandle) {
    if (!sourceTextureHandle || !shouldDumpGpuTexture(handle) ||
        pool_->dumpedGpuTextures.contains(handle.value)) {
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
      pool_->dumpedGpuTextures.insert(handle.value);
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
    pool_->dumpedGpuTextures.insert(handle.value);
  }

  // makeSampler overloads moved to dxmt9::encoders (Step 3d). encodeDraw
  // calls dxmt9::encoders::makeSampler(wrappedDevice_, ...) directly.

  // pipelineForDraw + depthStencilStateFor moved to pipeline::Cache
  // (Step 3d). encodeDraw now calls pipelineCache_->getOrBuildDrawPipelineForDraw
  // and pipelineCache_->depthStencilStateFor directly.

  // pipelineForPresent moved to dxmt9::Presenter (C4); each Presenter caches
  // its own pipeline per (opaqueAlpha) variant using buildPresentPipeline.

  // finishLoop moved to CommandQueue::runFinishLoop (Step 3c).

  void waitForSequenceUnlocked(u64 seqId, std::unique_lock<std::mutex>& lock) {
    commandQueue_->queueLifecycle_.waitForSequence(lock, seqId);
  }

  void tryGarbageCollectUnlocked() {
    pool_->reclaimCompleted(commandQueue_->completedSeqId_);
  }

  void purgeResourcesUnlocked() { pool_->purgeAll(); }

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
  // pool_->nextHandle, resource maps, and pool_->dumpedGpuTextures grouped into pool_ (C6).
  // currentBackBuffer_ + backBufferDiscardAfterPresent_ moved to
  // CommandQueue (Steps 3b/3d). Accessed via commandQueue_->.
  // deviceLostObserver_/presentationStatusObserver_/maxFrameLatency_ moved
  // to dxmt9::Device (task 3). Accessed via upperDevice_->notify*/... or
  // upperDevice_->maxFrameLatency().
  // Pool/Cache/Allocators owned by DeviceImpl (Step 1). Backend holds
  // pointers and forwards operations. Lifetime-safe: DeviceImpl destroys
  // the backend (which joins threads) before these objects destruct.
  dxmt9::resources::Pool* pool_ = nullptr;
  dxmt9::pipeline::Cache* pipelineCache_ = nullptr;
  dxmt9::scratch::FrameAllocators* allocators_ = nullptr;
  // presenters_ map removed — Presenter ownership lives on core::SwapChain.
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
                                                      dxmt9::Device& upperDevice,
                                                      dxmt9::resources::Pool& pool,
                                                      dxmt9::pipeline::Cache& pipelineCache,
                                                      dxmt9::scratch::FrameAllocators& allocators) {
  auto backend = std::make_shared<MetalBackendDevice>(
      limits, std::move(wmtDevice), commandQueue, shaderArchive, shaderArchivePath, upperDevice,
      pool, pipelineCache, allocators);
  return backend->ready() ? std::static_pointer_cast<BackendDevice>(std::move(backend))
                          : std::shared_ptr<BackendDevice>{};
}

}  // namespace dxmt9::core
