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
      upperDevice_(&upperDevice) {}

std::optional<core::metalqueue::QueueSubmissionRecord>
MetalBackendDevice::encodeChunk(std::size_t slotIndex, const core::ChunkSlot& slot) {
  @autoreleasepool {
    encoders::EncodeContext ctx{
        device_, *limits_, *pool_, *cache_, *allocators_,
        shaderArchive_, shaderArchivePath_, *queue_, upperDevice_,
    };
    return encoders::encodeChunk(ctx, slotIndex, slot);
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
