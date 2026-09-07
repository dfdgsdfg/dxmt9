#pragma once

// The append envelope owns cadence, retry, settlement, and observer ordering.
// This dispatcher constructs only the selected family-specific preparation,
// while the production append still has one shared transaction body.

#include "d3d9_pe_semantic_recorder_state.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <type_traits>
#include <utility>

namespace dxmt9::d3d9::pe {

struct PeSemanticConstantAppendPreparation {
  PeSemanticConstantRecordInput* input = nullptr;

  void setDestination(std::uint32_t recordType, std::uint64_t sourceOrdinal,
                      std::uint64_t recordOrdinal) noexcept {
    input->recordType = recordType;
    input->sourceOrdinal = sourceOrdinal;
    input->recordOrdinal = recordOrdinal;
  }
};

struct PeSemanticSparseAppendPreparation {
  SparseStatePlan* plan = nullptr;
  PeSemanticProducerKind producer = PeSemanticProducerKind::DrawPrimitive;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;

  void setDestination(std::uint32_t, std::uint64_t source,
                      std::uint64_t record) noexcept {
    sourceOrdinal = source;
    recordOrdinal = record;
  }
};

struct PeSemanticGenericAppendPreparation {
  PeSemanticRecordInput* staged = nullptr;
  PeSemanticRecordInputView inputView{};

  explicit PeSemanticGenericAppendPreparation(
      PeSemanticRecordInput& input) noexcept
      : staged(&input), inputView(borrowPeSemanticRecordInput(input)) {}

  void setDestination(std::uint32_t recordType, std::uint64_t sourceOrdinal,
                      std::uint64_t recordOrdinal) noexcept {
    inputView.destination.recordType = recordType;
    inputView.destination.sourceOrdinal = sourceOrdinal;
    inputView.destination.recordOrdinal = recordOrdinal;
    inputView.destination.sparse = &staged->sparse;
  }
};

struct PeSemanticAppendLimits {
  std::size_t maxRecords = 0u;
  std::size_t maxBytes = 0u;
};

// The six inline-constant ranges are necessarily empty in the legacy draw
// mode. Keep the branch around the query itself: source qualification still
// runs, but an OFF draw must not inspect the range spans merely to add zero to
// its cadence bytes.
template <typename Query>
  requires std::is_nothrow_invocable_r_v<std::size_t, Query&>
inline std::size_t peSemanticConstantPayloadBytes(bool inlineConstDelta,
                                                 Query&& query) noexcept {
  if (!inlineConstDelta) return 0u;
  return std::forward<Query>(query)();
}

// A live slot owns a strong COM reference. Its wire identity was populated by
// the full validator when the slot was installed and is immutable for the
// lifetime of that retained wrapper. This proof is intentionally narrower
// than membership validation: it can only certify the exact retained object,
// never null, a replacement, a foreign object, or a recording write. A
// malformed wire row is rejected by shape checks; `wire.valid` only rejects
// zero/invalid-shape fields. Nonzero identity consistency relies on the
// retained slot invariant. Only strongly retained texture/shader/index-buffer
// callers may use this helper; borrowed declarations keep full validation.
struct PeSemanticRetainedBinding {
  const void* ownerDevice = nullptr;
  const void* publicIdentity = nullptr;
  PeWireObjectRef wire{};
};

static_assert(std::is_trivially_copyable_v<PeSemanticRetainedBinding>);

inline bool peSemanticCanReuseRetainedBinding(
    bool writeAllowed, bool recording, const void* candidate,
    const PeSemanticRetainedBinding& retained, const void* expectedOwner,
    std::uint32_t expectedWireKind) noexcept {
  return writeAllowed && !recording && candidate != nullptr &&
      retained.publicIdentity == candidate &&
      retained.ownerDevice == expectedOwner && retained.wire.valid(expectedWireKind);
}

template <typename Visitor>
decltype(auto) withPeSemanticAppendPreparation(
    PeSemanticStagingFamily family, PeSemanticRecordInput& generic,
    PeSemanticConstantRecordInput& constant, SparseStatePlan& sparse,
    PeSemanticProducerKind producer, Visitor&& visitor) {
  if (family == PeSemanticStagingFamily::Constant) {
    PeSemanticConstantAppendPreparation preparation{.input = &constant};
    return std::forward<Visitor>(visitor)(preparation);
  }
  if (family == PeSemanticStagingFamily::SparsePlan) {
    PeSemanticSparseAppendPreparation preparation{.plan = &sparse,
                                                  .producer = producer};
    return std::forward<Visitor>(visitor)(preparation);
  }
  PeSemanticGenericAppendPreparation preparation{generic};
  return std::forward<Visitor>(visitor)(preparation);
}

template <typename Limits>
inline bool peSemanticFaultWouldFlush(bool enabled, std::size_t records,
                                      std::size_t payload, std::size_t bytes,
                                      Limits&& limits) noexcept {
  if (!enabled) return false;
  const auto resolved = std::forward<Limits>(limits)();
  const bool payloadWouldExceed =
      payload > resolved.maxBytes ||
      bytes > resolved.maxBytes - std::min(payload, resolved.maxBytes);
  return records != 0u &&
      (records >= resolved.maxRecords || payloadWouldExceed);
}

template <typename Preparation>
inline constexpr bool isPeSemanticConstantAppendPreparation =
    std::same_as<std::remove_cvref_t<Preparation>,
                 PeSemanticConstantAppendPreparation>;

template <typename Preparation>
inline constexpr bool isPeSemanticSparseAppendPreparation =
    std::same_as<std::remove_cvref_t<Preparation>,
                 PeSemanticSparseAppendPreparation>;

template <typename Preparation>
inline constexpr bool isPeSemanticGenericAppendPreparation =
    std::same_as<std::remove_cvref_t<Preparation>,
                 PeSemanticGenericAppendPreparation>;

}  // namespace dxmt9::d3d9::pe
