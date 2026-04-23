#pragma once

// Synchronous CPU-GPU transfer paths. Previously methods on
// MetalBackendDevice; extracted here (Step 3e) so DeviceImpl can call
// them directly without polymorphic indirection through BackendDevice.

#include "../winemetal/Metal.hpp"
#include "dxmt9/core.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace dxmt9 {

class CommandQueue;
namespace resources { struct Pool; }

namespace transfers {

// Map a buffer for CPU write. If the buffer's last-used seq hasn't been
// completed on the GPU, waits (under the queue mutex). Returns a pointer
// to the shadow storage or nullptr if the handle is unknown.
void* mapBuffer(CommandQueue& queue,
                 resources::Pool& pool,
                 core::BufferHandle handle,
                 std::uint32_t flags);

// Upload a texture level. Emits a debug trace if shouldTraceTexture(handle)
// and optionally dumps the resulting GPU texture (env-gated via
// shouldDumpGpuTexture).
void uploadTextureLevel(CommandQueue& queue,
                         resources::Pool& pool,
                         WMT::Reference<WMT::Device> device,
                         core::TextureHandle handle,
                         std::uint32_t level,
                         std::uint32_t width,
                         std::uint32_t height,
                         std::uint32_t pitch,
                         std::span<const std::uint8_t> bytes);

// Readback a surface region into CPU-visible pixels. Uses a two-step
// blit (source → staging texture → staging buffer, both synchronously
// waited). Returns false if the source is invalid.
bool readbackSurface(CommandQueue& queue,
                      resources::Pool& pool,
                      WMT::Reference<WMT::Device> device,
                      const core::BackendLimits& limits,
                      const core::ReadbackDesc& desc,
                      core::ReadbackPixels& pixels);

}  // namespace transfers
}  // namespace dxmt9
