#pragma once

// Resource upload service. Encapsulates the CPU→GPU staging path for
// texture level uploads: acquires the queue mutex, emits an optional
// debug trace, delegates the actual staging blit to Pool::uploadTextureLevel,
// and drives the gpu-dump sidechannel when enabled.
//
// Previously dxmt9::transfers::uploadTextureLevel. Lives here so the
// "upload" responsibility has a named owner rather than a misc bucket.

#include "../winemetal/Metal.hpp"
#include "dxmt9/core.hpp"

#include <cstdint>
#include <span>

namespace dxmt9 {

class CommandQueue;

namespace resources {

struct Pool;

class Initializer {
 public:
  Initializer(CommandQueue& queue, Pool& pool, WMT::Reference<WMT::Device> device);
  Initializer(const Initializer&) = delete;
  Initializer& operator=(const Initializer&) = delete;

  // Upload CPU-visible bytes into a texture mip level. Hot path: emits
  // a debug trace if DXMT_TRACE_TEXTURE_HANDLE matches, calls Pool's
  // staging path, and — for level 0 with DXMT_GPU_DUMP_TEXTURE_HANDLE
  // set — snapshots the result to disk.
  void uploadTextureLevel(core::TextureHandle handle,
                           std::uint32_t level,
                           std::uint32_t width,
                           std::uint32_t height,
                           std::uint32_t pitch,
                           std::span<const std::uint8_t> bytes);

 private:
  CommandQueue* queue_;
  Pool* pool_;
  WMT::Reference<WMT::Device> device_;
};

}  // namespace resources
}  // namespace dxmt9
