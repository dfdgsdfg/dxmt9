#include "dxmt9/dxmt9_command_queue.hpp"

namespace dxmt9 {

CommandQueue::CommandQueue(WMT::Device device) : device_(device) {
  if (!device_) {
    return;
  }
  queue_ = device_.newCommandQueue(0);
  if (queue_) {
    queueView_ = WMT::CommandQueue{queue_.handle};
  }
}

WMT::Reference<WMT::CommandBuffer> CommandQueue::newCommandBuffer() {
  if (!queue_) {
    return {};
  }
  return queue_.commandBuffer();
}

}  // namespace dxmt9
