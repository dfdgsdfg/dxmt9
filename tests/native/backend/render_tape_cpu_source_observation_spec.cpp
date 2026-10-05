// Real Shared backing comparisons only; no command buffers or GPU execution.
// Finite source/progress values isolate the cold helper's window/retention
// predicates, not actual queue tickets or source ownership. The executor's
// routing spec covers the genuine pinned source/origin/context attachment.
// Each fixture retains its Pool and genuine captured residency throughout.

#include "render_tape_cpu_source_observation.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace dxmt9;
using namespace dxmt9::test;
using Status = CpuTapeSourceObservationStatus;
using ProofStatus = resources::BufferReadProofStatus;
using Version = std::shared_ptr<const CpuTapeResourceVersion>;
using Context = std::shared_ptr<const CpuTapeResourceContext>;
constexpr std::uint64_t kSource = 7u, kQueue = 17u;
std::size_t checks = 0u;
static_assert(std::is_trivially_copyable_v<CpuTapeSourceObservation>);

void check(bool value, std::string_view message) {
  ++checks;
  if (!value) throw std::runtime_error(std::string(message));
}

void rejected(const CpuTapeSourceObservation& observation, Status status) {
  check(observation.status == status && !observation.qualified(),
      "rejection has its typed qualification reason");
  check(observation.sourceSequence == kSource && observation.completedSequence == 1u,
      "rejection preserves the supplied source/completed observation");
  if (observation.readProof)
    check(!observation.readProof->provedRequests && !observation.readProof->provedRanges &&
        !observation.readProof->provedBytes, "rejected batch exposes no partial proof counts");
}

struct Fixture {
  explicit Fixture(WMT::Device actualDevice, bool versioned = true,
      std::size_t vertexBytes = 128u, std::vector<std::uint16_t> indices = {1u, 5u, 5u})
      : device(actualDevice) {
    expected[0].resize(vertexBytes);
    for (std::size_t index = 0u; index < vertexBytes; ++index)
      expected[0][index] = static_cast<std::uint8_t>((index * 13u + 31u) & 0xffu);
    expected[1].resize(indices.size() * sizeof(std::uint16_t));
    std::memcpy(expected[1].data(), indices.data(), expected[1].size());
    for (std::size_t index = 0u; index < handles.size(); ++index) {
      core::BufferDesc desc{};
      desc.size = expected[index].size();
      desc.pool = core::Pool::Default;
      if (versioned) desc.usage = core::UsageDynamic;
      handles[index] = pool.createBuffer(device, desc);
      check(bool(handles[index]), "fixture owns a real Shared buffer allocation");
      check(pool.uploadBufferData(device, handles[index].value, expected[index].data(),
          expected[index].size(), 0u), "real complete upload precedes historical observation");
      facts[index] = pool.observeBufferVersion(handles[index]);
      check(facts[index].observed() && facts[index].hasVersionedBacking == versioned,
          "historical facts are pulled from the actual successfully initialized buffer");
      if (versioned) {
        captures[index] = pool.captureChunkBufferBinding(handles[index]);
        check(captures[index].snapshot.valid() && captures[index].backingResidency,
            "dynamic fixture retains the genuine captured backing");
        pool.markBufferSnapshotUse(handles[index], captures[index].snapshot, kSource);
      } else pool.markBufferUse(handles[index], kSource);
    }
    check(createCpuTapeExpectedBufferBudget({1024u * 1024u}, budget) ==
        CpuTapeResourceContextStatus::Accepted, "fixture owns an explicit expected-byte budget");
    bindContext();
    hot.streamMask = 1u;
    hot.streamBuffers[0] = handles[0];
    hot.streamOffsets[0] = 4u;
    hot.streamStrides[0] = 16u;
    hot.streamFrequencies[0] = 1u;
    hot.indexBuffer = handles[1];
    layout.vertexDecl.streams[0].offset = hot.streamOffsets[0];
    layout.vertexDecl.streams[0].stride = hot.streamStrides[0];
    layout.vertexDecl.fvf = 2u; // XYZ, decoded through the production FFP helper.
    layout.vertexShader.kind = core::ShaderRef::Kind::FixedFunctionVertex;
    parameters.primitiveType = core::PrimitiveType::TriangleList;
    parameters.primitiveCount = 1u;
    parameters.indexed = true;
    parameters.indexType = core::IndexType::UInt16;
    if (versioned) {
      snapshot.streamMask = 1u;
      snapshot.streams[0] = {.buffer = handles[0], .offset = hot.streamOffsets[0],
          .stride = hot.streamStrides[0], .snapshot = captures[0].snapshot};
      snapshot.indexBuffer = handles[1];
      snapshot.indexType = parameters.indexType;
      snapshot.indexSnapshot = captures[1].snapshot;
      snapshot.indexSnapshotValid = true;
    }
  }

  void bindContext(bool vertexFacts = true, bool includeVertex = true, bool includeIndex = true) {
    std::vector<Version> versions;
    for (std::size_t index = 0u; index < handles.size(); ++index) {
      if (index ? !includeIndex : !includeVertex) continue;
      const D9CWireObjectIdentity identity{D9C_CHUNK_HANDLE_KIND_BUFFER, 1u, 101u + index};
      const std::array receipts{CpuTapeResourceReceipt{.identity = identity, .revision = 3u}};
      CpuTapeResourceObservation logical;
      check(buildCpuTapeResourceObservation({.identity = identity, .contentIdentity = identity,
          .receipts = receipts}, logical) == CpuTapeResourceObservationStatus::Accepted,
          "logical observation has owned normalized receipt values");
      Version version;
      check(buildBudgetedCpuTapeResourceVersion({.nativeKind = identity.kind,
          .nativeHandle = handles[index], .contentEpoch = 91u + index,
          .expectedBufferByteSize = expected[index].size(), .expectedBufferBytes = expected[index],
          .localBufferFacts = index || vertexFacts
              ? std::optional<resources::BufferVersionObservation>(facts[index]) : std::nullopt},
          logical, budget, version) == CpuTapeResourceContextStatus::Accepted,
          "immutable historical version owns a complete expected image and original facts");
      versions.push_back(std::move(version));
    }
    check(buildCpuTapeV4SurfaceAliasInventory(versions, budget, context) ==
        CpuTapeResourceContextStatus::Accepted, "fixture uses the actual V4 inventory context purpose");
  }

  CpuTapeSourceObservation observe(CpuTapeSourceRowsLimits limits = {}) const {
    return observeCpuTapeSourceRows(pool, hot, &layout, parameters, &snapshot, *context,
        kSource, progress, kQueue, limits);
  }

  void patch(std::size_t buffer, std::uint64_t offset) {
    const std::uint8_t value = static_cast<std::uint8_t>(expected[buffer][offset] ^ 0xffu);
    check(pool.uploadBufferDataRange(device, handles[buffer].value, offset, &value, 1u, 0u),
        "distinguishing mutation uses the production arena-locked upload");
  }

  WMT::Device device;
  resources::Pool pool;
  std::array<core::Handle, 2u> handles{};
  std::array<std::vector<std::uint8_t>, 2u> expected{};
  std::array<resources::BufferVersionObservation, 2u> facts{};
  std::array<core::ChunkBufferBindingSnapshot, 2u> captures{};
  std::shared_ptr<CpuTapeExpectedBufferBudget> budget;
  Context context;
  core::FlatDrawStateRecord hot{};
  core::DrawShaderLayoutContext layout{};
  core::DrawParam parameters{};
  core::DrawBindingSnapshot snapshot{};
  query::Progress progress{.queue = kQueue, .published = kSource, .completed = 1u};
};

void testMatchingSparseAndGapMutation(WMT::Device device) {
  Fixture fixture(device);
  const auto first = fixture.observe();
  check(first.qualified() && first.effectiveIndexed && first.sourceBase == 4u &&
      first.sourceStride == 16u && first.sourceByteSize == 128u &&
      first.plannerStatus == CpuTapeSourceRowsStatus::Qualified &&
      first.qualifiedPrimitiveCount == 1u && first.sourceElementCount == 3u &&
      first.uniqueVertexRows == 2u && first.vertexRangeCount == 2u &&
      first.indexRange && first.indexRange->byteOffset == 0u && first.indexRange->byteSize == 6u &&
      first.plannedReadBytes == 38u, "duplicate indexed rows deduplicate without filling the gap");
  check(first.readProof && first.readProof->proved() && first.readProof->provedRequests == 2u &&
      first.readProof->provedRanges == 3u && first.readProof->provedBytes == 38u,
      "IB and sparse VB rows are observed in one complete coherent batch");
  const auto original = fixture.context->find(D9C_CHUNK_HANDLE_KIND_BUFFER, fixture.handles[0]);
  check(original->contentEpoch() == 91u &&
      original->contentEpoch() != original->localBufferFacts()->contentRevision,
      "CPU logical epoch is deliberately independent from the real Pool revision");
  fixture.patch(0u, 52u); // Unfetched row 3, strictly between rows 1 and 5.
  check(fixture.pool.observeBufferVersion(fixture.handles[0]).contentRevision !=
      original->localBufferFacts()->contentRevision, "legal gap write advances only current Pool facts");
  check(fixture.observe().qualified(), "legal gap mutation preserves original scoped source-row equality");
  fixture.patch(0u, 20u);
  const auto changed = fixture.observe();
  rejected(changed, Status::BatchRejected);
  check(changed.readProof && changed.readProof->status == ProofStatus::ByteMismatch &&
      changed.readProof->failedRequest == 1u && changed.readProof->failedRange == 0u,
      "fetched row mismatch is a conservative typed result with no corruption claim");

  Fixture changedIndex(device);
  changedIndex.patch(1u, 0u);
  const auto ib = changedIndex.observe();
  rejected(ib, Status::BatchRejected);
  check(ib.readProof && ib.readProof->status == ProofStatus::ByteMismatch &&
      ib.readProof->failedRequest == 0u, "actual IB must match before expected indices grant a row proof");
}

void testSnapshotAssociationAndOldBacking(WMT::Device device) {
  Fixture fixture(device);
  fixture.snapshot.streamMask = 0u;
  rejected(fixture.observe(), Status::SnapshotAssociationMismatch);
  fixture.snapshot.streamMask = 1u;
  ++fixture.snapshot.streams[0].snapshot.contentRevision;
  const auto revision = fixture.observe();
  rejected(revision, Status::SnapshotAssociationMismatch);
  check(!revision.readProof, "copied snapshot revision mismatch rejects before the physical batch");
  fixture.snapshot.streams[0].snapshot = fixture.captures[0].snapshot;
  ++fixture.snapshot.streams[0].snapshot.contentsAddress;
  const auto address = fixture.observe();
  rejected(address, Status::BatchRejected);
  check(address.readProof && address.readProof->status == ProofStatus::InvalidSnapshot,
      "Pool checks captured address identity without escaping or dereferencing a forged pointer");
  fixture.snapshot.streams[0].snapshot = fixture.captures[0].snapshot;
  fixture.snapshot.streams[0].snapshot.metalHandle = fixture.captures[1].snapshot.metalHandle;
  rejected(fixture.observe(), Status::SnapshotAssociationMismatch);
  fixture.snapshot.streams[0].snapshot = fixture.captures[0].snapshot;
  fixture.patch(0u, 52u);
  fixture.snapshot.streams[0].snapshot = fixture.pool.captureChunkBufferBinding(fixture.handles[0]).snapshot;
  rejected(fixture.observe(), Status::SnapshotAssociationMismatch);
  fixture.snapshot.streams[0].snapshot = fixture.captures[0].snapshot;
  check(resources::dynamicBufferRenameEnabled(), "old-backing witness uses the default rename policy");
  check(fixture.pool.finalizeBufferMap(device, fixture.handles[0], core::UsageDiscard, 0u) != nullptr,
      "genuine DISCARD rotates while the captured old residency remains retained");
  std::vector<std::uint8_t> replacement(fixture.expected[0].size(), 0xeeu);
  check(fixture.pool.uploadBufferData(device, fixture.handles[0].value, replacement.data(),
      replacement.size(), 0u), "replacement writes only the actual current backing");
  check(fixture.pool.observeBufferVersion(fixture.handles[0]).currentBacking.metalHandle !=
      fixture.facts[0].currentBacking.metalHandle, "current backing differs from historical captured metadata");
  check(fixture.observe().qualified(), "original source rows qualify against their retained old backing");
}

void testNonindexedAndStrideFallback(WMT::Device device) {
  Fixture fixture(device, false);
  fixture.parameters.indexed = false;
  fixture.parameters.startVertex = 2u;
  // A present payload with unselected invalid snapshots is not a binding.
  fixture.snapshot.streamMask = 1u;
  fixture.snapshot.streams[0].buffer = {};
  const auto direct = fixture.observe();
  check(direct.qualified() && !direct.effectiveIndexed && !direct.indexRange &&
      direct.sourceStride == 16u && direct.uniqueVertexRows == 3u &&
      direct.vertexRangeCount == 1u && direct.plannedReadBytes == 48u &&
      direct.readProof && direct.readProof->provedRequests == 1u &&
      direct.readProof->provedRanges == 1u && direct.readProof->provedBytes == 48u,
      "nonindexed bound-stride interval uses one live-buffer proof request");
  fixture.hot.streamStrides[0] = fixture.layout.vertexDecl.streams[0].stride = 0u;
  fixture.layout.vertexDecl.fvf = 0u;
  fixture.layout.vertexDecl.elements = {{.stream = 0u, .offset = 4u, .type = 1u, .usage = 5u}};
  fixture.layout.vertexShader.kind = core::ShaderRef::Kind::Bytecode;
  const auto fallback = fixture.observe();
  check(fallback.qualified() && fallback.sourceStride == 12u && fallback.plannedReadBytes == 36u,
      "programmable no-FFP-layout computed stride can differ from both raw zero strides");
  fixture.parameters.indexed = true;
  fixture.hot.indexBuffer = {};
  const auto fallthrough = fixture.observe();
  check(fallthrough.qualified() && !fallthrough.effectiveIndexed,
      "requested indexed draw with no IB preserves genuine nonindexed fallthrough");
  fixture.layout.vertexShader.kind = core::ShaderRef::Kind::FixedFunctionVertex;
  rejected(fixture.observe(), Status::UnsupportedDomain);
}

void testDomainFactsAndLimits(WMT::Device device) {
  Fixture fixture(device);
  auto observation = observeCpuTapeSourceRows(fixture.pool, fixture.hot, nullptr,
      fixture.parameters, &fixture.snapshot, *fixture.context, kSource, fixture.progress, kQueue);
  rejected(observation, Status::LayoutUnavailable);
  fixture.layout.vertexDecl.elements = {{.stream = 1u, .type = 2u, .usage = 0u}};
  rejected(fixture.observe(), Status::UnsupportedDomain);
  fixture.layout.vertexDecl.elements.count =
      static_cast<core::u8>(fixture.layout.vertexDecl.elements.max_size() + 1u);
  rejected(fixture.observe(), Status::UnsupportedDomain);
  fixture.layout.vertexDecl.elements.clear();
  ++fixture.layout.vertexDecl.streams[0].offset;
  rejected(fixture.observe(), Status::BindingMismatch);
  fixture.layout.vertexDecl.streams[0].offset = fixture.hot.streamOffsets[0];
  fixture.parameters.userVertexRange = {900u, 1u};
  rejected(fixture.observe(), Status::UnsupportedDomain);
  fixture.parameters.userVertexRange = {900u, 0u};
  check(fixture.observe().qualified(), "size-zero UP range remains absent even with nonzero offset");
  fixture.parameters.userVertexRange = {};
  fixture.hot.streamFrequencies[0] = 2u;
  rejected(fixture.observe(), Status::UnsupportedDomain);
  fixture.hot.streamFrequencies[0] = 1u;
  fixture.parameters.instanceCount = 2u;
  rejected(fixture.observe(), Status::UnsupportedDomain);
  fixture.parameters.instanceCount = 1u;
  fixture.bindContext(false);
  rejected(fixture.observe(), Status::MissingBufferFacts);
  fixture.bindContext(true, false);
  rejected(fixture.observe(), Status::MissingBufferVersion);
  fixture.bindContext();
  observation = fixture.observe({.maxRows = 2u});
  rejected(observation, Status::PlannerRejected);
  check(observation.plannerStatus == CpuTapeSourceRowsStatus::RowWorkLimitExceeded &&
      !observation.readProof, "row-work cap rejects before physical comparison");
  observation = fixture.observe({.maxRanges = 2u});
  rejected(observation, Status::PlannerRejected);
  check(observation.plannerStatus == CpuTapeSourceRowsStatus::RangeLimitExceeded,
      "range cap includes the IB interval plus exact sparse VB rows");
  observation = fixture.observe({.maxReadBytes = 37u});
  rejected(observation, Status::PlannerRejected);
  check(observation.plannerStatus == CpuTapeSourceRowsStatus::ReadByteLimitExceeded,
      "byte cap includes index and vertex comparisons");
  fixture.parameters.startIndex = 3u;
  observation = fixture.observe();
  rejected(observation, Status::PlannerRejected);
  check(observation.plannerStatus == CpuTapeSourceRowsStatus::IndexBoundsExceeded,
      "logical IB extent rejects before decoding or comparing unavailable rows");
  fixture.parameters.startIndex = 0u;
  fixture.progress.failed = true;
  rejected(fixture.observe(), Status::SourceWindowRejected);
  fixture.progress.failed = false;
  fixture.progress.queue = kQueue + 1u;
  rejected(fixture.observe(), Status::SourceWindowRejected);
  fixture.progress.queue = kQueue;
  fixture.progress.completed = kSource;
  observation = fixture.observe();
  check(observation.status == Status::SourceWindowRejected &&
      observation.completedSequence == kSource && !observation.readProof,
      "completed source rejection preserves the genuine supplied completed value");
  for (std::uint8_t value = 0u; value <= static_cast<std::uint8_t>(Status::BatchRejected); ++value)
    check(std::string_view(cpuTapeSourceObservationStatusName(static_cast<Status>(value))) != "unknown",
        "each typed status has a stable nonnumeric CLI name");
}

void testExplicitSparseProofLimits(WMT::Device device) {
  std::vector<std::uint16_t> indices(513u);
  for (std::size_t index = 0u; index < indices.size(); ++index)
    indices[index] = static_cast<std::uint16_t>(index * 2u);
  Fixture fixture(device, true, 16404u, std::move(indices));
  fixture.parameters.primitiveType = core::PrimitiveType::PointList;
  fixture.parameters.primitiveCount = 513u;
  const auto observation = fixture.observe();
  check(observation.qualified() && observation.uniqueVertexRows == 513u &&
      observation.vertexRangeCount == 513u && observation.readProof &&
      observation.readProof->provedRequests == 2u && observation.readProof->provedRanges == 514u &&
      observation.readProof->provedBytes == 9234u,
      "explicit compatible member and batch limits admit more than the Pool default 512 ranges");
}

}  // namespace

int main() {
  try {
    auto devices = WMT::CopyAllDevices();
    if (!devices || !devices.count()) {
      std::cerr << "SKIP: no Metal device\n";
      return 77;
    }
    const auto device = devices.object(0u);
    testMatchingSparseAndGapMutation(device);
    testSnapshotAssociationAndOldBacking(device);
    testNonindexedAndStrideFallback(device);
    testDomainFactsAndLimits(device);
    testExplicitSparseProofLimits(device);
    std::cout << "source observation checks: " << checks << '\n';
  } catch (const std::exception& failure) {
    std::cerr << "render_tape_cpu_source_observation_spec failed: " << failure.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
