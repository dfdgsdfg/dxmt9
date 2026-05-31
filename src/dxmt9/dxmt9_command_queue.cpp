#include "dxmt9_command_queue.hpp"
#include "dxmt9/assert.hpp"
#include "dxmt9_archive_prewarm.hpp"
#include "dxmt9_debug_alloc_guard.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_device.hpp"
#include "dxmt9_blit_encoders.hpp"
#include "dxmt9_compat.hpp"
#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_presenter.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9_resource_initializer.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_ring_arena.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace dxmt9 {

namespace {
// Tiny helper that uploads a printf-formatted label to the bridge as an
// autoreleased NSString, then returns a non-owning WMT::String view. The
// NSString lives on the autoreleasepool of the encoding thread — long
// enough for the setLabel: selector to retain it, but never longer.
template <std::size_t Cap = 96>
WMT::String makeLabelStringFmt(const char* fmt, ...) {
  char buf[Cap];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return WMT::String::string(buf, WMTUTF8StringEncoding);
}

// Monotonic counter family for resources where the call site lacks a
// stable id (e.g. transient slabs that don't have a Pool handle).
std::atomic_uint64_t gTransientLabelCounter{0};
std::atomic_uint64_t gCommandBufferLabelCounter{0};

// R-BACK-3.7 / R-BACK-3.8 / R-BACK-4.8 — archive path resolution moved
// into archive_prewarm. The path now embeds the dxmt9 archive ABI
// version and the sanitized GPU family token so cross-process readers
// never observe an archive that was serialized against an incompatible
// emitter / variant-key encoding (design §6.1).
std::string resolveShaderCachePath(WMT::Device device) {
  const auto mode = archive_prewarm::resolveMode();
  return archive_prewarm::resolveArchivePath(device, mode);
}

constexpr std::size_t kTransientBufferInitialCapacity = 8ull << 20;

bool forceDedicatedTransientUploads() {
  static const bool value = [] {
    const char* env = std::getenv("DXMT_DEBUG_FORCE_TRANSIENT_DEDICATED");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return value;
}

bool surfaceAliasesTracedTexture(resources::Pool& pool, core::Handle handle) {
  const auto wanted = debug::traceTextureHandle();
  if (wanted == 0 || !handle) {
    return false;
  }
  const auto* surface = pool.findSurface(handle.value);
  return surface && surface->aliasTexture.value == wanted;
}

void traceTextureSurfaceOp(resources::Pool& pool, const char* op,
                           core::Handle a = {}, core::Handle b = {},
                           core::Handle c = {}) {
  if (!op ||
      (!surfaceAliasesTracedTexture(pool, a) &&
       !surfaceAliasesTracedTexture(pool, b) &&
       !surfaceAliasesTracedTexture(pool, c))) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-texture] surface-op " << op;
  if (a) out << " a=0x" << std::hex << a.value << std::dec;
  if (b) out << " b=0x" << std::hex << b.value << std::dec;
  if (c) out << " c=0x" << std::hex << c.value << std::dec;
  core::metalqueue::emitTextureTraceLine(out.str());
}

void traceTextureClear(resources::Pool& pool, const core::ClearDesc& desc) {
  bool tracesTexture = false;
  for (const auto& attachment : desc.colorAttachments) {
    tracesTexture = tracesTexture || surfaceAliasesTracedTexture(pool, attachment.handle);
  }
  tracesTexture = tracesTexture || surfaceAliasesTracedTexture(pool, desc.depthStencil.handle);
  if (!tracesTexture) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-texture] clear flags color=" << (desc.clearColor ? 1 : 0)
      << " depth=" << (desc.clearDepth ? 1 : 0)
      << " stencil=" << (desc.clearStencil ? 1 : 0)
      << " rgba=(" << desc.color.r << "," << desc.color.g << ","
      << desc.color.b << "," << desc.color.a << ")"
      << " depthValue=" << desc.depth
      << " stencilValue=" << desc.stencil
      << " rects=" << desc.rects.size();
  core::metalqueue::emitTextureTraceLine(out.str());
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  if (alignment <= 1) {
    return value;
  }
  return (value + alignment - 1) & ~(alignment - 1);
}

std::size_t nextPowerOfTwo(std::size_t value) {
  if (value <= 1) {
    return 1;
  }
  --value;
  for (std::size_t shift = 1; shift < sizeof(std::size_t) * 8; shift <<= 1) {
    value |= value >> shift;
  }
  return value + 1;
}

class PerfScope {
 public:
  explicit PerfScope(void (*record)(std::uint64_t)) : record_(record) {}
  ~PerfScope() {
    if (!record_) {
      return;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started_;
    record_(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  }

  PerfScope(const PerfScope&) = delete;
  PerfScope& operator=(const PerfScope&) = delete;

 private:
  void (*record_)(std::uint64_t) = nullptr;
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

}  // namespace

CommandQueue::CommandQueue(WMT::Device device, core::BackendLimits limits)
    : device_(device),
      limits_(limits),
      shaderArchive_(device, resolveShaderCachePath(device)) {
  if (!device_) {
    return;
  }
  // R-BACK-3.7 / R-BACK-3.8 — drive the prewarm step now that the
  // archive instance is constructed. Runs the §6.1 failure-mode table
  // and bumps the relevant perf counters (missing / lock_busy /
  // entries / bytes / load_ns). Non-fatal under all conditions; never
  // blocks queue init beyond a single bounded flock retry budget.
  {
    const auto prewarmMode = archive_prewarm::resolveMode();
    archive_prewarm::run(device, shaderArchive_.path(), prewarmMode);
  }
  queue_ = device_.newCommandQueue(0);
  if (!queue_) {
    return;
  }
  queueView_ = WMT::CommandQueue{queue_.handle};
  // M1: name the queue so frame captures don't show a generic
  // <CAMetalQueue: 0x...>. deviceId is the underlying MTLDevice handle —
  // good enough to disambiguate multi-GPU configs in Xcode captures.
  queueView_.setLabel(makeLabelStringFmt("dxmt9-q-0x%llx",
      static_cast<unsigned long long>(device_.handle)));

  // R-BACK-5.7: probe `MTLDevice.hasUnifiedMemory` ONCE at queue/device
  // init and cache the result on the resource pool. Per-resource code
  // (createTexture, stageTextureUpload, …) reads `pool_.hasUnifiedMemory_`
  // — never re-queries Metal — so the storage-mode and staging-blit
  // decisions stay one-shot per resource and consistent for that
  // resource's lifetime.
  pool_.setHasUnifiedMemory(device_.hasUnifiedMemory());
  // R-BACK-13.* — cache Apple3 GPU family support so the tile-shader FFP
  // selector can gate on a single bool. WMTGPUFamilyApple3 is the floor
  // for `MTLTileRenderPipelineDescriptor` and programmable blending.
  pool_.setSupportsApple3(device_.supportsFamily(WMTGPUFamilyApple3));
  // R-BACK-12.22 — Stage 2 argument-buffer hybrid capability gate. Enable
  // only when the device supports Tier-2 argbufs AND lives on Apple3+.
  // Probed once and cached on the pool so per-encoder selection is a
  // single bool read with no Metal traffic.
  // DXMT9_DISABLE_ARGBUF_HYBRID escape hatch: forces the gate off so a
  // suspected Stage 2 regression can be tested in isolation against the
  // Stage 1 baseline without rebuilding.
  {
    const auto tier = device_.argumentBuffersSupport();
    const bool tierOk = tier >= WMTArgumentBuffersTier2;
    const char* disableEnv = std::getenv("DXMT9_DISABLE_ARGBUF_HYBRID");
    const bool disabled =
        disableEnv && disableEnv[0] != '\0' && std::strcmp(disableEnv, "0") != 0;
    pool_.setArgbufHybridEnabled(!disabled && tierOk && pool_.supportsApple3());
  }
  // R-BACK-12.22 / 12.24 — build the queue-owned MTLArgumentEncoder when
  // the capability gate held. The encoder is shared across every render
  // pass on this queue; per-pass `openArgbuf` retargets it onto the
  // freshly reserved transient storage. When the gate failed, the
  // resource stays uninitialized and `openArgbuf` returns an empty
  // handle — the Stage 1 binding path then runs unchanged. Skip on a
  // sentinel-null device (test/fake-backend fixtures); ArgbufEncoderResource::init
  // tolerates a null handle but the gate above already short-circuits.
  if (pool_.argbufHybridEnabled() && device_) {
    argbufEncoderResource_.init(device_);
  }
  // R-BACK-12.22..12.26 (resource-array sub-mode) — opt-in second lane.
  // Build the 20-entry resource-array encoder ONLY when the constants-only
  // Stage 2 gate held AND the DXMT9_ARGBUF_RESOURCE_ARRAY env flag is set.
  // Default off: resourceArrayLaneActive_ stays false, the second encoder
  // stays uninitialized, and every Stage 2 pass uses the byte-identical
  // constants-only encoder. When on, the encoder thread selects this
  // encoder + the resource-array PSO bit per pass.
  resourceArrayLaneActive_ =
      pool_.argbufHybridEnabled() && shaders::argbufResourceArrayEnabled();
  if (resourceArrayLaneActive_ && device_) {
    resourceArrayEncoderResource_.initResourceArray(device_);
    // If the extended encoder failed to build (driver rejected the 20-entry
    // table), fall back to the constants-only lane rather than half-enable.
    if (!resourceArrayEncoderResource_.initialized()) {
      resourceArrayLaneActive_ = false;
    }
  }
  // R-BACK-14.* — bind the small-resource heap manager to the same
  // WMT::Device + unified-memory probe used by the pool's storage-mode
  // selectors. Init must run before initializer_ / encode loops because
  // the very first createTexture / createBuffer can reach the heap path.
  pool_.heapManager().init(device_, pool_.hasUnifiedMemory_);

  initializer_ = std::make_unique<resources::Initializer>(*this, pool_, device_);

  // Bind queueLifecycle_ to our own state + a pool-based surface-compat
  // hook. CommandQueue is its own lifecycle root.
  bindSelfLifecycle([this](core::Handle h) -> std::uint32_t {
    if (!h) return 0;
    auto* surface = pool_.findSurface(h.value);
    if (!surface) return 0;
    return core::metalcompat::isFloatRenderTargetFormat(surface->desc.format)
        ? static_cast<std::uint32_t>(core::metalcompat::CompatFlagBits::CompatFlagFp16)
        : 0u;
  });

  // Spawn the three worker threads. Threads block on writeCv_ until the
  // first submit; no race with DeviceImpl's still-completing ctor because
  // submits can only happen after CreateDXMT9Device returns.
  startThreads(
      [this] {
        runEncodeLoop(
            [this](std::size_t slotIndex, const core::ChunkSlot& slot) {
              auto ctx = makeEncodeContext();
              return encoders::encodeChunk(ctx, slotIndex, slot);
            },
            [this](std::uint64_t) { allocators_.reclaim(completedSeqId_); });
      },
      [this] { runFinishLoop(); },
      [this] { runCompletionWatcherLoop(); });
}

encoders::EncodeContext CommandQueue::makeEncodeContext() {
  // Snapshot + reset the chunk-import dirty accumulator. The dirty
  // bits and high-water counters move to the freshly-built
  // EncodeContext; the queue's pendingDirty_ resets to all-clean so the
  // next chunk's records start fresh. C2 still calls markAllDirty(...)
  // at encoder init per R-BACK-12.12 to fold in the implicit
  // "everything could have changed since last encode" semantic.
  // Note: device_ may be a sentinel-null handle for fake-backend test
  // fixtures (see tests/native/backend/resource_hazard_spec.cpp). The
  // real Metal-side dereference happens deep inside encoders::encodeChunk
  // / encoders::encodeDraw; assertion belongs there, not at context
  // creation.
  return encoders::EncodeContext{
      device_, limits_, pool_, pipelineCache_, allocators_,
      &shaderArchive_.reference(), &shaderArchive_.path(),
      *this,
      consumePendingDirty(),
  };
}

void CommandQueue::prefetchSlotPipelines(const core::ChunkSlot& slot) {
  if (!device_) {
    return;
  }
  for (std::size_t i = 0; i < slot.commandCount(); ++i) {
    const auto command = slot.commandAt(i);
    if (command.kind != core::MetalCommandKind::DrawRun ||
        !command.drawState.hot ||
        !command.drawState.hasShaderContext()) {
      continue;
    }

    auto drawState = command.drawState;
    drawState.uniforms = command.drawUniformPayload;
    const auto tileSelection =
        pipeline::selectTileFfpForPass(drawState, pool_.supportsApple3());
    const bool tileFfpMode =
        tileSelection.decision == pipeline::TileFfpDecision::Tile;
    if (tileFfpMode) {
      (void)pipelineCache_.getOrBuildTileFfpBaseColorPipelineForState(
          device_, limits_, pool_, drawState, &shaderArchive_.reference(),
          &shaderArchive_.path());
      (void)pipelineCache_.getOrBuildDrawPipelineForState(
          device_, limits_, pool_, drawState, &shaderArchive_.reference(),
          &shaderArchive_.path(), /*tileFfpMode=*/true,
          /*argbufHybridMode=*/false, /*argbufResourceArray=*/false);
      continue;
    }

    const bool argbufHybridMode =
        pipeline::selectArgbufHybridForPass(drawState, pool_.argbufHybridEnabled()) ==
        pipeline::ArgbufHybridDecision::Stage2;
    const bool argbufResourceArray =
        argbufHybridMode && resourceArrayLaneActive_ &&
        resourceArrayEncoderResource_.initialized();
    (void)pipelineCache_.getOrBuildDrawPipelineForState(
        device_, limits_, pool_, drawState, &shaderArchive_.reference(),
        &shaderArchive_.path(), /*tileFfpMode=*/false, argbufHybridMode,
        argbufResourceArray);
  }
}

uniform::DirtyState CommandQueue::consumePendingDirty() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::DirtyState snapshot = pendingDirty_;
  pendingDirty_ = uniform::DirtyState{};
  return snapshot;
}

void CommandQueue::applyDirtyConstantSetVsF(std::uint32_t startReg, std::uint32_t count) {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyConstantSetVsF(pendingDirty_, startReg, count);
}

void CommandQueue::applyDirtyConstantSetVsI(std::uint32_t startReg, std::uint32_t count) {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyConstantSetVsI(pendingDirty_, startReg, count);
}

void CommandQueue::applyDirtyConstantSetVsB(std::uint32_t startReg, std::uint32_t count) {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyConstantSetVsB(pendingDirty_, startReg, count);
}

void CommandQueue::applyDirtyConstantSetPsF(std::uint32_t startReg, std::uint32_t count) {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyConstantSetPsF(pendingDirty_, startReg, count);
}

void CommandQueue::applyDirtyConstantSetPsI(std::uint32_t startReg, std::uint32_t count) {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyConstantSetPsI(pendingDirty_, startReg, count);
}

void CommandQueue::applyDirtyConstantSetPsB(std::uint32_t startReg, std::uint32_t count) {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyConstantSetPsB(pendingDirty_, startReg, count);
}

void CommandQueue::applyDirtyTransformChange() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyTransformChange(pendingDirty_);
}

void CommandQueue::applyDirtyClipPlaneChange() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyClipPlaneChange(pendingDirty_);
}

void CommandQueue::applyDirtyViewportChange() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyViewportChange(pendingDirty_);
}

void CommandQueue::applyDirtyRenderStateFog() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyRenderStateFog(pendingDirty_);
}

void CommandQueue::applyDirtyRenderStateAlpha() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyRenderStateAlpha(pendingDirty_);
}

void CommandQueue::applyDirtyRenderStateTexFactor() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyRenderStateTexFactor(pendingDirty_);
}

void CommandQueue::applyDirtyTextureStageConstant() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyTextureStageConstant(pendingDirty_);
}

// R-BACK-15.4 / 15.5 / 15.6: touched color attachment set. Single-thread
// access (encoder thread); no mutex. Null/zero handles are no-ops on
// every entry-point so callers don't have to guard.
bool CommandQueue::isColorHandleTouched(core::Handle handle) const {
  if (!handle) return false;
  return touchedColorHandles_.find(handle.value) != touchedColorHandles_.end();
}

void CommandQueue::markColorHandleTouched(core::Handle handle) {
  if (!handle) return;
  touchedColorHandles_.insert(handle.value);
}

void CommandQueue::invalidateColorHandle(core::Handle handle) {
  if (!handle) return;
  touchedColorHandles_.erase(handle.value);
}

void CommandQueue::clearAllTouchedColorHandles() {
  touchedColorHandles_.clear();
}

void CommandQueue::markPendingDirtyAll() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::markAllDirty(pendingDirty_);
}

CommandQueue::~CommandQueue() {
  if (threadsStarted_) {
    stopThreads();
  }
  // Archive persist lives on dxmt9::shaders::Archive's dtor — not here.
}

void CommandQueue::uploadTextureLevel(core::TextureHandle handle,
                                        std::uint32_t level,
                                        std::uint32_t width,
                                        std::uint32_t height,
                                        std::uint32_t depth,
                                        std::uint32_t pitch,
                                        std::uint32_t slicePitch,
                                        std::span<const std::uint8_t> bytes) {
  if (initializer_) {
    initializer_->uploadTextureLevel(handle, level, width, height, depth, pitch,
                                     slicePitch, bytes);
  }
}

void CommandQueue::initializeTextureZero(core::TextureHandle handle) {
  if (initializer_) {
    initializer_->initializeTextureZero(handle);
  }
}

core::HResult CommandQueue::generateTextureMipSublevels(core::TextureHandle handle) {
  if (!handle || !queue_) {
    return core::D3DERR_INVALIDCALL;
  }

  const auto flushResult = flushInitializerUploads();
  if (flushResult.event && flushResult.value > 0) {
    WMT::SharedEvent{flushResult.event.handle}.waitUntilSignaledValue(
        flushResult.value, /*timeout-ms*/ 1000);
  }

  WMT::Reference<WMT::Texture> texture;
  WMT::Heap heap{};
  bool isHeapBacked = false;
  {
    std::lock_guard lock(mutex_);
    auto* record = pool_.findTexture(handle.value);
    if (!record || !record->texture || record->desc.levels <= 1) {
      return record ? core::D3D_OK : core::D3DERR_INVALIDCALL;
    }
    texture = record->texture;
    heap = record->heap;
    isHeapBacked = record->isHeapBacked;
  }

  auto commandBuffer = newCommandBuffer();
  if (!commandBuffer) {
    return core::D3DERR_INVALIDCALL;
  }
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) {
    return core::D3DERR_INVALIDCALL;
  }
  if (isHeapBacked && heap.handle != 0) {
    blit.useHeap(heap);
    perf::countUseHeap();
  }
  blit.generateMipmaps(WMT::Texture{texture.handle});
  blit.endEncoding();
  commandBuffer.commit();
  const auto started = std::chrono::steady_clock::now();
  commandBuffer.waitUntilCompleted();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  perf::countSyncWait(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  return core::D3D_OK;
}

CommandQueue::InitializerFlush CommandQueue::flushInitializerUploads() {
  if (!initializer_) {
    return {};
  }
  auto result = initializer_->flushToWait();
  return {result.event, result.value};
}

WMT::Reference<WMT::CommandBuffer> CommandQueue::newCommandBuffer() {
  if (!queue_) {
    return {};
  }
  PerfScope scope(perf::countCommandBufferCreateCpuTime);
  auto commandBuffer = queue_.commandBuffer();
  if (commandBuffer) {
    perf::countCommandBuffer();
    // M1: monotonic counter — newCommandBuffer is called from multiple
    // sites (chunk encode, transfers, present split). Mixing a sequence
    // ID would be more meaningful but it isn't available at this site.
    const auto id = gCommandBufferLabelCounter.fetch_add(1, std::memory_order_relaxed);
    WMT::CommandBuffer view{commandBuffer.handle};
    view.setLabel(makeLabelStringFmt("cb_seq_%llu",
        static_cast<unsigned long long>(id)));
  }
  return commandBuffer;
}

void CommandQueue::reclaimTransientBuffersUnlocked(std::uint64_t completedSeqId) {
  while (!transientBufferAllocations_.empty() &&
         transientBufferAllocations_.front().seqId <= completedSeqId) {
    transientBufferAllocations_.pop_front();
  }
  if (transientBufferAllocations_.empty()) {
    transientBufferCursor_ = 0;
  }

  while (!retainedTransientBuffers_.empty() &&
         retainedTransientBuffers_.front().seqId <= completedSeqId) {
    retainedTransientBuffers_.pop_front();
  }

  while (!retainedSamplerStates_.empty() &&
         retainedSamplerStates_.front().seqId <= completedSeqId) {
    retainedSamplerStates_.pop_front();
  }
}

bool CommandQueue::ensureTransientBufferUnlocked(std::size_t minimumCapacity) {
  if (transientBuffer_) {
    return transientBufferCapacity_ >= minimumCapacity && transientBufferContents_ != nullptr;
  }

  const std::size_t capacity =
      nextPowerOfTwo(std::max(kTransientBufferInitialCapacity, minimumCapacity));
  WMTBufferInfo info{};
  info.length = capacity;
  info.options = WMTResourceStorageModeShared;
  auto buffer = device_.newBuffer(info);
  if (!buffer || !info.memory.ptr) {
    return false;
  }

  perf::countMetalBuffer(capacity);
  // M1: label the freshly-allocated transient slab. Each ensure-call
  // gets a fresh counter id so back-to-back rotations are
  // disambiguated in captures.
  {
    const auto id = gTransientLabelCounter.fetch_add(1, std::memory_order_relaxed);
    WMT::Buffer view{buffer.handle};
    view.setLabel(makeLabelStringFmt("dxmt9-transient-slab-%llu-cap%zu",
        static_cast<unsigned long long>(id), capacity));
  }
  transientBuffer_ = std::move(buffer);
  transientBufferContents_ = static_cast<std::byte*>(info.memory.ptr);
  transientBufferCapacity_ = capacity;
  transientBufferCursor_ = 0;
  return true;
}

bool CommandQueue::rotateTransientBufferUnlocked(std::size_t minimumCapacity, std::uint64_t seqId) {
  if (transientBuffer_) {
    if (!transientBufferAllocations_.empty()) {
      retainedTransientBuffers_.push_back(RetainedTransientBuffer{
          .buffer = std::move(transientBuffer_),
          .seqId = seqId,
      });
    } else {
      transientBuffer_ = nullptr;
    }
  }
  transientBufferContents_ = nullptr;
  transientBufferCapacity_ = 0;
  transientBufferCursor_ = 0;
  transientBufferAllocations_.clear();
  return ensureTransientBufferUnlocked(minimumCapacity);
}

CommandQueue::TransientBufferSlice CommandQueue::uploadTransientBuffer(
    std::span<const std::byte> bytes,
    std::size_t alignment,
    std::uint64_t seqId) {
  if (!device_ || bytes.empty()) {
    return {};
  }
  const auto uploadStarted = std::chrono::steady_clock::now();
  const auto recordUploadTime = [&] {
    const auto elapsed = std::chrono::steady_clock::now() - uploadStarted;
    perf::countTransientUploadCpuTime(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
        bytes.size());
  };

  std::uint64_t completedSeqId = 0;
  {
    std::lock_guard lock(mutex_);
    completedSeqId = completedSeqId_;
  }

  std::lock_guard lock(transientBufferMutex_);
  reclaimTransientBuffersUnlocked(completedSeqId);

  auto uploadDedicated = [&]() -> TransientBufferSlice {
    WMTBufferInfo info{};
    info.length = bytes.size();
    info.options = WMTResourceStorageModeShared;
    info.memory.set((void*)bytes.data());
    auto buffer = device_.newBuffer(info);
    if (!buffer) {
      recordUploadTime();
      return {};
    }
    perf::countMetalBuffer(static_cast<std::size_t>(info.length));
    // M1: dedicated transient slabs (oversized payloads bypass the
    // shared slab) — label with seq + size so captures distinguish
    // them from ring-allocated transients.
    {
      WMT::Buffer view{buffer.handle};
      view.setLabel(makeLabelStringFmt("dxmt9-transient-dedicated-seq%llu-bytes%zu",
          static_cast<unsigned long long>(seqId), bytes.size()));
    }
    retainedTransientBuffers_.push_back(RetainedTransientBuffer{
        .buffer = std::move(buffer),
        .seqId = seqId,
    });
    auto slice = TransientBufferSlice{
        .buffer = WMT::Buffer{retainedTransientBuffers_.back().buffer.handle},
        .offset = 0,
        .size = bytes.size(),
    };
    recordUploadTime();
    return slice;
  };

  alignment = std::max<std::size_t>(alignment, 1);
  if (forceDedicatedTransientUploads()) {
    return uploadDedicated();
  }
  const std::size_t alignedSize = alignUp(bytes.size(), alignment);
  if (!ensureTransientBufferUnlocked(alignedSize)) {
    return uploadDedicated();
  }

  auto canPlace = [&](std::size_t offset) {
    if (offset + alignedSize > transientBufferCapacity_) {
      return false;
    }
    for (const auto& allocation : transientBufferAllocations_) {
      const std::size_t begin = allocation.offset;
      const std::size_t end = allocation.offset + allocation.size;
      const std::size_t newBegin = offset;
      const std::size_t newEnd = offset + alignedSize;
      if (!(newEnd <= begin || newBegin >= end)) {
        return false;
      }
    }
    return true;
  };

  std::size_t offset = alignUp(transientBufferCursor_, alignment);
  if (!canPlace(offset)) {
    offset = 0;
    if (!canPlace(offset)) {
      if (!rotateTransientBufferUnlocked(alignedSize, seqId)) {
        return uploadDedicated();
      }
      offset = alignUp(transientBufferCursor_, alignment);
      if (!canPlace(offset)) {
        return uploadDedicated();
      }
    }
  }

  std::memcpy(transientBufferContents_ + offset, bytes.data(), bytes.size());
  transientBufferAllocations_.push_back(TransientBufferAllocation{
      .offset = offset,
      .size = alignedSize,
      .seqId = seqId,
  });
  transientBufferCursor_ = offset + alignedSize;

  auto slice = TransientBufferSlice{
      .buffer = WMT::Buffer{transientBuffer_.handle},
      .offset = offset,
      .size = bytes.size(),
  };
  recordUploadTime();
  return slice;
}

std::vector<CommandQueue::TransientBufferSlice> CommandQueue::uploadTransientBufferBatch(
    std::span<const std::span<const std::byte>> payloads,
    std::size_t alignment,
    std::uint64_t seqId) {
  std::vector<TransientBufferSlice> result;
  if (!device_ || payloads.empty()) {
    return result;
  }
  result.reserve(payloads.size());

  const auto uploadStarted = std::chrono::steady_clock::now();
  std::size_t totalBytes = 0;
  for (const auto& p : payloads) totalBytes += p.size();

  // Snapshot completedSeqId ONCE for the whole batch.
  std::uint64_t completedSeqId = 0;
  {
    std::lock_guard lock(mutex_);
    completedSeqId = completedSeqId_;
  }

  // Hold transientBufferMutex_ for the entire batch — single reclaim,
  // single mutex acquire, no inter-call lock thrash.
  std::lock_guard lock(transientBufferMutex_);
  reclaimTransientBuffersUnlocked(completedSeqId);

  alignment = std::max<std::size_t>(alignment, 1);

  for (const auto& bytes : payloads) {
    if (bytes.empty()) {
      result.push_back(TransientBufferSlice{});
      continue;
    }
    const std::size_t alignedSize = alignUp(bytes.size(), alignment);

    // Try slab placement; fall through to dedicated buffer if the slab
    // can't accommodate (rotation also tried).
    auto canPlace = [&](std::size_t offset) {
      if (offset + alignedSize > transientBufferCapacity_) return false;
      for (const auto& a : transientBufferAllocations_) {
        const std::size_t begin = a.offset;
        const std::size_t end = a.offset + a.size;
        const std::size_t newBegin = offset;
        const std::size_t newEnd = offset + alignedSize;
        if (!(newEnd <= begin || newBegin >= end)) return false;
      }
      return true;
    };

    auto uploadDedicated = [&]() -> TransientBufferSlice {
      WMTBufferInfo info{};
      info.length = bytes.size();
      info.options = WMTResourceStorageModeShared;
      info.memory.set((void*)bytes.data());
      auto buffer = device_.newBuffer(info);
      if (!buffer) return {};
      perf::countMetalBuffer(static_cast<std::size_t>(info.length));
      // M1: same labeling as single-payload uploadDedicated above.
      {
        WMT::Buffer view{buffer.handle};
        view.setLabel(makeLabelStringFmt("dxmt9-transient-dedicated-seq%llu-bytes%zu",
            static_cast<unsigned long long>(seqId), bytes.size()));
      }
      retainedTransientBuffers_.push_back(RetainedTransientBuffer{
          .buffer = std::move(buffer), .seqId = seqId});
      return TransientBufferSlice{
          .buffer = WMT::Buffer{retainedTransientBuffers_.back().buffer.handle},
          .offset = 0,
          .size = bytes.size(),
      };
    };

    if (forceDedicatedTransientUploads()) {
      result.push_back(uploadDedicated());
      continue;
    }

    if (!ensureTransientBufferUnlocked(alignedSize)) {
      result.push_back(uploadDedicated());
      continue;
    }
    std::size_t offset = alignUp(transientBufferCursor_, alignment);
    if (!canPlace(offset)) {
      offset = 0;
      if (!canPlace(offset)) {
        if (!rotateTransientBufferUnlocked(alignedSize, seqId)) {
          result.push_back(uploadDedicated());
          continue;
        }
        offset = alignUp(transientBufferCursor_, alignment);
        if (!canPlace(offset)) {
          result.push_back(uploadDedicated());
          continue;
        }
      }
    }
    std::memcpy(transientBufferContents_ + offset, bytes.data(), bytes.size());
    transientBufferAllocations_.push_back(TransientBufferAllocation{
        .offset = offset, .size = alignedSize, .seqId = seqId});
    transientBufferCursor_ = offset + alignedSize;
    result.push_back(TransientBufferSlice{
        .buffer = WMT::Buffer{transientBuffer_.handle},
        .offset = offset,
        .size = bytes.size(),
    });
  }

  const auto elapsed = std::chrono::steady_clock::now() - uploadStarted;
  perf::countTransientUploadCpuTime(
      static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
      totalBytes);
  return result;
}

void CommandQueue::retainSamplerForSeq(WMT::Reference<WMT::SamplerState> sampler,
                                       std::uint64_t seqId) {
  if (!sampler) {
    return;
  }
  std::lock_guard lock(transientBufferMutex_);
  retainedSamplerStates_.push_back(RetainedSamplerState{
      .sampler = std::move(sampler),
      .seqId = seqId,
  });
}

CommandQueue::TransientBufferReservation CommandQueue::reserveTransientBuffer(
    std::size_t size,
    std::size_t alignment,
    std::uint64_t seqId) {
  TransientBufferReservation reservation{};
  if (!device_ || size == 0) {
    return reservation;
  }
  const auto uploadStarted = std::chrono::steady_clock::now();
  const auto recordUploadTime = [&] {
    const auto elapsed = std::chrono::steady_clock::now() - uploadStarted;
    perf::countTransientUploadCpuTime(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
        size);
  };

  std::uint64_t completedSeqId = 0;
  {
    std::lock_guard lock(mutex_);
    completedSeqId = completedSeqId_;
  }

  std::lock_guard lock(transientBufferMutex_);
  reclaimTransientBuffersUnlocked(completedSeqId);

  alignment = std::max<std::size_t>(alignment, 1);
  const std::size_t alignedSize = alignUp(size, alignment);

  auto reserveDedicated = [&]() -> TransientBufferReservation {
    WMTBufferInfo info{};
    info.length = size;
    info.options = WMTResourceStorageModeShared;
    auto buffer = device_.newBuffer(info);
    if (!buffer || !info.memory.ptr) {
      recordUploadTime();
      return {};
    }
    perf::countMetalBuffer(static_cast<std::size_t>(info.length));
    // M1: reserved-slab dedicated path mirrors uploadDedicated naming.
    {
      WMT::Buffer view{buffer.handle};
      view.setLabel(makeLabelStringFmt("dxmt9-transient-reserved-seq%llu-bytes%zu",
          static_cast<unsigned long long>(seqId), size));
    }
    retainedTransientBuffers_.push_back(RetainedTransientBuffer{
        .buffer = std::move(buffer),
        .seqId = seqId,
    });
    TransientBufferReservation r{};
    r.slice = TransientBufferSlice{
        .buffer = WMT::Buffer{retainedTransientBuffers_.back().buffer.handle},
        .offset = 0,
        .size = size,
    };
    r.contents = static_cast<std::byte*>(info.memory.ptr);
    recordUploadTime();
    return r;
  };

  if (forceDedicatedTransientUploads()) {
    return reserveDedicated();
  }
  if (!ensureTransientBufferUnlocked(alignedSize)) {
    return reserveDedicated();
  }

  auto canPlace = [&](std::size_t offset) {
    if (offset + alignedSize > transientBufferCapacity_) {
      return false;
    }
    for (const auto& a : transientBufferAllocations_) {
      const std::size_t begin = a.offset;
      const std::size_t end = a.offset + a.size;
      const std::size_t newBegin = offset;
      const std::size_t newEnd = offset + alignedSize;
      if (!(newEnd <= begin || newBegin >= end)) {
        return false;
      }
    }
    return true;
  };

  std::size_t offset = alignUp(transientBufferCursor_, alignment);
  if (!canPlace(offset)) {
    offset = 0;
    if (!canPlace(offset)) {
      if (!rotateTransientBufferUnlocked(alignedSize, seqId)) {
        return reserveDedicated();
      }
      offset = alignUp(transientBufferCursor_, alignment);
      if (!canPlace(offset)) {
        return reserveDedicated();
      }
    }
  }

  transientBufferAllocations_.push_back(TransientBufferAllocation{
      .offset = offset,
      .size = alignedSize,
      .seqId = seqId,
  });
  transientBufferCursor_ = offset + alignedSize;

  reservation.slice = TransientBufferSlice{
      .buffer = WMT::Buffer{transientBuffer_.handle},
      .offset = offset,
      .size = size,
  };
  reservation.contents = transientBufferContents_ + offset;
  recordUploadTime();
  return reservation;
}

void CommandQueue::startThreads(std::function<void()> encodeLoop,
                                 std::function<void()> finishLoop,
                                 std::function<void()> completionLoop) {
  if (threadsStarted_) {
    return;
  }
  stop_ = false;
  encodeThread_ = std::thread(std::move(encodeLoop));
  finishThread_ = std::thread(std::move(finishLoop));
  completionThread_ = std::thread(std::move(completionLoop));
  threadsStarted_ = true;
}

void CommandQueue::stopThreads() {
  if (!threadsStarted_) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    encodeCv_.notify_all();
    finishCv_.notify_all();
    presentCompletedCv_.notify_all();
    presentDequeuedCv_.notify_all();
    writeCv_.notify_all();
  }
  queueLifecycle_.notifyPendingCompletionStop();
  if (encodeThread_.joinable()) encodeThread_.join();
  if (completionThread_.joinable()) completionThread_.join();
  if (finishThread_.joinable()) finishThread_.join();
  threadsStarted_ = false;
}

// --- Chunk-ring submission (Step 3b migration from MetalBackendDevice) --

namespace {

core::ChunkSlot& currentSlotUnlocked(CommandQueue& q) {
  // TLA+: RingSafety — caller holds q.mutex_ and has ensured a writing slot.
  return q.slots_[*q.writingSlot_];
}

void ensureWritingSlotUnlocked(CommandQueue& q, std::unique_lock<std::mutex>& lock) {
  (void)q.queueLifecycle_.ensureWriterSlot(lock, kMaxQueuedChunks);
}

std::uint64_t seqIdForMark(CommandQueue& q, std::uint64_t seqId) {
  return seqId == 0 ? q.nextSeqId_ : seqId;
}

std::size_t drawChunkCommandLimit() {
  static const std::size_t limit = [] {
    const char* env = std::getenv("DXMT9_DRAW_CHUNK_COMMAND_LIMIT");
    if (!env || env[0] == '\0') {
      return std::size_t{0};
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 10);
    if (end == env || parsed == 0) {
      return std::size_t{0};
    }
    return static_cast<std::size_t>(parsed);
  }();
  return limit;
}

bool splitPresentChunk() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_SPLIT_PRESENT_CHUNK");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool splitStretchChunk() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_SPLIT_STRETCH_CHUNK");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

// Boundary-wait policy — resolved once per process via
// dxmt9::resolveBoundaryPolicyFromEnv() (see dxmt9_presenter.hpp).
// The five-value enum collapses the previous trio of env-parsing
// lambdas (DXMT9_PRESENT_BOUNDARY_PRESENT_COMPLETION /
// DXMT9_PRESENT_BOUNDARY_COMPLETION / DXMT9_DISABLE_PRESENT_BOUNDARY)
// into a single switch — see resolveBoundaryPolicy() doc-comment for
// the priority ordering. AfterAcquire is observationally a no-op on
// the wait branch here; its effect lives in dxmt9_draw_encoder.mm,
// which also reads through resolveBoundaryPolicyFromEnv().

bool capFrameLatencyToBackBuffers() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

std::uint32_t backBufferLatencyCap(std::uint32_t backBufferCount) {
  const std::uint32_t normalized = std::max(1u, backBufferCount);
  if (normalized >= core::kMaxFrameLatency) {
    return core::kMaxFrameLatency;
  }
  return normalized + 1u;
}

std::uint32_t presentBoundaryLatency(const core::SwapDesc& desc) {
  if (capFrameLatencyToBackBuffers()) {
    return std::min(desc.maxFrameLatency, backBufferLatencyCap(desc.backBufferCount));
  }
  return desc.maxFrameLatency;
}

bool shouldApplyPresentBoundary(const core::SwapDesc&) {
  return resolveBoundaryPolicyFromEnv() != BoundaryPolicy::Disabled;
}

Presenter::AcquireParams makePresentAcquireParams(const core::SwapDesc& desc) {
  return Presenter::AcquireParams{
      .width = desc.width,
      .height = desc.height,
      .displaySyncEnabled = desc.displaySyncEnabled,
      .contentsScale = 1.0,
      .maxDrawableCount = kDefaultMetalDrawableCount,
  };
}

void markSlotResourcesUnlocked(resources::Pool& pool, const core::ChunkSlot& slot) {
  for (std::size_t i = 0; i < slot.commandCount(); ++i) {
    const auto command = slot.commandAt(i);
    switch (command.kind) {
      case core::MetalCommandKind::DrawRun:
        if (command.drawState.hot) pool.markDrawResources(*command.drawState.hot, slot.seqId);
        break;
      case core::MetalCommandKind::Clear:
        if (command.clear) pool.markClearResources(*command.clear, slot.seqId);
        break;
      case core::MetalCommandKind::SurfaceCopy:
        if (command.surfaceCopy) pool.markSurfaceCopyResources(*command.surfaceCopy, slot.seqId);
        break;
      case core::MetalCommandKind::StretchRect:
        if (command.stretchRect) pool.markStretchResources(*command.stretchRect, slot.seqId);
        break;
      case core::MetalCommandKind::Readback:
        if (command.readback) pool.markReadbackResources(*command.readback, slot.seqId);
        break;
      case core::MetalCommandKind::ColorFill:
        if (command.colorFill) pool.markColorFillResources(*command.colorFill, slot.seqId);
        break;
      case core::MetalCommandKind::DepthResolve:
        if (command.depthResolve) pool.markDepthResolveResources(*command.depthResolve, slot.seqId);
        break;
      case core::MetalCommandKind::Present:
        if (command.present && command.present->presentSource) {
          pool.markSurfaceUse(command.present->presentSource, slot.seqId);
        }
        break;
    }
  }
}

void prepareSlotForPublish(CommandQueue& q,
                           resources::Pool& pool,
                           const core::ChunkSlot& slot) {
  markSlotResourcesUnlocked(pool, slot);
  q.prefetchSlotPipelines(slot);
}

void maybeCommitDrawChunkUnlocked(
    CommandQueue& q,
    resources::Pool& pool,
    std::unique_lock<std::mutex>& lock) {
  const std::size_t limit = drawChunkCommandLimit();
  if (limit == 0 || !q.writingSlot_) {
    return;
  }
  if (currentSlotUnlocked(q).commandCount() < limit) {
    return;
  }
  q.queueLifecycle_.commitCurrentChunk(
      lock, kMaxQueuedChunks, [&q, &pool](const core::ChunkSlot& slot) {
        prepareSlotForPublish(q, pool, slot);
      });
}

}  // namespace

void CommandQueue::setSkipDrawResourceMarking(bool skip) {
  std::unique_lock lock(mutex_);
  skipDrawResourceMarking_ = skip;
}

void CommandQueue::markChunkResources(std::span<const core::ChunkHandleEntry> entries) {
  if (entries.empty()) {
    return;
  }
  std::unique_lock lock(mutex_);
  // Single seqId snapshot for the whole bulk-mark — the importer is
  // about to emit Draw* records onto the same chunk, so all resources
  // get pinned to the chunk's nextSeqId together.
  const std::uint64_t seqId = seqIdForMark(*this, 0);
  for (const auto& entry : entries) {
    switch (entry.kind) {
    case core::ChunkHandleKind::Texture:
      pool_.markTextureUse(entry.handle, seqId);
      break;
    case core::ChunkHandleKind::Surface:
      pool_.markSurfaceUse(entry.handle, seqId);
      break;
    case core::ChunkHandleKind::Buffer:
      pool_.markBufferUse(entry.handle, seqId);
      break;
    case core::ChunkHandleKind::Shader:
    case core::ChunkHandleKind::VertexDecl:
      // No pool table for these yet — kinds reserved for future use.
      break;
    }
  }
}

void CommandQueue::submitDrawRun(core::CanonicalDrawState state,
                                 const core::DrawUniformPayload& uniforms,
                                 std::span<const core::DrawParam> draws,
                                 std::span<const core::DrawParamPayloadView> payloads) {
  if (draws.empty()) {
    return;
  }
  // Per-draw-run hot entry. Heap-allocation invariant per
  // codebase_conventions.rules.md; debug-only guard, no-op unless
  // DXMT_DEBUG_NO_PER_DRAW_ALLOC=1 build flag and env are both set.
  DXMT_DEBUG_NO_HEAP_ALLOC_SCOPE("submitDrawRun");
  for (std::size_t i = 0; i < draws.size(); ++i) {
    perf::countSubmitDraw();
  }
  PerfScope scope(perf::countSubmitDrawCpuTime);
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  if (!skipDrawResourceMarking_) {
    pool_.markDrawResources(state.hot, seqIdForMark(*this, 0));
  }
  currentBackBuffer_ = state.hot.colorAttachments[0].handle;
  currentSlotUnlocked(*this).appendDrawRun(std::move(state), uniforms, draws, payloads);
  maybeCommitDrawChunkUnlocked(*this, pool_, lock);
}

void CommandQueue::submitClear(const core::ClearDesc& desc) {
  perf::countSubmitClear();
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  traceTextureClear(pool_, desc);
  for (const auto& attachment : desc.colorAttachments) {
    traceTextureSurfaceOp(pool_, "Clear", attachment.handle, desc.depthStencil.handle);
  }
  currentSlotUnlocked(*this).appendClear(desc);
  if (desc.colorAttachments[0].handle) {
    currentBackBuffer_ = desc.colorAttachments[0].handle;
  }
  pool_.markClearResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitSurfaceCopy(const core::SurfaceCopyDesc& desc) {
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  traceTextureSurfaceOp(pool_, "SurfaceCopy", desc.source, desc.destination);
  currentSlotUnlocked(*this).appendSurfaceCopy(desc);
  currentBackBuffer_ = desc.destination;
  pool_.markSurfaceCopyResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitStretchRect(const core::StretchRectDesc& desc) {
  perf::countSubmitStretch();
  std::unique_lock lock(mutex_);
  if (splitStretchChunk()) {
    queueLifecycle_.commitCurrentChunk(
        lock, kMaxQueuedChunks, [this](const core::ChunkSlot& slot) {
          prepareSlotForPublish(*this, pool_, slot);
        });
  }
  ensureWritingSlotUnlocked(*this, lock);
  traceTextureSurfaceOp(pool_, "StretchRect", desc.source, desc.destination);
  currentSlotUnlocked(*this).appendStretchRect(desc);
  currentBackBuffer_ = desc.destination;
  pool_.markStretchResources(desc, seqIdForMark(*this, 0));
  if (splitStretchChunk()) {
    queueLifecycle_.commitCurrentChunk(
        lock, kMaxQueuedChunks, [this](const core::ChunkSlot& slot) {
          prepareSlotForPublish(*this, pool_, slot);
        });
  }
}

void CommandQueue::submitReadback(const core::ReadbackDesc& desc) {
  std::lock_guard lock(mutex_);
  // Readback is satisfied synchronously in CommandQueue::readbackSurface.
  // Still mark resources so NoUseAfterFree remains meaningful.
  traceTextureSurfaceOp(pool_, "Readback", desc.source, desc.destination);
  pool_.markReadbackResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitColorFill(const core::ColorFillDesc& desc) {
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  traceTextureSurfaceOp(pool_, "ColorFill", desc.destination);
  currentSlotUnlocked(*this).appendColorFill(desc);
  currentBackBuffer_ = desc.destination;
  pool_.markColorFillResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitDepthResolve(const core::DepthResolveDesc& desc) {
  // R-FORMAT-11 — RESZ MSAA depth resolve. Fire-and-forget surface op:
  // append the command + mark both endpoints, mirroring submitColorFill.
  // The destination is the INTZ depth texture, not the present back buffer,
  // so currentBackBuffer_ is left untouched.
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  traceTextureSurfaceOp(pool_, "DepthResolve", desc.msaaDepth, desc.intzDest);
  currentSlotUnlocked(*this).appendDepthResolve(desc);
  pool_.markDepthResolveResources(desc, seqIdForMark(*this, 0));
}

std::uint64_t CommandQueue::submitPresent(const core::SwapDesc& desc) {
  // TLA+: PresentFrameLatency / CommitPresent.
  perf::countSubmitPresent();
  core::SwapDesc queuedDesc = desc;
  // Resolve the queue-local Presenter binding once. Stale ids (swapchain
  // destroyed between snapshotSwapDesc and submitPresent) resolve to
  // nullptr; the legacy raw-pointer code already tolerated that path so
  // the rest of this function preserves the same control flow.
  Presenter* presenter = lookupPresenter(queuedDesc.presentId);
  if (presenter) {
    switch (presenter->acquirePolicy()) {
      case AcquirePolicy::Async: {
        perf::countPresentAsyncAcquireRequest();
        auto token = presenter->beginAcquireDrawable(makePresentAcquireParams(queuedDesc));
        if (token) {
          perf::countPresentAsyncAcquireIssued();
          queuedDesc.drawableTokenRequired = true;
        } else {
          perf::countPresentAsyncAcquireFallback();
          queuedDesc.drawableTokenRequired = false;
        }
        stashDrawableToken(queuedDesc.presentId, std::move(token));
        break;
      }
      case AcquirePolicy::SyncOnSubmit: {
        {
          std::unique_lock lock(mutex_);
          queueLifecycle_.commitCurrentChunk(
              lock, kMaxQueuedChunks, [this](const core::ChunkSlot& slot) {
                prepareSlotForPublish(*this, pool_, slot);
              });
        }
        auto token = presenter->acquireDrawable(makePresentAcquireParams(queuedDesc));
        queuedDesc.drawableTokenRequired = true;
        stashDrawableToken(queuedDesc.presentId, std::move(token));
        break;
      }
      case AcquirePolicy::Sync:
      case AcquirePolicy::PreAcquire:
        // Sync acquires inline in encodeCommands; PreAcquire feeds the
        // prefetched-drawable cache from the encode thread.
        break;
    }
  }

  std::uint64_t presentSeqId = 0;
  {
    std::unique_lock lock(mutex_);
    const core::Handle sourceHandle =
        core::metalqueue::selectPresentSourceHandle(queuedDesc, currentBackBuffer_);
    perf::countPresentSourceSelection(static_cast<bool>(queuedDesc.sourceSurface),
                                      sourceHandle.value != 0 &&
                                          sourceHandle.value == currentBackBuffer_.value);
    if (splitPresentChunk()) {
      queueLifecycle_.commitCurrentChunk(
          lock, kMaxQueuedChunks, [this](const core::ChunkSlot& slot) {
            prepareSlotForPublish(*this, pool_, slot);
          });
      ensureWritingSlotUnlocked(*this, lock);
      queueLifecycle_.appendPresentCommand(queuedDesc, sourceHandle);
      queueLifecycle_.commitCurrentChunk(
          lock, kMaxQueuedChunks, [this](const core::ChunkSlot& slot) {
            prepareSlotForPublish(*this, pool_, slot);
          });
      presentSeqId = lastCommittedSeqId_;
    } else {
      queueLifecycle_.presentAndCommit(
          lock, kMaxQueuedChunks, queuedDesc, sourceHandle,
          [this](const core::ChunkSlot& slot) {
            prepareSlotForPublish(*this, pool_, slot);
          });
      presentSeqId = lastCommittedSeqId_;
    }
  }

  if (shouldApplyPresentBoundary(queuedDesc)) {
    perf::countPresentBoundaryApplied();
    presentBoundary(presentSeqId, presentBoundaryLatency(queuedDesc));
  } else {
    perf::countPresentBoundarySkipped();
  }
  return presentSeqId;
}

void CommandQueue::presentBoundary(std::uint64_t presentSeqId, std::uint32_t maxFrameLatency) {
  // TLA+: PresentFrameLatency / BeginPresentWait + CommitPendingPresent.
  if (presentSeqId == 0) {
    return;
  }
  maxFrameLatency = std::clamp<std::uint32_t>(maxFrameLatency, 1u, kMaxQueuedChunks);
  if (presentSeqId <= maxFrameLatency) {
    return;
  }
  const std::uint64_t targetSeqId = presentSeqId - maxFrameLatency;
  std::unique_lock lock(mutex_);
  // R-BACK / PresentFrameLatency — branch on the unified
  // BoundaryPolicy resolved once at process init. Disabled is
  // filtered earlier by shouldApplyPresentBoundary and never reaches
  // here; AfterAcquire shares the Default wait branch (the position
  // of notePresentDequeued is the only observable difference).
  const BoundaryPolicy policy = resolveBoundaryPolicyFromEnv();
  const auto reachedBoundary = [&] {
    switch (policy) {
      case BoundaryPolicy::PresentCompletion:
        return presentCompletedSeqId_ >= targetSeqId;
      case BoundaryPolicy::Completion:
        return completedSeqId_ >= targetSeqId;
      case BoundaryPolicy::Default:
      case BoundaryPolicy::AfterAcquire:
        return presentDequeuedSeqId_ >= targetSeqId;
      case BoundaryPolicy::Disabled:
        return true;
    }
    return true;
  };
  if (reachedBoundary()) {
    return;
  }
  const auto waitStarted = std::chrono::steady_clock::now();
  switch (policy) {
    case BoundaryPolicy::PresentCompletion:
      presentCompletedCv_.wait(lock, [&] { return stop_ || reachedBoundary(); });
      break;
    case BoundaryPolicy::Completion:
      finishCv_.wait(lock, [&] { return stop_ || reachedBoundary(); });
      break;
    case BoundaryPolicy::Default:
    case BoundaryPolicy::AfterAcquire:
      presentDequeuedCv_.wait(lock, [&] { return stop_ || reachedBoundary(); });
      break;
    case BoundaryPolicy::Disabled:
      break;
  }
  const auto waitElapsed = std::chrono::steady_clock::now() - waitStarted;
  // TLA+: PresentFrameLatency / AppWaitReturnSafe
  DXMT_ASSERT(stop_ || reachedBoundary());
  // TLA+: PresentFrameLatency / PresentCompletionSafety
  DXMT_ASSERT(presentCompletedSeqId_ <= completedSeqId_);
  perf::countPresentBoundaryWait(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(waitElapsed).count()));
}

void CommandQueue::notePresentDequeued(std::uint64_t seqId) {
  std::lock_guard lock(mutex_);
  presentDequeuedSeqId_ = std::max(presentDequeuedSeqId_, seqId);
  presentDequeuedCv_.notify_all();
}

std::optional<core::metalcapture::MetalCaptureRequest>
CommandQueue::metalCaptureForPresentChunk(std::uint64_t seqId) {
  return metalCapture_.maybeCapturePresentChunk(seqId);
}

std::optional<core::metalcapture::MetalCaptureRequest>
CommandQueue::metalCaptureForChunkBegin(std::uint64_t seqId) {
  return metalCapture_.maybeCaptureAtChunkBegin(seqId);
}

std::optional<core::metalcapture::MetalCaptureRequest>
CommandQueue::notePresentChunkForCapture(std::uint64_t seqId) {
  return metalCapture_.maybePresentChunkClosesSession(seqId);
}

void CommandQueue::submitFlush() {
  perf::countSubmitFlush();
  std::unique_lock lock(mutex_);
  queueLifecycle_.flushAndWait(
      lock, kMaxQueuedChunks, [this](const core::ChunkSlot& slot) {
        prepareSlotForPublish(*this, pool_, slot);
      });
}

core::HResult CommandQueue::waitForVBlank() {
  submitFlush();
  return core::HResult{0};
}

void* CommandQueue::mapBuffer(core::BufferHandle handle, std::uint32_t flags) {
  // Pool storage + queue's wait-for-sequence rule under one mutex.
  const auto totalStart = std::chrono::steady_clock::now();
  std::unique_lock lock(mutex_);
  const auto lockAcquired = std::chrono::steady_clock::now();
  const std::uint64_t waitSeq = pool_.mapWaitSeqId(handle, flags);
  const bool hasWaitSeq = waitSeq != 0;
  const auto waitStart = std::chrono::steady_clock::now();
  // Wine writeonly_vertex_buffer_readback_policy (#66): a Draw followed
  // by a Lock without an intervening Present must still observe the
  // draw on read. The drawn slot's seqId is set as soon as the draw is
  // appended to the current chunk, but the chunk hasn't been committed
  // to Metal yet — without committing, completedSeqId_ can never reach
  // waitSeq and the wait below would block forever. Drive the pending
  // chunk into the submit pipeline first.
  if (waitSeq > lastCommittedSeqId_) {
    queueLifecycle_.commitCurrentChunk(
        lock, kMaxQueuedChunks, [this](const core::ChunkSlot& slot) {
          prepareSlotForPublish(*this, pool_, slot);
        });
  }
  if (waitSeq > completedSeqId_) {
    queueLifecycle_.waitForSequence(lock, waitSeq);
  }
  const auto waitEnd = std::chrono::steady_clock::now();
  perf::countMapBufferWait(
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
          waitEnd - totalStart).count()),
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
          lockAcquired - totalStart).count()),
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
          waitEnd - waitStart).count()),
      flags,
      hasWaitSeq);
  // R-BACK-5.8 — pass the GPU completion watermark and the device
  // reference so the rename ring can rotate / fresh-allocate on
  // DISCARD without blocking on prior completion. Non-DYNAMIC paths
  // ignore both arguments.
  return pool_.finalizeBufferMap(device_, handle, flags, completedSeqId_);
}

bool CommandQueue::readbackSurface(const core::ReadbackDesc& desc, core::ReadbackPixels& pixels) {
  return encoders::readbackSurface(*this, pool_, device_, limits_, desc, pixels);
}

void CommandQueue::runEncodeLoop(EncodeChunkFn encodeChunk, OnSubmittedFn onSubmitted) {
  while (true) {
    std::unique_lock lock(mutex_);
    if (!queueLifecycle_.runEncodeIteration(lock, encodeChunk, onSubmitted)) {
      return;
    }
  }
}

void CommandQueue::bindSelfLifecycle(ResolveSurfaceFlagsFn resolveSurfaceFlags) {
  queueLifecycle_.bindTrackedSubmissionState({
      .writingSlot = &writingSlot_,
      .writeIndex = &writeIndex_,
      .nextSeqId = &nextSeqId_,
      .readySlots = &readySlots_,
      .completedSeqQueue = &completedSeqQueue_,
      .completedPresentSeqQueue = &completedPresentSeqQueue_,
      .inflightCount = &inflightCount_,
      .completedSeqId = &completedSeqId_,
      .presentCompletedSeqId = &presentCompletedSeqId_,
      .lastCommittedSeqId = &lastCommittedSeqId_,
      .slots = std::span<core::ChunkSlot>(slots_.data(), slots_.size()),
      .mutex = &mutex_,
      .writeCv = &writeCv_,
      .encodeCv = &encodeCv_,
      .finishCv = &finishCv_,
      .presentCompletedCv = &presentCompletedCv_,
      .stop = &stop_,
      .submissionDiagnostics = &submissionDiagnostics_,
      .resolveSurfaceFlags = std::move(resolveSurfaceFlags),
  });
}

void CommandQueue::runFinishLoop() {
  while (true) {
    std::unique_lock lock(mutex_);
    if (!queueLifecycle_.runFinishIteration(lock, [this](std::uint64_t) {
          allocators_.reclaim(completedSeqId_);
          pool_.reclaimCompleted(completedSeqId_);
          {
            std::lock_guard transientLock(transientBufferMutex_);
            reclaimTransientBuffersUnlocked(completedSeqId_);
          }
        })) {
      return;
    }
  }
}

void CommandQueue::runCompletionWatcherLoop() {
  while (queueLifecycle_.processOnePendingCompletion(stop_)) {
    // continue until processOnePendingCompletion returns false (stop)
  }
}

core::PresentId CommandQueue::registerPresenter(Presenter* presenter) {
  if (!presenter) {
    return {};
  }
  std::lock_guard lock(presenterRegistryMutex_);
  std::uint32_t slotIndex = 0;
  if (presenterFreeHead_ >= 0) {
    slotIndex = static_cast<std::uint32_t>(presenterFreeHead_);
    presenterFreeHead_ = presenterSlots_[slotIndex].nextFree;
    presenterSlots_[slotIndex].nextFree = -1;
  } else {
    slotIndex = static_cast<std::uint32_t>(presenterSlots_.size());
    presenterSlots_.emplace_back();
  }
  auto& slot = presenterSlots_[slotIndex];
  slot.presenter = presenter;
  slot.pendingToken.reset();
  // Generation is bumped on free; the current value is what the caller
  // observes for this allocation lifetime.
  return core::PresentId{encodePresentId(slotIndex, slot.generation)};
}

void CommandQueue::unregisterPresenter(core::PresentId id) {
  if (!id) {
    return;
  }
  std::lock_guard lock(presenterRegistryMutex_);
  const std::uint32_t slotIndex = decodePresentIdSlot(id);
  if (slotIndex >= presenterSlots_.size()) {
    return;
  }
  auto& slot = presenterSlots_[slotIndex];
  if (slot.generation != decodePresentIdGeneration(id)) {
    return;
  }
  slot.presenter = nullptr;
  slot.pendingToken.reset();
  // Bump generation so any in-flight PresentId carrying the old value
  // resolves to nullptr in lookupPresenter — the encoder will skip the
  // present rather than reach into a destroyed Presenter.
  ++slot.generation;
  slot.nextFree = presenterFreeHead_;
  presenterFreeHead_ = static_cast<std::int32_t>(slotIndex);
}

Presenter* CommandQueue::lookupPresenter(core::PresentId id) const {
  if (!id) {
    return nullptr;
  }
  std::lock_guard lock(presenterRegistryMutex_);
  const std::uint32_t slotIndex = decodePresentIdSlot(id);
  if (slotIndex >= presenterSlots_.size()) {
    return nullptr;
  }
  const auto& slot = presenterSlots_[slotIndex];
  if (slot.generation != decodePresentIdGeneration(id)) {
    return nullptr;
  }
  return slot.presenter;
}

void CommandQueue::stashDrawableToken(core::PresentId id,
                                       std::shared_ptr<PresentDrawableToken> token) {
  if (!id) {
    return;
  }
  std::lock_guard lock(presenterRegistryMutex_);
  const std::uint32_t slotIndex = decodePresentIdSlot(id);
  if (slotIndex >= presenterSlots_.size()) {
    return;
  }
  auto& slot = presenterSlots_[slotIndex];
  if (slot.generation != decodePresentIdGeneration(id)) {
    return;
  }
  slot.pendingToken = std::move(token);
}

std::shared_ptr<PresentDrawableToken>
CommandQueue::takeDrawableToken(core::PresentId id) {
  if (!id) {
    return {};
  }
  std::lock_guard lock(presenterRegistryMutex_);
  const std::uint32_t slotIndex = decodePresentIdSlot(id);
  if (slotIndex >= presenterSlots_.size()) {
    return {};
  }
  auto& slot = presenterSlots_[slotIndex];
  if (slot.generation != decodePresentIdGeneration(id)) {
    return {};
  }
  return std::exchange(slot.pendingToken, {});
}

}  // namespace dxmt9
