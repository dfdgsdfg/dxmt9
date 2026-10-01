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
  bool usesMetalOcclusion() const noexcept override { return gpuOcclusion; }
  bool setOcclusionScope(dxmt9::query::OcclusionScopeRef next) noexcept override {
    scope = std::move(next);
    return true;
  }
  dxmt9::query::EndReceipt publishQueryEnd() noexcept override {
    if (deferredMarker) {
      dxmt9::query::EndReceipt receipt{progress.queue, 0,
                                      progress.initializerPublished};
      receipt.marker = deferredMarker;
      return receipt;
    }
    return {progress.queue, ++progress.published, progress.initializerPublished};
  }
  dxmt9::query::Poll pollQueryFence(const dxmt9::query::EndReceipt& receipt,
                             bool flush) noexcept override {
    ++polls;
    flushPolls += flush;
    if (onPoll) {
      auto callback = std::exchange(onPoll, nullptr);
      callback(pollContext);
    }
    lastPoll = dxmt9::query::classifyEnd(receipt, progress);
    if (afterPoll) {
      auto callback = std::exchange(afterPoll, nullptr);
      callback(pollContext);
    }
    return lastPoll;
  }
  dxmt9::query::Progress queryProgress() noexcept override { return progress; }

  BackendLimits limits_{};
  std::shared_ptr<BackendDevice> backend_;
  dxmt9::CommandQueue queue_;
  dxmt9::query::Progress progress{7, 0, 0, false};
  std::shared_ptr<dxmt9::query::EndMarker> deferredMarker;
  bool gpuOcclusion = false;
  dxmt9::query::OcclusionScopeRef scope;
  u64 polls = 0;
  u64 flushPolls = 0;
  dxmt9::query::Poll lastPoll = dxmt9::query::Poll::Invalid;
  void (*onPoll)(void*) = nullptr;
  void (*afterPoll)(void*) = nullptr;
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
  check(device->queryPollSnapshotCurrent(snapshot),
        "successful poll retains current capture control epoch");
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
  check(!device->queryPollSnapshotCurrent(snapshot),
        "Reset after S_OK but before final capture check rejects old epoch");
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

void testPublishedReceiptPromotionTruthTable() {
  using dxmt9::query::Poll;
  for (bool valid : {false, true})
    for (bool marker : {false, true})
      for (u64 sequence : {0ull, 1ull, 2ull})
        for (bool cancelled : {false, true})
          for (auto poll : {Poll::Invalid, Poll::Failed, Poll::Pending,
                            Poll::Complete}) {
            check(queryPollPublishedReceiptAdmitted(valid, marker, sequence,
                                                    cancelled, poll) ==
                (valid && marker && sequence != 0 && !cancelled &&
                 (poll == Poll::Pending || poll == Poll::Complete)),
                "published promotion requires actual bound accepted receipt");
          }
}

void testPublishedSnapshotSurvivesUnpublishedReissueAndDestroy() {
  using dxmt9::query::EndMarker;
  using dxmt9::query::WriterIdentity;
  auto upper = std::make_shared<SnapshotUpper>(std::make_shared<RecordingBackend>());
  upper->gpuOcclusion = true;
  upper->progress.initializerPublished = 1;
  const WriterIdentity oldWriter{7, 1, 1, 1};
  auto marker = std::make_shared<EndMarker>(oldWriter);
  upper->deferredMarker = marker;
  Factory factory(upper);
  auto device = factory.createDevice(0, testParams());
  auto query = device->createQuery(QueryType::Occlusion);
  checkEq(device->issueQuery(query, true), D3D_OK, "retained issue BEGIN");
  checkEq(device->issueQuery(query, false), D3D_OK, "retained issue END");
  auto old = device->captureQueryPollSnapshot(query);
  check(!old.eligible && !old.publishedOnly && old.deferredBackendFence(),
        "default capture retains writer fallback");
  check(old.occlusionResult != nullptr, "snapshot retains original result");
  old.occlusionResult->samples.store(42, std::memory_order_release);
  old.occlusionResult->pending.store(1, std::memory_order_release);
  check(!device->promotePublishedQueryPollSnapshot(old),
        "unbound writer cannot be promoted");
  checkEq(upper->polls, 0ull, "unbound promotion never polls backend");
  u32 value = 0xdeadbeef;
  checkEq(device->pollQuerySnapshot(old, &value, sizeof(value),
                                    QUERY_GETDATA_FLUSH),
          D3DERR_NOTAVAILABLE, "default unbound FLUSH falls back");
  checkEq(upper->flushPolls, 0ull, "fallback cannot publish unbound writer");
  checkEq(value, 0xdeadbeefu, "fallback preserves output");

  upper->deferredMarker = std::make_shared<EndMarker>(WriterIdentity{7, 2, 2, 2});
  checkEq(device->issueQuery(query, true), D3D_OK, "replacement BEGIN");
  checkEq(device->issueQuery(query, false), D3D_OK, "replacement END");
  const auto replacement = device->captureQueryPollSnapshot(query);
  check(replacement.occlusionResult != old.occlusionResult &&
        replacement.issueOrdinal > old.issueOrdinal,
        "reissue has distinct result and ordinal");
  replacement.occlusionResult->samples.store(99, std::memory_order_release);
  query.reset();
  check(replacement.occlusionResult->abandoned.load(std::memory_order_acquire),
        "destroy marks only current issue abandoned");
  check(!old.occlusionResult->abandoned.load(std::memory_order_acquire),
        "old retained issue is independent of current Query destruction");

  check(marker->bind(oldWriter, 1), "bind successful old publication");
  check(!device->promotePublishedQueryPollSnapshot(old),
        "bound prefix beyond actual publication is invalid");
  check(!old.eligible && !old.publishedOnly,
        "invalid promotion leaves snapshot capability unchanged");
  upper->progress.published = 1;
  checkEq(device->pollQuerySnapshot(old, nullptr, 0, 0), D3DERR_NOTAVAILABLE,
          "binding alone does not silently change default eligibility");
  const auto identity = old.identity;
  const auto issue = old.issueOrdinal;
  const auto epoch = old.issueDeviceEpoch;
  const auto result = old.occlusionResult;
  check(device->promotePublishedQueryPollSnapshot(old),
        "captured issue promoted after publication and Query destruction");
  check(old.eligible && old.publishedOnly && old.identity == identity &&
        old.issueOrdinal == issue && old.issueDeviceEpoch == epoch &&
        old.backendFence.marker == marker && old.occlusionResult == result &&
        old.backend.get() == upper.get(),
        "promotion retains exact issue receipt result and backend");
  checkEq(upper->flushPolls, 0ull, "promotion only classifies without FLUSH");
  checkEq(device->pollQuerySnapshot(old, &value, sizeof(value), 0), S_FALSE,
          "publication is not completion");
  upper->progress.completed = 1;
  checkEq(device->pollQuerySnapshot(old, &value, sizeof(value), 0), S_FALSE,
          "initializer remains part of published completion");
  checkEq(value, 0xdeadbeefu, "pending retained issue preserves output");
  upper->progress.initializerCompleted = 1;
  checkEq(device->pollQuerySnapshot(old, &value, sizeof(value), 0),
          D3DERR_DEVICELOST, "completed fence cannot bypass pending result");
  checkEq(value, 0xdeadbeefu, "pending result error preserves output");
  old.occlusionResult->pending.store(0, std::memory_order_release);
  checkEq(device->pollQuerySnapshot(old, &value, sizeof(value), 0), S_OK,
          "retained old result completes after reissue and destroy");
  checkEq(value, 42u, "retained issue returns original samples");
  old.occlusionResult->failed.store(true, std::memory_order_release);
  value = 0xdeadbeef;
  checkEq(device->pollQuerySnapshot(old, &value, sizeof(value), 0),
          D3DERR_DEVICELOST, "retained result failure dominates completion");
  checkEq(value, 0xdeadbeefu, "failed retained result preserves output");
}

void testPublishedPromotionControlAndReceiptFailures() {
  using dxmt9::query::EndMarker;
  using dxmt9::query::WriterIdentity;
  auto upper = std::make_shared<SnapshotUpper>(std::make_shared<RecordingBackend>());
  const WriterIdentity writer{7, 1, 1, 1};
  auto marker = std::make_shared<EndMarker>(writer);
  upper->deferredMarker = marker;
  Factory factory(upper);
  auto device = factory.createDevice(0, testParams());
  auto query = device->createQuery(QueryType::Timestamp);
  checkEq(device->issueQuery(query, false), D3D_OK, "control issue END");
  auto snapshot = device->captureQueryPollSnapshot(query);
  check(marker->bind(writer, 1), "control marker bound");
  upper->progress.published = upper->progress.completed = 1;
  upper->progress.failed = true;
  check(!device->promotePublishedQueryPollSnapshot(snapshot),
        "terminal backend rejects promotion despite complete waterline");
  check(upper->lastPoll == dxmt9::query::Poll::Failed,
        "matching queue classifies terminal publication failed");
  upper->progress.queue = 8;
  check(!device->promotePublishedQueryPollSnapshot(snapshot),
        "foreign queue rejects promotion even with terminal failure");
  check(upper->lastPoll == dxmt9::query::Poll::Invalid,
        "queue authority mismatch takes precedence over foreign terminal state");
  upper->progress.failed = false;
  check(!device->promotePublishedQueryPollSnapshot(snapshot),
        "foreign queue rejects promotion with complete waterline");
  upper->progress.queue = 7;
  Factory otherFactory(upper);
  auto other = otherFactory.createDevice(0, testParams());
  check(!other->promotePublishedQueryPollSnapshot(snapshot),
        "foreign core owner cannot grant promotion");
  device->setDeviceLost(true);
  check(!device->promotePublishedQueryPollSnapshot(snapshot),
        "device loss rejects promotion");
  device->setDeviceLost(false);
  auto duringReset = snapshot;
  upper->pollContext = device.get();
  upper->onPoll = [](void* context) {
    static_cast<Device*>(context)->reset(testParams());
  };
  check(!device->promotePublishedQueryPollSnapshot(duringReset),
        "reset during backend classification rejects promotion");
  check(!duringReset.eligible && !duringReset.publishedOnly,
        "reset overlap never grants capability");
  check(!device->promotePublishedQueryPollSnapshot(snapshot),
        "stale issue epoch rejects promotion");

  // A new issue tests cancellation/terminal validation after a valid grant.
  marker = std::make_shared<EndMarker>(WriterIdentity{7, 2, 2, 2});
  upper->deferredMarker = marker;
  checkEq(device->issueQuery(query, false), D3D_OK, "post-reset END");
  snapshot = device->captureQueryPollSnapshot(query);
  check(marker->bind(marker->owner(), 1), "post-reset marker bound");
  check(device->promotePublishedQueryPollSnapshot(snapshot),
        "post-reset current issue promoted");
  u64 value = 0xdeadbeef;
  upper->progress.failed = true;
  checkEq(device->pollQuerySnapshot(snapshot, &value, sizeof(value), 0),
          D3DERR_DEVICELOST, "terminal rechecked after promotion");
  checkEq(value, 0xdeadbeefull, "terminal preserves output");
  upper->progress.failed = false;
  upper->progress.queue = 8;
  checkEq(device->pollQuerySnapshot(snapshot, &value, sizeof(value), 0),
          D3DERR_DEVICELOST, "foreign queue rechecked after promotion");
  upper->progress.queue = 7;
  marker->cancel();
  const auto before = upper->polls;
  checkEq(device->pollQuerySnapshot(snapshot, &value, sizeof(value),
                                    QUERY_GETDATA_FLUSH),
          D3DERR_DEVICELOST, "retained cancellation rejects promoted FLUSH");
  checkEq(upper->polls, before, "cancelled receipt never requests submission");
  checkEq(value, 0xdeadbeefull, "cancelled receipt preserves output");
  check(!device->promotePublishedQueryPollSnapshot(snapshot),
        "cancelled publication cannot be granted again");
  auto cancelled = device->captureQueryPollSnapshot(query);
  check(!device->promotePublishedQueryPollSnapshot(cancelled) &&
        !cancelled.eligible && !cancelled.publishedOnly,
        "cancelled default capture never receives capability");

  auto unbound = snapshot;
  unbound.backendFence.marker = std::make_shared<EndMarker>(writer);
  checkEq(device->pollQuerySnapshot(unbound, &value, sizeof(value),
                                    QUERY_GETDATA_FLUSH),
          D3DERR_DEVICELOST, "published-only capability cannot poll unbound writer");
  checkEq(upper->polls, before, "unbound published-only FLUSH never reaches backend");
  auto noMarker = snapshot;
  noMarker.backendFence.marker.reset();
  checkEq(device->pollQuerySnapshot(noMarker, &value, sizeof(value), 0),
          D3DERR_DEVICELOST, "published capability cannot discard marker authority");

  marker = std::make_shared<EndMarker>(WriterIdentity{7, 3, 3, 3});
  upper->deferredMarker = marker;
  checkEq(device->issueQuery(query, false), D3D_OK, "cancellation overlap END");
  auto overlap = device->captureQueryPollSnapshot(query);
  check(marker->bind(marker->owner(), 1), "cancellation overlap marker bound");
  upper->pollContext = marker.get();
  upper->afterPoll = [](void* context) {
    static_cast<EndMarker*>(context)->cancel();
  };
  check(!device->promotePublishedQueryPollSnapshot(overlap) &&
        !overlap.eligible && !overlap.publishedOnly,
        "cancellation after backend classification prevents capability grant");
  check(upper->lastPoll == dxmt9::query::Poll::Complete,
        "promotion independently rechecks cancellation after complete classify");

  marker = std::make_shared<EndMarker>(WriterIdentity{7, 4, 4, 4});
  upper->deferredMarker = marker;
  checkEq(device->issueQuery(query, false), D3D_OK, "poll overlap END");
  overlap = device->captureQueryPollSnapshot(query);
  check(marker->bind(marker->owner(), 1), "poll overlap marker bound");
  check(device->promotePublishedQueryPollSnapshot(overlap),
        "poll overlap issue promoted");
  upper->pollContext = marker.get();
  upper->afterPoll = [](void* context) {
    static_cast<EndMarker*>(context)->cancel();
  };
  checkEq(device->pollQuerySnapshot(overlap, &value, sizeof(value), 0),
          D3DERR_DEVICELOST, "cancellation after complete classify preserves failure");
  checkEq(value, 0xdeadbeefull, "cancellation overlap preserves output");
  marker = std::make_shared<EndMarker>(WriterIdentity{7, 5, 5, 5});
  upper->deferredMarker = marker;
  checkEq(device->issueQuery(query, false), D3D_OK, "stale granted snapshot END");
  auto stale = device->captureQueryPollSnapshot(query);
  check(marker->bind(marker->owner(), 1), "stale granted snapshot marker bound");
  check(device->promotePublishedQueryPollSnapshot(stale),
        "stale snapshot receives valid grant before reset");
  checkEq(device->reset(testParams()), D3D_OK, "reset granted snapshot");
  checkEq(device->pollQuerySnapshot(stale, &value, sizeof(value), 0),
          D3DERR_DEVICELOST, "granted capability cannot bypass stale issue epoch");
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
    testPublishedReceiptPromotionTruthTable();
    testPublishedSnapshotSurvivesUnpublishedReissueAndDestroy();
    testPublishedPromotionControlAndReceiptFailures();
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
