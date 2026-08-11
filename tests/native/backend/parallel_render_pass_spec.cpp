#include "../../../src/dxmt9/dxmt9_parallel_render_pass.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using dxmt9::encoders::EncodePartitionRangeSnapshot;
using dxmt9::encoders::ParallelFirstDrawSnapshot;
using dxmt9::encoders::ParallelPassChildPlan;
using dxmt9::encoders::ParallelPassExecutionStatus;
using dxmt9::encoders::ParallelPassFallbackReason;
using dxmt9::encoders::ParallelPassFailurePhase;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

template <typename T>
void check(const T& condition, std::string_view message) {
  if (!static_cast<bool>(condition)) {
    throw TestFailure(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    throw TestFailure(std::string(message));
  }
}

dxmt9::core::CpuReadyTape::SourceRef sourceRef(std::uint64_t seqId) {
  return {
      .id = {.index = 3u, .generation = seqId},
      .storage = {.firstPage = 7u, .pageCount = 1u, .generation = seqId},
  };
}

struct ProductionPlanFixture {
  dxmt9::core::ChunkSlot slot{};
  dxmt9::encoders::EncodePartitionReplayStream stream{};
  dxmt9::encoders::ProductionEncodePartitionPlanStorage production{};
  std::array<ParallelFirstDrawSnapshot, 3> snapshots{};

  ProductionPlanFixture() {
    slot.seqId = 401u;
    std::vector<dxmt9::core::DrawParam> draws(96u);
    std::vector<dxmt9::core::DrawParamPayloadView> payloads(draws.size());
    slot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                       dxmt9::core::DrawUniformPayload{}, draws, payloads);
    stream = dxmt9::encoders::makeEncodePartitionReplayStream(
        3u, slot, 0u, slot.commandCount(), false, {}, {},
        sourceRef(slot.seqId));
    const auto result = dxmt9::encoders::planProductionEncodePartitions(
        stream, production);
    check(result.explicitPlan && production.count == 3u,
          "production planner creates three bounded child ranges");
    check(dxmt9::encoders::validateEncodePartitionRanges(production.view(),
                                                          stream),
          "parallel fixture consumes only the validated production plan");
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
      snapshots[i] = ParallelFirstDrawSnapshot{
          .provenance = production.ranges[i].entry,
          .entryRender = dxmt9::core::RenderContinuationKey{
              .flags = dxmt9::core::RenderContinuationKeyValid |
                       dxmt9::core::RenderContinuationEntryStateComplete,
          },
          .generation = static_cast<std::uint64_t>(i + 1u),
          .complete = true,
      };
    }
  }

  dxmt9::encoders::ParallelPassEligibilityInput input() const {
    return {
        .ranges = production.view(),
        .firstDrawSnapshots = snapshots,
        .explicitPlan = true,
        .planValidated = true,
        .logicalPassSealed = true,
    };
  }
};

void eligibilityAndSelectionAreTypedAndBounded() {
  ProductionPlanFixture fixture;
  dxmt9::encoders::ParallelPassPlanStorage storage{};
  const auto eligible = dxmt9::encoders::planParallelRenderPassChildren(
      fixture.input(), storage);
  check(eligible.considered && eligible.eligible && eligible.childCount == 3u &&
            eligible.fallback == ParallelPassFallbackReason::None,
        "sealed validated draw-only plan is eligible");
  checkEq(storage.count, std::size_t{3},
          "eligible planning stays within fixed child storage");

  const auto unavailable = dxmt9::encoders::decideParallelPassExecution(
      true, eligible, false);
  check(unavailable.considered && unavailable.eligible &&
            !unavailable.selected &&
            unavailable.fallback ==
                ParallelPassFallbackReason::ParallelEncoderUnavailable,
        "missing real WMT implementation selects typed serial fallback");
  const auto selected = dxmt9::encoders::decideParallelPassExecution(
      true, eligible, true);
  check(selected.selected && selected.fallback ==
                                 ParallelPassFallbackReason::None,
        "a capable fake can select the pure executor surface");
  const auto notRequested = dxmt9::encoders::decideParallelPassExecution(
      false, eligible, true);
  check(!notRequested.considered && !notRequested.selected &&
            notRequested.fallback == ParallelPassFallbackReason::NotRequested,
        "serial provider does not create parallel-pass work");

  auto input = fixture.input();
  input.logicalPassSealed = false;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::PassNotSealed,
        "unsealed source-fragment ownership fails before child work");
  input = fixture.input();
  input.hasQuery = true;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::Query,
        "query work is coordinator-serial");
  input = fixture.input();
  input.hasClear = true;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::Clear,
        "clear work is coordinator-serial");
  input = fixture.input();
  input.hasSidecarObservation = true;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::SidecarObservation,
        "sidecar observation blocks children");
  input = fixture.input();
  input.hasInitializerWait = true;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::InitializerWait,
        "initializer wait blocks children");
  input = fixture.input();
  input.hasPresent = true;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::Present,
        "present work blocks children");
  input = fixture.input();
  input.hasUnresolvedHazard = true;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::UnresolvedHazard,
        "unresolved hazards block children");
  input = fixture.input();
  auto missingSnapshots = fixture.snapshots;
  missingSnapshots[1].complete = false;
  input.firstDrawSnapshots = missingSnapshots;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::FirstDrawSnapshotMissing,
        "every child requires a complete first-draw snapshot");

  auto commandRanges = fixture.production.ranges;
  commandRanges[1].kind =
      dxmt9::encoders::EncodePartitionRangeKind::CommandSegment;
  input = fixture.input();
  input.ranges = std::span<const EncodePartitionRangeSnapshot>(
      commandRanges.data(), fixture.production.count);
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::CoordinatorCommand,
        "a coordinator command cannot become a child range");

  std::array<EncodePartitionRangeSnapshot,
             dxmt9::encoders::kParallelRenderPassChildCapacity + 1u>
      oversizedRanges{};
  std::array<ParallelFirstDrawSnapshot,
             dxmt9::encoders::kParallelRenderPassChildCapacity + 1u>
      oversizedSnapshots{};
  input = fixture.input();
  input.ranges = oversizedRanges;
  input.firstDrawSnapshots = oversizedSnapshots;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::ChildCapacity,
        "child planning fails closed above fixed storage capacity");
}

void productionShadowSealsOnlyCompleteOwnedPasses() {
  ProductionPlanFixture fixture;
  dxmt9::encoders::SealedParallelPassSnapshot snapshot{};
  auto input = dxmt9::encoders::SealedParallelPassSnapshotInput{
      .stream = &fixture.stream,
      .ranges = fixture.production.view(),
      .planValidated = true,
      .sourceStartsPass = true,
      .sourceEndsPass = true,
  };
  const auto result =
      dxmt9::encoders::produceSealedParallelPassSnapshot(input, snapshot);
  check(result.considered && result.sealed && result.eligible &&
            result.childCount == 3u && result.drawCount == 96u &&
            result.fallback ==
                dxmt9::encoders::SealedParallelPassSnapshotFallback::None,
        std::string(
            "fresh complete source-local pass produces one sealed shadow: ") +
            std::to_string(static_cast<unsigned>(result.fallback)));
  check(snapshot.source == fixture.stream.source.source &&
            snapshot.seqId == fixture.stream.source.seqId &&
            snapshot.childCount == 3u && snapshot.drawCount == 96u &&
            !snapshot.hasTerminalPresent,
        "sealed shadow owns source identity and bounded pass dimensions");
  for (std::size_t i = 0; i < snapshot.childCount; ++i) {
    check(snapshot.ranges[i] == fixture.production.ranges[i] &&
              snapshot.firstDraws[i].provenance ==
                  fixture.production.ranges[i].entry &&
              snapshot.firstDraws[i].entryRender.valid() &&
              snapshot.firstDraws[i].entryRender.entryStateComplete() &&
              snapshot.firstDraws[i].complete,
          "every child owns locator and first-draw proof values");
  }
  check(dxmt9::encoders::classifyParallelPassEligibility({
            .ranges = snapshot.rangeView(),
            .firstDrawSnapshots = snapshot.firstDrawView(),
            .explicitPlan = true,
            .planValidated = true,
            .logicalPassSealed = true,
        }).eligible,
        "production shadow feeds the existing eligibility contract");

  input.sourceStartsPass = false;
  check(dxmt9::encoders::produceSealedParallelPassSnapshot(input, snapshot)
                .fallback ==
            dxmt9::encoders::SealedParallelPassSnapshotFallback::UnsealedStart &&
            snapshot.childCount == 0u,
        "carried predecessor state rejects sealing before snapshot effects");
  input.sourceStartsPass = true;
  input.sourceEndsPass = false;
  check(dxmt9::encoders::produceSealedParallelPassSnapshot(input, snapshot)
                .fallback ==
            dxmt9::encoders::SealedParallelPassSnapshotFallback::UnsealedEnd,
        "deferred source tail rejects sealing");

  input.sourceEndsPass = true;
  input.ranges = fixture.production.view().first(1u);
  check(dxmt9::encoders::produceSealedParallelPassSnapshot(input, snapshot)
                .fallback ==
            dxmt9::encoders::SealedParallelPassSnapshotFallback::TooFewChildren,
        "one child remains serial even when the pass is complete");

  auto malformed = fixture.production.ranges;
  ++malformed[1].entry.uniformHandle.generation;
  input.ranges = std::span<const EncodePartitionRangeSnapshot>(
      malformed.data(), fixture.production.count);
  check(dxmt9::encoders::produceSealedParallelPassSnapshot(input, snapshot)
                .fallback ==
            dxmt9::encoders::SealedParallelPassSnapshotFallback::FirstDrawSnapshot,
        "stale first-draw provenance rejects the whole shadow");

  std::array<EncodePartitionRangeSnapshot,
             dxmt9::encoders::kParallelRenderPassChildCapacity + 1u>
      oversized{};
  for (auto& range : oversized) {
    range = fixture.production.ranges[0];
  }
  input.ranges = oversized;
  check(dxmt9::encoders::produceSealedParallelPassSnapshot(input, snapshot)
                .fallback ==
            dxmt9::encoders::SealedParallelPassSnapshotFallback::ChildCapacity,
        "shadow storage fails closed above the child bound");

  dxmt9::core::ChunkSlot presentSlot{};
  presentSlot.seqId = 402u;
  std::vector<dxmt9::core::DrawParam> presentDraws(96u);
  std::vector<dxmt9::core::DrawParamPayloadView> presentPayloads(
      presentDraws.size());
  presentSlot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                            dxmt9::core::DrawUniformPayload{}, presentDraws,
                            presentPayloads);
  presentSlot.appendPresent({}, dxmt9::core::Handle{0x71u});
  auto presentStream = dxmt9::encoders::makeEncodePartitionReplayStream(
      4u, presentSlot, 0u, presentSlot.commandCount(), false, {}, {},
      sourceRef(presentSlot.seqId));
  dxmt9::encoders::ProductionEncodePartitionPlanStorage presentPlan{};
  check(dxmt9::encoders::planProductionEncodePartitions(
            presentStream, presentPlan).explicitPlan &&
            dxmt9::encoders::validateEncodePartitionRanges(
                presentPlan.view(), presentStream),
        "present-tail fixture has one completely validated plan");
  const auto presentResult =
      dxmt9::encoders::produceSealedParallelPassSnapshot({
            .stream = &presentStream,
            .ranges = presentPlan.view(),
            .planValidated = true,
            .sourceStartsPass = true,
            .sourceEndsPass = true,
          }, snapshot);
  check(presentResult.eligible && snapshot.hasTerminalPresent &&
            snapshot.childCount == 3u && snapshot.drawCount == 96u,
        "one final Present seals but does not become a child range");

  dxmt9::core::ChunkSlot clearSlot{};
  clearSlot.seqId = 403u;
  clearSlot.appendClear({});
  clearSlot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                          dxmt9::core::DrawUniformPayload{}, presentDraws,
                          presentPayloads);
  auto clearStream = dxmt9::encoders::makeEncodePartitionReplayStream(
      5u, clearSlot, 0u, clearSlot.commandCount(), false, {}, {},
      sourceRef(clearSlot.seqId));
  dxmt9::encoders::ProductionEncodePartitionPlanStorage clearPlan{};
  check(dxmt9::encoders::planProductionEncodePartitions(
            clearStream, clearPlan).explicitPlan &&
            dxmt9::encoders::validateEncodePartitionRanges(
                clearPlan.view(), clearStream),
        "clear fixture has one completely validated plan");
  check(dxmt9::encoders::produceSealedParallelPassSnapshot({
            .stream = &clearStream,
            .ranges = clearPlan.view(),
            .planValidated = true,
            .sourceStartsPass = true,
            .sourceEndsPass = true,
          }, snapshot).fallback ==
            dxmt9::encoders::SealedParallelPassSnapshotFallback::CoordinatorCommand,
        "a Clear remains coordinator-owned and rejects the first shadow shape");

  dxmt9::core::CanonicalDrawState stateA{};
  dxmt9::core::CanonicalDrawState stateB{};
  stateA.hot.colorAttachments[0].handle = dxmt9::core::Handle{0x81u};
  stateB.hot.colorAttachments[0].handle = dxmt9::core::Handle{0x82u};
  dxmt9::core::ChunkSlot attachmentSlot{};
  attachmentSlot.seqId = 404u;
  attachmentSlot.appendDrawRun(stateA, dxmt9::core::DrawUniformPayload{},
                               presentDraws, presentPayloads);
  attachmentSlot.appendDrawRun(stateB, dxmt9::core::DrawUniformPayload{},
                               presentDraws, presentPayloads);
  auto attachmentStream = dxmt9::encoders::makeEncodePartitionReplayStream(
      6u, attachmentSlot, 0u, attachmentSlot.commandCount(), false, {}, {},
      sourceRef(attachmentSlot.seqId));
  dxmt9::encoders::ProductionEncodePartitionPlanStorage attachmentPlan{};
  check(dxmt9::encoders::planProductionEncodePartitions(
            attachmentStream, attachmentPlan).explicitPlan &&
            dxmt9::encoders::validateEncodePartitionRanges(
                attachmentPlan.view(), attachmentStream),
        "attachment fixture has one completely validated plan");
  check(dxmt9::encoders::produceSealedParallelPassSnapshot({
            .stream = &attachmentStream,
            .ranges = attachmentPlan.view(),
            .planValidated = true,
            .sourceStartsPass = true,
            .sourceEndsPass = true,
          }, snapshot).fallback ==
            dxmt9::encoders::SealedParallelPassSnapshotFallback::AttachmentMismatch,
        "attachment changes split rather than over-seal a logical pass");

  stateB = stateA;
  stateB.hot.textures[0] = stateA.hot.colorAttachments[0].handle;
  dxmt9::core::ChunkSlot hazardSlot{};
  hazardSlot.seqId = 405u;
  hazardSlot.appendDrawRun(stateA, dxmt9::core::DrawUniformPayload{},
                           presentDraws, presentPayloads);
  hazardSlot.appendDrawRun(stateB, dxmt9::core::DrawUniformPayload{},
                           presentDraws, presentPayloads);
  auto hazardStream = dxmt9::encoders::makeEncodePartitionReplayStream(
      7u, hazardSlot, 0u, hazardSlot.commandCount(), false, {}, {},
      sourceRef(hazardSlot.seqId));
  dxmt9::encoders::ProductionEncodePartitionPlanStorage hazardPlan{};
  check(dxmt9::encoders::planProductionEncodePartitions(
            hazardStream, hazardPlan).explicitPlan &&
            dxmt9::encoders::validateEncodePartitionRanges(
                hazardPlan.view(), hazardStream),
        "hazard fixture has one completely validated plan");
  check(dxmt9::encoders::produceSealedParallelPassSnapshot({
            .stream = &hazardStream,
            .ranges = hazardPlan.view(),
            .planValidated = true,
            .sourceStartsPass = true,
            .sourceEndsPass = true,
          }, snapshot).fallback ==
            dxmt9::encoders::SealedParallelPassSnapshotFallback::ResourceHazard,
        "attachment sampling rejects the sealed parallel shadow");
}

enum class EventKind : std::uint8_t {
  Prepare,
  Create,
  BeginActions,
  LogicalCommands,
  Emit,
  EndChild,
  Join,
  EndActions,
  EndParent,
  Sidecars,
  Completion,
  Abandon,
  FailStop,
};

struct Event {
  EventKind kind = EventKind::Prepare;
  std::uint32_t child = 0;
};

struct FakeChildBackend {
  std::array<Event, 64> events{};
  std::size_t eventCount = 0;
  std::array<std::uint32_t, 96> drawReplayCount{};
  std::array<std::uint32_t, 3> shadows{};
  std::uint32_t logicalCommandReplayCount = 0;
  std::uint32_t actionBeginCount = 0;
  std::uint32_t actionEndCount = 0;
  std::uint32_t sidecarCount = 0;
  std::uint32_t completionCount = 0;
  std::uint32_t failStopCount = 0;
  std::uint32_t failCreateChild = UINT32_MAX;
  ParallelPassFailurePhase injectedFailure = ParallelPassFailurePhase::None;
  std::uint32_t injectedChild = UINT32_MAX;
  ParallelPassFailurePhase terminalFailure = ParallelPassFailurePhase::None;
  std::uint32_t terminalChild = UINT32_MAX;

  void note(EventKind kind, std::uint32_t child = 0u) noexcept {
    events[eventCount++] = Event{kind, child};
  }

  bool prepareParent() noexcept {
    note(EventKind::Prepare);
    return true;
  }
  bool createChild(const ParallelPassChildPlan& child) noexcept {
    note(EventKind::Create, child.childOrdinal);
    if (child.childOrdinal == failCreateChild) {
      return false;
    }
    check(child.forceFullFirstDrawBinding,
          "child must force complete first-draw native binding");
    check(child.firstDraw.complete && child.firstDraw.generation != 0u,
          "child receives its immutable first-draw snapshot");
    shadows[child.childOrdinal] = child.localShadowOrdinal;
    return true;
  }
  void abandonPrepared() noexcept { note(EventKind::Abandon); }
  bool shouldFail(ParallelPassFailurePhase phase,
                  std::uint32_t child = UINT32_MAX) const noexcept {
    return injectedFailure == phase &&
        (injectedChild == UINT32_MAX || injectedChild == child);
  }
  bool beginPassActions() noexcept {
    note(EventKind::BeginActions);
    ++actionBeginCount;
    return !shouldFail(ParallelPassFailurePhase::BeginPassActions);
  }
  bool replayLogicalCommands(
      std::span<const ParallelPassChildPlan> children) noexcept {
    note(EventKind::LogicalCommands);
    check(!children.empty(), "logical command replay receives child ranges");
    const auto commandIndex = children.front().range.entry.commandIndex;
    for (const auto& child : children) {
      checkEq(child.range.entry.commandIndex, commandIndex,
              "fixture child ranges belong to one logical DrawRun command");
    }
    ++logicalCommandReplayCount;
    return !shouldFail(ParallelPassFailurePhase::LogicalCommandReplay);
  }
  bool emitChild(const ParallelPassChildPlan& child) noexcept {
    note(EventKind::Emit, child.childOrdinal);
    if (shouldFail(ParallelPassFailurePhase::ChildEmission,
                   child.childOrdinal)) {
      return false;
    }
    const auto first = child.range.entry.drawParamIndex;
    for (std::uint32_t i = 0; i < child.range.drawEntryCount; ++i) {
      ++drawReplayCount[first + i];
    }
    return true;
  }
  bool endChild(std::uint32_t child) noexcept {
    note(EventKind::EndChild, child);
    return !shouldFail(ParallelPassFailurePhase::ChildEnd, child);
  }
  bool joinChild(std::uint32_t child) noexcept {
    note(EventKind::Join, child);
    return !shouldFail(ParallelPassFailurePhase::ChildJoin, child);
  }
  bool endPassActions() noexcept {
    note(EventKind::EndActions);
    ++actionEndCount;
    return !shouldFail(ParallelPassFailurePhase::EndPassActions);
  }
  bool endParent() noexcept {
    note(EventKind::EndParent);
    return !shouldFail(ParallelPassFailurePhase::ParentEnd);
  }
  bool publishSidecars() noexcept {
    note(EventKind::Sidecars);
    ++sidecarCount;
    return !shouldFail(ParallelPassFailurePhase::SidecarPublication);
  }
  bool publishCompletion() noexcept {
    note(EventKind::Completion);
    ++completionCount;
    return !shouldFail(ParallelPassFailurePhase::CompletionPublication);
  }
  void failStop(ParallelPassFailurePhase phase,
                std::uint32_t child) noexcept {
    note(EventKind::FailStop, child);
    ++failStopCount;
    terminalFailure = phase;
    terminalChild = child;
  }
};

dxmt9::encoders::ParallelPassPlanStorage eligiblePlan(
    ProductionPlanFixture& fixture) {
  dxmt9::encoders::ParallelPassPlanStorage storage{};
  check(dxmt9::encoders::planParallelRenderPassChildren(fixture.input(),
                                                         storage)
            .eligible,
        "fake executor fixture is eligible");
  return storage;
}

void fakeChildrenPreserveOwnershipOrderingAndExactlyOnceReplay() {
  ProductionPlanFixture fixture;
  const auto plan = eligiblePlan(fixture);
  FakeChildBackend backend{};
  const std::array<std::uint32_t, 3> completionOrder{2u, 0u, 1u};
  const auto result = dxmt9::encoders::executeParallelRenderPass(
      plan.view(), completionOrder, backend);
  check(result.status == ParallelPassExecutionStatus::Completed &&
            result.crossedEffectBoundary,
        "fake child execution completes through the selected lane");

  std::array<std::uint32_t, 3> created{};
  std::array<std::uint32_t, 3> joined{};
  std::size_t createCount = 0;
  std::size_t joinCount = 0;
  std::size_t lastJoin = 0;
  std::size_t parentEnd = 0;
  for (std::size_t i = 0; i < backend.eventCount; ++i) {
    const auto event = backend.events[i];
    if (event.kind == EventKind::Create) {
      created[createCount++] = event.child;
    } else if (event.kind == EventKind::Join) {
      joined[joinCount++] = event.child;
      lastJoin = i;
    } else if (event.kind == EventKind::EndParent) {
      parentEnd = i;
    }
  }
  checkEq(created, std::array<std::uint32_t, 3>{0u, 1u, 2u},
          "children are created in draw order");
  checkEq(joined, completionOrder,
          "coordinator accepts arbitrary child completion order");
  check(lastJoin < parentEnd, "all children join before parent end");
  check(backend.shadows[0] != backend.shadows[1] &&
            backend.shadows[0] != backend.shadows[2] &&
            backend.shadows[1] != backend.shadows[2],
        "every child owns a distinct local native shadow");
  for (const auto count : backend.drawReplayCount) {
    checkEq(count, std::uint32_t{1}, "every draw replays exactly once");
  }
  checkEq(backend.logicalCommandReplayCount, std::uint32_t{1},
          "the split DrawRun command is coordinated exactly once");
  check(backend.actionBeginCount == 1u && backend.actionEndCount == 1u &&
            backend.sidecarCount == 1u && backend.completionCount == 1u,
        "coordinator alone owns pass actions, sidecars, and completion");
  checkEq(backend.failStopCount, std::uint32_t{0},
          "successful execution does not invoke terminal cleanup");
}

enum class ChildPlanMalformation : std::uint8_t {
  CommandRange,
  ReplayCount,
  EmptyDrawRange,
  DrawOverflow,
  MissingLocator,
  Overlap,
  SourceMismatch,
  ChildOrdinal,
  ShadowDuplicate,
  ProvenanceMismatch,
  IncompleteSnapshot,
  IncompleteEntryRender,
  FullBindDisabled,
};

void malformedPlansFailClosedBeforeParentPreparation() {
  ProductionPlanFixture fixture;
  const auto valid = eligiblePlan(fixture);
  const std::array<std::uint32_t, 3> completionOrder{0u, 1u, 2u};
  struct Case {
    ChildPlanMalformation malformation;
    ParallelPassFallbackReason expected;
  };
  const std::array cases{
      Case{ChildPlanMalformation::CommandRange,
           ParallelPassFallbackReason::ChildRangeInvalid},
      Case{ChildPlanMalformation::ReplayCount,
           ParallelPassFallbackReason::ChildRangeInvalid},
      Case{ChildPlanMalformation::EmptyDrawRange,
           ParallelPassFallbackReason::ChildRangeInvalid},
      Case{ChildPlanMalformation::DrawOverflow,
           ParallelPassFallbackReason::ChildRangeInvalid},
      Case{ChildPlanMalformation::MissingLocator,
           ParallelPassFallbackReason::ChildRangeInvalid},
      Case{ChildPlanMalformation::Overlap,
           ParallelPassFallbackReason::ChildRangeOrderInvalid},
      Case{ChildPlanMalformation::SourceMismatch,
           ParallelPassFallbackReason::ChildRangeOrderInvalid},
      Case{ChildPlanMalformation::ChildOrdinal,
           ParallelPassFallbackReason::ChildOrdinalInvalid},
      Case{ChildPlanMalformation::ShadowDuplicate,
           ParallelPassFallbackReason::LocalShadowInvalid},
      Case{ChildPlanMalformation::ProvenanceMismatch,
           ParallelPassFallbackReason::FirstDrawProvenanceInvalid},
      Case{ChildPlanMalformation::IncompleteSnapshot,
           ParallelPassFallbackReason::FirstDrawProvenanceInvalid},
      Case{ChildPlanMalformation::IncompleteEntryRender,
           ParallelPassFallbackReason::FirstDrawProvenanceInvalid},
      Case{ChildPlanMalformation::FullBindDisabled,
           ParallelPassFallbackReason::FullFirstDrawBindingRequired},
  };

  for (const auto& testCase : cases) {
    auto malformed = valid;
    switch (testCase.malformation) {
    case ChildPlanMalformation::CommandRange:
      malformed.children[1].range.kind =
          dxmt9::encoders::EncodePartitionRangeKind::CommandSegment;
      break;
    case ChildPlanMalformation::ReplayCount:
      malformed.children[1].range.replayOrdinalCount = 2u;
      break;
    case ChildPlanMalformation::EmptyDrawRange:
      malformed.children[1].range.drawEntryCount = 0u;
      break;
    case ChildPlanMalformation::DrawOverflow:
      malformed.children[1].range.entry.drawParamIndex = UINT32_MAX;
      break;
    case ChildPlanMalformation::MissingLocator:
      malformed.children[1].range.entry.source.tapeSource = {};
      break;
    case ChildPlanMalformation::Overlap:
      --malformed.children[1].range.entry.drawParamIndex;
      malformed.children[1].firstDraw.provenance =
          malformed.children[1].range.entry;
      break;
    case ChildPlanMalformation::SourceMismatch:
      ++malformed.children[1].range.entry.source.seqId;
      malformed.children[1].firstDraw.provenance =
          malformed.children[1].range.entry;
      break;
    case ChildPlanMalformation::ChildOrdinal:
      malformed.children[1].childOrdinal = 0u;
      break;
    case ChildPlanMalformation::ShadowDuplicate:
      malformed.children[1].localShadowOrdinal =
          malformed.children[0].localShadowOrdinal;
      break;
    case ChildPlanMalformation::ProvenanceMismatch:
      ++malformed.children[1].firstDraw.provenance.commandIndex;
      break;
    case ChildPlanMalformation::IncompleteSnapshot:
      malformed.children[1].firstDraw.complete = false;
      break;
    case ChildPlanMalformation::IncompleteEntryRender:
      malformed.children[1].firstDraw.entryRender.flags = 0u;
      break;
    case ChildPlanMalformation::FullBindDisabled:
      malformed.children[1].forceFullFirstDrawBinding = false;
      break;
    }
    FakeChildBackend backend{};
    const auto result = dxmt9::encoders::executeParallelRenderPass(
        malformed.view(), completionOrder, backend);
    check(result.status == ParallelPassExecutionStatus::SerialFallback &&
              !result.crossedEffectBoundary && backend.eventCount == 0u &&
              result.failurePhase ==
                  ParallelPassFailurePhase::ChildPlanValidation &&
              result.fallback == testCase.expected,
          "malformed child plan fails closed before parent preparation");
  }
}

void failuresSeparatePreEffectFallbackFromPostEffectFailStop() {
  ProductionPlanFixture fixture;
  const auto plan = eligiblePlan(fixture);
  const std::array<std::uint32_t, 3> completionOrder{0u, 1u, 2u};

  FakeChildBackend preEffect{};
  preEffect.failCreateChild = 1u;
  const auto fallback = dxmt9::encoders::executeParallelRenderPass(
      plan.view(), completionOrder, preEffect);
  check(fallback.status == ParallelPassExecutionStatus::SerialFallback &&
            !fallback.crossedEffectBoundary &&
            fallback.fallback ==
                ParallelPassFallbackReason::ChildCreationFailed,
        "child creation rejection selects serial before effects");
  check(preEffect.actionBeginCount == 0u &&
            preEffect.logicalCommandReplayCount == 0u &&
            preEffect.sidecarCount == 0u && preEffect.completionCount == 0u,
        "pre-effect fallback publishes no parallel-pass ownership effects");
  checkEq(preEffect.failStopCount, std::uint32_t{0},
          "pre-effect serial fallback does not invoke fail-stop cleanup");

  FakeChildBackend invalidJoin{};
  const std::array<std::uint32_t, 3> duplicateCompletion{0u, 0u, 2u};
  const auto invalid = dxmt9::encoders::executeParallelRenderPass(
      plan.view(), duplicateCompletion, invalidJoin);
  check(invalid.status == ParallelPassExecutionStatus::SerialFallback &&
            !invalid.crossedEffectBoundary && invalidJoin.eventCount == 0u &&
            invalid.fallback ==
                ParallelPassFallbackReason::InvalidCompletionOrder,
        "invalid arbitrary completion order falls back before preparation");

  struct EffectFailure {
    ParallelPassFailurePhase phase;
    std::uint32_t child;
  };
  const std::array failures{
      EffectFailure{ParallelPassFailurePhase::BeginPassActions, UINT32_MAX},
      EffectFailure{ParallelPassFailurePhase::LogicalCommandReplay,
                    UINT32_MAX},
      EffectFailure{ParallelPassFailurePhase::ChildEmission, 0u},
      EffectFailure{ParallelPassFailurePhase::ChildEmission, 1u},
      EffectFailure{ParallelPassFailurePhase::ChildEmission, 2u},
      EffectFailure{ParallelPassFailurePhase::ChildEnd, 0u},
      EffectFailure{ParallelPassFailurePhase::ChildEnd, 1u},
      EffectFailure{ParallelPassFailurePhase::ChildEnd, 2u},
      EffectFailure{ParallelPassFailurePhase::ChildJoin, 0u},
      EffectFailure{ParallelPassFailurePhase::ChildJoin, 1u},
      EffectFailure{ParallelPassFailurePhase::ChildJoin, 2u},
      EffectFailure{ParallelPassFailurePhase::EndPassActions, UINT32_MAX},
      EffectFailure{ParallelPassFailurePhase::ParentEnd, UINT32_MAX},
      EffectFailure{ParallelPassFailurePhase::SidecarPublication, UINT32_MAX},
      EffectFailure{ParallelPassFailurePhase::CompletionPublication,
                    UINT32_MAX},
  };
  for (const auto& failure : failures) {
    FakeChildBackend postEffect{};
    postEffect.injectedFailure = failure.phase;
    postEffect.injectedChild = failure.child;
    const auto failStop = dxmt9::encoders::executeParallelRenderPass(
        plan.view(), completionOrder, postEffect);
    const std::uint32_t expectedChild =
        failure.child == UINT32_MAX ? 0u : failure.child;
    check(failStop.status == ParallelPassExecutionStatus::FailStop &&
              failStop.crossedEffectBoundary &&
              failStop.failurePhase == failure.phase &&
              failStop.affectedChild == expectedChild &&
              postEffect.failStopCount == 1u &&
              postEffect.terminalFailure == failure.phase &&
              postEffect.terminalChild == expectedChild &&
              postEffect.events[postEffect.eventCount - 1u].kind ==
                  EventKind::FailStop,
          "every post-effect failure invokes terminal fail-stop cleanup once");
  }
}

}  // namespace

int main() {
  try {
    eligibilityAndSelectionAreTypedAndBounded();
    productionShadowSealsOnlyCompleteOwnedPasses();
    fakeChildrenPreserveOwnershipOrderingAndExactlyOnceReplay();
    malformedPlansFailClosedBeforeParentPreparation();
    failuresSeparatePreEffectFallbackFromPostEffectFailStop();
  } catch (const TestFailure& error) {
    std::cerr << "parallel_render_pass_spec failed: " << error.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "parallel_render_pass_spec unexpected exception: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}
