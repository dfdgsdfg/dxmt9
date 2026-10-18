#include "gpu_source.hpp"
#include "once_sample.hpp"
#include "../../winemetal/Metal.hpp"

#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <memory>
#include <atomic>
#include <new>

using EndpointCell = dxmt9::observations::OnceSample<dxmt9::observations::GpuEndpoints>;

namespace dxmt9::observations {
namespace {
struct SourceStorage {
  EndpointCell commandBuffer;
  std::unique_ptr<EndpointCell[]> pairs;
  NSUInteger pairCount = 0;
};
using SourceBlock = SourceStorage* (^)();
char sourceKey;
std::atomic<bool> sourcesInstalled{false};
constexpr NSUInteger maxPairs = 4096;

SourceStorage* source(id owner, bool counterBuffer) noexcept {
  // Install only for an explicitly admitted trace. A copied Objective-C block
  // owns the C++ storage; no process-global Objective-C class is registered by
  // this private static library (native fixtures may link another copy).
  @synchronized(owner) {
    auto block = (SourceBlock)objc_getAssociatedObject(owner, &sourceKey);
    if (block) return block();
    try {
      auto value = std::make_shared<SourceStorage>();
      if (counterBuffer) {
        const auto samples = [(id<MTLCounterSampleBuffer>)owner sampleCount];
        if (samples / 2 > maxPairs) return nullptr;
        value->pairCount = samples / 2;
        value->pairs.reset(new (std::nothrow) EndpointCell[value->pairCount]);
        if (!value->pairs) return nullptr;
      }
      SourceBlock newBlock = ^SourceStorage* { return value.get(); };
      objc_setAssociatedObject(owner, &sourceKey, newBlock, OBJC_ASSOCIATION_COPY_NONATOMIC);
      sourcesInstalled.store(true, std::memory_order_release);
      return value.get();
    } catch (const std::bad_alloc&) {
      return nullptr;
    }
  }
}
SourceStorage* existing(obj_handle_t handle) noexcept {
  if (!handle || !sourcesInstalled.load(std::memory_order_acquire)) return nullptr;
  auto block = (SourceBlock)objc_getAssociatedObject((id)handle, &sourceKey);
  return block ? block() : nullptr;
}
}  // namespace

bool prepareCommandBufferSource(obj_handle_t handle) noexcept {
  return existing(handle) || (handle && source((id)handle, false));
}
bool prepareCounterSource(obj_handle_t handle, std::uint32_t firstIndex) noexcept {
  if (!handle || (firstIndex & 1u)) return false;
  auto* storage = existing(handle);
  if (!storage) storage = source((id)handle, true);
  return storage && firstIndex / 2 < storage->pairCount;
}
bool hasCounterSource(obj_handle_t handle) noexcept {
  return existing(handle) != nullptr;
}

GpuEndpoints commandBufferEndpoints(obj_handle_t handle) noexcept {
  if (!handle) return {};
  auto acquire = [&] {
    WMT::CommandBuffer buffer{handle};
    return GpuEndpoints{buffer.gpuStartTime(), buffer.gpuEndTime()};
  };
  auto* storage = existing(handle);
  return storage ? storage->commandBuffer.read(acquire) : acquire();
}

GpuEndpoints counterPairEndpoints(obj_handle_t handle,
                                   std::uint32_t firstIndex) noexcept {
  if (!handle || (firstIndex & 1u)) return {};
  auto* storage = existing(handle);
  if (!storage || firstIndex / 2 >= storage->pairCount) return {};
  return storage->pairs[firstIndex / 2].read([&] {
    std::uint64_t values[2]{};
    WMT::CounterSampleBuffer buffer{handle};
    buffer.resolveCounterRange(firstIndex, 2, values, sizeof(values));
    return GpuEndpoints{values[0], values[1]};
  });
}
}  // namespace dxmt9::observations
