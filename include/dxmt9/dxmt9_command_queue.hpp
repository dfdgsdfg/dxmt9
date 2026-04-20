#pragma once

// Upper-runtime CommandQueue — owns the WMT::CommandQueue handle and exposes
// the minimal API that upper consumers (dxmt9::Device, backend, swap chain)
// need. Mirrors dxmt's class CommandQueue (dxmt/src/dxmt/dxmt_command_queue.hpp)
// in PUBLIC SHAPE; the full chunk-ring + thread orchestration still lives
// inside MetalBackendDevice for now and reaches through this object for
// handle access.

#include "../../src/winemetal/Metal.hpp"

namespace dxmt9 {

class CommandQueue {
 public:
  // Construct by creating a new WMT::CommandQueue on the given device.
  explicit CommandQueue(WMT::Device device);
  CommandQueue(const CommandQueue&) = delete;
  CommandQueue& operator=(const CommandQueue&) = delete;

  // True if a WMT::CommandQueue was successfully allocated.
  bool valid() const noexcept { return static_cast<bool>(queue_); }

  // Access the underlying WMT::CommandQueue. Callers that need to issue
  // command buffers use newCommandBuffer() below.
  WMT::CommandQueue& raw() noexcept { return queueView_; }
  const WMT::CommandQueue& raw() const noexcept { return queueView_; }

  // Issue a new command buffer from the queue. Returns an owning reference
  // (released via RAII when the caller's Reference goes out of scope or is
  // moved into a CommandChunk).
  WMT::Reference<WMT::CommandBuffer> newCommandBuffer();

  // The WMT::Device this queue was created on.
  WMT::Device device() const noexcept { return device_; }

 private:
  WMT::Device device_{};
  WMT::Reference<WMT::CommandQueue> queue_{};
  // Non-owning view exposed via raw() — kept in sync with queue_.
  WMT::CommandQueue queueView_{};
};

}  // namespace dxmt9
