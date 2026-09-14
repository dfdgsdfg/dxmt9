#pragma once

// Pure, bounded admission for the CPU Render Tape policy.  This is a cold
// contract used by the native proof and by callers which already sampled the
// real runtime knobs.  It is deliberately not an override or a second policy
// store: changing a policy still goes through the existing environment and
// queue setup paths.

#include <cstddef>
#include <cstdint>

namespace dxmt9::test {

struct CpuTapePolicy {
  bool compatibility = false;
  std::size_t drawCommandLimit = 0u;
  std::size_t slotHeadroomBytes = 0u;
  std::size_t aggregateHeadroomBytes = 0u;
};

inline constexpr std::size_t kCpuTapeMinDrawCommandLimit = 3u;
inline constexpr std::size_t kCpuTapeMaxDrawCommandLimit = 64u;
inline constexpr std::size_t kCpuTapeMinSlotHeadroomBytes = 1u * 1024u * 1024u;
inline constexpr std::size_t kCpuTapeMaxSlotHeadroomBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kCpuTapeMaxAggregateHeadroomBytes =
    512u * 1024u * 1024u;
inline constexpr std::size_t kCpuTapeMaxRecordsPerRaw = 64u;
inline constexpr std::size_t kCpuTapeMaxPendingPerRaw = 31u;
// The isolated general lane keeps the existing payload-cap knob unchanged;
// zero here means no policy-side payload cap is introduced.
inline constexpr std::size_t kCpuTapeGeneralDrawPayloadLimitBytes = 0u;

enum class CpuTapePolicyAdmission : std::uint8_t {
  Accepted,
  DrawLimitRequiresCompatibility,
  DrawLimitOutOfRange,
  HeadroomRequiresProductionSlot,
  HeadroomMustBePaired,
  SlotHeadroomOutOfRange,
  AggregateHeadroomOutOfRange,
  AggregateHeadroomBelowSlot,
  DrawLimitConflictsWithHeadroom,
};

// This ordering intentionally checks the route before the numeric ranges:
// positive draw limits are compatibility-only, while positive headroom is
// production-slot-only.  The paired-zero form is valid for either route.
inline constexpr CpuTapePolicyAdmission admitCpuTapePolicy(
    const CpuTapePolicy& policy) noexcept {
  const bool hasSlotHeadroom = policy.slotHeadroomBytes != 0u;
  const bool hasAggregateHeadroom = policy.aggregateHeadroomBytes != 0u;
  const bool hasHeadroom = hasSlotHeadroom || hasAggregateHeadroom;
  const bool hasDrawLimit = policy.drawCommandLimit != 0u;

  if (hasDrawLimit && hasHeadroom)
    return CpuTapePolicyAdmission::DrawLimitConflictsWithHeadroom;
  if (hasDrawLimit && !policy.compatibility)
    return CpuTapePolicyAdmission::DrawLimitRequiresCompatibility;
  if (hasDrawLimit &&
      (policy.drawCommandLimit < kCpuTapeMinDrawCommandLimit ||
       policy.drawCommandLimit > kCpuTapeMaxDrawCommandLimit))
    return CpuTapePolicyAdmission::DrawLimitOutOfRange;
  if (hasSlotHeadroom != hasAggregateHeadroom)
    return CpuTapePolicyAdmission::HeadroomMustBePaired;
  if (hasHeadroom && policy.compatibility)
    return CpuTapePolicyAdmission::HeadroomRequiresProductionSlot;
  if (hasSlotHeadroom &&
      (policy.slotHeadroomBytes < kCpuTapeMinSlotHeadroomBytes ||
       policy.slotHeadroomBytes > kCpuTapeMaxSlotHeadroomBytes))
    return CpuTapePolicyAdmission::SlotHeadroomOutOfRange;
  if (hasAggregateHeadroom &&
      (policy.aggregateHeadroomBytes < policy.slotHeadroomBytes ||
       policy.aggregateHeadroomBytes > kCpuTapeMaxAggregateHeadroomBytes))
    return policy.aggregateHeadroomBytes < policy.slotHeadroomBytes
        ? CpuTapePolicyAdmission::AggregateHeadroomBelowSlot
        : CpuTapePolicyAdmission::AggregateHeadroomOutOfRange;
  return CpuTapePolicyAdmission::Accepted;
}

inline constexpr bool cpuTapePolicyValid(
    const CpuTapePolicy& policy) noexcept {
  return admitCpuTapePolicy(policy) == CpuTapePolicyAdmission::Accepted;
}

// The split sequence emits at most ceil(records / limit) bounded pieces plus
// one retained/pending source.  Division and remainder avoid records+limit-1
// overflow.  A zero limit means "not bounded by this helper"; 1 and 2 are
// intentionally rejected because the general CPU lane has no proof for those
// tiny windows.  Valid positive limits therefore establish the conservative
// ceil(64 / L) + 1 <= 31 bound.
inline constexpr std::size_t cpuTapeConservativeMaxPendingPerRaw(
    std::size_t records, std::size_t limit) noexcept {
  if (records > kCpuTapeMaxRecordsPerRaw || limit < kCpuTapeMinDrawCommandLimit ||
      limit > kCpuTapeMaxDrawCommandLimit)
    return 0u;
  return records / limit + (records % limit != 0u ? 1u : 0u) + 1u;
}

inline constexpr bool cpuTapeGeneralQueueBounded(
    std::size_t records, std::size_t limit,
    std::size_t queueCapacity) noexcept {
  const auto bound = cpuTapeConservativeMaxPendingPerRaw(records, limit);
  return bound != 0u && bound <= kCpuTapeMaxPendingPerRaw &&
         queueCapacity >= bound;
}

// Capture-source replay may be measured and may carry an explicit route
// policy.  Those modes share the same conservative source bound; they are not
// a reason to discard an otherwise valid policy at the CLI boundary.
inline constexpr bool cpuTapeCaptureSourcePolicyBounded(
    const CpuTapePolicy& policy) noexcept {
  return cpuTapePolicyValid(policy) &&
         cpuTapeGeneralQueueBounded(kCpuTapeMaxRecordsPerRaw,
                                    kCpuTapeMinDrawCommandLimit,
                                    kCpuTapeMaxPendingPerRaw);
}

static_assert(cpuTapeCaptureSourcePolicyBounded(CpuTapePolicy{}));
static_assert(cpuTapeCaptureSourcePolicyBounded(
    CpuTapePolicy{.compatibility = true, .drawCommandLimit = 3u}));
static_assert(cpuTapeCaptureSourcePolicyBounded(CpuTapePolicy{
    .compatibility = false,
    .slotHeadroomBytes = kCpuTapeMinSlotHeadroomBytes,
    .aggregateHeadroomBytes = kCpuTapeMinSlotHeadroomBytes}));

}  // namespace dxmt9::test
