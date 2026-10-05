#include "dxmt9/dxmt9_render_tape_topology_observer.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace dxmt9::render {
struct TapeTopologyCaptureTestAccess {
  static std::array<std::size_t, 16> storage(const TapeTopologyCapture& owner) {
    std::lock_guard lock(owner.mutex_);
    return {owner.sources_.size(), owner.sources_.capacity(),
        owner.commands_.size(), owner.commands_.capacity(),
        owner.plans_.size(), owner.plans_.capacity(),
        owner.placements_.size(), owner.placements_.capacity(),
        owner.pending_.size(), owner.pending_.capacity(),
        owner.pendingCommands_.size(), owner.pendingCommands_.capacity(),
        owner.staged_.size(), owner.staged_.capacity(),
        owner.liveOrder_.size(), owner.liveOrder_.capacity()};
  }
  static std::array<std::uint32_t, 2> sourceRange(
      const TapeTopologyCapture& owner, std::uint32_t index) {
    std::lock_guard lock(owner.mutex_);
    const auto& row = owner.sources_[index];
    return {row.firstCommand, row.identity.commandCount};
  }
};
} // namespace dxmt9::render

namespace {
using namespace dxmt9;
using namespace dxmt9::render;
using Route = d3d9::RenderTapeExecutionTopologyV1Route;
using Reason = d3d9::RenderTapeExecutionTopologyV1FallbackReason;
using Outcome = d3d9::RenderTapeExecutionTopologyV1Outcome;
constexpr TapeTopologyCaptureIdentity capture{7u, 8u, 9u};
constexpr auto noIndex = d3d9::kRenderTapeExecutionTopologyV1NoIndex;
std::uint64_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

TapeTopologySourceIdentity source(std::uint32_t index, std::uint32_t count,
    std::uint64_t edge = 2u) {
  return {.source = {.id = {.index = index, .generation = index + 1u},
      .storage = {.firstPage = index, .pageCount = 1u, .generation = index + 1u}},
      .sourceOrdinal = index + 10u, .sequence = index + 1u,
      .publicationEdge = edge, .sourceIndex = index, .commandCount = count};
}

TapeTopologyPhysicalExtent extent(std::uint32_t index,
    core::MetalCommandKind kind = core::MetalCommandKind::DrawRun) {
  return {index, tapeTopologyCommandKind(kind), kind == core::MetalCommandKind::DrawRun ? 1u : 0u};
}

template <std::size_t Count>
std::array<TapeTopologyCommandRef, Count> refs(TapeTopologyCapture& owner,
    std::uint32_t first = 0u) {
  std::array<TapeTopologyCommandRef, Count> result{};
  for (std::uint32_t index = 0u; index < Count; ++index)
    check(owner.commandRef(first + index, result[index]), "published physical ref is value-owned");
  return result;
}

framegraph::FrameGraph graph(std::span<const std::uint32_t> order,
    std::span<const TapeTopologyCommandRef> commands,
    std::uint32_t dead = noIndex, bool onePass = false) {
  framegraph::FrameGraph value;
  for (const auto index : order) {
    const auto kind = commands[index].extent.kind == tapeTopologyCommandKind(core::MetalCommandKind::DrawRun)
        ? core::MetalCommandKind::DrawRun : core::MetalCommandKind::Clear;
    value.commands.push_back({index, kind});
    if (!onePass) {
      framegraph::PassNode pass{};
      pass.commands = {static_cast<std::uint32_t>(value.commands.size() - 1u), 1u};
      pass.flags.dead = index == dead;
      value.passes.push_back(pass);
    }
  }
  if (onePass) {
    framegraph::PassNode pass{};
    pass.commands = {0u, static_cast<std::uint32_t>(order.size())};
    value.passes.push_back(pass);
  }
  return value;
}

bool encoded(TapeTopologyCapture& owner, std::uint64_t attempt,
    std::span<const TapeTopologyCommandRef> commands,
    std::span<const std::uint32_t> order) {
  std::vector<TapeTopologyCommandRef> execution;
  for (const auto index : order) execution.push_back(commands[index]);
  return owner.observeEncoded({capture, attempt}, execution);
}

void sourceLocalLookupIsExactCopiedAndBounded() {
  TapeTopologyCapture owner;
  auto first = source(0u, 2u, 2u);
  auto firstExtents = std::array{extent(0u), extent(1u, core::MetalCommandKind::Clear)};
  const auto savedFirst = first;
  const auto savedExtents = firstExtents;
  TapeTopologyCommandRef command;
  TapeTopologySourceIdentity identity;
  check(!owner.commandRef(first.source, first.sequence, 0u, command) &&
      !owner.sourceIdentity(first.source, first.sequence, identity) &&
      command == TapeTopologyCommandRef{} && identity == TapeTopologySourceIdentity{},
      "inactive source-local lookup clears both copied outputs");
  check(owner.begin(capture, 1u, {.maxSources = 3u, .maxCommands = 6u, .maxPlans = 3u}),
      "source-local lookup capture reserves bounded source/command rows once");
  const auto reservation = TapeTopologyCaptureTestAccess::storage(owner);
  check(!owner.commandRef(first.source, first.sequence, 0u, command) &&
      !owner.sourceIdentity(first.source, first.sequence, identity) && !owner.failed(),
      "unpublished source is not a physical locator or a capture failure");
  check(owner.publish(first, firstExtents), "first source publishes exact owned physical extents");
  const auto firstCommands = refs<2>(owner);
  const std::array<std::uint32_t, 1> localOrder{0u};
  const auto delayed = owner.stageWithoutDag(Route::NaturalOrder, Reason::None,
      std::span(firstCommands).first(1u), localOrder);
  check(delayed && encoded(owner, delayed, std::span(firstCommands).first(1u), localOrder),
      "old generation's real invocation encodes before delayed registration");
  const auto second = source(1u, 1u, 3u);
  const std::array secondExtents{extent(0u, core::MetalCommandKind::Present)};
  auto reused = source(2u, 3u, 4u);
  reused.source.id.index = savedFirst.source.id.index;
  reused.source.id.generation = 5u;
  reused.source.storage = {.firstPage = 5u, .pageCount = 2u, .generation = 6u};
  const std::array reusedExtents{extent(0u), extent(1u, core::MetalCommandKind::Clear),
      extent(2u, core::MetalCommandKind::GenerateMipmaps)};
  check(owner.publish(second, secondExtents) && owner.publish(reused, reusedExtents),
      "another source and a reused slot's new full generation publish distinct command bases");
  check(TapeTopologyCaptureTestAccess::sourceRange(owner, 0u) == std::array<std::uint32_t, 2>{0u, 2u} &&
      TapeTopologyCaptureTestAccess::sourceRange(owner, 1u) == std::array<std::uint32_t, 2>{2u, 1u} &&
      TapeTopologyCaptureTestAccess::sourceRange(owner, 2u) == std::array<std::uint32_t, 2>{3u, 3u},
      "dense copied source rows own disjoint bounded physical command ranges");
  for (const auto publication : {savedFirst, second, reused}) {
    check(owner.sourceIdentity(publication.source, publication.sequence, identity) && identity == publication,
        "whole-source lookup copies the exact published source identity");
    for (std::uint32_t local = 0u; local < publication.commandCount; ++local) {
      TapeTopologyCommandRef global;
      const auto base = publication.sourceIndex == 0u ? 0u : publication.sourceIndex == 1u ? 2u : 3u;
      check(owner.commandRef(publication.source, publication.sequence, local, command) &&
          owner.commandRef(base + local, global) && command == global &&
          command.owner == publication && command.extent.physicalCommandIndex == local,
          "source-local cursor and preserved global API identify the same immutable physical command");
    }
  }
  first.source.storage.generation = 999u; first.sourceOrdinal = 999u;
  firstExtents[1].kind = UINT32_MAX;
  check(owner.commandRef(savedFirst.source, savedFirst.sequence, 1u, command) &&
      command.owner == savedFirst && command.extent == savedExtents[1],
      "lookup never retains the caller's mutable source or extent span");
  command.owner.sourceOrdinal = 888u; command.extent.kind = UINT32_MAX;
  check(owner.commandRef(savedFirst.source, savedFirst.sequence, 1u, command) &&
      command == firstCommands[1], "mutating a copied lookup result cannot corrupt capture-owned facts");
  const auto lookupStorage = TapeTopologyCaptureTestAccess::storage(owner);
  for (std::uint32_t variant = 0u; variant < 10u; ++variant) {
    auto supplied = savedFirst.source;
    auto sequence = savedFirst.sequence;
    std::uint32_t local = 0u;
    if (variant == 0u) ++supplied.id.generation;
    if (variant == 1u) ++supplied.storage.generation;
    if (variant == 2u) ++supplied.storage.firstPage;
    if (variant == 3u) ++supplied.storage.pageCount;
    if (variant == 4u) ++supplied.id.index;
    if (variant == 5u) sequence = 0u;
    if (variant == 6u) sequence = reused.sequence;
    if (variant == 7u) { supplied = reused.source; sequence = savedFirst.sequence; }
    if (variant == 8u) local = savedFirst.commandCount;
    if (variant == 9u) local = UINT32_MAX;
    command = firstCommands[0];
    check(!owner.commandRef(supplied, sequence, local, command) &&
        command == TapeTopologyCommandRef{} && !owner.failed() &&
        TapeTopologyCaptureTestAccess::storage(owner) == lookupStorage,
        "stale full source/storage, sequence aliases and out-of-range cursors reject without storage mutation");
    if (variant < 8u) {
      identity = savedFirst;
      check(!owner.sourceIdentity(supplied, sequence, identity) &&
          identity == TapeTopologySourceIdentity{}, "failed whole-source lookup exposes no stale output");
    }
  }
  check(owner.sourceIdentity(savedFirst.source, savedFirst.sequence, identity) &&
      owner.accept({capture, delayed, 5u}, std::array{savedFirst}),
      "delayed actual registration preserves the historical published generation after slot reuse");
  TapeTopologyRuntimeSnapshot output;
  check(owner.snapshot(output, false) && !output.complete && output.plans.size() == 1u &&
      output.placements[0].commandIndex == firstCommands[0].commandIndex,
      "source lookup does not invent registration or execution for unrelated new commands");
  const auto after = TapeTopologyCaptureTestAccess::storage(owner);
  for (std::size_t index = 1u; index < reservation.size(); index += 2u)
    check(after[index] == reservation[index], "lookup, publication and delayed acceptance grow no reserved arena capacity");
}

void sourceLocatorPublicationFailureIsTransactional() {
  for (std::uint32_t variant = 0u; variant < 3u; ++variant) {
    TapeTopologyCapture owner;
    check(owner.begin(capture, 1u, {.maxSources = 2u, .maxCommands = 3u, .maxPlans = 1u}),
        "transactional source locator capture begins");
    const auto first = source(0u, 1u, 2u);
    check(owner.publish(first, std::array{extent(0u)}), "initial source range publishes");
    const auto before = TapeTopologyCaptureTestAccess::storage(owner);
    const auto range = TapeTopologyCaptureTestAccess::sourceRange(owner, 0u);
    auto next = source(1u, variant == 0u ? 3u : 2u, 3u);
    std::array candidate{extent(0u), extent(1u), extent(2u)};
    auto extents = std::span<const TapeTopologyPhysicalExtent>(candidate);
    if (variant != 0u) extents = extents.first(2u);
    if (variant == 1u) candidate[1].physicalCommandIndex = 9u;
    if (variant == 2u) next.sequence = first.sequence;
    check(!owner.publish(next, extents) && owner.failed() &&
        TapeTopologyCaptureTestAccess::storage(owner) == before &&
        TapeTopologyCaptureTestAccess::sourceRange(owner, 0u) == range,
        "capacity, malformed local extent and aliased publication sequence expose no partial source/base/command row");
    TapeTopologyCommandRef command;
    TapeTopologySourceIdentity identity;
    check(!owner.commandRef(first.source, first.sequence, 0u, command) &&
        !owner.sourceIdentity(first.source, first.sequence, identity) &&
        command == TapeTopologyCommandRef{} && identity == TapeTopologySourceIdentity{},
        "failed capture cannot issue even previously published source locators");
  }
  TapeTopologyCapture revoked;
  check(revoked.begin(capture, 1u), "revoked source locator begins");
  const auto publication = source(0u, 1u);
  check(revoked.publish(publication, std::array{extent(0u)}), "revoked source locator publishes");
  revoked.revoke();
  TapeTopologyCommandRef command;
  TapeTopologySourceIdentity identity;
  check(!revoked.commandRef(publication.source, publication.sequence, 0u, command) &&
      !revoked.sourceIdentity(publication.source, publication.sequence, identity) &&
      command == TapeTopologyCommandRef{} && identity == TapeTopologySourceIdentity{},
      "revocation invalidates exact source locators without returning old copied authority");
}

void publicationAndStagingAreNotAcceptance() {
  TapeTopologyCapture owner;
  check(owner.begin(capture, 1u), "cold topology capture begins");
  const auto publication = source(0u, 2u);
  const std::array extents{extent(0u), extent(1u)};
  check(owner.publish(publication, extents), "actual immutable publication registered");
  TapeTopologyRuntimeSnapshot output;
  check(!owner.snapshot(output) && output.plans.empty() &&
      owner.snapshot(output, false) && output.placements.empty(),
      "publication alone remains pending, not invented pass membership");
  auto commands = refs<2>(owner);
  std::array<std::uint32_t, 2> order{0u, 1u};
  const auto attempt = owner.stageWithoutDag(Route::NaturalOrder, Reason::FeaturesOff, commands, order);
  check(attempt && !owner.snapshot(output) && owner.snapshot(output, false) && output.plans.empty(),
      "a selected candidate still has no authenticated topology");
  const auto actualCommands = commands;
  const auto actualOrder = order;
  commands[0].owner.source.storage.generation = 999u;
  order[0] = 999u;
  const std::array registered{publication};
  check(owner.accept({capture, attempt, 3u}, registered) && !owner.snapshot(output) &&
      owner.snapshot(output, false) && !output.complete && output.plans.empty(),
      "registration alone cannot label unexecuted commands as executed");
  check(encoded(owner, attempt, actualCommands, actualOrder) && owner.snapshot(output) && output.complete,
      "execution consumes copied values, never retained caller spans");
  check(output.plans.size() == 1u && output.placements.size() == 2u &&
      output.plans[0].acceptanceEdge == 3u, "acceptance is a separate owned invocation");
  for (const auto& row : output.placements)
    check(row.outcome == static_cast<std::uint32_t>(Outcome::ExecutedWithoutDag) &&
        row.dagPassIndex == noIndex && !row.passKind && !row.omissionReason,
        "natural execution has no fabricated DAG or no-pass classification");
}

void discardedGraphCannotContributeMembership() {
  TapeTopologyCapture owner;
  check(owner.begin(capture, 1u), "rollback capture begins");
  const auto publication = source(0u, 2u);
  const std::array extents{extent(0u), extent(1u)};
  check(owner.publish(publication, extents), "rollback publication registered");
  const auto commands = refs<2>(owner);
  const std::array<std::uint32_t, 2> reordered{1u, 0u};
  auto candidate = graph(reordered, commands, noIndex, true);
  const auto discarded = owner.stageSelectedDag(Reason::None, candidate, commands, reordered);
  check(discarded && owner.reject(discarded), "actual rollback discards only the staged candidate");
  candidate.reset();
  TapeTopologyRuntimeSnapshot output;
  check(owner.snapshot(output, false) && output.plans.empty() && output.placements.empty(),
      "rejected graph emits no pass evidence");
  const std::array<std::uint32_t, 2> natural{0u, 1u};
  const auto accepted = owner.stageWithoutDag(Route::NaturalOrder, Reason::FrontierRollback, commands, natural);
  check(accepted > discarded && owner.accept({capture, accepted, 3u}, std::array{publication}) &&
      encoded(owner, accepted, commands, natural) && owner.snapshot(output),
      "natural fallback is a distinct actual invocation");
  check(output.plans[0].planId == accepted && output.plans[0].route == static_cast<std::uint32_t>(Route::NaturalOrder) &&
      output.placements[0].outcome == static_cast<std::uint32_t>(Outcome::ExecutedWithoutDag),
      "fallback does not reuse discarded optimized identity");
  check(!owner.accept({capture, discarded, 4u}, std::array{publication}) && owner.failed(),
      "rejected attempt cannot later authenticate");
}

void selectedOrderAndOmissionAreIndependentOfMaterialization() {
  TapeTopologyCapture owner;
  check(owner.begin(capture, 1u), "DCE capture begins");
  const auto publication = source(0u, 3u);
  const std::array extents{extent(0u), extent(1u), extent(2u, core::MetalCommandKind::Clear)};
  check(owner.publish(publication, extents), "all physical commands stay materialized");
  const auto commands = refs<3>(owner);
  const std::array<std::uint32_t, 3> selected{0u, 2u, 1u};
  const std::array<std::uint32_t, 2> actual{2u, 1u};
  auto selectedGraph = graph(selected, commands, 0u);
  const auto attempt = owner.stageSelectedDag(Reason::None, selectedGraph, commands, actual);
  selectedGraph.reset();
  check(attempt && owner.accept({capture, attempt, 3u}, std::array{publication}),
      "only the selected live order and proven dead commands are admitted");
  check(encoded(owner, attempt, commands, actual), "actual DCE invocation encoded the live suffix only");
  TapeTopologyRuntimeSnapshot output;
  check(owner.snapshot(output) && output.placements.size() == 3u &&
      output.plans[0].executedCount == 2u, "omitted commands retain exact terminal coverage");
  const auto& omitted = output.placements[0];
  check(omitted.commandIndex == 0u && omitted.outcome == static_cast<std::uint32_t>(Outcome::ProvenOmitted) &&
      omitted.executionOrdinal == noIndex && omitted.dagPassIndex == noIndex && !omitted.passKind &&
      omitted.omissionReason == static_cast<std::uint32_t>(d3d9::RenderTapeExecutionTopologyV1OmissionReason::DeadPass),
      "omission is not relabelled executed without a pass");
  check(output.placements[1].commandIndex == 1u && output.placements[1].executionOrdinal == 1u &&
      output.placements[2].commandIndex == 2u && output.placements[2].executionOrdinal == 0u,
      "accepted execution order is independent of physical-table order");
}

void combinedPassUsesOneAcceptedInvocation() {
  TapeTopologyCapture owner;
  check(owner.begin(capture, 1u), "combined capture begins");
  const auto first = source(0u, 1u, 2u);
  const auto second = source(1u, 1u, 3u);
  const std::array extents{extent(0u)};
  check(owner.publish(first, extents) && owner.publish(second, extents), "distinct actual sources registered");
  const auto commands = refs<2>(owner);
  const std::array<std::uint32_t, 2> order{1u, 0u};
  const auto combined = graph(order, commands, noIndex, true);
  const auto attempt = owner.stageSelectedDag(Reason::ExternalCommittedPlan, combined, commands, order);
  check(attempt && owner.accept({capture, attempt, 4u}, std::array{first, second}),
      "accepted combined plan owns the cross-source grouping");
  check(encoded(owner, attempt, commands, order), "combined registration precedes successful encoding");
  TapeTopologyRuntimeSnapshot output;
  check(owner.snapshot(output) && output.plans.size() == 1u &&
      output.placements[0].planIndex == output.placements[1].planIndex &&
      output.placements[0].dagPassIndex == output.placements[1].dagPassIndex,
      "cross-source equivalence is one plan-qualified DAG key, not physical encoder identity");
}

void prefixAndSuffixAreSeparateInvocations() {
  TapeTopologyCapture owner;
  check(owner.begin(capture, 1u), "prefix capture begins");
  const auto publication = source(0u, 3u);
  const std::array extents{extent(0u), extent(1u), extent(2u)};
  check(owner.publish(publication, extents), "prefix source registered once");
  const auto commands = refs<3>(owner);
  const std::array<std::uint32_t, 1> prefixOrder{0u};
  const auto prefix = owner.stageWithoutDag(Route::PlannerBypass, Reason::DceEncodedPrefix,
      std::span(commands).first(1u), prefixOrder);
  check(prefix && encoded(owner, prefix, std::span(commands).first(1u), prefixOrder),
      "actual DCE prefix encoded before source registration");
  const std::array<std::uint32_t, 3> fullOrder{0u, 2u, 1u};
  const std::array<std::uint32_t, 2> suffixOrder{2u, 1u};
  const auto selected = graph(fullOrder, commands, noIndex, true);
  const auto suffix = owner.stageSelectedDag(Reason::None, selected, commands, suffixOrder, prefixOrder);
  TapeTopologyRuntimeSnapshot pending;
  check(suffix > prefix && encoded(owner, suffix, commands, suffixOrder) &&
      !owner.snapshot(pending) && owner.snapshot(pending, false) && pending.plans.empty(),
      "both encoded invocations remain unauthenticated until actual registration");
  const std::array receipts{TapeTopologyAcceptedInvocation{prefix, 3u},
      TapeTopologyAcceptedInvocation{suffix, 4u}};
  check(owner.acceptBatch(capture, receipts, std::array{publication}),
      "one actual source receipt accepts the exact deferred invocation prefix");
  TapeTopologyRuntimeSnapshot output;
  check(owner.snapshot(output) && output.plans.size() == 2u && output.placements.size() == 3u &&
      output.plans[0].planId != output.plans[1].planId &&
      output.placements[0].outcome == static_cast<std::uint32_t>(Outcome::ExecutedWithoutDag) &&
      output.placements[0].dagPassIndex == noIndex && output.placements[1].planIndex == 1u,
      "final graph never retrospectively regroups the already encoded prefix");
}

void wrongAuthorityAndMalformedSchedulesReject() {
  for (std::uint32_t variant = 0u; variant < 4u; ++variant) {
    TapeTopologyCapture owner;
    check(owner.begin(capture, 1u), "negative capture begins");
    const auto publication = source(0u, 2u);
    const std::array extents{extent(0u), extent(1u)};
    check(owner.publish(publication, extents), "negative publication registered");
    auto commands = refs<2>(owner);
    std::array<std::uint32_t, 2> order{0u, 1u};
    if (variant == 0u) commands[0].owner.source.id.generation += 1u;
    if (variant == 1u) order = {1u, 1u};
    const auto attempt = owner.stageWithoutDag(Route::NaturalOrder, Reason::None, commands, order);
    if (variant < 2u) {
      check(!attempt && owner.failed(), "stale extent or duplicate schedule fails closed");
    } else {
      auto acceptance = TapeTopologyAcceptanceIdentity{capture, attempt, 3u};
      auto registered = publication;
      if (variant == 2u) ++acceptance.capture.epoch;
      if (variant == 3u) ++registered.source.storage.generation;
      check(!owner.accept(acceptance, std::array{registered}) && owner.failed(),
          "foreign capture or stale registration cannot promote a stage");
    }
    TapeTopologyRuntimeSnapshot output;
    check(!owner.snapshot(output, false) && output.plans.empty(), "invalid capture publishes no accepted evidence");
  }
  TapeTopologyCapture owner;
  check(owner.begin(capture, 1u), "graph-order negative begins");
  const auto publication = source(0u, 2u);
  const std::array extents{extent(0u), extent(1u)};
  check(owner.publish(publication, extents), "graph-order negative publishes");
  const auto commands = refs<2>(owner);
  const std::array<std::uint32_t, 2> selected{1u, 0u}, wrong{0u, 1u};
  const auto candidate = graph(selected, commands);
  check(!owner.stageSelectedDag(Reason::None, candidate, commands, wrong) && owner.failed(),
      "candidate graph cannot authenticate a different actual command order");
}

void rejectedCapacityDoesNotGrowStorage() {
  TapeTopologyCapture owner;
  check(owner.begin(capture, 1u, {.maxSources = 1u, .maxCommands = 1u, .maxPlans = 1u}),
      "small bounded ledger begins");
  const auto before = TapeTopologyCaptureTestAccess::storage(owner);
  const std::array extents{extent(0u), extent(1u)};
  check(!owner.publish(source(0u, 2u), extents) && owner.failed() &&
      TapeTopologyCaptureTestAccess::storage(owner) == before,
      "oversized publication rejects before changing storage size or capacity");

  TapeTopologyCapture graphOwner;
  check(graphOwner.begin(capture, 1u, {.maxSources = 1u, .maxCommands = 2u,
      .maxPlans = 1u, .maxGraphPasses = 1u, .maxGraphCommandRefs = 2u}),
      "small graph budget begins");
  check(graphOwner.publish(source(0u, 2u), extents), "bounded graph source publishes");
  const auto commands = refs<2>(graphOwner);
  const std::array<std::uint32_t, 2> order{0u, 1u};
  const auto candidate = graph(order, commands);
  const auto graphBefore = TapeTopologyCaptureTestAccess::storage(graphOwner);
  check(!graphOwner.stageSelectedDag(Reason::None, candidate, commands, order) &&
      TapeTopologyCaptureTestAccess::storage(graphOwner) == graphBefore,
      "oversized candidate graph rejects before staging size or capacity changes");

  TapeTopologyCapture pendingOwner;
  check(pendingOwner.begin(capture, 1u, {.maxSources = 1u, .maxCommands = 3u, .maxPlans = 2u}),
      "multiple-invocation plan budget begins");
  const std::array threeExtents{extent(0u), extent(1u), extent(2u)};
  check(pendingOwner.publish(source(0u, 3u), threeExtents), "three physical commands publish");
  const auto pendingCommands = refs<3>(pendingOwner);
  const std::array<std::uint32_t, 1> singleton{0u};
  check(pendingOwner.stageWithoutDag(Route::NaturalOrder, Reason::None,
      std::span(pendingCommands).subspan(0u, 1u), singleton) &&
      pendingOwner.stageWithoutDag(Route::NaturalOrder, Reason::None,
          std::span(pendingCommands).subspan(1u, 1u), singleton),
      "two deferred invocations coexist without registration or execution");
  const auto pendingBefore = TapeTopologyCaptureTestAccess::storage(pendingOwner);
  check(!pendingOwner.stageWithoutDag(Route::NaturalOrder, Reason::None,
      std::span(pendingCommands).subspan(2u, 1u), singleton) &&
      TapeTopologyCaptureTestAccess::storage(pendingOwner) == pendingBefore,
      "a third pending invocation rejects before growing or modifying any arena");
}

void repeatedPrefixesStoreOnlyNewPhysicalReferences() {
  TapeTopologyCapture owner;
  check(owner.begin(capture, 1u, {.maxSources = 1u, .maxCommands = 3u, .maxPlans = 3u}),
      "repeated prefix bounded capture begins");
  const auto publication = source(0u, 3u);
  const std::array extents{extent(0u), extent(1u), extent(2u)};
  check(owner.publish(publication, extents), "repeated prefix physical table publishes");
  const auto commands = refs<3>(owner);
  const auto before = TapeTopologyCaptureTestAccess::storage(owner);
  const std::array<std::uint32_t, 1> firstOrder{0u}, secondOrder{1u}, thirdOrder{2u};
  const std::array<std::uint32_t, 2> pairOrder{0u, 1u};
  const std::array<std::uint32_t, 3> fullOrder{0u, 1u, 2u};
  const auto first = owner.stageWithoutDag(Route::PlannerBypass, Reason::DceEncodedPrefix,
      std::span(commands).first(1u), firstOrder);
  check(first && encoded(owner, first, commands, firstOrder), "first unregistered prefix executes");
  const auto pairGraph = graph(pairOrder, std::span(commands).first(2u));
  const auto second = owner.stageSelectedDag(Reason::None, pairGraph,
      std::span(commands).first(2u), secondOrder, firstOrder);
  check(second && encoded(owner, second, commands, secondOrder), "second unregistered suffix executes");
  const auto fullGraph = graph(fullOrder, commands);
  const auto third = owner.stageSelectedDag(Reason::None, fullGraph, commands, thirdOrder, pairOrder);
  check(third && encoded(owner, third, commands, thirdOrder), "third invocation proves the cumulative prefix");
  const auto staged = TapeTopologyCaptureTestAccess::storage(owner);
  check(staged[8u] == 3u && staged[10u] == 3u,
      "three repeated graph snapshots own exactly three new physical references");
  for (std::size_t index = 1u; index < before.size(); index += 2u)
    check(staged[index] == before[index], "all staged arena capacities remain the begin reservation");
  const std::array receipts{TapeTopologyAcceptedInvocation{first, 3u},
      TapeTopologyAcceptedInvocation{second, 4u}, TapeTopologyAcceptedInvocation{third, 5u}};
  TapeTopologyRuntimeSnapshot output;
  check(owner.acceptBatch(capture, receipts, std::array{publication}) && owner.snapshot(output) &&
      output.plans.size() == 3u && output.placements.size() == 3u,
      "aggregate registration promotes exactly the invocation-local terminal facts");
  const auto after = TapeTopologyCaptureTestAccess::storage(owner);
  for (std::size_t index = 1u; index < before.size(); index += 2u)
    check(after[index] == before[index], "terminal publication grows no arena capacity");
}

void registrationBeforeExecutionPromotesOnlyTerminalPrefix() {
  TapeTopologyCapture owner;
  check(owner.begin(capture, 1u), "registration-first deferred capture begins");
  const auto publication = source(0u, 2u);
  const std::array extents{extent(0u), extent(1u)};
  check(owner.publish(publication, extents), "registration-first physical table publishes");
  const auto commands = refs<2>(owner);
  const std::array<std::uint32_t, 1> localOrder{0u}, secondOrder{1u};
  const auto first = owner.stageWithoutDag(Route::NaturalOrder, Reason::None,
      std::span(commands).first(1u), localOrder);
  const auto second = owner.stageWithoutDag(Route::NaturalOrder, Reason::None,
      std::span(commands).subspan(1u), localOrder);
  const std::array receipts{TapeTopologyAcceptedInvocation{first, 3u},
      TapeTopologyAcceptedInvocation{second, 4u}};
  TapeTopologyRuntimeSnapshot output;
  check(first && second && owner.acceptBatch(capture, receipts, std::array{publication}) &&
      !owner.snapshot(output) && owner.snapshot(output, false) && output.plans.empty() && !output.complete,
      "early aggregate registration retains Encode handles without claiming execution");
  check(encoded(owner, first, commands, localOrder) && !owner.snapshot(output) &&
      owner.snapshot(output, false) && output.plans.size() == 1u &&
      output.placements.size() == 1u && !output.complete,
      "successful first Encode publishes only the terminal chronological prefix");
  check(encoded(owner, second, commands, secondOrder) && owner.snapshot(output) &&
      output.complete && output.plans.size() == 2u && output.placements.size() == 2u,
      "successful later Encode finishes the accepted topology exactly once");
}

void cumulativePrefixMustPreserveObservedInvocationOrder() {
  for (std::uint32_t variant = 0u; variant < 2u; ++variant) {
    TapeTopologyCapture owner;
    check(owner.begin(capture, 1u), "chronological prefix negative begins");
    const auto publication = source(0u, 3u);
    const std::array extents{extent(0u), extent(1u), extent(2u)};
    check(owner.publish(publication, extents), "chronological prefix physical table publishes");
    const auto commands = refs<3>(owner);
    const std::array<std::uint32_t, 1> localOrder{0u};
    if (variant == 0u) {
      const auto first = owner.stageWithoutDag(Route::PlannerBypass, Reason::DceEncodedPrefix,
          std::span(commands).first(2u), std::array<std::uint32_t, 2>{0u, 1u});
      check(first && encoded(owner, first, commands, std::array<std::uint32_t, 2>{0u, 1u}),
          "one invocation executes its exact two-command order");
    } else {
      for (std::uint32_t index = 0u; index < 2u; ++index) {
        const auto attempt = owner.stageWithoutDag(Route::PlannerBypass, Reason::DceEncodedPrefix,
            std::span(commands).subspan(index, 1u), localOrder);
        check(attempt && encoded(owner, attempt, commands, std::array{index}),
            "separate invocations execute in chronological order");
      }
    }
    const std::array<std::uint32_t, 3> wrongOrder{1u, 0u, 2u};
    const auto selected = graph(wrongOrder, commands, noIndex, true);
    check(!owner.stageSelectedDag(Reason::None, selected, commands,
        std::array<std::uint32_t, 1>{2u}, std::array<std::uint32_t, 2>{1u, 0u}) && owner.failed(),
        "matching graph prefix membership cannot reverse observed execution chronology");
  }
}

void batchAcceptanceIsTransactionalAndSourceExact() {
  for (std::uint32_t variant = 0u; variant < 11u; ++variant) {
    TapeTopologyCapture owner;
    check(owner.begin(capture, 1u, {.maxSources = 2u, .maxCommands = 2u, .maxPlans = 2u}),
        "batch negative capture begins");
    const auto firstSource = source(0u, 1u, 2u), secondSource = source(1u, 1u, 3u);
    const std::array extents{extent(0u)};
    check(owner.publish(firstSource, extents) && owner.publish(secondSource, extents),
        "batch source union publishes");
    const auto commands = refs<2>(owner);
    const std::array<std::uint32_t, 1> localOrder{0u};
    const auto first = owner.stageWithoutDag(Route::NaturalOrder, Reason::None,
        std::span(commands).first(1u), localOrder);
    const auto second = owner.stageWithoutDag(Route::NaturalOrder, Reason::None,
        std::span(commands).subspan(1u), localOrder);
    check(first && second, "two distinct pending invocation identities exist");
    auto suppliedCapture = capture;
    std::array receipts{TapeTopologyAcceptedInvocation{first, 4u},
        TapeTopologyAcceptedInvocation{second, 5u}};
    std::array suppliedSources{firstSource, secondSource};
    std::span<const TapeTopologyAcceptedInvocation> batch = receipts;
    std::span<const TapeTopologySourceIdentity> registered = suppliedSources;
    if (variant == 0u) receipts[1u].attempt = first;
    if (variant == 1u) std::swap(receipts[0u].attempt, receipts[1u].attempt);
    if (variant == 2u) batch = std::span(receipts).subspan(1u);
    if (variant == 3u) ++suppliedCapture.epoch;
    if (variant == 4u) ++suppliedSources[1u].source.storage.generation;
    if (variant == 5u) registered = std::span(suppliedSources).first(1u);
    if (variant == 6u) batch = std::span(receipts).first(1u);
    if (variant == 7u) suppliedSources[1u] = firstSource;
    if (variant == 8u) receipts[1u].edge = receipts[0u].edge;
    if (variant == 9u) receipts[0u].edge = secondSource.publicationEdge;
    if (variant == 10u) ++suppliedCapture.queue;
    const auto before = TapeTopologyCaptureTestAccess::storage(owner);
    check(!owner.acceptBatch(suppliedCapture, batch, registered) && owner.failed() &&
        TapeTopologyCaptureTestAccess::storage(owner) == before,
        "malformed aggregate receipt poisons capture without partial row or storage mutation");
    TapeTopologyRuntimeSnapshot output;
    check(!owner.snapshot(output, false) && output.plans.empty() && !output.complete,
        "no part of a rejected batch becomes authentic topology");
  }
}

void executionPrefixAndEffectFailureBoundaries() {
  for (std::uint32_t variant = 0u; variant < 5u; ++variant) {
    TapeTopologyCapture owner;
    check(owner.begin(capture, 1u), "effect-boundary capture begins");
    const auto publication = source(0u, 2u);
    const std::array extents{extent(0u), extent(1u)};
    check(owner.publish(publication, extents), "effect-boundary physical table publishes");
    const auto commands = refs<2>(owner);
    const std::array<std::uint32_t, 1> firstOrder{0u}, secondOrder{1u};
    const std::array<std::uint32_t, 2> fullOrder{0u, 1u};
    const auto first = owner.stageWithoutDag(Route::PlannerBypass, Reason::DceEncodedPrefix,
        std::span(commands).first(1u), firstOrder);
    check(first != 0u, "effect-boundary prefix stages");
    if (variant == 0u) {
      const auto selected = graph(fullOrder, commands);
      check(!owner.stageSelectedDag(Reason::None, selected, commands, secondOrder, firstOrder),
          "selected but unexecuted prefix cannot justify suffix reuse");
    } else if (variant == 1u) {
      const auto second = owner.stageWithoutDag(Route::NaturalOrder, Reason::None,
          std::span(commands).subspan(1u), firstOrder);
      check(second && !encoded(owner, second, commands, secondOrder),
          "invocation execution cannot skip an earlier unexecuted candidate");
    } else if (variant == 2u) {
      check(owner.accept({capture, first, 3u}, std::array{publication}) && !owner.reject(first),
          "rollback after actual registration is fail-stop even before Encode");
    } else if (variant == 3u) {
      check(encoded(owner, first, commands, firstOrder) && !owner.reject(first),
          "rollback after successful Encode is fail-stop before registration");
    } else {
      auto stale = commands[0u];
      ++stale.owner.source.id.generation;
      check(!owner.observeEncoded({capture, first}, std::array{stale}),
          "execution observation rechecks source generation and immutable extent");
    }
    TapeTopologyRuntimeSnapshot output;
    check(owner.failed() && !owner.snapshot(output, false), "effect failure never qualifies a partial capture");
  }
}

void rejectingUneffectedCandidatePreservesIndependentStages() {
  TapeTopologyCapture owner;
  check(owner.begin(capture, 1u), "independent rollback capture begins");
  const auto publication = source(0u, 2u);
  const std::array extents{extent(0u), extent(1u)};
  check(owner.publish(publication, extents), "independent rollback physical table publishes");
  const auto commands = refs<2>(owner);
  const std::array<std::uint32_t, 1> localOrder{0u}, secondOrder{1u};
  const auto discarded = owner.stageWithoutDag(Route::NaturalOrder, Reason::None,
      std::span(commands).first(1u), localOrder);
  const auto preserved = owner.stageWithoutDag(Route::NaturalOrder, Reason::None,
      std::span(commands).subspan(1u), localOrder);
  check(discarded && preserved && owner.reject(discarded) && encoded(owner, preserved, commands, secondOrder) &&
      owner.accept({capture, preserved, 3u}, std::array{publication}),
      "pre-effect rejection rebases the independent later invocation without invention");
  const auto replacement = owner.stageWithoutDag(Route::NaturalOrder, Reason::FrontierRollback,
      std::span(commands).first(1u), localOrder);
  TapeTopologyRuntimeSnapshot output;
  check(replacement > preserved && owner.accept({capture, replacement, 4u}, std::array{publication}) &&
      encoded(owner, replacement, commands, localOrder) && owner.snapshot(output) &&
      output.plans.size() == 2u && output.plans[0u].planId == preserved &&
      output.placements[0u].commandIndex == 1u && output.placements[1u].commandIndex == 0u,
      "replacement preserves actual chronological invocation order rather than physical command order");
}

void revokeInvalidatesAllPendingHandles() {
  TapeTopologyCapture owner;
  check(owner.begin(capture, 1u), "revoke capture begins");
  const auto publication = source(0u, 1u);
  const std::array extents{extent(0u)};
  check(owner.publish(publication, extents), "revoke physical table publishes");
  const auto commands = refs<1>(owner);
  const std::array<std::uint32_t, 1> order{0u};
  const auto attempt = owner.stageWithoutDag(Route::NaturalOrder, Reason::None, commands, order);
  check(attempt != 0u, "revoke pending handle exists");
  owner.revoke();
  TapeTopologyRuntimeSnapshot output;
  check(!encoded(owner, attempt, commands, order) &&
      !owner.accept({capture, attempt, 3u}, std::array{publication}) &&
      !owner.snapshot(output, false) && !owner.begin(capture, 1u) && owner.failed(),
      "revoked capture and stale attempt can never regain authority");
}

void exhaustiveTwoCommandLifecycle() {
  constexpr std::uint32_t actionCount = 9u;
  constexpr std::uint32_t sequenceCount = actionCount * actionCount * actionCount * actionCount;
  for (std::uint32_t sequence = 0u; sequence < sequenceCount; ++sequence) {
    TapeTopologyCapture owner;
    check(owner.begin(capture, 1u, {.maxSources = 1u, .maxCommands = 2u, .maxPlans = 2u}),
        "exhaustive lifecycle capture begins");
    const auto publication = source(0u, 2u);
    const std::array extents{extent(0u), extent(1u)};
    check(owner.publish(publication, extents), "exhaustive physical extents publish");
    const auto commands = refs<2>(owner);
    const std::array<std::uint32_t, 2> natural{0u, 1u}, reverse{1u, 0u};
    std::uint64_t liveAttempt = 0u;
    std::uint64_t rejectedAttempt = 0u;
    bool pending = false, accepted = false, observed = false, poisoned = false;
    bool reversed = false;
    std::uint32_t remaining = sequence;
    for (std::uint32_t step = 0u; step < 4u; ++step) {
      const auto action = remaining % actionCount;
      remaining /= actionCount;
      const bool permitted = !poisoned && (action < 2u ? !pending :
          action == 2u ? pending && !accepted : action == 3u ? pending && !observed :
          action == 4u ? pending && !accepted && !observed : false);
      bool actual = false;
      if (action < 2u) {
        const auto attempt = owner.stageWithoutDag(
            action == 0u ? Route::NaturalOrder : Route::PlannerBypass,
            action == 0u ? Reason::None : Reason::ExplicitSkip,
            commands, action == 0u ? natural : reverse);
        actual = attempt != 0u;
        if (actual) {
          liveAttempt = attempt;
          pending = true;
          reversed = action == 1u;
          accepted = observed = false;
        }
      } else if (action == 2u) {
        actual = owner.accept({capture, liveAttempt, step + 3u}, std::array{publication});
        if (actual) accepted = true;
      } else if (action == 3u) {
        actual = encoded(owner, liveAttempt, commands, reversed ? reverse : natural);
        if (actual) observed = true;
      } else if (action == 4u) {
        actual = owner.reject(liveAttempt);
        if (actual) { pending = false; rejectedAttempt = liveAttempt; }
      } else if (action == 5u) {
        auto stale = capture;
        ++stale.epoch;
        actual = owner.accept({stale, liveAttempt, step + 3u}, std::array{publication});
      } else if (action == 6u) {
        actual = owner.accept({capture, rejectedAttempt, step + 3u}, std::array{publication});
      } else if (action == 7u) {
        auto stale = capture;
        ++stale.epoch;
        actual = owner.observeEncoded({stale, liveAttempt}, commands);
      } else {
        actual = encoded(owner, liveAttempt, commands, reversed ? natural : reverse);
      }
      check(actual == permitted, "finite registration/execution/reject sequence obeys independent exactly-once facts");
      if (!permitted) poisoned = true;
      TapeTopologyRuntimeSnapshot output;
      const bool complete = owner.snapshot(output);
      const bool terminal = pending && accepted && observed;
      check(complete == (!poisoned && terminal),
          "no finite sequence qualifies without both registration and execution");
      check(owner.snapshot(output, false) == !poisoned,
          "poisoned finite sequence exposes no authenticated partial evidence");
      if (!poisoned)
        check(output.complete == terminal && output.plans.size() == (terminal ? 1u : 0u) &&
            output.placements.size() == (terminal ? 2u : 0u),
            "finite sequence never duplicates physical-command placement");
    }
  }
}

void exhaustivePrefixCannotBeReassigned() {
  for (std::uint32_t acceptedPhysical = 0u; acceptedPhysical < 2u; ++acceptedPhysical)
    for (std::uint32_t graphHead = 0u; graphHead < 2u; ++graphHead)
      for (std::uint32_t claimedPrefix = 0u; claimedPrefix < 2u; ++claimedPrefix)
        for (std::uint32_t actualSuffix = 0u; actualSuffix < 2u; ++actualSuffix) {
          TapeTopologyCapture owner;
          check(owner.begin(capture, 1u, {.maxSources = 1u, .maxCommands = 2u, .maxPlans = 2u}),
              "exhaustive prefix capture begins");
          const auto publication = source(0u, 2u);
          const std::array extents{extent(0u), extent(1u)};
          check(owner.publish(publication, extents), "exhaustive prefix publication registered");
          const auto commands = refs<2>(owner);
          const std::array prefixCommands{commands[acceptedPhysical]};
          const std::array<std::uint32_t, 1> localOrder{0u}, prefixOrder{claimedPrefix}, suffixOrder{actualSuffix};
          const auto prefix = owner.stageWithoutDag(Route::PlannerBypass, Reason::DceEncodedPrefix,
              prefixCommands, localOrder);
          check(prefix && encoded(owner, prefix, prefixCommands, localOrder),
              "only the actual prefix is observed encoded once");
          const std::array<std::uint32_t, 2> fullOrder{graphHead, 1u - graphHead};
          const auto selected = graph(fullOrder, commands, noIndex, true);
          const bool permitted = graphHead == acceptedPhysical && claimedPrefix == acceptedPhysical &&
              actualSuffix != acceptedPhysical;
          const auto suffix = owner.stageSelectedDag(Reason::None, selected, commands, suffixOrder, prefixOrder);
          check((suffix != 0u) == permitted,
              "all two-command prefixes require exact earlier executed identity and selected suffix order");
          if (permitted) {
            TapeTopologyRuntimeSnapshot output;
            const std::array receipts{TapeTopologyAcceptedInvocation{prefix, 3u},
                TapeTopologyAcceptedInvocation{suffix, 4u}};
            check(encoded(owner, suffix, commands, suffixOrder) &&
                owner.acceptBatch(capture, receipts, std::array{publication}) && owner.snapshot(output) &&
                output.plans.size() == 2u && output.placements.size() == 2u &&
                output.placements[0].commandIndex == acceptedPhysical &&
                output.placements[0].outcome == static_cast<std::uint32_t>(Outcome::ExecutedWithoutDag) &&
                output.placements[0].dagPassIndex == noIndex &&
                output.placements[1].commandIndex != acceptedPhysical,
                "suffix never reassigns the accepted prefix to its later graph");
          } else {
            TapeTopologyRuntimeSnapshot output;
            check(owner.failed() && !owner.snapshot(output), "inconsistent prefix fails final qualification");
          }
        }
}

void scopedOwnerIsSynchronousAndThreadLocal() {
  TapeTopologyCapture first, second;
  check(activeTapeTopologyCapture == nullptr, "disabled scope has no installed owner");
  {
    ScopedTapeTopologyCapture outer(first);
    check(activeTapeTopologyCapture == &first, "cold outer scope installs owner");
    {
      ScopedTapeTopologyCapture inner(second);
      check(activeTapeTopologyCapture == &second, "nested scope selects current invocation owner");
      bool isolated = false;
      std::thread other([&] { isolated = activeTapeTopologyCapture == nullptr; });
      other.join();
      check(isolated, "another thread never borrows the caller's owner");
    }
    check(activeTapeTopologyCapture == &first, "nested scope restores earlier owner");
  }
  check(activeTapeTopologyCapture == nullptr, "scope destruction cannot leave a borrowed owner installed");
}
} // namespace

int main() {
  try {
    sourceLocalLookupIsExactCopiedAndBounded();
    sourceLocatorPublicationFailureIsTransactional();
    publicationAndStagingAreNotAcceptance();
    discardedGraphCannotContributeMembership();
    selectedOrderAndOmissionAreIndependentOfMaterialization();
    combinedPassUsesOneAcceptedInvocation();
    prefixAndSuffixAreSeparateInvocations();
    wrongAuthorityAndMalformedSchedulesReject();
    rejectedCapacityDoesNotGrowStorage();
    repeatedPrefixesStoreOnlyNewPhysicalReferences();
    registrationBeforeExecutionPromotesOnlyTerminalPrefix();
    cumulativePrefixMustPreserveObservedInvocationOrder();
    batchAcceptanceIsTransactionalAndSourceExact();
    executionPrefixAndEffectFailureBoundaries();
    rejectingUneffectedCandidatePreservesIndependentStages();
    revokeInvalidatesAllPendingHandles();
    exhaustiveTwoCommandLifecycle();
    exhaustivePrefixCannotBeReassigned();
    scopedOwnerIsSynchronousAndThreadLocal();
    std::cout << "render-tape-topology-observer checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render-tape-topology-observer: " << error.what() << '\n';
    return 1;
  }
}
