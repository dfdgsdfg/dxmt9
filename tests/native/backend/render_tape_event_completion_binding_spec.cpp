// Bounded completion-binding truth table for fixed-input Render Tape plans.
//
// This spec invokes the production RenderTapeCompletionPlanController.  It
// does not emulate resources, Metal, or GPU completion.  The adapter checks
// below are intentionally separate: source-slot reuse and expected-generation
// admission belong to the production Pool/queue binder, while the controller
// owns event/submit/release frontiers after that binder supplies the actual
// qualified key.

#include "../../../src/d3d9/device_c_render_tape_completion_plan.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dxmt9::d3d9;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

RenderTapeCompletionPlanFile recycledSourcePlan() {
  RenderTapeCompletionPlanFile plan{};
  plan.header.byteSize = sizeof(plan);
  plan.header.tapeEventCount = 4u;
  plan.header.submitCount = 2u;
  plan.header.releaseCount = 2u;
  plan.header.repeatCount = 1u;
  plan.header.route = RenderTapeCompletionPlanRoute::ProductionSlot;
  plan.header.eventsDigest[0] = std::byte{0x7a};
  plan.submits[0] = {.eventOrdinal = 1u, .eventSubmissionOrdinal = 1u};
  plan.submits[1] = {.eventOrdinal = 3u, .eventSubmissionOrdinal = 1u};
  plan.releases[0] = {
      .releaseAtEventOrdinal = 2u,
      .sourceEventOrdinal = 1u,
      .sourceSubmissionOrdinal = 1u};
  plan.releases[1] = {
      .releaseAtEventOrdinal = 4u,
      .sourceEventOrdinal = 3u,
      .sourceSubmissionOrdinal = 1u};
  return plan;
}

RenderTapeQualifiedCompletionKey actual(std::uint64_t seq,
                                        std::uint64_t sourceGeneration,
                                        std::uint64_t storageGeneration) {
  return {
      .seqId = seq,
      .sourceIndex = 1u,
      .sourceGeneration = sourceGeneration,
      .storageFirstPage = 1u,
      .storagePageCount = 1u,
      .storageGeneration = storageGeneration};
}

const RenderTapeQualifiedCompletionKey kFirst = actual(11u, 101u, 1001u);
const RenderTapeQualifiedCompletionKey kSecond = actual(23u, 202u, 2002u);

void testRecycledSourceBindsAndReleasesInEventOrder() {
  const auto plan = recycledSourcePlan();
  RenderTapeCompletionPlanController controller(plan, plan.header.eventsDigest);
  check(controller.beginIteration(1u, RenderTapeCompletionPlanRoute::ProductionSlot)
            .accepted(),
        "authenticated iteration starts");

  std::vector<RenderTapeQualifiedCompletionKey> released;
  check(controller.submit(1u, 1u, kFirst, [] { return true; }).accepted(),
        "first Tape event binds its actual source");
  check(controller.releaseDue(1u, [&](const auto&) {
          released.push_back(kFirst);
          return true;
        }).accepted() && released.empty(),
        "completion is delayed until the declared release event");
  check(controller.endEvent(1u).accepted(), "first event closes");
  check(controller.releaseDue(2u, [&](const auto& key) {
          released.push_back(key);
          return true;
        }).accepted() && released.size() == 1u && released[0] == kFirst,
        "first source completion reaches the callback at event two");
  check(controller.endEvent(2u).accepted(), "second event closes");

  // This bounded schedule recycles the source-slot index only after the first
  // callback.  The qualified generation and storage generation both change.
  // It is not a universal Pool/GPU reuse proof; post-encode retirement may
  // provision a new generation before this schedule's callback boundary.
  check(controller.submit(3u, 1u, kSecond, [] { return true; }).accepted(),
        "recycled slot binds a distinct actual generation");
  check(controller.endEvent(3u).accepted(), "third event closes");
  check(controller.releaseDue(4u, [&](const auto& key) {
          released.push_back(key);
          return true;
        }).accepted() && released.size() == 2u && released[1] == kSecond,
        "second completion remains ordered after the first");
  check(controller.endEvent(4u).accepted(), "all Tape events close");
  check(controller.finishIteration().accepted() &&
            controller.submitted() == 2u && controller.released() == 2u,
        "finish requires every submit and ordered release frontier");
  check(kFirst.sourceIndex == kSecond.sourceIndex &&
            kFirst.sourceGeneration != kSecond.sourceGeneration &&
            kFirst.storageGeneration != kSecond.storageGeneration,
        "recycled source index remains generation-qualified");

  RenderTapeCompletionPlanController earlyFinish(
      plan, plan.header.eventsDigest);
  check(earlyFinish.beginIteration(
                    1u, RenderTapeCompletionPlanRoute::ProductionSlot)
                .accepted() &&
            earlyFinish.submit(1u, 1u, kFirst, [] { return true; })
                .accepted() &&
            earlyFinish.endEvent(1u).accepted() &&
            earlyFinish.finishIteration().disposition ==
                RenderTapeCompletionPlanDisposition::Unresolved &&
            earlyFinish.failed(),
        "finish cannot bypass the remaining event and completion frontiers");
}

void testCallbackFailureIsTerminal() {
  const auto plan = recycledSourcePlan();
  RenderTapeCompletionPlanController submitFailure(
      plan, plan.header.eventsDigest);
  check(submitFailure.beginIteration(
                  1u, RenderTapeCompletionPlanRoute::ProductionSlot)
            .accepted() &&
            submitFailure.submit(1u, 1u, kFirst, [] { return false; })
                    .disposition ==
                RenderTapeCompletionPlanDisposition::CallbackFailure,
        "failed production submit is terminal");
  check(submitFailure.submit(1u, 1u, kFirst, [] { return true; })
                    .disposition ==
                RenderTapeCompletionPlanDisposition::Unresolved &&
            submitFailure.failed() &&
            submitFailure.finishIteration().disposition ==
                RenderTapeCompletionPlanDisposition::Unresolved,
        "terminal submit failure cannot be retried or finished");

  RenderTapeCompletionPlanController releaseFailure(
      plan, plan.header.eventsDigest);
  check(releaseFailure.beginIteration(
                    1u, RenderTapeCompletionPlanRoute::ProductionSlot)
                .accepted() &&
            releaseFailure.submit(1u, 1u, kFirst, [] { return true; })
                .accepted() &&
            releaseFailure.releaseDue(2u, [](const auto&) { return false; })
                    .disposition ==
                RenderTapeCompletionPlanDisposition::CallbackFailure &&
            releaseFailure.failed() &&
            releaseFailure.finishIteration().disposition ==
                RenderTapeCompletionPlanDisposition::Unresolved,
        "failed completion callback leaves an unresolved prefix");
}

void testAdapterRejectsStaleGenerationAndPrematureReuse() {
  const auto plan = recycledSourcePlan();

  // The plan reducer cannot infer Pool generation ownership from an opaque
  // ActualKey.  The production binder must reject a stale generation before
  // acknowledging the controller callback.
  RenderTapeCompletionPlanController stale(
      plan, plan.header.eventsDigest);
  check(stale.beginIteration(1u, RenderTapeCompletionPlanRoute::ProductionSlot)
                .accepted() &&
            stale.submit(1u, 1u, kFirst, [] { return true; }).accepted() &&
            stale.releaseDue(2u, [](const auto&) { return true; }).accepted(),
        "stale-generation control reaches the second submit frontier");
  const auto staleGeneration = actual(23u, kFirst.sourceGeneration,
                                      kFirst.storageGeneration);
  bool staleCallbackCalled = false;
  check(stale.submit(3u, 1u, staleGeneration, [&] {
          staleCallbackCalled = true;
          return false;
        }).disposition == RenderTapeCompletionPlanDisposition::DuplicateSubmission &&
            !staleCallbackCalled,
        "controller rejects a stale source pair before the production callback");
  check(stale.failed() &&
            stale.endEvent(3u).disposition ==
                RenderTapeCompletionPlanDisposition::Unresolved,
        "stale-generation rejection latches the active iteration terminal");

  // Reuse before the first completion is likewise an adapter/Pool violation;
  // this only tracks pending completion ownership, not Pool replay residency;
  // the controller callback is the terminal boundary for that failed bind.
  RenderTapeCompletionPlanController premature(
      plan, plan.header.eventsDigest);
  bool firstPendingOwner = true;
  check(premature.beginIteration(
                    1u, RenderTapeCompletionPlanRoute::ProductionSlot)
                .accepted() &&
            premature.submit(1u, 1u, kFirst, [] { return true; }).accepted() &&
            premature.submit(3u, 1u, kSecond, [&] {
              return !firstPendingOwner;
            }).disposition == RenderTapeCompletionPlanDisposition::CallbackFailure,
        "adapter rejects recycled source while prior completion remains pending");
  check(premature.failed() &&
            premature.releaseDue(2u, [](const auto&) { return true; })
                    .disposition ==
                RenderTapeCompletionPlanDisposition::Unresolved,
        "premature reuse rejection latches the active iteration terminal");
  firstPendingOwner = false;

  RenderTapeCompletionPlanController earlyRelease(
      plan, plan.header.eventsDigest);
  check(earlyRelease.beginIteration(
                    1u, RenderTapeCompletionPlanRoute::ProductionSlot)
                .accepted() &&
            earlyRelease.releaseDue(2u, [](const auto&) { return true; })
                    .disposition ==
                RenderTapeCompletionPlanDisposition::ReleaseBeforeSubmit,
        "completion cannot acknowledge an unpublished source");
  check(earlyRelease.failed() &&
            earlyRelease.endEvent(1u).disposition ==
                RenderTapeCompletionPlanDisposition::Unresolved,
        "premature completion latches the active iteration terminal");
}

void testSwappedReleaseRowsFailBeforeBinding() {
  auto plan = recycledSourcePlan();
  std::swap(plan.releases[0], plan.releases[1]);
  RenderTapeCompletionPlanController controller(
      plan, plan.header.eventsDigest);
  check(controller.beginIteration(
                    1u, RenderTapeCompletionPlanRoute::ProductionSlot)
                .disposition ==
            RenderTapeCompletionPlanDisposition::SkippedReleaseFrontier &&
            !controller.failed(),
        "swapped release rows are rejected by plan preflight before effects");
}

enum class TraceStep : std::uint8_t {
  SubmitFirst,
  ReleaseProbeAtOne,
  EndOne,
  ReleaseAtTwo,
  EndTwo,
  SubmitSecond,
  EndThree,
  ReleaseAtFour,
  EndFour,
};

bool applyTraceStep(RenderTapeCompletionPlanController& controller,
                    TraceStep step) {
  switch (step) {
    case TraceStep::SubmitFirst:
      return controller.submit(1u, 1u, kFirst, [] { return true; }).accepted();
    case TraceStep::ReleaseProbeAtOne:
      return controller.releaseDue(1u, [](const auto&) { return true; })
          .accepted();
    case TraceStep::EndOne:
      return controller.endEvent(1u).accepted();
    case TraceStep::ReleaseAtTwo:
      return controller.releaseDue(2u, [](const auto&) { return true; })
          .accepted();
    case TraceStep::EndTwo:
      return controller.endEvent(2u).accepted();
    case TraceStep::SubmitSecond:
      return controller.submit(3u, 1u, kSecond, [] { return true; }).accepted();
    case TraceStep::EndThree:
      return controller.endEvent(3u).accepted();
    case TraceStep::ReleaseAtFour:
      return controller.releaseDue(4u, [](const auto&) { return true; })
          .accepted();
    case TraceStep::EndFour:
      return controller.endEvent(4u).accepted();
  }
  return false;
}

void testExhaustiveBoundedControllerTraces() {
  // Enumerate every ordering of the nine bounded controller operations.  No
  // parallel reducer is used: each trace calls the production controller and
  // only records whether that concrete trace reaches finish.
  std::array<TraceStep, 9> steps{
      TraceStep::SubmitFirst, TraceStep::ReleaseProbeAtOne,
      TraceStep::EndOne, TraceStep::ReleaseAtTwo, TraceStep::EndTwo,
      TraceStep::SubmitSecond, TraceStep::EndThree,
      TraceStep::ReleaseAtFour, TraceStep::EndFour};
  std::size_t acceptedTraces = 0u;
  std::size_t rejectedTraces = 0u;
  do {
    const auto plan = recycledSourcePlan();
    RenderTapeCompletionPlanController controller(
        plan, plan.header.eventsDigest);
    bool accepted = controller.beginIteration(
                              1u, RenderTapeCompletionPlanRoute::ProductionSlot)
                        .accepted();
    for (const auto step : steps) {
      if (!accepted) break;
      accepted = applyTraceStep(controller, step);
    }
    if (accepted) {
      ++acceptedTraces;
      check(controller.finishIteration().accepted(),
            "an all-accepted bounded trace finishes");
      check(controller.submitted() == 2u && controller.released() == 2u,
            "accepted trace reaches both reducer frontiers");
    } else {
      ++rejectedTraces;
    }
  } while (std::next_permutation(steps.begin(), steps.end(),
                                 [](TraceStep lhs, TraceStep rhs) {
                                   return static_cast<unsigned>(lhs) <
                                          static_cast<unsigned>(rhs);
                                 }));
  check(acceptedTraces != 0u && rejectedTraces != 0u,
        "bounded controller trace table has both legal and rejected orders");
}

}  // namespace

int main() {
  try {
            testRecycledSourceBindsAndReleasesInEventOrder();
    testCallbackFailureIsTerminal();
    testAdapterRejectsStaleGenerationAndPrematureReuse();
    testSwappedReleaseRowsFailBeforeBinding();
    testExhaustiveBoundedControllerTraces();
  } catch (const TestFailure& failure) {
    std::cerr << "render_tape_event_completion_binding_spec: "
              << failure.what() << '\n';
    return 1;
  }
  return 0;
}
