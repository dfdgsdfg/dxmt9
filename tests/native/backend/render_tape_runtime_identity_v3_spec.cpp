#include "device_c_render_tape_runtime_identity_v3.hpp"
#include "device_c_replay_offload.hpp"
#include "device_c_replay_projection.hpp"
#include "dxmt9/dxmt9_queue.hpp"

#include <array>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace dxmt9::d3d9 {
struct RenderTapeRuntimeIdentityV3TestAccess {
  static std::array<std::size_t, 8> storage(const RenderTapeRuntimeIdentityV3& runtime) {
    std::lock_guard lock(runtime.mutex_);
    return {runtime.sources_.size(), runtime.sources_.capacity(),
        runtime.commands_.size(), runtime.commands_.capacity(),
        runtime.writers_.size(), runtime.writers_.capacity(),
        runtime.generated_.size(), runtime.generated_.capacity()};
  }
};
} // namespace dxmt9::d3d9

namespace {
using namespace dxmt9;
using namespace dxmt9::d3d9;
constexpr RenderTapeIdentityV3CaptureKey key{5u, 6u};
constexpr query::WriterIdentity writer{1u, 1u, 2u, 3u};
std::uint64_t checks = 0u;
void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
core::CpuReadyTape::SourceRef source() {
  core::CpuReadyTape::SourceRef value{};
  value.id.index = writer.index;
  value.id.generation = writer.generation;
  value.storage.generation = writer.storageGeneration;
  return value;
}
void emit(RenderTapeRuntimeIdentityV3& runtime, render::TapeRuntimeObservation value) {
  const auto sink = runtime.sink();
  sink.fn(sink.context, value);
}
void record(RenderTapeRuntimeIdentityV3& runtime, std::uint32_t index,
    std::uint32_t type) {
  check(runtime.beginRecord(index, type), "record admitted once");
  check(runtime.beginDispatch(), "original dispatch position admitted");
  check(runtime.endRecord(0), "actual successful dispatch settles record");
}

ReplayTransactionState committedTransaction(std::uint64_t raw, std::uint32_t count) {
  ReplayTransaction transaction;
  transaction.begin({.source = 1u, .lastSource = 1u, .sequence = raw});
  for (std::uint32_t index = 0u; index < count; ++index)
    check(transaction.project({.stateGeneration = 7u, .source = 1u, .recordOrdinal = index}),
        "production replay transaction projects original complete cursor");
  check(transaction.receiveDestination({.identity = transaction.state().identity,
      .queueSequence = raw}), "production transaction receives valid compatibility destination");
  check(transaction.commit(), "production transaction actually commits");
  return transaction.state();
}

void settle(RenderTapeRuntimeIdentityV3& runtime, std::uint64_t raw, std::uint32_t count) {
  check(runtime.transactionCommitted(committedTransaction(raw, count)),
      "observer accepts actual committed transaction before ledger settlement");
  ReplayDrainLedger ledger;
  RawCommandChunk chunk;
  for (std::uint64_t index = 0u; index < raw; ++index)
    check(ledger.publishInline(chunk), "production ledger accepts fixture FIFO entry");
  check(chunk.replaySeq == raw, "ledger binds actual admitted raw ordinal");
  ledger.publishReplayed(chunk);
  check(runtime.settleReplayAttempt(raw, !ledger.terminal()),
      "actual successful replay ledger boundary settles exactly once");
}

void coalescedAndCrossEventWriter() {
  RenderTapeRuntimeIdentityV3 runtime;
  check(runtime.begin(key, {.queue = 1u}), "explicit empty arm baseline");
  check(runtime.beginReplayAttempt(key.token, 2u, 1u, 3u), "first actual transaction attempt admitted");
  record(runtime, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  record(runtime, 1u, D9C_COMMAND_RECORD_SET_VS_CONST_F);
  record(runtime, 2u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  core::ChunkSlot slot{};
  std::array<core::DrawParam, 2> draws{};
  draws[0].primitiveCount = draws[1].primitiveCount = 1u;
  std::array<core::DrawParamPayloadView, 2> payloads{};
  check(slot.appendDrawRun({}, {}, draws, payloads), "real physical DrawRun built");
  emit(runtime, {.edge = render::TapeRuntimeEdge::Append, .writer = writer,
      .source = source(), .payload = core::SourcePayloadView(slot),
      .commandIndex = 0u, .subdrawCount = 2u});
  settle(runtime, 1u, 3u);
  RenderTapeIdentityV3View pending;
  check(runtime.snapshot(key, pending, false) && pending.sources.empty(),
      "replay settlement never fabricates a source");
  check(runtime.beginReplayAttempt(key.token, 3u, 2u, 1u), "cross-event writer retained");
  record(runtime, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  check(slot.appendDrawRun({}, {}, std::span(draws).first(1),
      std::span(payloads).first(1)), "second actual command appended");
  emit(runtime, {.edge = render::TapeRuntimeEdge::Append, .writer = writer,
      .source = source(), .payload = core::SourcePayloadView(slot),
      .commandIndex = 1u, .subdrawCount = 1u});
  settle(runtime, 2u, 1u);
  emit(runtime, {.edge = render::TapeRuntimeEdge::Publish, .writer = writer,
      .source = source(), .metadata = {.sourceOrdinal = 7u, .seqId = 10u, .pageCount = 1u},
      .payload = core::SourcePayloadView(slot), .sequence = 10u});
  check(!runtime.failed(), "real publication binds physical coverage");
  check(!runtime.snapshot(key, pending), "publication is not GPU completion");
  emit(runtime, {.edge = render::TapeRuntimeEdge::Submit, .source = source(), .sequence = 10u});
  check(!runtime.snapshot(key, pending), "submission is not GPU completion");
  emit(runtime, {.edge = render::TapeRuntimeEdge::Complete, .source = source(),
      .sequence = 10u, .completedSequence = 10u});
  check(runtime.freeze(key, 4u), "drained capture freezes final Present identity");
  check(runtime.snapshot(key, pending), "complete value-owned snapshot");
  check(pending.sources.size() == 1u && pending.commands.size() == 2u &&
      pending.dispositions.size() == 4u && pending.settlements.size() == 2u,
      "one actual source covers two canonical events");
  check(pending.settlements[0].replayEdge == 2u && pending.settlements[1].replayEdge == 3u &&
      pending.sources[0].publicationEdge == 4u && pending.sources[0].submissionEdge == 5u &&
      pending.sources[0].completionEdge == 6u,
      "neutral-core extraction preserves historical V3 edge allocation bytes");
  check(pending.commands[0].drawCount == 2u &&
      pending.dispositions[0].commandIndex == pending.dispositions[2].commandIndex &&
      pending.dispositions[0].firstSubdraw == 0u && pending.dispositions[2].firstSubdraw == 1u,
      "nonadjacent draws own disjoint slices of one physical DrawRun");
  const std::array<RenderTapeIdentityV3EventFact, 2> events{{
      {2u, static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 3u},
      {3u, static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 1u}}};
  const std::array<RenderTapeIdentityV3RecordFact, 4> facts{{
      {2u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE},
      {2u, 1u, D9C_COMMAND_RECORD_SET_VS_CONST_F},
      {2u, 2u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE},
      {3u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE}}};
  check(validateRenderTapeIdentityV3Rows(pending, events, facts) == RenderTapeIdentityV3Status::Valid,
      "runtime tables satisfy production v3 refinement predicates");
  RenderTapeIdentityV3View repeated;
  check(runtime.freeze(key, 4u) && runtime.snapshot(key, repeated) &&
      repeated.sources[0].completionEdge == pending.sources[0].completionEdge,
      "size/copy repetition retains exact frozen completion evidence");
}

void liveQueryObservationBeforePresentBinding() {
  RenderTapeRuntimeIdentityV3 runtime;
  check(runtime.begin(key, {.queue = 1u}), "live query capture begins at completed baseline");
  check(runtime.beginReplayAttempt(key.token, 2u, 1u, 2u), "query transaction attempt begins");
  record(runtime, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  core::ChunkSlot slot{};
  core::DrawParam draw{};
  draw.primitiveCount = 1u;
  const core::DrawParamPayloadView payload{};
  check(slot.appendDrawRun({}, {}, std::span(&draw, 1u), std::span(&payload, 1u)),
      "query prefix uses real physical DrawRun");
  emit(runtime, {.edge = render::TapeRuntimeEdge::Append, .writer = writer,
      .source = source(), .payload = core::SourcePayloadView(slot), .subdrawCount = 1u});
  check(runtime.beginRecord(1u, D9C_COMMAND_RECORD_QUERY_ISSUE), "END starts original coordinator record");
  emit(runtime, {.edge = render::TapeRuntimeEdge::Publish, .writer = writer,
      .source = source(), .metadata = {.sourceOrdinal = 7u, .seqId = 10u, .pageCount = 1u},
      .payload = core::SourcePayloadView(slot), .sequence = 10u});
  emit(runtime, {.edge = render::TapeRuntimeEdge::Submit, .source = source(), .sequence = 10u});
  emit(runtime, {.edge = render::TapeRuntimeEdge::InitializerPublish, .sequence = 2u});
  emit(runtime, {.edge = render::TapeRuntimeEdge::InitializerSubmit, .sequence = 2u});
  emit(runtime, {.edge = render::TapeRuntimeEdge::OrderedRelease,
      .release = {.event = {.ordinal = 1u, .fenceRawOrdinal = 1u, .fenceSeqId = 10u},
          .generation = 1u}});
  check(runtime.beginDispatch(), "END dispatch follows actual ordered release");
  emit(runtime, {.edge = render::TapeRuntimeEdge::QueryEnd, .sequence = 10u,
      .initializer = 2u, .commandIndex = 1u});
  constexpr D9CWireObjectIdentity queryIdentity{D9C_CHUNK_HANDLE_KIND_QUERY, 1u, 9u};
  core::QueryPollSnapshot poll{};
  poll.type = core::QueryType::Event;
  poll.issueOrdinal = 1u;
  poll.issueDeviceEpoch = 3u;
  poll.backendFence = query::EndReceipt(1u, 10u, 2u);
  runtime.queryIssue(queryIdentity, 1u, poll, 0);
  check(runtime.endRecord(0), "successful original END finishes its record");
  settle(runtime, 1u, 2u);
  runtime.queryPoll(queryIdentity, poll, 1);
  RenderTapeIdentityV3View view;
  check(runtime.snapshot(key, view, false) && !view.header.presentOrdinal &&
      view.coordinators[0].observation == static_cast<std::uint32_t>(RenderTapeIdentityV3Observation::Pending),
      "actual Pending observation is retained before final Present exists");
  const RenderTapeIdentityV3CurrentIssue current{key, queryIdentity, 1u, 1u, 3u};
  const auto admitted = [&](const RenderTapeIdentityV3Header& context,
      RenderTapeIdentityV3Observation observation) {
    return renderTapeIdentityV3RuntimeObservationAdmitted(context, view.coordinators[0],
        current, view.sources, view.initializers, observation);
  };
  check(admitted(view.header, RenderTapeIdentityV3Observation::Pending) &&
      !admitted(view.header, RenderTapeIdentityV3Observation::Ready) &&
      !renderTapeIdentityV3ObservationAdmitted(view.header, view.coordinators[0], current,
          view.sources, view.initializers, RenderTapeIdentityV3Observation::Pending),
      "live observation defers only Present binding, never completion or final-header admission");
  for (std::uint32_t missing = 0u; missing != 6u; ++missing) {
    auto context = view.header;
    switch (missing) {
    case 0u: context.captureToken = 0u; break;
    case 1u: context.captureEpoch = 0u; break;
    case 2u: context.queue = 0u; break;
    case 3u: context.baselineCompletionEdge = 0u; break;
    case 4u: context.initializerEpoch = 0u; break;
    case 5u: context.baselineSequence = 1u; break;
    }
    check(!admitted(context, RenderTapeIdentityV3Observation::Pending),
        "live runtime context retains capture, queue and successful-baseline authentication");
  }
  emit(runtime, {.edge = render::TapeRuntimeEdge::Complete, .source = source(),
      .sequence = 10u, .completedSequence = 10u});
  check(runtime.snapshot(key, view, false) &&
      !admitted(view.header, RenderTapeIdentityV3Observation::Ready),
      "render completion alone cannot qualify a still-pending initializer prefix");
  emit(runtime, {.edge = render::TapeRuntimeEdge::InitializerComplete,
      .sequence = 2u, .completedSequence = 2u});
  check(runtime.snapshot(key, view, false) && !view.header.presentOrdinal &&
      admitted(view.header, RenderTapeIdentityV3Observation::Ready),
      "independent actual render and initializer completion admit Ready before Present");
  runtime.queryPoll(queryIdentity, poll, 0);
  check(runtime.snapshot(key, view, false) &&
      view.coordinators[0].observation == static_cast<std::uint32_t>(RenderTapeIdentityV3Observation::Completed),
      "actual successful GetData observation is retained before freeze");
  check(runtime.freeze(key, 4u) && runtime.snapshot(key, view) && view.header.presentOrdinal == 4u,
      "only final capture freeze binds real Present ordinal");
  const std::array events{RenderTapeIdentityV3EventFact{
      2u, static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 2u}};
  const std::array facts{
      RenderTapeIdentityV3RecordFact{2u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE},
      RenderTapeIdentityV3RecordFact{.eventOrdinal = 2u, .recordIndex = 1u,
          .recordType = D9C_COMMAND_RECORD_QUERY_ISSUE, .queryFlags = 1u,
          .queryIdentity = queryIdentity, .queryType = 8u, .issueCycle = 1u, .issueOrdinal = 1u}};
  check(validateRenderTapeIdentityV3Rows(view, events, facts) == RenderTapeIdentityV3Status::Valid,
      "final bound identity validates under unchanged complete-header production policy");
}

void stickyFailure() {
  for (std::uint32_t limit = 1u; limit != 4u; ++limit) {
    RenderTapeRuntimeIdentityV3 runtime;
    RenderTapeIdentityV3Limits limits{};
    limits.maxDispositions = limit;
    check(runtime.begin(key, {.queue = 1u}, limits), "bounded owner admitted");
    check(runtime.beginReplayAttempt(key.token, 2u, 1u, limit + 1u), "bounded trace started");
    for (std::uint32_t index = 0u; index < limit; ++index)
      record(runtime, index, D9C_COMMAND_RECORD_SET_VS_CONST_F);
    check(!runtime.beginRecord(limit, D9C_COMMAND_RECORD_SET_VS_CONST_F),
        "capacity fails before later dispatch");
    check(runtime.failed() && !runtime.settleReplayAttempt(1u, true),
        "failure cannot synthesize settlement");
    RenderTapeIdentityV3View view;
    check(!runtime.snapshot(key, view, false) && view.sources.empty(), "sticky failure exports no identity");
  }
}

void transactionAndLedgerAuthority() {
  ReplayDrainLedger ledger;
  RawCommandChunk chunk;
  check(ledger.publishInline(chunk), "earlier noncapture raw admitted to real FIFO ledger");
  ledger.publishReplayed(chunk);
  RenderTapeRuntimeIdentityV3 runtime;
  check(runtime.begin(key, {.queue = 1u}), "attempt capture admitted");
  for (std::uint64_t event = 2u; event != 4u; ++event) {
    check(ledger.publishInline(chunk), "captured raw admitted to real FIFO ledger");
    check(runtime.beginReplayAttempt(key.token, event, chunk.replaySeq, 1u),
        "actual transaction begin allocates capture-qualified attempt generation");
    record(runtime, 0u, D9C_COMMAND_RECORD_SET_VS_CONST_F);
    RenderTapeIdentityV3View view;
    check(runtime.snapshot(key, view, false) && view.settlements.size() == event - 2u,
        "walker end alone never produces replay settlement");
    check(runtime.transactionCommitted(committedTransaction(chunk.replaySeq, 1u)),
        "actual production commit is authenticated separately");
    check(runtime.snapshot(key, view, false) && view.settlements.size() == event - 2u,
        "transaction commit alone never invents successful ledger completion");
    ledger.publishReplayed(chunk);
    check(runtime.settleReplayAttempt(chunk.replaySeq, !ledger.terminal()),
        "real ledger publication completes capture attempt");
  }
  RenderTapeIdentityV3View view;
  check(runtime.freeze(key, 4u) && runtime.snapshot(key, view) &&
      view.settlements[0].rawOrdinal == 2u && view.settlements[0].buildGeneration == 1u &&
      view.settlements[1].rawOrdinal == 3u && view.settlements[1].buildGeneration == 2u,
      "replay attempt generation is independent of FIFO raw and source-build identities");

  for (std::uint32_t failure = 0u; failure != 3u; ++failure) {
    RenderTapeRuntimeIdentityV3 failedRuntime;
    check(failedRuntime.begin(key, {.queue = 1u}) &&
        failedRuntime.beginReplayAttempt(key.token, 2u, 1u, 1u), "failure fixture begins actual attempt");
    record(failedRuntime, 0u, D9C_COMMAND_RECORD_SET_VS_CONST_F);
    if (failure == 0u) {
      ReplayTransaction transaction;
      transaction.begin({.source = 1u, .lastSource = 1u, .sequence = 1u});
      check(transaction.project({.stateGeneration = 1u, .source = 1u}), "failed commit fixture projects");
      check(!transaction.commit(), "production commit rejects missing destination receipt");
      check(!failedRuntime.transactionCommitted(transaction.state()),
          "failed real transaction cannot authenticate a capture commit");
    } else if (failure == 1u) {
      check(failedRuntime.transactionCommitted(committedTransaction(1u, 1u)), "ledger failure follows real commit");
      ReplayDrainLedger poisonedLedger;
      RawCommandChunk raw;
      check(poisonedLedger.publishInline(raw), "ledger failure raw admitted");
      poisonedLedger.poison();
      poisonedLedger.publishReplayed(raw);
      check(!failedRuntime.settleReplayAttempt(raw.replaySeq, !poisonedLedger.terminal()),
          "poisoned ledger cannot produce successful settlement even after real transaction commit");
    } else {
      check(!failedRuntime.settleReplayAttempt(1u, true),
          "ledger success cannot substitute for missing real transaction commit");
    }
    check(failedRuntime.failed() && !failedRuntime.snapshot(key, view, false) && view.settlements.empty(),
        "failed commit or ledger exports no invented successful settlement");
  }
}

void initializerCompletionRefreshPredicates() {
  for (bool completed : {false, true})
    for (bool errored : {false, true})
      check(render::tapeRuntimeUploadCompleted(completed, errored) == (completed && !errored),
          "only successful actual command-buffer status emits upload completion");
  for (bool pending : {false, true})
    for (bool failed : {false, true})
      for (std::size_t retained : {0u, 1u, 64u})
        check(render::tapeRuntimeInitializerCompletionAdmitted(pending, failed, retained) ==
            (!pending && !failed && !retained),
            "finish refresh rejects unpublished uploads, failed CBs and incomplete retained FIFO");
  RenderTapeRuntimeIdentityV3 owner;
  RenderTapeRuntimeIdentityV3 other;
  check(render::tapeRuntimeObserverMatches(owner.sink(), owner.sink()) &&
      !render::tapeRuntimeObserverMatches(owner.sink(), other.sink()) &&
      !render::tapeRuntimeObserverMatches(owner.sink(), {}),
      "finish refresh requires exact installed capture owner, not a compatible callback type");
}

void boundedPublicationAndGeneratedStorage() {
  RenderTapeIdentityV3Limits limits{};
  limits.maxCommands = 1u;
  RenderTapeRuntimeIdentityV3 runtime;
  check(runtime.begin(key, {.queue = 1u}, limits) &&
      runtime.beginReplayAttempt(key.token, 2u, 1u, 2u), "small physical command budget admitted");
  record(runtime, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  record(runtime, 1u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  core::ChunkSlot slot{};
  core::DrawParam draw{};
  draw.primitiveCount = 1u;
  const core::DrawParamPayloadView payload{};
  for (std::uint32_t index = 0u; index != 2u; ++index) {
    check(slot.appendDrawRun({}, {}, std::span(&draw, 1u), std::span(&payload, 1u)),
        "real over-budget source command appended");
    emit(runtime, {.edge = render::TapeRuntimeEdge::Append, .writer = writer,
        .source = source(), .payload = core::SourcePayloadView(slot),
        .commandIndex = index, .subdrawCount = 1u});
  }
  const auto before = RenderTapeRuntimeIdentityV3TestAccess::storage(runtime);
  emit(runtime, {.edge = render::TapeRuntimeEdge::Publish, .writer = writer,
      .source = source(), .metadata = {.sourceOrdinal = 7u, .seqId = 10u, .pageCount = 1u},
      .payload = core::SourcePayloadView(slot), .sequence = 10u});
  check(runtime.failed() && RenderTapeRuntimeIdentityV3TestAccess::storage(runtime) == before,
      "over-budget publication rejects before source/owner mutation or storage growth");

  RenderTapeRuntimeIdentityV3 generated;
  check(generated.begin(key, {.queue = 1u}, limits) &&
      generated.beginReplayAttempt(key.token, 2u, 1u, 1u) &&
      generated.beginRecord(0u, D9C_COMMAND_RECORD_UPDATE_TEXTURE) && generated.beginDispatch(),
      "generated-command budget fixture starts actual coordinator dispatch");
  core::ChunkSlot copies{};
  copies.appendSurfaceCopy({});
  copies.appendSurfaceCopy({});
  emit(generated, {.edge = render::TapeRuntimeEdge::Append, .writer = writer,
      .source = source(), .payload = core::SourcePayloadView(copies), .commandIndex = 0u});
  check(!generated.failed(), "one actual generated command fits exact budget");
  const auto generatedBefore = RenderTapeRuntimeIdentityV3TestAccess::storage(generated);
  emit(generated, {.edge = render::TapeRuntimeEdge::Append, .writer = writer,
      .source = source(), .payload = core::SourcePayloadView(copies), .commandIndex = 1u});
  check(generated.failed() && RenderTapeRuntimeIdentityV3TestAccess::storage(generated) == generatedBefore,
      "over-budget generated metadata rejects before vector reallocation or growth");

  RenderTapeRuntimeIdentityV3 oversized;
  const auto empty = RenderTapeRuntimeIdentityV3TestAccess::storage(oversized);
  limits.maxCommands = kRenderTapeIdentityV3MaxRows + 1u;
  check(!oversized.begin(key, {.queue = 1u}, limits) &&
      RenderTapeRuntimeIdentityV3TestAccess::storage(oversized) == empty,
      "runtime shares final-ledger bounded limit admission before any reserve");
}

struct AppendExtents {
  RenderTapeRuntimeIdentityV3* runtime = nullptr;
  std::array<std::array<std::uint32_t, 3>, 4> rows{};
  std::size_t count = 0u;
  static void observe(void* context, const render::TapeRuntimeObservation& value) noexcept {
    auto& owner = *static_cast<AppendExtents*>(context);
    if (value.edge == render::TapeRuntimeEdge::Append) {
      if (owner.count == owner.rows.size()) { owner.runtime->fail(); return; }
      owner.rows[owner.count++] = {value.commandIndex, value.firstSubdraw, value.subdrawCount};
    }
    const auto sink = owner.runtime->sink();
    sink.fn(sink.context, value);
  }
};

void actualMutationNewRunAndMergedTail() {
  RenderTapeRuntimeIdentityV3 runtime;
  check(runtime.begin(key, {.queue = 1u}) &&
      runtime.beginReplayAttempt(key.token, 2u, 1u, 5u), "actual mutation observer capture begins");
  AppendExtents extents{.runtime = &runtime};
  render::TapeRuntimeObserverEndpoint endpoint;
  const render::TapeRuntimeObserverSink sink{&extents, AppendExtents::observe};
  check(endpoint.attach(sink), "actual mutation observer endpoint attached");
  core::ChunkSlot slot{};
  std::array<core::ChunkSlotControl, 1> controls{};
  controls[0].sourceId = source().id;
  controls[0].storage = source().storage;
  controls[0].payload = &slot;
  std::optional<std::size_t> writing = 0u;
  core::metalqueue::QueueLifecycleController controller;
  controller.bindTrackedSubmissionState({.writingSlot = &writing,
      .queryQueueIdentity = 1u, .slots = controls, .tapeRuntimeObserver = &endpoint});
  const core::CanonicalDrawState state{};
  const core::DrawUniformPayload uniforms{};
  core::DrawParam draw{};
  draw.primitiveCount = 1u;
  const core::DrawParamPayloadView payload{};
  record(runtime, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  check(controller.mutateWritingPayload(slot, [&] {
    return slot.appendDrawRun(state, uniforms, std::span(&draw, 1u), std::span(&payload, 1u));
  }), "real new DrawRun mutation succeeds");
  record(runtime, 1u, D9C_COMMAND_RECORD_SET_VS_CONST_F);
  record(runtime, 2u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  check(controller.mutateWritingPayload(slot, [&] {
    return slot.appendCompatibleDrawRunEntry(state, uniforms,
        std::span(&draw, 1u), std::span(&payload, 1u));
  }), "real compatible physical tail extension succeeds");
  check(runtime.beginRecord(3u, D9C_COMMAND_RECORD_COLOR_FILL) && runtime.beginDispatch(),
      "real command barrier dispatch begins");
  check(controller.mutateWritingPayload(slot, [&] {
    slot.appendColorFill({});
    return true;
  }) && runtime.endRecord(0), "physical barrier creates separate command");
  record(runtime, 4u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  check(!controller.mutateWritingPayload(slot, [&] {
    return slot.appendCompatibleDrawRunEntry(state, uniforms,
        std::span(&draw, 1u), std::span(&payload, 1u));
  }), "actual compatibility refuses merging across physical command barrier");
  check(controller.mutateWritingPayload(slot, [&] {
    return slot.appendDrawRun(state, uniforms, std::span(&draw, 1u), std::span(&payload, 1u));
  }), "post-barrier draw writes real new physical run");
  const std::array<std::array<std::uint32_t, 3>, 4> expected{{
      {0u, 0u, 1u}, {0u, 1u, 1u}, {1u, 0u, 0u}, {2u, 0u, 1u}}};
  check(extents.count == 4u && extents.rows == expected && slot.commandCount() == 3u &&
      !runtime.failed(), "generic observer emits each actual new/merged draw extent once without raw-count inference");
  settle(runtime, 1u, 5u);
  RenderTapeIdentityV3View view;
  check(runtime.snapshot(key, view, false) && view.sources.empty() &&
      view.dispositions[0].firstSubdraw == 0u && view.dispositions[2].firstSubdraw == 1u,
      "actual physical tail delta binds nonadjacent canonical draw slices without fabricating publication");
  endpoint.revoke(sink);
}

struct BlockingSink {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  bool release = false;
  std::uint32_t calls = 0u;
  static void observe(void* context, const render::TapeRuntimeObservation&) noexcept {
    auto& value = *static_cast<BlockingSink*>(context);
    std::unique_lock lock(value.mutex);
    ++value.calls;
    value.entered = true;
    value.cv.notify_all();
    value.cv.wait(lock, [&] { return value.release; });
  }
};

void detachJoinsAndStaleProxyIsInert() {
  render::TapeRuntimeObserverEndpoint endpoint;
  BlockingSink owner;
  const render::TapeRuntimeObserverSink sink{&owner, BlockingSink::observe};
  check(endpoint.attach(sink), "queue-owned endpoint installed");
  const auto staleProxy = endpoint.proxy();
  std::thread callback([&] { staleProxy.fn(staleProxy.context, {}); });
  {
    std::unique_lock lock(owner.mutex);
    owner.cv.wait(lock, [&] { return owner.entered; });
  }
  bool detached = false;
  std::thread detach([&] { detached = endpoint.detach(sink); });
  {
    std::lock_guard lock(owner.mutex);
    owner.release = true;
  }
  owner.cv.notify_all();
  callback.join();
  detach.join();
  check(detached && !endpoint.enabled() && owner.calls == 1u,
      "detach joins actual in-flight callback before owner destruction");
  staleProxy.fn(staleProxy.context, {});
  check(owner.calls == 1u, "copied callback cannot reach detached owner");
  BlockingSink replacement;
  replacement.release = true;
  const render::TapeRuntimeObserverSink second{&replacement, BlockingSink::observe};
  check(endpoint.attach(second), "later capture incarnation installed");
  staleProxy.fn(staleProxy.context, {});
  check(replacement.calls == 0u && owner.calls == 1u,
      "old generation cannot target new capture owner");
  endpoint.revoke(second);
  check(!endpoint.enabled(), "device teardown revokes all owner callbacks");
}
} // namespace

int main() {
  try {
    coalescedAndCrossEventWriter();
    liveQueryObservationBeforePresentBinding();
    stickyFailure();
    transactionAndLedgerAuthority();
    initializerCompletionRefreshPredicates();
    boundedPublicationAndGeneratedStorage();
    actualMutationNewRunAndMergedTail();
    detachJoinsAndStaleProxyIsInert();
    std::cout << "render-tape-runtime-identity-v3 checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render-tape-runtime-identity-v3: " << error.what() << '\n';
    return 1;
  }
}
