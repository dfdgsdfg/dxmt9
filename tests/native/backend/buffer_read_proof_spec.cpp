// Cold physical-read oracle using real Shared WMT allocations only. No
// command buffers are submitted and no GPU execution/pixel claim is made.
// Finite sequence stamps below isolate Pool's retained-watermark predicates;
// they do not manufacture queue tickets, source ownership, or completion
// authority. Each fixture owns its Pool and captured residency throughout
// the call, and all backing writes use arena-locked production upload APIs.

#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../../src/winemetal/Metal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using ResourcePool = dxmt9::resources::Pool;
using Range = dxmt9::resources::BufferReadProofRange;
using Request = dxmt9::resources::BufferReadProofRequest;
using Result = dxmt9::resources::BufferReadProofResult;
using Status = dxmt9::resources::BufferReadProofStatus;
using BatchLimits = dxmt9::resources::BufferReadProofBatchLimits;
using BatchResult = dxmt9::resources::BufferReadProofBatchResult;
using VersionObservation = dxmt9::resources::BufferVersionObservation;
using VersionStatus = dxmt9::resources::BufferVersionObservationStatus;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

Result expectStatus(const ResourcePool& pool, const Request& request,
                    Status status, std::string_view message) {
  const auto result = pool.proveBufferReadFootprint(request);
  check(result.status == status, message);
  check(result.proved() == (status == Status::Valid),
        "proved() reports only a complete valid proof");
  if (status != Status::Valid) {
    check(result.provedRanges == 0u && result.provedBytes == 0u,
          "a rejected proof grants no partial proved counts");
  }
  return result;
}

BatchResult expectBatchStatus(const ResourcePool& pool, std::span<const Request> requests,
                              Status status, std::string_view message,
                              BatchLimits limits = {}) {
  const auto result = pool.proveBufferReadFootprints(requests, limits);
  check(result.status == status, message);
  check(result.proved() == (status == Status::Valid),
        "a batch qualifies only complete coherent equality");
  if (status != Status::Valid)
    check(!result.provedRequests && !result.provedRanges && !result.provedBytes,
          "a rejected batch grants no partial request/range/byte authority");
  return result;
}

VersionObservation expectVersion(const ResourcePool& pool,
                                 dxmt9::core::Handle handle,
                                 VersionStatus status,
                                 std::string_view message) {
  const auto observation = pool.observeBufferVersion(handle);
  check(observation.status == status, message);
  check(observation.observed() == (status == VersionStatus::Valid),
        "observed() qualifies only a complete physical version observation");
  if (status != VersionStatus::Valid) {
    check(!observation.buffer && observation.byteSize == 0u &&
              observation.contentRevision == 0u &&
              !observation.hasVersionedBacking &&
              observation.currentBacking.metalHandle == 0u &&
              observation.currentBacking.byteSize == 0u,
          "failed version observation returns no partial facts");
  }
  return observation;
}

struct DynamicFixture {
  static constexpr std::size_t kBytes = 32u;
  static constexpr std::uint64_t kSource = 7u;

  explicit DynamicFixture(WMT::Device device, bool stamp = true)
      : device(device) {
    dxmt9::core::BufferDesc desc{};
    desc.size = kBytes;
    desc.pool = dxmt9::core::Pool::Default;
    desc.usage = dxmt9::core::UsageDynamic;
    handle = pool.createBuffer(device, desc);
    check(static_cast<bool>(handle), "real dynamic buffer is created");
    for (std::size_t index = 0u; index < expected.size(); ++index) {
      expected[index] = static_cast<std::uint8_t>(index + 0x20u);
    }
    check(pool.uploadBufferData(device, handle.value, expected.data(),
                                expected.size(), 0u),
          "initial upload resolves the dynamic buffer");
    captured = pool.captureChunkBufferBinding(handle);
    check(captured.requiresCapturedBacking && captured.snapshot.valid() &&
              captured.snapshot.contentsAddress != 0u &&
              captured.snapshot.byteSize == expected.size() &&
              captured.backingResidency,
          "capture owns a real Shared backing with its complete extent");
    if (stamp) {
      pool.markBufferSnapshotUse(handle, captured.snapshot, kSource);
    }
  }

  Request request(std::span<const Range> ranges) const {
    return {.buffer = handle,
            .sourceSequence = kSource,
            .completedSequence = 0u,
            .snapshot = captured.snapshot,
            .expectedBytes = expected,
            .ranges = ranges};
  }

  void patch(std::uint64_t offset, std::span<const std::uint8_t> bytes) {
    check(pool.uploadBufferDataRange(device, handle.value, offset, bytes.data(),
                                     bytes.size(), 0u),
          "arena-locked range upload is admitted");
  }

  WMT::Device device;
  ResourcePool pool;
  dxmt9::core::Handle handle{};
  std::array<std::uint8_t, kBytes> expected{};
  dxmt9::core::ChunkBufferBindingSnapshot captured{};
};

struct BatchFixture {
  static constexpr std::size_t kBytes = 32u;
  static constexpr std::uint64_t kSource = 7u;

  explicit BatchFixture(WMT::Device device, bool versioned = true) : device(device) {
    dxmt9::core::BufferDesc desc{};
    desc.size = kBytes;
    desc.pool = dxmt9::core::Pool::Default;
    if (versioned) desc.usage = dxmt9::core::UsageDynamic;
    for (std::size_t index = 0u; index < handles.size(); ++index) {
      expected[index].fill(static_cast<std::uint8_t>(0x30u + index));
      handles[index] = pool.createBuffer(device, desc);
      check(bool(handles[index]), "batch fixture owns a real Shared allocation");
      check(pool.uploadBufferData(device, handles[index].value, expected[index].data(), kBytes, 0u),
            "batch fixture uploads the complete expected image under the arena lock");
      if (versioned) {
        captured[index] = pool.captureChunkBufferBinding(handles[index]);
        check(captured[index].snapshot.valid() && captured[index].backingResidency,
              "each versioned batch member owns its genuine captured backing");
        pool.markBufferSnapshotUse(handles[index], captured[index].snapshot, kSource);
      } else pool.markBufferUse(handles[index], kSource);
    }
  }

  std::array<Request, 2> requests(std::span<const Range> first,
                                 std::span<const Range> second) const {
    std::array<Request, 2> out{};
    for (std::size_t index = 0u; index < out.size(); ++index) {
      out[index] = {.buffer = handles[index], .sourceSequence = kSource,
                    .expectedBytes = expected[index], .ranges = index ? second : first};
      if (captured[index].snapshot.valid()) out[index].snapshot = captured[index].snapshot;
    }
    return out;
  }

  void patch(std::size_t index, std::uint64_t offset) {
    const std::uint8_t value = 0xffu;
    check(pool.uploadBufferDataRange(device, handles[index].value, offset, &value, 1u, 0u),
          "batch distinguishing mutation uses the exclusive production upload lock");
  }

  WMT::Device device;
  ResourcePool pool;
  std::array<dxmt9::core::Handle, 2> handles{};
  std::array<std::array<std::uint8_t, kBytes>, 2> expected{};
  std::array<dxmt9::core::ChunkBufferBindingSnapshot, 2> captured{};
};

void testCurrentVersionObservation(WMT::Device device) {
  ResourcePool pool;
  dxmt9::core::BufferDesc desc{};
  desc.size = 32u;
  desc.pool = dxmt9::core::Pool::Default;
  desc.usage = dxmt9::core::UsageDynamic;
  const auto handle = pool.createBuffer(device, desc);
  check(static_cast<bool>(handle), "version oracle creates a real dynamic buffer");
  const auto created = expectVersion(pool, handle, VersionStatus::Valid,
                                     "creation exposes a physical current version");
  check(created.buffer == handle && created.byteSize == desc.size &&
            created.contentRevision == 1u && created.hasVersionedBacking &&
            created.currentBacking.metalHandle != 0u &&
            created.currentBacking.byteSize == created.byteSize,
        "creation facts contain the actual handle, extent, revision and route");

  std::array<std::uint8_t, 32> bytesA{};
  bytesA.fill(0x35u);
  check(pool.uploadBufferData(device, handle.value, bytesA.data(), bytesA.size(), 0u),
        "successful full upload precedes version observation");
  const auto uploaded = expectVersion(pool, handle, VersionStatus::Valid,
                                      "full upload exposes its new logical revision");
  check(uploaded.contentRevision == created.contentRevision + 1u &&
            uploaded.currentBacking.metalHandle == created.currentBacking.metalHandle,
        "full dynamic upload updates revision without changing concrete backing");
  auto capturedA = pool.captureChunkBufferBinding(handle);
  check(capturedA.snapshot.metalHandle == uploaded.currentBacking.metalHandle &&
            capturedA.snapshot.byteSize == uploaded.byteSize &&
            capturedA.snapshot.contentRevision == uploaded.contentRevision,
        "sequential capture agrees with the current version value facts");

  const std::array<std::uint8_t, 2> patch = {0xa1u, 0xa2u};
  check(pool.uploadBufferDataRange(device, handle.value, 20u, patch.data(),
                                   patch.size(), 0u),
        "successful NOOVERWRITE upload precedes current version pull");
  const auto patched = expectVersion(pool, handle, VersionStatus::Valid,
                                     "NOOVERWRITE exposes a new logical revision");
  check(patched.contentRevision == uploaded.contentRevision + 1u &&
            patched.currentBacking.metalHandle == uploaded.currentBacking.metalHandle,
        "NOOVERWRITE keeps concrete allocation identity but changes logical revision");
  check(!pool.uploadBufferDataRange(device, handle.value, 31u, patch.data(),
                                    patch.size(), 0u),
        "out-of-bounds upload is rejected before mutation");
  const auto afterRejected = expectVersion(pool, handle, VersionStatus::Valid,
                                           "rejected mutation leaves an observable version");
  check(afterRejected.contentRevision == patched.contentRevision &&
            afterRejected.currentBacking.metalHandle == patched.currentBacking.metalHandle,
        "an attempted but rejected upload creates no new revision facts");

  // Retain and stamp a genuine capture before DISCARD. The observation itself
  // grants no pin; its revision describes only the current logical image.
  pool.markBufferSnapshotUse(handle, capturedA.snapshot, 7u);
  check(dxmt9::resources::dynamicBufferRenameEnabled(),
        "version oracle requires the default dynamic rename policy");
  check(pool.finalizeBufferMap(device, handle, dxmt9::core::UsageDiscard, 0u) != nullptr,
        "DISCARD rotates under the production arena lock");
  const auto discarded = expectVersion(pool, handle, VersionStatus::Valid,
                                       "DISCARD observes the freshly current backing");
  check(discarded.contentRevision == patched.contentRevision + 1u &&
            discarded.currentBacking.metalHandle != patched.currentBacking.metalHandle &&
            discarded.hasVersionedBacking && discarded.byteSize == patched.byteSize,
        "DISCARD changes current allocation and logical revision, not logical extent");
  check(capturedA.snapshot.metalHandle == uploaded.currentBacking.metalHandle &&
            capturedA.snapshot.contentRevision == uploaded.contentRevision,
        "current observation does not rewrite old capture into an old-entry epoch");

  expectVersion(pool, {}, VersionStatus::InvalidRequest,
                "null handle cannot produce version facts");
}

void testNonversionedVersionObservation(WMT::Device device) {
  ResourcePool pool;
  dxmt9::core::BufferDesc desc{};
  desc.size = 32u;
  desc.pool = dxmt9::core::Pool::Default;
  const auto handle = pool.createBuffer(device, desc);
  const auto created = expectVersion(pool, handle, VersionStatus::Valid,
                                     "live nonversioned creation exposes current version");
  check(created.contentRevision == 1u && !created.hasVersionedBacking &&
            created.currentBacking.metalHandle != 0u &&
            created.currentBacking.byteSize == desc.size,
        "nonversioned observation does not depend on captured-ring routing");
  std::array<std::uint8_t, 32> bytes{};
  bytes.fill(0x48u);
  check(pool.uploadBufferData(device, handle.value, bytes.data(), bytes.size(), 0u),
        "nonversioned upload succeeds before version linkage");
  const auto uploaded = expectVersion(pool, handle, VersionStatus::Valid,
                                      "live nonversioned upload exposes its current revision");
  check(uploaded.contentRevision == created.contentRevision + 1u &&
            uploaded.currentBacking.metalHandle == created.currentBacking.metalHandle &&
            uploaded.buffer == handle && uploaded.byteSize == bytes.size(),
        "live route supplies coherent logical and physical value facts");
  bytes.fill(0x59u);
  check(pool.uploadBufferData(device, handle.value, bytes.data(), bytes.size(), 0u),
        "second nonversioned upload succeeds");
  const auto second = expectVersion(pool, handle, VersionStatus::Valid,
                                    "second live upload observes a further revision");
  check(second.contentRevision == uploaded.contentRevision + 1u &&
            !second.hasVersionedBacking,
        "nonversioned revisions continue to update without a capture-ring API");
}

void testFreshFootprintComparison(WMT::Device device) {
  DynamicFixture fixture(device);
  const std::array<Range, 2> footprint = {{{4u, 4u}, {16u, 4u}}};
  const std::array<Range, 1> whole = {{{0u, DynamicFixture::kBytes}}};
  const auto request = fixture.request(footprint);
  const auto initial = expectStatus(fixture.pool, request, Status::Valid,
                                    "initial fetched bytes match");
  check(initial.provedRanges == footprint.size() && initial.provedBytes == 8u &&
            initial.failedRange == std::numeric_limits<std::uint32_t>::max(),
        "complete success records exact footprint counts");

  const std::array<std::uint8_t, 4> outside = {0xa1u, 0xa2u, 0xa3u, 0xa4u};
  fixture.patch(24u, outside);
  const auto afterOutside = fixture.pool.snapshotBufferBinding(fixture.handle);
  check(afterOutside.metalHandle == fixture.captured.snapshot.metalHandle &&
            afterOutside.contentRevision != fixture.captured.snapshot.contentRevision,
        "NOOVERWRITE changes the same backing and current revision");
  expectStatus(fixture.pool, request, Status::Valid,
               "unread NOOVERWRITE bytes do not invalidate the old footprint");
  expectStatus(fixture.pool, fixture.request(whole), Status::ByteMismatch,
               "whole-buffer comparison would reject the unread mutation");

  const std::array<std::uint8_t, 1> inside = {0xffu};
  fixture.patch(5u, inside);
  const auto mismatch = expectStatus(fixture.pool, request, Status::ByteMismatch,
                                     "a later same-backing fetched-byte write is rechecked");
  check(mismatch.failedRange == 0u, "mismatch identifies the fetched range");

  std::array<std::uint8_t, 12> preserving{};
  std::copy_n(fixture.expected.begin(), preserving.size(), preserving.begin());
  preserving[0] = 0xb1u;
  preserving[11] = 0xb2u;
  fixture.patch(0u, preserving);
  expectStatus(fixture.pool, request, Status::Valid,
               "an overlapping upload passes when every fetched byte is preserved");
}

void testRangeValidationAndBudgets(WMT::Device device) {
  DynamicFixture fixture(device);
  const std::array<Range, 2> footprint = {{{4u, 4u}, {16u, 4u}}};
  auto request = fixture.request(footprint);
  request.limits.maxRanges = footprint.size();
  request.limits.maxReadBytes = 8u;
  expectStatus(fixture.pool, request, Status::Valid,
               "exact range and compared-byte budgets are accepted");
  request.limits.maxRanges = 1u;
  expectStatus(fixture.pool, request, Status::CapacityExceeded,
               "one range beyond the count budget is rejected");
  request.limits.maxRanges = footprint.size();
  request.limits.maxReadBytes = 7u;
  const auto bytesOver = expectStatus(fixture.pool, request, Status::CapacityExceeded,
                                     "one byte beyond the comparison budget is rejected");
  check(bytesOver.failedRange == 1u, "byte budget identifies its overflowing range");

  const std::array<Range, 3> overlapping = {{{4u, 4u}, {8u, 4u}, {6u, 4u}}};
  request = fixture.request(overlapping);
  request.limits.maxRanges = overlapping.size();
  request.limits.maxReadBytes = 12u;
  const auto overlap = expectStatus(fixture.pool, request, Status::Valid,
                                    "adjacent and overlapping footprint entries are legal");
  check(overlap.provedBytes == 12u && overlap.provedRanges == 3u,
        "overlapping bytes count separately toward the budget");
  request.limits.maxReadBytes = 11u;
  expectStatus(fixture.pool, request, Status::CapacityExceeded,
               "overlaps cannot hide compared bytes from the budget");

  request = fixture.request(footprint);
  request.ranges = {};
  expectStatus(fixture.pool, request, Status::InvalidRequest,
               "empty footprint is rejected");
  const std::array<Range, 1> emptyRange = {{{4u, 0u}}};
  expectStatus(fixture.pool, fixture.request(emptyRange), Status::InvalidRequest,
               "zero-size footprint entry is rejected");
  const std::array<Range, 1> endRange = {{{31u, 1u}}};
  expectStatus(fixture.pool, fixture.request(endRange), Status::Valid,
               "the final byte of the logical extent is admissible");
  const std::array<Range, 1> outOfBounds = {{{31u, 2u}}};
  expectStatus(fixture.pool, fixture.request(outOfBounds), Status::InvalidRange,
               "a range crossing the complete extent is rejected");
  const std::array<Range, 1> pastEnd = {{{33u, 1u}}};
  expectStatus(fixture.pool, fixture.request(pastEnd), Status::InvalidRange,
               "an offset beyond the extent is rejected");
  const std::array<Range, 1> overflowing = {{{
      std::numeric_limits<std::uint64_t>::max(), 2u}}};
  expectStatus(fixture.pool, fixture.request(overflowing), Status::ArithmeticOverflow,
               "offset plus byte size cannot wrap");

  const std::array<std::uint8_t, 1> mismatch = {0xffu};
  fixture.patch(4u, mismatch);
  const std::array<Range, 2> malformedLater = {{{4u, 4u}, {31u, 2u}}};
  const auto malformed = expectStatus(fixture.pool, fixture.request(malformedLater),
                                      Status::InvalidRange,
                                      "all ranges are validated before the first comparison");
  check(malformed.failedRange == 1u,
        "later invalid range takes precedence over early byte mismatch");
}

void testRequestAndSnapshotValidation(WMT::Device device) {
  DynamicFixture fixture(device);
  const std::array<Range, 1> footprint = {{{4u, 4u}}};
  const auto valid = fixture.request(footprint);
  auto request = valid;
  request.buffer = {};
  expectStatus(fixture.pool, request, Status::InvalidRequest,
               "null logical buffer is rejected");
  request = valid;
  request.sourceSequence = 0u;
  expectStatus(fixture.pool, request, Status::InvalidRequest,
               "zero source sequence is rejected");
  request = valid;
  request.completedSequence = request.sourceSequence;
  expectStatus(fixture.pool, request, Status::InvalidRequest,
               "already-completed source window is rejected");
  ++request.completedSequence;
  expectStatus(fixture.pool, request, Status::InvalidRequest,
               "completed watermark beyond the source is rejected");
  request = valid;
  request.limits.maxRanges = 0u;
  expectStatus(fixture.pool, request, Status::InvalidRequest,
               "zero range limit is rejected");
  request = valid;
  request.limits.maxReadBytes = 0u;
  expectStatus(fixture.pool, request, Status::InvalidRequest,
               "zero byte limit is rejected");
  request = valid;
  request.expectedBytes = request.expectedBytes.first(request.expectedBytes.size() - 1u);
  expectStatus(fixture.pool, request, Status::InvalidExpectedExtent,
               "expected image must cover the complete logical extent");
  request = valid;
  request.snapshot.reset();
  expectStatus(fixture.pool, request, Status::BackingRouteMismatch,
               "versioned storage cannot silently use the live route");
  request = valid;
  request.snapshot->metalHandle = 0u;
  expectStatus(fixture.pool, request, Status::InvalidSnapshot,
               "null captured Metal identity is rejected");
  request = valid;
  request.snapshot->contentsAddress = 0u;
  expectStatus(fixture.pool, request, Status::InvalidSnapshot,
               "null captured contents address is rejected");
  request.snapshot->contentsAddress = 1u;
  expectStatus(fixture.pool, request, Status::InvalidSnapshot,
               "forged nonnull address is rejected without dereferencing it");
  request = valid;
  --request.snapshot->byteSize;
  expectStatus(fixture.pool, request, Status::InvalidSnapshot,
               "captured extent must equal the complete logical extent");

  DynamicFixture other(device);
  request = valid;
  request.snapshot = other.captured.snapshot;
  expectStatus(fixture.pool, request, Status::InvalidSnapshot,
               "another real buffer's backing is not a member of this ring");
  // Intentionally forged revision: this API proves byte equality only. This
  // success must not qualify snapshot provenance or revision authenticity;
  // the source/context layer independently checks both before consumption.
  request = valid;
  request.snapshot->contentRevision = std::numeric_limits<std::uint64_t>::max();
  expectStatus(fixture.pool, request, Status::Valid,
               "revision is not an old-backing content-validity cache");

  DynamicFixture unmarked(device, false);
  expectStatus(unmarked.pool, unmarked.request(footprint), Status::UnretainedBacking,
               "an unstamped record cannot satisfy the source watermark");
}

void testOldDiscardBacking(WMT::Device device) {
  DynamicFixture fixture(device);
  const std::array<Range, 1> whole = {{{0u, DynamicFixture::kBytes}}};
  check(dxmt9::resources::dynamicBufferRenameEnabled(),
        "this oracle requires the default dynamic rename policy");
  check(fixture.pool.finalizeBufferMap(device, fixture.handle,
                                       dxmt9::core::UsageDiscard, 0u) != nullptr,
        "DISCARD selects a mapped real backing");
  std::array<std::uint8_t, DynamicFixture::kBytes> bytesB{};
  bytesB.fill(0xc3u);
  check(fixture.pool.uploadBufferData(device, fixture.handle.value, bytesB.data(),
                                      bytesB.size(), 0u),
        "new DISCARD backing receives its complete image");
  const auto current = fixture.pool.captureChunkBufferBinding(fixture.handle);
  check(current.snapshot.valid() &&
            current.snapshot.metalHandle != fixture.captured.snapshot.metalHandle &&
            current.snapshot.contentRevision != fixture.captured.snapshot.contentRevision,
        "retained old capture forces a distinct DISCARD backing and revision");
  fixture.pool.markBufferSnapshotUse(fixture.handle, current.snapshot, 11u);
  expectStatus(fixture.pool, fixture.request(whole), Status::Valid,
               "old capture reads its own retained backing after current rotation");
  auto currentRequest = fixture.request(whole);
  currentRequest.sourceSequence = 11u;
  currentRequest.snapshot = current.snapshot;
  currentRequest.expectedBytes = bytesB;
  expectStatus(fixture.pool, currentRequest, Status::Valid,
               "new backing proves its separately stamped image");
  auto falselyStampedOld = fixture.request(whole);
  falselyStampedOld.sourceSequence = 11u;
  expectStatus(fixture.pool, falselyStampedOld, Status::UnretainedBacking,
               "aggregate logical stamp cannot replace the matched old-entry stamp");
}

void testLiveRouteAndStaleHandle(WMT::Device device) {
  ResourcePool pool;
  dxmt9::core::BufferDesc desc{};
  desc.size = 32u;
  desc.pool = dxmt9::core::Pool::Default;
  const auto handle = pool.createBuffer(device, desc);
  check(static_cast<bool>(handle), "real nonversioned buffer is created");
  std::array<std::uint8_t, 32> expected{};
  expected.fill(0x8cu);
  check(pool.uploadBufferData(device, handle.value, expected.data(), expected.size(), 0u),
        "nonversioned complete image is uploaded");
  pool.markBufferUse(handle, 7u);
  const std::array<Range, 1> whole = {{{0u, expected.size()}}};
  Request request{.buffer = handle,
                  .sourceSequence = 7u,
                  .completedSequence = 0u,
                  .expectedBytes = expected,
                  .ranges = whole};
  expectStatus(pool, request, Status::Valid,
               "nonversioned route compares the live Shared backing");
  request.snapshot = dxmt9::core::DrawBufferBindingSnapshot{};
  expectStatus(pool, request, Status::BackingRouteMismatch,
               "nonversioned route rejects even an empty supplied capture");
  request.snapshot.reset();

  // Deliberately end this fixture's logical ownership before probing the stale
  // encoded handle. The new allocation need not reuse a physical address.
  check(pool.markBufferDestroyAndGc(handle.value, 7u),
        "completed logical buffer is reclaimed");
  const auto replacement = pool.createBuffer(device, desc);
  check(replacement && replacement != handle,
        "new buffer has a distinct generation-qualified identity");
  pool.markBufferUse(replacement, 7u);
  expectStatus(pool, request, Status::MissingBuffer,
               "stale encoded handle cannot prove a replacement backing");
  expectVersion(pool, handle, VersionStatus::MissingBuffer,
                "stale generation cannot observe a replacement's version facts");
  expectVersion(pool, replacement, VersionStatus::Valid,
                "replacement generation remains independently observable");

  desc.pool = dxmt9::core::Pool::SystemMem;
  const auto cpuOnly = pool.createBuffer(device, desc);
  pool.markBufferUse(cpuOnly, 7u);
  request.buffer = cpuOnly;
  expectStatus(pool, request, Status::BackingUnavailable,
               "CPU shadow alone is not physical Metal backing evidence");
  expectVersion(pool, cpuOnly, VersionStatus::BackingUnavailable,
                "shadow-only version route returns status without physical facts");
}

void testCoherentBatchEqualityAndMismatch(WMT::Device device) {
  BatchFixture fixture(device);
  const std::array<Range, 1> index = {{{4u, 4u}}};
  const std::array<Range, 2> vertices = {{{8u, 4u}, {16u, 4u}}};
  const auto requests = fixture.requests(index, vertices);
  const auto valid = expectBatchStatus(fixture.pool, requests, Status::Valid,
                                      "index and vertex slices match in one coherent observation");
  check(valid.provedRequests == 2u && valid.provedRanges == 3u && valid.provedBytes == 12u &&
            valid.failedRequest == std::numeric_limits<std::uint32_t>::max() &&
            valid.failedRange == std::numeric_limits<std::uint32_t>::max(),
        "batch success reports exact aggregate counts and no failed member");
  fixture.patch(1u, 16u);
  const auto mismatch = expectBatchStatus(fixture.pool, requests, Status::ByteMismatch,
      "a second-buffer mismatch cannot expose the first-buffer success as partial authority");
  check(mismatch.failedRequest == 1u && mismatch.failedRange == 1u,
        "batch mismatch identifies its exact request and range");

  BatchFixture live(device, false);
  expectBatchStatus(live.pool, live.requests(index, vertices), Status::Valid,
                    "a coherent batch also supports unversioned live Shared backing");
  dxmt9::core::BufferDesc desc{};
  desc.size = BatchFixture::kBytes;
  desc.pool = dxmt9::core::Pool::Default;
  const auto liveHandle = fixture.pool.createBuffer(device, desc);
  check(liveHandle && fixture.pool.uploadBufferData(device, liveHandle.value,
      fixture.expected[1].data(), BatchFixture::kBytes, 0u), "mixed batch owns a live vertex backing");
  fixture.pool.markBufferUse(liveHandle, BatchFixture::kSource);
  auto mixed = requests;
  mixed[1].buffer = liveHandle;
  mixed[1].snapshot.reset();
  expectBatchStatus(fixture.pool, mixed, Status::Valid,
                    "captured index and unversioned live vertices share one batch lock");
}

void testBatchValidationBeforeComparisons(WMT::Device device) {
  BatchFixture fixture(device);
  const std::array<Range, 1> index = {{{4u, 4u}}};
  const std::array<Range, 2> vertices = {{{8u, 4u}, {16u, 4u}}};
  const auto valid = fixture.requests(index, vertices);
  fixture.patch(0u, 4u);
  auto requests = valid;
  requests[1].snapshot = fixture.captured[0].snapshot;
  const auto foreign = expectBatchStatus(fixture.pool, requests, Status::InvalidSnapshot,
      "all captured backings are validated before an earlier index mismatch is compared");
  check(foreign.failedRequest == 1u && foreign.failedRange == std::numeric_limits<std::uint32_t>::max(),
        "foreign backing identifies the second request without inventing a range failure");
  requests = valid;
  ++requests[1].snapshot->contentsAddress;
  expectBatchStatus(fixture.pool, requests, Status::InvalidSnapshot,
                    "stale or forged captured address rejects without a dereference");
  requests = valid;
  const std::array<Range, 1> outside = {{{31u, 2u}}};
  requests[1].ranges = outside;
  const auto invalid = expectBatchStatus(fixture.pool, requests, Status::InvalidRange,
      "a later member's invalid extent precedes any first-buffer comparison");
  check(invalid.failedRequest == 1u && invalid.failedRange == 0u,
        "invalid batch range has an exact request/range location");
  requests = valid;
  requests[1].expectedBytes = requests[1].expectedBytes.first(BatchFixture::kBytes - 1u);
  expectBatchStatus(fixture.pool, requests, Status::InvalidExpectedExtent,
                    "each batch expected image must remain complete");
  requests = valid;
  requests[1].limits.maxReadBytes = 7u;
  const auto perRequestBytes = expectBatchStatus(fixture.pool, requests, Status::CapacityExceeded,
      "the member's own byte budget applies before any earlier mismatch comparison");
  check(perRequestBytes.failedRequest == 1u && perRequestBytes.failedRange == 1u,
        "per-member byte rejection preserves exact location and zero batch proof counts");

  check(fixture.pool.markBufferDestroyAndGc(fixture.handles[1].value, BatchFixture::kSource),
        "batch stale-handle case ends the second logical lifetime");
  const auto missing = expectBatchStatus(fixture.pool, valid, Status::MissingBuffer,
      "a stale second handle is qualified before any first-buffer comparison");
  check(missing.failedRequest == 1u && missing.failedRange == std::numeric_limits<std::uint32_t>::max(),
        "missing buffer has an exact request but no range authority");
}

void testBatchWindowsAndAggregateBounds(WMT::Device device) {
  BatchFixture fixture(device);
  const std::array<Range, 1> index = {{{4u, 4u}}};
  const std::array<Range, 2> vertices = {{{8u, 4u}, {16u, 4u}}};
  const auto valid = fixture.requests(index, vertices);
  auto requests = valid;
  ++requests[1].sourceSequence;
  auto invalid = expectBatchStatus(fixture.pool, requests, Status::InvalidRequest,
      "a foreign source sequence cannot compose into one coherent source proof");
  check(invalid.failedRequest == 1u, "foreign source locates the differing member");
  requests = valid;
  ++requests[1].completedSequence;
  invalid = expectBatchStatus(fixture.pool, requests, Status::InvalidRequest,
                             "a differing completion waterline is not the same source window");
  check(invalid.failedRequest == 1u, "foreign waterline locates the differing member");
  expectBatchStatus(fixture.pool, {}, Status::InvalidRequest, "empty batches grant no proof");
  BatchLimits limits{.maxRequests = 2u, .maxRanges = 3u, .maxReadBytes = 12u};
  expectBatchStatus(fixture.pool, valid, Status::Valid, "exact aggregate budgets pass", limits);
  limits.maxRequests = 1u;
  invalid = expectBatchStatus(fixture.pool, valid, Status::CapacityExceeded,
                             "request count is bounded before arena inspection", limits);
  check(invalid.failedRequest == 1u, "request count rejects the first out-of-budget member");
  limits.maxRequests = 2u;
  limits.maxRanges = 2u;
  invalid = expectBatchStatus(fixture.pool, valid, Status::CapacityExceeded,
                             "aggregate ranges cannot hide across buffers", limits);
  check(invalid.failedRequest == 1u && invalid.failedRange == 1u,
        "aggregate range limit reports its first out-of-budget range");
  limits.maxRanges = 3u;
  limits.maxReadBytes = 11u;
  invalid = expectBatchStatus(fixture.pool, valid, Status::CapacityExceeded,
                             "aggregate compared bytes count every member", limits);
  check(invalid.failedRequest == 1u && invalid.failedRange == 1u,
        "aggregate byte limit reports its first out-of-budget range");
  limits.maxReadBytes = 12u;
  limits.maxRequests = 0u;
  expectBatchStatus(fixture.pool, valid, Status::InvalidRequest, "zero request budget rejects", limits);
  limits.maxRequests = dxmt9::resources::kBufferReadProofBatchMaxRequests + 1u;
  expectBatchStatus(fixture.pool, valid, Status::InvalidRequest, "hard request cap rejects", limits);
  limits.maxRequests = 2u;
  limits.maxRanges = 0u;
  expectBatchStatus(fixture.pool, valid, Status::InvalidRequest, "zero range budget rejects", limits);
  limits.maxRanges = dxmt9::resources::kBufferReadProofBatchMaxRanges + 1u;
  expectBatchStatus(fixture.pool, valid, Status::InvalidRequest, "hard aggregate range cap rejects", limits);
  limits.maxRanges = 3u;
  limits.maxReadBytes = 0u;
  expectBatchStatus(fixture.pool, valid, Status::InvalidRequest, "zero byte budget rejects", limits);
  requests = valid;
  requests[1].limits.maxRanges = 1u;
  invalid = expectBatchStatus(fixture.pool, requests, Status::CapacityExceeded,
                             "member range limits remain independent of batch limits");
  check(invalid.failedRequest == 1u, "member range budget locates its request");

  requests = valid;
  const std::array<Range, 1> wrapped = {{{std::numeric_limits<std::uint64_t>::max(), 2u}}};
  requests[1].ranges = wrapped;
  invalid = expectBatchStatus(fixture.pool, requests, Status::ArithmeticOverflow,
                             "batch range endpoints are checked before comparisons");
  check(invalid.failedRequest == 1u && invalid.failedRange == 0u,
        "overflowing endpoint has precise failure location");
  if constexpr (sizeof(std::size_t) == sizeof(std::uint64_t)) {
    const std::array<Range, 1> huge = {{{0u, std::numeric_limits<std::uint64_t>::max()}}};
    const std::array<Range, 1> one = {{{0u, 1u}}};
    requests = fixture.requests(huge, one);
    requests[0].limits.maxReadBytes = std::numeric_limits<std::uint64_t>::max();
    limits = {.maxRequests = 2u, .maxRanges = 2u,
              .maxReadBytes = std::numeric_limits<std::uint64_t>::max()};
    invalid = expectBatchStatus(fixture.pool, requests, Status::ArithmeticOverflow,
        "aggregate byte addition overflows before any backing comparison or huge dereference", limits);
    check(invalid.failedRequest == 1u && invalid.failedRange == 0u,
          "aggregate sum overflow grants no partial authority and locates the addition");
  }
}

void testBatchDuplicatesAndOldBacking(WMT::Device device) {
  BatchFixture fixture(device);
  const std::array<Range, 1> footprint = {{{4u, 4u}}};
  auto requests = fixture.requests(footprint, footprint);
  requests[1] = requests[0];
  auto valid = expectBatchStatus(fixture.pool, requests, Status::Valid,
                                "duplicate handles and ranges are legal batch members");
  check(valid.provedRequests == 2u && valid.provedRanges == 2u && valid.provedBytes == 8u,
        "duplicates are charged separately instead of silently deduplicated");
  check(fixture.pool.finalizeBufferMap(device, fixture.handles[0], dxmt9::core::UsageDiscard, 0u),
        "retained old backing forces a distinct current DISCARD allocation");
  std::array<std::uint8_t, BatchFixture::kBytes> bytesB{};
  bytesB.fill(0xa5u);
  check(fixture.pool.uploadBufferData(device, fixture.handles[0].value,
      bytesB.data(), bytesB.size(), 0u), "current renamed allocation receives its complete image");
  const auto current = fixture.pool.captureChunkBufferBinding(fixture.handles[0]);
  check(current.snapshot.valid() && current.snapshot.metalHandle != fixture.captured[0].snapshot.metalHandle,
        "old and new request backings are concretely distinct");
  fixture.pool.markBufferSnapshotUse(fixture.handles[0], current.snapshot, BatchFixture::kSource);
  const std::array<Range, 1> whole = {{{0u, BatchFixture::kBytes}}};
  requests[0].ranges = whole;
  requests[1] = requests[0];
  requests[1].snapshot = current.snapshot;
  requests[1].expectedBytes = bytesB;
  valid = expectBatchStatus(fixture.pool, requests, Status::Valid,
      "one batch can compare distinct genuine retained snapshots of the same logical buffer");
  check(valid.provedBytes == 64u, "both old and current images contribute to the aggregate byte budget");

  // Pin that the batch's new hard range/request caps do not narrow the old
  // single-request API's independently caller-bounded range semantics.
  std::vector<Range> many(dxmt9::resources::kBufferReadProofBatchMaxRanges + 1u, {4u, 1u});
  requests[0].ranges = many;
  requests[0].limits.maxRanges = many.size();
  requests[0].limits.maxReadBytes = many.size();
  const auto single = expectStatus(fixture.pool, requests[0], Status::Valid,
                                  "single-request behavior is not capped by new batch dimensions");
  check(single.provedRanges == many.size() && single.provedBytes == many.size(),
        "single proof retains its original large overlapping-range behavior");
}

void testArenaBatchKeepsOneSharedLock() {
  struct Record { unsigned value = 0u; };
  using Arena = dxmt9::resources::detail::HandleArena<
      Record, dxmt9::resources::detail::ResourceHandleKind::Buffer>;
  Arena arena;
  const std::array handles = {arena.insert(Record{}), arena.insert(Record{})};
  const auto handleSpan = std::span<const dxmt9::core::Handle>(handles);
  std::mutex signals;
  std::condition_variable wake;
  bool startWriter = false, writerEntering = false;
  std::atomic<bool> writerDone = false, writerFailed = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  std::jthread writer([&] {
    try {
      {
        std::unique_lock lock(signals);
        if (!wake.wait_until(lock, deadline, [&] { return startWriter; })) return;
        writerEntering = true;
        wake.notify_all();
      }
      arena.updateMany(handleSpan, [](Record& record) { record.value = 1u; });
      writerDone.store(true);
    } catch (...) { writerFailed.store(true); }
  });
  std::array<unsigned, 2> observed{};
  bool handshake = false, finishVisited = false, completedDuringInspection = false;
  bool inspected = false;
  try {
    // The synchronous inspection callbacks are the existing cold arena seam,
    // not a production diagnostic hook. The writer attempts one exclusive
    // two-record update after the first read, while the same shared lock still
    // protects the second read and final visitor. No assertion throws until
    // all gates are released and the writer is joined.
    inspected = arena.inspectMany(handleSpan,
        [](dxmt9::core::Handle handle) { return handle.value; },
        [&](const Record* record, dxmt9::core::Handle, std::size_t index) {
          if (!record) return false;
          observed[index] = record->value;
          if (!index) {
            std::unique_lock lock(signals);
            startWriter = true;
            wake.notify_all();
            handshake = wake.wait_until(lock, deadline, [&] { return writerEntering; });
            if (!handshake) return false;
          }
          return true;
        },
        [&] {
          finishVisited = true;
          completedDuringInspection = writerDone.load();
          return true;
        });
  } catch (...) {
    {
      std::lock_guard lock(signals);
      startWriter = true;
    }
    wake.notify_all();
    writer.join();
    throw;
  }
  {
    std::lock_guard lock(signals);
    startWriter = true;
  }
  wake.notify_all();
  writer.join();
  check(inspected && handshake && finishVisited && !completedDuringInspection &&
            observed[0] == 0u && observed[1] == 0u && writerDone.load() && !writerFailed.load(),
        "one shared inspection lock excludes a coordinated two-record writer until after final visitation");
  std::size_t updated = 0u;
  arena.inspectMany(handleSpan,
      [](dxmt9::core::Handle handle) { return handle.value; },
      [&](const Record* record, dxmt9::core::Handle, std::size_t) {
        if (!record || record->value != 1u) return false;
        ++updated;
        return true;
      }, [] { return true; });
  check(updated == handles.size(), "both writer values become visible only in a subsequent inspection");
}

} // namespace

int main() {
  try {
    // Keep the owning device array alive for every WMT allocation and Pool.
    auto devices = WMT::CopyAllDevices();
    if (!devices || devices.count() == 0u) {
      std::cerr << "SKIP: no Metal device\n";
      return 77;
    }
    const auto device = devices.object(0u);
    testCurrentVersionObservation(device);
    testNonversionedVersionObservation(device);
    testFreshFootprintComparison(device);
    testRangeValidationAndBudgets(device);
    testRequestAndSnapshotValidation(device);
    testOldDiscardBacking(device);
    testLiveRouteAndStaleHandle(device);
    testCoherentBatchEqualityAndMismatch(device);
    testBatchValidationBeforeComparisons(device);
    testBatchWindowsAndAggregateBounds(device);
    testBatchDuplicatesAndOldBacking(device);
    testArenaBatchKeepsOneSharedLock();
  } catch (const std::exception& failure) {
    std::cerr << "buffer_read_proof_spec failed: " << failure.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
