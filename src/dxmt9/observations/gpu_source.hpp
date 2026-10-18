#pragma once

#include "../../winemetal/winemetal.h"
#include <cstdint>

namespace dxmt9::observations {

struct GpuEndpoints {
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
  bool valid() const noexcept { return begin != 0 && end > begin; }
};

// Prepare only when a trace consumer subscribes, before commit. Legacy-only
// collection keeps its existing direct/bulk path without allocating a cache.
bool prepareCommandBufferSource(obj_handle_t commandBuffer) noexcept;
bool prepareCounterSource(obj_handle_t buffer,
                          std::uint32_t firstIndex = 0) noexcept;
bool hasCounterSource(obj_handle_t buffer) noexcept;

// The caller retains the object and proves successful GPU completion. Source
// cells are associated with that exact Metal object, never a recycled address.
GpuEndpoints commandBufferEndpoints(obj_handle_t commandBuffer) noexcept;
GpuEndpoints counterPairEndpoints(obj_handle_t buffer,
                                   std::uint32_t firstIndex) noexcept;
}  // namespace dxmt9::observations
