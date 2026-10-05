#include "render_tape_identity_v4_fixture.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace dxmt9::d3d9;
using Status = RenderTapeExecutionTopologyV1Status;
using Outcome = RenderTapeExecutionTopologyV1Outcome;
using Route = RenderTapeExecutionTopologyV1Route;
constexpr auto noIndex = kRenderTapeExecutionTopologyV1NoIndex;
void check(bool value, std::string_view message) {
  if (!value) throw std::runtime_error(std::string(message));
}

void exactPairAndRouteDomain() {
  const auto fixture = test::makeIdentityV4Fixture();
  RenderTapeIdentityV4Status identityStatus{};
  const auto identity = buildRenderTapeIdentityV4(fixture.tape, {}, fixture.identity, &identityStatus);
  check(identityStatus == RenderTapeIdentityV4Status::Valid, "topology fixture authenticates neutral identity");
  for (std::uint32_t variant = 0u; variant < 3u; ++variant) {
    auto topology = fixture.topology;
    if (variant) {
      topology.plans[0].route = static_cast<std::uint32_t>(variant == 1u ? Route::NaturalOrder : Route::PlannerBypass);
      for (std::uint32_t i = 0u; i < topology.placements.size(); ++i) {
        auto& placement = topology.placements[i];
        placement.outcome = static_cast<std::uint32_t>(Outcome::ExecutedWithoutDag);
        placement.executionOrdinal = i; placement.dagPassIndex = noIndex; placement.passKind = 0u;
      }
    }
    Status status{};
    const auto bytes = buildRenderTapeExecutionTopologyV1(fixture.tape, {}, identity, topology, &status);
    RenderTapeExecutionTopologyV1View imported;
    check(status == Status::Valid && !bytes.empty() &&
              validateRenderTapeExecutionTopologyV1(fixture.tape, {}, identity, bytes, &imported) == Status::Valid &&
              imported.header.identityBytes == identity.size() && imported.placements.size() == 3u &&
              imported.plans[0].route == topology.plans[0].route &&
              buildRenderTapeExecutionTopologyV1(fixture.tape, {}, identity, imported, &status) == bytes,
          "companion binds exact identity/tape and keeps optimized, natural and bypass routes distinct");
  }
  auto fragments = fixture.topology;
  for (std::uint32_t i = 0u; i < 3u; ++i) {
    fragments.placements[i].outcome = static_cast<std::uint32_t>(Outcome::ExecutedWithoutDag);
    fragments.placements[i].executionOrdinal = i ? i - 1u : 0u;
    fragments.placements[i].planIndex = i ? 1u : 0u;
    fragments.placements[i].dagPassIndex = noIndex; fragments.placements[i].passKind = 0u;
  }
  fragments.plans = {
      {.planId = 1u, .acceptanceEdge = 14u, .route = static_cast<std::uint32_t>(Route::PlannerBypass),
       .placementCount = 1u, .executedCount = 1u},
      {.planId = 2u, .acceptanceEdge = 15u, .route = static_cast<std::uint32_t>(Route::PlannerBypass),
       .firstPlacement = 1u, .placementCount = 2u, .executedCount = 2u}};
  auto fragmentedIdentity = fixture.identity;
  fragmentedIdentity.sources.erase(fragmentedIdentity.sources.begin() + 1u);
  fragmentedIdentity.sources[0].lifecycle.commandCount = 2u;
  fragmentedIdentity.sources[0].lifecycle.cutEventOrdinal = 4u;
  fragmentedIdentity.sources[0].lifecycle.publicationEdge = 7u;
  fragmentedIdentity.commands[1].sourceIndex = 0u;
  fragmentedIdentity.commands[1].physicalCommandIndex = 1u;
  fragmentedIdentity.commands[2].sourceIndex = 1u;
  check(!buildRenderTapeIdentityV4(fixture.tape, {}, fragmentedIdentity).empty() &&
            validateRenderTapeExecutionTopologyV1Rows(fragmentedIdentity, fragments) == Status::Valid,
        "one source's prefix/suffix retain separate accepted invocations and plan-local execution ordinals");
  auto groups = fragments;
  for (auto& plan : groups.plans) plan.route = static_cast<std::uint32_t>(Route::Optimized);
  for (std::uint32_t i = 0u; i < 2u; ++i) {
    groups.placements[i].outcome = static_cast<std::uint32_t>(Outcome::ExecutedDagPass);
    groups.placements[i].dagPassIndex = 17u; groups.placements[i].passKind = 1u;
  }
  check(validateRenderTapeExecutionTopologyV1Rows(fragmentedIdentity, groups) == Status::Valid &&
            groups.plans[0].planId != groups.plans[1].planId,
        "equal DAG indices in distinct accepted invocations do not establish cross-plan equivalence");
  fragments.plans[1].planId = 1u;
  check(validateRenderTapeExecutionTopologyV1Rows(fixture.identity, fragments) == Status::InvalidPlan,
        "prefix and suffix cannot alias one accepted invocation");
  auto bypass = fixture.topology;
  bypass.plans[0].route = static_cast<std::uint32_t>(Route::PlannerBypass);
  bypass.plans[0].fallbackReason = static_cast<std::uint32_t>(
      RenderTapeExecutionTopologyV1FallbackReason::DceEncodedPrefix);
  for (auto& placement : bypass.placements) {
    placement.outcome = static_cast<std::uint32_t>(Outcome::ExecutedWithoutDag);
    placement.dagPassIndex = noIndex; placement.passKind = 0u;
  }
  check(validateRenderTapeExecutionTopologyV1Rows(fixture.identity, bypass) == Status::Valid,
        "planner bypass preserves supplied predicted order without inventing DAG authority");
}

void placementAndAcceptanceNegatives() {
  const auto fixture = test::makeIdentityV4Fixture();
  for (std::uint32_t mutation = 0u; mutation < 17u; ++mutation) {
    auto topology = fixture.topology;
    auto expected = Status::InvalidPlacement;
    if (mutation == 0u) topology.placements[1].commandIndex = 0u;
    if (mutation == 1u) topology.placements[1].executionOrdinal = 1u;
    if (mutation == 2u) topology.placements[0].executionOrdinal = 3u;
    if (mutation == 3u) topology.placements[0].dagPassIndex = noIndex;
    if (mutation == 4u) topology.placements[0].passKind = 2u;
    if (mutation == 5u) topology.placements[0].planIndex = 1u;
    if (mutation == 6u) { topology.placements.pop_back(); expected = Status::Incomplete; }
    if (mutation == 7u) { ++topology.header.captureEpoch; expected = Status::BindingMismatch; }
    if (mutation == 8u) { ++topology.header.queue; expected = Status::BindingMismatch; }
    if (mutation == 9u) { topology.plans[0].acceptanceEdge = 11u; expected = Status::InvalidPlan; }
    if (mutation == 10u) { topology.plans[0].acceptanceEdge = 16u; expected = Status::InvalidPlan; }
    if (mutation == 11u) { topology.plans[0].acceptanceEdge = 13u; expected = Status::InvalidPlan; }
    if (mutation == 12u) { topology.plans[0].fallbackReason = UINT32_MAX; expected = Status::InvalidPlan; }
    if (mutation == 13u) topology.plans[0].route = static_cast<std::uint32_t>(Route::NaturalOrder);
    if (mutation == 14u) {
      topology.plans[0].route = static_cast<std::uint32_t>(Route::NaturalOrder);
      for (auto& placement : topology.placements) {
        placement.outcome = static_cast<std::uint32_t>(Outcome::ExecutedWithoutDag);
        placement.dagPassIndex = noIndex; placement.passKind = 0u;
      }
    }
    if (mutation == 15u) {
      topology.placements[0].outcome = static_cast<std::uint32_t>(Outcome::ExecutedWithoutDag);
    }
    if (mutation == 16u) {
      topology.placements[2].outcome = static_cast<std::uint32_t>(Outcome::ProvenOmitted);
      topology.placements[2].executionOrdinal = noIndex;
      topology.placements[2].omissionReason = 1u;
      --topology.plans[0].executedCount;
      topology.placements[0].executionOrdinal = 1u;
      topology.placements[1].executionOrdinal = 0u;
    }
    check(validateRenderTapeExecutionTopologyV1Rows(fixture.identity, topology) == expected,
          "duplicate/missing/stale placement, fabricated groups, wrong route/order and invalid acceptance edges reject");
  }
  auto identity = fixture.identity;
  // Both DrawRuns belong to one source to cover mixed executed/omitted
  // membership; physical materialization remains two unique commands.
  identity.sources.erase(identity.sources.begin() + 1u);
  identity.sources[0].lifecycle.commandCount = 2u;
  identity.sources[0].lifecycle.cutEventOrdinal = 4u;
  identity.sources[0].lifecycle.publicationEdge = 7u;
  identity.commands[1].sourceIndex = 0u; identity.commands[1].physicalCommandIndex = 1u;
  identity.commands[2].sourceIndex = 1u;
  RenderTapeIdentityV4Status identityStatus{};
  check(!buildRenderTapeIdentityV4(fixture.tape, {}, identity, &identityStatus).empty() &&
            identityStatus == RenderTapeIdentityV4Status::Valid,
        "omission fixture retains fully validated multi-command source provenance");
  auto omitted = fixture.topology;
  omitted.placements[0].outcome = static_cast<std::uint32_t>(Outcome::ProvenOmitted);
  omitted.placements[0].executionOrdinal = noIndex; omitted.placements[0].dagPassIndex = noIndex;
  omitted.placements[0].passKind = 0u; omitted.placements[0].omissionReason = 1u;
  omitted.placements[1].executionOrdinal = 0u; omitted.placements[2].executionOrdinal = 1u;
  omitted.plans[0].executedCount = 2u;
  check(validateRenderTapeExecutionTopologyV1Rows(identity, omitted) == Status::Valid,
        "proven omission is explicit and distinct from executed no-DAG placement");
  check(validateRenderTapeExecutionTopologyV1Rows(fixture.identity, omitted) == Status::Valid,
        "fully omitted source retains its genuine publication, submission and completion");
  auto inlineIdentity = fixture.identity;
  inlineIdentity.sources[0].lifecycle.submissionEdge = 0u;
  inlineIdentity.sources[0].lifecycle.completionEdge = 0u;
  inlineIdentity.sources[0].lifecycle.completedSequence = 0u;
  check(validateRenderTapeExecutionTopologyV1Rows(inlineIdentity, omitted) == Status::UnsupportedLifecycle,
        "fully omitted inline terminal remains unsupported without actual GPU submission");
  omitted.placements[0].omissionReason = 0u;
  check(validateRenderTapeExecutionTopologyV1Rows(identity, omitted) == Status::InvalidPlacement,
        "an unexplained missing execution is not authenticated optimizer omission");
  auto groupIdentity = fixture.identity;
  groupIdentity.commands[0].kind = static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::SurfaceCopy);
  groupIdentity.commands[1].kind = static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::ColorFill);
  groupIdentity.commands[0].drawCount = groupIdentity.commands[1].drawCount = 0u;
  auto mismatchedGroups = fixture.topology;
  mismatchedGroups.placements[0].passKind = 3u;
  mismatchedGroups.placements[1].passKind = 2u;
  check(validateRenderTapeExecutionTopologyV1Rows(groupIdentity, mismatchedGroups) == Status::InvalidPlacement,
        "one plan-qualified logical DAG group cannot carry conflicting pass kinds");
}

void ordinaryAdmissionEdgeCannotAliasAcceptance() {
  auto fixture = test::makeIdentityV4CoordinatorFixture(TapeCoordinatorAuthority::OrdinarySerial);
  ImportedRenderTapeView original;
  check(importPrevalidatedRenderTape(fixture.tape, original), "ordinary coordinator fixture imports");
  RenderTapeBuilder builder;
  for (std::size_t index = 0u; index < original.events.size(); ++index) {
    const auto event = original.event(index);
    if (event.header.ordinal == 4u)
      builder.appendCommandChunk({.recordCount = 1u}, test::fullStateChunk(true));
    if (event.header.type == static_cast<std::uint32_t>(RenderTapeEventType::PresentComplete)) {
      auto payload = std::vector<std::byte>(event.payload.begin(), event.payload.end());
      RenderTapePresentCompleteHeader completion{};
      std::memcpy(&completion, payload.data(), sizeof(completion));
      completion.presentOrdinal = 7u;
      std::memcpy(payload.data(), &completion, sizeof(completion));
      builder.appendRawEvent(RenderTapeEventType::PresentComplete, payload);
    } else {
      builder.appendRawEvent(static_cast<RenderTapeEventType>(event.header.type), event.payload);
    }
  }
  fixture.tape = builder.seal();
  auto& identity = fixture.identity;
  identity.header.presentOrdinal = 7u;
  for (auto& coordinator : identity.coordinators) ++coordinator.provenance.eventOrdinal;
  for (auto& disposition : identity.dispositions) ++disposition.eventOrdinal;
  for (auto& settlement : identity.settlements) ++settlement.eventOrdinal;
  auto& present = identity.sources[0].lifecycle;
  present.sourceOrdinal = present.seqId = present.writerCaptureId = present.completedSequence = 2u;
  present.cutEventOrdinal = 7u;
  identity.commands[0].sourceIndex = 1u;
  identity.dispositions.back().commandIndex = 1u;
  identity.sources.insert(identity.sources.begin(), {.lifecycle = {
      .sourceOrdinal = 1u, .seqId = 1u, .writerCaptureId = 1u,
      .writerGeneration = 2u, .storageGeneration = 3u,
      .publicationEdge = 15u, .submissionEdge = 60u, .completionEdge = 61u,
      .completedSequence = 1u, .cutEventOrdinal = 4u, .commandCount = 1u,
      .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent)}});
  identity.commands.insert(identity.commands.begin(), {.sourceIndex = 0u,
      .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::DrawRun), .drawCount = 1u});
  identity.dispositions.insert(identity.dispositions.begin(), {.eventOrdinal = 4u, .recordCount = 1u,
      .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::GpuCommand),
      .commandIndex = 0u, .subdrawCount = 1u});
  identity.settlements.insert(identity.settlements.begin(), {4u, 3u, 10u, 1u});
  auto& end = identity.coordinators[1].provenance;
  end.prefixKind = static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::RealSource);
  end.sourceIndex = 0u; end.prefixSequence = end.writerCaptureId = end.commandEnd = 1u;
  end.writerGeneration = 2u; end.storageGeneration = 3u;
  test::setWholeRawCompatibilityAttempts(identity);
  auto& topology = fixture.topology;
  topology.plans.insert(topology.plans.begin(), {.planId = 1u, .acceptanceEdge = 47u,
      .route = static_cast<std::uint32_t>(Route::Optimized), .placementCount = 1u, .executedCount = 1u});
  topology.plans[1].planId = 2u; topology.plans[1].firstPlacement = 1u;
  topology.placements[0].planIndex = topology.placements[0].commandIndex = 1u;
  topology.placements.insert(topology.placements.begin(), {.planIndex = 0u, .commandIndex = 0u,
      .outcome = static_cast<std::uint32_t>(Outcome::ExecutedWithoutDag), .executionOrdinal = 0u});
  RenderTapeIdentityV4Status identityStatus{};
  const auto identityBytes = buildRenderTapeIdentityV4(fixture.tape, {}, identity, &identityStatus);
  Status status{};
  const auto bytes = buildRenderTapeExecutionTopologyV1(fixture.tape, {}, identityBytes, topology, &status);
  check(identityStatus == RenderTapeIdentityV4Status::Valid && !identityBytes.empty() &&
      status == Status::Valid && !bytes.empty() &&
      validateRenderTapeExecutionTopologyV1(fixture.tape, {}, identityBytes, bytes) == Status::Valid,
      "ordinary paired fixture retains authentic END admission and distinct accepted source plans");
  auto collision = topology;
  collision.plans[0].acceptanceEdge = identity.coordinators[1].admissionEdge;
  check(collision.plans[0].acceptanceEdge > identity.sources[0].lifecycle.publicationEdge &&
      collision.plans[0].acceptanceEdge < identity.sources[0].lifecycle.submissionEdge &&
      validateRenderTapeExecutionTopologyV1Rows(identity, collision) == Status::InvalidPlan &&
      buildRenderTapeExecutionTopologyV1(fixture.tape, {}, identityBytes, collision, &status).empty() &&
      status == Status::InvalidPlan,
      "accepted-plan edge cannot alias ordinary admission even inside the otherwise valid source window");
}

void submittedAllOmittedSourceClosure() {
  const auto fixture = test::makeIdentityV4Fixture();
  auto identity = fixture.identity;
  // One real submission/completion closure carries all three sources. Each
  // observation has a distinct capture-local edge; completion covers the
  // actual final source sequence, not a fabricated per-omission terminal.
  for (std::uint32_t i = 0u; i < identity.sources.size(); ++i) {
    auto& source = identity.sources[i].lifecycle;
    source.submissionEdge = 16u + i;
    source.completionEdge = 19u + i;
    source.completedSequence = 3u;
  }
  auto topology = fixture.topology;
  topology.plans = {
      {.planId = 1u, .acceptanceEdge = 14u,
       .route = static_cast<std::uint32_t>(Route::Optimized),
       .placementCount = 2u, .executedCount = 0u},
      {.planId = 2u, .acceptanceEdge = 15u,
       .route = static_cast<std::uint32_t>(Route::PlannerBypass),
       .firstPlacement = 2u, .placementCount = 1u, .executedCount = 1u}};
  for (std::uint32_t i = 0u; i < 2u; ++i) {
    auto& placement = topology.placements[i];
    placement.outcome = static_cast<std::uint32_t>(Outcome::ProvenOmitted);
    placement.executionOrdinal = noIndex;
    placement.dagPassIndex = noIndex;
    placement.passKind = 0u;
    placement.omissionReason = static_cast<std::uint32_t>(
        RenderTapeExecutionTopologyV1OmissionReason::DeadPass);
  }
  topology.placements[2].planIndex = 1u;
  topology.placements[2].executionOrdinal = 0u;
  RenderTapeIdentityV4Status identityStatus{};
  const auto identityBytes = buildRenderTapeIdentityV4(
      fixture.tape, {}, identity, &identityStatus);
  check(identityStatus == RenderTapeIdentityV4Status::Valid && !identityBytes.empty(),
        "submitted all-omitted source closure retains authentic V4 lifecycle and provenance");
  Status status{};
  const auto bytes = buildRenderTapeExecutionTopologyV1(
      fixture.tape, {}, identityBytes, topology, &status);
  RenderTapeExecutionTopologyV1View imported;
  check(validateRenderTapeExecutionTopologyV1Rows(identity, topology) == Status::Valid &&
            status == Status::Valid && !bytes.empty() &&
            validateRenderTapeExecutionTopologyV1(
                fixture.tape, {}, identityBytes, bytes, &imported) == Status::Valid &&
            imported.plans[0].executedCount == 0u &&
            imported.placements[0].outcome == static_cast<std::uint32_t>(Outcome::ProvenOmitted) &&
            imported.placements[1].outcome == static_cast<std::uint32_t>(Outcome::ProvenOmitted),
        "empty selected replay order is valid only with complete omitted coverage and genuine submitted closure");

  for (std::uint32_t mutation = 0u; mutation < 17u; ++mutation) {
    auto changedIdentity = identity;
    auto changedTopology = topology;
    auto expected = Status::InvalidPlacement;
    auto& source = changedIdentity.sources[0].lifecycle;
    auto& placement = changedTopology.placements[0];
    if (mutation == 0u) { source.publicationEdge = 0u; expected = Status::UnsupportedLifecycle; }
    if (mutation == 1u) { source.submissionEdge = 0u; expected = Status::UnsupportedLifecycle; }
    if (mutation == 2u) {
      source.completionEdge = source.completedSequence = 0u;
      expected = Status::UnsupportedLifecycle;
    }
    if (mutation == 3u) {
      source.submissionEdge = source.completionEdge = source.completedSequence = 0u;
      expected = Status::UnsupportedLifecycle;
    }
    if (mutation == 4u) { source.completedSequence = 0u; expected = Status::UnsupportedLifecycle; }
    if (mutation == 5u) { changedTopology.plans[0].acceptanceEdge = 7u; expected = Status::InvalidPlan; }
    if (mutation == 6u) { changedTopology.plans[0].acceptanceEdge = 16u; expected = Status::InvalidPlan; }
    if (mutation == 7u) { changedTopology.plans[0].acceptanceEdge = 13u; expected = Status::InvalidPlan; }
    if (mutation == 8u) placement.omissionReason = 0u;
    if (mutation == 9u) placement.dagPassIndex = 17u;
    if (mutation == 10u) placement.outcome = static_cast<std::uint32_t>(Outcome::ExecutedWithoutDag);
    if (mutation == 11u) changedTopology.plans[0].route = static_cast<std::uint32_t>(Route::NaturalOrder);
    if (mutation == 12u) {
      changedTopology.placements.pop_back();
      expected = Status::Incomplete;
    }
    if (mutation == 13u) changedTopology.placements[1].commandIndex = 0u;
    if (mutation == 14u) {
      changedIdentity.commands[0].sourceIndex = static_cast<std::uint32_t>(identity.sources.size());
      expected = Status::InvalidIdentity;
    }
    if (mutation == 15u) {
      changedIdentity.sources.push_back(identity.sources[0]);
      expected = Status::UnsupportedLifecycle;
    }
    if (mutation == 16u) { source.commandCount = 0u; expected = Status::UnsupportedLifecycle; }
    check(validateRenderTapeExecutionTopologyV1Rows(changedIdentity, changedTopology) == expected,
          "all-omitted sources reject absent/invalid lifecycle, fake acceptance, unsupported omission and incomplete coverage");
  }
}

void wireBindingAndBoundsNegatives() {
  const auto fixture = test::makeIdentityV4Fixture();
  const auto identity = buildRenderTapeIdentityV4(fixture.tape, {}, fixture.identity);
  Status status{};
  const auto original = buildRenderTapeExecutionTopologyV1(fixture.tape, {}, identity, fixture.topology, &status);
  check(status == Status::Valid, "wire topology negatives start fully bound");
  for (std::uint32_t mutation = 0u; mutation < 11u; ++mutation) {
    auto bytes = original;
    RenderTapeExecutionTopologyV1Header header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    auto expected = Status::InvalidHeader;
    if (mutation == 0u) header.magic = kRenderTapeIdentityMagic;
    if (mutation == 1u) header.version = 2u;
    if (mutation == 2u) header.reserved0 = 1u;
    if (mutation == 3u) { ++header.identityBytes; expected = Status::BindingMismatch; }
    if (mutation == 4u) { header.identityDigest[0] ^= std::byte{1}; expected = Status::BindingMismatch; }
    if (mutation == 5u) { header.eventsDigest[0] ^= std::byte{1}; expected = Status::BindingMismatch; }
    if (mutation == 6u) { ++header.placements.offset; expected = Status::InvalidLayout; }
    if (mutation == 7u) { header.plans.count = kRenderTapeIdentityV4MaxRows + 1u; expected = Status::CapacityExceeded; }
    if (mutation == 8u) { ++header.captureToken; expected = Status::BindingMismatch; }
    if (mutation == 9u) { bytes.push_back(std::byte{}); expected = Status::InvalidLayout; }
    if (mutation == 10u) { header.placements.entrySize = 1u; expected = Status::InvalidLayout; }
    std::memcpy(bytes.data(), &header, sizeof(header));
    auto imported = fixture.topology;
    check(validateRenderTapeExecutionTopologyV1(fixture.tape, {}, identity, bytes, &imported) == expected &&
              imported.plans.empty() && imported.placements.empty(),
          "companion exact bytes/digests/key/queue and canonical bounded layout reject without partial output");
  }
  auto freshAttempts = fixture.identity;
  for (auto& attempt : freshAttempts.attempts) attempt.attemptGeneration += 10u;
  RenderTapeIdentityV4Status identityStatus{};
  const auto changedIdentity = buildRenderTapeIdentityV4(fixture.tape, {}, freshAttempts, &identityStatus);
  check(identityStatus == RenderTapeIdentityV4Status::Valid && changedIdentity != identity &&
            validateRenderTapeExecutionTopologyV1(fixture.tape, {}, changedIdentity, original) == Status::BindingMismatch,
        "topology companion authenticates actual attempt bytes as part of the complete V4 identity, not only physical command extents");
  check(!buildRenderTapeExecutionTopologyV1(fixture.tape, {}, changedIdentity, fixture.topology, &status).empty() &&
            status == Status::Valid,
        "new admitted transaction evidence requires a newly bound companion without changing selected plan semantics");
}

} // namespace
int main() {
  try {
    exactPairAndRouteDomain(); placementAndAcceptanceNegatives();
    ordinaryAdmissionEdgeCannotAliasAcceptance();
    submittedAllOmittedSourceClosure(); wireBindingAndBoundsNegatives(); return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "render tape topology v1 spec failed: " << error.what() << '\n'; return 1;
  }
}
