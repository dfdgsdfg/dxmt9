#pragma once

#include "../dxmt9_source_semantics.hpp"

#include <cstdint>
#include <limits>
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

enum EncodeSessionAdmissionFlags : std::uint32_t {
  EncodeSessionAdmissionValid = 1u << 0,
  EncodeSessionAdmissionHasPresent = 1u << 1,
};

struct EncodeSessionAdmissionState {
  SessionAdmissionKey key{};
  std::uint64_t lastRawOrdinal = 0;
  std::uint64_t lastSourceOrdinal = 0;
  std::uint64_t lastSeqId = 0;
  std::uint64_t bytes = 0;
  std::uint32_t sources = 0;
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
  std::uint64_t rawOrdinal = 0;
  std::uint64_t sourceOrdinal = 0;
  std::uint64_t seqId = 0;
  std::uint32_t predictedCommandBuffers = 1;
};

enum class SessionAdmissionDecision : std::uint8_t {
  Admit,
  SubmitPrefixBeforeCandidate,
  ProcessCandidateIsolated,
  RejectInvalid,
};

SessionAdmissionDecision classifySessionAdmission(
    const EncodeSessionAdmissionState& session,
    const SessionAdmissionCandidate& candidate,
    const EncodeSessionLimits& limits) noexcept;

bool appendSessionAdmission(
    EncodeSessionAdmissionState& session,
    const SessionAdmissionCandidate& candidate,
    const EncodeSessionLimits& limits) noexcept;

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
static_assert(std::is_trivially_copyable_v<ActiveRenderContinuationState>);
static_assert(std::is_standard_layout_v<ActiveRenderContinuationState>);

}  // namespace dxmt9::render
