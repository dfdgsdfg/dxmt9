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

enum class EventKind : std::uint8_t {
  Prepare,
  Create,
  BeginActions,
  Emit,
  EndChild,
  Join,
  EndActions,
  EndParent,
  Sidecars,
  Completion,
  Abandon,
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
  std::uint32_t failCreateChild = UINT32_MAX;
  std::uint32_t failEmitChild = UINT32_MAX;

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
  bool beginPassActions() noexcept {
    note(EventKind::BeginActions);
    ++actionBeginCount;
    return true;
  }
  bool replayLogicalCommands(
      std::span<const ParallelPassChildPlan> children) noexcept {
    check(!children.empty(), "logical command replay receives child ranges");
    const auto commandIndex = children.front().range.entry.commandIndex;
    for (const auto& child : children) {
      checkEq(child.range.entry.commandIndex, commandIndex,
              "fixture child ranges belong to one logical DrawRun command");
    }
    ++logicalCommandReplayCount;
    return true;
  }
  bool emitChild(const ParallelPassChildPlan& child) noexcept {
    note(EventKind::Emit, child.childOrdinal);
    if (child.childOrdinal == failEmitChild) {
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
    return true;
  }
  bool joinChild(std::uint32_t child) noexcept {
    note(EventKind::Join, child);
    return true;
  }
  bool endPassActions() noexcept {
    note(EventKind::EndActions);
    ++actionEndCount;
    return true;
  }
  bool endParent() noexcept {
    note(EventKind::EndParent);
    return true;
  }
  bool publishSidecars() noexcept {
    note(EventKind::Sidecars);
    ++sidecarCount;
    return true;
  }
  bool publishCompletion() noexcept {
    note(EventKind::Completion);
    ++completionCount;
    return true;
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

  FakeChildBackend invalidJoin{};
  const std::array<std::uint32_t, 3> duplicateCompletion{0u, 0u, 2u};
  const auto invalid = dxmt9::encoders::executeParallelRenderPass(
      plan.view(), duplicateCompletion, invalidJoin);
  check(invalid.status == ParallelPassExecutionStatus::SerialFallback &&
            !invalid.crossedEffectBoundary && invalidJoin.eventCount == 0u &&
            invalid.fallback ==
                ParallelPassFallbackReason::InvalidCompletionOrder,
        "invalid arbitrary completion order falls back before preparation");

  FakeChildBackend postEffect{};
  postEffect.failEmitChild = 1u;
  const auto failStop = dxmt9::encoders::executeParallelRenderPass(
      plan.view(), completionOrder, postEffect);
  check(failStop.status == ParallelPassExecutionStatus::FailStop &&
            failStop.crossedEffectBoundary &&
            failStop.failurePhase ==
                dxmt9::encoders::ParallelPassFailurePhase::ChildEmission,
        "failure after first pass effect is fail-stop");
  check(postEffect.sidecarCount == 0u && postEffect.completionCount == 0u,
        "fail-stop never publishes a partial logical-pass tail");
}

}  // namespace

int main() {
  try {
    eligibilityAndSelectionAreTypedAndBounded();
    fakeChildrenPreserveOwnershipOrderingAndExactlyOnceReplay();
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
