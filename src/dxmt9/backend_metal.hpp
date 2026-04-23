#pragma once

// Very thin encode glue. Owned by DeviceImpl. Provides:
//   - encodeChunk callback (wraps @autoreleasepool + builds EncodeContext
//     + forwards to dxmt9::encoders::encodeChunk)
//   - compatFlagsForSurface hook for the queueLifecycle binding
// Does NOT inherit BackendDevice; that abstract interface is test-only.
// Does NOT own threads, queueLifecycle binding, or shader-archive persist —
// those moved to CommandQueue and DeviceImpl.

#include "../winemetal/Metal.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9_backend_types.hpp"
#include "dxmt9_queue.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace dxmt9 {

class CommandQueue;
class Device;

namespace resources { struct Pool; }
namespace pipeline { class Cache; }
namespace scratch { struct FrameAllocators; }

class MetalBackendDevice {
 public:
  MetalBackendDevice(WMT::Reference<WMT::Device> device,
                      const core::BackendLimits& limits,
                      CommandQueue& queue,
                      resources::Pool& pool,
                      pipeline::Cache& cache,
                      scratch::FrameAllocators& allocators,
                      WMT::Reference<WMT::BinaryArchive>& shaderArchive,
                      const std::string& shaderArchivePath,
                      Device& upperDevice);
  MetalBackendDevice(const MetalBackendDevice&) = delete;
  MetalBackendDevice& operator=(const MetalBackendDevice&) = delete;

  // Per-chunk encoder callback — wraps @autoreleasepool and calls
  // dxmt9::encoders::encodeChunk with an EncodeContext assembled from
  // our borrowed references. Supplied to CommandQueue::runEncodeLoop.
  std::optional<core::metalqueue::QueueSubmissionRecord>
  encodeChunk(std::size_t slotIndex, const core::ChunkSlot& slot);

  // Surface-format compat hook. Supplied to CommandQueue::bindSelfLifecycle
  // as the queueLifecycle_.resolveSurfaceFlags callback.
  std::uint32_t compatFlagsForSurface(core::Handle handle) const;

 private:
  WMT::Reference<WMT::Device> device_;
  const core::BackendLimits* limits_ = nullptr;
  CommandQueue* queue_ = nullptr;
  resources::Pool* pool_ = nullptr;
  pipeline::Cache* cache_ = nullptr;
  scratch::FrameAllocators* allocators_ = nullptr;
  WMT::Reference<WMT::BinaryArchive>* shaderArchive_ = nullptr;
  const std::string* shaderArchivePath_ = nullptr;
  Device* upperDevice_ = nullptr;
};

}  // namespace dxmt9
