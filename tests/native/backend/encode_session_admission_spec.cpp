#include "../../../src/dxmt9/dxmt9_source_semantics.hpp"
#include "../../../src/dxmt9/render/encode_session_admission.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

using dxmt9::core::CanonicalDrawState;
using dxmt9::core::ChunkSlot;
using dxmt9::core::DrawParam;
using dxmt9::core::Handle;
using dxmt9::core::RenderRoute;
using dxmt9::core::SourceEntryEncoderKind;
using dxmt9::core::SourcePayloadView;
using dxmt9::core::SourceSemanticBoundaryKind;
using dxmt9::core::SourceSemanticSummary;
using dxmt9::core::SourceSemanticSummaryContext;
using dxmt9::render::ActiveRenderContinuationActive;
using dxmt9::render::ActiveRenderContinuationState;
using dxmt9::render::EncodeCaptureMode;
using dxmt9::render::EncodeSessionAdmissionState;
using dxmt9::render::EncodeSessionLimits;
using dxmt9::render::RenderContinuationDecision;
using dxmt9::render::SessionAdmissionCandidate;
using dxmt9::render::SessionAdmissionDecision;
using dxmt9::render::SessionCapacityDimension;
using dxmt9::render::SessionCapacityLeaseState;
using dxmt9::render::SessionCapacityPolicy;
using dxmt9::render::SessionCapacityVector;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    fail(std::string(message));
  }
}

void appendDraw(ChunkSlot& slot,
                std::uint64_t color,
                std::uint64_t sampled = 0) {
  CanonicalDrawState state{};
  state.hot.colorAttachments[0].handle = Handle{color};
  state.hot.colorAttachments[0].sampleCount = 1;
  if (sampled != 0) {
    state.hot.textures[0] = Handle{sampled};
    state.hot.textureMask = 1;
  }
  std::array<DrawParam, 1> draws{};
  slot.appendDrawRun(std::move(state), {}, draws, {});
}

SourceSemanticSummary summarize(
    const ChunkSlot& slot,
    RenderRoute route = RenderRoute::Portable,
    bool canonicalized = true,
    bool entryStable = true) {
  return dxmt9::core::summarizeSourcePayload(
      SourcePayloadView(slot),
      SourceSemanticSummaryContext{
          .byteCount = 8192,
          .pageCount = 2,
          .firstRenderRoute = route,
          .passActionEpoch = 1,
          .sealed = true,
          .entryStable = entryStable,
          .resourcesCanonicalized = canonicalized,
      });
}

SessionAdmissionCandidate candidate(
    std::uint64_t ordinal,
    std::uint64_t seqId,
    SourceSemanticSummary semantic) {
  return SessionAdmissionCandidate{
      .semantic = semantic,
      .rawOrdinal = ordinal,
      .sourceOrdinal = ordinal,
      .seqId = seqId,
      .predictedCommandBuffers = 1,
  };
}

void sourceSummaryIsFlatAndClassifiesBoundaries() {
  static_assert(std::is_trivially_copyable_v<SourceSemanticSummary>);
  static_assert(std::is_standard_layout_v<SourceSemanticSummary>);
  static_assert(std::is_trivially_copyable_v<SessionAdmissionCandidate>);
  static_assert(std::is_standard_layout_v<SessionAdmissionCandidate>);
  static_assert(sizeof(SourceSemanticSummary) <= 256);

  ChunkSlot draws{};
  appendDraw(draws, 0x10);
  appendDraw(draws, 0x10, 0x10);
  const SourceSemanticSummary drawSummary = summarize(draws);
  check(drawSummary.valid(), "draw summary is sealed and valid");
  checkEq(drawSummary.entryKind, SourceEntryEncoderKind::Render,
          "draw source begins in the render encoder");
  checkEq(drawSummary.drawCount, 2u, "draw count covers both draw runs");
  checkEq(drawSummary.firstBoundaryOrdinal, 1u,
          "first hazard boundary names the second command");
  checkEq(drawSummary.firstBoundary,
          SourceSemanticBoundaryKind::ResourceHazard,
          "attachment sampling is a render continuation hazard");
  checkEq(drawSummary.byteCount, std::uint64_t{8192},
          "summary retains payload byte count");
  checkEq(drawSummary.pageCount, 2u,
          "summary retains payload page count");

  ChunkSlot clear{};
  clear.appendClear({});
  const SourceSemanticSummary clearSummary = summarize(clear);
  checkEq(clearSummary.entryKind, SourceEntryEncoderKind::Render,
          "clear is render work but not pass continuation");
  checkEq(clearSummary.firstBoundaryOrdinal, 0u,
          "entry clear is an immediate pass boundary");
  checkEq(clearSummary.firstBoundary, SourceSemanticBoundaryKind::Clear,
          "entry clear keeps its semantic reason");

  ChunkSlot present{};
  appendDraw(present, 0x20);
  present.appendPresent({}, Handle{0x20});
  const SourceSemanticSummary presentSummary = summarize(present);
  check(presentSummary.hasPresent(), "present flag covers a final tail");
  check(presentSummary.hasFinalPresentTail(),
        "single terminal Present is identified as the session tail");
  checkEq(presentSummary.firstBoundaryOrdinal, 1u,
          "present boundary follows the opening draw");
  checkEq(presentSummary.firstBoundary, SourceSemanticBoundaryKind::Present,
          "present keeps its semantic boundary reason");

  present.appendClear({});
  const SourceSemanticSummary nonTailPresent = summarize(present);
  check(nonTailPresent.hasPresent() &&
            !nonTailPresent.hasFinalPresentTail(),
        "Present followed by work cannot close a carried session");

  const SourceSemanticSummary invalid =
      dxmt9::core::summarizeSourcePayload({});
  check(!invalid.valid(), "invalid borrowed payload cannot seal a summary");
  checkEq(invalid.entryKind, SourceEntryEncoderKind::Invalid,
          "invalid payload is explicit rather than an empty source");

  const std::size_t logicalBytes =
      dxmt9::core::measureSourcePayloadLogicalExtent(
          SourcePayloadView(clear));
  check(logicalBytes != 0,
        "non-empty common payload view has a logical byte extent");
  SourceSemanticSummary logicalSummary = summarize(clear);
  logicalSummary.byteCount = logicalBytes;
  const EncodeSessionLimits byteLimited{
      .maxSources = 1,
      .maxPages = 2,
      .maxBytes = logicalBytes - 1u,
      .maxDraws = 1,
      .maxCommandBuffers = 1,
  };
  checkEq(dxmt9::render::classifySessionAdmission(
              {}, candidate(1, 1, logicalSummary), byteLimited),
          SessionAdmissionDecision::ProcessCandidateIsolated,
          "logical Legacy bytes participate in the session byte cap");
}

void admissionSeparatesOrderingCapsAndIsolation() {
  ChunkSlot slot{};
  appendDraw(slot, 0x10);
  const SourceSemanticSummary semantic = summarize(slot);
  const EncodeSessionLimits limits{
      .maxSources = 2,
      .maxPages = 4,
      .maxBytes = 16384,
      .maxDraws = 2,
      .maxCommandBuffers = 2,
  };
  EncodeSessionAdmissionState session{};
  const auto first = candidate(1, 1, semantic);
  checkEq(dxmt9::render::classifySessionAdmission(session, first, limits),
          SessionAdmissionDecision::Admit,
          "first ordinary source starts a session");
  check(dxmt9::render::appendSessionAdmission(session, first, limits),
        "first source advances admission state");

  const auto second = candidate(2, 2, semantic);
  checkEq(dxmt9::render::classifySessionAdmission(session, second, limits),
          SessionAdmissionDecision::Admit,
          "consecutive source within caps appends");
  check(dxmt9::render::appendSessionAdmission(session, second, limits),
        "second source advances bounded counters");
  checkEq(session.sources, 2u, "session source count is exact");
  checkEq(session.pages, 4u, "session page count is exact");

  const auto third = candidate(3, 3, semantic);
  checkEq(dxmt9::render::classifySessionAdmission(session, third, limits),
          SessionAdmissionDecision::SubmitPrefixBeforeCandidate,
          "fixed source/page caps submit the represented prefix");

  auto gap = candidate(4, 4, semantic);
  checkEq(dxmt9::render::classifySessionAdmission(session, gap, limits),
          SessionAdmissionDecision::SubmitPrefixBeforeCandidate,
          "a source/seq gap is independent intervening work");

  auto stale = candidate(2, 2, semantic);
  checkEq(dxmt9::render::classifySessionAdmission(session, stale, limits),
          SessionAdmissionDecision::RejectInvalid,
          "non-increasing identity is invalid rather than a split hint");

  EncodeSessionAdmissionState oneSource{};
  auto isolated = candidate(1, 1, semantic);
  isolated.key.captureMode = EncodeCaptureMode::Isolated;
  checkEq(dxmt9::render::classifySessionAdmission(
              oneSource, isolated, limits),
          SessionAdmissionDecision::ProcessCandidateIsolated,
          "capture mode cannot create a parked session");
  checkEq(dxmt9::render::classifySessionAdmission(
              session, isolated, limits),
          SessionAdmissionDecision::RejectInvalid,
          "stale isolated identity remains invalid");
  isolated.rawOrdinal = 3;
  isolated.sourceOrdinal = 3;
  isolated.seqId = 3;
  checkEq(dxmt9::render::classifySessionAdmission(
              session, isolated, limits),
          SessionAdmissionDecision::SubmitPrefixBeforeCandidate,
          "capture isolation first submits an older session prefix");

  EncodeSessionAdmissionState keyed{};
  check(dxmt9::render::appendSessionAdmission(keyed, first, limits),
        "key fixture starts an ordinary session");
  auto differentKey = second;
  differentKey.key.allocatorPolicyEpoch = 2;
  checkEq(dxmt9::render::classifySessionAdmission(
              keyed, differentKey, limits),
          SessionAdmissionDecision::SubmitPrefixBeforeCandidate,
          "admission-key changes submit the older prefix");
  checkEq(dxmt9::render::classifySessionAdmissionDetailed(
              keyed, differentKey, limits).limitingDimension,
          SessionCapacityDimension::None,
          "a semantic key boundary must not be mislabeled as SessionCap");

  SourceSemanticSummary observed = semantic;
  observed.flags |= dxmt9::core::SourceSemanticGlobalObservation;
  auto observedCandidate = candidate(2, 2, observed);
  checkEq(dxmt9::render::classifySessionAdmission(
              keyed, observedCandidate, limits),
          SessionAdmissionDecision::SubmitPrefixBeforeCandidate,
          "global observation cannot overtake an older session");
  checkEq(dxmt9::render::classifySessionAdmission(
              {}, candidate(1, 1, observed), limits),
          SessionAdmissionDecision::ProcessCandidateIsolated,
          "global observation runs outside a parked session");

  ChunkSlot presentSlot{};
  appendDraw(presentSlot, 0x10);
  presentSlot.appendPresent({}, Handle{0x10});
  const EncodeSessionLimits presentLimits{
      .maxSources = 3,
      .maxPages = 8,
      .maxBytes = 32768,
      .maxDraws = 4,
      .maxCommandBuffers = 3,
  };
  auto finalTail = candidate(2, 2, summarize(presentSlot));
  checkEq(dxmt9::render::classifySessionAdmission(
              keyed, finalTail, presentLimits),
          SessionAdmissionDecision::Admit,
          "one final Present tail may close an existing session");
  check(dxmt9::render::appendSessionAdmission(
            keyed, finalTail, presentLimits),
        "admitted Present tail seals the session state");
  checkEq(dxmt9::render::classifySessionAdmission(
              keyed, candidate(3, 3, semantic), presentLimits),
          SessionAdmissionDecision::SubmitPrefixBeforeCandidate,
          "no source may append after a session Present");
}

void leaseHeadroomAndCapGroupingAreDeterministic() {
  checkEq(dxmt9::render::worstCaseNonWrappingReservationPages(1),
          std::uint64_t{1},
          "one page needs no additional wrap padding");
  checkEq(dxmt9::render::worstCaseNonWrappingReservationPages(4),
          std::uint64_t{7},
          "headroom covers payload plus the worst three-page tail");

  const SessionCapacityPolicy policy{
      .highWater = {3, 17, 1700, 30, 3, 3, 3, 3, 3},
      .maxSession = {2, 10, 1000, 20, 2, 2, 2, 2, 2},
      .successorHeadroom = {1, 7, 700, 10, 1, 1, 1, 1, 1},
      .ordinaryDirect = {1, 4, 400, 10, 1, 1, 1, 1, 1},
  };
  check(policy.valid(),
        "fixed session footprint plus complete successor headroom fits highs");
  auto invalid = policy;
  invalid.successorHeadroom.pages = 6;
  check(!invalid.valid(),
        "configuration rejects missing circular-wrap successor credit");

  SessionCapacityLeaseState leases;
  const SessionCapacityVector firstCharge{1, 4, 400, 10, 1, 1, 1, 1, 1};
  auto unavailable = SessionCapacityVector{};
  unavailable.pages = 1;
  check(!leases.acquire(policy, unavailable, firstCharge),
        "older submitted residency delays fixed lease acquisition");
  check(leases.acquire(policy, {}, firstCharge) &&
            leases.lease().generation == 1,
        "coordinator atomically acquires and charges the Ready head");
  check(leases.lease().reserved == policy.highWater,
        "lease reserves fixed session and successor vectors once");
  check(leases.lease().used == firstCharge &&
            leases.charge(
                1, SessionCapacityVector{1, 4, 400, 10, 1, 1, 1, 1, 1}),
        "resident Ready heads transfer into used session credits");
  check(!leases.charge(
            1, SessionCapacityVector{1, 1, 1, 1, 1, 1, 1, 1, 1}),
        "a candidate beyond the fixed cap cannot consume the lease");
  check(!leases.release(2) && leases.lease().valid(),
        "a stale generation cannot release the live lease");
  check(leases.release(1), "the owning generation releases the lease");
  check(leases.acquire(policy, {}, firstCharge) &&
            leases.lease().generation == 2,
        "released lease identity is never reused");
  check(!leases.charge(1, firstCharge),
        "a stale generation cannot charge its successor lease");

  ChunkSlot slot{};
  appendDraw(slot, 0x20);
  const auto semantic = summarize(slot);
  const EncodeSessionLimits limits{
      .maxSources = 2,
      .maxPages = 4,
      .maxBytes = 16384,
      .maxDraws = 2,
      .maxCommandBuffers = 2,
  };
  const std::array candidates{
      candidate(1, 1, semantic),
      candidate(2, 2, semantic),
      candidate(3, 3, semantic),
  };
  const auto selectPrefix = [&](std::span<const std::uint32_t>
                                    completionSchedule) {
    // Completion schedule is deliberately not an input to classification.
    (void)completionSchedule;
    EncodeSessionAdmissionState session{};
    std::size_t selected = 0;
    for (const auto& next : candidates) {
      const auto result = dxmt9::render::classifySessionAdmissionDetailed(
          session, next, limits);
      if (result.decision != SessionAdmissionDecision::Admit) {
        checkEq(result.limitingDimension,
                SessionCapacityDimension::Sources,
                "first over-cap candidate names the limiting dimension");
        break;
      }
      check(dxmt9::render::appendSessionAdmission(session, next, limits),
            "selected prefix charges exactly once");
      ++selected;
    }
    return selected;
  };
  constexpr std::array scheduleA{1u, 2u, 3u};
  constexpr std::array scheduleB{3u, 1u, 2u};
  check(selectPrefix(scheduleA) == 2 && selectPrefix(scheduleB) == 2,
        "completion order cannot change the fixed-cap predecessor prefix");

  auto wrapped = candidates.front();
  wrapped.reservationPages = 5;
  const auto wrappedResult =
      dxmt9::render::classifySessionAdmissionDetailed({}, wrapped, limits);
  check(wrappedResult.decision ==
            SessionAdmissionDecision::ProcessCandidateIsolated &&
            wrappedResult.limitingDimension ==
                SessionCapacityDimension::Pages,
        "admission charges actual payload-plus-wrap pages, not payload alone");
}

ActiveRenderContinuationState activeFor(
    const SourceSemanticSummary& summary,
    RenderRoute route) {
  ActiveRenderContinuationState active{
      .key = summary.entryRender,
      .activeWrites = {},
      .flags = ActiveRenderContinuationActive,
  };
  active.key.route = route;
  for (const auto& attachment : active.key.attachments.color) {
    active.activeWrites.add(attachment.handle.value);
  }
  active.activeWrites.add(active.key.attachments.depthStencil.handle.value);
  active.activeWrites.flags |= dxmt9::core::ExactResourceSetCanonicalized;
  return active;
}

void continuationIsConservativeAndRouteAsymmetric() {
  ChunkSlot portableSlot{};
  appendDraw(portableSlot, 0x10);
  SourceSemanticSummary portable = summarize(
      portableSlot, RenderRoute::Portable, true);
  ActiveRenderContinuationState active =
      activeFor(portable, RenderRoute::Portable);
  checkEq(dxmt9::render::classifyRenderContinuation(active, portable),
          RenderContinuationDecision::ContinueProven,
          "canonical same-key portable draw can continue");

  SourceSemanticSummary tile = portable;
  tile.entryRender.route = RenderRoute::Tile;
  checkEq(dxmt9::render::classifyRenderContinuation(active, tile),
          RenderContinuationDecision::ContinueProven,
          "portable active pass can execute a tile-eligible draw");

  active.key.route = RenderRoute::Tile;
  checkEq(dxmt9::render::classifyRenderContinuation(active, portable),
          RenderContinuationDecision::ClosePass,
          "tile active pass splits before portable-required work");
  checkEq(dxmt9::render::classifyRenderContinuation(active, tile),
          RenderContinuationDecision::ContinueProven,
          "tile active pass continues tile-eligible work");

  SourceSemanticSummary unknown = tile;
  unknown.entryRender.route = RenderRoute::Unknown;
  checkEq(dxmt9::render::classifyRenderContinuation(active, unknown),
          RenderContinuationDecision::ExactReplayRequired,
          "unknown route defers to the existing encoder checks");

  SourceSemanticSummary unstable = tile;
  unstable.flags &= ~dxmt9::core::SourceSemanticEntryStable;
  checkEq(dxmt9::render::classifyRenderContinuation(active, unstable),
          RenderContinuationDecision::ExactReplayRequired,
          "DCE-unstable entry defers to exact replay");

  SourceSemanticSummary aliasUnknown = tile;
  aliasUnknown.entryRender.entryReads.flags &=
      ~dxmt9::core::ExactResourceSetCanonicalized;
  checkEq(dxmt9::render::classifyRenderContinuation(active, aliasUnknown),
          RenderContinuationDecision::ExactReplayRequired,
          "uncanonicalized no-overlap proof cannot authorize continuation");

  ChunkSlot hazardSlot{};
  appendDraw(hazardSlot, 0x10, 0x10);
  SourceSemanticSummary hazard = summarize(
      hazardSlot, RenderRoute::Tile, true);
  checkEq(dxmt9::render::classifyRenderContinuation(active, hazard),
          RenderContinuationDecision::ClosePass,
          "incoming read of an active attachment closes the pass");

  ChunkSlot otherTarget{};
  appendDraw(otherTarget, 0x11);
  SourceSemanticSummary changed = summarize(
      otherTarget, RenderRoute::Tile, true);
  checkEq(dxmt9::render::classifyRenderContinuation(active, changed),
          RenderContinuationDecision::ClosePass,
          "attachment change closes only the pass");

  ChunkSlot clearSlot{};
  clearSlot.appendClear({});
  checkEq(dxmt9::render::classifyRenderContinuation(
              active, summarize(clearSlot)),
          RenderContinuationDecision::ClosePass,
          "entry clear is an explicit pass boundary");

  CanonicalDrawState wideState{};
  wideState.hot.colorAttachments[0].handle = Handle{0x10};
  wideState.hot.colorAttachments[0].sampleCount = 1;
  wideState.hot.indexBuffer = Handle{0x100};
  for (std::size_t i = 0;
       i < dxmt9::core::kSourceEntryResourceCapacity;
       ++i) {
    wideState.hot.streamBuffers[i] = Handle{0x200 + i};
  }
  std::array<DrawParam, 1> wideDraw{};
  ChunkSlot wideSlot{};
  wideSlot.appendDrawRun(std::move(wideState), {}, wideDraw, {});
  const SourceSemanticSummary wide = summarize(
      wideSlot, RenderRoute::Portable, true);
  check(!wide.entryRender.entryReads.complete() &&
            !wide.entryRender.entryStateComplete() &&
            (wide.entryRender.entryReads.flags &
             dxmt9::core::ExactResourceSetOverflow) != 0,
        "wide entry read sets overflow to an explicitly incomplete proof");
  checkEq(dxmt9::render::classifyRenderContinuation(active, wide),
          RenderContinuationDecision::ExactReplayRequired,
          "resource overflow can never authorize optimistic continuation");
}

}  // namespace

int main() {
  try {
    sourceSummaryIsFlatAndClassifiesBoundaries();
    admissionSeparatesOrderingCapsAndIsolation();
    leaseHeadroomAndCapGroupingAreDeterministic();
    continuationIsConservativeAndRouteAsymmetric();
  } catch (const TestFailure& error) {
    std::cerr << "encode_session_admission_spec failed: "
              << error.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "encode_session_admission_spec unexpected error: "
              << error.what() << '\n';
    return 1;
  }
  std::cout << "encode_session_admission_spec passed\n";
  return 0;
}
