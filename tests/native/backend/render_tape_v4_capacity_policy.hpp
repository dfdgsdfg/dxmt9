#pragma once

#include "render_tape_cpu_policy.hpp"

#include <cstddef>
#include <limits>
#include <string_view>

namespace dxmt9::test {

// Shared cold V4 admission only. Runtime provisioning still uses the existing
// cached selectors and queue policy; this grants no timing, pixel,
// source-boundary, or retention/RSS proof.
struct RenderTapeV4CapacityPolicy {
  std::size_t slotHeadroomBytes = 0u;
  std::size_t aggregateHeadroomBytes = 0u;
};

struct RenderTapeV4ReplayPolicy {
  bool productionSlot = false;
  std::size_t drawCommandLimit = 0u;
  RenderTapeV4CapacityPolicy capacity{};
};

inline constexpr CpuTapePolicyAdmission admitRenderTapeV4ReplayPolicy(
    const RenderTapeV4ReplayPolicy& policy) noexcept {
  return admitCpuTapePolicy({
      .compatibility = !policy.productionSlot,
      .drawCommandLimit = policy.drawCommandLimit,
      .slotHeadroomBytes = policy.capacity.slotHeadroomBytes,
      .aggregateHeadroomBytes = policy.capacity.aggregateHeadroomBytes});
}

inline constexpr bool renderTapeV4ReplayPolicyValid(
    const RenderTapeV4ReplayPolicy& policy) noexcept {
  return admitRenderTapeV4ReplayPolicy(policy) == CpuTapePolicyAdmission::Accepted;
}

inline constexpr CpuTapePolicyAdmission admitRenderTapeV4CapacityPolicy(
    const RenderTapeV4CapacityPolicy& policy, bool productionSlot) noexcept {
  return admitCpuTapePolicy({
      .compatibility = !productionSlot,
      .drawCommandLimit = 0u,
      .slotHeadroomBytes = policy.slotHeadroomBytes,
      .aggregateHeadroomBytes = policy.aggregateHeadroomBytes});
}

inline constexpr bool renderTapeV4CapacityPolicyValid(
    const RenderTapeV4CapacityPolicy& policy, bool productionSlot) noexcept {
  return admitRenderTapeV4CapacityPolicy(policy, productionSlot) == CpuTapePolicyAdmission::Accepted;
}

// Presence pairing is separate from value admission: an explicit half-pair
// must reject even when its supplied value is zero. Duplicate flags are the
// CLI's responsibility, before either cached runtime selector is initialized.
inline constexpr bool renderTapeV4CapacityFlagsPaired(
    bool slotSpecified, bool aggregateSpecified) noexcept {
  return slotSpecified == aggregateSpecified;
}

inline constexpr bool parseRenderTapeV4CapacityBytes(
    std::string_view text, std::size_t& out) noexcept {
  if (text.empty()) return false;
  std::size_t value = 0u;
  for (const auto character : text) {
    if (character < '0' || character > '9') return false;
    const auto digit = static_cast<std::size_t>(character - '0');
    if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10u) return false;
    value = value * 10u + digit;
  }
  out = value;
  return true;
}

static_assert(renderTapeV4ReplayPolicyValid({}));
static_assert(renderTapeV4ReplayPolicyValid({.productionSlot = true}));
static_assert(renderTapeV4ReplayPolicyValid({.drawCommandLimit = 3u}));
static_assert(renderTapeV4ReplayPolicyValid({.drawCommandLimit = 64u}));
static_assert(admitRenderTapeV4ReplayPolicy({.drawCommandLimit = 1u}) ==
    CpuTapePolicyAdmission::DrawLimitOutOfRange);
static_assert(admitRenderTapeV4ReplayPolicy({.drawCommandLimit = 2u}) ==
    CpuTapePolicyAdmission::DrawLimitOutOfRange);
static_assert(admitRenderTapeV4ReplayPolicy({.drawCommandLimit = 65u}) ==
    CpuTapePolicyAdmission::DrawLimitOutOfRange);
static_assert(admitRenderTapeV4ReplayPolicy({.productionSlot = true, .drawCommandLimit = 3u}) ==
    CpuTapePolicyAdmission::DrawLimitRequiresCompatibility);
static_assert(admitRenderTapeV4ReplayPolicy({.drawCommandLimit = 3u,
    .capacity = {kCpuTapeMinSlotHeadroomBytes, kCpuTapeMinSlotHeadroomBytes}}) ==
    CpuTapePolicyAdmission::DrawLimitConflictsWithHeadroom);
static_assert(admitRenderTapeV4ReplayPolicy({.productionSlot = true, .drawCommandLimit = 3u,
    .capacity = {kCpuTapeMinSlotHeadroomBytes, kCpuTapeMinSlotHeadroomBytes}}) ==
    CpuTapePolicyAdmission::DrawLimitConflictsWithHeadroom);
static_assert(renderTapeV4CapacityPolicyValid({}, false));
static_assert(renderTapeV4CapacityPolicyValid({}, true));
static_assert(renderTapeV4CapacityPolicyValid(
    {kCpuTapeMinSlotHeadroomBytes, kCpuTapeMinSlotHeadroomBytes}, true));
static_assert(renderTapeV4CapacityPolicyValid(
    {kCpuTapeMaxSlotHeadroomBytes, kCpuTapeMaxAggregateHeadroomBytes}, true));
static_assert(admitRenderTapeV4CapacityPolicy(
    {kCpuTapeMinSlotHeadroomBytes, kCpuTapeMinSlotHeadroomBytes}, false) ==
    CpuTapePolicyAdmission::HeadroomRequiresProductionSlot);
static_assert(admitRenderTapeV4CapacityPolicy({0u, kCpuTapeMinSlotHeadroomBytes}, true) ==
    CpuTapePolicyAdmission::HeadroomMustBePaired);
static_assert(!renderTapeV4CapacityPolicyValid({kCpuTapeMinSlotHeadroomBytes - 1u,
    kCpuTapeMinSlotHeadroomBytes}, true));
static_assert(!renderTapeV4CapacityPolicyValid({kCpuTapeMaxSlotHeadroomBytes + 1u,
    kCpuTapeMaxAggregateHeadroomBytes}, true));
static_assert(!renderTapeV4CapacityPolicyValid({kCpuTapeMinSlotHeadroomBytes,
    kCpuTapeMaxAggregateHeadroomBytes + 1u}, true));
static_assert(!renderTapeV4CapacityPolicyValid({kCpuTapeMaxSlotHeadroomBytes,
    kCpuTapeMinSlotHeadroomBytes}, true));
static_assert(renderTapeV4CapacityFlagsPaired(false, false));
static_assert(renderTapeV4CapacityFlagsPaired(true, true));
static_assert(!renderTapeV4CapacityFlagsPaired(true, false));
static_assert(!renderTapeV4CapacityFlagsPaired(false, true));
static_assert([] {
  std::size_t value = 7u;
  return parseRenderTapeV4CapacityBytes("0", value) && value == 0u &&
      parseRenderTapeV4CapacityBytes("1048576", value) && value == kCpuTapeMinSlotHeadroomBytes &&
      !parseRenderTapeV4CapacityBytes("", value) &&
      !parseRenderTapeV4CapacityBytes("+1", value) &&
      !parseRenderTapeV4CapacityBytes("-1", value) &&
      !parseRenderTapeV4CapacityBytes(" 1", value) &&
      !parseRenderTapeV4CapacityBytes("1x", value) &&
      !parseRenderTapeV4CapacityBytes("9999999999999999999999999999999999999999", value) &&
      value == kCpuTapeMinSlotHeadroomBytes;
}());

}  // namespace dxmt9::test
