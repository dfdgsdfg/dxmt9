#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "dxmt9/core.hpp"

namespace dxmt9::d3d9 {

struct RawCommandChunk {
  std::vector<dxmt9::core::u8> recordBlob;
  uint32_t recordCount = 0;
  uint32_t recordBytes = 0;
  bool hasPresent = false;
  bool skipDrawResourceMarking = false;
  std::vector<void*> retainedWrappers;
  std::chrono::steady_clock::time_point bridgeCommitStart{};
};

class ReplayOffloadQueue {
 public:
  ReplayOffloadQueue(std::size_t maxChunks, std::size_t maxBytes)
      : maxChunks_(maxChunks), maxBytes_(maxBytes) {}

  bool push(RawCommandChunk&& chunk) {
    std::unique_lock lock(mutex_);
    spaceCv_.wait(lock, [&] {
      return stop_ || (queue_.size() < maxChunks_ &&
                       queuedBytes_ + chunk.recordBytes <= maxBytes_);
    });
    if (stop_) return false;
    queuedBytes_ += chunk.recordBytes;
    queue_.push_back(std::move(chunk));
    workCv_.notify_one();
    return true;
  }

  bool pop(RawCommandChunk& out) {
    std::unique_lock lock(mutex_);
    workCv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
    if (queue_.empty()) return false;  // stop_ with empty queue
    out = std::move(queue_.front());
    queue_.pop_front();
    queuedBytes_ -= out.recordBytes;
    inFlight_ = true;
    spaceCv_.notify_all();
    return true;
  }

  void markReplayDone() {
    std::lock_guard lock(mutex_);
    inFlight_ = false;
    drainCv_.notify_all();
  }

  void waitDrained() {
    std::unique_lock lock(mutex_);
    drainCv_.wait(lock, [&] { return queue_.empty() && !inFlight_; });
  }

  void stop() {
    std::lock_guard lock(mutex_);
    stop_ = true;
    workCv_.notify_all();
    spaceCv_.notify_all();
    drainCv_.notify_all();
  }

  bool stopped() const {
    std::lock_guard lock(mutex_);
    return stop_;
  }

  std::size_t depth() const {
    std::lock_guard lock(mutex_);
    return queue_.size() + (inFlight_ ? 1 : 0);
  }

 private:
  const std::size_t maxChunks_;
  const std::size_t maxBytes_;
  mutable std::mutex mutex_;
  std::condition_variable workCv_;
  std::condition_variable spaceCv_;
  std::condition_variable drainCv_;
  std::deque<RawCommandChunk> queue_;
  std::size_t queuedBytes_ = 0;
  bool inFlight_ = false;
  bool stop_ = false;
};

}  // namespace dxmt9::d3d9
