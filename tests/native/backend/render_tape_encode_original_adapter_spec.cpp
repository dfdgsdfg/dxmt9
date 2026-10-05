// Pure final-address closure plus real Shared Pool byte observations. Numeric
// issue receipts below model the adapter inputs, not an actual GPU invocation.
// The real-buffer fixture owns original immutable versions and captured backing
// residency throughout each call. No queue, command buffer or GPU work occurs.

#include "render_tape_encode_original_adapter.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dxmt9;
using namespace dxmt9::test;
using AddressStatus = OriginalEncodeAddressStatus;
using DirectStatus = OriginalEncodeDirectStatus;
using ProofStatus = resources::BufferReadProofStatus;
using Version = std::shared_ptr<const CpuTapeResourceVersion>;
constexpr std::uint64_t kSource = 7u, kQueue = 17u;
constexpr std::uint64_t kBase = 4u, kStride = 16u;
std::size_t checks = 0u;

void check(bool value, std::string_view message) {
  ++checks;
  if (!value) throw std::runtime_error(std::string(message));
}

template <typename T, std::size_t Count>
std::vector<std::uint8_t> bytesOf(const std::array<T, Count>& values) {
  std::vector<std::uint8_t> bytes(sizeof(values));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}

struct AddressFixture {
  AddressFixture() {
    parameters.primitiveType = core::PrimitiveType::TriangleList;
    parameters.primitiveCount = 1u;
    parameters.indexed = true;
    parameters.startIndex = 1u;
    parameters.baseVertexIndex = 2;
    indices = bytesOf(std::array<std::uint16_t, 5u>{99u, 1u, 5u, 5u, 77u});
    rebuildPlan();
    issue.draw = {kSource, 2u, 3u};
    issue.indexed = true;
    issue.primitiveType = WMTPrimitiveTypeTriangle;
    issue.instanceCount = 1u;
    issue.count = 3u;
    issue.indexElementBytes = 2u;
    issue.indexSlice = {21u, 2u, 6u};
    issue.boundStreamCount = 1u;
    issue.streams[0] = {.slot = 1u, .bindingOffset = kBase, .bindingBuffer = 11u};
    volatileValue.vertexBaseIndex = parameters.baseVertexIndex;
    volatileValue.vertexStreamStride = kStride;
  }

  void rebuildPlan() {
    check(buildCpuTapeSourceRowsPlan({.parameters = parameters,
        .actualEffectiveIndexed = parameters.indexed, .hasBoundIndexBuffer = parameters.indexed,
        .sourceBase = kBase, .sourceStride = kStride, .sourceByteSize = 256u,
        .expectedIndexBytes = indices}, plan) == CpuTapeSourceRowsStatus::Qualified,
        "fixture uses the actual source-row planner, not a test-local reducer");
  }

  OriginalEncodeAddressInput input() const {
    return {.parameters = parameters, .streamMask = 1u, .streamFrequency = 1u,
        .declarationSingleStream = true, .sourceBase = kBase, .sourceStride = kStride,
        .expectedVertexBacking = 11u, .expectedIndexBacking = 21u, .plan = &plan,
        .expectedIndexBytes = indices, .issue = &issue, .drawVolatile = volatileValue};
  }

  core::DrawParam parameters{};
  std::vector<std::uint8_t> indices;
  CpuTapeSourceRowsPlan plan;
  encode_witness::Issue issue{};
  state::DrawVolatile volatileValue{};
};

void testBaseVertexAddressClosure() {
  AddressFixture fixture;
  check(fixture.plan.indexRange && fixture.plan.indexRange->byteOffset == 2u &&
      fixture.plan.indexRange->byteSize == 6u && fixture.plan.uniqueVertexRows == 2u &&
      fixture.plan.vertexRanges.size() == 2u && fixture.plan.vertexRanges[0].byteOffset == 52u &&
      fixture.plan.vertexRanges[1].byteOffset == 116u,
      "nonzero startIndex, stream base and source base vertex distinguish the admitted original rows");
  check(qualifyOriginalEncodeAddressMapping(fixture.input()) == AddressStatus::Qualified,
      "ordinary indexed issue carries original base only in observed slot5");
  fixture.issue.baseVertex = 2;
  fixture.volatileValue.vertexBaseIndex = 0;
  check(qualifyOriginalEncodeAddressMapping(fixture.input()) == AddressStatus::Qualified,
      "native indexed base plus slot5 zero closes the same original source-row addresses");
  fixture.volatileValue.vertexBaseIndex = 2;
  check(qualifyOriginalEncodeAddressMapping(fixture.input()) == AddressStatus::BaseVertexMismatch,
      "applying source base in both native issue and slot5 is rejected");
  fixture.issue.baseVertex = 0;
  fixture.volatileValue.vertexBaseIndex = 0;
  check(qualifyOriginalEncodeAddressMapping(fixture.input()) == AddressStatus::BaseVertexMismatch,
      "omitting nonzero source base from both native issue and slot5 is rejected");
  fixture.issue.baseVertex = 1;
  fixture.volatileValue.vertexBaseIndex = 1;
  check(qualifyOriginalEncodeAddressMapping(fixture.input()) == AddressStatus::BaseVertexMismatch,
      "an unsupported split adjustment does not invent a production addressing route");
  fixture.parameters.baseVertexIndex = -1;
  fixture.issue.baseVertex = 0;
  fixture.volatileValue.vertexBaseIndex = -1;
  fixture.rebuildPlan();
  check(qualifyOriginalEncodeAddressMapping(fixture.input()) == AddressStatus::Qualified,
      "negative shader base is admitted only when every selected index stays in signed nonnegative domain");
  fixture.issue.baseVertex = -1;
  fixture.volatileValue.vertexBaseIndex = 0;
  check(qualifyOriginalEncodeAddressMapping(fixture.input()) == AddressStatus::BaseVertexMismatch,
      "negative native base is outside the actual supported native-base route");
  fixture.parameters.baseVertexIndex = 0;
  fixture.issue.baseVertex = 0;
  fixture.volatileValue.vertexBaseIndex = 0;
  fixture.rebuildPlan();
  check(qualifyOriginalEncodeAddressMapping(fixture.input()) == AddressStatus::Qualified,
      "zero source base has one unambiguous zero-adjustment closure");
}

void testBindingShapeAndDomainRejection() {
  AddressFixture fixture;
  for (std::uint32_t mismatch = 0u; mismatch != 10u; ++mismatch) {
    auto issue = fixture.issue;
    if (mismatch == 0u) issue.boundStreamCount = 0u;
    if (mismatch == 1u) issue.boundStreamCount = 2u;
    if (mismatch == 2u) issue.boundStreamCount = UINT8_MAX;
    if (mismatch == 3u) issue.streams[0].slot = 6u;
    if (mismatch == 4u) ++issue.streams[0].bindingBuffer;
    if (mismatch == 5u) ++issue.streams[0].bindingOffset;
    if (mismatch == 6u) ++issue.indexSlice.buffer;
    if (mismatch == 7u) ++issue.indexSlice.offset;
    if (mismatch == 8u) ++issue.indexSlice.size;
    if (mismatch == 9u) issue.indexElementBytes = 4u;
    auto input = fixture.input();
    input.issue = &issue;
    check(qualifyOriginalEncodeAddressMapping(input) == AddressStatus::FinalBindingMismatch,
        "exact final VB/IB backing, offset, extent, type and single-stream closure are required");
  }
  for (std::uint32_t mismatch = 0u; mismatch != 5u; ++mismatch) {
    auto issue = fixture.issue;
    if (mismatch == 0u) issue.indexed = false;
    if (mismatch == 1u) issue.primitiveType = WMTPrimitiveTypeLine;
    if (mismatch == 2u) ++issue.count;
    if (mismatch == 3u) ++issue.instanceCount;
    if (mismatch == 4u) ++issue.baseInstance;
    auto input = fixture.input();
    input.issue = &issue;
    check(qualifyOriginalEncodeAddressMapping(input) == AddressStatus::IssueShapeMismatch,
        "native primitive/count/indexed/instance shape must match the exact admitted source plan");
  }
  for (std::uint32_t mismatch = 0u; mismatch != 9u; ++mismatch) {
    auto input = fixture.input();
    if (mismatch == 0u) input.streamMask = 3u;
    if (mismatch == 1u) input.streamFrequency = 2u;
    if (mismatch == 2u) input.declarationSingleStream = false;
    if (mismatch == 3u) input.parameters.userVertexRange = {0u, 1u};
    if (mismatch == 4u) input.parameters.userIndexRange = {0u, 1u};
    if (mismatch == 5u) input.parameters.instanceCount = 2u;
    if (mismatch == 6u) input.plan = nullptr;
    if (mismatch == 7u) input.issue = nullptr;
    if (mismatch == 8u) input.sourceStride = std::uint64_t{UINT32_MAX} + 1u;
    check(qualifyOriginalEncodeAddressMapping(input) == AddressStatus::UnsupportedDomain,
        "extra streams, UP, instancing and malformed address input remain explicitly unsupported");
  }
  for (std::uint32_t mismatch = 0u; mismatch != 4u; ++mismatch) {
    auto input = fixture.input();
    if (mismatch == 0u) ++input.drawVolatile->vertexStreamStride;
    if (mismatch == 1u) ++input.drawVolatile->vertexStreamOffset;
    if (mismatch == 2u) ++input.drawVolatile->_pad;
    if (mismatch == 3u) input.drawVolatile->streamInstanceDivisors[15u] = 1u;
    check(qualifyOriginalEncodeAddressMapping(input) == AddressStatus::VolatileMismatch,
        "stale stride/offset, malformed padding and any instance divisor reject actual slot5 closure");
  }
}

void testNonindexedDisplacementAndSignedDomain() {
  AddressFixture fixture;
  fixture.parameters.indexed = false;
  fixture.parameters.startVertex = 3u;
  fixture.rebuildPlan();
  fixture.issue.indexed = false;
  fixture.issue.indexSlice = {};
  fixture.issue.indexElementBytes = 0u;
  fixture.issue.streams[0].bindingOffset = 52u;
  fixture.volatileValue.vertexBaseIndex = 0;
  check(qualifyOriginalEncodeAddressMapping(fixture.input()) == AddressStatus::Qualified,
      "nonindexed startVertex displacement is absorbed once in native VB offset with issue start/slot5 base zero");
  for (const auto offset : {4u, 100u}) {
    fixture.issue.streams[0].bindingOffset = offset;
    check(qualifyOriginalEncodeAddressMapping(fixture.input()) == AddressStatus::FinalBindingMismatch,
        "missing or twice-applied nonindexed start displacement rejects");
  }
  fixture.issue.streams[0].bindingOffset = 52u;
  fixture.issue.vertexStart = 3u;
  check(qualifyOriginalEncodeAddressMapping(fixture.input()) == AddressStatus::IssueShapeMismatch,
      "native nonindexed start cannot repeat a displacement already absorbed in the binding");
  fixture.issue.vertexStart = 0u;
  fixture.volatileValue.vertexBaseIndex = 3;
  check(qualifyOriginalEncodeAddressMapping(fixture.input()) == AddressStatus::BaseVertexMismatch,
      "slot5 cannot repeat the nonindexed start displacement");
  auto overflow = fixture.input();
  overflow.sourceBase = UINT64_MAX - 1u;
  overflow.drawVolatile->vertexBaseIndex = 0;
  check(qualifyOriginalEncodeAddressMapping(overflow) == AddressStatus::FinalBindingMismatch,
      "checked nonindexed displacement cannot wrap an overflowing source base");

  AddressFixture signedFixture;
  signedFixture.parameters.indexType = core::IndexType::UInt32;
  signedFixture.indices = bytesOf(std::array<std::uint32_t, 5u>{99u, 1u, 5u, 5u, 77u});
  signedFixture.rebuildPlan();
  signedFixture.issue.indexElementBytes = 4u;
  signedFixture.issue.indexSlice = {21u, 4u, 12u};
  check(qualifyOriginalEncodeAddressMapping(signedFixture.input()) == AddressStatus::Qualified,
      "UInt32 issue closes its real four-byte index extent");
  // Keep the actual planner's valid shape to isolate the adapter's own checked
  // index-domain guard; these pure cases grant no byte-proof authority.
  signedFixture.indices = bytesOf(std::array<std::uint32_t, 5u>{99u, UINT32_MAX, 5u, 5u, 77u});
  check(qualifyOriginalEncodeAddressMapping(signedFixture.input()) == AddressStatus::SignedVertexDomainRejected,
      "raw UInt32 values outside signed vertex_id domain cannot rely on implementation conversion");
  signedFixture.indices = bytesOf(std::array<std::uint32_t, 5u>{99u, 1u, 5u, 5u, 77u});
  signedFixture.parameters.baseVertexIndex = INT32_MAX;
  signedFixture.volatileValue.vertexBaseIndex = INT32_MAX;
  check(qualifyOriginalEncodeAddressMapping(signedFixture.input()) == AddressStatus::SignedVertexDomainRejected,
      "signed vertex addition overflow is not reinterpreted as widened address equivalence");
  signedFixture.parameters.baseVertexIndex = -2;
  signedFixture.volatileValue.vertexBaseIndex = -2;
  check(qualifyOriginalEncodeAddressMapping(signedFixture.input()) == AddressStatus::SignedVertexDomainRejected,
      "negative final vertex excludes shader clamp-to-zero as original address equivalence");
  signedFixture.parameters.baseVertexIndex = 2;
  signedFixture.volatileValue.vertexBaseIndex = 2;
  auto shortIndices = signedFixture.input();
  shortIndices.expectedIndexBytes = shortIndices.expectedIndexBytes.first(4u);
  check(qualifyOriginalEncodeAddressMapping(shortIndices) == AddressStatus::SignedVertexDomainRejected,
      "missing selected expected index bytes cannot supply address closure");
}

void testCurrentDrawVolatileDecoder() {
  AddressFixture fixture;
  OriginalEncodeVolatileObservation observation;
  auto input = fixture.input();
  input.drawVolatile = observation.value();
  check(qualifyOriginalEncodeAddressMapping(input) == AddressStatus::MissingVolatile,
      "no actual slot5 observation grants no addressing authority");
  observation.observe(&fixture.volatileValue, sizeof(fixture.volatileValue), 3u);
  check(!observation.value(), "other uniform slots cannot manufacture current DrawVolatile");
  observation.observe(&fixture.volatileValue, sizeof(fixture.volatileValue), 5u);
  check(observation.value() && observation.value()->vertexBaseIndex == 2,
      "exact actual slot5 POD is copied synchronously");
  input.drawVolatile = observation.value();
  check(qualifyOriginalEncodeAddressMapping(input) == AddressStatus::Qualified,
      "actual current slot5 decoder output supplies the qualifier's matching address constants");
  fixture.volatileValue.vertexBaseIndex = 9;
  check(observation.value()->vertexBaseIndex == 2, "slot5 observation owns values rather than an escaped pointer");
  observation.observeBufferBind(1u);
  check(observation.value().has_value(), "unrelated stream binding cannot invalidate slot5 values");
  observation.observeBufferBind(5u);
  check(!observation.value(), "native buffer replacement of slot5 invalidates prior inline constants");
  fixture.volatileValue.vertexBaseIndex = 2;
  for (const auto size : {0u, 79u, 81u}) {
    observation.observe(&fixture.volatileValue, sizeof(fixture.volatileValue), 5u);
    observation.observe(&fixture.volatileValue, size, 5u);
    check(!observation.value(), "malformed slot5 length clears earlier valid values without reading it");
  }
  observation.observe(&fixture.volatileValue, sizeof(fixture.volatileValue), 5u);
  observation.observe(nullptr, sizeof(fixture.volatileValue), 5u);
  check(!observation.value(), "null slot5 bytes cannot preserve earlier valid values");
  observation.observe(&fixture.volatileValue, sizeof(fixture.volatileValue), 5u);
  observation.beginDraw();
  input.drawVolatile = observation.value();
  check(qualifyOriginalEncodeAddressMapping(input) == AddressStatus::MissingVolatile,
      "beginDraw prevents another draw's slot5 constants from being reused as current observation");
}

void testObservationLifecycleDomain() {
  using Phase = OriginalEncodeObservationLifecycle::Phase;
  using Scope = OriginalEncodeSourceScope;
  OriginalEncodeObservationLifecycle lifecycle;
  check(lifecycle.phase() == Phase::AwaitingArm && !lifecycle.acceptsOriginalBodyRecord() &&
      !lifecycle.acceptsOriginalDraw(), "new observer awaits actual arm and admits no original body record or draw");
  check(lifecycle.classifySource(true, false) == Scope::SetupOutsideBody &&
      lifecycle.classifySource(true, true) == Scope::SetupOutsideBody,
      "valid setup source stays explicitly outside original body regardless of projection label");
  check(lifecycle.classifySource(false, false) == Scope::InvalidSource &&
      lifecycle.classifySource(false, true) == Scope::InvalidSource &&
      lifecycle.phase() == Phase::AwaitingArm, "invalid setup locator is not accepted as outside-body evidence");
  check(lifecycle.arm() && lifecycle.phase() == Phase::Armed &&
      lifecycle.acceptsOriginalBodyRecord() && lifecycle.acceptsOriginalDraw(),
      "one actual arm opens the original-body record and draw observation domain");
  check(!lifecycle.arm() && lifecycle.phase() == Phase::Armed,
      "duplicate arm rejects without replacing the already-armed observation domain");
  check(lifecycle.classifySource(true, false) == Scope::MissingOriginalBodyProjection &&
      lifecycle.phase() == Phase::Armed,
      "post-arm missing authentic origin cannot fall back to setup omission");
  check(lifecycle.classifySource(true, true) == Scope::OriginalBody &&
      lifecycle.classifySource(false, false) == Scope::InvalidSource &&
      lifecycle.classifySource(false, true) == Scope::InvalidSource,
      "armed source needs both a valid locator and authenticated original projection");
  check(lifecycle.close() && lifecycle.phase() == Phase::Closed &&
      !lifecycle.acceptsOriginalBodyRecord() && !lifecycle.acceptsOriginalDraw(),
      "actual close ends the original-body domain without reopening record or draw admission");
  check(lifecycle.classifySource(true, false) == Scope::TeardownOutsideBody &&
      lifecycle.classifySource(true, true) == Scope::TeardownOutsideBody &&
      lifecycle.classifySource(false, false) == Scope::InvalidSource &&
      lifecycle.classifySource(false, true) == Scope::InvalidSource,
      "valid teardown sources remain outside body while invalid locators still reject");
  check(!lifecycle.arm() && !lifecycle.close() && lifecycle.phase() == Phase::Closed,
      "closed domain cannot be armed again or closed twice");
  OriginalEncodeObservationLifecycle setupFailureCleanup;
  check(setupFailureCleanup.close() && setupFailureCleanup.phase() == Phase::Closed &&
      !setupFailureCleanup.arm(), "cleanup may close an unarmed observer but cannot turn it into body evidence");
}

void testActualPreArmAdapterCallbacks() {
  // Valid numeric locators isolate outside-body classification. They grant no
  // actual source publication, payload lifetime, queue or original-origin proof.
  const core::CpuReadyTape::SourceRef setup{
      .id = {.index = 1u, .generation = 2u},
      .storage = {.firstPage = 4u, .pageCount = 1u, .generation = 3u}};
  RenderTapeEncodeOriginalAdapter adapter({}, {});
  check(!adapter.failed() && !adapter.setupSourceFragments() && !adapter.sourceFragments(),
      "empty-owned actual adapter starts clean before provider arm");
  adapter.observeSource(setup, kSource);
  adapter.observeSource(setup, kSource); // Repeated callbacks count fragments, including continuations.
  auto anotherSetup = setup;
  ++anotherSetup.id.generation;
  ++anotherSetup.storage.generation;
  adapter.observeSource(anotherSetup, kSource + 1u);
  check(!adapter.failed() && adapter.setupSourceFragments() == 3u && !adapter.teardownSourceFragments() &&
      !adapter.sourceFragments() && adapter.receipts().empty() && !adapter.recordCount() &&
      !adapter.attemptedInspectionBytes() && !adapter.contextComplete() && !adapter.observationComplete(),
      "actual pre-arm setup source callbacks are counted without poisoning or manufacturing original-body evidence");
  adapter.beginDraw({kSource, 0u, 0u});
  check(adapter.failed() && adapter.setupDrawAttempts() == 1u && !adapter.teardownDrawAttempts() &&
      std::string_view(adapter.sourceFailure()) == "original-encode-draw-outside-armed-body" &&
      adapter.receipts().empty() && !adapter.attemptedInspectionBytes(),
      "pre-arm original draw attempt is counted and explicitly rejected, never silently omitted as setup");

  for (std::uint32_t mismatch = 0u; mismatch != 2u; ++mismatch) {
    RenderTapeEncodeOriginalAdapter invalid({}, {});
    invalid.observeSource(mismatch ? setup : core::CpuReadyTape::SourceRef{}, mismatch ? 0u : kSource);
    check(invalid.failed() && !invalid.setupSourceFragments() && !invalid.sourceFragments() &&
        invalid.receipts().empty() && !invalid.attemptedInspectionBytes(),
        "outside-body allowance still rejects an invalid source locator or zero sequence without proof reads");
  }
}

struct PoolFixture {
  explicit PoolFixture(WMT::Device actualDevice) : device(actualDevice) {
    expected[0].resize(256u);
    for (std::size_t index = 0u; index != expected[0].size(); ++index)
      expected[0][index] = static_cast<std::uint8_t>((index * 13u + 31u) & 0xffu);
    expected[1] = shape.indices;
    check(createCpuTapeExpectedBufferBudget({1024u}, budget) == CpuTapeResourceContextStatus::Accepted,
        "real fixture has an explicit retained original-byte budget");
    for (std::size_t index = 0u; index != handles.size(); ++index) {
      core::BufferDesc desc{};
      desc.size = expected[index].size();
      desc.pool = core::Pool::Default;
      desc.usage = core::UsageDynamic | (index ? core::UsageIndexBuffer : core::UsageVertexBuffer);
      handles[index] = pool.createBuffer(device, desc);
      check(bool(handles[index]) && pool.uploadBufferData(device, handles[index].value,
          expected[index].data(), expected[index].size(), 0u), "real Shared allocation receives complete original bytes");
      facts[index] = pool.observeBufferVersion(handles[index]);
      captures[index] = pool.captureChunkBufferBinding(handles[index]);
      check(facts[index].observed() && facts[index].hasVersionedBacking && captures[index].snapshot.valid() &&
          captures[index].backingResidency, "fixture retains actual historical facts and captured backing residency");
      pool.markBufferSnapshotUse(handles[index], captures[index].snapshot, kSource);
      const D9CWireObjectIdentity identity{D9C_CHUNK_HANDLE_KIND_BUFFER, 1u, 101u + index};
      const std::array receipts{CpuTapeResourceReceipt{.identity = identity, .revision = 3u}};
      CpuTapeResourceObservation logical;
      check(buildCpuTapeResourceObservation({.identity = identity, .contentIdentity = identity,
          .receipts = receipts}, logical) == CpuTapeResourceObservationStatus::Accepted,
          "original logical observation owns exact normalized identity receipts");
      check(buildBudgetedCpuTapeResourceVersion({.nativeKind = identity.kind, .nativeHandle = handles[index],
          .contentEpoch = 91u + index, .expectedBufferByteSize = expected[index].size(),
          .expectedBufferBytes = expected[index], .localBufferFacts = facts[index]}, logical, budget,
          versions[index]) == CpuTapeResourceContextStatus::Accepted,
          "immutable original byte version owns historical metadata and complete expected bytes");
    }
    shape.issue.streams[0].bindingBuffer = captures[0].snapshot.metalHandle;
    shape.issue.indexSlice.buffer = captures[1].snapshot.metalHandle;
  }

  OriginalEncodeDirectInput input() const {
    return {.address = shape.input(), .vertex = versions[0].get(), .index = versions[1].get(),
        .vertexSnapshot = captures[0].snapshot, .indexSnapshot = captures[1].snapshot,
        .sourceSequence = kSource, .expectedQueue = kQueue,
        .progress = {.queue = kQueue, .published = kSource, .completed = 1u}};
  }

  void patch(std::size_t buffer, std::uint64_t offset) {
    const auto changed = static_cast<std::uint8_t>(expected[buffer][offset] ^ 0xffu);
    check(pool.uploadBufferDataRange(device, handles[buffer].value, offset, &changed, 1u, 0u),
        "distinguishing byte mutation uses genuine arena-locked Pool upload");
  }

  WMT::Device device;
  resources::Pool pool;
  AddressFixture shape;
  std::array<core::Handle, 2u> handles{};
  std::array<std::vector<std::uint8_t>, 2u> expected{};
  std::array<resources::BufferVersionObservation, 2u> facts{};
  std::array<core::ChunkBufferBindingSnapshot, 2u> captures{};
  std::shared_ptr<CpuTapeExpectedBufferBudget> budget;
  std::array<Version, 2u> versions{};
};

void checkPreProof(const OriginalEncodeDirectObservation& result, DirectStatus status,
    std::string_view message) {
  check(result.status == status && !result.qualified() && !result.readProof &&
      !result.attemptedInspectionBytes, message);
}

void testCoherentOriginalBytesAndSelectedBacking(WMT::Device device) {
  PoolFixture fixture(device);
  const auto input = fixture.input();
  const auto first = observeOriginalEncodeDirectBinding(fixture.pool, input, 76u);
  check(first.qualified() && first.plannedReadBytes == 38u && first.attemptedInspectionBytes == 76u &&
      first.uniqueVertexRows == 2u && first.vertexRanges == 2u && first.readProof &&
      first.readProof->proved() && first.readProof->provedRequests == 2u &&
      first.readProof->provedRanges == 3u && first.readProof->provedBytes == 38u,
      "actual selected IB interval and sparse VB rows qualify together in one complete coherent batch");
  check(fixture.versions[0]->contentEpoch() != fixture.facts[0].contentRevision &&
      originalEncodeHistoricalSnapshotMatches(*fixture.versions[0], input.vertexSnapshot),
      "logical content epoch is not substituted for the original actual Pool revision");
  fixture.shape.issue.baseVertex = 2;
  fixture.shape.volatileValue.vertexBaseIndex = 0;
  check(observeOriginalEncodeDirectBinding(fixture.pool, fixture.input(), 76u).qualified(),
      "native-base route proves the same original physical rows as ordinary shader-base route");
  fixture.shape.issue.baseVertex = 0;
  fixture.shape.volatileValue.vertexBaseIndex = 2;
  fixture.patch(0u, 84u); // Unfetched row 5, between original rows 3 and 7.
  check(observeOriginalEncodeDirectBinding(fixture.pool, fixture.input(), 76u).qualified(),
      "legal gap mutation does not fill sparse source-row proof into whole-buffer equality");
  fixture.patch(0u, 52u);
  const auto changedVertex = observeOriginalEncodeDirectBinding(fixture.pool, fixture.input(), 76u);
  check(changedVertex.status == DirectStatus::PhysicalByteProofRejected && changedVertex.readProof &&
      changedVertex.readProof->status == ProofStatus::ByteMismatch && changedVertex.readProof->failedRequest == 1u &&
      !changedVertex.readProof->provedRequests && !changedVertex.readProof->provedRanges &&
      !changedVertex.readProof->provedBytes && changedVertex.attemptedInspectionBytes == 76u,
      "wrong real fetched VB byte conservatively rejects the entire batch and remains charged");
  checkPreProof(observeOriginalEncodeDirectBinding(fixture.pool, fixture.input(),
      76u - changedVertex.attemptedInspectionBytes), DirectStatus::InspectionBudgetExceeded,
      "charging a failed physical comparison prevents retrying it outside the caller's aggregate budget");

  PoolFixture wrongIndex(device);
  wrongIndex.patch(1u, 2u);
  const auto changedIndex = observeOriginalEncodeDirectBinding(wrongIndex.pool, wrongIndex.input(), 76u);
  check(changedIndex.status == DirectStatus::PhysicalByteProofRejected && changedIndex.readProof &&
      changedIndex.readProof->status == ProofStatus::ByteMismatch && changedIndex.readProof->failedRequest == 0u &&
      !changedIndex.readProof->provedRequests && changedIndex.attemptedInspectionBytes == 76u,
      "wrong actual selected IB rejects before expected indices can grant any partial VB proof");
}

void testSnapshotClosureAndProofBudgets(WMT::Device device) {
  PoolFixture fixture(device);
  for (std::uint32_t mismatch = 0u; mismatch != 6u; ++mismatch) {
    auto input = fixture.input();
    if (mismatch == 0u) input.vertexSnapshot.reset();
    if (mismatch == 1u) ++input.vertexSnapshot->contentRevision;
    if (mismatch == 2u) input.vertexSnapshot->metalHandle = fixture.captures[1].snapshot.metalHandle;
    if (mismatch == 3u) ++input.indexSnapshot->contentRevision;
    if (mismatch == 4u) input.indexSnapshot->metalHandle = fixture.captures[0].snapshot.metalHandle;
    if (mismatch == 5u) ++input.vertexSnapshot->byteSize;
    checkPreProof(observeOriginalEncodeDirectBinding(fixture.pool, input, 76u),
        DirectStatus::SnapshotAssociationMismatch,
        "missing/stale/wrong selected historical VB or IB snapshot rejects before any proof reads");
  }
  auto invalidAddress = fixture.input();
  ++invalidAddress.vertexSnapshot->contentsAddress;
  const auto invalid = observeOriginalEncodeDirectBinding(fixture.pool, invalidAddress, 76u);
  check(invalid.status == DirectStatus::PhysicalByteProofRejected && invalid.readProof &&
      invalid.readProof->status == ProofStatus::InvalidSnapshot && invalid.attemptedInspectionBytes == 76u,
      "Pool authenticates altered captured contents address without dereferencing an invented pointer");
  for (std::uint32_t mismatch = 0u; mismatch != 6u; ++mismatch) {
    auto issue = fixture.shape.issue;
    auto input = fixture.input();
    input.address.issue = &issue;
    if (mismatch == 0u) issue.streams[0].bindingBuffer = fixture.captures[1].snapshot.metalHandle;
    if (mismatch == 1u) issue.indexSlice.buffer = fixture.captures[0].snapshot.metalHandle;
    if (mismatch == 2u) ++issue.streams[0].bindingOffset;
    if (mismatch == 3u) ++issue.indexSlice.offset;
    if (mismatch == 4u) issue.boundStreamCount = 2u;
    if (mismatch == 5u) ++input.address.drawVolatile->vertexStreamStride;
    checkPreProof(observeOriginalEncodeDirectBinding(fixture.pool, input, 76u),
        DirectStatus::AddressMappingRejected,
        "wrong actual native backing/offset/stride or extra native stream cannot reach physical byte proof");
  }
  for (std::uint32_t mismatch = 0u; mismatch != 6u; ++mismatch) {
    auto input = fixture.input();
    if (mismatch == 0u) input.address.streamMask = 3u;
    if (mismatch == 1u) input.address.streamFrequency = 2u;
    if (mismatch == 2u) input.address.declarationSingleStream = false;
    if (mismatch == 3u) input.address.parameters.userVertexRange = {0u, 1u};
    if (mismatch == 4u) input.address.parameters.userIndexRange = {0u, 1u};
    if (mismatch == 5u) input.address.parameters.instanceCount = 2u;
    checkPreProof(observeOriginalEncodeDirectBinding(fixture.pool, input, 76u),
        DirectStatus::UnsupportedDomain,
        "extra streams, UP or instancing reject genuine direct inputs before planner or physical proof");
  }
  auto missingVolatile = fixture.input();
  missingVolatile.address.drawVolatile.reset();
  checkPreProof(observeOriginalEncodeDirectBinding(fixture.pool, missingVolatile, 76u),
      DirectStatus::AddressMappingRejected, "missing actual slot5 cannot borrow genuine source bytes as substitute authority");

  checkPreProof(observeOriginalEncodeDirectBinding(fixture.pool, fixture.input(), 75u),
      DirectStatus::InspectionBudgetExceeded, "one-under aggregate input-plus-physical-byte budget rejects before batch");
  checkPreProof(observeOriginalEncodeDirectBinding(fixture.pool, fixture.input(), 0u),
      DirectStatus::InspectionBudgetExceeded, "zero remaining aggregate budget grants no physical comparison");
  for (std::uint32_t mismatch = 0u; mismatch != 3u; ++mismatch) {
    CpuTapeSourceRowsLimits limits;
    limits.maxReadBytes = 64u * 1024u * 1024u;
    if (mismatch == 0u) limits.maxRows = 2u;
    if (mismatch == 1u) limits.maxRanges = 2u;
    if (mismatch == 2u) limits.maxReadBytes = 37u;
    const auto result = observeOriginalEncodeDirectBinding(fixture.pool, fixture.input(), 76u, limits);
    checkPreProof(result, DirectStatus::PlannerRejected, "row/range/member-byte work caps reject before physical batch");
    const auto expected = mismatch == 0u ? CpuTapeSourceRowsStatus::RowWorkLimitExceeded :
        mismatch == 1u ? CpuTapeSourceRowsStatus::RangeLimitExceeded : CpuTapeSourceRowsStatus::ReadByteLimitExceeded;
    check(result.plannerStatus == expected, "tiny limits preserve exact planner rejection reason");
  }
  for (std::uint32_t mismatch = 0u; mismatch != 4u; ++mismatch) {
    CpuTapeSourceRowsLimits limits{32768u, 32769u, 64u * 1024u * 1024u};
    auto remaining = std::uint64_t{76u};
    if (mismatch == 0u) ++limits.maxRows;
    if (mismatch == 1u) ++limits.maxRanges;
    if (mismatch == 2u) ++limits.maxReadBytes;
    if (mismatch == 3u) remaining = 1024u * 1024u * 1024u + 1u;
    checkPreProof(observeOriginalEncodeDirectBinding(fixture.pool, fixture.input(), remaining, limits),
        DirectStatus::UnsupportedDomain, "one-over fixed row/range/member/aggregate ceilings grant no inspection");
  }
  auto staleWindow = fixture.input();
  staleWindow.progress.completed = kSource;
  checkPreProof(observeOriginalEncodeDirectBinding(fixture.pool, staleWindow, 76u),
      DirectStatus::SourceWindowRejected, "completed source cannot re-enter instantaneous live-Encode proof window");
  staleWindow = fixture.input();
  ++staleWindow.progress.queue;
  checkPreProof(observeOriginalEncodeDirectBinding(fixture.pool, staleWindow, 76u),
      DirectStatus::SourceWindowRejected, "foreign queue progress cannot authorize original source proof");

  check(resources::dynamicBufferRenameEnabled(), "old generation fixture uses the default production rename policy");
  check(fixture.pool.finalizeBufferMap(device, fixture.handles[0], core::UsageDiscard, 0u) != nullptr,
      "genuine DISCARD rotates while original selected backing remains retained");
  std::vector<std::uint8_t> replacement(fixture.expected[0].size(), 0xeeu);
  check(fixture.pool.uploadBufferData(device, fixture.handles[0].value, replacement.data(), replacement.size(), 0u),
      "real replacement generation receives distinguishing bytes");
  const auto current = fixture.pool.captureChunkBufferBinding(fixture.handles[0]);
  check(current.snapshot.metalHandle != fixture.captures[0].snapshot.metalHandle &&
      observeOriginalEncodeDirectBinding(fixture.pool, fixture.input(), 76u).qualified(),
      "proof follows actual retained old selected backing, never substituted current generation");
  auto staleSnapshot = fixture.input();
  staleSnapshot.vertexSnapshot = current.snapshot;
  checkPreProof(observeOriginalEncodeDirectBinding(fixture.pool, staleSnapshot, 76u),
      DirectStatus::SnapshotAssociationMismatch, "new generation snapshot cannot replace the original historical selection");
}

void testNonindexedRetainedIndexRegression(WMT::Device device) {
  PoolFixture fixture(device);
  fixture.shape.parameters.indexed = false;
  fixture.shape.parameters.startVertex = 3u;
  fixture.shape.issue.indexed = false;
  fixture.shape.issue.streams[0].bindingOffset = 52u;
  fixture.shape.issue.indexSlice = {};
  fixture.shape.issue.indexElementBytes = 0u;
  fixture.shape.volatileValue.vertexBaseIndex = 0;
  // A retained but unconsumed IB is not an index range. The explicit rejection
  // occurs before dereferencing the absent nonindexed plan.indexRange.
  checkPreProof(observeOriginalEncodeDirectBinding(fixture.pool, fixture.input(), 96u),
      DirectStatus::UnsupportedDomain, "nonindexed draw retaining an unselected IB rejects before missing-range access");
  auto input = fixture.input();
  input.index = nullptr;
  checkPreProof(observeOriginalEncodeDirectBinding(fixture.pool, input, 96u),
      DirectStatus::UnsupportedDomain, "unselected index snapshot alone cannot invent an IB request");
  input.indexSnapshot.reset();
  const auto result = observeOriginalEncodeDirectBinding(fixture.pool, input, 96u);
  check(result.qualified() && result.plannedReadBytes == 48u && result.attemptedInspectionBytes == 96u &&
      result.readProof && result.readProof->provedRequests == 1u && result.readProof->provedRanges == 1u &&
      result.readProof->provedBytes == 48u,
      "genuine nonindexed draw consumes only displaced contiguous VB rows in one request");
}

} // namespace

int main() {
  try {
    testBaseVertexAddressClosure();
    testBindingShapeAndDomainRejection();
    testNonindexedDisplacementAndSignedDomain();
    testCurrentDrawVolatileDecoder();
    testObservationLifecycleDomain();
    testActualPreArmAdapterCallbacks();
    auto devices = WMT::CopyAllDevices();
    if (!devices || !devices.count()) {
      std::cerr << "SKIP: no Metal device; pure adapter checks: " << checks << '\n';
      return 77;
    }
    const auto device = devices.object(0u);
    testCoherentOriginalBytesAndSelectedBacking(device);
    testSnapshotClosureAndProofBudgets(device);
    testNonindexedRetainedIndexRegression(device);
    std::cout << "original encode adapter checks: " << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "original encode adapter failure: " << error.what() << '\n';
    return 1;
  }
}
