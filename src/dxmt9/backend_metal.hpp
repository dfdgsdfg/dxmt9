#pragma once

// Thin Metal-specific glue owned by DeviceImpl. Hosts the encode-thread
// main loop (with @autoreleasepool) and the compatFlagsForSurface hook
// bound into queueLifecycle_. Does NOT inherit BackendDevice — that
// abstract interface is reserved for test mocks.
//
// Construction contract: the caller (DeviceImpl) must outlive this
// instance. All references/pointers captured here point into DeviceImpl
// members; destructing MetalBackendDevice joins the worker threads
// before DeviceImpl's siblings tear down.

#include "../winemetal/Metal.hpp"
#include "dxmt9/core.hpp"

#include <cstdint>
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
  ~MetalBackendDevice();
  MetalBackendDevice(const MetalBackendDevice&) = delete;
  MetalBackendDevice& operator=(const MetalBackendDevice&) = delete;

  bool valid() const noexcept { return valid_; }

 private:
  void encodeLoop();
  std::uint32_t compatFlagsForSurface(core::Handle handle) const;

  WMT::Reference<WMT::Device> device_;
  const core::BackendLimits* limits_ = nullptr;
  CommandQueue* queue_ = nullptr;
  resources::Pool* pool_ = nullptr;
  pipeline::Cache* cache_ = nullptr;
  scratch::FrameAllocators* allocators_ = nullptr;
  WMT::Reference<WMT::BinaryArchive>* shaderArchive_ = nullptr;
  const std::string* shaderArchivePath_ = nullptr;
  Device* upperDevice_ = nullptr;
  bool valid_ = false;
};

}  // namespace dxmt9
