#pragma once

#include "../dxmt9_source_semantics.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>

namespace dxmt9::render {

enum class EncodeQueueLane : std::uint8_t {
  Invalid,
  CpuReadySerial,
};

enum class EncodeCaptureMode : std::uint8_t {
  Disabled,
  Isolated,
};

enum class EncodeCompletionMode : std::uint8_t {
  Metal,
  TestNullCommandBuffer,
};

struct SessionAdmissionKey {
  // The current runtime does not expose resettable device/allocator epochs.
  // Phase one binds both values to the lifetime/policy of one CommandQueue;
  // shutdown and device loss release that queue before either value can vary.
  std::uint64_t queueLifetimeEpoch = 1;
  std::uint64_t allocatorPolicyEpoch = 1;
  EncodeQueueLane lane = EncodeQueueLane::CpuReadySerial;
  EncodeCaptureMode captureMode = EncodeCaptureMode::Disabled;
  EncodeCompletionMode completionMode = EncodeCompletionMode::Metal;

  constexpr bool valid() const noexcept {
    return queueLifetimeEpoch != 0 && allocatorPolicyEpoch != 0 &&
           lane != EncodeQueueLane::Invalid;
  }

  friend constexpr bool operator==(const SessionAdmissionKey&,
                                   const SessionAdmissionKey&) = default;
};

struct EncodeSessionLimits {
  std::uint32_t maxSources = 32;
  std::uint32_t maxPages = std::numeric_limits<std::uint32_t>::max();
  std::uint64_t maxBytes = std::numeric_limits<std::uint64_t>::max();
  std::uint32_t maxDraws = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t maxCommandBuffers =
      std::numeric_limits<std::uint32_t>::max();

  constexpr bool valid() const noexcept {
    return maxSources != 0 && maxPages != 0 && maxBytes != 0 &&
           maxDraws != 0 && maxCommandBuffers != 0;
  }
};

// Scheduler-owned Tape bytes charged to the bounded session lease. This is
// deliberately distinct from SourceSemanticSummary::byteCount, whose
// publication extent has representation-specific meaning.
struct SessionResidencyByteCharge {
  std::uint64_t value = 0;

  constexpr bool valid() const noexcept { return value != 0; }

  friend constexpr bool operator==(const SessionResidencyByteCharge&,
                                   const SessionResidencyByteCharge&) = default;
};

constexpr SessionResidencyByteCharge sessionTapeByteCharge(
    bool arenaPayload,
    std::uint64_t usedBytes,
    std::uint32_t pageCount,
    std::uint32_t pageSize) noexcept {
  return {
      .value = arenaPayload
          ? usedBytes
          : static_cast<std::uint64_t>(pageCount) * pageSize,
  };
}

enum class SessionCapacityDimension : std::uint8_t {
  None,
  Sources,
  Pages,
  Bytes,
  Draws,
  CommandBuffers,
};

struct SessionCapacityVector {
  std::uint64_t sources = 0;
  std::uint64_t pages = 0;
  std::uint64_t bytes = 0;
  std::uint64_t draws = 0;
  std::uint64_t payloadBlocks = 0;
  std::uint64_t readyEntries = 0;
  std::uint64_t retentionEntries = 0;
  std::uint64_t allocatorTickets = 0;
  std::uint64_t commandBuffers = 0;

  friend constexpr bool operator==(const SessionCapacityVector&,
                                   const SessionCapacityVector&) = default;
};

bool sessionCapacityFitsWithin(
    const SessionCapacityVector& value,
    const SessionCapacityVector& limit) noexcept;
std::optional<SessionCapacityVector> addSessionCapacity(
    const SessionCapacityVector& left,
    const SessionCapacityVector& right) noexcept;
std::optional<SessionCapacityVector> subtractSessionCapacity(
    const SessionCapacityVector& value,
    const SessionCapacityVector& deduction) noexcept;

// Fixed queue-creation policy for R-BACK-2.65. Page headroom includes the
// payload run plus the maximum circular tail discarded before a non-wrapping
// run. The other fields name physical source/Ready/block ownership through the
// source credit and logical session limits through the remaining dimensions.
struct SessionCapacityPolicy {
  SessionCapacityVector highWater{};
  SessionCapacityVector maxSession{};
  SessionCapacityVector successorHeadroom{};
  SessionCapacityVector ordinaryDirect{};

  bool valid() const noexcept;
};

struct SessionCapacityLease {
  std::uint64_t generation = 0;
  SessionCapacityVector reserved{};
  SessionCapacityVector used{};
  SessionCapacityVector successorRemaining{};

  constexpr bool valid() const noexcept { return generation != 0; }
};

std::optional<SessionCapacityVector> sessionCapacityLeaseUsableBound(
    const SessionCapacityLease& lease) noexcept;

struct SessionCapacityLeaseStats {
  std::uint64_t acquisitions = 0;
  std::uint64_t denials = 0;
  std::uint64_t releases = 0;
  std::uint64_t peakCurrent = 0;
  SessionCapacityVector reserved{};
  SessionCapacityVector used{};
  SessionCapacityVector slack{};
  SessionCapacityVector successorMinimum{};
};

// Typed observation used only before the first lease acquisition. A unique
// ordered-tail Writing source may already occupy the physical capacity that
// successorHeadroom reserves. Qualification credits that exact claim once;
// the acquired lease still owns the complete fixed successor vector.
struct SessionCapacityLeaseAcquisitionSnapshot {
  SessionCapacityVector olderUnavailable{};
  std::optional<SessionCapacityVector> orderedTailWritingSuccessor{};
  bool valid = true;
};

std::optional<SessionCapacityVector>
sessionCapacityUnavailableForFirstLease(
    const SessionCapacityLeaseAcquisitionSnapshot& snapshot,
    const SessionCapacityVector& successorHeadroom) noexcept;

constexpr std::uint64_t worstCaseNonWrappingReservationPages(
    std::uint64_t payloadPages) noexcept {
  return payloadPages == 0 ||
                 payloadPages >
                     std::numeric_limits<std::uint64_t>::max() / 2u + 1u
             ? 0
             : payloadPages * 2u - 1u;
}

class SessionCapacityLeaseState {
 public:
  bool acquire(const SessionCapacityPolicy& policy,
               const SessionCapacityVector& unavailable,
               const SessionCapacityVector& initialCharge) noexcept;
  bool charge(std::uint64_t generation,
              const SessionCapacityVector& candidate) noexcept;
  bool uncharge(std::uint64_t generation,
                const SessionCapacityVector& candidate) noexcept;
  bool release(std::uint64_t generation) noexcept;

  const SessionCapacityLease& lease() const noexcept { return lease_; }
  const SessionCapacityLeaseStats& stats() const noexcept { return stats_; }

 private:
  std::uint64_t nextGeneration_ = 1;
  SessionCapacityLease lease_{};
  SessionCapacityLeaseStats stats_{};
};

enum EncodeSessionAdmissionFlags : std::uint32_t {
  EncodeSessionAdmissionValid = 1u << 0,
  EncodeSessionAdmissionHasPresent = 1u << 1,
};

struct EncodeSessionAdmissionState {
  SessionAdmissionKey key{};
  std::uint64_t lastRawOrdinal = 0;
  std::uint64_t lastSourceOrdinal = 0;
  std::uint64_t lastSeqId = 0;
  SessionResidencyByteCharge residencyBytes{};
  // `sources` is encoded-unsubmitted work. `residentSources`, `pages`, and
  // residencyBytes are physical publication/Tape credit and may fall while
  // the same session remains open after post-encode retirement.
  std::uint32_t sources = 0;
  std::uint32_t residentSources = 0;
  std::uint32_t pages = 0;
  std::uint32_t draws = 0;
  std::uint32_t commandBuffers = 0;
  std::uint32_t flags = 0;

  constexpr bool valid() const noexcept {
    return (flags & EncodeSessionAdmissionValid) != 0;
  }

  constexpr bool hasPresent() const noexcept {
    return (flags & EncodeSessionAdmissionHasPresent) != 0;
  }

  friend constexpr bool operator==(const EncodeSessionAdmissionState&,
                                   const EncodeSessionAdmissionState&) =
      default;
};

struct SessionAdmissionCandidate {
  SessionAdmissionKey key{};
  core::SourceSemanticSummary semantic{};
  SessionResidencyByteCharge residencyBytes{};
  std::uint64_t rawOrdinal = 0;
  std::uint64_t sourceOrdinal = 0;
  std::uint64_t seqId = 0;
  std::uint32_t predictedCommandBuffers = 1;
  // Zero keeps pure semantic/unit callers compatible. Production supplies
  // payload pages plus the source's actual circular wrap padding.
  std::uint32_t reservationPages = 0;
  std::uint32_t payloadBlocks = 1;
  std::uint32_t retentionEntries = 1;
  std::uint32_t allocatorTickets = 1;

  friend constexpr bool operator==(const SessionAdmissionCandidate&,
                                   const SessionAdmissionCandidate&) =
      default;
};

bool sessionAdmissionCandidateIdentityValid(
    const SessionAdmissionCandidate& candidate) noexcept;

enum class SessionAdmissionDecision : std::uint8_t {
  Admit,
  SubmitPrefixBeforeCandidate,
  ProcessCandidateIsolated,
  RejectInvalid,
};

enum SessionCapacityExceededAxis : std::uint8_t {
  SessionCapacityExceededNone = 0,
  SessionCapacityExceededSources = 1u << 0,
  SessionCapacityExceededPages = 1u << 1,
};

// Exact source/page demand at a capacity rejection. Payload pages exclude
// circular wrap padding; required pages include it. The totals are computed
// from the predecessor session plus the rejected candidate.
struct SessionCapacityRejectionObservation {
  std::uint64_t predecessorSources = 0;
  std::uint64_t predecessorPages = 0;
  std::uint64_t candidatePayloadPages = 0;
  std::uint64_t candidateWrapPaddingPages = 0;
  std::uint64_t candidateRequiredPages = 0;
  std::uint64_t requiredTotalSources = 0;
  std::uint64_t requiredTotalPages = 0;
  std::uint8_t exceededAxes = SessionCapacityExceededNone;

  constexpr bool valid() const noexcept {
    return exceededAxes != SessionCapacityExceededNone;
  }
  constexpr bool sourcesExceeded() const noexcept {
    return (exceededAxes & SessionCapacityExceededSources) != 0;
  }
  constexpr bool pagesExceeded() const noexcept {
    return (exceededAxes & SessionCapacityExceededPages) != 0;
  }
};

struct SessionAdmissionResult {
  SessionAdmissionDecision decision = SessionAdmissionDecision::RejectInvalid;
  SessionCapacityDimension limitingDimension =
      SessionCapacityDimension::None;
  SessionCapacityRejectionObservation capacityRejection{};
};

SessionCapacityVector sessionCapacityFor(
    const SessionAdmissionCandidate& candidate) noexcept;
SessionCapacityVector sessionPhysicalResidencyCapacityFor(
    const SessionAdmissionCandidate& candidate) noexcept;
bool retireSessionAdmissionResidency(
    EncodeSessionAdmissionState& session,
    const SessionAdmissionCandidate& candidate) noexcept;

SessionAdmissionResult classifySessionAdmissionDetailed(
    const EncodeSessionAdmissionState& session,
    const SessionAdmissionCandidate& candidate,
    const EncodeSessionLimits& limits) noexcept;

SessionAdmissionDecision classifySessionAdmission(
    const EncodeSessionAdmissionState& session,
    const SessionAdmissionCandidate& candidate,
    const EncodeSessionLimits& limits) noexcept;

bool appendSessionAdmission(
    EncodeSessionAdmissionState& session,
    const SessionAdmissionCandidate& candidate,
    const EncodeSessionLimits& limits) noexcept;

enum class MultiSourceSessionWindowPreflightReason : std::uint8_t {
  Eligible,
  NoPendingSession,
  SourceCount,
  ActiveRenderIncomplete,
  CaptureBoundary,
  InitializerBoundary,
  OrderedReleaseBoundary,
  InvalidCandidate,
  PresentBoundary,
  IsolationBoundary,
  InitializerSemanticBoundary,
  NonConsecutiveIdentity,
  AdmissionRejected,
};

enum class MultiSourceSessionWindowFrontier : std::uint8_t {
  FreshClean,
  CleanClosed,
  ActiveRenderComplete,
  Unsupported,
};

inline constexpr std::size_t kMaxMultiSourceSessionWindowSources = 8u;

struct MultiSourceSessionWindowPreflight {
  EncodeSessionAdmissionState stagedAdmission{};
  MultiSourceSessionWindowPreflightReason reason =
      MultiSourceSessionWindowPreflightReason::NoPendingSession;

  constexpr bool eligible() const noexcept {
    return reason == MultiSourceSessionWindowPreflightReason::Eligible;
  }
};

// Queue-independent preflight for the bounded CPU-ready Tape multi-source
// planning window. This does not retain sources, mutate the session, or run the
// FrameGraph planner. A successful result carries the admission state that can
// be installed atomically after every fragment has encoded successfully.
// sourceOrdinal and seqId must be exact successors. rawOrdinal is optional
// when zero and otherwise only has to advance monotonically between observed
// values because raw entries need not each publish a CPU-ready source.
MultiSourceSessionWindowPreflight preflightMultiSourceSessionWindow(
    const EncodeSessionAdmissionState& pending,
    std::span<const SessionAdmissionCandidate> candidates,
    const EncodeSessionLimits& limits,
    MultiSourceSessionWindowFrontier frontier,
    bool captureBoundary,
    bool initializerBoundary,
    bool orderedReleaseBoundary) noexcept;

enum ActiveRenderContinuationFlags : std::uint32_t {
  ActiveRenderContinuationActive = 1u << 0,
  ActiveRenderContinuationPendingNonFoldableClear = 1u << 1,
  ActiveRenderContinuationInitializerWait = 1u << 2,
  ActiveRenderContinuationSidecarObservation = 1u << 3,
};

struct ActiveRenderContinuationState {
  core::RenderContinuationKey key{};
  core::ExactResourceSet activeWrites{};
  std::uint32_t flags = 0;

  constexpr bool active() const noexcept {
    return (flags & ActiveRenderContinuationActive) != 0;
  }
};

enum class RenderContinuationDecision : std::uint8_t {
  NoActivePass,
  ContinueProven,
  ClosePass,
  ExactReplayRequired,
};

// This is a side-effect-free preflight. ContinueProven never bypasses the
// existing encodeChunk attachment/hazard/tile checks; exact replay remains the
// authority that decides whether Metal may actually keep the encoder open.
RenderContinuationDecision classifyRenderContinuation(
    const ActiveRenderContinuationState& active,
    const core::SourceSemanticSummary& incoming) noexcept;

static_assert(std::is_trivially_copyable_v<SessionAdmissionKey>);
static_assert(std::is_standard_layout_v<SessionAdmissionKey>);
static_assert(std::is_trivially_copyable_v<EncodeSessionAdmissionState>);
static_assert(std::is_standard_layout_v<EncodeSessionAdmissionState>);
static_assert(std::is_trivially_copyable_v<SessionAdmissionCandidate>);
static_assert(std::is_standard_layout_v<SessionAdmissionCandidate>);
static_assert(std::is_trivially_copyable_v<SessionResidencyByteCharge>);
static_assert(std::is_standard_layout_v<SessionResidencyByteCharge>);
static_assert(std::is_trivially_copyable_v<SessionCapacityVector>);
static_assert(std::is_standard_layout_v<SessionCapacityVector>);
static_assert(
    std::is_trivially_copyable_v<SessionCapacityRejectionObservation>);
static_assert(std::is_standard_layout_v<SessionCapacityRejectionObservation>);
static_assert(std::is_trivially_copyable_v<SessionCapacityLease>);
static_assert(std::is_standard_layout_v<SessionCapacityLease>);
static_assert(std::is_trivially_copyable_v<ActiveRenderContinuationState>);
static_assert(std::is_standard_layout_v<ActiveRenderContinuationState>);

}  // namespace dxmt9::render
