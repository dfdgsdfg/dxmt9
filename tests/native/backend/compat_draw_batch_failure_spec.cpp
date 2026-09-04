// Compatibility draw-run publication must fail closed. The fake upper device
// injects the two failures a real queue can expose at this boundary: an
// explicit rejection and an allocation exception. Neither may turn a Clear
// or Present into a successful command after the pending draws were retired.

#include "dxmt9/core_snapshots.hpp"
#include "dxmt9/dxmt9_device.hpp"

#include <cstdio>
#include <memory>
#include <new>
#include <stdexcept>

namespace {

using namespace dxmt9::core;

int failures = 0;

void check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "FAIL: %s\n", message);
  ++failures;
}

struct FailingUpper final : dxmt9::Device {
  enum class BatchFailure { None, Reject, Allocate };

  FailingUpper()
      : queue_(WMT::Device{NULL_OBJECT_HANDLE}, limits_, false) {}

  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  dxmt9::CommandQueue& queue() override { return queue_; }
  const BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<BackendDevice> backend() override { return {}; }

  bool submitDrawRunBatch(
      std::span<const DrawRunBatchEntry> entries) override {
    ++batchCalls;
    if (failure == BatchFailure::Allocate) {
      throw std::bad_alloc{};
    }
    if (failure == BatchFailure::Reject) {
      return false;
    }
    submittedDraws += entries.empty() ? 0u : entries.front().draws.size();
    return true;
  }

  void submitClear(const ClearDesc&) override { ++clearCalls; }
  void present(const SwapDesc&) override { ++presentCalls; }

  BackendLimits limits_{};
  dxmt9::CommandQueue queue_;
  BatchFailure failure = BatchFailure::None;
  std::size_t batchCalls = 0;
  std::size_t submittedDraws = 0;
  std::size_t clearCalls = 0;
  std::size_t presentCalls = 0;
};

std::shared_ptr<Device> makeDevice(
    const std::shared_ptr<FailingUpper>& upper) {
  return std::make_shared<Device>(AdapterInfo{}, BackendLimits{},
                                  PresentParameters{}, 0u, upper);
}

DrawParam draw() {
  DrawParam value{};
  value.primitiveType = PrimitiveType::TriangleList;
  value.primitiveCount = 1u;
  return value;
}

void rejectedPublicationFailsClear() {
  auto upper = std::make_shared<FailingUpper>();
  auto device = makeDevice(upper);
  check(device->submitCompatibilityReplayDrawBatched(draw()).result == D3D_OK,
        "a draw is accepted into the pending compatibility batch");
  upper->failure = FailingUpper::BatchFailure::Reject;

  check(device->clear(ClearDesc{}) == D3DERR_DEVICELOST,
        "a rejected batch makes Clear fail-stop");
  check(upper->clearCalls == 0u,
        "Clear is not submitted after its preceding batch is rejected");
  check(device->testCooperativeLevel() == D3DERR_DEVICELOST,
        "a rejected batch poisons the core device");
  check(!device->compatibilityReplayDrawBatchPending(),
        "a rejected batch is retired exactly once");
}

void allocationPublicationFailsPresent() {
  auto upper = std::make_shared<FailingUpper>();
  auto device = makeDevice(upper);
  check(device->submitCompatibilityReplayDrawBatched(draw()).result == D3D_OK,
        "a draw is accepted before the allocation fault");
  upper->failure = FailingUpper::BatchFailure::Allocate;

  check(device->present() == D3DERR_DEVICELOST,
        "an allocation exception makes Present fail-stop");
  check(upper->presentCalls == 0u,
        "Present is not submitted after batch publication throws");
  check(upper->batchCalls == 1u,
        "the pending batch has one publication attempt");
}

}  // namespace

int main() {
  rejectedPublicationFailsClear();
  allocationPublicationFailsPresent();
  if (failures != 0) {
    std::fprintf(stderr, "%d compatibility batch failure checks failed\n",
                 failures);
    return 1;
  }
  std::puts("compat_draw_batch_failure_spec passed");
  return 0;
}
