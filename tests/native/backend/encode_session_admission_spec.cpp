#include "../../../src/dxmt9/dxmt9_cpu_ready_tape.hpp"
#include "../../../src/dxmt9/dxmt9_source_semantics.hpp"
#include "../../../src/dxmt9/render/deferred_terminal_suffix.hpp"
#include "../../../src/dxmt9/render/encode_scheduling_progress.hpp"
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
using dxmt9::core::CpuReadyTapeConfig;
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
using dxmt9::render::DeferredTerminalSuffixDecision;
using dxmt9::render::DeferredTerminalSuffixDecisionReason;
using dxmt9::render::DeferredTerminalSuffixObservation;
using dxmt9::render::DeferredTerminalSuffixPhase;
using dxmt9::render::DeferredTerminalSuffixSourceIdentity;
using dxmt9::render::RenderContinuationDecision;
using dxmt9::render::SessionAdmissionCandidate;
using dxmt9::render::SessionAdmissionDecision;
using dxmt9::render::SessionCapacityDimension;
using dxmt9::render::SessionCapacityLeaseState;
using dxmt9::render::SessionCapacityPolicy;
using dxmt9::render::SessionCapacityVector;
using dxmt9::render::SessionResidencyByteCharge;
using dxmt9::render::sessionTapeByteCharge;

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
      .residencyBytes = {.value = semantic.byteCount},
      .rawOrdinal = ordinal,
      .sourceOrdinal = ordinal,
      .seqId = seqId,
      .predictedCommandBuffers = 1,
  };
}

DeferredTerminalSuffixSourceIdentity suffixIdentity(
    std::uint32_t sourceIndex,
    std::uint32_t firstPage,
    std::uint64_t generation,
    std::uint64_t ordinal) {
  return {
      .source = {
          .id = {.index = sourceIndex, .generation = generation},
          .storage = {
              .firstPage = firstPage,
              .pageCount = 2,
              .generation = generation,
          },
      },
      .rawOrdinal = ordinal,
      .sourceOrdinal = ordinal,
      .seqId = ordinal,
  };
}

dxmt9::framegraph::DeferredTerminalSuffixProof suffixProof(
    const DeferredTerminalSuffixSourceIdentity& current,
    const DeferredTerminalSuffixSourceIdentity& successor) {
  const auto range = [](
                         const DeferredTerminalSuffixSourceIdentity& source,
                         std::uint32_t begin,
                         std::uint32_t count) {
    return dxmt9::framegraph::SourceCommandRange{
        .source = source.source,
        .sourceOrdinal = source.sourceOrdinal,
        .seqId = source.seqId,
        .commandBegin = begin,
        .commandCount = count,
    };
  };
  return {
      .currentPrefix = range(current, 0u, 1u),
      .currentSuffix = range(current, 1u, 2u),
      .successorHead = range(successor, 0u, 1u),
  };
}

dxmt9::core::metalqueue::ReadySlotSnapshot suffixReadySnapshot(
    const DeferredTerminalSuffixSourceIdentity& identity,
    const SessionAdmissionCandidate& admission,
    const dxmt9::framegraph::DeferredTerminalSuffixProof& proof) {
  return {
      .slotIndex = 12u,
      .seqId = identity.seqId,
      .metadata = {
          .rawOrdinal = identity.rawOrdinal,
          .sourceOrdinal = identity.sourceOrdinal,
          .seqId = identity.seqId,
          .usedBytes = admission.semantic.byteCount,
          .pageCount = identity.source.storage.pageCount,
      },
      .semantic = admission.semantic,
      .hasPresent = admission.semantic.hasPresent(),
      .commandBegin = proof.currentPrefix.commandBegin,
      .commandCount = static_cast<std::size_t>(
          proof.currentPrefix.commandCount + proof.currentSuffix.commandCount),
      .sourceId = identity.source.id,
      .storage = identity.source.storage,
  };
}

dxmt9::core::metalqueue::QueueCompletionSource suffixCompletionSource(
    const DeferredTerminalSuffixSourceIdentity& identity,
    const dxmt9::core::metalqueue::ReadySlotSnapshot& snapshot) {
  return {
      .source = identity.source,
      .slotIndex = snapshot.slotIndex,
      .seqId = identity.seqId,
      .hasPresent = snapshot.hasPresent,
      .commandBegin = snapshot.commandBegin,
      .commandCount = snapshot.commandCount,
  };
}

dxmt9::core::CpuReadyTape::LeaseCapacityClaim suffixWritingClaim(
    const SessionCapacityVector& capacity) {
  return {
      .sources = capacity.sources,
      .pages = capacity.pages,
      .bytes = capacity.bytes,
      .payloadBlocks = capacity.payloadBlocks,
      .readyEntries = capacity.readyEntries,
      .retentionEntries = capacity.retentionEntries,
      .allocatorTickets = capacity.allocatorTickets,
  };
}

SessionCapacityVector suffixWritingCapacity(
    const SessionCapacityVector& capacity) {
  auto result = capacity;
  result.draws = 0u;
  result.commandBuffers = 0u;
  return result;
}

SessionCapacityPolicy suffixCapacityPolicy(
    const SessionCapacityVector& current,
    const SessionCapacityVector& successor) {
  const auto maxSession =
      dxmt9::render::addSessionCapacity(current, successor);
  check(maxSession.has_value(), "suffix session capacity adds exactly");
  auto successorHeadroom = successor;
  successorHeadroom.pages =
      dxmt9::render::worstCaseNonWrappingReservationPages(
          successor.pages);
  const auto highWater = dxmt9::render::addSessionCapacity(
      *maxSession, successorHeadroom);
  check(highWater.has_value(), "suffix high-water capacity adds exactly");
  return {
      .highWater = *highWater,
      .maxSession = *maxSession,
      .successorHeadroom = successorHeadroom,
      .ordinaryDirect = current,
  };
}

std::optional<dxmt9::render::DeferredTerminalSuffixState>
makeHeldSuffix(
    const DeferredTerminalSuffixSourceIdentity& current,
    const DeferredTerminalSuffixSourceIdentity& successor,
    const SessionAdmissionCandidate& currentAdmission,
    const dxmt9::render::SessionCapacityLease& lease,
    const SessionCapacityVector& successorHeadroom) {
  const auto proof = suffixProof(current, successor);
  const auto currentSnapshot =
      suffixReadySnapshot(current, currentAdmission, proof);
  const auto currentCompletion =
      suffixCompletionSource(current, currentSnapshot);
  const auto prefixEncoded =
      dxmt9::render::makeDeferredTerminalSuffixPrefixEncoded(
          current, currentSnapshot, currentCompletion, currentAdmission,
          dxmt9::render::sessionCapacityFor(currentAdmission),
          proof.currentPrefix, proof.currentSuffix, successor.source,
          suffixWritingClaim(dxmt9::render::sessionCapacityFor(
              candidate(successor.sourceOrdinal, successor.seqId,
                        currentAdmission.semantic))),
          lease, successorHeadroom, {});
  if (!prefixEncoded) {
    return std::nullopt;
  }
  return dxmt9::render::holdDeferredTerminalSuffix(*prefixEncoded);
}

void sourceSummaryIsFlatAndClassifiesBoundaries() {
  static_assert(std::is_trivially_copyable_v<SourceSemanticSummary>);
  static_assert(std::is_standard_layout_v<SourceSemanticSummary>);
  static_assert(std::is_trivially_copyable_v<SessionAdmissionCandidate>);
  static_assert(std::is_standard_layout_v<SessionAdmissionCandidate>);
  static_assert(std::is_trivially_copyable_v<SessionResidencyByteCharge>);
  static_assert(std::is_standard_layout_v<SessionResidencyByteCharge>);
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
  logicalSummary.byteCount = logicalBytes * 1024u;
  const EncodeSessionLimits byteLimited{
      .maxSources = 2,
      .maxPages = 4,
      .maxBytes = 8192,
      .maxDraws = 2,
      .maxCommandBuffers = 2,
  };
  check(logicalSummary.byteCount > byteLimited.maxBytes,
        "Legacy logical extent exceeds the Arena-derived byte cap");
  auto boundedLegacy = candidate(1, 1, logicalSummary);
  boundedLegacy.residencyBytes = SessionResidencyByteCharge{.value = 4096};
  checkEq(dxmt9::render::classifySessionAdmission(
              {}, boundedLegacy, byteLimited),
          SessionAdmissionDecision::Admit,
          "large logical Legacy extent does not consume Arena byte credit");
  checkEq(dxmt9::render::sessionCapacityFor(boundedLegacy).bytes,
          std::uint64_t{4096},
          "session capacity retains the typed compatibility residency charge");
  checkEq(sessionTapeByteCharge(false, logicalSummary.byteCount, 1, 4096),
          SessionResidencyByteCharge{.value = 4096},
          "production Legacy mapping charges its reserved Tape page");
  checkEq(sessionTapeByteCharge(true, 6145, 2, 4096),
          SessionResidencyByteCharge{.value = 6145},
          "production Arena mapping charges exact constructed Tape bytes");

  EncodeSessionAdmissionState compatibilitySession{};
  check(dxmt9::render::appendSessionAdmission(
            compatibilitySession, boundedLegacy, byteLimited),
        "bounded compatibility source starts a session");
  auto secondLegacy = boundedLegacy;
  secondLegacy.rawOrdinal = 2;
  secondLegacy.sourceOrdinal = 2;
  secondLegacy.seqId = 2;
  checkEq(dxmt9::render::classifySessionAdmission(
              compatibilitySession, secondLegacy, byteLimited),
          SessionAdmissionDecision::Admit,
          "logical Legacy extents do not fragment a bounded session");
  check(dxmt9::render::appendSessionAdmission(
            compatibilitySession, secondLegacy, byteLimited),
        "second bounded compatibility source appends cumulatively");
  checkEq(compatibilitySession.residencyBytes,
          SessionResidencyByteCharge{.value = 8192},
          "two Legacy Tape-page charges exactly fill the session byte cap");

  auto thirdLegacy = secondLegacy;
  thirdLegacy.rawOrdinal = 3;
  thirdLegacy.sourceOrdinal = 3;
  thirdLegacy.seqId = 3;
  const EncodeSessionLimits cumulativeByteLimits{
      .maxSources = 3,
      .maxPages = 6,
      .maxBytes = 8192,
      .maxDraws = 2,
      .maxCommandBuffers = 3,
  };
  const auto cumulativeResult =
      dxmt9::render::classifySessionAdmissionDetailed(
          compatibilitySession, thirdLegacy, cumulativeByteLimits);
  checkEq(cumulativeResult.decision,
          SessionAdmissionDecision::SubmitPrefixBeforeCandidate,
          "next Legacy Tape page deterministically caps a full session");
  checkEq(cumulativeResult.limitingDimension,
          SessionCapacityDimension::Bytes,
          "next Legacy Tape page reports the exhausted byte credit");

  auto physicallyOversize = boundedLegacy;
  physicallyOversize.residencyBytes =
      SessionResidencyByteCharge{.value = 8193};
  const auto physicalResult =
      dxmt9::render::classifySessionAdmissionDetailed(
          {}, physicallyOversize, byteLimited);
  checkEq(physicalResult.decision,
          SessionAdmissionDecision::ProcessCandidateIsolated,
          "physical residency over the byte cap isolates deterministically");
  checkEq(physicalResult.limitingDimension,
          SessionCapacityDimension::Bytes,
          "physical byte isolation reports the capacity dimension");
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
  const SessionCapacityVector writingClaim{1, 4, 400, 0, 1, 1, 1, 1, 0};
  const dxmt9::render::SessionCapacityLeaseAcquisitionSnapshot credited{
      .olderUnavailable = {},
      .orderedTailWritingSuccessor = writingClaim,
  };
  const auto creditedUnavailable =
      dxmt9::render::sessionCapacityUnavailableForFirstLease(
          credited, policy.successorHeadroom);
  check(creditedUnavailable.has_value() &&
            *creditedUnavailable == SessionCapacityVector{},
        "an ordinary ordered-tail Writing claim is credited once against "
        "successor headroom");
  SessionCapacityLeaseState creditedLease;
  check(creditedLease.acquire(policy, *creditedUnavailable, firstCharge) &&
            creditedLease.lease().reserved == policy.highWater &&
            creditedLease.lease().successorRemaining ==
                policy.successorHeadroom,
        "Writing credit does not shrink the fixed acquired lease");
  check(creditedLease.release(1) &&
            creditedLease.acquire(policy, {}, firstCharge) &&
            creditedLease.lease().generation == 2,
        "credited acquisition and release preserve monotonic lease generations");

  auto oversizedWriting = credited;
  oversizedWriting.orderedTailWritingSuccessor->pages =
      policy.successorHeadroom.pages + 1u;
  const auto oversizedUnavailable =
      dxmt9::render::sessionCapacityUnavailableForFirstLease(
          oversizedWriting, policy.successorHeadroom);
  check(oversizedUnavailable.has_value() &&
            oversizedUnavailable->sources == 1u &&
            oversizedUnavailable->pages ==
                policy.successorHeadroom.pages + 1u,
        "a Writing claim beyond successor headroom remains unavailable");
  SessionCapacityLeaseState oversizedLease;
  check(!oversizedLease.acquire(policy, *oversizedUnavailable, firstCharge) &&
            !oversizedLease.lease().valid(),
        "an oversized Writing claim cannot acquire the fixed lease");

  auto overflowingWriting = oversizedWriting;
  overflowingWriting.olderUnavailable.sources =
      std::numeric_limits<std::uint64_t>::max();
  overflowingWriting.orderedTailWritingSuccessor->sources = 1;
  check(!dxmt9::render::sessionCapacityUnavailableForFirstLease(
             overflowingWriting, policy.successorHeadroom).has_value(),
        "overflow while retaining an ineligible Writing claim rejects the "
        "acquisition snapshot");
  auto invalidSnapshot = credited;
  invalidSnapshot.valid = false;
  check(!dxmt9::render::sessionCapacityUnavailableForFirstLease(
             invalidSnapshot, policy.successorHeadroom).has_value(),
        "invalid Tape snapshot arithmetic cannot be credited");

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

void productionPageCapacityLeavesExactSuccessorHeadroom() {
  const auto config = CpuReadyTapeConfig::queueSessionStreaming(32);
  const auto& values = config.values();
  const std::uint64_t successorPages =
      dxmt9::render::worstCaseNonWrappingReservationPages(
          values.maxPagesPerSource);
  const std::uint64_t sessionPages =
      values.highWaterPages - successorPages;

  check(values.pageCount == 640u && values.highWaterPages == 640u &&
            values.lowWaterPages == 320u &&
            values.maxPagesPerSource == 64u,
        "32-control streaming profile exposes the bounded page policy");
  check(successorPages == 127u && sessionPages == 513u,
        "one non-wrapping successor leaves the exact session page cap");

  const SessionCapacityPolicy policy{
      .highWater = {2, values.highWaterPages, 2, 2, 2, 2, 2, 2, 2},
      .maxSession = {1, sessionPages, 1, 1, 1, 1, 1, 1, 1},
      .successorHeadroom = {1, successorPages, 1, 1, 1, 1, 1, 1, 1},
      .ordinaryDirect = {
          1, values.maxPagesPerSource, 1, 1, 1, 1, 1, 1, 1},
  };
  check(policy.valid(),
        "session cap plus exact successor headroom fills page high-water");
}

void firstLeaseOrdinaryShapeExcludesWrapPadding() {
  ChunkSlot slot{};
  appendDraw(slot, 0x20);
  auto semantic = summarize(slot);
  semantic.pageCount = 64u;
  auto wrapped = candidate(1u, 1u, semantic);
  wrapped.reservationPages = 127u;

  const auto view =
      dxmt9::render::firstLeaseReadyHeadCapacityViewFor(wrapped);
  check(view.ordinaryShape.pages == 64u &&
            view.fullReservation.pages == 127u &&
            view.ordinaryShape.bytes == view.fullReservation.bytes &&
            dxmt9::render::sessionCapacityFor(wrapped) ==
                view.fullReservation &&
            dxmt9::render::sessionPhysicalResidencyCapacityFor(wrapped).pages ==
                127u,
        "typed first-lease view separates semantic payload pages from the "
        "unchanged full reservation/high-water charge");

  auto ordinaryDirect = view.ordinaryShape;
  auto highWater = view.fullReservation;
  const auto classify = [&] {
    return dxmt9::render::classifyFirstLeaseReadyHeadEligibility({
        .arena = true,
        .present = false,
        .fitsOrdinaryCapacity = dxmt9::render::sessionCapacityFitsWithin(
            view.ordinaryShape, ordinaryDirect),
        .fitsHighWater = dxmt9::render::sessionCapacityFitsWithin(
            view.fullReservation, highWater),
    });
  };
  check(classify() ==
            dxmt9::render::FirstLeaseReadyHeadEligibility::Eligible,
        "wrap-padded max-payload source is ordinary-shape eligible while its "
        "full reservation passes high-water");

  highWater.pages = 126u;
  check(classify() ==
            dxmt9::render::FirstLeaseReadyHeadEligibility::HighWater,
        "ordinary payload eligibility cannot bypass the independent full "
        "reservation high-water proof");

  highWater = view.fullReservation;
  ordinaryDirect.pages = 63u;
  check(classify() ==
            dxmt9::render::FirstLeaseReadyHeadEligibility::OrdinaryCapacity,
        "payload shape above ordinaryDirect remains ineligible even when the "
        "full reservation fits high-water");
}

void capacityRejectionObservationDistinguishesAxesAndWrapPadding() {
  ChunkSlot slot{};
  appendDraw(slot, 0x18);
  const SourceSemanticSummary semantic = summarize(slot);
  const EncodeSessionLimits roomy{
      .maxSources = 4,
      .maxPages = 16,
      .maxBytes = 65536,
      .maxDraws = 4,
      .maxCommandBuffers = 4,
  };
  EncodeSessionAdmissionState predecessor{};
  check(dxmt9::render::appendSessionAdmission(
            predecessor, candidate(1, 1, semantic), roomy),
        "capacity observation fixture starts one predecessor source");

  const auto next = candidate(2, 2, semantic);
  const EncodeSessionLimits sourceOnlyLimits{
      .maxSources = 1,
      .maxPages = 8,
      .maxBytes = 65536,
      .maxDraws = 4,
      .maxCommandBuffers = 4,
  };
  const auto sourceOnly =
      dxmt9::render::classifySessionAdmissionDetailed(
          predecessor, next, sourceOnlyLimits);
  check(sourceOnly.decision ==
            SessionAdmissionDecision::SubmitPrefixBeforeCandidate &&
            sourceOnly.limitingDimension ==
                SessionCapacityDimension::Sources &&
            sourceOnly.capacityRejection.sourcesExceeded() &&
            !sourceOnly.capacityRejection.pagesExceeded(),
        "source-only cap rejection has exclusive axis attribution");
  check(sourceOnly.capacityRejection.predecessorSources == 1u &&
            sourceOnly.capacityRejection.predecessorPages == 2u &&
            sourceOnly.capacityRejection.candidatePayloadPages == 2u &&
            sourceOnly.capacityRejection.candidateWrapPaddingPages == 0u &&
            sourceOnly.capacityRejection.candidateRequiredPages == 2u &&
            sourceOnly.capacityRejection.requiredTotalSources == 2u &&
            sourceOnly.capacityRejection.requiredTotalPages == 4u,
        "source-only observation retains exact predecessor and payload demand");

  auto wrapped = next;
  wrapped.reservationPages = 5;
  const EncodeSessionLimits pageOnlyLimits{
      .maxSources = 4,
      .maxPages = 6,
      .maxBytes = 65536,
      .maxDraws = 4,
      .maxCommandBuffers = 4,
  };
  const auto pageOnly =
      dxmt9::render::classifySessionAdmissionDetailed(
          predecessor, wrapped, pageOnlyLimits);
  check(pageOnly.decision ==
            SessionAdmissionDecision::SubmitPrefixBeforeCandidate &&
            pageOnly.limitingDimension == SessionCapacityDimension::Pages &&
            !pageOnly.capacityRejection.sourcesExceeded() &&
            pageOnly.capacityRejection.pagesExceeded(),
        "page-only cap rejection has exclusive axis attribution");
  check(pageOnly.capacityRejection.predecessorSources == 1u &&
            pageOnly.capacityRejection.predecessorPages == 2u &&
            pageOnly.capacityRejection.candidatePayloadPages == 2u &&
            pageOnly.capacityRejection.candidateWrapPaddingPages == 3u &&
            pageOnly.capacityRejection.candidateRequiredPages == 5u &&
            pageOnly.capacityRejection.requiredTotalSources == 2u &&
            pageOnly.capacityRejection.requiredTotalPages == 7u,
        "page observation separates payload, wrap padding, and required total");

  const EncodeSessionLimits bothLimits{
      .maxSources = 1,
      .maxPages = 6,
      .maxBytes = 65536,
      .maxDraws = 4,
      .maxCommandBuffers = 4,
  };
  const auto both = dxmt9::render::classifySessionAdmissionDetailed(
      predecessor, wrapped, bothLimits);
  check(both.decision ==
            SessionAdmissionDecision::SubmitPrefixBeforeCandidate &&
            both.limitingDimension == SessionCapacityDimension::Sources &&
            both.capacityRejection.sourcesExceeded() &&
            both.capacityRejection.pagesExceeded() &&
            both.capacityRejection.requiredTotalSources == 2u &&
            both.capacityRejection.requiredTotalPages == 7u,
        "combined source/page rejection preserves both axes despite first-axis "
        "compatibility attribution");
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

void multiSourceWindowPreflightIsStrictAndTransactional() {
  ChunkSlot slot{};
  appendDraw(slot, 0x10);
  const SourceSemanticSummary semantic = summarize(slot);
  const EncodeSessionLimits limits{
      .maxSources = 8,
      .maxPages = 64,
      .maxBytes = 64u * 4096u,
      .maxDraws = 64,
      .maxCommandBuffers = 8,
  };
  EncodeSessionAdmissionState pending{};
  auto head = candidate(1, 1, semantic);
  head.reservationPages = 1;
  check(dxmt9::render::appendSessionAdmission(pending, head, limits),
        "multi-source preflight fixture starts one pending session");
  const EncodeSessionAdmissionState unchanged = pending;

  std::array candidates{
      candidate(2, 2, semantic),
      candidate(3, 3, semantic),
  };
  candidates[0].reservationPages = 1;
  candidates[1].reservationPages = 1;
  const auto eligible = dxmt9::render::preflightMultiSourceSessionWindow(
      pending, candidates, limits,
      dxmt9::render::MultiSourceSessionWindowFrontier::ActiveRenderComplete,
      false, false, false);
  check(eligible.eligible() && eligible.stagedAdmission.sources == 3 &&
            eligible.stagedAdmission.lastSeqId == 3,
        "complete consecutive B|A window stages both sources");
  checkEq(pending, unchanged,
          "successful preflight does not mutate the pending admission");

  auto freshCandidates = candidates;
  freshCandidates[0].sourceOrdinal = 40;
  freshCandidates[0].seqId = 50;
  freshCandidates[0].rawOrdinal = 60;
  freshCandidates[1].sourceOrdinal = 41;
  freshCandidates[1].seqId = 51;
  freshCandidates[1].rawOrdinal = 61;
  const auto fresh = dxmt9::render::preflightMultiSourceSessionWindow(
      {}, freshCandidates, limits,
      dxmt9::render::MultiSourceSessionWindowFrontier::FreshClean,
      false, false, false);
  check(fresh.eligible() && fresh.stagedAdmission.sources == 2 &&
            fresh.stagedAdmission.lastSourceOrdinal == 41 &&
            fresh.stagedAdmission.lastSeqId == 51,
        "fresh clean preflight admits any valid head then exact successors");
  const auto mismatchedFresh =
      dxmt9::render::preflightMultiSourceSessionWindow(
          pending, freshCandidates, limits,
          dxmt9::render::MultiSourceSessionWindowFrontier::FreshClean,
          false, false, false);
  check(!mismatchedFresh.eligible(),
        "fresh frontier rejects a non-empty carried admission base");
  const auto unsupported =
      dxmt9::render::preflightMultiSourceSessionWindow(
          {}, freshCandidates, limits,
          dxmt9::render::MultiSourceSessionWindowFrontier::Unsupported,
          false, false, false);
  check(!unsupported.eligible(),
        "unsafe replay frontiers cannot stage a fresh window");

  auto forwardRawGap = candidates;
  forwardRawGap[0].rawOrdinal = 10;
  forwardRawGap[1].rawOrdinal = 12;
  const auto forwardRawGapEligible =
      dxmt9::render::preflightMultiSourceSessionWindow(
          pending, forwardRawGap, limits,
          dxmt9::render::MultiSourceSessionWindowFrontier::
              ActiveRenderComplete,
          false, false, false);
  check(forwardRawGapEligible.eligible(),
        "forward raw gaps do not imply missing CPU-ready sources");

  auto missingRaw = candidates;
  missingRaw[0].rawOrdinal = 0;
  missingRaw[1].rawOrdinal = 0;
  const auto missingRawEligible =
      dxmt9::render::preflightMultiSourceSessionWindow(
          pending, missingRaw, limits,
          dxmt9::render::MultiSourceSessionWindowFrontier::
              ActiveRenderComplete,
          false, false, false);
  check(missingRawEligible.eligible() &&
            missingRawEligible.stagedAdmission.lastRawOrdinal == 1,
        "missing raw identity remains an optional diagnostic coordinate");

  auto sourceGap = candidates;
  sourceGap[1].sourceOrdinal = 4;
  const auto nonConsecutiveSource =
      dxmt9::render::preflightMultiSourceSessionWindow(
          pending, sourceGap, limits,
          dxmt9::render::MultiSourceSessionWindowFrontier::
              ActiveRenderComplete,
          false, false, false);
  checkEq(nonConsecutiveSource.reason,
          dxmt9::render::MultiSourceSessionWindowPreflightReason::
              NonConsecutiveIdentity,
          "an independent source-ordinal gap rejects the window");

  auto seqGap = candidates;
  seqGap[1].seqId = 4;
  const auto nonConsecutiveSeq =
      dxmt9::render::preflightMultiSourceSessionWindow(
          pending, seqGap, limits,
          dxmt9::render::MultiSourceSessionWindowFrontier::
              ActiveRenderComplete,
          false, false, false);
  checkEq(nonConsecutiveSeq.reason,
          dxmt9::render::MultiSourceSessionWindowPreflightReason::
              NonConsecutiveIdentity,
          "an independent sequence-ID gap rejects the window");

  auto nonMonotonicRaw = candidates;
  nonMonotonicRaw[0].rawOrdinal = 10;
  nonMonotonicRaw[1].rawOrdinal = 9;
  const auto nonMonotonicRawIdentity =
      dxmt9::render::preflightMultiSourceSessionWindow(
          pending, nonMonotonicRaw, limits,
          dxmt9::render::MultiSourceSessionWindowFrontier::
              ActiveRenderComplete,
          false, false, false);
  checkEq(nonMonotonicRawIdentity.reason,
          dxmt9::render::MultiSourceSessionWindowPreflightReason::
              NonConsecutiveIdentity,
          "comparable nonzero raw identities must remain monotonic");

  auto missingBetweenObservedRaw = candidates;
  missingBetweenObservedRaw[0].rawOrdinal = 0;
  missingBetweenObservedRaw[1].rawOrdinal = 1;
  const auto staleAfterMissingRaw =
      dxmt9::render::preflightMultiSourceSessionWindow(
          pending, missingBetweenObservedRaw, limits,
          dxmt9::render::MultiSourceSessionWindowFrontier::
              ActiveRenderComplete,
          false, false, false);
  checkEq(staleAfterMissingRaw.reason,
          dxmt9::render::MultiSourceSessionWindowPreflightReason::
              NonConsecutiveIdentity,
          "missing raw identity does not erase the last observed coordinate");
  checkEq(pending, unchanged,
          "identity rejection leaves the pending admission unchanged");

  auto present = candidates;
  present[1].semantic.flags |= dxmt9::core::SourceSemanticHasPresent;
  const auto presentBoundary =
      dxmt9::render::preflightMultiSourceSessionWindow(
          pending, present, limits,
          dxmt9::render::MultiSourceSessionWindowFrontier::
              ActiveRenderComplete,
          false, false, false);
  checkEq(presentBoundary.reason,
          dxmt9::render::MultiSourceSessionWindowPreflightReason::
              PresentBoundary,
          "Present never enters the replay window");

  const auto orderedBoundary =
      dxmt9::render::preflightMultiSourceSessionWindow(
          pending, candidates, limits,
          dxmt9::render::MultiSourceSessionWindowFrontier::
              ActiveRenderComplete,
          false, false, true);
  checkEq(orderedBoundary.reason,
          dxmt9::render::MultiSourceSessionWindowPreflightReason::
              OrderedReleaseBoundary,
          "ordered control fences exclude the whole replay window");
}

void retirementSplitsResidencyCreditFromEncodedWorkCap() {
  ChunkSlot clear{};
  clear.appendClear({});
  const SourceSemanticSummary semantic = summarize(clear);
  const EncodeSessionLimits limits{
      .maxSources = 128u,
      .maxPages = semantic.pageCount,
      .maxBytes = semantic.byteCount,
      .maxDraws = std::numeric_limits<std::uint32_t>::max(),
      .maxCommandBuffers = 128u,
  };
  EncodeSessionAdmissionState session{};
  for (std::uint64_t seqId = 1u; seqId <= limits.maxSources; ++seqId) {
    const auto source = candidate(seqId, seqId, semantic);
    check(dxmt9::render::appendSessionAdmission(session, source, limits),
          "retired physical credit must admit work through the fixed cap");
    check(dxmt9::render::retireSessionAdmissionResidency(session, source),
          "post-encode retirement releases physical admission credit");
    checkEq(session.sources, static_cast<std::uint32_t>(seqId),
            "encoded-unsubmitted work remains charged after retirement");
    check(session.residentSources == 0u && session.pages == 0u &&
              session.residencyBytes.value == 0u,
          "publication/Tape residency credit returns independently");
    checkEq(session.commandBuffers, static_cast<std::uint32_t>(seqId),
            "bounded command-buffer work credit remains charged");
  }

  const auto successor = candidate(129u, 129u, semantic);
  const auto capped = dxmt9::render::classifySessionAdmissionDetailed(
      session, successor, limits);
  checkEq(capped.decision,
          SessionAdmissionDecision::SubmitPrefixBeforeCandidate,
          "the deterministic work cap closes before source 129");
  check(capped.limitingDimension == SessionCapacityDimension::Sources ||
            capped.limitingDimension ==
                SessionCapacityDimension::CommandBuffers,
        "work-cap attribution names a logical, not residency, dimension");
}

void deferredTerminalSuffixQualificationIsExactAndBounded() {
  static_assert(std::is_trivially_copyable_v<
                dxmt9::render::DeferredTerminalSuffixState>);
  static_assert(std::is_standard_layout_v<
                dxmt9::render::DeferredTerminalSuffixState>);
  static_assert(std::is_trivially_copyable_v<
                DeferredTerminalSuffixSourceIdentity>);
  static_assert(!std::is_pointer_v<decltype(
                DeferredTerminalSuffixSourceIdentity::source)>);

  ChunkSlot slot{};
  appendDraw(slot, 0x10);
  const auto semantic = summarize(slot);
  const auto current = suffixIdentity(2u, 4u, 7u, 10u);
  const auto successor = suffixIdentity(3u, 6u, 9u, 11u);
  const auto currentAdmission = candidate(10u, 10u, semantic);
  const auto successorAdmission = candidate(11u, 11u, semantic);
  const auto currentCharge =
      dxmt9::render::sessionCapacityFor(currentAdmission);
  const auto successorClaim =
      dxmt9::render::sessionCapacityFor(successorAdmission);
  const auto policy = suffixCapacityPolicy(currentCharge, successorClaim);
  check(policy.valid(), "bounded suffix lease policy is valid");
  const dxmt9::render::SessionCapacityLeaseAcquisitionSnapshot snapshot{
      .orderedTailWritingSuccessor =
          suffixWritingCapacity(successorClaim),
  };
  const auto unavailable =
      dxmt9::render::sessionCapacityUnavailableForFirstLease(
          snapshot, policy.successorHeadroom);
  check(unavailable.has_value() && *unavailable == SessionCapacityVector{},
        "exact Writing physical claim is credited at acquisition");
  SessionCapacityLeaseState leaseState;
  check(leaseState.acquire(policy, *unavailable, currentCharge),
        "real lease state admits the current charge");
  const auto lease = leaseState.lease();
  const auto proof = suffixProof(current, successor);
  const auto currentSnapshot =
      suffixReadySnapshot(current, currentAdmission, proof);
  const auto currentCompletion =
      suffixCompletionSource(current, currentSnapshot);

  const auto prefixEncoded =
      dxmt9::render::makeDeferredTerminalSuffixPrefixEncoded(
          current, currentSnapshot, currentCompletion, currentAdmission,
          currentCharge, proof.currentPrefix, proof.currentSuffix,
          successor.source, suffixWritingClaim(successorClaim), lease,
          policy.successorHeadroom, {});
  check(prefixEncoded.has_value() &&
            prefixEncoded->phase ==
                DeferredTerminalSuffixPhase::PrefixEncoded &&
            !prefixEncoded->proofAttached,
        "prefix state parks without a successor payload proof");
  const auto held = dxmt9::render::holdDeferredTerminalSuffix(
      *prefixEncoded);
  check(held.has_value(),
        "exact current ranges, Writing source, and physical headroom hold");
  checkEq(held->phase, DeferredTerminalSuffixPhase::Held,
          "qualification enters the bounded Held phase");
  checkEq(held->leaseGeneration, lease.generation,
          "held state snapshots the exact lease generation");
  checkEq(held->leaseUsedBeforeSuccessor, currentCharge,
          "current work and residency are represented by one lease charge");
  check(!held->proofAttached &&
            held->successorClaim == SessionCapacityVector{},
        "Held retains neither proof nor invented logical successor charge");

  const auto narrowHighWater = dxmt9::render::addSessionCapacity(
      currentCharge, policy.successorHeadroom);
  check(narrowHighWater.has_value(), "narrow lease policy adds exactly");
  const SessionCapacityPolicy narrowPolicy{
      .highWater = *narrowHighWater,
      .maxSession = currentCharge,
      .successorHeadroom = policy.successorHeadroom,
      .ordinaryDirect = currentCharge,
  };
  SessionCapacityLeaseState narrowLeaseState;
  check(narrowPolicy.valid() &&
            narrowLeaseState.acquire(narrowPolicy, {}, currentCharge),
        "the formerly accepted current-only usable lease is producible");
  check(!dxmt9::render::makeDeferredTerminalSuffixPrefixEncoded(
             current, currentSnapshot, currentCompletion, currentAdmission,
             currentCharge, proof.currentPrefix, proof.currentSuffix,
             successor.source, suffixWritingClaim(successorClaim),
             narrowLeaseState.lease(), narrowPolicy.successorHeadroom, {}),
        "Held rejects Writing residency beyond reserved-minus-remaining");
  check(!narrowLeaseState.charge(
             narrowLeaseState.lease().generation, successorClaim),
        "real lease state confirms the old held fixture was impossible");

  check(held->expectedWritingSuccessor.source == successor.source &&
            held->expectedWritingSuccessor.sourceOrdinal ==
                current.sourceOrdinal + 1u &&
            held->expectedWritingSuccessor.seqId == current.seqId + 1u,
        "Writing identity infers exact consecutive ordinals without payload");

  auto staleAdmission = currentAdmission;
  ++staleAdmission.sourceOrdinal;
  check(!dxmt9::render::makeDeferredTerminalSuffixPrefixEncoded(
             current, currentSnapshot, currentCompletion, staleAdmission,
             currentCharge, proof.currentPrefix, proof.currentSuffix,
             successor.source, suffixWritingClaim(successorClaim), lease,
             policy.successorHeadroom, {}),
        "admission identity must match the represented current source");

  const auto doubledCurrent =
      dxmt9::render::addSessionCapacity(currentCharge, currentCharge);
  check(doubledCurrent.has_value(), "duplicate-charge fixture adds");
  check(!dxmt9::render::makeDeferredTerminalSuffixPrefixEncoded(
             current, currentSnapshot, currentCompletion, currentAdmission,
             *doubledCurrent, proof.currentPrefix, proof.currentSuffix,
             successor.source, suffixWritingClaim(successorClaim), lease,
             policy.successorHeadroom, {}),
        "current encoded work cannot be duplicated in held state");

  auto oversizedWriting = suffixWritingClaim(successorClaim);
  oversizedWriting.pages = policy.successorHeadroom.pages + 1u;
  check(!dxmt9::render::makeDeferredTerminalSuffixPrefixEncoded(
             current, currentSnapshot, currentCompletion, currentAdmission,
             currentCharge, proof.currentPrefix, proof.currentSuffix,
             successor.source, oversizedWriting, lease,
             policy.successorHeadroom, {}),
        "the physical Writing claim must fit reserved successor headroom");

  auto staleLease = lease;
  staleLease.generation = 0u;
  check(!dxmt9::render::makeDeferredTerminalSuffixPrefixEncoded(
             current, currentSnapshot, currentCompletion, currentAdmission,
             currentCharge, proof.currentPrefix, proof.currentSuffix,
             successor.source, suffixWritingClaim(successorClaim), staleLease,
             policy.successorHeadroom, {}),
        "a dead lease generation cannot own a held suffix");
}

void deferredTerminalSuffixWakePolicyDrainsWithoutLockedWait() {
  ChunkSlot slot{};
  appendDraw(slot, 0x10);
  const auto semantic = summarize(slot);
  const auto current = suffixIdentity(2u, 4u, 7u, 10u);
  const auto successor = suffixIdentity(3u, 6u, 9u, 11u);
  const auto currentAdmission = candidate(10u, 10u, semantic);
  const auto successorAdmission = candidate(11u, 11u, semantic);
  const auto currentCharge =
      dxmt9::render::sessionCapacityFor(currentAdmission);
  const auto successorClaim =
      dxmt9::render::sessionCapacityFor(successorAdmission);
  const auto policy = suffixCapacityPolicy(currentCharge, successorClaim);
  SessionCapacityLeaseState leaseState;
  check(leaseState.acquire(policy, {}, currentCharge),
        "wake fixture uses a producible lease");
  const auto lease = leaseState.lease();
  const auto held = makeHeldSuffix(
      current, successor, currentAdmission, lease,
      policy.successorHeadroom);
  check(held.has_value(), "wake fixture qualifies");

  DeferredTerminalSuffixObservation observation{
      .currentIdentity = current,
      .currentAdmission = currentAdmission,
      .successor = successor,
      .successorAdmission = successorAdmission,
      .lease = lease,
  };
  const auto idle = dxmt9::render::classifyDeferredTerminalSuffix(
      *held, observation);
  check(idle.mayWait(), "quiescent held state may wait");

  observation.schedulingMutexOwned = true;
  const auto locked = dxmt9::render::classifyDeferredTerminalSuffix(
      *held, observation);
  checkEq(locked.decision, DeferredTerminalSuffixDecision::Invalid,
          "the wait API rejects scheduling-lock ownership");
  checkEq(locked.reason,
          DeferredTerminalSuffixDecisionReason::SchedulingMutexOwned,
          "locked-wait rejection is explicit");
  observation.schedulingMutexOwned = false;

  observation.wakeFlags =
      dxmt9::render::DeferredTerminalSuffixWakeExactSuccessorReady;
  observation.successorAdmission.predictedCommandBuffers = 2u;
  const auto logicalHeadroomExceeded =
      dxmt9::render::classifyDeferredTerminalSuffix(*held, observation);
  checkEq(logicalHeadroomExceeded.decision,
          DeferredTerminalSuffixDecision::NaturalDrain,
          "Ready-derived logical charge must fit successor policy");
  observation.successorAdmission = successorAdmission;
  observation.successorAdmission.key.allocatorPolicyEpoch++;
  const auto incompatibleAdmission =
      dxmt9::render::classifyDeferredTerminalSuffix(*held, observation);
  checkEq(incompatibleAdmission.decision,
          DeferredTerminalSuffixDecision::NaturalDrain,
          "Ready-derived admission must remain session-compatible");
  observation.successorAdmission = successorAdmission;

  struct DrainCase {
    std::uint32_t flag;
    DeferredTerminalSuffixDecisionReason reason;
  };
  constexpr DrainCase drains[] = {
      {dxmt9::render::DeferredTerminalSuffixWakeOrderedRelease,
       DeferredTerminalSuffixDecisionReason::OrderedRelease},
      {dxmt9::render::DeferredTerminalSuffixWakeProducerWait,
       DeferredTerminalSuffixDecisionReason::ProducerWait},
      {dxmt9::render::DeferredTerminalSuffixWakeInitializer,
       DeferredTerminalSuffixDecisionReason::Initializer},
      {dxmt9::render::DeferredTerminalSuffixWakeQuery,
       DeferredTerminalSuffixDecisionReason::Query},
      {dxmt9::render::DeferredTerminalSuffixWakeReadback,
       DeferredTerminalSuffixDecisionReason::Readback},
      {dxmt9::render::DeferredTerminalSuffixWakeUpdateTexture,
       DeferredTerminalSuffixDecisionReason::UpdateTexture},
      {dxmt9::render::DeferredTerminalSuffixWakePresent,
       DeferredTerminalSuffixDecisionReason::Present},
      {dxmt9::render::DeferredTerminalSuffixWakeStop,
       DeferredTerminalSuffixDecisionReason::Stop},
      {dxmt9::render::DeferredTerminalSuffixWakeDeviceLoss,
       DeferredTerminalSuffixDecisionReason::DeviceLoss},
      {dxmt9::render::DeferredTerminalSuffixWakeWriterLost,
       DeferredTerminalSuffixDecisionReason::WriterLost},
      {dxmt9::render::DeferredTerminalSuffixWakeLeaseInvalidated,
       DeferredTerminalSuffixDecisionReason::LeaseInvalidated},
      {dxmt9::render::DeferredTerminalSuffixWakeHeadroomInvalidated,
       DeferredTerminalSuffixDecisionReason::HeadroomInvalidated},
      {dxmt9::render::DeferredTerminalSuffixWakeAdmissionPressure,
       DeferredTerminalSuffixDecisionReason::AdmissionPressure},
      {dxmt9::render::DeferredTerminalSuffixWakeWriterPressure,
       DeferredTerminalSuffixDecisionReason::WriterPressure},
  };
  for (const DrainCase& drain : drains) {
    observation.wakeFlags = drain.flag;
    const auto result =
        dxmt9::render::classifyDeferredTerminalSuffix(*held, observation);
    checkEq(result.decision,
            DeferredTerminalSuffixDecision::NaturalDrain,
            "each semantic, loss, lease, and pressure wake drains");
    checkEq(result.reason, drain.reason,
            "drain reason retains the exact wake identity");
  }

  observation.wakeFlags =
      dxmt9::render::DeferredTerminalSuffixWakeExactSuccessorReady |
      dxmt9::render::DeferredTerminalSuffixWakeAdmissionPressure |
      dxmt9::render::DeferredTerminalSuffixWakeWriterPressure;
  const auto readyOverPressure =
      dxmt9::render::classifyDeferredTerminalSuffix(*held, observation);
  checkEq(readyOverPressure.decision,
          DeferredTerminalSuffixDecision::ReserveExactSuccessor,
          "exact Ready successor wins simultaneous pressure");

  observation.wakeFlags |=
      dxmt9::render::DeferredTerminalSuffixWakeOrderedRelease;
  const auto releaseOverReady =
      dxmt9::render::classifyDeferredTerminalSuffix(*held, observation);
  checkEq(releaseOverReady.decision,
          DeferredTerminalSuffixDecision::NaturalDrain,
          "ordered release still drains an exact Ready successor");
  checkEq(releaseOverReady.reason,
          DeferredTerminalSuffixDecisionReason::OrderedRelease,
          "ordered release retains priority over the join optimization");
}

void deferredTerminalSuffixRevalidatesTentativeChargeBeforeJoin() {
  ChunkSlot slot{};
  appendDraw(slot, 0x10);
  const auto semantic = summarize(slot);
  const auto current = suffixIdentity(2u, 4u, 7u, 10u);
  const auto successor = suffixIdentity(3u, 6u, 9u, 11u);
  const auto currentAdmission = candidate(10u, 10u, semantic);
  const auto successorAdmission = candidate(11u, 11u, semantic);
  const auto currentCharge =
      dxmt9::render::sessionCapacityFor(currentAdmission);
  const auto successorClaim =
      dxmt9::render::sessionCapacityFor(successorAdmission);
  const auto policy = suffixCapacityPolicy(currentCharge, successorClaim);
  SessionCapacityLeaseState leaseState;
  check(leaseState.acquire(policy, {}, currentCharge),
        "tentative fixture acquires through real lease state");
  const auto proof = suffixProof(current, successor);
  const auto held = makeHeldSuffix(
      current, successor, currentAdmission, leaseState.lease(),
      policy.successorHeadroom);
  check(held.has_value(), "tentative fixture qualifies");
  DeferredTerminalSuffixObservation observation{
      .currentIdentity = current,
      .currentAdmission = currentAdmission,
      .successor = successor,
      .successorAdmission = successorAdmission,
      .lease = leaseState.lease(),
      .wakeFlags =
          dxmt9::render::DeferredTerminalSuffixWakeExactSuccessorReady,
  };

  const auto reserve = dxmt9::render::classifyDeferredTerminalSuffix(
      *held, observation);
  checkEq(reserve.decision,
          DeferredTerminalSuffixDecision::ReserveExactSuccessor,
          "exact Ready identity authorizes reservation before proof exists");
  check(!dxmt9::render::markDeferredTerminalSuffixSuccessorTentative(
             *held, observation),
        "proof absence never authorizes Tentative or effects");

  observation.proof = proof;
  observation.proofValidated = true;
  check(!dxmt9::render::markDeferredTerminalSuffixSuccessorTentative(
             *held, observation),
        "proof attachment still requires the real successor lease charge");
  check(leaseState.charge(leaseState.lease().generation, successorClaim),
        "real lease usable bound accepts the exact successor once");
  observation.lease = leaseState.lease();
  observation.proofValidated = false;
  check(!dxmt9::render::markDeferredTerminalSuffixSuccessorTentative(
             *held, observation),
        "proof absence after charge still cannot authorize effects");
  const auto chargedWithoutProof =
      dxmt9::render::classifyDeferredTerminalSuffix(*held, observation);
  checkEq(chargedWithoutProof.decision,
          DeferredTerminalSuffixDecision::NaturalDrain,
          "unattached charged successor follows the pre-effect drain path");
  observation.proofValidated = true;
  const auto tentative =
      dxmt9::render::markDeferredTerminalSuffixSuccessorTentative(
          *held, observation);
  check(tentative.has_value(),
        "Ready proof attaches only after the exact real charge");
  checkEq(tentative->phase,
          DeferredTerminalSuffixPhase::SuccessorTentative,
          "reservation advances the bounded phase once");
  check(tentative->proofAttached &&
            tentative->proof.currentPrefix == proof.currentPrefix &&
            tentative->proof.currentSuffix == proof.currentSuffix &&
            tentative->proof.successorHead == proof.successorHead,
        "complete proof is retained only by the tentative state");

  const auto join = dxmt9::render::classifyDeferredTerminalSuffix(
      *tentative, observation);
  checkEq(join.decision,
          DeferredTerminalSuffixDecision::JoinExactSuccessor,
          "one exact successor charge permits the pre-effect join");

  check(!leaseState.charge(leaseState.lease().generation, successorClaim),
        "real lease refuses a second successor charge at the exact bound");
  const auto twice = dxmt9::render::addSessionCapacity(
      observation.lease.used, successorClaim);
  check(twice.has_value(), "double-charge stale observation adds");
  observation.lease.used = *twice;
  const auto duplicateCharge =
      dxmt9::render::classifyDeferredTerminalSuffix(
          *tentative, observation);
  checkEq(duplicateCharge.decision,
          DeferredTerminalSuffixDecision::NaturalDrain,
          "a duplicated successor charge rejects before join effect");
  checkEq(duplicateCharge.reason,
          DeferredTerminalSuffixDecisionReason::StaleIdentity,
          "charge mismatch follows the pre-effect stale drain cut");

  observation.lease = leaseState.lease();
  observation.successor.source.id.generation++;
  const auto staleSuccessor =
      dxmt9::render::classifyDeferredTerminalSuffix(
          *tentative, observation);
  checkEq(staleSuccessor.decision,
          DeferredTerminalSuffixDecision::NaturalDrain,
          "a stale successor locator rejects before join effect");

  auto staleBeforeAttachment = observation;
  staleBeforeAttachment.successor = successor;
  staleBeforeAttachment.successor.seqId++;
  staleBeforeAttachment.lease.used = currentCharge;
  staleBeforeAttachment.proofValidated = false;
  const auto staleHeld = dxmt9::render::classifyDeferredTerminalSuffix(
      *held, staleBeforeAttachment);
  checkEq(staleHeld.decision,
          DeferredTerminalSuffixDecision::NaturalDrain,
          "a stale Ready successor naturally drains before proof attachment");

  observation.successor = successor;
  observation.release = {
      .event = {
          .ordinal = 1u,
          .reason = dxmt9::core::metalqueue::SessionReleaseReason::Present,
          .action =
              dxmt9::core::metalqueue::SessionReleaseAction::SubmitSession,
      },
      .generation = 1u,
  };
  const auto staleRelease =
      dxmt9::render::classifyDeferredTerminalSuffix(
          *tentative, observation);
  checkEq(staleRelease.decision,
          DeferredTerminalSuffixDecision::NaturalDrain,
          "release-generation drift rejects before join effect");
}

}  // namespace

int main() {
  try {
    sourceSummaryIsFlatAndClassifiesBoundaries();
    admissionSeparatesOrderingCapsAndIsolation();
    capacityRejectionObservationDistinguishesAxesAndWrapPadding();
    leaseHeadroomAndCapGroupingAreDeterministic();
    productionPageCapacityLeavesExactSuccessorHeadroom();
    firstLeaseOrdinaryShapeExcludesWrapPadding();
    continuationIsConservativeAndRouteAsymmetric();
    multiSourceWindowPreflightIsStrictAndTransactional();
    retirementSplitsResidencyCreditFromEncodedWorkCap();
    deferredTerminalSuffixQualificationIsExactAndBounded();
    deferredTerminalSuffixWakePolicyDrainsWithoutLockedWait();
    deferredTerminalSuffixRevalidatesTentativeChargeBeforeJoin();
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
