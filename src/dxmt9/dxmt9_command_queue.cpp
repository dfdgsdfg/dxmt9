#include "dxmt9_command_queue.hpp"
#include "dxmt9_device.hpp"
#include "dxmt9_blit_encoders.hpp"
#include "dxmt9_compat.hpp"
#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_presenter.hpp"
#include "dxmt9_resource_initializer.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_ring_arena.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace dxmt9 {

namespace {

using core::MetalCommandRecord;

MetalCommandRecord makeDrawCommand(const core::DrawDesc& desc) {
  MetalCommandRecord op;
  op.kind = MetalCommandRecord::Kind::Draw;
  op.draw = desc;
  return op;
}

MetalCommandRecord makeDrawRunCommand(core::DrawRunDesc desc) {
  MetalCommandRecord op;
  op.kind = MetalCommandRecord::Kind::DrawRun;
  op.drawRun = std::move(desc);
  return op;
}

MetalCommandRecord makeClearCommand(const core::ClearDesc& desc) {
  MetalCommandRecord op;
  op.kind = MetalCommandRecord::Kind::Clear;
  op.clear = desc;
  return op;
}

MetalCommandRecord makeSurfaceCopyCommand(const core::SurfaceCopyDesc& desc) {
  MetalCommandRecord op;
  op.kind = MetalCommandRecord::Kind::SurfaceCopy;
  op.surfaceCopy = desc;
  return op;
}

MetalCommandRecord makeStretchRectCommand(const core::StretchRectDesc& desc) {
  MetalCommandRecord op;
  op.kind = MetalCommandRecord::Kind::StretchRect;
  op.stretchRect = desc;
  return op;
}

MetalCommandRecord makeColorFillCommand(const core::ColorFillDesc& desc) {
  MetalCommandRecord op;
  op.kind = MetalCommandRecord::Kind::ColorFill;
  op.colorFill = desc;
  return op;
}

}  // namespace

namespace {
std::string resolveShaderCachePath() {
  char buf[4096]{};
  WMTGetShaderCachePath(buf, sizeof(buf));
  return std::string(buf);
}

constexpr std::size_t kTransientBufferInitialCapacity = 8ull << 20;

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
      shaderArchive_(device, resolveShaderCachePath()) {
  if (!device_) {
    return;
  }
  queue_ = device_.newCommandQueue(0);
  if (!queue_) {
    return;
  }
  queueView_ = WMT::CommandQueue{queue_.handle};

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
  return encoders::EncodeContext{
      device_, limits_, pool_, pipelineCache_, allocators_,
      &shaderArchive_.reference(), &shaderArchive_.path(),
      *this,
  };
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
                                        std::uint32_t pitch,
                                        std::span<const std::uint8_t> bytes) {
  if (initializer_) {
    initializer_->uploadTextureLevel(handle, level, width, height, pitch, bytes);
  }
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
      retainedTransientBuffers_.push_back(RetainedTransientBuffer{
          .buffer = std::move(buffer), .seqId = seqId});
      return TransientBufferSlice{
          .buffer = WMT::Buffer{retainedTransientBuffers_.back().buffer.handle},
          .offset = 0,
          .size = bytes.size(),
      };
    };

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

bool presentBoundaryWaitsForCompletion() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PRESENT_BOUNDARY_COMPLETION");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool presentBoundaryWaitsForPresentCompletion() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PRESENT_BOUNDARY_PRESENT_COMPLETION");
    if (!env) {
      return true;
    }
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool disablePresentBoundary() {
  static const bool disabled = [] {
    const char* env = std::getenv("DXMT9_DISABLE_PRESENT_BOUNDARY");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return disabled;
}

bool forceSyncPresentBoundary() {
  static const bool force = [] {
    const char* env = std::getenv("DXMT9_FORCE_SYNC_PRESENT_BOUNDARY");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return force;
}

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

bool shouldApplyPresentBoundary(const core::SwapDesc& desc) {
  return !disablePresentBoundary() &&
         (!desc.displaySyncEnabled || forceSyncPresentBoundary());
}

bool acquirePresentOnSubmit() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PRESENT_ACQUIRE_ON_SUBMIT");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool asyncAcquirePresentOnSubmit() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PRESENT_ASYNC_ACQUIRE");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
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
  for (const auto& command : slot.commands) {
    switch (command.kind) {
      case core::MetalCommandRecord::Kind::Draw:
        pool.markDrawResources(command.draw, slot.seqId);
        break;
      case core::MetalCommandRecord::Kind::Clear:
        pool.markClearResources(command.clear, slot.seqId);
        break;
      case core::MetalCommandRecord::Kind::SurfaceCopy:
        pool.markSurfaceCopyResources(command.surfaceCopy, slot.seqId);
        break;
      case core::MetalCommandRecord::Kind::StretchRect:
        pool.markStretchResources(command.stretchRect, slot.seqId);
        break;
      case core::MetalCommandRecord::Kind::Readback:
        pool.markReadbackResources(command.readback, slot.seqId);
        break;
      case core::MetalCommandRecord::Kind::ColorFill:
        pool.markColorFillResources(command.colorFill, slot.seqId);
        break;
      case core::MetalCommandRecord::Kind::Present:
        if (command.presentSource) {
          if (auto* surface = pool.findSurface(command.presentSource.value)) {
            surface->lastUsedSeqId = std::max(surface->lastUsedSeqId, slot.seqId);
          }
        }
        break;
    }
  }
}

void maybeCommitDrawChunkUnlocked(
    CommandQueue& q,
    resources::Pool& pool,
    std::unique_lock<std::mutex>& lock) {
  const std::size_t limit = drawChunkCommandLimit();
  if (limit == 0 || !q.writingSlot_) {
    return;
  }
  if (currentSlotUnlocked(q).commands.size() < limit) {
    return;
  }
  q.queueLifecycle_.commitCurrentChunk(
      lock, kMaxQueuedChunks, [&pool](const core::ChunkSlot& slot) {
        markSlotResourcesUnlocked(pool, slot);
      });
}

}  // namespace

void CommandQueue::submitDraw(const core::DrawDesc& desc) {
  perf::countSubmitDraw();
  PerfScope scope(perf::countSubmitDrawCpuTime);
  std::unique_lock lock(mutex_);
  // TLA+: WineCommit
  ensureWritingSlotUnlocked(*this, lock);
  currentSlotUnlocked(*this).commands.push_back(makeDrawCommand(desc));
  currentBackBuffer_ = desc.rts.color[0].handle;
  // Phase 14: chunk-import path already pinned every resource via
  // markChunkResources before iterating records — skip the redundant
  // per-draw walk. Legacy non-chunk path keeps the per-draw mark.
  if (!skipDrawResourceMarking_) {
    pool_.markDrawResources(desc, seqIdForMark(*this, 0));
  }
  maybeCommitDrawChunkUnlocked(*this, pool_, lock);
}

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

void CommandQueue::submitDrawRun(core::DrawRunDesc desc) {
  if (desc.draws.empty()) {
    return;
  }
  // Count each per-draw param toward submit_draw so the perf counter
  // remains comparable across submitDraw / submitDrawBatch / submitDrawRun
  // ingress paths.
  for (std::size_t i = 0; i < desc.draws.size(); ++i) {
    perf::countSubmitDraw();
  }
  PerfScope scope(perf::countSubmitDrawCpuTime);
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  // Phase 14: chunk-import path already bulk-marked all resources; the
  // per-draw markDrawResources walk is pure CPU waste in that mode.
  // Legacy non-chunk path still needs the per-draw walk — but since the
  // BaseDrawState is shared, mark it once with a single synthetic that
  // shadows just enough to walk the resource set (textures + RT + DS +
  // VBuffers + IB are all base-stable across a run). Per-draw fields
  // (primitiveType / counts / UP payloads) don't carry handles; they
  // only feed the encoder, not the resource walker.
  if (!skipDrawResourceMarking_) {
    pool_.markDrawResources(desc.base, seqIdForMark(*this, 0));
  }
  currentBackBuffer_ = desc.base.rts.color[0].handle;
  currentSlotUnlocked(*this).commands.push_back(makeDrawRunCommand(std::move(desc)));
  maybeCommitDrawChunkUnlocked(*this, pool_, lock);
}

void CommandQueue::submitDrawBatch(std::span<const core::DrawDesc> descs) {
  if (descs.empty()) {
    return;
  }
  for (std::size_t i = 0; i < descs.size(); ++i) {
    perf::countSubmitDraw();
  }
  PerfScope scope(perf::countSubmitDrawCpuTime);
  std::unique_lock lock(mutex_);
  // Single mutex acquire amortized across N draws — the per-draw cost
  // here is just makeDrawCommand + push_back + markDrawResources.
  // ensureWritingSlotUnlocked + maybeCommitDrawChunkUnlocked may still
  // close + reopen a chunk in the middle of the batch if the chunk-byte
  // limit fires; that's intentional, otherwise a runaway batch would
  // bypass the limit guard entirely.
  for (const auto& desc : descs) {
    ensureWritingSlotUnlocked(*this, lock);
    currentSlotUnlocked(*this).commands.push_back(makeDrawCommand(desc));
    currentBackBuffer_ = desc.rts.color[0].handle;
    if (!skipDrawResourceMarking_) {
      pool_.markDrawResources(desc, seqIdForMark(*this, 0));
    }
    maybeCommitDrawChunkUnlocked(*this, pool_, lock);
  }
}

void CommandQueue::submitClear(const core::ClearDesc& desc) {
  perf::countSubmitClear();
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  currentSlotUnlocked(*this).commands.push_back(makeClearCommand(desc));
  if (desc.colorAttachments[0].handle) {
    currentBackBuffer_ = desc.colorAttachments[0].handle;
  }
  pool_.markClearResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitSurfaceCopy(const core::SurfaceCopyDesc& desc) {
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  currentSlotUnlocked(*this).commands.push_back(makeSurfaceCopyCommand(desc));
  currentBackBuffer_ = desc.destination;
  pool_.markSurfaceCopyResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitStretchRect(const core::StretchRectDesc& desc) {
  perf::countSubmitStretch();
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  currentSlotUnlocked(*this).commands.push_back(makeStretchRectCommand(desc));
  currentBackBuffer_ = desc.destination;
  pool_.markStretchResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitReadback(const core::ReadbackDesc& desc) {
  std::lock_guard lock(mutex_);
  // Readback is satisfied synchronously in CommandQueue::readbackSurface.
  // Still mark resources so NoUseAfterFree remains meaningful.
  pool_.markReadbackResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitColorFill(const core::ColorFillDesc& desc) {
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  currentSlotUnlocked(*this).commands.push_back(makeColorFillCommand(desc));
  currentBackBuffer_ = desc.destination;
  pool_.markColorFillResources(desc, seqIdForMark(*this, 0));
}

std::uint64_t CommandQueue::submitPresent(const core::SwapDesc& desc) {
  perf::countSubmitPresent();
  core::SwapDesc queuedDesc = desc;
  if (queuedDesc.presenter && asyncAcquirePresentOnSubmit()) {
    perf::countPresentAsyncAcquireRequest();
    queuedDesc.drawableToken =
        queuedDesc.presenter->beginAcquireDrawable(makePresentAcquireParams(queuedDesc));
    if (queuedDesc.drawableToken) {
      perf::countPresentAsyncAcquireIssued();
    } else {
      perf::countPresentAsyncAcquireFallback();
    }
    queuedDesc.drawableTokenRequired = static_cast<bool>(queuedDesc.drawableToken);
  } else if (queuedDesc.presenter && acquirePresentOnSubmit()) {
    {
      std::unique_lock lock(mutex_);
      queueLifecycle_.commitCurrentChunk(
          lock, kMaxQueuedChunks, [this](const core::ChunkSlot& slot) {
            markSlotResourcesUnlocked(pool_, slot);
          });
    }
    queuedDesc.drawableToken =
        queuedDesc.presenter->acquireDrawable(makePresentAcquireParams(queuedDesc));
    queuedDesc.drawableTokenRequired = true;
  }

  std::uint64_t presentSeqId = 0;
  {
    std::unique_lock lock(mutex_);
    const core::Handle sourceHandle = queuedDesc.sourceSurface ? queuedDesc.sourceSurface : currentBackBuffer_;
    if (splitPresentChunk()) {
      queueLifecycle_.commitCurrentChunk(
          lock, kMaxQueuedChunks, [this](const core::ChunkSlot& slot) {
            markSlotResourcesUnlocked(pool_, slot);
          });
      ensureWritingSlotUnlocked(*this, lock);
      queueLifecycle_.appendPresentCommand(queuedDesc, sourceHandle);
      queueLifecycle_.commitCurrentChunk(
          lock, kMaxQueuedChunks, [this](const core::ChunkSlot& slot) {
            markSlotResourcesUnlocked(pool_, slot);
          });
      presentSeqId = lastCommittedSeqId_;
    } else {
      queueLifecycle_.presentAndCommit(
          lock, kMaxQueuedChunks, queuedDesc, sourceHandle,
          [this](const core::ChunkSlot& slot) {
            markSlotResourcesUnlocked(pool_, slot);
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
  if (presentSeqId == 0) {
    return;
  }
  maxFrameLatency = std::clamp<std::uint32_t>(maxFrameLatency, 1u, kMaxQueuedChunks);
  if (presentSeqId <= maxFrameLatency) {
    return;
  }
  const std::uint64_t targetSeqId = presentSeqId - maxFrameLatency;
  std::unique_lock lock(mutex_);
  const bool waitForPresentCompletion = presentBoundaryWaitsForPresentCompletion();
  const bool waitForCompletion = !waitForPresentCompletion && presentBoundaryWaitsForCompletion();
  const auto reachedBoundary = [&] {
    if (waitForPresentCompletion) {
      return presentCompletedSeqId_ >= targetSeqId;
    }
    if (waitForCompletion) {
      return completedSeqId_ >= targetSeqId;
    }
    return presentDequeuedSeqId_ >= targetSeqId;
  };
  if (reachedBoundary()) {
    return;
  }
  const auto waitStarted = std::chrono::steady_clock::now();
  if (waitForPresentCompletion) {
    presentCompletedCv_.wait(lock, [&] { return stop_ || reachedBoundary(); });
  } else if (waitForCompletion) {
    finishCv_.wait(lock, [&] { return stop_ || reachedBoundary(); });
  } else {
    presentDequeuedCv_.wait(lock, [&] { return stop_ || reachedBoundary(); });
  }
  const auto waitElapsed = std::chrono::steady_clock::now() - waitStarted;
  perf::countPresentBoundaryWait(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(waitElapsed).count()));
}

void CommandQueue::notePresentDequeued(std::uint64_t seqId) {
  std::lock_guard lock(mutex_);
  presentDequeuedSeqId_ = std::max(presentDequeuedSeqId_, seqId);
  presentDequeuedCv_.notify_all();
}

void CommandQueue::submitFlush() {
  perf::countSubmitFlush();
  std::unique_lock lock(mutex_);
  queueLifecycle_.flushAndWait(
      lock, kMaxQueuedChunks, [this](const core::ChunkSlot& slot) {
        markSlotResourcesUnlocked(pool_, slot);
      });
}

core::HResult CommandQueue::waitForVBlank() {
  submitFlush();
  return core::HResult{0};
}

void* CommandQueue::mapBuffer(core::BufferHandle handle, std::uint32_t flags) {
  // Pool storage + queue's wait-for-sequence rule under one mutex.
  std::unique_lock lock(mutex_);
  const std::uint64_t waitSeq = pool_.mapWaitSeqId(handle, flags);
  if (waitSeq > completedSeqId_) {
    queueLifecycle_.waitForSequence(lock, waitSeq);
  }
  return pool_.finalizeBufferMap(handle, flags);
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

}  // namespace dxmt9
