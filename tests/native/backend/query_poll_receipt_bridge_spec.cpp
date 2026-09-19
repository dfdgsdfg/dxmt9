// Production bridge coverage for the opt-in query replay receipt lane.
#include "device_c_common.hpp"
#include "device_c_query_poll_policy.hpp"
#include "d3d9_pe_chunk_builder.hpp"
#include "dxmt9/com.hpp"
#include "dxmt9/dxmt9_device.hpp"

#include <cstdint>
#include <cstring>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

// A real COM device is still constructed by Direct3DCreate9Ex; this upper
// runtime only makes its query fence complete deterministically and avoids a
// Metal dependency in the bridge fixture.
struct QueryUpper final : dxmt9::Device {
  QueryUpper()
      : limits_{}, queue_(dxmt9::CommandQueue::ArenaLeaseTestQueueTag{},
                          limits_) {}

  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  dxmt9::CommandQueue& queue() override { return queue_; }
  const dxmt9::core::BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<dxmt9::core::BackendDevice> backend() override { return {}; }
  bool usesAsyncQueryFences() const noexcept override { return true; }
  dxmt9::query::EndReceipt publishQueryEnd() noexcept override {
    return {1u, ++published_, 0u};
  }
  dxmt9::query::Poll pollQueryFence(
      const dxmt9::query::EndReceipt&, bool) noexcept override {
    if (failLedger) failLedger->publishFailure();
    return dxmt9::query::Poll::Complete;
  }

  dxmt9::core::BackendLimits limits_{};
  dxmt9::CommandQueue queue_;
  std::uint64_t published_ = 0u;
  dxmt9::d3d9::ReplayDrainLedger* failLedger = nullptr;
};

struct DeviceFixture {
  DeviceFixture()
      : factory(dxmt9::com::Direct3DCreate9Ex(
            dxmt9::com::D3D_SDK_VERSION, std::make_unique<QueryUpper>())) {
    check(factory != nullptr, "bridge fixture factory");
    dxmt9::core::PresentParameters params{};
    params.backBufferWidth = 8u;
    params.backBufferHeight = 8u;
    params.backBufferFormat = dxmt9::core::Format::A8R8G8B8;
    params.windowed = true;
    params.deviceWindow = dxmt9::core::Handle{1u};
    params.presentationInterval = dxmt9::core::PresentInterval::Immediate;
    device = factory->CreateDeviceEx(0u, params, nullptr);
    check(device != nullptr, "bridge fixture device");
    device->AddRef();
  }

  ~DeviceFixture() {
    if (device) device->Release();
    if (factory) factory->Release();
  }

  dxmt9::com::IDirect3D9Ex* factory = nullptr;
  dxmt9::com::IDirect3DDevice9Ex* device = nullptr;
};

dxmt9::d3d9::pe::QueryRef queryRef(D9CQuery* query) {
  dxmt9::d3d9::pe::QueryRef ref{};
  ref.identity = query->wireIdentity;
  ref.object = query;
  return ref;
}

void commitContiguous(D9CDevice& device, dxmt9::d3d9::pe::CommandChunkBuilder& builder) {
  const auto sealed = builder.seal();
  check(sealed.valid(), "contiguous query chunk seals");
  const D9CCommandChunk chunk{
      .version = D9C_COMMAND_CHUNK_VERSION,
      .recordCount = sealed.recordCount,
      .recordBytes = static_cast<std::uint32_t>(sealed.blob.size()),
      .records = toWireHandle(sealed.blob.data()),
      .handleCount = sealed.handleCount,
  };
  check(dxmt9c_device_commit_chunk(&device, &chunk) == dxmt9::core::D3D_OK,
        "contiguous query chunk commits");
  check(dxmt9::d3d9::drainDeferredReplay(&device, "query-receipt-spec"),
        "contiguous query chunk settles");
}

void submitContiguousWithoutDrain(
    D9CDevice& device, dxmt9::d3d9::pe::CommandChunkBuilder& builder) {
  const auto sealed = builder.seal();
  check(sealed.valid(), "contiguous queued query chunk seals");
  const D9CCommandChunk chunk{
      .version = D9C_COMMAND_CHUNK_VERSION,
      .recordCount = sealed.recordCount,
      .recordBytes = static_cast<std::uint32_t>(sealed.blob.size()),
      .records = toWireHandle(sealed.blob.data()),
      .handleCount = sealed.handleCount,
  };
  check(dxmt9c_device_commit_chunk(&device, &chunk) == dxmt9::core::D3D_OK,
        "contiguous queued query chunk commits");
}

void commitSegmented(D9CDevice& device,
                     dxmt9::d3d9::pe::CommandChunkBuilder& builder) {
  const auto sealed = builder.sealSegmented();
  check(sealed.valid(), "segmented query chunk seals");
  device.commandChunkTransport = D9C_COMMAND_CHUNK_TRANSPORT_SEGMENTED_V1;
  check(dxmt9c_device_commit_chunk_segmented(&device, &sealed.transport) ==
            dxmt9::core::D3D_OK,
        "segmented query chunk commits");
  check(dxmt9::d3d9::drainDeferredReplay(&device, "query-receipt-spec"),
        "segmented query chunk settles");
}

void testValidatedContiguousBeginEndAndBridgePoll() {
  DeviceFixture fixture;
  D9CDevice device(fixture.device);
  auto* query = dxmt9c_device_create_query(&device, 10u);
  check(query && query->replayDrainTarget && query->pollState,
        "timestamp query allocates opt-in receipt state");

  dxmt9::d3d9::pe::CommandChunkBuilder builder;
  const auto ref = queryRef(query);
  check(dxmt9::d3d9::pe::appendQueryIssue(builder, 2u, ref) &&
            dxmt9::d3d9::pe::appendQueryIssue(builder, 1u, ref) &&
            dxmt9::d3d9::pe::appendClear(
                builder, D9CCommandChunkWireClear{.flags = 1u},
                std::span<const D9CRect>{}),
        "validated contiguous BEGIN+END records and suffix append");
  commitContiguous(device, builder);
  check(query->replayDrainTarget->lastQueuedSeq != 0u &&
            query->replayDrainTarget->lastQueuedSeq ==
                query->replayDrainTarget->lastReplayedSeq,
        "contiguous BEGIN+END attaches and settles one target");

  std::uint64_t value = 0xfeedfacecafebeefull;
  check(dxmt9c_query_get_data(query, &value, sizeof(value), 0u) ==
            dxmt9::core::D3D_OK,
        "bridge poll uses the qualified immutable receipt");
  check(value != 0xfeedfacecafebeefull,
        "qualified timestamp poll writes the result");
  check(dxmt9::d3d9::tryPollQueryReceipt(query, nullptr, 0u, 0u).has_value(),
        "contiguous raw uses snapshot route, not merely successful fallback");
  const auto old = device.replayDrainLedger.captureReceipt(query->replayDrainTarget);
  dxmt9::d3d9::pe::CommandChunkBuilder begin;
  check(dxmt9::d3d9::pe::appendQueryIssue(begin, 2u, ref), "new BEGIN");
  commitContiguous(device, begin);
  check(device.replayDrainLedger.waitReceipt(old) ==
            dxmt9::query::ReplayReceiptState::Superseded,
        "actual BEGIN admission supersedes the old END receipt");
  value = 0xfeedfacecafebeefull;
  check(dxmt9c_query_get_data(query, &value, sizeof(value), 0u) ==
            dxmt9::core::S_FALSE && value == 0xfeedfacecafebeefull,
        "active BEGIN preserves bytes instead of reusing old result");
  dxmt9c_query_release(query);
}

void testValidatedSegmentedBeginEndAndTerminalSentinel() {
  DeviceFixture fixture;
  D9CDevice device(fixture.device);
  auto* query = dxmt9c_device_create_query(&device, 10u);
  check(query && query->replayDrainTarget && query->pollState,
        "segmented timestamp query allocates receipt state");

  dxmt9::d3d9::pe::CommandChunkBuilder builder;
  const auto ref = queryRef(query);
  check(dxmt9::d3d9::pe::appendQueryIssue(builder, 2u, ref) &&
            dxmt9::d3d9::pe::appendQueryIssue(builder, 1u, ref),
        "validated segmented BEGIN+END records append");
  commitSegmented(device, builder);
  check(query->replayDrainTarget->lastQueuedSeq != 0u &&
            query->replayDrainTarget->lastQueuedSeq ==
                query->replayDrainTarget->lastReplayedSeq,
        "segmented BEGIN+END attaches and settles one target");

  check(dxmt9::d3d9::tryPollQueryReceipt(query, nullptr, 0u, 0u).has_value(),
        "segmented raw snapshot has a qualified replay ordinal");
  auto* upper = static_cast<QueryUpper*>(device.dev().upperDevice().get());
  upper->failLedger = &device.replayDrainLedger;
  std::uint64_t value = 0x123456789abcdef0ull;
  check(dxmt9c_query_get_data(query, &value, sizeof(value), 0u) ==
            dxmt9::core::D3DERR_DEVICELOST,
        "terminal publication during backend poll fails closed through the bridge");
  check(value == 0x123456789abcdef0ull,
        "terminal receipt poll preserves the output sentinel");
  dxmt9c_query_release(query);
}

void testMissingReceiptFallsBackToLegacyBridge() {
  DeviceFixture fixture;
  D9CDevice device(fixture.device);
  auto* query = dxmt9c_device_create_query(&device, 10u);
  check(query != nullptr, "fallback query creates");
  // This simulates a query created before the opt-in state was admitted.  The
  // bridge must return the provider's ordinary result rather than inventing a
  // receipt or touching queue state.
  query->pollState.reset();
  query->replayDrainTarget.reset();
  std::uint64_t value = 0xabcdef0123456789ull;
  check(dxmt9c_query_get_data(query, &value, sizeof(value), 0u) ==
            dxmt9::core::S_OK,
        "missing receipt uses direct provider fallback");
  check(value == 0u,
        "legacy pre-issue poll writes the provider's zero result");
  dxmt9c_query_release(query);
}

std::atomic<bool> holdUnrelatedRaw{false};
std::atomic<bool> unrelatedRawEntered{false};
std::atomic<bool> releaseUnrelatedRaw{false};
std::uint64_t gateOrdinal = 2u;

struct GateRelease {
  ~GateRelease() { releaseUnrelatedRaw.store(true, std::memory_order_release); }
};

void awaitRawGate() {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!unrelatedRawEntered.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  check(unrelatedRawEntered.load(std::memory_order_acquire), "raw reaches gate");
}

std::int32_t replayWithUnrelatedRawGate(
    D9CDevice* device, dxmt9::d3d9::RawCommandChunk& chunk) {
  const auto result = dxmt9::d3d9::replayRawChunk(device, chunk);
  if (chunk.replaySeq == gateOrdinal && holdUnrelatedRaw.load(std::memory_order_acquire)) {
    unrelatedRawEntered.store(true, std::memory_order_release);
    while (!releaseUnrelatedRaw.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }
  return result;
}

void testLaterUnrelatedRawDoesNotExtendQualifiedReceipt() {
  if (!dxmt9::d3d9::offloadCommitReplayEnabled()) {
    return;  // The production queue is intentionally disabled in this lane.
  }
  DeviceFixture fixture;
  D9CDevice device(fixture.device);
  auto* query = dxmt9c_device_create_query(&device, 10u);
  check(query && query->replayDrainTarget && query->pollState,
        "queued query allocates receipt state");

  device.replayOffload = std::make_unique<dxmt9::d3d9::ReplayOffloadWorker>(
      replayWithUnrelatedRawGate);
  check(device.replayOffload->start(&device), "receipt worker starts");
  GateRelease releaseOnFailure;
  gateOrdinal = 2u;
  unrelatedRawEntered = false;
  releaseUnrelatedRaw = false;
  holdUnrelatedRaw.store(true, std::memory_order_release);

  dxmt9::d3d9::pe::CommandChunkBuilder issue;
  check(dxmt9::d3d9::pe::appendQueryIssue(issue, 1u, queryRef(query)),
        "queued query END appends");
  submitContiguousWithoutDrain(device, issue);

  dxmt9::d3d9::pe::CommandChunkBuilder unrelated;
  check(dxmt9::d3d9::pe::appendClear(
            unrelated, D9CCommandChunkWireClear{.flags = 1u},
            std::span<const D9CRect>{}),
        "later unrelated clear appends");
  submitContiguousWithoutDrain(device, unrelated);
  awaitRawGate();

  std::uint64_t value = 0x0badf00ddeadbeefull;
  check(dxmt9c_query_get_data(query, &value, sizeof(value), 0u) ==
            dxmt9::core::D3D_OK,
        "qualified receipt poll ignores later unrelated raw");
  check(value != 0x0badf00ddeadbeefull,
        "qualified receipt poll writes before unrelated raw completes");
  auto reference = std::async(std::launch::async, [&] {
    return dxmt9::d3d9::drainDeferredReplay(&device, "query-receipt-reference");
  });
  const bool referenceBlocked = reference.wait_for(std::chrono::milliseconds(20)) ==
                               std::future_status::timeout;
  releaseUnrelatedRaw.store(true, std::memory_order_release);
  check(reference.get() && referenceBlocked,
        "global reference waits for the held later raw");
  check(dxmt9::d3d9::drainDeferredReplay(&device, "query-receipt-spec"),
        "later unrelated raw settles");
  holdUnrelatedRaw.store(false, std::memory_order_release);
  dxmt9c_query_release(query);
}

void testWholeRawCompletionStillRequired() {
  if (!dxmt9::d3d9::offloadCommitReplayEnabled()) return;
  DeviceFixture fixture;
  D9CDevice device(fixture.device);
  auto* query = dxmt9c_device_create_query(&device, 10u);
  device.replayOffload = std::make_unique<dxmt9::d3d9::ReplayOffloadWorker>(
      replayWithUnrelatedRawGate);
  check(device.replayOffload->start(&device), "whole-raw worker starts");
  GateRelease releaseOnFailure;
  gateOrdinal = 1u;
  unrelatedRawEntered = false;
  releaseUnrelatedRaw = false;
  holdUnrelatedRaw = true;
  dxmt9::d3d9::pe::CommandChunkBuilder issue;
  check(dxmt9::d3d9::pe::appendQueryIssue(issue, 1u, queryRef(query)) &&
        dxmt9::d3d9::pe::appendClear(issue, D9CCommandChunkWireClear{.flags = 1u}, {}),
        "END and suffix share a raw");
  submitContiguousWithoutDrain(device, issue);
  awaitRawGate();
  const auto receipt = device.replayDrainLedger.captureReceipt(query->replayDrainTarget);
  bool copied = false;
  check(device.replayDrainLedger.visitReadyReceipt(receipt, [&] { copied = true; }) ==
            dxmt9::query::ReplayReceiptState::Pending && !copied,
        "published END snapshot alone never authorizes copy before whole-raw completion");
  auto poll = std::async(std::launch::async, [&] {
    return dxmt9c_query_get_data(query, nullptr, 0, 0);
  });
  const bool blocked = poll.wait_for(std::chrono::milliseconds(20)) ==
                       std::future_status::timeout;
  releaseUnrelatedRaw = true;
  check(poll.get() == dxmt9::core::S_OK && blocked,
        "bridge poll waits for its own raw completion");
  check(dxmt9::d3d9::drainDeferredReplay(&device, "query-receipt-spec"), "gate settles");
  holdUnrelatedRaw = false;
  dxmt9c_query_release(query);
}

}  // namespace

int main() {
  try {
    if (!dxmt9::d3d9::queryPollReceiptsEnabled()) {
      std::cout << "query_poll_receipt_bridge_spec skipped (gate off)\n";
      return 0;
    }
    testValidatedContiguousBeginEndAndBridgePoll();
    testValidatedSegmentedBeginEndAndTerminalSentinel();
    testMissingReceiptFallsBackToLegacyBridge();
    testLaterUnrelatedRawDoesNotExtendQualifiedReceipt();
    testWholeRawCompletionStillRequired();
  } catch (const TestFailure& failure) {
    return 1;
  }
  std::cout << "query_poll_receipt_bridge_spec passed\n";
  return 0;
}
