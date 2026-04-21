#include "dxmt9/dxmt9_command_queue.hpp"

#include <utility>

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

void CommandQueue::startThreads(std::function<void()> encodeLoop,
                                 std::function<void()> finishLoop,
                                 std::function<void()> completionLoop) {
  if (threadsStarted_) {
    return;
  }
  stop_ = false;
  encodeThread_ = std::thread(std::move(encodeLoop));
  finishThread_ = std::thread(std::move(finishLoop));
  completionThread_ = std::thread(std::move(completionLoop));
  threadsStarted_ = true;
}

void CommandQueue::stopThreads() {
  if (!threadsStarted_) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    encodeCv_.notify_all();
    finishCv_.notify_all();
    writeCv_.notify_all();
  }
  queueLifecycle_.notifyPendingCompletionStop();
  if (encodeThread_.joinable()) encodeThread_.join();
  if (completionThread_.joinable()) completionThread_.join();
  if (finishThread_.joinable()) finishThread_.join();
  threadsStarted_ = false;
}

}  // namespace dxmt9
