#include "device_c_render_tape_runtime_identity_v4.hpp"
#include "device_c_render_tape_runtime_identity_core.hpp"
#include "device_c_render_tape_runtime_replay_v4.hpp"
#include "device_c_replay_projection.hpp"
#include "dxmt9/dxmt9_queue.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace dxmt9::d3d9 {
struct RenderTapeRuntimeIdentityV4TestAccess {
  struct Binding {
    RenderTapeIdentityV3Disposition disposition{};
    query::WriterIdentity writer{};
    std::uint32_t physical = 0u;
  };
  struct Snapshot {
    std::vector<Binding> records{};
    std::vector<std::array<std::uint64_t, 7>> writers{};
  };
  struct QueryState {
    std::vector<std::array<std::uint64_t, 6>> queries{};
    std::size_t records = 0u, coordinators = 0u, sources = 0u, initializers = 0u;
    bool strict = false;
    bool operator==(const QueryState&) const = default;
  };
  static QueryState queryState(RenderTapeRuntimeIdentityV4& runtime) {
    auto& core = *static_cast<RenderTapeRuntimeIdentityCore*>(runtime.sink().context);
    std::lock_guard lock(core.mutex_);
    QueryState out;
    for (const auto& query : core.queries_)
      out.queries.push_back({query.identity.objectId, query.identity.generation,
          query.coreIdentity, query.ordinal, query.epoch, query.cycle});
    out.records = core.records_.size();
    out.coordinators = core.coordinators_.size();
    out.sources = core.sources_.size();
    out.initializers = core.initializers_.size();
    out.strict = core.strictQueryContinuity();
    return out;
  }
  static Snapshot bindings(RenderTapeRuntimeIdentityV4& runtime) {
    auto& core = *static_cast<RenderTapeRuntimeIdentityCore*>(runtime.sink().context);
    std::lock_guard lock(core.mutex_);
    Snapshot out;
    for (const auto& record : core.records_)
      out.records.push_back({record.disposition, record.writer, record.physicalCommand});
    for (const auto& writer : core.writers_)
      out.writers.push_back({writer.identity.index, writer.identity.generation,
          writer.identity.storageGeneration, writer.captureId, writer.sourceIndex, writer.commandCount,
          writer.identity.queue});
    return out;
  }
  static bool same(const Snapshot& left, const Snapshot& right) {
    if (left.records.size() != right.records.size() || left.writers != right.writers) return false;
    for (std::size_t index = 0u; index < left.records.size(); ++index) {
      const auto& a = left.records[index];
      const auto& b = right.records[index];
      if (a.writer != b.writer || a.physical != b.physical ||
          a.disposition.eventOrdinal != b.disposition.eventOrdinal ||
          a.disposition.firstRecord != b.disposition.firstRecord ||
          a.disposition.recordCount != b.disposition.recordCount ||
          a.disposition.kind != b.disposition.kind ||
          a.disposition.commandIndex != b.disposition.commandIndex ||
          a.disposition.firstSubdraw != b.disposition.firstSubdraw ||
          a.disposition.subdrawCount != b.disposition.subdrawCount ||
          a.disposition.coordinatorIndex != b.disposition.coordinatorIndex ||
          a.disposition.reserved0 != b.disposition.reserved0) return false;
    }
    return true;
  }
};
} // namespace dxmt9::d3d9

namespace {
using namespace dxmt9;
using namespace dxmt9::d3d9;
using Outcome = RenderTapeIdentityV4AttemptOutcome;
constexpr RenderTapeIdentityV4CaptureKey key{5u, 6u};
constexpr query::WriterIdentity writer{1u, 1u, 2u, 3u};
std::uint64_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

template <typename T>
bool sameRows(const std::vector<T>& left, const std::vector<T>& right) {
  static_assert(std::is_trivially_copyable_v<T>);
  return left.size() == right.size() && (left.empty() ||
      std::memcmp(left.data(), right.data(), left.size() * sizeof(T)) == 0);
}

core::CpuReadyTape::SourceRef source() {
  return {.id = {.index = writer.index, .generation = writer.generation},
      .storage = {.firstPage = 4u, .pageCount = 1u, .generation = writer.storageGeneration}};
}

constexpr render::TapeTopologyCaptureIdentity originCapture{key.token, key.epoch, writer.queue};

core::metalqueue::ReplaySourcePublicationReceipt originPublication() {
  return {source(), {.rawOrdinal = 1u, .sourceOrdinal = 7u, .seqId = 10u,
      .buildGeneration = 7u, .pageCount = 1u}, 4u, false};
}

bool emptyOrigins(const RenderTapeRuntimePublishedSourceOriginsV4& value) {
  return !value.capture.valid() && !value.source.source.valid() && !value.metadata.valid() &&
      value.controlIndex == SIZE_MAX && !value.hasPresent && value.commands.empty() && value.origins.empty();
}

bool sameOrigins(const RenderTapeRuntimePublishedSourceOriginsV4& left,
    const RenderTapeRuntimePublishedSourceOriginsV4& right) {
  return left.capture == right.capture && left.source == right.source && left.metadata == right.metadata &&
      left.controlIndex == right.controlIndex && left.hasPresent == right.hasPresent &&
      sameRows(left.commands, right.commands) && left.origins == right.origins;
}

ReplaySourceIdentity projectionIdentity(std::uint64_t raw) {
  return {.frame = 19u, .source = 21u, .lastSource = 23u, .sequence = raw};
}

void emit(RenderTapeRuntimeIdentityV4& runtime, render::TapeRuntimeObservation observation) {
  const auto sink = runtime.sink();
  sink.fn(sink.context, observation);
}

void record(RenderTapeRuntimeIdentityV4& runtime, ReplayTransaction& transaction,
    std::uint32_t ordinal, std::uint32_t type, bool emission = false) {
  check(runtime.beginRecord(ordinal, type), "original canonical record begins once");
  check(transaction.project({.stateGeneration = 7u, .source = transaction.state().identity.source,
      .recordOrdinal = ordinal}), "actual transaction projects raw-absolute cursor");
  check(runtime.beginDispatch(), "original dispatch boundary remains exact");
  if (emission) check(transaction.stage({.commandCount = 1u, .byteCount = 8u}),
      "actual staged emission cardinality is separate from physical commands");
  check(runtime.endRecord(0), "successful original dispatch supplies disposition");
}

void appendDraw(core::ChunkSlot& slot) {
  core::DrawParam draw{};
  draw.primitiveCount = 1u;
  const core::DrawParamPayloadView payload{};
  check(slot.appendDrawRun({}, {}, std::span(&draw, 1u), std::span(&payload, 1u)),
      "real final-storage physical DrawRun is appended");
}

void observeAppendedDraw(RenderTapeRuntimeIdentityV4& runtime, core::ChunkSlot& slot,
    std::uint32_t physical, RenderTapeRuntimeRecordLocator locator, bool bind = true) {
  if (bind) {
    const std::array records{locator};
    check(runtime.bindEmission(writer, physical, core::MetalCommandKind::DrawRun, records),
        "actual append provides explicit original event/record locator before verification");
  }
  emit(runtime, {.edge = render::TapeRuntimeEdge::Append, .writer = writer,
      .source = source(), .payload = core::SourcePayloadView(slot), .commandIndex = physical,
      .subdrawCount = 1u});
}

void receiveAndCommit(ReplayTransaction& transaction, ReplayDestinationKind kind,
    std::uint64_t buildGeneration = 0u) {
  ReplayDestinationReceipt receipt{.kind = kind, .identity = transaction.state().identity,
      .queueSequence = kind == ReplayDestinationKind::Compatibility ? transaction.state().identity.sequence : 10u,
      .buildGeneration = buildGeneration,
      .sourceGeneration = kind == ReplayDestinationKind::Compatibility ? 0u : writer.generation,
      .storageGeneration = kind == ReplayDestinationKind::Compatibility ? 0u : writer.storageGeneration,
      .controlIndex = kind == ReplayDestinationKind::Compatibility ? UINT32_MAX : 4u,
      .commandCount = transaction.state().stagedCommandCount};
  check(transaction.receiveDestination(receipt), "production reducer accepts actual destination receipt");
  check(transaction.commit(), "actual same transaction commits");
}

void completeSource(RenderTapeRuntimeIdentityV4& runtime) {
  emit(runtime, {.edge = render::TapeRuntimeEdge::Submit, .source = source(), .sequence = 10u});
  emit(runtime, {.edge = render::TapeRuntimeEdge::Complete, .source = source(),
      .sequence = 10u, .completedSequence = 10u});
}

void acceptAndEncodeAll(RenderTapeRuntimeIdentityV4& runtime, std::uint32_t count,
    bool encodeFirst = false) {
  std::vector<render::TapeTopologyCommandRef> commands(count);
  std::vector<std::uint32_t> order(count);
  for (std::uint32_t index = 0u; index < count; ++index) {
    check(runtime.topology().commandRef(source(), 10u, index, commands[index]),
        "full source/storage locator resolves copied immutable extent");
    order[index] = index;
  }
  const auto attempt = runtime.topology().stageWithoutDag(RenderTapeExecutionTopologyV1Route::NaturalOrder,
      RenderTapeExecutionTopologyV1FallbackReason::None, commands, order);
  check(attempt != 0u, "actual natural invocation stages without manufacturing DAG membership");
  const render::TapeTopologyCaptureIdentity capture{key.token, key.epoch, writer.queue};
  const std::array owners{commands.front().owner};
  if (encodeFirst)
    check(runtime.topology().observeEncoded({capture, attempt}, commands),
        "successful invocation may precede actual registration receipt");
  const auto acceptance = runtime.edges().next();
  check(acceptance && runtime.topology().accept({capture, attempt, acceptance}, owners),
      "actual accepted registration uses shared root edge clock");
  if (!encodeFirst)
    check(runtime.topology().observeEncoded({capture, attempt}, commands),
        "accepted registration never substitutes for successful Encode");
}

void directWritingAcrossSpansAndRaws() {
  RenderTapeRuntimeIdentityV4 runtime;
  check(runtime.begin(key, {.queue = writer.queue}), "V4 root starts at actual completed baseline");
  core::ChunkSlot slot{};
  ReplayTransaction transaction;
  constexpr ReplayTransactionRawIdentity first{key.token, 2u, 1u, 3u};
  {
    ScopedReplayTransactionObserver scope(first, runtime.replaySink());
    transaction.begin(projectionIdentity(first.rawOrdinal));
    record(runtime, transaction, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, true);
    record(runtime, transaction, 1u, D9C_COMMAND_RECORD_SET_VS_CONST_F);
    appendDraw(slot);
    observeAppendedDraw(runtime, slot, 0u, {2u, 0u});
    noteExternalEffect(transaction, ReplayTransactionEffectWitness::DirectLease);
    receiveAndCommit(transaction, ReplayDestinationKind::DirectChunkSlot, 11u);
    transaction.begin(projectionIdentity(first.rawOrdinal), nullptr, 2u);
    record(runtime, transaction, 2u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, true);
    appendDraw(slot);
    observeAppendedDraw(runtime, slot, 1u, {2u, 2u});
    noteExternalEffect(transaction, ReplayTransactionEffectWitness::DirectLease);
    receiveAndCommit(transaction, ReplayDestinationKind::DirectChunkSlot, 12u);
  }
  check(runtime.acknowledgeRaw(first, true), "raw ledger ACK is distinct from two span commits");
  constexpr ReplayTransactionRawIdentity second{key.token, 3u, 2u, 1u};
  {
    ScopedReplayTransactionObserver scope(second, runtime.replaySink());
    transaction.begin(projectionIdentity(second.rawOrdinal));
    record(runtime, transaction, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, true);
    appendDraw(slot);
    observeAppendedDraw(runtime, slot, 2u, {3u, 0u});
    noteExternalEffect(transaction, ReplayTransactionEffectWitness::DirectLease);
    receiveAndCommit(transaction, ReplayDestinationKind::DirectChunkSlot, 13u);
  }
  check(runtime.acknowledgeRaw(second, true), "second raw ACK closes coverage but not Writing source");
  RenderTapeIdentityV4View view;
  RenderTapeExecutionTopologyV1View topology;
  check(runtime.snapshot(key, view, topology, false) && view.sources.empty() && view.commands.empty() &&
      view.attempts.size() == 3u && view.settlements.size() == 2u,
      "three actual commits never synthesize publication or physical extents");
  check(view.attempts[0].recordCount == 2u && view.attempts[1].firstRecord == 2u &&
      view.attempts[1].recordCount == 1u && view.attempts[0].stagedCommandCount == 1u &&
      view.attempts[0].identity.frame == 19u && view.attempts[0].identity.lastSource == 23u &&
      view.attempts[0].receipt.buildGeneration == 11u && view.attempts[1].receipt.buildGeneration == 12u,
      "actual span ranges, producer values and receipt generations are copied exactly");
  emit(runtime, {.edge = render::TapeRuntimeEdge::Publish, .writer = writer, .source = source(),
      .metadata = {.rawOrdinal = 2u, .sourceOrdinal = 7u, .seqId = 10u,
          .buildGeneration = 13u, .pageCount = 1u},
      .payload = core::SourcePayloadView(slot), .controlIndex = 4u, .sequence = 10u});
  check(!runtime.failed() && runtime.snapshot(key, view, topology, false) && view.commands.size() == 3u,
      "later actual source publication materializes neutral immutable commands once");
  check(!runtime.snapshot(key, view, topology), "publication alone proves neither Encode nor completion");
  acceptAndEncodeAll(runtime, 3u, true);
  completeSource(runtime);
  check(runtime.freeze(key, 4u) && runtime.snapshot(key, view, topology), "terminal capture returns both domains");
  const std::array events{
      RenderTapeIdentityV3EventFact{2u, static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 3u},
      RenderTapeIdentityV3EventFact{3u, static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 1u}};
  const std::array facts{
      RenderTapeIdentityV3RecordFact{2u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE},
      RenderTapeIdentityV3RecordFact{2u, 1u, D9C_COMMAND_RECORD_SET_VS_CONST_F},
      RenderTapeIdentityV3RecordFact{2u, 2u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE},
      RenderTapeIdentityV3RecordFact{3u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE}};
  check(validateRenderTapeIdentityV4Rows(view, events, facts) == RenderTapeIdentityV4Status::Valid &&
      validateRenderTapeExecutionTopologyV1Rows(view, topology) == RenderTapeExecutionTopologyV1Status::Valid,
      "actual Direct receipts join later publication without equating ephemeral raw/build metadata");
  check(view.attempts[0].beginEdge == 2u && view.attempts[0].effectEdge == 3u &&
      view.attempts[0].terminalEdge == 4u && view.sources[0].lifecycle.publicationEdge == 13u &&
      topology.plans[0].acceptanceEdge == 14u && view.sources[0].lifecycle.submissionEdge == 15u &&
      view.sources[0].lifecycle.completionEdge == 16u,
      "one monotonic root clock orders provenance and actual topology receipts");
}

void confirmedRollbackAndRetry() {
  auto device = std::make_unique<core::Device>(core::AdapterInfo{}, core::BackendLimits{},
      core::PresentParameters{}, 0u);
  RenderTapeRuntimeIdentityV4 runtime;
  check(runtime.begin(key, {.queue = writer.queue}), "rollback root begins");
  constexpr ReplayTransactionRawIdentity raw{key.token, 2u, 1u, 2u};
  ReplayTransaction transaction;
  RenderTapeIdentityV4View view;
  RenderTapeExecutionTopologyV1View topology;
  {
    ScopedReplayTransactionObserver scope(raw, runtime.replaySink());
    transaction.begin(projectionIdentity(1u), device.get());
    record(runtime, transaction, 0u, D9C_COMMAND_RECORD_SET_VS_CONST_F);
    check(runtime.beginRecord(1u, D9C_COMMAND_RECORD_SET_PS_CONST_F), "failed dispatch candidate begins");
    check(transaction.project({.stateGeneration = 7u, .source = 21u, .recordOrdinal = 1u}),
        "actual failed candidate has projected cursor but no successful dispatch");
    check(transaction.rollback(*device), "production semantic journal actually rolls back");
    check(!runtime.failed() && !runtime.freeze(key, 4u) &&
        runtime.snapshot(key, view, topology, false) && view.attempts.empty(),
        "semantic rollback is pending, not an exported successful no-effect attempt");
    noteDestinationRollback(transaction, true);
    check(runtime.snapshot(key, view, topology, false) && view.attempts.size() == 1u &&
        view.attempts[0].outcome == static_cast<std::uint32_t>(Outcome::RolledBack) &&
        view.attempts[0].recordCount == 2u && view.dispositions.empty(),
        "successful destination rollback truncates candidate dispositions and promotes actual rollback");
    transaction.begin(projectionIdentity(1u), device.get());
    record(runtime, transaction, 0u, D9C_COMMAND_RECORD_SET_VS_CONST_F);
    record(runtime, transaction, 1u, D9C_COMMAND_RECORD_SET_PS_CONST_F);
    receiveAndCommit(transaction, ReplayDestinationKind::Compatibility);
  }
  check(runtime.acknowledgeRaw(raw, true) && runtime.freeze(key, 4u) &&
      runtime.snapshot(key, view, topology), "new real retry commits contiguous canonical coverage");
  const std::array events{RenderTapeIdentityV3EventFact{
      2u, static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 2u}};
  const std::array facts{
      RenderTapeIdentityV3RecordFact{2u, 0u, D9C_COMMAND_RECORD_SET_VS_CONST_F},
      RenderTapeIdentityV3RecordFact{2u, 1u, D9C_COMMAND_RECORD_SET_PS_CONST_F}};
  check(view.attempts.size() == 2u && view.attempts[0].attemptGeneration == 1u &&
      view.attempts[1].attemptGeneration == 2u && view.attempts[1].beginEdge > view.attempts[0].terminalEdge &&
      validateRenderTapeIdentityV4Rows(view, events, facts) == RenderTapeIdentityV4Status::Valid,
      "real rollback/retry attempts stay distinct without duplicating canonical records");
}

void unsuccessfulRollbackFailsCaptureOnly() {
  auto device = std::make_unique<core::Device>(core::AdapterInfo{}, core::BackendLimits{},
      core::PresentParameters{}, 0u);
  constexpr ReplayTransactionRawIdentity raw{key.token, 2u, 1u, 0u};
  for (std::uint32_t failure = 0u; failure != 3u; ++failure) {
    RenderTapeRuntimeIdentityV4 runtime;
    check(runtime.begin(key, {.queue = writer.queue}), "failed rollback root begins");
    ReplayTransaction transaction;
    {
      ScopedReplayTransactionObserver scope(raw, runtime.replaySink());
      transaction.begin(projectionIdentity(1u), device.get());
      check(transaction.rollback(*device), "renderer semantic rollback succeeds independently");
      if (failure == 0u) noteDestinationRollback(transaction, false);
      if (failure == 1u) transaction.begin(projectionIdentity(1u), device.get());
      // failure 2 exits the scope without destination rollback evidence.
    }
    RenderTapeIdentityV4View view;
    RenderTapeExecutionTopologyV1View topology;
    check(runtime.failed() && runtime.topology().failed() && !runtime.edges().active() &&
        !runtime.snapshot(key, view, topology, false) && view.attempts.empty(),
        "failed, overwritten or unconfirmed destination rollback invalidates both capture domains");
    check(transaction.state().phase == (failure == 1u ? ReplayTransactionPhase::Created :
        ReplayTransactionPhase::RolledBack), "diagnostic failure never changes actual renderer outcome");
  }
}

void committedWritingPrefixSurvivesTentativeSuffixRollback() {
  auto device = std::make_unique<core::Device>(core::AdapterInfo{}, core::BackendLimits{},
      core::PresentParameters{}, 0u);
  RenderTapeRuntimeIdentityV4 runtime;
  check(runtime.begin(key, {.queue = writer.queue}), "shared Writing-prefix root begins");
  constexpr ReplayTransactionRawIdentity raw{key.token, 2u, 1u, 2u};
  ReplayTransaction transaction;
  core::ChunkSlot finalSlot{};
  {
    ScopedReplayTransactionObserver scope(raw, runtime.replaySink());
    transaction.begin(projectionIdentity(1u), device.get());
    record(runtime, transaction, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, true);
    appendDraw(finalSlot);
    observeAppendedDraw(runtime, finalSlot, 0u, {2u, 0u});
    noteExternalEffect(transaction, ReplayTransactionEffectWitness::DirectLease);
    receiveAndCommit(transaction, ReplayDestinationKind::DirectChunkSlot, 11u);
    const auto prefix = RenderTapeRuntimeIdentityV4TestAccess::bindings(runtime);
    check(prefix.records.size() == 1u && prefix.records[0].physical == 0u &&
        prefix.records[0].disposition.commandIndex == UINT32_MAX && prefix.writers[0][5] == 1u,
        "committed prefix owns actual physical mapping while source remains unpublished Writing");

    transaction.begin(projectionIdentity(1u), device.get(), 1u);
    record(runtime, transaction, 1u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, true);
    // This call-local candidate represents the actual tentative final-slot
    // suffix; production's successful destination rollback restores its
    // prior one-command prefix. The observer retains neither payload.
    core::ChunkSlot candidateSlot{};
    appendDraw(candidateSlot);
    appendDraw(candidateSlot);
    const std::array suffix{RenderTapeRuntimeRecordLocator{2u, 1u}};
    check(runtime.bindEmission(writer, 1u, core::MetalCommandKind::DrawRun, suffix),
        "explicit candidate append binds only current tentative suffix locators");
    check(RenderTapeRuntimeIdentityV4TestAccess::bindings(runtime).writers[0][5] == 2u,
        "candidate really advanced pre-existing writer's command count");
    check(transaction.rollback(*device), "tentative suffix actually semantically rolls back");
    noteDestinationRollback(transaction, true);
    check(RenderTapeRuntimeIdentityV4TestAccess::same(prefix,
            RenderTapeRuntimeIdentityV4TestAccess::bindings(runtime)),
        "confirmed rollback restores exact committed physical/subdraw mappings and writer facts");

    transaction.begin(projectionIdentity(1u), device.get(), 1u);
    record(runtime, transaction, 1u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, true);
    appendDraw(finalSlot);
    observeAppendedDraw(runtime, finalSlot, 1u, {2u, 1u});
    noteExternalEffect(transaction, ReplayTransactionEffectWitness::DirectLease);
    receiveAndCommit(transaction, ReplayDestinationKind::DirectChunkSlot, 13u);
  }
  check(runtime.acknowledgeRaw(raw, true), "only contiguous committed prefix plus real retry permits raw ACK");
  emit(runtime, {.edge = render::TapeRuntimeEdge::Publish, .writer = writer, .source = source(),
      .metadata = {.rawOrdinal = 1u, .sourceOrdinal = 7u, .seqId = 10u,
          .buildGeneration = 13u, .pageCount = 1u},
      .payload = core::SourcePayloadView(finalSlot), .controlIndex = 4u, .sequence = 10u});
  acceptAndEncodeAll(runtime, 2u);
  completeSource(runtime);
  RenderTapeIdentityV4View view;
  RenderTapeExecutionTopologyV1View topology;
  check(runtime.freeze(key, 4u) && runtime.snapshot(key, view, topology) && view.commands.size() == 2u &&
      view.dispositions.size() == 2u && view.attempts.size() == 3u &&
      view.dispositions[0].commandIndex == 0u && view.dispositions[1].commandIndex == 1u,
      "shared Writing source finally publishes prefix and successful suffix exactly once");
  const std::array events{RenderTapeIdentityV3EventFact{
      2u, static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 2u}};
  const std::array facts{
      RenderTapeIdentityV3RecordFact{2u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE},
      RenderTapeIdentityV3RecordFact{2u, 1u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE}};
  check(validateRenderTapeIdentityV4Rows(view, events, facts) == RenderTapeIdentityV4Status::Valid,
      "committed prefix, real local rollback and suffix retry satisfy production refinement predicates");

  // A late binding of a previously committed but still unbound prefix cannot
  // be silently attributed to a new rollbackable transaction.
  for (std::uint32_t explicitLocator = 0u; explicitLocator != 2u; ++explicitLocator) {
    RenderTapeRuntimeIdentityV4 guarded;
    check(guarded.begin(key, {.queue = writer.queue}), "tentative old-prefix ownership guard begins");
    ReplayTransaction attempt;
    ScopedReplayTransactionObserver scope(raw, guarded.replaySink());
    attempt.begin(projectionIdentity(1u), device.get());
    record(guarded, attempt, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
    receiveAndCommit(attempt, ReplayDestinationKind::Compatibility);
    const auto prior = RenderTapeRuntimeIdentityV4TestAccess::bindings(guarded);
    attempt.begin(projectionIdentity(1u), device.get(), 1u);
    if (explicitLocator) {
      const std::array old{RenderTapeRuntimeRecordLocator{2u, 0u}};
      check(!guarded.bindEmission(writer, 0u, core::MetalCommandKind::DrawRun, old),
          "tentative explicit locator cannot mutate earlier committed unbound record");
    } else {
      core::ChunkSlot slot{};
      appendDraw(slot);
      observeAppendedDraw(guarded, slot, 0u, {2u, 0u}, false);
    }
    check(guarded.failed() && RenderTapeRuntimeIdentityV4TestAccess::same(prior,
            RenderTapeRuntimeIdentityV4TestAccess::bindings(guarded)),
        "pre-effect append ownership failure preserves earlier committed mappings and writer facts");
  }
}

void arenaPublicationRequiresActualLocators() {
  for (std::uint32_t missing = 0u; missing != 2u; ++missing) {
    core::SourcePayloadCapacity capacity{};
    capacity.commandHeaders = capacity.clearRecords = 2u;
    const auto layout = core::makeSourcePayloadLayout(capacity, 4096u, 8u);
    check(layout.has_value(), "real bounded Arena layout is built");
    auto memory = core::allocateCpuReadyAlignedBytes(layout->usedBytes);
    core::ArenaSourcePayloadBlock block;
    core::ArenaSourcePayloadBuilder builder(block, *layout, {memory.get(), layout->usedBytes});
    check(builder.tryAppendClearCommand({}) && builder.tryAppendClearCommand({}) && builder.publish(),
        "two same-kind Arena extents are genuinely immutable");
    RenderTapeRuntimeIdentityV4 runtime;
    check(runtime.begin(key, {.queue = writer.queue}), "Arena root begins");
    constexpr ReplayTransactionRawIdentity raw{key.token, 2u, 1u, 3u};
    ReplayTransaction transaction;
    {
      ScopedReplayTransactionObserver scope(raw, runtime.replaySink());
      transaction.begin(projectionIdentity(1u));
      record(runtime, transaction, 0u, D9C_COMMAND_RECORD_CLEAR, true);
      record(runtime, transaction, 1u, D9C_COMMAND_RECORD_SET_VS_CONST_F);
      record(runtime, transaction, 2u, D9C_COMMAND_RECORD_CLEAR, true);
      if (!missing) {
        const std::array first{RenderTapeRuntimeRecordLocator{2u, 0u}};
        const std::array second{RenderTapeRuntimeRecordLocator{2u, 2u}};
        check(runtime.bindEmission(writer, 0u, core::MetalCommandKind::Clear, first) &&
            runtime.bindEmission(writer, 1u, core::MetalCommandKind::Clear, second),
            "assembler supplies copied original locators for each same-kind physical extent");
      }
      emit(runtime, {.edge = render::TapeRuntimeEdge::Publish, .writer = writer, .source = source(),
          .metadata = {.rawOrdinal = 1u, .sourceOrdinal = 7u, .seqId = 10u, .buildGeneration = 77u,
              .usedBytes = layout->usedBytes, .pageCount = 1u},
          .payload = core::SourcePayloadView(block), .controlIndex = 4u, .sequence = 10u});
      block.destroyConstructed();
      memory.reset();
      noteExternalEffect(transaction, ReplayTransactionEffectWitness::ArenaEmitted);
      receiveAndCommit(transaction, ReplayDestinationKind::Arena, 77u);
    }
    RenderTapeIdentityV4View view;
    RenderTapeExecutionTopologyV1View topology;
    if (missing) {
      check(runtime.failed() && !runtime.snapshot(key, view, topology, false),
          "Arena publication never infers bindings by first matching command kind");
      continue;
    }
    check(runtime.acknowledgeRaw(raw, true), "real Arena transaction and raw ACK remain separate facts");
    acceptAndEncodeAll(runtime, 2u);
    completeSource(runtime);
    check(runtime.freeze(key, 4u) && runtime.snapshot(key, view, topology) &&
        view.commands.size() == 2u && view.commands[0].kind == static_cast<std::uint32_t>(
            RenderTapeIdentityV3CommandKind::Clear) && view.dispositions[2].commandIndex == 1u &&
        view.attempts[0].stagedCommandCount == 2u && view.sources[0].publicationBuildGeneration == 77u,
        "publication snapshot owns neutral extents and real metadata after payload borrow ends");
    const std::array events{RenderTapeIdentityV3EventFact{
        2u, static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 3u}};
    const std::array facts{
        RenderTapeIdentityV3RecordFact{2u, 0u, D9C_COMMAND_RECORD_CLEAR},
        RenderTapeIdentityV3RecordFact{2u, 1u, D9C_COMMAND_RECORD_SET_VS_CONST_F},
        RenderTapeIdentityV3RecordFact{2u, 2u, D9C_COMMAND_RECORD_CLEAR}};
    check(validateRenderTapeIdentityV4Rows(view, events, facts) == RenderTapeIdentityV4Status::Valid,
        "actual Arena receipt is anchored before terminal by exact raw/build/source/control publication");
  }
}

void sharedQueryLifecycleUsesActualAttempt() {
  RenderTapeRuntimeIdentityV4 runtime;
  check(runtime.begin(key, {.queue = writer.queue, .sessionLaneEnabled = true}), "query root begins");
  constexpr ReplayTransactionRawIdentity raw{key.token, 2u, 1u, 1u};
  constexpr D9CWireObjectIdentity queryIdentity{D9C_CHUNK_HANDLE_KIND_QUERY, 1u, 9u};
  ReplayTransaction transaction;
  {
    ScopedReplayTransactionObserver scope(raw, runtime.replaySink());
    transaction.begin(projectionIdentity(1u));
    check(runtime.beginRecord(0u, D9C_COMMAND_RECORD_QUERY_ISSUE), "query BEGIN starts canonical coordinator");
    check(transaction.project({.stateGeneration = 7u, .source = 21u, .recordOrdinal = 0u}) &&
        transaction.startIrreversibleEffect(), "actual ordered-control effect precedes dispatch");
    emit(runtime, {.edge = render::TapeRuntimeEdge::OrderedRelease,
        .release = {.event = {.ordinal = 1u, .fenceRawOrdinal = 1u}, .generation = 1u}});
    check(runtime.beginDispatch(), "query dispatch remains at original record boundary");
    core::QueryPollSnapshot poll{};
    poll.type = core::QueryType::Occlusion;
    poll.issueOrdinal = 1u;
    poll.issueDeviceEpoch = 3u;
    poll.active = true;
    runtime.queryIssue(queryIdentity, 2u, poll, 0);
    check(runtime.endRecord(0), "shared core records successful real query coordinator");
    receiveAndCommit(transaction, ReplayDestinationKind::Compatibility);
  }
  RenderTapeIdentityV4View view;
  RenderTapeExecutionTopologyV1View topology;
  check(runtime.acknowledgeRaw(raw, true) && runtime.freeze(key, 4u) && runtime.snapshot(key, view, topology),
      "query coordinator closes through real transaction and raw ACK");
  const std::array events{RenderTapeIdentityV3EventFact{
      2u, static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 1u}};
  const std::array facts{RenderTapeIdentityV3RecordFact{
      .eventOrdinal = 2u, .recordIndex = 0u, .recordType = D9C_COMMAND_RECORD_QUERY_ISSUE,
      .queryFlags = 2u, .queryIdentity = queryIdentity, .queryType = 9u,
      .issueCycle = 1u, .issueOrdinal = 1u}};
  check(view.coordinators.size() == 1u && view.coordinators[0].provenance.buildGeneration == view.attempts[0].attemptGeneration &&
      view.attempts[0].effectEdge < view.coordinators[0].provenance.dispatchEdge &&
      view.coordinators[0].provenance.dispatchEdge < view.attempts[0].terminalEdge &&
      validateRenderTapeIdentityV4Rows(view, events, facts) == RenderTapeIdentityV4Status::Valid,
      "shared canonical query row joins actual V4 attempt, not completed V3 output");
}

constexpr D9CWireObjectIdentity replayQuery{D9C_CHUNK_HANDLE_KIND_QUERY, 7u, 91u};

RenderTapeRuntimeIdentityV4Limits queryLimits() {
  return {.provenance = {4u, 4u, 8u, 16u, 4u, 4u},
      .topology = {4u, 8u, 4u, 8u, 8u}, .maxAttempts = 4u};
}

core::QueryPollSnapshot setupEnd() {
  return {.identity = 77u, .type = core::QueryType::Event, .issueOrdinal = 1u,
      .issueDeviceEpoch = 3u, .backendFence = {writer.queue, 0u}, .eligible = true};
}

bool queryRecord(RenderTapeRuntimeIdentityV4& runtime, ReplayTransaction& transaction,
    ReplayTransactionRawIdentity raw, std::uint32_t recordIndex, std::uint32_t flags,
    const core::QueryPollSnapshot& poll, std::uint64_t cycle = 0u) {
  check(runtime.beginRecord(recordIndex, D9C_COMMAND_RECORD_QUERY_ISSUE) &&
      transaction.project({.stateGeneration = 7u, .source = 21u, .recordOrdinal = recordIndex}) &&
      transaction.startIrreversibleEffect(), "real query record has an irreversible effect witness");
  emit(runtime, {.edge = render::TapeRuntimeEdge::OrderedRelease,
      .release = {.event = {.ordinal = raw.rawOrdinal, .fenceRawOrdinal = raw.rawOrdinal}, .generation = 1u}});
  check(runtime.beginDispatch(), "replay query retains its original dispatch boundary");
  if (flags == 1u) {
    const auto fence = poll.backendFence.snapshot();
    emit(runtime, {.edge = render::TapeRuntimeEdge::QueryEnd,
        .sequence = fence.sequence, .initializer = fence.initializer});
  }
  runtime.queryIssue(replayQuery, flags, poll, 0, cycle);
  if (runtime.failed()) return false;
  return runtime.endRecord(0);
}

bool actualQueryIssue(RenderTapeRuntimeIdentityV4& runtime, const core::QueryPollSnapshot& poll,
    std::uint32_t flags = 1u, std::uint64_t cycle = 0u,
    ReplayTransactionRawIdentity raw = {key.token, 2u, 1u, 1u}) {
  ReplayTransaction transaction;
  bool admitted = false;
  {
    ScopedReplayTransactionObserver scope(raw, runtime.replaySink());
    transaction.begin(projectionIdentity(raw.rawOrdinal));
    admitted = queryRecord(runtime, transaction, raw, 0u, flags, poll, cycle);
    receiveAndCommit(transaction, ReplayDestinationKind::Compatibility);
  }
  return admitted && runtime.acknowledgeRaw(raw, true);
}

void atomicReplayQueryBaselineAdmission() {
  std::uint64_t cases = 0u;
  for (const auto ordinal : std::array<std::uint64_t, 4>{0u, 1u, 2u, UINT64_MAX})
    for (const auto epoch : {0ull, 3ull})
      for (const auto coreIdentity : {0ull, 77ull})
        for (bool failed : {false, true})
          for (std::uint32_t fenceKind = 0u; fenceKind != 9u; ++fenceKind) {
            RenderTapeRuntimeIdentityV4 runtime;
            check(runtime.begin(key, {.queue = writer.queue, .sequence = 2u, .completedSequence = 2u,
                .initializer = 2u, .completedInitializer = 2u}, queryLimits()), "seed matrix root begins");
            auto poll = setupEnd();
            poll.issueOrdinal = ordinal; poll.issueDeviceEpoch = epoch;
            poll.identity = coreIdentity; poll.failed = failed;
            if (fenceKind == 1u || fenceKind == 7u) poll.backendFence = {writer.queue, 1u, 1u};
            if (fenceKind == 2u) poll.backendFence = {writer.queue + 1u, 0u};
            if (fenceKind == 3u) poll.backendFence = {writer.queue, 3u};
            if (fenceKind == 4u) poll.backendFence = {writer.queue, 1u, 3u};
            if (fenceKind == 8u) poll.backendFence = {};
            if (fenceKind >= 5u && fenceKind <= 7u) {
              auto marker = std::make_shared<query::EndMarker>(writer);
              if (fenceKind != 5u) check(marker->bind(writer, 1u), "matrix marker really binds");
              if (fenceKind == 6u) marker->cancel();
              poll.backendFence.marker = marker;
            }
            const std::array seeds{RenderTapeRuntimeQueryBaselineV4{replayQuery, poll}};
            const bool expected = ordinal == 1u && epoch == 3u && coreIdentity == 77u && !failed &&
                (fenceKind == 0u || fenceKind == 1u || fenceKind == 7u);
            check(runtime.seedQueryBaselines(key, seeds) == expected,
                "seed cross-product rejects non-fresh, foreign, failed, unbound, cancelled and incomplete authority");
            const auto state = RenderTapeRuntimeIdentityV4TestAccess::queryState(runtime);
            check(state.strict == expected && state.queries.size() == (expected ? 1u : 0u) &&
                !state.records && !state.coordinators && !state.sources && !state.initializers,
                "failed admission enables neither replay mode nor partial authority or synthetic work");
            ++cases;
          }
  check(cases == 288u, "baseline seed relation exhausts bounded counterexample domain");
  for (std::uint32_t mutation = 0u; mutation != 11u; ++mutation) {
    RenderTapeRuntimeIdentityV4 runtime;
    check(runtime.begin(key, {.queue = writer.queue}, queryLimits()), "atomic two-seed root begins");
    std::array seeds{RenderTapeRuntimeQueryBaselineV4{replayQuery, setupEnd()},
        RenderTapeRuntimeQueryBaselineV4{{D9C_CHUNK_HANDLE_KIND_QUERY, 8u, 92u}, setupEnd()}};
    seeds[1].snapshot.identity = 78u;
    if (mutation == 0u) seeds[1].localIdentity = replayQuery;
    if (mutation == 1u) seeds[1].snapshot.identity = seeds[0].snapshot.identity;
    if (mutation == 2u) seeds[1].snapshot.issueDeviceEpoch = 4u;
    if (mutation == 3u) seeds[1].localIdentity.kind = D9C_CHUNK_HANDLE_KIND_TEXTURE;
    if (mutation == 4u) seeds[1].localIdentity.generation = 0u;
    if (mutation == 5u) seeds[1].localIdentity.objectId = 0u;
    if (mutation == 6u) seeds[1].snapshot.active = true;
    if (mutation == 7u) seeds[1].snapshot.type = core::QueryType::Occlusion;
    if (mutation == 8u) seeds[1].snapshot.eligible = false;
    if (mutation == 9u) seeds[1].snapshot.backendFence.prefix.queue = 9u;
    if (mutation == 10u) seeds[1].snapshot.issueOrdinal = 2u;
    check(!runtime.seedQueryBaselines(key, seeds), "invalid second seed rejects complete arm atomically");
    const auto state = RenderTapeRuntimeIdentityV4TestAccess::queryState(runtime);
    check(state.queries.empty() && !state.strict, "valid first seed never leaks from rejected batch");
  }
  RenderTapeRuntimeIdentityV4 bounded;
  check(bounded.begin(key, {.queue = writer.queue}, queryLimits()), "seed capacity root begins");
  std::array<RenderTapeRuntimeQueryBaselineV4, 5> oversized;
  for (std::uint32_t index = 0u; index != oversized.size(); ++index) {
    oversized[index] = {replayQuery, setupEnd()};
    oversized[index].localIdentity.objectId += index;
    oversized[index].snapshot.identity += index;
  }
  check(!bounded.seedQueryBaselines(key, oversized) &&
      RenderTapeRuntimeIdentityV4TestAccess::queryState(bounded).queries.empty(),
      "seed count cap rejects atomically before authority publication");
}

void exactCheckpointOnlyPollHasNoCoordinator() {
  std::uint64_t cases = 0u;
  for (const auto ordinal : std::array<std::uint64_t, 4>{0u, 1u, 2u, UINT64_MAX})
    for (const auto epoch : {3ull, 4ull})
      for (const auto coreIdentity : {77ull, 78ull})
        for (const auto result : {-1, 0, 1})
          for (std::uint32_t receiptKind = 0u; receiptKind != 6u; ++receiptKind) {
            RenderTapeRuntimeIdentityV4 runtime;
            check(runtime.begin(key, {.queue = writer.queue, .sequence = 2u, .completedSequence = 2u,
                .initializer = 2u, .completedInitializer = 2u}, queryLimits()), "checkpoint poll root begins");
            auto seed = setupEnd();
            seed.backendFence = {writer.queue, 1u, 1u};
            auto marker = std::make_shared<query::EndMarker>(writer);
            check(marker->bind(writer, 1u), "checkpoint poll retains its exact bound marker");
            seed.backendFence.marker = marker;
            const std::array seeds{RenderTapeRuntimeQueryBaselineV4{replayQuery, seed}};
            check(runtime.seedQueryBaselines(key, seeds), "authentic checkpoint poll seed admitted");
            const auto before = RenderTapeRuntimeIdentityV4TestAccess::queryState(runtime);
            auto poll = seed;
            poll.issueOrdinal = ordinal; poll.issueDeviceEpoch = epoch; poll.identity = coreIdentity;
            if (receiptKind == 1u) poll.backendFence.prefix.queue = 9u;
            if (receiptKind == 2u) {
              auto other = std::make_shared<query::EndMarker>(writer);
              check(other->bind(writer, 1u), "substituted marker has equal scalar labels");
              poll.backendFence.marker = other;
            }
            if (receiptKind == 3u) ++poll.backendFence.prefix.sequence;
            if (receiptKind == 4u) ++poll.backendFence.prefix.initializer;
            if (receiptKind == 5u) marker->cancel();
            runtime.queryPoll(replayQuery, poll, result);
            const bool accepted = ordinal == 1u && epoch == 3u && coreIdentity == 77u &&
                result == 0 && receiptKind == 0u;
            check(runtime.failed() != accepted, "checkpoint Poll requires exact current completed seed receipt");
            check(RenderTapeRuntimeIdentityV4TestAccess::queryState(runtime) == before,
                "checkpoint-only Poll never creates a coordinator, source, record or synthetic Issue");
            ++cases;
          }
  check(cases == 288u, "checkpoint Poll relation exhausts bounded exact-receipt domain");
  RenderTapeRuntimeIdentityV4 promoted;
  check(promoted.begin(key, {.queue = writer.queue, .sequence = 1u, .completedSequence = 1u}, queryLimits()),
      "promoted checkpoint root starts at actual completed prefix");
  auto retained = setupEnd();
  retained.backendFence = {writer.queue, 0u};
  auto marker = std::make_shared<query::EndMarker>(writer);
  check(marker->bind(writer, 1u), "writer-bound marker has actual publication");
  retained.backendFence.marker = marker;
  const std::array seeds{RenderTapeRuntimeQueryBaselineV4{replayQuery, retained}};
  check(promoted.seedQueryBaselines(key, seeds), "retained promoted eligible snapshot seeds exact receipt");
  auto fresh = retained;
  fresh.eligible = false;
  promoted.queryPoll(replayQuery, fresh, 0);
  check(!promoted.failed() && !RenderTapeRuntimeIdentityV4TestAccess::queryState(promoted).coordinators,
      "fresh ineligible snapshot can observe same completed retained receipt without fabricating a coordinator");
  auto staleWire = replayQuery;
  ++staleWire.generation;
  promoted.queryPoll(staleWire, fresh, 0);
  check(promoted.failed(), "stale wire generation cannot bypass exact seeded query lookup as unknown pre-arm Poll");
}

void replayQueryContinuityCrossProduct() {
  std::uint64_t cases = 0u;
  for (bool seeded : {false, true})
    for (const auto ordinal : std::array<std::uint64_t, 5>{0u, 1u, 2u, 3u, UINT64_MAX})
      for (const auto coreIdentity : {77ull, 78ull})
        for (const auto epoch : {3ull, 4ull})
          for (const auto flags : {1u, 2u})
            for (bool active : {false, true})
              for (bool failed : {false, true})
                for (const auto cycle : {0ull, 2ull, 47ull}) {
                  RenderTapeRuntimeIdentityV4 runtime;
                  check(runtime.begin(key, {.queue = writer.queue, .sessionLaneEnabled = true}, queryLimits()),
                      "strict local query root begins");
                  const auto initial = setupEnd();
                  const std::array seeds{RenderTapeRuntimeQueryBaselineV4{replayQuery, initial}};
                  check(runtime.seedQueryBaselines(key, seeded ? std::span<const RenderTapeRuntimeQueryBaselineV4>(seeds) :
                      std::span<const RenderTapeRuntimeQueryBaselineV4>{}), "private arm selects replay-only query mode");
                  if (!seeded) check(actualQueryIssue(runtime, initial), "fresh query records its first actual END");
                  auto next = initial;
                  next.issueOrdinal = ordinal; next.identity = coreIdentity; next.issueDeviceEpoch = epoch;
                  next.active = active; next.failed = failed;
                  if (flags == 2u) next.backendFence = {};
                  const ReplayTransactionRawIdentity raw{key.token, seeded ? 2u : 3u, seeded ? 1u : 2u, 1u};
                  const bool expected = ordinal == 2u && coreIdentity == 77u && epoch == 3u &&
                      active == (flags == 2u) && !failed && cycle != 47u;
                  check(actualQueryIssue(runtime, next, flags, cycle, raw) == expected,
                      "local core/epoch/ordinal and reducer-active/cycle adjacency hold even with explicit cycle");
                  const auto state = RenderTapeRuntimeIdentityV4TestAccess::queryState(runtime);
                  check(state.queries.size() == 1u && state.queries[0][3] == (expected ? 2u : 1u),
                      "rejected Issue cannot replace the authenticated query counter history");
                  if (expected) {
                    RenderTapeIdentityV4View view;
                    RenderTapeExecutionTopologyV1View topology;
                    check(runtime.freeze(key, 9u) && runtime.snapshot(key, view, topology) &&
                        view.coordinators.back().provenance.issueOrdinal == 2u &&
                        view.coordinators.back().provenance.issueCycle == 2u && view.sources.empty() &&
                        view.commands.empty() && view.attempts.front().beginEdge == 2u,
                        "actual rows retain local counters and seed admission consumes no source or edge");
                  }
                  ++cases;
                }
  check(cases == 960u, "strict Issue relation exhausts seeded and fresh bounded transitions");
}

void replayQueryReissueAndModeBoundaries() {
  for (std::uint32_t mutation = 0u; mutation != 6u; ++mutation) {
    RenderTapeRuntimeIdentityV4 runtime;
    check(runtime.begin(key, {.queue = writer.queue, .sessionLaneEnabled = true}, queryLimits()),
        "reissue receipt root begins");
    auto initial = setupEnd();
    const std::array seeds{RenderTapeRuntimeQueryBaselineV4{replayQuery, initial}};
    check(runtime.seedQueryBaselines(key, seeds), "reissue receipt seed admitted");
    auto next = initial; next.issueOrdinal = 2u;
    check(actualQueryIssue(runtime, next, 1u, 2u), "first actual END advances restored setup END");
    auto poll = mutation == 1u ? initial : next;
    if (mutation == 2u) ++poll.backendFence.prefix.queue;
    if (mutation == 3u) ++poll.backendFence.prefix.sequence;
    if (mutation == 4u) ++poll.backendFence.prefix.initializer;
    auto identity = replayQuery;
    if (mutation == 5u) ++identity.generation;
    runtime.queryPoll(identity, poll, 0);
    check(runtime.failed() == (mutation != 0u),
        "old checkpoint Poll or foreign/replaced actual END receipt cannot qualify replacement coordinator");
  }
  RenderTapeRuntimeIdentityV4 legacy;
  check(legacy.begin(key, {.queue = writer.queue, .sessionLaneEnabled = true}, queryLimits()),
      "ordinary V4 capture retains non-replay policy");
  auto captured = setupEnd(); captured.identity = 0u; captured.issueOrdinal = 7u; captured.active = true;
  check(actualQueryIssue(legacy, captured, 2u, 47u) &&
      !RenderTapeRuntimeIdentityV4TestAccess::queryState(legacy).strict,
      "without private seed admission existing explicit captured-cycle behavior is unchanged");

  RenderTapeRuntimeIdentityV4 beginEnd;
  check(beginEnd.begin(key, {.queue = writer.queue, .sessionLaneEnabled = true}, queryLimits()), "BEGIN/END seed root begins");
  const std::array beginSeeds{RenderTapeRuntimeQueryBaselineV4{replayQuery, setupEnd()}};
  check(beginEnd.seedQueryBaselines(key, beginSeeds), "BEGIN/END query installs actual closed setup END");
  auto begin = setupEnd(); begin.issueOrdinal = 2u; begin.active = true; begin.backendFence = {};
  check(actualQueryIssue(beginEnd, begin, 2u, 2u), "replacement BEGIN advances cycle and actual ordinal");
  auto end = setupEnd(); end.issueOrdinal = 3u;
  check(actualQueryIssue(beginEnd, end, 1u, 2u, {key.token, 3u, 2u, 1u}),
      "closing END advances ordinal without counting another BEGIN cycle");
  const auto closed = RenderTapeRuntimeIdentityV4TestAccess::queryState(beginEnd);
  check(closed.queries.size() == 1u && closed.queries[0][3] == 3u && closed.queries[0][5] == 2u,
      "shared pure query reducer keeps BEGIN/END cycle separate from absolute Issue count");

  for (std::uint32_t boundary = 0u; boundary != 5u; ++boundary) {
    auto runtime = std::make_shared<RenderTapeRuntimeIdentityV4>();
    check(runtime->begin(key, {.queue = writer.queue}, queryLimits()), "seed boundary root begins");
    render::TapeRuntimeObserverEndpoint endpoint;
    check(endpoint.attachRetained(runtime), "seed boundary has actual retained attachment");
    const std::array seeds{RenderTapeRuntimeQueryBaselineV4{replayQuery, setupEnd()}};
    if (boundary == 0u) check(runtime->seedQueryBaselines(key, seeds), "first admission succeeds");
    if (boundary == 1u) check(runtime->setCut(2u, RenderTapeIdentityV3CutKind::BeforeEvent), "late seed crosses event cut");
    if (boundary == 2u) check(runtime->freeze(key, 9u), "unmutated root freezes before wrong seed call");
    if (boundary == 3u) check(endpoint.revoke(runtime->sink()), "retirement closes seed admission");
    const auto before = RenderTapeRuntimeIdentityV4TestAccess::queryState(*runtime);
    const auto supplied = boundary == 4u ? RenderTapeIdentityV4CaptureKey{key.token + 1u, key.epoch} : key;
    check(!runtime->seedQueryBaselines(supplied, seeds), "duplicate, late, frozen, retired or foreign arm seed rejects");
    check(RenderTapeRuntimeIdentityV4TestAccess::queryState(*runtime) == before,
        "invalid boundary cannot mutate root authority or replay mode");
    if (boundary == 2u || boundary == 3u) check(!runtime->failed(), "frozen/retired transport stays unpoisoned");
  }
  RenderTapeRuntimeIdentityV4 active;
  check(active.begin(key, {.queue = writer.queue}, queryLimits()), "active-attempt seed root begins");
  ReplayTransaction transaction;
  ScopedReplayTransactionObserver scope({key.token, 2u, 1u, 0u}, active.replaySink());
  transaction.begin(projectionIdentity(1u));
  check(!active.seedQueryBaselines(key, {}) &&
      !RenderTapeRuntimeIdentityV4TestAccess::queryState(active).strict,
      "even empty-batch replay mode cannot be admitted after an actual attempt begins");
}

void queryBaselineRollbackPreservesOnlyPreEffectHistory() {
  auto device = std::make_unique<core::Device>(core::AdapterInfo{}, core::BackendLimits{},
      core::PresentParameters{}, 0u);
  RenderTapeRuntimeIdentityV4 runtime;
  check(runtime.begin(key, {.queue = writer.queue, .sessionLaneEnabled = true}, queryLimits()), "seed rollback root begins");
  const std::array seeds{RenderTapeRuntimeQueryBaselineV4{replayQuery, setupEnd()}};
  check(runtime.seedQueryBaselines(key, seeds), "rollback begins with one authenticated seed");
  constexpr ReplayTransactionRawIdentity raw{key.token, 2u, 1u, 2u};
  ReplayTransaction transaction;
  {
    ScopedReplayTransactionObserver scope(raw, runtime.replaySink());
    transaction.begin(projectionIdentity(1u), device.get());
    record(runtime, transaction, 0u, D9C_COMMAND_RECORD_SET_VS_CONST_F);
    check(transaction.rollback(*device), "nonquery prefix rolls back before any irreversible effect");
    noteDestinationRollback(transaction, true);
    const auto restored = RenderTapeRuntimeIdentityV4TestAccess::queryState(runtime);
    check(!runtime.failed() && restored.queries.size() == 1u && restored.queries[0][3] == 1u &&
        !restored.records && !restored.coordinators, "confirmed pre-effect rollback retains unchanged baseline authority");
    transaction.begin(projectionIdentity(1u), device.get());
    record(runtime, transaction, 0u, D9C_COMMAND_RECORD_SET_VS_CONST_F);
    auto next = setupEnd(); next.issueOrdinal = 2u;
    check(queryRecord(runtime, transaction, raw, 1u, 1u, next, 2u), "retry contributes exactly one actual query END");
    receiveAndCommit(transaction, ReplayDestinationKind::Compatibility);
  }
  RenderTapeIdentityV4View view;
  RenderTapeExecutionTopologyV1View topology;
  check(runtime.acknowledgeRaw(raw, true) && runtime.freeze(key, 9u) && runtime.snapshot(key, view, topology) &&
      view.attempts.size() == 2u && view.coordinators.size() == 1u && view.dispositions.size() == 2u &&
      view.sources.empty() && view.coordinators[0].provenance.issueOrdinal == 2u,
      "rollback/retry cannot duplicate seed authority, query effects or committed canonical coverage");

  for (bool effect : {false, true}) {
    RenderTapeRuntimeIdentityV4 rejected;
    check(rejected.begin(key, {.queue = writer.queue, .sessionLaneEnabled = true}, queryLimits()) &&
        rejected.seedQueryBaselines(key, seeds), "query-before-endRecord rollback counterexample begins");
    ReplayTransaction actual;
    constexpr ReplayTransactionRawIdentity oneRecord{key.token, 2u, 1u, 1u};
    {
      ScopedReplayTransactionObserver scope(oneRecord, rejected.replaySink());
      actual.begin(projectionIdentity(1u), device.get());
      check(rejected.beginRecord(0u, D9C_COMMAND_RECORD_QUERY_ISSUE) &&
          actual.project({.stateGeneration = 7u, .source = 21u, .recordOrdinal = 0u}), "query mutation starts exact record");
      if (effect) check(actual.startIrreversibleEffect(), "query effect becomes irreversibly witnessed");
      emit(rejected, {.edge = render::TapeRuntimeEdge::OrderedRelease,
          .release = {.event = {.ordinal = 1u, .fenceRawOrdinal = 1u}, .generation = 1u}});
      check(rejected.beginDispatch(), "counterexample reaches actual query dispatch");
      auto next = setupEnd(); next.issueOrdinal = 2u;
      rejected.queryIssue(replayQuery, 1u, next, 0, 2u);
      if (effect) {
        check(!actual.rollback(*device) && actual.failStop(), "actual post-Issue effect cannot become rollback/retry");
      } else {
        check(actual.rollback(*device), "missing-effect rejection does not change actual renderer rollback");
        noteDestinationRollback(actual, true);
      }
    }
    const auto state = RenderTapeRuntimeIdentityV4TestAccess::queryState(rejected);
    check(rejected.failed() && state.queries.size() == 1u && !state.coordinators &&
        state.queries[0][3] == (effect ? 2u : 1u) && !rejected.freeze(key, 9u),
        "equal query/coordinator counts cannot authorize replay of a mutated irreversible query history");
  }
}

void exactSourceLocatorAndAttemptBudgetFailures() {
  for (std::uint32_t mismatch = 0u; mismatch != 2u; ++mismatch) {
    RenderTapeRuntimeIdentityV4 runtime;
    check(runtime.begin(key, {.queue = writer.queue}), "exact source negative begins");
    constexpr ReplayTransactionRawIdentity raw{key.token, 2u, 1u, 1u};
    core::ChunkSlot slot{};
    ReplayTransaction transaction;
    {
      ScopedReplayTransactionObserver scope(raw, runtime.replaySink());
      transaction.begin(projectionIdentity(1u));
      record(runtime, transaction, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, true);
      appendDraw(slot);
      observeAppendedDraw(runtime, slot, 0u, {2u, 0u});
      check(transaction.startIrreversibleEffect(), "actual effect establishes source-negative trace");
      receiveAndCommit(transaction, ReplayDestinationKind::Compatibility);
    }
    check(runtime.acknowledgeRaw(raw, true), "negative raw really acknowledged");
    emit(runtime, {.edge = render::TapeRuntimeEdge::Publish, .writer = writer, .source = source(),
        .metadata = {.sourceOrdinal = 7u, .seqId = 10u, .pageCount = 1u},
        .payload = core::SourcePayloadView(slot), .controlIndex = 4u, .sequence = 10u});
    auto mixed = source();
    if (!mismatch) ++mixed.storage.firstPage;
    else ++mixed.storage.pageCount;
    emit(runtime, {.edge = render::TapeRuntimeEdge::Submit, .source = mixed, .sequence = 10u});
    check(runtime.failed() && runtime.topology().failed(),
        "Submit cannot match only source generations while mixing full storage range");
  }
  RenderTapeRuntimeIdentityV4 runtime;
  RenderTapeRuntimeIdentityV4Limits limits;
  limits.maxAttempts = 1u;
  check(runtime.begin(key, {.queue = writer.queue}, limits), "attempt budget reserves bounded cold storage");
  constexpr ReplayTransactionRawIdentity raw{key.token, 2u, 1u, 1u};
  ReplayTransaction transaction;
  {
    ScopedReplayTransactionObserver scope(raw, runtime.replaySink());
    transaction.begin(projectionIdentity(1u));
    record(runtime, transaction, 0u, D9C_COMMAND_RECORD_SET_VS_CONST_F);
    receiveAndCommit(transaction, ReplayDestinationKind::Compatibility);
    transaction.begin(projectionIdentity(1u), nullptr, 1u);
    check(transaction.state().phase == ReplayTransactionPhase::Created,
        "exhausted diagnostic budget cannot change next production begin");
  }
  RenderTapeIdentityV4View view;
  RenderTapeExecutionTopologyV1View topology;
  check(runtime.failed() && !runtime.edges().active() && !runtime.snapshot(key, view, topology, false),
      "attempt capacity failure is sticky and exports no authentic evidence");
}

void publishedSourceOriginsBeforeCoordinatorFinishes() {
  RenderTapeRuntimeIdentityV4 runtime;
  check(runtime.begin(key, {.queue = writer.queue, .sessionLaneEnabled = true}), "origin pull root begins");
  constexpr ReplayTransactionRawIdentity raw{key.token, 2u, 1u, 2u};
  ReplayTransaction transaction;
  core::ChunkSlot slot{};
  const auto publication = originPublication();
  RenderTapeRuntimePublishedSourceOriginsV4 observed;
  {
    ScopedReplayTransactionObserver scope(raw, runtime.replaySink());
    transaction.begin(projectionIdentity(raw.rawOrdinal));
    check(runtime.beginRecord(0u, D9C_COMMAND_RECORD_UPDATE_TEXTURE) &&
        transaction.project({.stateGeneration = 7u, .source = 21u, .recordOrdinal = 0u}) &&
        transaction.startIrreversibleEffect(), "generated-command origin starts one authentic coordinator effect");
    emit(runtime, {.edge = render::TapeRuntimeEdge::OrderedRelease,
        .release = {.event = {.ordinal = 1u, .fenceRawOrdinal = 1u}, .generation = 1u}});
    check(runtime.beginDispatch(), "generated-command origin precedes coordinator completion");
    slot.appendSurfaceCopy({});
    const std::array locator{RenderTapeRuntimeRecordLocator{raw.eventOrdinal, 0u}};
    check(runtime.bindEmission(writer, 0u, core::MetalCommandKind::SurfaceCopy, locator),
        "actual generated append supplies original coordinator locator before endRecord");
    emit(runtime, {.edge = render::TapeRuntimeEdge::Append, .writer = writer, .source = source(),
        .payload = core::SourcePayloadView(slot), .commandIndex = 0u});
    check(!runtime.copyPublishedSourceOrigins(originCapture, publication, observed) && emptyOrigins(observed) &&
        !runtime.failed(), "Writing command is not a published origin authority and query cannot poison capture");
    check(!runtime.copyPublishedSourceOrigins(originCapture, publication.source, publication.metadata.seqId,
        observed) && emptyOrigins(observed) && !runtime.failed(),
        "actual Writing source and sequence cannot fabricate an owned publication receipt");
    emit(runtime, {.edge = render::TapeRuntimeEdge::Publish, .writer = writer, .source = source(),
        .metadata = publication.metadata, .payload = core::SourcePayloadView(slot),
        .controlIndex = publication.controlIndex, .sequence = publication.metadata.seqId});
    RenderTapeIdentityV4View partial;
    RenderTapeExecutionTopologyV1View partialTopology;
    check(runtime.snapshot(key, partial, partialTopology, false) && partial.coordinators.empty() &&
        partial.commands.size() == 1u && partial.commands[0].coordinatorIndex == 0u &&
        partial.dispositions[0].coordinatorIndex == UINT32_MAX,
        "ordinary partial snapshot still lacks the active coordinator's final row");
    check(runtime.copyPublishedSourceOrigins(originCapture, publication, observed) &&
        observed.capture == originCapture && observed.source.source == publication.source &&
        observed.source.sequence == publication.metadata.seqId && observed.source.commandCount == 1u &&
        observed.source.publicationEdge && observed.metadata == publication.metadata &&
        observed.controlIndex == publication.controlIndex && observed.commands.size() == 1u &&
        observed.origins.size() == 1u && observed.origins[0] == RenderTapeRuntimePublishedSourceOriginV4{
            0u, static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::SurfaceCopy), 0u, 0u, {2u, 0u}},
        "published source returns exact generated canonical origin without waiting for coordinator/raw completion");
    RenderTapeRuntimePublishedSourceOriginsV4 lookup;
    check(runtime.copyPublishedSourceOrigins(originCapture, publication.source, publication.metadata.seqId,
        lookup) && sameOrigins(lookup, observed),
        "actual source and sequence pull exactly the owned receipt's full projection before coordinator end/raw ACK");

    for (std::uint32_t mismatch = 0u; mismatch != 14u; ++mismatch) {
      auto staleCapture = originCapture;
      auto staleSource = publication.source;
      auto staleSequence = publication.metadata.seqId;
      if (mismatch == 0u) ++staleCapture.token;
      if (mismatch == 1u) ++staleCapture.epoch;
      if (mismatch == 2u) ++staleCapture.queue;
      if (mismatch == 3u) ++staleSource.id.index;
      if (mismatch == 4u) ++staleSource.id.generation;
      if (mismatch == 5u) ++staleSource.storage.firstPage;
      if (mismatch == 6u) ++staleSource.storage.pageCount;
      if (mismatch == 7u) ++staleSource.storage.generation;
      if (mismatch == 8u) ++staleSequence;
      if (mismatch == 9u) staleSequence = 0u;
      if (mismatch == 10u) staleSource = {};
      if (mismatch == 11u) staleCapture = {};
      if (mismatch == 12u) staleSource.id.generation = 0u;
      if (mismatch == 13u) staleSource.storage.generation = 0u;
      auto rejected = lookup;
      check(!runtime.copyPublishedSourceOrigins(staleCapture, staleSource, staleSequence, rejected) &&
          emptyOrigins(rejected) && !runtime.failed(),
          "invalid/foreign/stale full source, sequence or capture clears lookup output without poisoning");
    }

    for (std::uint32_t mismatch = 0u; mismatch != 17u; ++mismatch) {
      auto staleCapture = originCapture;
      auto stale = publication;
      if (mismatch == 0u) ++staleCapture.token;
      if (mismatch == 1u) ++staleCapture.epoch;
      if (mismatch == 2u) ++staleCapture.queue;
      if (mismatch == 3u) ++stale.source.id.index;
      if (mismatch == 4u) ++stale.source.id.generation;
      if (mismatch == 5u) ++stale.source.storage.firstPage;
      if (mismatch == 6u) ++stale.source.storage.pageCount;
      if (mismatch == 7u) ++stale.source.storage.generation;
      if (mismatch == 8u) ++stale.metadata.rawOrdinal;
      if (mismatch == 9u) ++stale.metadata.sourceOrdinal;
      if (mismatch == 10u) ++stale.metadata.seqId;
      if (mismatch == 11u) ++stale.metadata.buildGeneration;
      if (mismatch == 12u) ++stale.metadata.usedBytes;
      if (mismatch == 13u) ++stale.metadata.pageCount;
      if (mismatch == 14u) ++stale.metadata.paddingPagesBefore;
      if (mismatch == 15u) ++stale.controlIndex;
      if (mismatch == 16u) stale.hasPresent = true;
      auto rejected = observed;
      check(!runtime.copyPublishedSourceOrigins(staleCapture, stale, rejected) && emptyOrigins(rejected) &&
          !runtime.failed(), "stale exact receipt/capture rejection clears output without root mutation");
    }
    auto strict = publication;
    strict.metadata.strictAdmission = !strict.metadata.strictAdmission;
    auto rejected = observed;
    check(!runtime.copyPublishedSourceOrigins(originCapture, strict, rejected) && emptyOrigins(rejected) &&
        !runtime.failed(), "publication admission policy is part of exact receipt equality");

    check(transaction.stage({.commandCount = 1u, .byteCount = 8u}) && runtime.endRecord(0) &&
        runtime.beginRecord(1u, D9C_COMMAND_RECORD_QUERY_ISSUE) &&
        transaction.project({.stateGeneration = 7u, .source = 21u, .recordOrdinal = 1u}),
        "unrelated coordinator opens after selected source publication");
    check(runtime.copyPublishedSourceOrigins(originCapture, publication, rejected) &&
        rejected.origins == observed.origins, "unrelated active coordinator cannot suppress a published source's origins");
    check(runtime.copyPublishedSourceOrigins(originCapture, publication.source, publication.metadata.seqId,
        lookup) && sameOrigins(lookup, rejected),
        "source/sequence lookup stays exact while an unrelated coordinator is open");

    // Readers only retain copied neutral values. Dispatch/commit can continue
    // while the consumer copies source origins and ordinary partial metadata.
    std::atomic<bool> startReader{false}, readerOkay{true};
    std::jthread reader([&] {
      while (!startReader.load(std::memory_order_acquire)) std::this_thread::yield();
      for (std::uint32_t index = 0u; index != 128u; ++index) {
        RenderTapeRuntimePublishedSourceOriginsV4 copy;
        RenderTapeRuntimePublishedSourceOriginsV4 lookupCopy;
        RenderTapeIdentityV4View identity;
        RenderTapeExecutionTopologyV1View topology;
        if (!runtime.copyPublishedSourceOrigins(originCapture, publication, copy) ||
            !runtime.copyPublishedSourceOrigins(originCapture, publication.source, publication.metadata.seqId,
                lookupCopy) || !sameOrigins(copy, lookupCopy) ||
            copy.origins != observed.origins || !runtime.snapshot(key, identity, topology, false))
          readerOkay.store(false, std::memory_order_release);
      }
    });
    startReader.store(true, std::memory_order_release);
    emit(runtime, {.edge = render::TapeRuntimeEdge::OrderedRelease,
        .release = {.event = {.ordinal = 2u, .fenceRawOrdinal = 1u}, .generation = 2u}});
    const bool dispatch = runtime.beginDispatch();
    core::QueryPollSnapshot poll{};
    poll.type = core::QueryType::Occlusion;
    poll.issueOrdinal = 1u;
    poll.issueDeviceEpoch = 3u;
    poll.active = true;
    if (dispatch) runtime.queryIssue({D9C_CHUNK_HANDLE_KIND_QUERY, 1u, 9u}, 2u, poll, 0);
    const bool end = dispatch && runtime.endRecord(0);
    if (end) receiveAndCommit(transaction, ReplayDestinationKind::Compatibility);
    reader.join();
    check(dispatch && end && readerOkay.load(std::memory_order_acquire),
        "source-specific copying stays stable during unrelated producer metadata mutations");
  }
  check(runtime.acknowledgeRaw(raw, true), "origin query does not replace genuine raw ACK");
  acceptAndEncodeAll(runtime, 1u);
  completeSource(runtime);
  check(runtime.freeze(key, 4u) && runtime.copyPublishedSourceOrigins(originCapture, publication, observed) &&
      observed.origins.size() == 1u, "frozen retained source origins remain readable without reopening replay");
  RenderTapeRuntimePublishedSourceOriginsV4 frozenLookup;
  check(runtime.copyPublishedSourceOrigins(originCapture, publication.source, publication.metadata.seqId,
      frozenLookup) && sameOrigins(frozenLookup, observed),
      "frozen retained source/sequence lookup preserves exact immutable publication projection");
}

void combinedDrawRunAndReusedSourceOrigins() {
  RenderTapeRuntimeIdentityV4 runtime;
  check(runtime.begin(key, {.queue = writer.queue}), "combined compatibility origin root begins");
  ReplayTransaction transaction;
  constexpr ReplayTransactionRawIdentity first{key.token, 2u, 1u, 2u};
  constexpr ReplayTransactionRawIdentity second{key.token, 3u, 2u, 1u};
  for (const auto raw : {first, second}) {
    {
      ScopedReplayTransactionObserver scope(raw, runtime.replaySink());
      transaction.begin(projectionIdentity(raw.rawOrdinal));
      for (std::uint32_t index = 0u; index < raw.totalRawRecords; ++index)
        record(runtime, transaction, index, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, true);
      receiveAndCommit(transaction, ReplayDestinationKind::Compatibility);
    }
    check(runtime.acknowledgeRaw(raw, true), "compatibility records retain canonical origins across raw ACKs");
  }
  core::ChunkSlot combined{};
  std::array<core::DrawParam, 3> draws{};
  std::array<core::DrawParamPayloadView, 3> payloads{};
  check(combined.appendDrawRun({}, {}, draws, payloads), "final storage combines three original draws into one command");
  const std::array locators{RenderTapeRuntimeRecordLocator{2u, 0u}, RenderTapeRuntimeRecordLocator{2u, 1u},
      RenderTapeRuntimeRecordLocator{3u, 0u}};
  check(runtime.bindEmission(writer, 0u, core::MetalCommandKind::DrawRun, locators),
      "actual compatibility flush binds original event/record order to final subdraws");
  auto publication = originPublication();
  publication.metadata.rawOrdinal = second.rawOrdinal;
  emit(runtime, {.edge = render::TapeRuntimeEdge::Publish, .writer = writer, .source = source(),
      .metadata = publication.metadata, .payload = core::SourcePayloadView(combined),
      .controlIndex = publication.controlIndex, .sequence = publication.metadata.seqId});
  RenderTapeRuntimePublishedSourceOriginsV4 original;
  check(runtime.copyPublishedSourceOrigins(originCapture, publication, original) &&
      original.commands.size() == 1u && original.commands[0].drawCount == 3u && original.origins.size() == 3u,
      "source origin pull covers every actual final-slot subdraw, not one context per publication");
  for (std::uint32_t index = 0u; index != locators.size(); ++index)
    check(original.origins[index] == RenderTapeRuntimePublishedSourceOriginV4{0u,
        static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::DrawRun), index, 1u, locators[index]},
        "source-local subdraw selects its own prior canonical resource context");
  acceptAndEncodeAll(runtime, 1u);
  completeSource(runtime);

  auto nextWriter = writer;
  ++nextWriter.generation;
  ++nextWriter.storageGeneration;
  auto nextPublication = publication;
  ++nextPublication.source.id.generation;
  ++nextPublication.source.storage.generation;
  ++nextPublication.metadata.rawOrdinal;
  ++nextPublication.metadata.sourceOrdinal;
  ++nextPublication.metadata.seqId;
  ++nextPublication.metadata.buildGeneration;
  constexpr ReplayTransactionRawIdentity third{key.token, 4u, 3u, 1u};
  core::ChunkSlot reused{};
  {
    ScopedReplayTransactionObserver scope(third, runtime.replaySink());
    transaction.begin(projectionIdentity(third.rawOrdinal));
    record(runtime, transaction, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, true);
    appendDraw(reused);
    const std::array next{RenderTapeRuntimeRecordLocator{4u, 0u}};
    check(runtime.bindEmission(nextWriter, 0u, core::MetalCommandKind::DrawRun, next),
        "reused resident slot owns a different full source/storage generation");
    check(transaction.startIrreversibleEffect(), "reused source publication has actual irreversible effect");
    emit(runtime, {.edge = render::TapeRuntimeEdge::Publish, .writer = nextWriter,
        .source = nextPublication.source, .metadata = nextPublication.metadata,
        .payload = core::SourcePayloadView(reused), .controlIndex = nextPublication.controlIndex,
        .sequence = nextPublication.metadata.seqId});
    receiveAndCommit(transaction, ReplayDestinationKind::Compatibility);
  }
  check(runtime.acknowledgeRaw(third, true), "reused source raw commits independently");
  RenderTapeRuntimePublishedSourceOriginsV4 next;
  check(runtime.copyPublishedSourceOrigins(originCapture, nextPublication, next) && next.origins.size() == 1u &&
      next.origins[0].record == RenderTapeRuntimeRecordLocator{4u, 0u} &&
      runtime.copyPublishedSourceOrigins(originCapture, publication, original) && original.origins.size() == 3u,
      "old immutable publication remains distinct from reuse, never resolves by resident index alone");
  RenderTapeRuntimePublishedSourceOriginsV4 originalLookup, nextLookup;
  check(runtime.copyPublishedSourceOrigins(originCapture, publication.source, publication.metadata.seqId,
      originalLookup) && sameOrigins(originalLookup, original) &&
      runtime.copyPublishedSourceOrigins(originCapture, nextPublication.source, nextPublication.metadata.seqId,
          nextLookup) && sameOrigins(nextLookup, next),
      "bounded source/sequence lookup distinguishes old retained publication from actual reused generation");
  for (std::uint32_t mismatch = 0u; mismatch != 3u; ++mismatch) {
    auto staleSource = nextPublication.source;
    auto staleSequence = nextPublication.metadata.seqId;
    if (mismatch == 0u) staleSource.id.generation = publication.source.id.generation;
    if (mismatch == 1u) staleSource.storage.generation = publication.source.storage.generation;
    if (mismatch == 2u) staleSequence = publication.metadata.seqId;
    auto rejected = nextLookup;
    check(!runtime.copyPublishedSourceOrigins(originCapture, staleSource, staleSequence, rejected) &&
        emptyOrigins(rejected) && !runtime.failed(),
        "reused source cannot mix a prior resident/storage generation or old publication sequence");
  }
  auto mixed = nextPublication;
  mixed.source.storage.generation = publication.source.storage.generation;
  check(!runtime.copyPublishedSourceOrigins(originCapture, mixed, next) && emptyOrigins(next) && !runtime.failed(),
      "mixed reused source/storage generation is rejected without changing retained provenance");
}

void generatedSourceOriginRollbackAndRetry() {
  auto device = std::make_unique<core::Device>(core::AdapterInfo{}, core::BackendLimits{},
      core::PresentParameters{}, 0u);
  RenderTapeRuntimeIdentityV4 runtime;
  check(runtime.begin(key, {.queue = writer.queue, .sessionLaneEnabled = true}), "generated rollback origin root begins");
  constexpr ReplayTransactionRawIdentity raw{key.token, 2u, 1u, 1u};
  ReplayTransaction transaction;
  const std::array locator{RenderTapeRuntimeRecordLocator{2u, 0u}};
  const auto publication = originPublication();
  RenderTapeRuntimePublishedSourceOriginsV4 origins;
  {
    ScopedReplayTransactionObserver scope(raw, runtime.replaySink());
    transaction.begin(projectionIdentity(raw.rawOrdinal), device.get());
    check(runtime.beginRecord(0u, D9C_COMMAND_RECORD_UPDATE_TEXTURE) &&
        transaction.project({.stateGeneration = 7u, .source = 21u, .recordOrdinal = 0u}) &&
        runtime.beginDispatch() && runtime.bindEmission(writer, 0u, core::MetalCommandKind::SurfaceCopy, locator),
        "tentative generated append records its original locator without publication");
    check(transaction.rollback(*device), "pre-effect generated destination remains genuinely rollbackable");
    noteDestinationRollback(transaction, true);
    check(!runtime.copyPublishedSourceOrigins(originCapture, publication, origins) && emptyOrigins(origins) &&
        !runtime.failed(), "confirmed rollback cannot expose discarded generated source origin");
    check(!runtime.copyPublishedSourceOrigins(originCapture, publication.source, publication.metadata.seqId,
        origins) && emptyOrigins(origins) && !runtime.failed(),
        "source/sequence lookup cannot expose a rolled-back unpublished source");
    transaction.begin(projectionIdentity(raw.rawOrdinal), device.get());
    check(runtime.beginRecord(0u, D9C_COMMAND_RECORD_UPDATE_TEXTURE) &&
        transaction.project({.stateGeneration = 7u, .source = 21u, .recordOrdinal = 0u}) &&
        transaction.startIrreversibleEffect(), "real retry receives a new authentic attempt");
    emit(runtime, {.edge = render::TapeRuntimeEdge::OrderedRelease,
        .release = {.event = {.ordinal = 1u, .fenceRawOrdinal = 1u}, .generation = 1u}});
    check(runtime.beginDispatch() &&
        runtime.bindEmission(writer, 0u, core::MetalCommandKind::SurfaceCopy, locator),
        "rollback truncates tentative generated origin together with its physical binding");
    core::ChunkSlot finalSlot{};
    finalSlot.appendSurfaceCopy({});
    emit(runtime, {.edge = render::TapeRuntimeEdge::Publish, .writer = writer, .source = source(),
        .metadata = publication.metadata, .payload = core::SourcePayloadView(finalSlot),
        .controlIndex = publication.controlIndex, .sequence = publication.metadata.seqId});
    check(runtime.copyPublishedSourceOrigins(originCapture, publication, origins) && origins.origins.size() == 1u &&
        origins.origins[0].record == locator[0], "only successful retry's exact canonical generated origin is exposed");
    check(transaction.stage({.commandCount = 1u, .byteCount = 8u}) && runtime.endRecord(0),
        "successful retry completes its real generated coordinator");
    receiveAndCommit(transaction, ReplayDestinationKind::Compatibility);
  }
  check(runtime.acknowledgeRaw(raw, true) && !runtime.failed(), "origin lookup never fabricates retry settlement");
}

void replayLeaseRetirementKeepsFrozenTransportImmutable() {
  auto runtime = std::make_shared<RenderTapeRuntimeIdentityV4>();
  std::weak_ptr<RenderTapeRuntimeIdentityV4> weak = runtime;
  check(runtime->begin(key, {.queue = writer.queue}), "retirement root starts at authentic baseline");
  render::TapeRuntimeObserverEndpoint endpoint;
  check(endpoint.attachRetained(runtime), "replay root attaches to the actual endpoint admission domain");
  auto lease = endpoint.acquireCapture();
  constexpr ReplayTransactionRawIdentity raw{key.token, 2u, 1u, 1u};
  {
    ScopedRenderTapeRuntimeReplayV4 outer(runtime, raw, true, lease);
    auto* access = activeRenderTapeRuntimeReplayV4;
    auto* emission = render::activeTapeEmissionObserver;
    const auto staleReplay = access->replaySink();
    const auto staleEmission = access->emissionSink();
    ReplayTransaction transaction;
    {
      ScopedRenderTapeRuntimeReplayV4 inner(runtime, raw, false, endpoint.acquireCapture());
      check(activeRenderTapeRuntimeReplayV4 == access && render::activeTapeEmissionObserver == emission &&
            activeRenderTapeRuntimeReplayV4->replaySink().context == staleReplay.context,
          "same-root same-raw nesting shares exact observer context and pending original locators");
      transaction.begin(projectionIdentity(raw.rawOrdinal));
      check(access->beginRecord(0u, D9C_COMMAND_RECORD_SET_VS_CONST_F) &&
            transaction.project({.stateGeneration = 7u, .source = transaction.state().identity.source,
                .recordOrdinal = 0u}) && access->beginDispatch() && access->endRecord(0),
          "each original replay record callback receives its own short endpoint admission");
      receiveAndCommit(transaction, ReplayDestinationKind::Compatibility);
    }
    check(!runtime->failed() && outer.acknowledge(true),
        "nested driver closes the actual attempt and separately observes raw ledger ACK");
    check(endpoint.revoke(runtime->sink()) && !lease.active() && runtime->freeze(key, 4u),
        "retirement joins short operations without waiting for retained replay scope lifetime");
    const auto before = RenderTapeRuntimeIdentityV4TestAccess::bindings(*runtime);
    RenderTapeIdentityV4View frozen;
    RenderTapeExecutionTopologyV1View frozenTopology;
    check(runtime->snapshot(key, frozen, frozenTopology) && frozen.attempts.size() == 1u &&
          frozen.settlements.size() == 1u && frozen.dispositions.size() == 1u,
        "detached frozen transfer remains available after endpoint retirement");

    const std::array records{RenderTapeRuntimeRecordLocator{raw.eventOrdinal, 0u}};
    check(!access->beginRecord(0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE) &&
          !access->beginDispatch() && !access->endRecord(-1) && !outer.acknowledge(true) &&
          !staleReplay.observe(staleReplay.context,
              {raw, ReplayTransactionObservationKind::Begin,
               ReplayTransactionEffectWitness::Transaction, &transaction, &transaction.state()}) &&
          !staleEmission.bind(staleEmission.context, writer, 0u, core::MetalCommandKind::DrawRun, records, 0u),
        "retained stale record, transaction, emission and ACK callbacks cannot reopen retired admission");
    staleReplay.failed(staleReplay.context, ReplayTransactionObserverFailure::InvalidObservation);
    staleEmission.fail(staleEmission.context);
    access->fail();
    bool visited = false;
    check(!access->visit([&](RenderTapeRuntimeIdentityV4&) noexcept { visited = true; return true; }) && !visited,
        "stale facade cannot obtain even a borrowed old-root visit after revoke/join");
    auto replacement = std::make_shared<RenderTapeRuntimeIdentityV4>();
    check(replacement->begin(key, {.queue = writer.queue}) && endpoint.attachRetained(replacement),
        "replacement arm attaches while the old replay scope remains retained");
    RenderTapeRuntimeReplayAccessV4 mismatched(runtime.get(), raw, endpoint.acquireCapture());
    check(!mismatched.visit([&](RenderTapeRuntimeIdentityV4&) noexcept { visited = true; return true; }) &&
          !visited && !replacement->failed(),
        "a new-arm lease cannot grant access to a retained old root");
    check(endpoint.revoke(replacement->sink()), "replacement retires independently from stale replay callbacks");
    RenderTapeIdentityV4View after;
    RenderTapeExecutionTopologyV1View afterTopology;
    check(!runtime->failed() && runtime->frozen() && runtime->snapshot(key, after, afterTopology) &&
          after.header.presentOrdinal == frozen.header.presentOrdinal &&
          sameRows(after.sources, frozen.sources) && sameRows(after.initializers, frozen.initializers) &&
          sameRows(after.commands, frozen.commands) && sameRows(after.attempts, frozen.attempts) &&
          sameRows(after.settlements, frozen.settlements) && sameRows(after.dispositions, frozen.dispositions) &&
          sameRows(after.coordinators, frozen.coordinators) &&
          sameRows(afterTopology.plans, frozenTopology.plans) &&
          sameRows(afterTopology.placements, frozenTopology.placements) &&
          RenderTapeRuntimeIdentityV4TestAccess::same(
              before, RenderTapeRuntimeIdentityV4TestAccess::bindings(*runtime)),
        "revoke then stale callbacks leave frozen provenance, transactions and transport unchanged");
  }
  check(!runtime->failed() && runtime->frozen() && !activeRenderTapeRuntimeReplayV4 &&
        !render::activeTapeEmissionObserver,
      "scope exit restores TLS without a late mutation of the retired frozen root");
  runtime.reset();
  check(!weak.expired(), "retired lease retains only old-arm lifetime, not admission");
  lease = {};
  check(weak.expired(), "scope and lease release the root without a root-to-lease ownership cycle");

  auto unstarted = std::make_shared<RenderTapeRuntimeIdentityV4>();
  check(unstarted->begin(key, {.queue = writer.queue}) && endpoint.attachRetained(unstarted),
      "unstarted replay scope attaches for late missing-ACK diagnostic regression");
  {
    ScopedRenderTapeRuntimeReplayV4 scope(unstarted, raw, true, endpoint.acquireCapture());
    check(endpoint.revoke(unstarted->sink()) && unstarted->freeze(key, 4u),
        "an idle scope does not obstruct retirement and detached freeze");
  }
  RenderTapeIdentityV4View unchanged;
  RenderTapeExecutionTopologyV1View unchangedTopology;
  check(!unstarted->failed() && unstarted->snapshot(key, unchanged, unchangedTopology) &&
        unchanged.attempts.empty() && unchanged.settlements.empty(),
      "post-retirement missing-ACK scope destructor cannot poison an already frozen capture");
}
void firstFailureAttributionIsSticky() {
  RenderTapeRuntimeIdentityV4 runtime;
  check(runtime.begin(key, {.queue = writer.queue}) &&
        runtime.setCut(4u, RenderTapeIdentityV3CutKind::AfterEvent),
      "successful arm boundary has no rejection attribution");
  check(!runtime.firstFailure().location.line(),
      "successful cursor updates do not manufacture a failure origin");
  const auto before = runtime.diagnosticState();
  check(before.captureToken == key.token && before.captureEpoch == key.epoch &&
        before.queue == writer.queue && before.eventOrdinal == 4u &&
        before.cutKind == static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent) &&
        before.active && !before.failed && !before.frozen &&
        !before.eventActive && !before.recordActive && !before.location.line(),
      "cold current-state copy describes the actual idle arm cursor");
  check(!runtime.beginRecord(0u, D9C_COMMAND_RECORD_QUERY_ISSUE),
      "record without an authentic active raw fails the production predicate");
  const auto first = runtime.firstFailure();
  check(first.location.line() &&
        std::string_view(first.location.function_name()).find("beginRecord") != std::string_view::npos &&
        first.captureToken == key.token && first.captureEpoch == key.epoch &&
        first.queue == writer.queue && first.eventOrdinal == 4u && !first.rawOrdinal &&
        first.cutKind == before.cutKind && !first.cutRecord && !first.recordIndex &&
        !first.recordType && !first.eventActive && !first.recordActive,
      "first production rejection retains its source and pre-rejection cursor");
  auto& core = *static_cast<RenderTapeRuntimeIdentityCore*>(runtime.sink().context);
  core.fail(); // A later explicit failure must not replace the original cause.
  check(!runtime.setCut(99u, RenderTapeIdentityV3CutKind::BeforeEvent),
      "a revoked root does not fabricate the requested later journal cut");
  const auto again = runtime.firstFailure();
  const auto after = runtime.diagnosticState();
  check(again.location.line() == first.location.line() &&
        again.location.column() == first.location.column() &&
        std::string_view(again.location.file_name()) == first.location.file_name() &&
        std::string_view(again.location.function_name()) == first.location.function_name() &&
        again.captureToken == first.captureToken && again.captureEpoch == first.captureEpoch &&
        again.queue == first.queue && again.eventOrdinal == first.eventOrdinal &&
        again.rawOrdinal == first.rawOrdinal && again.attemptGeneration == first.attemptGeneration &&
        again.cutKind == first.cutKind && again.cutRecord == first.cutRecord &&
        again.recordIndex == first.recordIndex && again.recordType == first.recordType &&
        again.eventActive == first.eventActive && again.recordActive == first.recordActive &&
        after.failed && after.eventOrdinal == before.eventOrdinal && after.cutKind == before.cutKind,
      "later failures and rejected cuts preserve the exact first-error attribution and cursor");
}

void ordinaryCoordinatorNeedsExactPositiveAdmission() {
  constexpr ReplayTransactionRawIdentity raw{key.token, 2u, 1u, 1u};
  constexpr D9CWireObjectIdentity queryIdentity{D9C_CHUNK_HANDLE_KIND_QUERY, 1u, 9u};
  for (std::uint32_t mutation = 0u; mutation < 15u; ++mutation) {
    RenderTapeRuntimeIdentityV4 runtime;
    check(runtime.begin(key, {.queue = writer.queue, .sessionLaneEnabled = mutation == 12u || mutation == 14u}),
        "ordinary counterexample observes the actual arm session domain");
    ReplayTransaction transaction;
    ScopedReplayTransactionObserver scope(raw, runtime.replaySink());
    transaction.begin(projectionIdentity(1u));
    check(runtime.beginRecord(0u, D9C_COMMAND_RECORD_QUERY_ISSUE) &&
          transaction.project({.stateGeneration = 7u, .source = 21u, .recordOrdinal = 0u}) &&
          transaction.startIrreversibleEffect(),
        "ordinary query retains authentic active attempt and irreversible admission");
    const auto current = runtime.diagnosticState();
    render::TapeOrdinaryCoordinatorAdmission admission{
        .queue = current.queue, .captureToken = current.captureToken,
        .captureEpoch = current.captureEpoch, .eventOrdinal = current.eventOrdinal,
        .rawOrdinal = current.rawOrdinal, .attemptGeneration = current.attemptGeneration,
        .recordIndex = current.recordIndex, .recordType = current.recordType};
    if (mutation == 1u) ++admission.queue;
    if (mutation == 2u) ++admission.captureToken;
    if (mutation == 3u) ++admission.captureEpoch;
    if (mutation == 4u) ++admission.eventOrdinal;
    if (mutation == 5u) ++admission.rawOrdinal;
    if (mutation == 6u) ++admission.attemptGeneration;
    if (mutation == 7u) ++admission.recordIndex;
    if (mutation == 8u) admission.recordType = D9C_COMMAND_RECORD_READBACK;
    if (mutation == 11u) check(runtime.beginDispatch(), "late admission test crosses the genuine dispatch cut");
    if (mutation != 9u && mutation != 13u && mutation != 14u)
      emit(runtime, {.edge = render::TapeRuntimeEdge::OrdinaryCoordinatorAdmission,
          .ordinaryAdmission = admission});
    if (mutation == 10u)
      emit(runtime, {.edge = render::TapeRuntimeEdge::OrdinaryCoordinatorAdmission,
          .ordinaryAdmission = admission});
    if (mutation == 13u)
      emit(runtime, {.edge = render::TapeRuntimeEdge::OrderedRelease,
          .release = {.event = {.ordinal = 1u, .fenceRawOrdinal = raw.rawOrdinal}, .generation = 1u}});
    if (mutation && mutation != 9u && mutation != 14u) {
      check(runtime.failed() && !runtime.edges().active(),
          "wrong, duplicate, late, session-expected or mixed admission revokes only capture");
      continue;
    }
    check(runtime.beginDispatch(), "positive admission still precedes the actual query dispatch");
    core::QueryPollSnapshot poll{};
    poll.type = core::QueryType::Occlusion;
    poll.issueOrdinal = 1u; poll.issueDeviceEpoch = 3u; poll.active = true;
    runtime.queryIssue(queryIdentity, 2u, poll, 0);
    if (mutation == 9u || mutation == 14u) {
      check(!runtime.endRecord(0) && runtime.failed(),
          "a successful query with neither authority witness cannot complete its coordinator");
      continue;
    }
    check(runtime.endRecord(0), "truthful ordinary query has no fabricated session receipt");
    receiveAndCommit(transaction, ReplayDestinationKind::Compatibility);
    RenderTapeIdentityV4View view;
    RenderTapeExecutionTopologyV1View topology;
    check(runtime.acknowledgeRaw(raw, true) && runtime.freeze(key, 4u) && runtime.snapshot(key, view, topology),
        "ordinary coordinator closes through actual transaction and independent raw ACK");
    const auto& row = view.coordinators.at(0u);
    const auto& control = row.provenance;
    check(view.header.coordinatorDomain == static_cast<std::uint32_t>(TapeCoordinatorAuthority::OrdinarySerial) &&
          row.authority == view.header.coordinatorDomain && row.admissionEdge &&
          view.attempts[0].effectEdge < row.admissionEdge && row.admissionEdge < control.dispatchEdge &&
          !control.releaseOrdinal && !control.releaseGeneration && !control.releaseAction && !control.releaseWriter,
        "exported ordinary authority retains its real local edge and zero release evidence");
    const std::array events{RenderTapeIdentityV3EventFact{
        2u, static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 1u}};
    const std::array facts{RenderTapeIdentityV3RecordFact{
        .eventOrdinal = 2u, .recordIndex = 0u, .recordType = D9C_COMMAND_RECORD_QUERY_ISSUE,
        .queryFlags = 2u, .queryIdentity = queryIdentity, .queryType = 9u,
        .issueCycle = 1u, .issueOrdinal = 1u}};
    check(validateRenderTapeIdentityV4Rows(view, events, facts) == RenderTapeIdentityV4Status::Valid,
        "production validator admits explicit ordinary BEGIN without source or session release");
    auto missing = view;
    missing.coordinators[0].admissionEdge = 0u;
    check(validateRenderTapeIdentityV4Rows(missing, events, facts) == RenderTapeIdentityV4Status::InvalidCoordinator,
        "pure validation cannot infer ordinary admission from missing release fields");
  }
}
} // namespace

int main() {
  try {
    directWritingAcrossSpansAndRaws();
    confirmedRollbackAndRetry();
    unsuccessfulRollbackFailsCaptureOnly();
    committedWritingPrefixSurvivesTentativeSuffixRollback();
    arenaPublicationRequiresActualLocators();
    sharedQueryLifecycleUsesActualAttempt();
    atomicReplayQueryBaselineAdmission();
    exactCheckpointOnlyPollHasNoCoordinator();
    replayQueryContinuityCrossProduct();
    replayQueryReissueAndModeBoundaries();
    queryBaselineRollbackPreservesOnlyPreEffectHistory();
    exactSourceLocatorAndAttemptBudgetFailures();
    publishedSourceOriginsBeforeCoordinatorFinishes();
    combinedDrawRunAndReusedSourceOrigins();
    generatedSourceOriginRollbackAndRetry();
    replayLeaseRetirementKeepsFrozenTransportImmutable();
    firstFailureAttributionIsSticky();
    ordinaryCoordinatorNeedsExactPositiveAdmission();
    std::cout << "render tape runtime identity v4: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render tape runtime identity v4: " << error.what() << '\n';
    return 1;
  }
}
