#include "dxmt9/dxmt9_render_tape_topology_scope.hpp"
#include "dxmt9/dxmt9_draw_encoder.hpp"
#include "dxmt9/dxmt9_encode_session_internal.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

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

class Root final : public TapeRuntimeCaptureAttachment {
public:
  TapeRuntimeObserverSink sink() noexcept override { return {this, observe}; }
  TapeTopologyCapture& topology() noexcept override { return topology_; }
private:
  static void observe(void*, const TapeRuntimeObservation&) noexcept {}
  TapeTopologyCapture topology_{};
};

struct Fixture {
  TapeRuntimeObserverEndpoint endpoint{};
  std::shared_ptr<Root> root = std::make_shared<Root>();
  TapeTopologyCaptureIdentity identity{};
  std::vector<TapeTopologySourceIdentity> sources{};
  std::vector<TapeTopologySourceRange> ranges{};

  explicit Fixture(TapeTopologyCaptureIdentity value = capture) : identity(value) {
    check(root->topology().begin(identity, root->edges().baseline()), "capture begins on the shared clock");
    check(endpoint.attachRetained(root), "typed capture attaches");
  }
  ~Fixture() { endpoint.revoke(root->sink()); }

  void publish(std::uint32_t count) {
    const auto index = static_cast<std::uint32_t>(sources.size());
    const TapeTopologySourceIdentity source{
        .source = {.id = {.index = index, .generation = index + 1u},
          .storage = {.firstPage = index, .pageCount = 1u, .generation = index + 1u}},
        .sourceOrdinal = index + 10u, .sequence = index + 1u,
        .publicationEdge = root->edges().next(), .sourceIndex = index, .commandCount = count};
    std::vector<TapeTopologyPhysicalExtent> extents;
    for (std::uint32_t command = 0u; command < count; ++command)
      extents.push_back({command, tapeTopologyCommandKind(core::MetalCommandKind::DrawRun), 1u});
    check(root->topology().publish(source, extents), "immutable physical source publishes");
    sources.push_back(source);
    ranges.push_back({source.source, source.sequence, 0u, count});
  }
  std::shared_ptr<TapeTopologyInvocationContext> context() {
    auto result = TapeTopologyInvocationContext::create(endpoint.acquireCapture(), identity, ranges);
    check(bool(result), "exact published ranges bind to a retained context");
    return result;
  }
};

framegraph::FrameGraph graph(std::span<const std::uint32_t> order,
    std::uint32_t dead = noIndex) {
  framegraph::FrameGraph result;
  for (const auto index : order) {
    result.commands.push_back({index, core::MetalCommandKind::DrawRun});
    framegraph::PassNode pass{};
    pass.commands = {static_cast<std::uint32_t>(result.commands.size() - 1u), 1u};
    pass.flags.dead = index == dead;
    result.passes.push_back(pass);
  }
  return result;
}

void partialOffsetsAndBorrowedInputsStayExact() {
  Fixture fixture;
  fixture.publish(5u);
  auto range = fixture.ranges[0];
  range.commandBegin = 2u;
  range.commandCount = 2u;
  auto context = TapeTopologyInvocationContext::create(fixture.endpoint.acquireCapture(), capture,
      std::span(&range, 1u));
  check(context && context->commandCount() == 2u, "partial context owns only the supplied physical range");
  range.commandBegin = 0u;
  std::uint32_t index = 0u;
  const auto source = fixture.sources[0];
  check(context->commandIndex(source.source, source.sequence, 2u, index) && index == 0u &&
      context->commandIndex(source.source, source.sequence, 3u, index) && index == 1u,
      "partial local indices preserve original physical offsets");
  check(!context->commandIndex(source.source, source.sequence, 0u, index) && index == UINT32_MAX,
      "a full-source locator cannot escape this bound partial range");
  auto stale = source.source;
  ++stale.storage.generation;
  check(!context->commandIndex(stale, source.sequence, 2u, index) && index == UINT32_MAX,
      "stale storage generation is not physical provenance");
  std::array<std::uint32_t, 2> order{0u, 1u};
  TapeTopologyInvocationHandle handle;
  check(context->stageWithoutDag(Route::NaturalOrder, Reason::None, order, handle),
      "partial no-DAG invocation stages exact refs");
  order = {1u, 0u};
  const std::array<std::uint32_t, 2> actual{0u, 1u};
  check(context->observeEncoded(handle, actual) && context->registerSources(fixture.ranges) &&
      context->acceptRegisteredBatch(std::span(&handle, 1u)),
      "successful Encode and real registration consume copied stage values");
  TapeTopologyRuntimeSnapshot snapshot;
  check(fixture.root->topology().snapshot(snapshot, false) && !snapshot.complete &&
      snapshot.plans.size() == 1u && snapshot.placements.size() == 2u &&
      snapshot.placements[0].commandIndex == 2u && snapshot.placements[1].commandIndex == 3u,
      "partial Encode never relabels physical commands zero and one or completes the full source");
  check(snapshot.placements[0].dagPassIndex == noIndex &&
      snapshot.placements[0].outcome == static_cast<std::uint32_t>(Outcome::ExecutedWithoutDag),
      "natural execution grants no DAG membership");
}

void sourceHelpersUseActualPartialReplayRanges() {
  for (const bool preregistered : {false, true}) {
    Fixture fixture;
    fixture.publish(5u);
    auto context = fixture.context();
    const auto source = fixture.sources[0];
    core::ChunkSlot slot;
    slot.commandHeaders.resize(5u);
    const core::SourcePayloadView payload(slot);
    encoders::EncodeChunkSessionState session;
    encoders::EncodeChunkOptions options;
    options.partitionSource = source.source;
    options.session = &session;
    options.deferSessionFinalization = true;
    if (preregistered) {
      options.preRegisteredFragment = encoders::PreRegisteredEncodeChunkFragment{
          .commandBegin = 2u, .commandCount = 2u};
    } else {
      options.sessionSource = core::metalqueue::QueueCompletionSource{
          .source = source.source, .slotIndex = 17u, .seqId = source.sequence,
          .commandBegin = 2u, .commandCount = 2u};
    }
    TapeTopologyInvocationHandle handle;
    {
      ScopedTapeTopologyInvocation scope(context);
      stageTapeTopologySourceWithoutDag(payload, source.sequence, options, Reason::None);
      handle = scope.handle();
      check(bool(handle), "valid partial source helper stages one exact invocation");
      prepareTapeTopologySourceEncode(payload, source.sequence, options);
      check(scope.observePreparedEncode(true), "both helpers agree on the real partial replay range");
    }
    const TapeTopologySourceRange receipt{source.source, source.sequence, 2u, 2u};
    check(context->registerSources(std::span(&receipt, 1u)) &&
        context->acceptRegisteredBatch(std::span(&handle, 1u)),
        "actual partial registration covers the helper-selected physical commands");
    TapeTopologyRuntimeSnapshot snapshot;
    check(fixture.root->topology().snapshot(snapshot, false) && !snapshot.complete &&
        snapshot.plans.size() == 1u && snapshot.placements.size() == 2u &&
        snapshot.placements[0].commandIndex == 2u && snapshot.placements[1].commandIndex == 3u,
        "partial sessionSource and preregistered fragment never stage the full payload");
  }
}

void sourceHelpersRejectInvalidReplayRangeAndConflictingModes() {
  for (const bool prepareOnly : {false, true}) {
    for (std::uint32_t variant = 0u; variant < 9u; ++variant) {
      Fixture fixture;
      fixture.publish(5u);
      auto context = fixture.context();
      const auto source = fixture.sources[0];
      core::ChunkSlot slot;
      slot.commandHeaders.resize(5u);
      const core::SourcePayloadView payload(slot);
      encoders::EncodeChunkSessionState session;
      encoders::EncodeChunkOptions options;
      options.partitionSource = source.source;
      options.session = &session;
      options.deferSessionFinalization = true;
      if (variant < 6u) {
        options.preRegisteredFragment = encoders::PreRegisteredEncodeChunkFragment{
            .commandBegin = 2u, .commandCount = 2u};
        if (variant == 0u) {
          options.sessionSource = core::metalqueue::QueueCompletionSource{
              .source = source.source, .slotIndex = 17u, .seqId = source.sequence,
              .commandBegin = 1u, .commandCount = 3u};
        }
        if (variant == 1u) options.session = nullptr;
        if (variant == 2u) options.deferSessionFinalization = false;
        if (variant == 3u) options.preRegisteredFragment->commandCount = 0u;
        if (variant == 4u) options.preRegisteredFragment->commandBegin = 6u;
        if (variant == 5u) options.preRegisteredFragment->commandBegin = 4u;
      } else {
        options.sessionSource = core::metalqueue::QueueCompletionSource{
            .source = source.source, .slotIndex = 17u, .seqId = source.sequence,
            .commandBegin = 2u, .commandCount = 2u};
        if (variant == 6u) ++options.sessionSource->seqId;
        if (variant == 7u) options.sessionSource->commandBegin = 6u;
        if (variant == 8u) options.sessionSource->commandBegin = 4u;
      }
      ScopedTapeTopologyInvocation scope(context);
      if (prepareOnly) prepareTapeTopologySourceEncode(payload, source.sequence, options);
      else stageTapeTopologySourceWithoutDag(payload, source.sequence, options, Reason::None);
      check(fixture.root->topology().failed() && !scope.handle(),
          "both helpers fail capture for actual encoder-invalid ranges and conflicting source modes");
    }
  }
}

void registrationAndEncodeAreIndependentFacts() {
  for (const bool registrationFirst : {false, true}) {
    Fixture fixture;
    fixture.publish(2u);
    auto context = fixture.context();
    const std::array<std::uint32_t, 2> order{1u, 0u};
    TapeTopologyInvocationHandle handle;
    check(context->stageWithoutDag(Route::PlannerBypass, Reason::None, order, handle),
        "externally committed no-DAG order stages without graph labels");
    TapeTopologyRuntimeSnapshot snapshot;
    if (registrationFirst) {
      check(context->registerSources(fixture.ranges) && context->acceptRegisteredBatch(std::span(&handle, 1u)),
          "actual registration may precede Encode");
    } else {
      check(context->observeEncoded(handle, order), "actual successful Encode may precede registration");
    }
    check(fixture.root->topology().snapshot(snapshot, false) && snapshot.plans.empty() && !snapshot.complete,
        "one fact alone cannot authenticate topology");
    if (registrationFirst) check(context->observeEncoded(handle, order), "later Encode joins registration");
    else check(context->registerSources(fixture.ranges) &&
        context->acceptRegisteredBatch(std::span(&handle, 1u)), "later registration joins Encode");
    check(fixture.root->topology().snapshot(snapshot) && snapshot.complete &&
        snapshot.placements[0].executionOrdinal == 1u && snapshot.placements[1].executionOrdinal == 0u,
        "both facts publish the actual order, independently of arrival order");
    check(snapshot.plans[0].acceptanceEdge > fixture.sources[0].publicationEdge,
        "acceptance allocates its edge from the retained root's shared clock");
  }
}

void selectedDagExcludesPendingPrefixAndAcceptsExplicitBatch() {
  Fixture fixture;
  fixture.publish(3u);
  auto context = fixture.context();
  const std::array<std::uint32_t, 1> prefix{0u}, suffix{1u};
  TapeTopologyInvocationHandle first, second;
  check(context->stageWithoutDag(Route::NaturalOrder, Reason::None, prefix, first) &&
      context->observeEncoded(first, prefix), "DCE prefix is a pending successful invocation, not registration");
  const std::array<std::uint32_t, 3> physical{0u, 1u, 2u};
  auto selected = graph(physical, 2u);
  check(context->stageSelectedDag(Reason::None, selected, suffix, prefix, second) &&
      context->observeEncoded(second, suffix), "selected suffix retains the actual DAG and excludes the earlier prefix");
  selected.commands.clear();
  selected.passes.clear();
  check(context->registerSources(fixture.ranges), "real suffix registration covers the prefix and suffix source");
  const std::array handles{first, second};
  check(context->acceptRegisteredBatch(handles), "explicit chronological prefix/suffix handles accept atomically");
  TapeTopologyRuntimeSnapshot snapshot;
  check(fixture.root->topology().snapshot(snapshot) && snapshot.plans.size() == 2u &&
      snapshot.placements.size() == 3u && snapshot.placements[0].dagPassIndex == noIndex &&
      snapshot.placements[1].dagPassIndex == 1u &&
      snapshot.placements[2].outcome == static_cast<std::uint32_t>(Outcome::ProvenOmitted),
      "prefix is never retrospectively regrouped and omitted suffix commands keep real DAG evidence");
  check(snapshot.plans[0].acceptanceEdge < snapshot.plans[1].acceptanceEdge,
      "each invocation has a distinct chronological acceptance edge");
}

void aggregateFragmentsSuppressRestagingAndWaitForFolds() {
  Fixture fixture;
  fixture.publish(2u);
  fixture.publish(2u);
  auto context = fixture.context();
  const std::array<std::uint32_t, 4> order{1u, 0u, 3u, 2u};
  const auto selected = graph(order);
  TapeTopologyInvocationHandle handle;
  check(context->stageSelectedDag(Reason::None, selected, order, {}, handle) &&
      context->beginAggregate(handle, 2u), "combined selected DAG stages once before backend fragments");
  check(context->registerSources(fixture.ranges) && context->acceptRegisteredBatch(std::span(&handle, 1u)),
      "actual preregistration is recorded without fabricating fragment execution");
  const std::array<std::uint32_t, 2> first{1u, 0u}, second{3u, 2u};
  {
    ScopedTapeTopologyInvocation fragment(context, handle, true);
    check(activeTapeTopologyInvocation == &fragment && fragment.suppressesFragmentStaging(),
        "fragment TLS retains the aggregate invocation");
    check(fragment.stageWithoutDag(Route::Optimized, Reason::None, first),
        "fragment staging is suppressed even though this no-DAG route would otherwise be invalid");
    check(fragment.observeSuccessfulEncode(first), "first successful fragment records its exact aggregate prefix");
  }
  check(context->foldFragment(handle), "first actual result fold is independent of its Encode fact");
  {
    ScopedTapeTopologyInvocation fragment(context, handle, true);
    const framegraph::FrameGraph empty;
    check(fragment.stageSelectedDag(Reason::None, empty, second), "fragment graph cannot replace the selected aggregate DAG");
    check(fragment.observeSuccessfulEncode(second), "second successful fragment records the exact aggregate suffix");
  }
  TapeTopologyRuntimeSnapshot snapshot;
  check(fixture.root->topology().snapshot(snapshot, false) && snapshot.plans.empty(),
      "even successful fragments do not promote the aggregate before all folds");
  check(context->foldFragment(handle) && context->finishAggregate(handle),
      "all successful Encode facts and folds finalize exactly one invocation");
  check(fixture.root->topology().snapshot(snapshot) && snapshot.plans.size() == 1u &&
      snapshot.placements.size() == 4u && snapshot.placements[0].executionOrdinal == 1u &&
      snapshot.placements[2].executionOrdinal == 3u,
      "combined physical refs map both sources without per-fragment topology rows");
}

void staleRejectionAndCancellationNeverUndoEffects() {
  {
    Fixture fixture;
    fixture.publish(1u);
    auto context = fixture.context();
    const std::array<std::uint32_t, 1> order{0u};
    TapeTopologyInvocationHandle stale, accepted;
    check(context->stageWithoutDag(Route::NaturalOrder, Reason::None, order, stale) && context->reject(stale),
        "a stale selected candidate with no effects is rejected");
    check(context->stageWithoutDag(Route::NaturalOrder, Reason::None, order, accepted) &&
        accepted.attempt > stale.attempt, "reselection does not reuse stale attempt identity");
    check(context->observeEncoded(accepted, order) && context->registerSources(fixture.ranges) &&
        context->acceptRegisteredBatch(std::span(&accepted, 1u)), "reselected actual invocation can qualify");
  }
  for (const bool registered : {false, true}) {
    Fixture fixture;
    fixture.publish(1u);
    auto context = fixture.context();
    const std::array<std::uint32_t, 1> order{0u};
    TapeTopologyInvocationHandle handle;
    check(context->stageWithoutDag(Route::NaturalOrder, Reason::None, order, handle), "effectful cancellation candidate stages");
    if (registered) check(context->registerSources(fixture.ranges), "actual registration is already an effect");
    else check(context->observeEncoded(handle, order), "actual successful Encode is already an effect");
    check(!context->reject(handle) && fixture.root->topology().failed(),
        "post-effect rejection fails capture rather than discarding facts or replaying rendering");
  }
  {
    Fixture fixture;
    fixture.publish(1u);
    const std::array<std::uint32_t, 1> order{0u};
    auto context = fixture.context();
    TapeTopologyInvocationHandle cancelled;
    check(context->stageWithoutDag(Route::NaturalOrder, Reason::None, order, cancelled), "abandoned cold candidate stages");
    context.reset();
    check(!fixture.root->topology().failed(), "last context owner cancels a no-effect candidate");
    context = fixture.context();
    TapeTopologyInvocationHandle next;
    check(context->stageWithoutDag(Route::NaturalOrder, Reason::None, order, next) && next.attempt > cancelled.attempt,
        "scope cancellation releases only its staged command ownership");
  }
}

void forgedRegistrationOrderAndHandlesFailClosed() {
  for (std::uint32_t variant = 0u; variant < 6u; ++variant) {
    Fixture fixture;
    fixture.publish(2u);
    auto context = fixture.context();
    const std::array<std::uint32_t, 2> order{0u, 1u}, wrong{1u, 0u};
    TapeTopologyInvocationHandle handle;
    check(context->stageWithoutDag(Route::NaturalOrder, Reason::None, order, handle), "forgery fixture stages");
    bool result = true;
    if (variant < 2u) {
      auto receipt = fixture.ranges[0];
      if (variant == 0u) ++receipt.source.storage.generation;
      else ++receipt.sequence;
      result = context->registerSources(std::span(&receipt, 1u));
    }
    if (variant == 2u) result = context->observeEncoded(handle, wrong);
    if (variant == 3u) {
      ++handle.capture.epoch;
      result = context->observeEncoded(handle, order);
    }
    if (variant == 4u) result = context->acceptRegisteredBatch(std::span(&handle, 1u));
    if (variant == 5u) {
      auto receipt = fixture.ranges[0];
      receipt.commandCount = 1u;
      check(context->registerSources(std::span(&receipt, 1u)), "actual partial registration can be recorded");
      result = context->acceptRegisteredBatch(std::span(&handle, 1u));
    }
    check(!result && fixture.root->topology().failed(),
        "stale registration, wrong actual order, forged handles and uncovered acceptance fail closed");
  }
}

void tlsRetainsContextAcrossRetirementWithoutJoiningEncode() {
  TapeRuntimeObserverEndpoint endpoint;
  auto root = std::make_shared<Root>();
  check(root->topology().begin(capture, root->edges().baseline()), "retained TLS capture begins");
  const TapeTopologySourceIdentity source{
      .source = {.id = {.index = 0u, .generation = 1u},
        .storage = {.firstPage = 0u, .pageCount = 1u, .generation = 1u}},
      .sourceOrdinal = 10u, .sequence = 1u, .publicationEdge = root->edges().next(),
      .sourceIndex = 0u, .commandCount = 1u};
  check(root->topology().publish(source, std::array{TapeTopologyPhysicalExtent{
      0u, tapeTopologyCommandKind(core::MetalCommandKind::DrawRun), 1u}}) && endpoint.attachRetained(root),
      "retained TLS source publishes and attaches");
  const auto sink = root->sink();
  std::weak_ptr<Root> weak = root;
  const TapeTopologySourceRange range{source.source, source.sequence, 0u, 1u};
  auto context = TapeTopologyInvocationContext::create(endpoint.acquireCapture(), capture, std::span(&range, 1u));
  const std::array<std::uint32_t, 1> order{0u};
  check(context && !activeTapeTopologyInvocation, "TLS starts empty");
  {
    ScopedTapeTopologyInvocation outer(context);
    check(outer.stageWithoutDag(Route::NaturalOrder, Reason::None, order), "outer backend scope stages");
    {
      ScopedTapeTopologyInvocation inner({});
      check(activeTapeTopologyInvocation == &inner, "nested TLS scope installs itself");
    }
    check(activeTapeTopologyInvocation == &outer, "nested TLS scope restores the retained predecessor");
    context.reset();
    root.reset();
    check(endpoint.revoke(sink) && !weak.expired(),
        "retirement joins no backend-long admission and retained TLS keeps the old root alive");
    auto replacement = std::make_shared<Root>();
    check(endpoint.attachRetained(replacement), "replacement capture attaches while old Encode scope remains alive");
    std::uint32_t index = 0u;
    check(!outer.observeSuccessfulEncode(order) &&
        !outer.commandIndex(source.source, source.sequence, 0u, index) && index == UINT32_MAX &&
        !replacement->topology().failed(), "late TLS receipts cannot access old topology or target a replacement root");
    check(endpoint.revoke(replacement->sink()), "replacement retires independently");
  }
  check(!activeTapeTopologyInvocation && weak.expired(), "TLS restoration releases the final old owner without a cycle");
}

void pendingStoreJoinsExplicitInvocationsAcrossBackendScopes() {
  Fixture fixture;
  fixture.publish(3u);
  auto lease = fixture.endpoint.acquireCapture();
  TapeTopologyCaptureIdentity identity;
  check(lease.visit([&](TapeRuntimeCaptureAttachment& root) noexcept {
    identity = root.topology().identity();
  }) && identity == capture, "value-only identity getter works inside a short admitted visit");
  const std::array<std::uint32_t, 1> prefix{0u}, suffix{1u};
  {
    ScopedTapeTopologyInvocation scope(retainTapeTopologyInvocation(lease, fixture.ranges));
    check(scope.stageWithoutDag(Route::NaturalOrder, Reason::None, prefix) &&
        scope.observeSuccessfulEncode(prefix), "first real backend scope retains its pending explicit handle");
  }
  const std::array<std::uint32_t, 3> physical{0u, 1u, 2u};
  {
    ScopedTapeTopologyInvocation scope(retainTapeTopologyInvocation(lease, fixture.ranges));
    check(scope.stageSelectedDag(Reason::DceEncodedPrefix, graph(physical, 2u), suffix, prefix) &&
        scope.observeSuccessfulEncode(suffix), "later selected backend scope owns only its actual suffix");
  }
  TapeTopologyRuntimeSnapshot snapshot;
  check(fixture.root->topology().snapshot(snapshot, false) && snapshot.plans.empty(),
      "destroying backend TLS scopes does not invent actual source registration");
  registerTapeTopologySources(lease, fixture.ranges);
  check(fixture.root->topology().snapshot(snapshot) && snapshot.plans.size() == 2u &&
      snapshot.plans[0].acceptanceEdge < snapshot.plans[1].acceptanceEdge,
      "real registration joins both explicit pending handles atomically and chronologically");
}

void preregistrationAndExactCandidateReceiptsDriveAggregate() {
  Fixture fixture;
  fixture.publish(2u);
  auto lease = fixture.endpoint.acquireCapture();
  const std::array<std::uint32_t, 2> order{1u, 0u};
  TapeTopologyInvocationHandle stale;
  {
    ScopedTapeTopologyInvocation candidate(retainTapeTopologyInvocation(lease, fixture.ranges, true));
    check(candidate.stageSelectedDag(Reason::None, graph(order), order), "tentative planner stages the actual live graph");
    stale = candidate.handle();
  }
  rejectTapeTopologyCandidate(lease, fixture.ranges);
  check(!fixture.root->topology().failed(), "exact stale source-set rejection discards only a no-effect candidate");
  {
    ScopedTapeTopologyInvocation candidate(retainTapeTopologyInvocation(lease, fixture.ranges, true));
    check(candidate.stageSelectedDag(Reason::None, graph(order), order) &&
        candidate.handle().attempt > stale.attempt, "a selected retry owns a fresh explicit handle");
  }
  check(activateTapeTopologyCandidate(lease, fixture.ranges, 2u), "coordinator selects the exact candidate before effects");
  registerTapeTopologySources(lease, fixture.ranges);
  TapeTopologyRuntimeSnapshot snapshot;
  check(fixture.root->topology().snapshot(snapshot, false) && snapshot.plans.empty(),
      "real preregistration before fragment Encode is not a successful execution fact");
  for (std::uint32_t fragment = 0u; fragment < 2u; ++fragment) {
    TapeTopologyInvocationHandle handle;
    auto context = tapeTopologyCandidateContext(lease, fixture.ranges, handle);
    const std::array<std::uint32_t, 1> actual{order[fragment]};
    {
      ScopedTapeTopologyInvocation scope(context, handle, true);
      check(scope.stageWithoutDag(Route::Optimized, Reason::None, actual) &&
          scope.observeSuccessfulEncode(actual), "actual fragment scope suppresses restaging and records its successful order");
    }
    foldTapeTopologyCandidate(lease, fixture.ranges, fragment == 1u);
  }
  check(fixture.root->topology().snapshot(snapshot) && snapshot.plans.size() == 1u &&
      snapshot.plans[0].route == static_cast<std::uint32_t>(Route::Optimized),
      "all real fragment results/folds authenticate one aggregate, never source-local reconstructed graphs");
}

void storeSeparatesQueuesAndRetiredCaptureArms() {
  Fixture first;
  Fixture second({capture.token, capture.epoch, capture.queue + 1u});
  first.publish(1u);
  second.publish(1u);
  const std::array<std::uint32_t, 1> order{0u};
  for (auto* fixture : {&first, &second}) {
    ScopedTapeTopologyInvocation scope(retainTapeTopologyInvocation(
        fixture->endpoint.acquireCapture(), fixture->ranges));
    check(scope.stageWithoutDag(Route::NaturalOrder, Reason::None, order) &&
        scope.observeSuccessfulEncode(order), "each queue has its own qualified pending invocation");
  }
  registerTapeTopologySources(second.endpoint.acquireCapture(), second.ranges);
  TapeTopologyRuntimeSnapshot snapshot;
  check(second.root->topology().snapshot(snapshot) &&
      first.root->topology().snapshot(snapshot, false) && snapshot.plans.empty(),
      "equal SourceRef and sequence values on different queues cannot cross-register");
  registerTapeTopologySources(first.endpoint.acquireCapture(), first.ranges);
  check(first.root->topology().snapshot(snapshot), "first queue registers only on its real receipt");

  auto stale = retainTapeTopologyInvocation(first.endpoint.acquireCapture(), first.ranges, true);
  check(first.endpoint.revoke(first.root->sink()), "old capture arm retires while its cold context is retained");
  Fixture replacement;
  replacement.publish(1u);
  {
    ScopedTapeTopologyInvocation scope(retainTapeTopologyInvocation(replacement.endpoint.acquireCapture(), replacement.ranges));
    check(scope.stageWithoutDag(Route::NaturalOrder, Reason::None, order) &&
        scope.observeSuccessfulEncode(order), "replacement arm binds its own root after stale-store pruning");
  }
  registerTapeTopologySources(replacement.endpoint.acquireCapture(), replacement.ranges);
  TapeTopologyInvocationHandle invalid;
  check(!stale->stageWithoutDag(Route::NaturalOrder, Reason::None, order, invalid) &&
      replacement.root->topology().snapshot(snapshot) && !replacement.root->topology().failed(),
      "retained old contexts cannot issue topology operations against a rearmed capture");
}

} // namespace

int main() {
  try {
    partialOffsetsAndBorrowedInputsStayExact();
    sourceHelpersUseActualPartialReplayRanges();
    sourceHelpersRejectInvalidReplayRangeAndConflictingModes();
    registrationAndEncodeAreIndependentFacts();
    selectedDagExcludesPendingPrefixAndAcceptsExplicitBatch();
    aggregateFragmentsSuppressRestagingAndWaitForFolds();
    staleRejectionAndCancellationNeverUndoEffects();
    forgedRegistrationOrderAndHandlesFailClosed();
    tlsRetainsContextAcrossRetirementWithoutJoiningEncode();
    pendingStoreJoinsExplicitInvocationsAcrossBackendScopes();
    preregistrationAndExactCandidateReceiptsDriveAggregate();
    storeSeparatesQueuesAndRetiredCaptureArms();
    std::cout << "render tape topology scope: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render tape topology scope: " << error.what() << '\n';
    return 1;
  }
}
