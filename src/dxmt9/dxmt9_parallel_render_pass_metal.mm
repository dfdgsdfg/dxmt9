#include "dxmt9_parallel_render_pass_metal.hpp"

#include "dxmt9_perf_counters.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <dispatch/dispatch.h>

namespace dxmt9::encoders {

ParallelPassMetalBackend::ParallelPassMetalBackend(
    WMT::CommandBuffer commandBuffer,
    const WMTRenderPassInfo& passInfo,
    ParallelPassMetalCallbacks callbacks) noexcept
    : commandBuffer_(commandBuffer),
      passInfo_(passInfo),
      callbacks_(callbacks) {}

bool ParallelPassMetalBackend::prepareParent() noexcept {
  prepared_ = commandBuffer_ && callbacks_.emitChild;
  preparedChildCount_ = 0u;
  parentEnded_ = false;
  childEnded_.fill(false);
  return prepared_;
}

bool ParallelPassMetalBackend::createChild(
    const ParallelPassChildPlan& child) noexcept {
  if (!prepared_ || child.childOrdinal != preparedChildCount_ ||
      preparedChildCount_ >= children_.size()) {
    return false;
  }
  ++preparedChildCount_;
  return true;
}

void ParallelPassMetalBackend::abandonPrepared() noexcept {
  prepared_ = false;
  preparedChildCount_ = 0u;
}

bool ParallelPassMetalBackend::createMetalEncoders() noexcept {
  if (!prepared_ || preparedChildCount_ < 2u || parent_) {
    return false;
  }
  parent_ = WMT::Reference<WMT::ParallelRenderCommandEncoder>(
      commandBuffer_.parallelRenderCommandEncoder(passInfo_));
  if (!parent_) {
    return false;
  }
  for (std::uint32_t i = 0; i < preparedChildCount_; ++i) {
    children_[i] = WMT::Reference<WMT::RenderCommandEncoder>(
        parent_.renderCommandEncoder());
    if (!children_[i]) {
      return false;
    }
  }
  return true;
}

bool ParallelPassMetalBackend::beginPassActions() noexcept {
  if (!createMetalEncoders()) {
    return false;
  }
  return !callbacks_.beginPassActions ||
      callbacks_.beginPassActions(callbacks_.context);
}

bool ParallelPassMetalBackend::replayLogicalCommands(
    std::span<const ParallelPassChildPlan> children) noexcept {
  return !callbacks_.replayLogicalCommands ||
      callbacks_.replayLogicalCommands(callbacks_.context, children);
}

bool ParallelPassMetalBackend::emitChild(
    const ParallelPassChildPlan& child) noexcept {
  if (child.childOrdinal >= preparedChildCount_ ||
      !children_[child.childOrdinal] || childEnded_[child.childOrdinal]) {
    return false;
  }
  return callbacks_.emitChild(callbacks_.context, child,
                              children_[child.childOrdinal]);
}

std::uint32_t ParallelPassMetalBackend::emitChildren(
    std::span<const ParallelPassChildPlan> children) noexcept {
  if (children.size() != preparedChildCount_ || children.empty()) {
    return 0u;
  }
  // The executor is persistent and the submitted task set is statically
  // bounded by kParallelRenderPassChildCapacity. dispatch_apply_f joins every
  // task before returning, so callback context and child-plan spans remain
  // borrowed strictly within executeParallelRenderPass().
  static dispatch_queue_t workerQueue = dispatch_queue_create(
      "org.dxmt9.parallel-render-pass", DISPATCH_QUEUE_CONCURRENT);
  struct Batch {
    ParallelPassMetalBackend* backend = nullptr;
    std::span<const ParallelPassChildPlan> children{};
    std::atomic<std::uint32_t> failedChild{
        kParallelRenderPassNoFailedChild};
  } batch{
      .backend = this,
      .children = children,
  };
  const bool collectPerf = perf::enabled();
  const auto batchStarted = collectPerf
      ? std::chrono::steady_clock::now()
      : std::chrono::steady_clock::time_point{};
  if (collectPerf) {
    perf::countParallelPassWorkerBatch(
        static_cast<std::uint32_t>(children.size()));
  }
  dispatch_apply_f(
      children.size(), workerQueue, &batch,
      [](void* raw, std::size_t index) {
        @autoreleasepool {
          auto& work = *static_cast<Batch*>(raw);
          const auto& child = work.children[index];
          const bool collectTaskPerf = perf::enabled();
          const auto taskStarted = collectTaskPerf
              ? std::chrono::steady_clock::now()
              : std::chrono::steady_clock::time_point{};
          if (collectTaskPerf) {
            perf::countParallelPassWorkerTaskBegin();
          }
          const bool emitted = work.backend->emitChild(child);
          if (collectTaskPerf) {
            perf::countParallelPassWorkerTaskEnd(
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - taskStarted)
                        .count()));
          }
          if (!emitted) {
            std::uint32_t observed = work.failedChild.load(
                std::memory_order_relaxed);
            while (child.childOrdinal < observed &&
                   !work.failedChild.compare_exchange_weak(
                       observed, child.childOrdinal,
                       std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
            }
          }
        }
      });
  if (collectPerf) {
    perf::countParallelPassWorkerWallTime(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - batchStarted)
                .count()));
  }
  return batch.failedChild.load(std::memory_order_relaxed);
}

bool ParallelPassMetalBackend::endChild(std::uint32_t ordinal) noexcept {
  if (ordinal >= preparedChildCount_ || !children_[ordinal] ||
      childEnded_[ordinal]) {
    return false;
  }
  children_[ordinal].endEncoding();
  childEnded_[ordinal] = true;
  return true;
}

bool ParallelPassMetalBackend::joinChild(std::uint32_t ordinal) noexcept {
  if (ordinal >= preparedChildCount_ || !childEnded_[ordinal]) {
    return false;
  }
  return !callbacks_.joinChild ||
      callbacks_.joinChild(callbacks_.context, ordinal);
}

bool ParallelPassMetalBackend::endPassActions() noexcept {
  return !callbacks_.endPassActions ||
      callbacks_.endPassActions(callbacks_.context);
}

bool ParallelPassMetalBackend::endParent() noexcept {
  if (!parent_ || parentEnded_) {
    return false;
  }
  for (std::uint32_t i = 0; i < preparedChildCount_; ++i) {
    if (!childEnded_[i]) {
      return false;
    }
  }
  parent_.endEncoding();
  parentEnded_ = true;
  return true;
}

bool ParallelPassMetalBackend::publishSidecars() noexcept {
  return !callbacks_.publishSidecars ||
      callbacks_.publishSidecars(callbacks_.context);
}

bool ParallelPassMetalBackend::publishCompletion() noexcept {
  return !callbacks_.publishCompletion ||
      callbacks_.publishCompletion(callbacks_.context);
}

void ParallelPassMetalBackend::closeMetalEncoders() noexcept {
  for (std::uint32_t i = 0; i < preparedChildCount_; ++i) {
    if (children_[i] && !childEnded_[i]) {
      children_[i].endEncoding();
      childEnded_[i] = true;
    }
  }
  if (parent_ && !parentEnded_) {
    parent_.endEncoding();
    parentEnded_ = true;
  }
}

void ParallelPassMetalBackend::failStop(
    ParallelPassFailurePhase phase, std::uint32_t child) noexcept {
  closeMetalEncoders();
  if (callbacks_.failStop) {
    callbacks_.failStop(callbacks_.context, phase, child);
  }
}

}  // namespace dxmt9::encoders
