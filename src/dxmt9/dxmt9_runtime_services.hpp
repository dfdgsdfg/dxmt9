#pragma once

// Narrow live service facade between DeviceImpl and its downstream
// runtime nodes (CommandQueue today; potentially more tomorrow).
// DeviceImpl owns a RuntimeServices and passes it by reference to
// CommandQueue; the queue reads the services it needs through getters
// and stashes pointers internally for its hot paths.
//
// This replaces the old CommandQueueDeps POD bundle. The runtime shape
// is the same — CommandQueue still captures raw pointers at ctor — but
// the interface boundary is named and reusable, matching upstream
// dxmt's service-facade pattern.

#include "dxmt9/core.hpp"

namespace dxmt9 {

class Device;
namespace resources { struct Pool; }
namespace pipeline { class Cache; }
namespace shaders { class Archive; }

class RuntimeServices {
 public:
  RuntimeServices(resources::Pool& pool,
                   pipeline::Cache& cache,
                   shaders::Archive& archive,
                   Device& upperDevice,
                   const core::BackendLimits& limits) noexcept
      : pool_(&pool),
        cache_(&cache),
        archive_(&archive),
        upperDevice_(&upperDevice),
        limits_(&limits) {}

  resources::Pool& pool() const noexcept { return *pool_; }
  pipeline::Cache& pipelineCache() const noexcept { return *cache_; }
  shaders::Archive& shaderArchive() const noexcept { return *archive_; }
  Device& upperDevice() const noexcept { return *upperDevice_; }
  const core::BackendLimits& limits() const noexcept { return *limits_; }

 private:
  resources::Pool* pool_;
  pipeline::Cache* cache_;
  shaders::Archive* archive_;
  Device* upperDevice_;
  const core::BackendLimits* limits_;
};

}  // namespace dxmt9
