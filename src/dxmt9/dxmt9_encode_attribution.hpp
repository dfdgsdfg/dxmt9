#pragma once

// Observation-only value contracts shared by EncodeSession, the FrameGraph
// planner, and encodeChunk. They carry no Metal ownership and must never enter
// replay, pass-lifetime, completion, or scheduling decisions.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace dxmt9::encoders {

struct RenderPassInstanceToken {
  std::uint64_t seqId = 0;
  std::uint64_t encoderIndex = 0;

  constexpr bool valid() const noexcept { return seqId != 0u; }

  friend constexpr bool operator==(const RenderPassInstanceToken&,
                                   const RenderPassInstanceToken&) = default;
};

// Flattened command identity captured at the exact passcoalesce mutation.
// The multi-source planner maps it to a retained source/local command pair.
struct ActiveSeedMergeCommandWitness {
  std::uint32_t flattenedCommandIndex = 0;
  std::uint32_t mergeOrdinal = 0;
  std::uint32_t mergeDistance = 0;

  constexpr bool valid() const noexcept { return mergeDistance != 0u; }
};

struct ActiveSeedMergeTargetWitness {
  std::uint32_t retainedSourceIndex = 0;
  std::uint32_t commandIndex = 0;
  std::uint32_t mergeOrdinal = 0;
  std::uint32_t mergeDistance = 0;

  constexpr bool valid() const noexcept { return mergeDistance != 0u; }

  friend constexpr bool operator==(const ActiveSeedMergeTargetWitness&,
                                   const ActiveSeedMergeTargetWitness&) =
      default;
};

// The queue adds the revalidated physical seed token and current window
// identity without copying the planner-owned target-witness vector.
struct ActiveSeedMergeTicketContext {
  RenderPassInstanceToken seed{};
  std::uint64_t windowId = 0;
  std::uint32_t sourceCount = 0;

  constexpr bool valid() const noexcept {
    return seed.valid() && windowId != 0u && sourceCount >= 2u;
  }
};

enum class ActiveSeedInstanceRevalidation : std::uint8_t {
  Available,
  Unavailable,
  Stale,
};

inline constexpr ActiveSeedInstanceRevalidation
classifyActiveSeedInstanceRevalidation(
    std::optional<RenderPassInstanceToken> planned,
    std::optional<RenderPassInstanceToken> live) noexcept {
  if (!planned || !live || !planned->valid() || !live->valid()) {
    return ActiveSeedInstanceRevalidation::Unavailable;
  }
  return *planned == *live
      ? ActiveSeedInstanceRevalidation::Available
      : ActiveSeedInstanceRevalidation::Stale;
}

inline constexpr bool activeSeedMergeTicketAttributionEnabled(
    bool perfEnabled,
    const ActiveSeedMergeTicketContext& context,
    std::size_t targetCount) noexcept {
  return perfEnabled && context.valid() && targetCount != 0u;
}

static_assert(std::is_trivially_copyable_v<RenderPassInstanceToken>);
static_assert(std::is_standard_layout_v<RenderPassInstanceToken>);
static_assert(std::is_trivially_copyable_v<ActiveSeedMergeCommandWitness>);
static_assert(std::is_standard_layout_v<ActiveSeedMergeCommandWitness>);
static_assert(std::is_trivially_copyable_v<ActiveSeedMergeTargetWitness>);
static_assert(std::is_standard_layout_v<ActiveSeedMergeTargetWitness>);
static_assert(std::is_trivially_copyable_v<ActiveSeedMergeTicketContext>);
static_assert(std::is_standard_layout_v<ActiveSeedMergeTicketContext>);

}  // namespace dxmt9::encoders
