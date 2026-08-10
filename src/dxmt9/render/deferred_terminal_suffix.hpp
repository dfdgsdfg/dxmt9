#pragma once

#include "../dxmt9_encode_chunk_types.hpp"
#include "../dxmt9_queue.hpp"
#include "../dxmt9_cpu_ready_tape.hpp"
#include "../dxmt9_session_release.hpp"
#include "../framegraph/fg_multi_source_planner.hpp"
#include "encode_session_admission.hpp"

#include <cstdint>
#include <optional>
#include <type_traits>

namespace dxmt9::render {

enum class DeferredTerminalSuffixPhase : std::uint8_t {
  Empty,
  PrefixEncoded,
  Held,
  SuccessorTentative,
  JoinEffectful,
};

struct DeferredTerminalSuffixSourceIdentity {
  core::CpuReadyTape::SourceRef source{};
  std::uint64_t rawOrdinal = 0;
  std::uint64_t sourceOrdinal = 0;
  std::uint64_t seqId = 0;

  constexpr bool valid() const noexcept {
    return source.valid() && sourceOrdinal != 0 && seqId != 0;
  }

  friend constexpr bool operator==(
      const DeferredTerminalSuffixSourceIdentity&,
      const DeferredTerminalSuffixSourceIdentity&) = default;
};

// Coordinator-owned policy state for one represented current source and one
// exact ordered-tail Writing successor. It contains only generation-stamped
// locators and immutable value snapshots; payload views and scheduling-lock
// ownership must remain call-local.
struct DeferredTerminalSuffixState {
  DeferredTerminalSuffixPhase phase =
      DeferredTerminalSuffixPhase::Empty;
  core::metalqueue::ReadySlotSnapshot current{};
  core::metalqueue::QueueCompletionSource currentCompletion{};
  DeferredTerminalSuffixSourceIdentity currentIdentity{};
  DeferredTerminalSuffixSourceIdentity expectedWritingSuccessor{};
  SessionAdmissionCandidate admission{};
  SessionCapacityVector currentCharge{};
  framegraph::SourceCommandRange currentPrefix{};
  framegraph::SourceCommandRange currentSuffix{};
  core::CpuReadyTape::LeaseCapacityClaim successorWritingClaim{};
  core::CpuReadyTape::LeaseCapacityClaim successorPhysicalHeadroom{};
  SessionCapacityVector successorClaim{};
  SessionCapacityVector leaseReserved{};
  SessionCapacityVector leaseUsedBeforeSuccessor{};
  SessionCapacityVector leaseSuccessorRemaining{};
  core::metalqueue::SessionReleaseSnapshot release{};
  std::uint64_t leaseGeneration = 0;
  SessionAdmissionCandidate successorAdmission{};
  framegraph::DeferredTerminalSuffixProof proof{};
  bool proofAttached = false;
  encoders::PreRegisteredEncodeSourceFragmentAccumulator fragments{};
};

enum DeferredTerminalSuffixWakeFlag : std::uint32_t {
  DeferredTerminalSuffixWakeNone = 0,
  DeferredTerminalSuffixWakeExactSuccessorReady = 1u << 0,
  DeferredTerminalSuffixWakeOrderedRelease = 1u << 1,
  DeferredTerminalSuffixWakeProducerWait = 1u << 2,
  DeferredTerminalSuffixWakeInitializer = 1u << 3,
  DeferredTerminalSuffixWakeQuery = 1u << 4,
  DeferredTerminalSuffixWakeReadback = 1u << 5,
  DeferredTerminalSuffixWakeUpdateTexture = 1u << 6,
  DeferredTerminalSuffixWakePresent = 1u << 7,
  DeferredTerminalSuffixWakeStop = 1u << 8,
  DeferredTerminalSuffixWakeDeviceLoss = 1u << 9,
  DeferredTerminalSuffixWakeWriterLost = 1u << 10,
  DeferredTerminalSuffixWakeLeaseInvalidated = 1u << 11,
  DeferredTerminalSuffixWakeHeadroomInvalidated = 1u << 12,
  DeferredTerminalSuffixWakeAdmissionPressure = 1u << 13,
  DeferredTerminalSuffixWakeWriterPressure = 1u << 14,
};

struct DeferredTerminalSuffixObservation {
  DeferredTerminalSuffixSourceIdentity currentIdentity{};
  SessionAdmissionCandidate currentAdmission{};
  DeferredTerminalSuffixSourceIdentity successor{};
  SessionAdmissionCandidate successorAdmission{};
  SessionCapacityLease lease{};
  core::metalqueue::SessionReleaseSnapshot release{};
  framegraph::DeferredTerminalSuffixProof proof{};
  std::uint32_t wakeFlags = DeferredTerminalSuffixWakeNone;
  bool proofValidated = false;
  bool schedulingMutexOwned = false;
};

enum class DeferredTerminalSuffixDecision : std::uint8_t {
  Invalid,
  WaitUnlocked,
  ReserveExactSuccessor,
  JoinExactSuccessor,
  NaturalDrain,
};

enum class DeferredTerminalSuffixDecisionReason : std::uint8_t {
  None,
  InvalidState,
  SchedulingMutexOwned,
  StaleIdentity,
  OrderedRelease,
  ProducerWait,
  Initializer,
  Query,
  Readback,
  UpdateTexture,
  Present,
  Stop,
  DeviceLoss,
  WriterLost,
  LeaseInvalidated,
  HeadroomInvalidated,
  AdmissionPressure,
  WriterPressure,
  ExactSuccessorReady,
};

struct DeferredTerminalSuffixDecisionResult {
  DeferredTerminalSuffixDecision decision =
      DeferredTerminalSuffixDecision::Invalid;
  DeferredTerminalSuffixDecisionReason reason =
      DeferredTerminalSuffixDecisionReason::InvalidState;

  constexpr bool mayWait() const noexcept {
    return decision == DeferredTerminalSuffixDecision::WaitUnlocked;
  }
};

std::optional<DeferredTerminalSuffixState>
makeDeferredTerminalSuffixPrefixEncoded(
    const DeferredTerminalSuffixSourceIdentity& current,
    const core::metalqueue::ReadySlotSnapshot& currentSnapshot,
    const core::metalqueue::QueueCompletionSource& currentCompletion,
    const SessionAdmissionCandidate& admission,
    const SessionCapacityVector& currentCharge,
    const framegraph::SourceCommandRange& currentPrefix,
    const framegraph::SourceCommandRange& currentSuffix,
    const core::CpuReadyTape::SourceRef& writingSuccessor,
    const core::CpuReadyTape::LeaseCapacityClaim& writingClaim,
    const SessionCapacityLease& lease,
    const SessionCapacityVector& successorHeadroom,
    const core::metalqueue::SessionReleaseSnapshot& release) noexcept;

// PrefixEncoded and Held deliberately have no successor admission, logical
// charge, or complete planner proof: Writing exposes only SourceRef and its
// physical claim. Once that exact source is Ready, the coordinator validates
// the complete proof call-locally, charges the Ready-derived candidate through
// SessionCapacityLeaseState, and supplies the resulting lease observation to
// markDeferredTerminalSuffixSuccessorTentative.
std::optional<DeferredTerminalSuffixState> holdDeferredTerminalSuffix(
    const DeferredTerminalSuffixState& prefixEncoded) noexcept;

std::optional<DeferredTerminalSuffixState>
markDeferredTerminalSuffixSuccessorTentative(
    const DeferredTerminalSuffixState& held,
    const DeferredTerminalSuffixObservation& observation) noexcept;

DeferredTerminalSuffixDecisionResult classifyDeferredTerminalSuffix(
    const DeferredTerminalSuffixState& state,
    const DeferredTerminalSuffixObservation& observation) noexcept;

static_assert(
    std::is_trivially_copyable_v<DeferredTerminalSuffixSourceIdentity>);
static_assert(
    std::is_standard_layout_v<DeferredTerminalSuffixSourceIdentity>);
static_assert(std::is_trivially_copyable_v<DeferredTerminalSuffixState>);
static_assert(std::is_standard_layout_v<DeferredTerminalSuffixState>);
static_assert(
    std::is_trivially_copyable_v<DeferredTerminalSuffixObservation>);
static_assert(std::is_standard_layout_v<DeferredTerminalSuffixObservation>);

}  // namespace dxmt9::render
