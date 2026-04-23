#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "backend_metal.hpp"

#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9_compat.hpp"
#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_ring_arena.hpp"
#include "dxmt9_shader_sources.hpp"

#include <mutex>
#include <span>
#include <utility>

namespace dxmt9 {

MetalBackendDevice::MetalBackendDevice(WMT::Reference<WMT::Device> device,
                                         const core::BackendLimits& limits,
                                         CommandQueue& queue,
                                         resources::Pool& pool,
                                         pipeline::Cache& cache,
                                         scratch::FrameAllocators& allocators,
                                         WMT::Reference<WMT::BinaryArchive>& shaderArchive,
                                         const std::string& shaderArchivePath,
                                         Device& upperDevice)
    : device_(std::move(device)),
      limits_(&limits),
      queue_(&queue),
      pool_(&pool),
      cache_(&cache),
      allocators_(&allocators),
      shaderArchive_(&shaderArchive),
      shaderArchivePath_(&shaderArchivePath),
      upperDevice_(&upperDevice) {
  if (!device_ || !queue_->valid()) {
    return;
  }

  queue_->queueLifecycle_.bindTrackedSubmissionState({
      .writingSlot = &queue_->writingSlot_,
      .writeIndex = &queue_->writeIndex_,
      .nextSeqId = &queue_->nextSeqId_,
      .readySlots = &queue_->readySlots_,
      .completedSeqQueue = &queue_->completedSeqQueue_,
      .inflightCount = &queue_->inflightCount_,
      .completedSeqId = &queue_->completedSeqId_,
      .lastCommittedSeqId = &queue_->lastCommittedSeqId_,
      .slots = std::span<core::ChunkSlot>(queue_->slots_.data(), queue_->slots_.size()),
      .mutex = &queue_->mutex_,
      .writeCv = &queue_->writeCv_,
      .encodeCv = &queue_->encodeCv_,
      .finishCv = &queue_->finishCv_,
      .stop = &queue_->stop_,
      .submissionDiagnostics = &queue_->submissionDiagnostics_,
      .resolveSurfaceFlags = [this](core::Handle handle) {
        return compatFlagsForSurface(handle);
      },
  });

  queue_->startThreads(
      [this] { encodeLoop(); },
      [this] { queue_->runFinishLoop(*pool_, *allocators_); },
      [this] { queue_->runCompletionWatcherLoop(); });
  valid_ = true;
}

MetalBackendDevice::~MetalBackendDevice() {
  if (!queue_) {
    return;
  }
  // Stop threads first — they reach into pool/cache/allocators which
  // DeviceImpl will destruct after *this.
  queue_->stopThreads();
  if (shaderArchive_ && *shaderArchive_) {
    std::lock_guard lock(queue_->mutex_);
    shaders::persistShaderArchive(*shaderArchive_, *shaderArchivePath_);
  }
}

void MetalBackendDevice::encodeLoop() {
  @autoreleasepool {
    while (true) {
      std::unique_lock lock(queue_->mutex_);
      if (!queue_->queueLifecycle_.runEncodeIteration(
              lock,
              [this](std::size_t slotIndex, const core::ChunkSlot& slot) {
                encoders::EncodeContext ctx{
                    device_, *limits_, *pool_, *cache_, *allocators_,
                    shaderArchive_, shaderArchivePath_, *queue_, upperDevice_,
                };
                return encoders::encodeChunk(ctx, slotIndex, slot);
              },
              [this](std::uint64_t) {
                allocators_->reclaim(queue_->completedSeqId_);
              })) {
        return;
      }
    }
  }
}

std::uint32_t MetalBackendDevice::compatFlagsForSurface(core::Handle handle) const {
  if (!handle) {
    return 0;
  }
  const auto* surface = pool_->findSurface(handle.value);
  if (!surface) {
    return 0;
  }
  return core::metalcompat::isFloatRenderTargetFormat(surface->desc.format)
             ? static_cast<std::uint32_t>(core::metalcompat::CompatFlagBits::CompatFlagFp16)
             : 0u;
}

}  // namespace dxmt9
