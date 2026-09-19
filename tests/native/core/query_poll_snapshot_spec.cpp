#include "core_spec_fixtures.hpp"

#include "../../../src/dxmt9/dxmt9_device.hpp"

#include <array>
#include <cstdint>
#include <memory>

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;
using namespace dxmt9::core::spec;

namespace {

class SnapshotUpper final : public dxmt9::Device {
 public:
  explicit SnapshotUpper(std::shared_ptr<BackendDevice> backend)
      : limits_{}, backend_(std::move(backend)),
        queue_(dxmt9::CommandQueue::ArenaLeaseTestQueueTag{}, {}) {}

  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  dxmt9::CommandQueue& queue() override { return queue_; }
  const BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<BackendDevice> backend() override { return backend_; }

  bool usesAsyncQueryFences() const noexcept override { return true; }
  dxmt9::query::EndReceipt publishQueryEnd() noexcept override {
    return {progress.queue, ++progress.published, progress.initializerPublished};
  }
  dxmt9::query::Poll pollQueryFence(const dxmt9::query::EndReceipt& receipt,
                             bool) noexcept override {
    if (onPoll) {
      auto callback = std::exchange(onPoll, nullptr);
      callback(pollContext);
    }
    return dxmt9::query::classifyEnd(receipt, progress);
  }
  dxmt9::query::Progress queryProgress() noexcept override { return progress; }

  BackendLimits limits_{};
  std::shared_ptr<BackendDevice> backend_;
  dxmt9::CommandQueue queue_;
  dxmt9::query::Progress progress{7, 0, 0, false};
  void (*onPoll)(void*) = nullptr;
  void* pollContext = nullptr;
};

PresentParameters testParams() {
  PresentParameters params{};
  params.backBufferWidth = 8;
  params.backBufferHeight = 8;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  return params;
}

void testFormatterParityAndShortBuffers() {
  u64 timestamp = 0xaaaaaaaaaaaaaaaaull;
  checkEq(formatQueryPollResult(QueryType::Timestamp, 0x1122334455667788ull,
                                &timestamp, sizeof(u32)),
          S_OK, "short timestamp still succeeds");
  checkEq(timestamp, 0xaaaaaaaaaaaaaaaaull, "short timestamp preserves bytes");

  u32 occlusion = 0xa5a5a5a5u;
  checkEq(formatQueryPollResult(QueryType::Occlusion, 0x1234ull, &occlusion,
                                sizeof(u16)),
          S_OK, "short occlusion still succeeds");
  checkEq(occlusion, 0xa5a5a5a5u, "short occlusion preserves bytes");

  checkEq(formatQueryPollResult(QueryType::Event, {}, nullptr, 0), S_OK,
          "event has no result bytes");
  u64 frequency = 0;
  checkEq(formatQueryPollResult(QueryType::TimestampFreq, {}, &frequency,
                                sizeof(frequency)),
          S_OK, "frequency formatter");
  checkEq(frequency, 1000000000ull, "frequency value");
  u32 disjoint = 1;
  checkEq(formatQueryPollResult(QueryType::TimestampDisjoint, {}, &disjoint,
                                sizeof(disjoint)),
          S_OK, "disjoint formatter");
  checkEq(disjoint, 0u, "disjoint value");
}

void testEligibilityAndIndependentOwner() {
  auto backend = std::make_shared<RecordingBackend>();
  Factory legacyFactory({}, backend);
  auto legacy = legacyFactory.createDevice(0, testParams());
  auto issued = legacy->createQuery(QueryType::Timestamp);
  checkEq(legacy->issueQuery(issued, false), D3D_OK, "legacy issue");
  const auto legacySnapshot = legacy->captureQueryPollSnapshot(issued);
  check(legacySnapshot.identity != 0, "snapshot identity");
  check(legacySnapshot.backend != nullptr, "snapshot retains backend");
  check(!legacySnapshot.eligible, "synchronous legacy snapshot is ineligible");
  u64 untouched = 0xdeadbeefcafef00dull;
  checkEq(legacy->pollQuerySnapshot(legacySnapshot, &untouched,
                                    sizeof(untouched), 0),
          D3DERR_NOTAVAILABLE, "legacy snapshot falls back");
  checkEq(untouched, 0xdeadbeefcafef00dull,
          "ineligible snapshot preserves output");

  auto upper = std::make_shared<SnapshotUpper>(std::make_shared<RecordingBackend>());
  Factory factory(upper);
  auto device = factory.createDevice(0, testParams());
  auto event = device->createQuery(QueryType::Event);
  const auto preIssue = device->captureQueryPollSnapshot(event);
  check(preIssue.identity != 0 && preIssue.eligible, "pre-issue snapshot eligible");
  checkEq(device->pollQuerySnapshot(preIssue, nullptr, 0, 0), S_OK,
          "pre-issue event snapshot");

  // A snapshot belongs to the core Device that captured it. This check also
  // prevents a retained upper backend from being used through another owner.
  Factory otherFactory(upper);
  auto other = otherFactory.createDevice(0, testParams());
  checkEq(other->pollQuerySnapshot(preIssue, nullptr, 0, 0), D3DERR_DEVICELOST,
          "foreign core owner fails closed");
}

void testPendingCompletionReissueLifetimeAndFailure() {
  auto upper = std::make_shared<SnapshotUpper>(std::make_shared<RecordingBackend>());
  Factory factory(upper);
  auto device = factory.createDevice(0, testParams());
  auto query = device->createQuery(QueryType::Timestamp);
  checkEq(device->issueQuery(query, false), D3D_OK, "async issue");
  auto snapshot = device->captureQueryPollSnapshot(query);
  check(snapshot.identity != 0 && snapshot.backend.get() == upper.get(),
        "snapshot captures the upper backend owner");
  check(snapshot.eligible && snapshot.backendFence.valid(),
        "published receipt is eligible");

  u64 value = 0x123456789abcdef0ull;
  checkEq(device->pollQuerySnapshot(snapshot, &value, sizeof(value), 0),
          S_FALSE, "pending snapshot poll");
  checkEq(value, 0x123456789abcdef0ull,
          "pending snapshot leaves output untouched");
  upper->progress.completed = upper->progress.published;
  checkEq(device->pollQuerySnapshot(snapshot, &value, sizeof(value), 0), S_OK,
          "completed snapshot poll");
  checkEq(value, 1ull, "snapshot copied resolved value");

  const auto oldReceipt = snapshot.backendFence.snapshot();
  checkEq(device->issueQuery(query, false), D3D_OK, "reissue query");
  const auto replacement = device->captureQueryPollSnapshot(query);
  check(replacement.issueOrdinal > snapshot.issueOrdinal,
        "reissue receives a new issue ordinal");
  checkEq(replacement.backendFence.snapshot().sequence, oldReceipt.sequence + 1,
          "reissue receives a new backend receipt");
  checkEq(device->pollQuerySnapshot(snapshot, nullptr, 0, 0), S_OK,
          "old immutable receipt remains complete");
  upper->progress.completed = oldReceipt.sequence;
  checkEq(device->pollQuerySnapshot(replacement, nullptr, 0, 0), S_FALSE,
          "replacement receipt remains pending");

  query.reset();
  checkEq(device->pollQuerySnapshot(snapshot, nullptr, 0, 0), S_OK,
          "snapshot outlives source Query");

  value = 0xfeedfaceu;
  device->setDeviceLost(true);
  checkEq(device->pollQuerySnapshot(snapshot, &value, sizeof(value), 0),
          D3DERR_DEVICELOST, "device loss fails closed");
  checkEq(value, 0xfeedfaceu, "device loss preserves output");
  device->setDeviceLost(false);
  upper->progress.failed = true;
  value = 0xfeedfaceu;
  checkEq(device->pollQuerySnapshot(snapshot, &value, sizeof(value), 0),
          D3DERR_DEVICELOST, "backend failure fails closed");
  checkEq(value, 0xfeedfaceu, "backend failure preserves output");
  upper->progress.failed = false;
  checkEq(device->reset(testParams()), D3D_OK, "reset after snapshot");
  value = 0xfeedfaceu;
  checkEq(device->pollQuerySnapshot(snapshot, &value, sizeof(value), 0),
          D3DERR_DEVICELOST, "stale epoch fails closed");
  checkEq(value, 0xfeedfaceu, "stale epoch preserves output");
}

void testConstantsAndDeferredMarkers() {
  auto upper = std::make_shared<SnapshotUpper>(std::make_shared<RecordingBackend>());
  Factory factory(upper);
  auto device = factory.createDevice(0, testParams());
  auto frequency = device->createQuery(QueryType::TimestampFreq);
  const auto snapshot = device->captureQueryPollSnapshot(frequency);
  check(snapshot.eligible && snapshot.constant(), "constant snapshot eligible");
  u64 value = 0;
  checkEq(device->pollQuerySnapshot(snapshot, &value, sizeof(value), 0), S_OK,
          "constant snapshot poll");
  checkEq(value, 1000000000ull, "constant snapshot bytes");

  auto query = device->createQuery(QueryType::Timestamp);
  auto marker = std::make_shared<dxmt9::query::EndMarker>(
      dxmt9::query::WriterIdentity{7, 1, 1, 1});
  // The test backend's receipt is eager; directly proving marker rejection is
  // still useful because eligibility is a snapshot-local policy.
  checkEq(device->issueQuery(query, false), D3D_OK, "marker test issue");
  auto deferred = device->captureQueryPollSnapshot(query);
  deferred.backendFence.marker = marker;
  deferred.eligible = false;
  value = 0xabcdef;
  checkEq(device->pollQuerySnapshot(deferred, &value, sizeof(value), 0),
          D3DERR_NOTAVAILABLE, "deferred marker falls back");
  checkEq(value, 0xabcdefull, "deferred marker preserves output");
}

void testControlChangesDuringPollAndInitializer() {
  auto upper = std::make_shared<SnapshotUpper>(std::make_shared<RecordingBackend>());
  Factory factory(upper);
  auto device = factory.createDevice(0, testParams());
  auto q = device->createQuery(QueryType::Timestamp);
  upper->progress.initializerPublished = 1;
  checkEq(device->issueQuery(q, false), D3D_OK, "initializer issue");
  const auto snapshot = device->captureQueryPollSnapshot(q);
  upper->progress.completed = upper->progress.published;
  u64 value = 0xdeadbeef;
  checkEq(device->pollQuerySnapshot(snapshot, &value, sizeof(value), 0), S_FALSE,
          "ordinary completion cannot bypass initializer");
  checkEq(value, 0xdeadbeefull, "initializer pending preserves bytes");
  upper->progress.initializerCompleted = 1;

  upper->pollContext = device.get();
  upper->onPoll = [](void* context) {
    static_cast<Device*>(context)->setDeviceLost(true);
  };
  checkEq(device->pollQuerySnapshot(snapshot, &value, sizeof(value), 0),
          D3DERR_DEVICELOST, "loss during backend poll fails final check");
  checkEq(value, 0xdeadbeefull, "concurrent loss preserves bytes");
  device->setDeviceLost(false);
  upper->onPoll = [](void* context) {
    // Deterministic interleaving: reset completes inside the backend poll,
    // after the snapshot entry check but before its final control check.
    static_cast<Device*>(context)->reset(testParams());
  };
  checkEq(device->pollQuerySnapshot(snapshot, &value, sizeof(value), 0),
          D3DERR_DEVICELOST, "reset during backend poll fails final check");
  checkEq(value, 0xdeadbeefull, "concurrent reset preserves bytes");
  checkEq(device->pollQuerySnapshot({}, &value, sizeof(value), 0),
          D3DERR_DEVICELOST, "missing snapshot fails closed");

  checkEq(device->issueQuery(q, true), D3D_OK, "BEGIN issue");
  const auto active = device->captureQueryPollSnapshot(q);
  checkEq(device->pollQuerySnapshot(active, &value, sizeof(value), 0), S_FALSE,
          "BEGIN cannot reuse a completed END");
  checkEq(value, 0xdeadbeefull, "BEGIN pending preserves bytes");
}

}  // namespace

int main() {
  try {
    testFormatterParityAndShortBuffers();
    testEligibilityAndIndependentOwner();
    testPendingCompletionReissueLifetimeAndFailure();
    testConstantsAndDeferredMarkers();
    testControlChangesDuringPollAndInitializer();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
