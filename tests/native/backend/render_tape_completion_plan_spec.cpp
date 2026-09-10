#include "../../../src/d3d9/device_c_render_tape_completion_plan.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace dxmt9::d3d9;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

RenderTapeCompletionPlanFile twoSourcePlan(
    std::uint32_t repeat = 1u,
    RenderTapeCompletionPlanRoute route =
        RenderTapeCompletionPlanRoute::ProductionSlot) {
  RenderTapeCompletionPlanFile plan{};
  plan.header.byteSize = sizeof(plan);
  plan.header.tapeEventCount = 4u;
  plan.header.submitCount = 2u;
  plan.header.releaseCount = 2u;
  plan.header.repeatCount = repeat;
  plan.header.route = route;
  plan.header.eventsDigest[0] = std::byte{0x42};
  plan.submits[0] = {.eventOrdinal = 2u, .eventSubmissionOrdinal = 1u};
  plan.submits[1] = {.eventOrdinal = 2u, .eventSubmissionOrdinal = 2u};
  plan.releases[0] = {
      .releaseAtEventOrdinal = 3u,
      .sourceEventOrdinal = 2u,
      .sourceSubmissionOrdinal = 1u};
  plan.releases[1] = {
      .releaseAtEventOrdinal = 4u,
      .sourceEventOrdinal = 2u,
      .sourceSubmissionOrdinal = 2u};
  return plan;
}

RenderTapeQualifiedCompletionKey actual(std::uint64_t seq,
                                        std::uint32_t source,
                                        std::uint64_t generation) {
  return {
      .seqId = seq,
      .sourceIndex = source,
      .sourceGeneration = generation,
      .storageFirstPage = source + 10u,
      .storagePageCount = 1u,
      .storageGeneration = generation + 100u};
}

void testPlanFileDecodeAndValidation() {
  auto plan = twoSourcePlan();
  check(validateRenderTapeCompletionPlan(plan).accepted(),
        "canonical completion plan validates");
  check(validateRenderTapeCompletionPlan(plan, plan.header.eventsDigest).accepted(),
        "plan digest authenticates against the fixed Tape digest");
  auto wrongDigest = plan.header.eventsDigest;
  wrongDigest[0] = std::byte{0x43};
  check(validateRenderTapeCompletionPlan(plan, wrongDigest).disposition ==
            RenderTapeCompletionPlanDisposition::InvalidDigest,
        "plan digest mismatch is rejected before effects");

  RenderTapeCompletionPlanFile decoded{};
  const auto bytes = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(&plan), sizeof(plan));
  check(decodeRenderTapeCompletionPlan(bytes, decoded).accepted() &&
            decoded.header.eventsDigest == plan.header.eventsDigest,
        "fixed plan bytes decode without allocation");
  check(decodeRenderTapeCompletionPlan(bytes.first(bytes.size() - 1u), decoded)
            .disposition == RenderTapeCompletionPlanDisposition::InvalidSize,
        "truncated plan is rejected before effects");

  auto duplicate = plan;
  duplicate.submits[1] = duplicate.submits[0];
  check(validateRenderTapeCompletionPlan(duplicate).disposition ==
            RenderTapeCompletionPlanDisposition::DuplicateSubmit,
        "duplicate submit locator is rejected");

  auto missing = plan;
  missing.header.releaseCount = 1u;
  check(validateRenderTapeCompletionPlan(missing).disposition ==
            RenderTapeCompletionPlanDisposition::MissingRelease,
        "one-to-two submission/release mismatch is rejected");

  auto restartOrdinal = plan;
  restartOrdinal.submits[1].eventOrdinal = 3u;
  check(validateRenderTapeCompletionPlan(restartOrdinal).disposition ==
            RenderTapeCompletionPlanDisposition::NonMonotonicSubmit,
        "each event's physical source ordinal must restart at one");

  auto unknown = plan;
  unknown.releases[1].sourceSubmissionOrdinal = 3u;
  check(validateRenderTapeCompletionPlan(unknown).disposition ==
            RenderTapeCompletionPlanDisposition::MissingRelease,
        "release for an unplanned source is rejected");

  auto boundary = plan;
  boundary.releases[0].releaseAtEventOrdinal = 1u;
  check(validateRenderTapeCompletionPlan(boundary).disposition ==
            RenderTapeCompletionPlanDisposition::InvalidReleaseBoundary,
        "release before source event is rejected");

  auto reserved = plan;
  reserved.header.reserved0 = 1u;
  check(validateRenderTapeCompletionPlan(reserved).disposition ==
            RenderTapeCompletionPlanDisposition::InvalidHeader,
        "reserved plan header fields are rejected");
  reserved = plan;
  reserved.header.reserved1 = 1u;
  check(validateRenderTapeCompletionPlan(reserved).disposition ==
            RenderTapeCompletionPlanDisposition::InvalidHeader,
        "reserved plan tail field is rejected");
  auto trailing = plan;
  trailing.submits[2] = {.eventOrdinal = 4u, .eventSubmissionOrdinal = 1u};
  check(validateRenderTapeCompletionPlan(trailing).disposition ==
            RenderTapeCompletionPlanDisposition::InvalidCapacity,
        "nonzero unused submit rows are rejected");
}

void testGlobalBindingPreservesTwoPhysicalSources() {
  const auto plan = twoSourcePlan();
  RenderTapeCompletionPlanController controller(plan, plan.header.eventsDigest);
  check(controller.beginIteration(1u,
                                  RenderTapeCompletionPlanRoute::ProductionSlot)
            .accepted(),
        "controller starts the authenticated route");

  const auto first = actual(11u, 3u, 21u);
  const auto second = actual(12u, 7u, 22u);
  bool firstSubmitted = false;
  bool secondSubmitted = false;
  check(controller.submit(2u, 1u, first, [&] {
          firstSubmitted = true;
          return true;
        }).accepted() &&
            controller.submit(2u, 2u, second, [&] {
              secondSubmitted = true;
              return true;
            }).accepted() &&
            firstSubmitted && secondSubmitted,
        "both physical sources bind only after their actual identities exist");
  check(controller.endEvent(2u).accepted(),
        "event end accepts complete physical source coverage");

  std::array<RenderTapeQualifiedCompletionKey, 2> released{};
  std::size_t releaseCount = 0u;
  check(controller.releaseDue(3u, [&](const auto& key) {
          released[releaseCount++] = key;
          return true;
        }).accepted() &&
            releaseCount == 1u && released[0] == first,
        "first delayed release acknowledges the first bound source");
  check(controller.endEvent(3u).accepted(),
        "an event with no due release remains explicit and complete");
  check(controller.releaseDue(4u, [&](const auto& key) {
          released[releaseCount++] = key;
          return true;
        }).accepted() &&
            releaseCount == 2u && released[1] == second,
        "second physical source releases at its declared boundary");
  check(controller.endEvent(4u).accepted() &&
            controller.finishIteration().accepted() &&
            controller.submitted() == 2u && controller.released() == 2u,
        "global plan finishes only after both actual sources release");
}

void testRuntimeNegativeControls() {
  using Disposition = RenderTapeCompletionPlanDisposition;

  auto routePlan = twoSourcePlan();
  RenderTapeCompletionPlanController routeController(
      routePlan, routePlan.header.eventsDigest);
  check(routeController.beginIteration(1u,
                                       RenderTapeCompletionPlanRoute::Compatibility)
            .disposition == Disposition::RouteMismatch,
        "route mismatch is rejected before binding");

  auto extraFile = twoSourcePlan();
  RenderTapeCompletionPlanController extra(extraFile,
                                           extraFile.header.eventsDigest);
  check(extra.beginIteration(1u, RenderTapeCompletionPlanRoute::ProductionSlot)
            .accepted() &&
            extra.submit(2u, 1u, actual(1u, 1u, 1u), [] { return true; })
                .accepted() &&
            extra.submit(2u, 3u, actual(2u, 2u, 2u), [] { return true; })
                .disposition == Disposition::UnexpectedSubmit,
        "extra physical source cannot bypass the planned frontier");

  auto missingFile = twoSourcePlan();
  RenderTapeCompletionPlanController missing(missingFile,
                                             missingFile.header.eventsDigest);
  check(missing.beginIteration(1u, RenderTapeCompletionPlanRoute::ProductionSlot)
            .accepted() &&
            missing.submit(2u, 1u, actual(1u, 1u, 1u), [] { return true; })
                .accepted() &&
            missing.endEvent(2u).disposition == Disposition::MissingSubmit,
        "event end detects a missing physical submission");

  auto beforeFile = twoSourcePlan();
  RenderTapeCompletionPlanController before(beforeFile,
                                            beforeFile.header.eventsDigest);
  check(before.beginIteration(1u, RenderTapeCompletionPlanRoute::ProductionSlot)
            .accepted() &&
            before.releaseDue(3u, [](const auto&) { return true; }).disposition ==
                Disposition::ReleaseBeforeSubmit,
        "release cannot occur before actual publication");

  auto skippedFile = twoSourcePlan();
  skippedFile.releases[0] = {
      .releaseAtEventOrdinal = 3u,
      .sourceEventOrdinal = 2u,
      .sourceSubmissionOrdinal = 2u};
  skippedFile.releases[1] = {
      .releaseAtEventOrdinal = 3u,
      .sourceEventOrdinal = 2u,
      .sourceSubmissionOrdinal = 1u};
  RenderTapeCompletionPlanController skipped(
      skippedFile, skippedFile.header.eventsDigest);
  check(validateRenderTapeCompletionPlan(skippedFile).disposition ==
            Disposition::SkippedReleaseFrontier &&
            skipped.beginIteration(1u, RenderTapeCompletionPlanRoute::ProductionSlot)
                .disposition == Disposition::SkippedReleaseFrontier,
        "release frontier rejects a later source before its predecessor");

  auto callbackFailureFile = twoSourcePlan();
  RenderTapeCompletionPlanController callbackFailure(
      callbackFailureFile, callbackFailureFile.header.eventsDigest);
  check(callbackFailure
            .beginIteration(1u, RenderTapeCompletionPlanRoute::ProductionSlot)
            .accepted() &&
            callbackFailure
                .submit(2u, 1u, actual(1u, 1u, 1u), [] { return false; })
                .disposition == Disposition::CallbackFailure &&
            callbackFailure
                .submit(2u, 1u, actual(1u, 1u, 1u), [] { return true; })
                .disposition == Disposition::Unresolved &&
            callbackFailure.finishIteration().disposition ==
                Disposition::Unresolved,
        "failed production submission latches an unresolved prefix");

  auto staleFile = twoSourcePlan(2u);
  RenderTapeCompletionPlanController stale(staleFile,
                                           staleFile.header.eventsDigest);
  check(stale.beginIteration(1u, RenderTapeCompletionPlanRoute::ProductionSlot)
            .accepted() &&
            stale.submit(2u, 1u, actual(1u, 1u, 1u), [] { return true; })
                .accepted() &&
            stale.beginIteration(1u, RenderTapeCompletionPlanRoute::ProductionSlot)
                .disposition == Disposition::StaleIteration,
        "a repeated active iteration is stale rather than reset implicitly");

  auto activeRoute = twoSourcePlan();
  RenderTapeCompletionPlanController activeRouteController(
      activeRoute, activeRoute.header.eventsDigest);
  check(activeRouteController
            .beginIteration(1u, RenderTapeCompletionPlanRoute::ProductionSlot)
            .accepted() &&
            activeRouteController
                .beginIteration(1u, RenderTapeCompletionPlanRoute::Compatibility)
                .disposition == Disposition::RouteMismatch &&
            activeRouteController
                .beginIteration(1u, RenderTapeCompletionPlanRoute::ProductionSlot)
                .disposition == Disposition::Unresolved,
        "active route changes latch the completion controller terminally");

  auto sourceReuse = twoSourcePlan();
  RenderTapeCompletionPlanController sourceReuseController(
      sourceReuse, sourceReuse.header.eventsDigest);
  check(sourceReuseController
            .beginIteration(1u, RenderTapeCompletionPlanRoute::ProductionSlot)
            .accepted() &&
            sourceReuseController
                .submit(2u, 1u, actual(11u, 3u, 21u), [] { return true; })
                .accepted() &&
            sourceReuseController
                .submit(2u, 2u, actual(12u, 3u, 21u), [] { return true; })
                .disposition == Disposition::DuplicateSubmission,
        "source index and generation cannot be reused at a later sequence");

  auto storageReuse = twoSourcePlan();
  RenderTapeCompletionPlanController storageReuseController(
      storageReuse, storageReuse.header.eventsDigest);
  const auto overlapping = RenderTapeQualifiedCompletionKey{
      .seqId = 12u,
      .sourceIndex = 7u,
      .sourceGeneration = 22u,
      .storageFirstPage = 13u,
      .storagePageCount = 1u,
      .storageGeneration = 121u};
  check(storageReuseController
            .beginIteration(1u, RenderTapeCompletionPlanRoute::ProductionSlot)
            .accepted() &&
            storageReuseController
                .submit(2u, 1u, actual(11u, 3u, 21u), [] { return true; })
                .accepted() &&
            storageReuseController
                .submit(2u, 2u, overlapping, [] { return true; })
                .disposition == Disposition::DuplicateSubmission,
        "same-generation overlapping storage cannot be reused");

  auto overflow = twoSourcePlan();
  RenderTapeCompletionPlanController overflowController(
      overflow, overflow.header.eventsDigest);
  auto overflowing = actual(11u, 3u, 21u);
  overflowing.storageFirstPage = std::numeric_limits<std::uint32_t>::max();
  overflowing.storagePageCount = 2u;
  check(overflowController
            .beginIteration(1u, RenderTapeCompletionPlanRoute::ProductionSlot)
            .accepted() &&
            overflowController
                .submit(2u, 1u, overflowing, [] { return true; })
                .disposition == Disposition::InvalidActualSource,
        "storage range overflow is rejected before production submission");

  auto repeatedPlan = twoSourcePlan(2u);
  RenderTapeCompletionPlanController repeated(
      repeatedPlan, repeatedPlan.header.eventsDigest);
  for (std::uint32_t iteration = 1u; iteration <= 2u; ++iteration) {
    check(repeated.beginIteration(
              iteration, RenderTapeCompletionPlanRoute::ProductionSlot)
              .accepted(),
          "completed iterations advance the shared controller frontier");
    check(repeated.submit(2u, 1u, actual(10u + iteration, 1u, iteration),
                          [] { return true; })
              .accepted() &&
              repeated.submit(2u, 2u, actual(20u + iteration, 2u, iteration),
                              [] { return true; })
                  .accepted() &&
              repeated.endEvent(2u).accepted() &&
              repeated.releaseDue(3u, [](const auto&) { return true; })
                  .accepted() &&
              repeated.endEvent(3u).accepted() &&
              repeated.releaseDue(4u, [](const auto&) { return true; })
                  .accepted() &&
              repeated.endEvent(4u).accepted() &&
              repeated.finishIteration().accepted(),
          "shared controller closes each complete Tape iteration");
  }
}

}  // namespace

int main() {
  try {
    testPlanFileDecodeAndValidation();
    testGlobalBindingPreservesTwoPhysicalSources();
    testRuntimeNegativeControls();
  } catch (const TestFailure& failure) {
    std::cerr << "render_tape_completion_plan_spec: " << failure.what()
              << '\n';
    return 1;
  }
  return 0;
}
