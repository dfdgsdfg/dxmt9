#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9_compat.hpp"
#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_resource_initializer.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_ring_arena.hpp"
#include "dxmt9_runtime_services.hpp"
#include "dxmt9_shader_archive.hpp"

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

CommandQueue::CommandQueue(WMT::Device device, RuntimeServices& services)
    : device_(device),
      limits_(&services.limits()),
      pool_(&services.pool()),
      cache_(&services.pipelineCache()),
      archive_(&services.shaderArchive()),
      upperDevice_(&services.upperDevice()) {
  if (!device_) {
    return;
  }
  queue_ = device_.newCommandQueue(0);
  if (!queue_) {
    return;
  }
  queueView_ = WMT::CommandQueue{queue_.handle};

  initializer_ = std::make_unique<resources::Initializer>(*this, services.pool(), device_);

  // Bind queueLifecycle_ to our own state + a pool-based surface-compat
  // hook. CommandQueue is its own lifecycle root.
  bindSelfLifecycle([this](core::Handle h) -> std::uint32_t {
    if (!h) return 0;
    auto* surface = pool_->findSurface(h.value);
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
      device_, *limits_, *pool_, *cache_, allocators_,
      &archive_->reference(), &archive_->path(),
      *this, upperDevice_,
  };
}

CommandQueue::CommandQueue(WMT::Device device) : device_(device) {
  if (!device_) {
    return;
  }
  queue_ = device_.newCommandQueue(0);
  if (queue_) {
    queueView_ = WMT::CommandQueue{queue_.handle};
  }
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
  return queue_.commandBuffer();
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
  (void)q.queueLifecycle_.ensureWriterSlot(lock, kMaxInflight);
}

std::uint64_t seqIdForMark(CommandQueue& q, std::uint64_t seqId) {
  return seqId == 0 ? q.nextSeqId_ : seqId;
}

void markSlotResourcesUnlocked(CommandQueue& q, resources::Pool& pool,
                                const core::ChunkSlot& slot) {
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

}  // namespace

void CommandQueue::submitDraw(resources::Pool& pool, const core::DrawDesc& desc) {
  std::unique_lock lock(mutex_);
  // TLA+: WineCommit
  ensureWritingSlotUnlocked(*this, lock);
  currentSlotUnlocked(*this).commands.push_back(makeDrawCommand(desc));
  currentBackBuffer_ = desc.rts.color[0].handle;
  pool.markDrawResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitClear(resources::Pool& pool, const core::ClearDesc& desc) {
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  currentSlotUnlocked(*this).commands.push_back(makeClearCommand(desc));
  if (desc.colorAttachments[0].handle) {
    currentBackBuffer_ = desc.colorAttachments[0].handle;
  }
  pool.markClearResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitSurfaceCopy(resources::Pool& pool, const core::SurfaceCopyDesc& desc) {
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  currentSlotUnlocked(*this).commands.push_back(makeSurfaceCopyCommand(desc));
  pool.markSurfaceCopyResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitStretchRect(resources::Pool& pool, const core::StretchRectDesc& desc) {
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  currentSlotUnlocked(*this).commands.push_back(makeStretchRectCommand(desc));
  pool.markStretchResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitReadback(resources::Pool& pool, const core::ReadbackDesc& desc) {
  std::lock_guard lock(mutex_);
  // Readback is satisfied synchronously in dxmt9::Device::readbackSurface.
  // Still mark resources so NoUseAfterFree remains meaningful.
  pool.markReadbackResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitColorFill(resources::Pool& pool, const core::ColorFillDesc& desc) {
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  currentSlotUnlocked(*this).commands.push_back(makeColorFillCommand(desc));
  currentBackBuffer_ = desc.destination;
  pool.markColorFillResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitPresent(resources::Pool& pool, const core::SwapDesc& desc) {
  std::unique_lock lock(mutex_);
  queueLifecycle_.presentAndCommit(
      lock, kMaxInflight, desc, currentBackBuffer_,
      [this, &pool](const core::ChunkSlot& slot) {
        markSlotResourcesUnlocked(*this, pool, slot);
      });
}

void CommandQueue::submitFlush(resources::Pool& pool) {
  std::unique_lock lock(mutex_);
  queueLifecycle_.flushAndWait(
      lock, kMaxInflight, [this, &pool](const core::ChunkSlot& slot) {
        markSlotResourcesUnlocked(*this, pool, slot);
      });
}

core::HResult CommandQueue::waitForVBlank(resources::Pool& pool) {
  submitFlush(pool);
  return core::HResult{0};
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
      .lastCommittedSeqId = &lastCommittedSeqId_,
      .slots = std::span<core::ChunkSlot>(slots_.data(), slots_.size()),
      .mutex = &mutex_,
      .writeCv = &writeCv_,
      .encodeCv = &encodeCv_,
      .finishCv = &finishCv_,
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
          pool_->reclaimCompleted(completedSeqId_);
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
