// Device-independent coverage of the controlled logical observer's boundary.
// Actual ChunkSlot storage drives the serializer. Original locators, published
// projections and copied Pool facts below are explicit shape fixtures, not
// authenticated roots, retained source leases or physical-byte proofs. Typed
// source-row callback fixtures isolate the observer's required-proof contract;
// render_tape_cpu_source_observation_spec owns the actual Pool comparisons.

#include "dxmt9/dxmt9_queue.hpp"
#include "render_tape_cpu_v4_logical_semantics.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace dxmt9;
using namespace dxmt9::test;
using Kind = core::MetalCommandKind;
using IdentityKind = d3d9::RenderTapeIdentityV3CommandKind;
using Locator = render::TapeEmissionRecordLocator;
using Version = std::shared_ptr<const CpuTapeResourceVersion>;
using Context = std::shared_ptr<const CpuTapeResourceContext>;
using Budget = std::shared_ptr<CpuTapeExpectedBufferBudget>;
using Originals = std::shared_ptr<const std::vector<CpuTapeV4OriginalSurfaceRoleEntry>>;
using Facts = resources::BufferVersionObservation;
using Result = CpuGeneralTapeControlledV4LogicalSemanticResult;

constexpr D9CWireObjectIdentity bufferIdentity{D9C_CHUNK_HANDLE_KIND_BUFFER, 3u, 101u};
constexpr D9CWireObjectIdentity firstSurface{D9C_CHUNK_HANDLE_KIND_SURFACE, 7u, 201u};
constexpr D9CWireObjectIdentity secondSurface{D9C_CHUNK_HANDLE_KIND_SURFACE, 8u, 202u};
constexpr D9CWireObjectIdentity parentTexture{D9C_CHUNK_HANDLE_KIND_TEXTURE, 9u, 301u};
constexpr core::Handle bufferHandle{401u}, surfaceHandle{501u};
constexpr Locator firstRecord{11u, 0u}, secondRecord{12u, 0u};
constexpr std::uint64_t logicalEpoch = 17u;
std::size_t checks = 0u;

void check(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}

Budget budget(std::size_t bytes = 1024u) {
  Budget out;
  check(createCpuTapeExpectedBufferBudget({bytes}, out) == CpuTapeResourceContextStatus::Accepted,
      "fixture owns an explicit positive expected-byte session budget");
  return out;
}

Facts facts(bool captured, std::uint64_t revision = 51u, std::uint64_t metal = 601u) {
  // No address, backing owner or real Pool claim is encoded in these facts.
  return {.status = resources::BufferVersionObservationStatus::Valid,
      .buffer = bufferHandle, .byteSize = 64u, .contentRevision = revision,
      .hasVersionedBacking = captured, .currentBacking = {.metalHandle = metal, .byteSize = 64u}};
}

Version version(const Budget& owner, D9CWireObjectIdentity identity, core::Handle native,
    std::uint64_t epoch, std::span<const std::uint8_t> bytes = {},
    std::optional<Facts> local = std::nullopt) {
  const bool alias = identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE;
  const auto content = alias ? parentTexture : identity;
  // The same receipt/seed is intentional: distinct alias identity and logical
  // epoch must not be inferred from native keys or indistinguishable seeds.
  const std::array receipts{CpuTapeResourceReceipt{.identity = content, .revision = 5u}};
  CpuTapeResourceObservation observation;
  check(buildCpuTapeResourceObservation({.identity = identity, .contentIdentity = content,
      .aliasSubresource = alias ? std::optional<std::uint32_t>{0u} : std::nullopt,
      .receipts = receipts}, observation) == CpuTapeResourceObservationStatus::Accepted,
      "fixture uses production owned resource normalization");
  Version out;
  check(buildBudgetedCpuTapeResourceVersion({.nativeKind = identity.kind, .nativeHandle = native,
      .contentEpoch = epoch, .expectedBufferByteSize = bytes.size(), .expectedBufferBytes = bytes,
      .localBufferFacts = local}, observation, owner, out) == CpuTapeResourceContextStatus::Accepted,
      "production immutable version factory owns bytes and copied facts under the exact budget");
  return out;
}

Context inventory(const Budget& owner, std::span<const Version> versions = {}) {
  Context out;
  check(buildCpuTapeV4SurfaceAliasInventory(versions, owner, out) == CpuTapeResourceContextStatus::Accepted,
      "production V4 inventory retains every exact wire version");
  return out;
}

CpuTapeV4OriginalSurfaceRoleEntry entry(Locator record, std::uint32_t recordType,
    std::optional<D9CWireObjectIdentity> target = std::nullopt) {
  CpuTapeV4OriginalSurfaceRoleEntry out;
  out.record = record;
  out.roles.recordType = recordType;
  for (auto& binding : out.roles.renderTargets) binding.known = true;
  out.roles.depthStencil.known = true;
  if (target) out.roles.renderTargets[0].identity = *target;
  out.identityCommandKind = static_cast<std::uint32_t>(d3d9::renderTapeIdentityV3RecordCommandKind(recordType));
  return out;
}

Originals originals(std::initializer_list<CpuTapeV4OriginalSurfaceRoleEntry> entries) {
  return std::make_shared<const std::vector<CpuTapeV4OriginalSurfaceRoleEntry>>(entries);
}

void bindRecordContext(CpuTapeRecordContextStore& records, Locator record, const Context& context) {
  check(records.bind(record, context) == CpuTapeResourceContextStatus::Accepted,
      "write-once production record ledger retains the original immutable context");
}

core::ChunkSlot drawSlot(const Facts& local, bool withSnapshot, std::size_t drawCount = 1u,
    std::optional<core::DrawBufferBindingSnapshot> changedSnapshot = std::nullopt,
    float uniformValue = 0.0f) {
  core::ChunkSlot slot;
  core::CanonicalDrawState state;
  state.hot.streamMask = state.hot.key.streamMask = 1u;
  state.hot.streamBuffers[0] = state.hot.key.streamBuffers[0] = bufferHandle;
  state.hot.streamStrides[0] = state.hot.key.streamStrides[0] = 16u;
  state.hot.streamFrequencies[0] = state.hot.key.streamFrequencies[0] = 1u;
  state.hot.indexBuffer = state.hot.key.indexBuffer = bufferHandle;
  state.shaderLayout.vertexDecl.streams[0].stride = 16u;
  std::vector<core::DrawParam> parameters(drawCount);
  std::vector<core::DrawParamPayloadView> payloads(drawCount);
  core::DrawBindingSnapshot snapshot{};
  snapshot.streamMask = 1u;
  snapshot.streams[0] = {.buffer = bufferHandle, .offset = 0u, .stride = 16u,
      .snapshot = changedSnapshot.value_or(core::DrawBufferBindingSnapshot{
          .metalHandle = local.currentBacking.metalHandle, .contentsAddress = 0u,
          .byteSize = local.byteSize, .contentRevision = local.contentRevision})};
  snapshot.indexBuffer = bufferHandle;
  snapshot.indexType = core::IndexType::UInt16;
  snapshot.indexSnapshot = snapshot.streams[0].snapshot;
  snapshot.indexSnapshotValid = true;
  for (std::size_t index = 0u; index < drawCount; ++index) {
    parameters[index] = {.primitiveType = core::PrimitiveType::PointList,
        .primitiveCount = 1u, .indexType = core::IndexType::UInt16, .indexed = true};
    if (withSnapshot) payloads[index].bindingSnapshotData = std::span<const core::u8>(
        reinterpret_cast<const core::u8*>(&snapshot), sizeof(snapshot));
  }
  core::DrawUniformPayload uniforms{};
  uniforms.vertexFloatConstantCount = 1u;
  uniforms.vsConst.float4[0][0] = uniformValue;
  check(slot.appendDrawRun(std::move(state), uniforms, parameters, payloads),
      "actual ChunkSlot appender owns all final draw parameters and snapshot bytes");
  check(slot.commandCount() == 1u && slot.commandAt(0u).drawParams.size() == drawCount,
      "the observer receives actual physical DrawRun/subdraw positions");
  const auto command = slot.commandAt(0u);
  core::DrawUniformPayload materializedScratch{};
  for (const auto& parameter : command.drawParams) {
    const auto* materialized = core::drawRunUniformPayloadForParam(
        command, parameter, materializedScratch);
    check(materialized && materialized->vertexFloatConstantCount == 1u &&
        materialized->vsConst.float4[0][0] == uniformValue,
        "actual stored uniform materialization preserves the active changed constant");
  }
  return slot;
}

core::ChunkSlot clearSlot(std::size_t count = 1u, core::Handle target = {}, float red = 0.1f) {
  core::ChunkSlot slot;
  core::ClearDesc clear;
  clear.clearColor = true;
  clear.colorAttachments[0].handle = target;
  clear.color = {red, 0.2f, 0.3f, 1.0f};
  for (std::size_t index = 0u; index < count; ++index) slot.appendClear(clear);
  check(slot.commandCount() == count, "actual ChunkSlot owns each distinct physical Clear");
  return slot;
}

d3d9::RenderTapeRuntimePublishedSourceOriginsV4 projection(
    const core::ChunkSlot& slot, std::span<const Locator> records) {
  d3d9::RenderTapeRuntimePublishedSourceOriginsV4 out;
  std::size_t cursor = 0u;
  for (std::size_t index = 0u; index < slot.commandCount(); ++index) {
    const auto command = slot.commandAt(index);
    const bool draw = command.kind == Kind::DrawRun;
    const auto count = draw ? command.drawParams.size() : 1u;
    for (std::size_t subdraw = 0u; subdraw < count; ++subdraw) {
      check(cursor < records.size(), "synthetic projection maps every actual fixture item explicitly");
      out.origins.push_back({.physicalCommandIndex = static_cast<std::uint32_t>(index),
          .kind = render::tapeTopologyCommandKind(command.kind),
          .firstSubdraw = draw ? static_cast<std::uint32_t>(subdraw) : 0u,
          .subdrawCount = draw ? 1u : 0u, .record = records[cursor++]});
    }
  }
  check(cursor == records.size(), "synthetic origin list has no fabricated payload positions");
  return out;
}

template <class SourceRowsFn>
bool observe(CpuTapeV4LogicalSemanticObserver& observer, const core::ChunkSlot& slot,
    const d3d9::RenderTapeRuntimePublishedSourceOriginsV4& projected,
    const CpuTapeRecordContextStore& records, SourceRowsFn&& rows, std::uint64_t sequence = 7u) {
  core::metalqueue::ReplaySourcePublicationReceipt publication;
  publication.metadata.seqId = sequence;
  return observer.observe(publication, core::SourcePayloadView(slot), projected, records,
      std::forward<SourceRowsFn>(rows));
}

const CpuTapeSourceObservation* noRows(std::size_t, std::size_t) { return nullptr; }

CpuTapeSourceObservation qualifiedBoundaryRows() {
  // Scripted return from the required proof callback, not a claimed Pool proof.
  return {.status = CpuTapeSourceObservationStatus::Qualified, .sourceSequence = 7u,
      .completedSequence = 1u, .effectiveIndexed = true, .sourceStride = 16u,
      .sourceByteSize = 64u, .plannerStatus = CpuTapeSourceRowsStatus::Qualified,
      .qualifiedPrimitiveCount = 1u, .sourceElementCount = 1u,
      .uniqueVertexRows = 1u, .vertexRangeCount = 1u};
}

void noBroaderAuthority(const Result& result) {
  check(!result.fullLogicalSemanticOracleComplete && !result.originalSemanticEquivalenceQualified &&
      !result.crossPolicyEquivalenceQualified && !result.gpuResultOracle &&
      !result.policyQualified && !result.timingEligible &&
      !result.futurePreservationQualified && !result.fullShaderFetchCoverageQualified,
      "stream completion never grants original/cross-policy equivalence, full oracle or broader authority");
}

void rejected(const Result& result, const char* reason = nullptr) {
  check(result.requested && !result.accepted && !result.complete &&
      !result.logicalStreamComplete && !result.failure.empty(),
      "requested failure cannot mint any logical complete/accepted capability");
  if (reason) check(result.failure == reason, "failure retains the production typed reason");
  noBroaderAuthority(result);
}

Result singleDraw(const Budget& owner, const Version& resource, const Facts& local,
    bool withSnapshot, std::optional<core::DrawBufferBindingSnapshot> changedSnapshot = std::nullopt,
    float uniformValue = 0.0f) {
  const auto roleEntries = originals({entry(firstRecord, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE)});
  const std::array versions{resource};
  CpuTapeRecordContextStore records;
  bindRecordContext(records, firstRecord, inventory(owner, versions));
  CpuTapeV4LogicalSemanticObserver observer(roleEntries, owner, true);
  const auto slot = drawSlot(local, withSnapshot, 1u, changedSnapshot, uniformValue);
  const auto projected = projection(slot, std::array{firstRecord});
  const auto rows = qualifiedBoundaryRows();
  const bool observed = observe(observer, slot, projected, records,
      [&](std::size_t, std::size_t) { return &rows; });
  const bool finished = observer.finish(1u, 1u);
  check(observed == finished, "single draw cannot complete after rejected serialization");
  return observer.takeResult();
}

void immutableVersionHashCache() {
  const auto owner = budget();
  std::array<std::uint8_t, 64u> callerBytes{};
  callerBytes.back() = 29u;
  const auto local = facts(false);
  const auto original = version(owner, bufferIdentity, bufferHandle, logicalEpoch, callerBytes, local);
  const std::array versions{original};
  const auto originalContext = inventory(owner, versions);
  CpuTapeRecordContextStore records;
  bindRecordContext(records, firstRecord, originalContext);
  bindRecordContext(records, secondRecord, originalContext);
  const auto roleEntries = originals({entry(firstRecord, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE),
      entry(secondRecord, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE)});
  CpuTapeV4LogicalSemanticObserver observer(roleEntries, owner, true);
  const auto slot = drawSlot(local, false);
  const auto rows = qualifiedBoundaryRows();
  const auto sourceRows = [&](std::size_t, std::size_t) { return &rows; };
  check(observe(observer, slot, projection(slot, std::array{firstRecord}), records, sourceRows),
      "first source hashes its complete immutable original image");
  callerBytes.back() ^= 0xffu;
  check(original->expectedBufferBytes().back() == 29u,
      "mutating caller bytes after observation cannot rewrite the historical version");
  check(observe(observer, slot, projection(slot, std::array{secondRecord}), records, sourceRows, 8u) &&
      observer.finish(2u, 2u), "the same version remains usable across actual source boundaries");
  const auto result = observer.takeResult();
  check(result.complete && result.accepted && result.logicalStreamComplete &&
      result.admitted && result.conditionalOnLegalNoOverwrite &&
      result.legalCandidateNoOverwriteContract, "logical success is explicitly conditional on the admitted contract");
  check(result.sources == 2u && result.commands == 2u && result.draws == 2u &&
      result.sourceRowsRequired == 2u && result.sourceRowsQualified == 2u &&
      result.originalItems == 2u && result.logicalBufferHashBytes == callerBytes.size(),
      "repeated hot/resource/VB/IB callbacks and sources hash one owned version only once");
  check(result.receipts[0].digest == result.receipts[1].digest &&
      result.receipts[0].sequence == 7u && result.receipts[1].sequence == 8u &&
      owner->snapshot().chargedExpectedBufferBytes == callerBytes.size(),
      "physical source sequence is a receipt fact, not a logical byte digest or extra image copy");
  noBroaderAuthority(result);

  const auto replacement = version(owner, bufferIdentity, bufferHandle, logicalEpoch, callerBytes, local);
  const auto changed = singleDraw(owner, replacement, local, false);
  check(changed.complete && changed.logicalBufferHashBytes == callerBytes.size() &&
      changed.receipts[0].digest != result.receipts[0].digest,
      "complete-image hashing distinguishes an unfetched tail byte without claiming corruption");
}

void cacheIsImmutableVersionIdentityNotNativeHandle() {
  const auto owner = budget();
  std::array<std::uint8_t, 64u> bytes{};
  const auto local = facts(false);
  const auto earlier = version(owner, bufferIdentity, bufferHandle, logicalEpoch, bytes, local);
  bytes.back() = 91u;
  const auto later = version(owner, bufferIdentity, bufferHandle, logicalEpoch + 1u, bytes, local);
  const std::array earlierVersions{earlier}, laterVersions{later};
  CpuTapeRecordContextStore records;
  bindRecordContext(records, firstRecord, inventory(owner, earlierVersions));
  bindRecordContext(records, secondRecord, inventory(owner, laterVersions));
  CpuTapeV4LogicalSemanticObserver observer(originals({
      entry(firstRecord, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE),
      entry(secondRecord, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE)}), owner, true);
  const auto slot = drawSlot(local, false);
  const auto rows = qualifiedBoundaryRows();
  const auto sourceRows = [&](std::size_t, std::size_t) { return &rows; };
  check(observe(observer, slot, projection(slot, std::array{firstRecord}), records, sourceRows) &&
      observe(observer, slot, projection(slot, std::array{secondRecord}), records, sourceRows, 8u) &&
      observer.finish(2u, 2u), "same native key can join different immutable original record versions");
  const auto result = observer.takeResult();
  check(result.logicalBufferHashBytes == 2u * bytes.size() &&
      result.receipts[0].digest != result.receipts[1].digest &&
      earlier->expectedBufferBytes().back() == 0u && later->expectedBufferBytes().back() == 91u,
      "cache cannot substitute later/current bytes using native-handle equality");
  noBroaderAuthority(result);
}

void snapshotAndLiveNormalizeTheSameLogicalEpoch() {
  const auto owner = budget();
  const std::array<std::uint8_t, 64u> bytes{};
  const auto capturedFacts = facts(true, 71u, 701u);
  const auto liveFacts = facts(false, 93u, 702u);
  const auto captured = version(owner, bufferIdentity, bufferHandle, logicalEpoch, bytes, capturedFacts);
  const auto live = version(owner, bufferIdentity, bufferHandle, logicalEpoch, bytes, liveFacts);
  check(captured->contentEpoch() != capturedFacts.contentRevision &&
      live->contentEpoch() != liveFacts.contentRevision,
      "logical content epoch remains independent of each copied physical Pool revision");
  const auto snapshotResult = singleDraw(owner, captured, capturedFacts, true);
  const auto liveResult = singleDraw(owner, live, liveFacts, false);
  check(snapshotResult.complete && liveResult.complete && snapshotResult.digest == liveResult.digest &&
      snapshotResult.receipts[0].digest == liveResult.receipts[0].digest &&
      snapshotResult.logicalBufferHashBytes == bytes.size() && liveResult.logicalBufferHashBytes == bytes.size(),
      "snapshot/live VB and IB representations hash the same logical bytes and epoch, not physical revision");
  const auto nextEpoch = version(owner, bufferIdentity, bufferHandle, logicalEpoch + 1u, bytes, liveFacts);
  const auto epochResult = singleDraw(owner, nextEpoch, liveFacts, false);
  check(epochResult.complete && epochResult.digest != liveResult.digest,
      "changing only the original logical epoch changes the full logical fingerprint");
  noBroaderAuthority(snapshotResult);
  noBroaderAuthority(liveResult);

  core::DrawBufferBindingSnapshot mismatched{.metalHandle = capturedFacts.currentBacking.metalHandle,
      .contentsAddress = 0u, .byteSize = capturedFacts.byteSize,
      .contentRevision = capturedFacts.contentRevision + 1u};
  rejected(singleDraw(owner, captured, capturedFacts, true, mismatched),
      "controlled-v4-logical-historical-snapshot");
  mismatched.contentRevision = capturedFacts.contentRevision;
  ++mismatched.metalHandle;
  rejected(singleDraw(owner, captured, capturedFacts, true, mismatched),
      "controlled-v4-logical-historical-snapshot");
  --mismatched.metalHandle;
  ++mismatched.byteSize;
  rejected(singleDraw(owner, captured, capturedFacts, true, mismatched),
      "controlled-v4-logical-historical-snapshot");
  rejected(singleDraw(owner, captured, capturedFacts, false),
      "controlled-v4-logical-historical-live-binding");
  rejected(singleDraw(owner, live, liveFacts, true), "controlled-v4-logical-historical-snapshot");
}

void nativeAliasesPreserveOriginalRoleIdentity() {
  const auto owner = budget();
  const auto first = version(owner, firstSurface, surfaceHandle, logicalEpoch);
  const auto second = version(owner, secondSurface, surfaceHandle, logicalEpoch);
  const std::array versions{first, second};
  const auto context = inventory(owner, versions);
  check(!context->find(D9C_CHUNK_HANDLE_KIND_SURFACE, surfaceHandle) &&
      context->findIdentity(firstSurface) == first && context->findIdentity(secondSurface) == second,
      "same-native surface aliases remain ambiguous by native key and exact by wire identity");
  const auto roleEntries = originals({entry(firstRecord, D9C_COMMAND_RECORD_CLEAR, firstSurface),
      entry(secondRecord, D9C_COMMAND_RECORD_CLEAR, secondSurface)});
  CpuTapeRecordContextStore records;
  bindRecordContext(records, firstRecord, context);
  bindRecordContext(records, secondRecord, context);
  const auto slot = clearSlot(2u, surfaceHandle);
  const auto projected = projection(slot, std::array{firstRecord, secondRecord});
  CpuTapeV4LogicalSemanticObserver observer(roleEntries, owner, true);
  check(observe(observer, slot, projected, records, noRows) && observer.finish(1u, 2u),
      "exact original RT0 role resolves each alias without first-native-match selection");
  const auto result = observer.takeResult();
  check(result.receipts.size() == 2u && result.receipts[0].serialized && result.receipts[1].serialized &&
      result.receipts[0].digest != result.receipts[1].digest && !result.logicalBufferHashBytes &&
      !result.draws && !result.sourceRowsRequired && !result.sourceRowsQualified,
      "indistinguishable native/seed/epoch aliases retain distinct wire-role logical fingerprints");
  noBroaderAuthority(result);

  const std::array reversedVersions{second, first};
  CpuTapeRecordContextStore reversedRecords;
  const auto reversedContext = inventory(owner, reversedVersions);
  bindRecordContext(reversedRecords, firstRecord, reversedContext);
  bindRecordContext(reversedRecords, secondRecord, reversedContext);
  CpuTapeV4LogicalSemanticObserver stable(roleEntries, owner, true);
  check(observe(stable, slot, projected, reversedRecords, noRows) && stable.finish(1u, 2u) &&
      stable.takeResult().digest == result.digest,
      "context inventory order does not alter exact canonical role selection or digest");
}

void conditionalContractIsRequired() {
  const auto owner = budget();
  const auto roleEntries = originals({entry(firstRecord, D9C_COMMAND_RECORD_CLEAR)});
  CpuTapeRecordContextStore records;
  bindRecordContext(records, firstRecord, inventory(owner));
  const auto slot = clearSlot();
  const auto projected = projection(slot, std::array{firstRecord});
  CpuTapeV4LogicalSemanticObserver absentContract(roleEntries, owner, false);
  check(!observe(absentContract, slot, projected, records, noRows) && !absentContract.finish(1u, 1u),
      "metadata inventory and valid payload cannot replace the explicit legal-candidate contract");
  const auto result = absentContract.takeResult();
  rejected(result, "controlled-v4-logical-session-contract");
  check(!result.admitted && !result.legalCandidateNoOverwriteContract && !result.conditionalOnLegalNoOverwrite &&
      !result.commands && result.receipts.empty(), "unadmitted session fails before any logical observation");
  CpuTapeV4LogicalSemanticObserver noBudget(roleEntries, {}, true);
  check(!noBudget.finish(0u, 0u), "contract cannot stand in for exact immutable-byte budget ownership");
  rejected(noBudget.takeResult(), "controlled-v4-logical-session-contract");
  CpuTapeV4LogicalSemanticObserver noOriginals({}, owner, true);
  check(!noOriginals.finish(0u, 0u), "contract cannot stand in for original canonical identity inventory");
  rejected(noOriginals.takeResult(), "controlled-v4-logical-session-contract");
}

void originalItemOrderTruthTable() {
  const auto stateRecord = Locator{10u, 0u};
  const auto between = Locator{11u, 1u};
  const auto roleEntries = originals({entry(stateRecord, D9C_COMMAND_RECORD_APPLY_STATE),
      entry(firstRecord, D9C_COMMAND_RECORD_CLEAR), entry(between, D9C_COMMAND_RECORD_APPLY_STATE),
      entry(secondRecord, D9C_COMMAND_RECORD_CLEAR)});
  const auto clearKind = static_cast<std::uint32_t>(IdentityKind::Clear);
  const auto drawKind = static_cast<std::uint32_t>(IdentityKind::DrawRun);
  CpuTapeV4OriginalItemOrder ordered(roleEntries);
  check(!ordered.complete() && ordered.accept(firstRecord, clearKind) && ordered.accepted() == 1u &&
      !ordered.complete() && ordered.accept(secondRecord, clearKind) && ordered.complete() &&
      ordered.accepted() == 2u, "cursor skips state-only records but preserves original executable order");
  check(!ordered.accept(secondRecord, clearKind) && ordered.accepted() == 2u,
      "duplicate after the end cannot add an accepted original item");
  CpuTapeV4OriginalItemOrder reordered(roleEntries);
  check(!reordered.accept(secondRecord, clearKind) && !reordered.complete() && !reordered.accepted(),
      "drop-forward or same-kind reorder rejects instead of sorting the stream afterward");
  CpuTapeV4OriginalItemOrder wrongKind(roleEntries);
  check(!wrongKind.accept(firstRecord, drawKind) && !wrongKind.accepted(),
      "exact locator does not permit a different executable command identity kind");
  CpuTapeV4OriginalItemOrder duplicate(roleEntries);
  check(duplicate.accept(firstRecord, clearKind) && !duplicate.accept(firstRecord, clearKind) &&
      duplicate.accepted() == 1u && !duplicate.complete(), "duplicate original cannot replace the next item");
  CpuTapeV4OriginalItemOrder dropped(roleEntries);
  check(dropped.accept(firstRecord, clearKind) && !dropped.complete(),
      "completion detects a trailing original drop even with no later observation");
  CpuTapeV4OriginalItemOrder stateOnly(originals({entry(stateRecord, D9C_COMMAND_RECORD_APPLY_STATE)}));
  check(stateOnly.complete() && !stateOnly.accept(stateRecord, clearKind) && !stateOnly.accepted(),
      "state-only originals supply no executable item to a caller");
  CpuTapeV4OriginalItemOrder missing({});
  check(!missing.complete() && !missing.accept(firstRecord, clearKind),
      "absent canonical inventory cannot qualify an item or completion");
}

void observerRejectsReorderDuplicateAndDropAcrossSources() {
  const auto owner = budget();
  const auto roleEntries = originals({entry(firstRecord, D9C_COMMAND_RECORD_CLEAR),
      entry(secondRecord, D9C_COMMAND_RECORD_CLEAR)});
  CpuTapeRecordContextStore records;
  const auto context = inventory(owner);
  bindRecordContext(records, firstRecord, context);
  bindRecordContext(records, secondRecord, context);
  const auto slot = clearSlot();
  const auto first = projection(slot, std::array{firstRecord});
  const auto second = projection(slot, std::array{secondRecord});
  CpuTapeV4LogicalSemanticObserver reordered(roleEntries, owner, true);
  check(!observe(reordered, slot, second, records, noRows) &&
      !observe(reordered, slot, first, records, noRows) && !reordered.finish(2u, 2u),
      "same-kind source reorder is a sticky requested failure, not digest normalization");
  rejected(reordered.takeResult(), "controlled-v4-logical-observed-original-order");
  CpuTapeV4LogicalSemanticObserver duplicate(roleEntries, owner, true);
  check(observe(duplicate, slot, first, records, noRows) &&
      !observe(duplicate, slot, first, records, noRows) &&
      !observe(duplicate, slot, second, records, noRows) && !duplicate.finish(2u, 2u),
      "duplicate original cannot be rescued by later observation of the missing item");
  rejected(duplicate.takeResult(), "controlled-v4-logical-observed-original-order");
  CpuTapeV4LogicalSemanticObserver dropped(roleEntries, owner, true);
  check(observe(dropped, slot, first, records, noRows) && !dropped.finish(1u, 1u),
      "matching physical counters cannot excuse an unobserved trailing canonical original");
  rejected(dropped.takeResult(), "controlled-v4-logical-canonical-coverage");

  CpuTapeV4LogicalSemanticObserver mismatchedCounters(roleEntries, owner, true);
  check(observe(mismatchedCounters, slot, first, records, noRows) &&
      observe(mismatchedCounters, slot, second, records, noRows) &&
      !mismatchedCounters.finish(1u, 2u) && !mismatchedCounters.finish(2u, 2u),
      "requested source-count failure stays terminal even after retry with correct counters");
  rejected(mismatchedCounters.takeResult(), "controlled-v4-logical-complete-coverage");
}

void missingOrRejectedRowProofCannotComplete() {
  const auto owner = budget();
  const std::array<std::uint8_t, 64u> bytes{};
  const auto local = facts(false);
  const auto resource = version(owner, bufferIdentity, bufferHandle, logicalEpoch, bytes, local);
  const std::array versions{resource};
  const auto context = inventory(owner, versions);
  const auto roleEntries = originals({entry(firstRecord, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE),
      entry(secondRecord, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE)});
  CpuTapeRecordContextStore records;
  bindRecordContext(records, firstRecord, context);
  bindRecordContext(records, secondRecord, context);
  const auto slot = drawSlot(local, false, 2u);
  const auto projected = projection(slot, std::array{firstRecord, secondRecord});
  const auto firstRows = qualifiedBoundaryRows();
  auto secondRows = firstRows;
  secondRows.status = CpuTapeSourceObservationStatus::BatchRejected;
  CpuTapeV4LogicalSemanticObserver partial(roleEntries, owner, true);
  check(!observe(partial, slot, projected, records,
      [&](std::size_t command, std::size_t subdraw) {
        check(command == 0u && subdraw < 2u, "required rows use exact actual command/subdraw positions");
        return subdraw ? &secondRows : &firstRows;
      }) && !partial.finish(1u, 1u), "one rejected draw proof prevents aggregate logical completion");
  const auto result = partial.takeResult();
  rejected(result, "controlled-v4-logical-source-row-proof-rejected");
  check(result.draws == 2u && result.sourceRowsRequired == 2u && result.sourceRowsQualified == 1u &&
      result.receipts.size() == 2u && result.receipts[0].serialized && !result.receipts[1].serialized &&
      result.receipts[1].sourceRows.status == CpuTapeSourceObservationStatus::BatchRejected,
      "typed failed receipt survives without promoting earlier serialized draw or partial proof count");
  CpuTapeV4LogicalSemanticObserver missing(roleEntries, owner, true);
  check(!observe(missing, slot, projected, records, noRows) && !missing.finish(1u, 1u),
      "metadata and immutable bytes cannot replace a missing required source-row proof");
  rejected(missing.takeResult(), "controlled-v4-logical-missing-source-rows");
}

void missingAndAmbiguousOriginalContextFailClosed() {
  const auto owner = budget();
  const auto roleEntries = originals({entry(firstRecord, D9C_COMMAND_RECORD_CLEAR)});
  const auto slot = clearSlot();
  auto projected = projection(slot, std::array{firstRecord});
  CpuTapeRecordContextStore missingRecords;
  CpuTapeV4LogicalSemanticObserver missingContext(roleEntries, owner, true);
  check(!observe(missingContext, slot, projected, missingRecords, noRows) &&
      !missingContext.finish(1u, 1u), "original locator without immutable context is insufficient");
  rejected(missingContext.takeResult(), "controlled-v4-logical-original-context");
  CpuTapeRecordContextStore records;
  bindRecordContext(records, firstRecord, inventory(owner));
  projected.origins.push_back(projected.origins.front());
  CpuTapeV4LogicalSemanticObserver ambiguous(roleEntries, owner, true);
  check(!observe(ambiguous, slot, projected, records, noRows) && !ambiguous.finish(1u, 1u),
      "multiple matching original origins cannot be resolved by first matching kind");
  rejected(ambiguous.takeResult(), "controlled-v4-logical-ambiguous-original");
}

void actualScalarAndUniformChangesAreDiagnosticsNotEquivalence() {
  const auto owner = budget();
  const auto roleEntries = originals({entry(firstRecord, D9C_COMMAND_RECORD_CLEAR)});
  CpuTapeRecordContextStore records;
  bindRecordContext(records, firstRecord, inventory(owner));
  const auto clearDigest = [&](float red) {
    CpuTapeV4LogicalSemanticObserver observer(roleEntries, owner, true);
    const auto slot = clearSlot(1u, {}, red);
    check(observe(observer, slot, projection(slot, std::array{firstRecord}), records, noRows) &&
        observer.finish(1u, 1u), "changed actual Clear scalar can still complete diagnostic observation");
    const auto result = observer.takeResult();
    check(result.logicalStreamComplete, "complete stream is distinct from expected-original equality");
    noBroaderAuthority(result);
    return result.digest;
  };
  check(clearDigest(0.1f) != clearDigest(0.4f),
      "actual scalar fingerprint changes without promoting equality to a reconstructed original stream");

  const std::array<std::uint8_t, 64u> bytes{};
  const auto local = facts(false);
  const auto resource = version(owner, bufferIdentity, bufferHandle, logicalEpoch, bytes, local);
  const auto unchanged = singleDraw(owner, resource, local, false, std::nullopt, 0.0f);
  const auto changed = singleDraw(owner, resource, local, false, std::nullopt, 3.5f);
  check(unchanged.logicalStreamComplete && changed.logicalStreamComplete &&
      unchanged.digest != changed.digest && unchanged.receipts[0].digest != changed.receipts[0].digest,
      "actual uniform field change alters the full-field fingerprint but is not independently certified equal");
  noBroaderAuthority(unchanged);
  noBroaderAuthority(changed);
}

void originalStreamDigestIsIndependentOfPhysicalDrawPartition() {
  const auto owner = budget();
  const std::array<std::uint8_t, 64u> bytes{};
  const auto local = facts(false);
  const auto resource = version(owner, bufferIdentity, bufferHandle, logicalEpoch, bytes, local);
  const std::array versions{resource};
  const auto context = inventory(owner, versions);
  CpuTapeRecordContextStore records;
  bindRecordContext(records, firstRecord, context);
  bindRecordContext(records, secondRecord, context);
  const auto roleEntries = originals({entry(firstRecord, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE),
      entry(secondRecord, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE)});
  const auto rows = qualifiedBoundaryRows();
  const auto bundledSlot = drawSlot(local, false, 2u);
  CpuTapeV4LogicalSemanticObserver bundled(roleEntries, owner, true);
  std::size_t bundledDraws = 0u;
  check(observe(bundled, bundledSlot, projection(bundledSlot, std::array{firstRecord, secondRecord}),
      records, [&](std::size_t command, std::size_t subdraw) {
        check(command == 0u && subdraw == bundledDraws++,
            "bundled observation proves both actual subdraw positions in one physical DrawRun");
        return &rows;
      }, 19u) && bundled.finish(1u, 1u),
      "one source with one two-subdraw DrawRun completely observes both canonical originals");
  const auto bundledResult = bundled.takeResult();

  const auto separateSlot = drawSlot(local, false);
  CpuTapeV4LogicalSemanticObserver separate(roleEntries, owner, true);
  std::size_t separateDraws = 0u;
  const auto separateRows = [&](std::size_t command, std::size_t subdraw) {
    check(command == 0u && subdraw == 0u,
        "separate publications each use the actual single-draw physical position");
    ++separateDraws;
    return &rows;
  };
  check(observe(separate, separateSlot, projection(separateSlot, std::array{firstRecord}),
      records, separateRows, 23u) &&
      observe(separate, separateSlot, projection(separateSlot, std::array{secondRecord}),
          records, separateRows, 24u) && separate.finish(2u, 2u),
      "two one-draw source publications completely observe the same two canonical originals");
  const auto separateResult = separate.takeResult();
  check(bundledResult.logicalStreamComplete && separateResult.logicalStreamComplete &&
      bundledDraws == 2u && separateDraws == 2u && bundledResult.draws == 2u && separateResult.draws == 2u &&
      bundledResult.sources == 1u && bundledResult.commands == 1u &&
      separateResult.sources == 2u && separateResult.commands == 2u,
      "physical source/command counts remain accurate without changing original item coverage");
  check(bundledResult.receipts.size() == 2u && separateResult.receipts.size() == 2u &&
      bundledResult.receipts[1].subdrawIndex == 1u && separateResult.receipts[1].subdrawIndex == 0u &&
      bundledResult.receipts[0].sequence == 19u && bundledResult.receipts[1].sequence == 19u &&
      separateResult.receipts[0].sequence == 23u && separateResult.receipts[1].sequence == 24u,
      "receipts preserve actual physical subdraw/sequence differences for the partitioned inputs");
  check(bundledResult.digest == separateResult.digest &&
      bundledResult.receipts[0].record == separateResult.receipts[0].record &&
      bundledResult.receipts[1].record == separateResult.receipts[1].record &&
      bundledResult.receipts[0].digest == separateResult.receipts[0].digest &&
      bundledResult.receipts[1].digest == separateResult.receipts[1].digest &&
      bundledResult.logicalBufferHashBytes == bytes.size() &&
      separateResult.logicalBufferHashBytes == bytes.size(),
      "canonical logical stream digest ignores physical grouping, indices, sequence and command-count differences");
  noBroaderAuthority(bundledResult);
  noBroaderAuthority(separateResult);
}

} // namespace

int main() {
  try {
    immutableVersionHashCache();
    cacheIsImmutableVersionIdentityNotNativeHandle();
    snapshotAndLiveNormalizeTheSameLogicalEpoch();
    nativeAliasesPreserveOriginalRoleIdentity();
    conditionalContractIsRequired();
    originalItemOrderTruthTable();
    observerRejectsReorderDuplicateAndDropAcrossSources();
    missingOrRejectedRowProofCannotComplete();
    missingAndAmbiguousOriginalContextFailClosed();
    actualScalarAndUniformChangesAreDiagnosticsNotEquivalence();
    originalStreamDigestIsIndependentOfPhysicalDrawPartition();
    std::cout << "render-tape-cpu-v4-logical-semantics-spec: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render-tape-cpu-v4-logical-semantics-spec: " << error.what() << '\n';
    return 1;
  }
}
