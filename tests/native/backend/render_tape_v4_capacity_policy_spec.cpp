#include "render_tape_v4_capacity_policy.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace dxmt9::test;
constexpr std::size_t kMiB = 1024u * 1024u;

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void decimalParsing() {
  struct Case {
    std::string_view text;
    std::size_t bytes;
  };
  constexpr std::array cases{
      Case{"0", 0u}, Case{"000", 0u},
      Case{"1048576", kMiB}, Case{"0001048576", kMiB},
      Case{"5242880", 5u * kMiB}, Case{"67108864", 64u * kMiB},
      Case{"536870912", 512u * kMiB}};
  for (const auto& item : cases) {
    std::size_t value = 17u;
    check(parseRenderTapeV4CapacityBytes(item.text, value) && value == item.bytes,
          "decimal byte parsing preserves the exact value");
  }

  const auto maximum = std::numeric_limits<std::size_t>::max();
  const auto maximumText = std::to_string(maximum);
  std::size_t value = 17u;
  check(parseRenderTapeV4CapacityBytes(maximumText, value) && value == maximum,
        "the parser accepts size_t maximum without wrapping");
  check(!renderTapeV4CapacityPolicyValid({value, value}, true),
        "numeric parsing does not grant capacity-policy admission");

  constexpr std::array<std::string_view, 17u> malformed{
      "", "+1", "-1", " 1", "1 ", "\t1", "1\t", "1\n", "\n1",
      "1.0", "1e6", "0x100000", "1MiB", "1_048_576", "1,048,576",
      std::string_view("1\0", 2u), std::string_view("\0", 1u)};
  for (const auto text : malformed) {
    value = 5u * kMiB;
    check(!parseRenderTapeV4CapacityBytes(text, value) && value == 5u * kMiB,
          "malformed decimal input rejects without changing output");
  }
  for (const auto& text : {maximumText + "0",
                           std::string("9999999999999999999999999999999999999999")}) {
    value = kMiB;
    check(!parseRenderTapeV4CapacityBytes(text, value) && value == kMiB,
          "decimal overflow rejects without changing output");
  }
}

void explicitPresencePairing() {
  check(renderTapeV4CapacityFlagsPaired(false, false),
        "omitted capacity flags retain the exact-fit default");
  check(renderTapeV4CapacityFlagsPaired(true, true),
        "both explicit capacity flags form a complete identity");
  check(!renderTapeV4CapacityFlagsPaired(true, false) &&
            !renderTapeV4CapacityFlagsPaired(false, true),
        "either explicit half-pair rejects, including a supplied zero");
  std::size_t zero = 17u;
  check(parseRenderTapeV4CapacityBytes("0", zero) && zero == 0u &&
            renderTapeV4CapacityPolicyValid({zero, zero}, true) &&
            !renderTapeV4CapacityFlagsPaired(true, false),
        "zero value admission cannot erase an incomplete explicit flag pair");
}

void capacityAdmissionBoundaries() {
  for (const bool productionSlot : {false, true}) {
    check(admitRenderTapeV4CapacityPolicy({}, productionSlot) ==
              CpuTapePolicyAdmission::Accepted &&
              renderTapeV4CapacityPolicyValid({}, productionSlot),
          "exact-fit paired zeros are valid on either route");
  }
  for (const std::size_t slot : {kMiB, 5u * kMiB, 64u * kMiB}) {
    for (const std::size_t aggregate : {slot, 512u * kMiB}) {
      const RenderTapeV4CapacityPolicy policy{slot, aggregate};
      check(admitRenderTapeV4CapacityPolicy(policy, true) ==
                CpuTapePolicyAdmission::Accepted &&
                renderTapeV4CapacityPolicyValid(policy, true),
            "one/five/sixty-four MiB production capacity has bounded admission");
      check(admitRenderTapeV4CapacityPolicy(policy, false) ==
                CpuTapePolicyAdmission::HeadroomRequiresProductionSlot &&
                !renderTapeV4CapacityPolicyValid(policy, false),
            "compatibility cannot select positive direct-slot capacity");
    }
  }

  check(admitRenderTapeV4CapacityPolicy({kMiB, 0u}, true) ==
            CpuTapePolicyAdmission::HeadroomMustBePaired &&
            admitRenderTapeV4CapacityPolicy({0u, kMiB}, true) ==
            CpuTapePolicyAdmission::HeadroomMustBePaired,
        "one-sided positive capacity values reject");
  for (const std::size_t slot : {std::size_t{1u}, kMiB - 1u, 64u * kMiB + 1u}) {
    check(admitRenderTapeV4CapacityPolicy({slot, 512u * kMiB}, true) ==
              CpuTapePolicyAdmission::SlotHeadroomOutOfRange,
          "slot values outside one through sixty-four MiB reject");
  }
  check(admitRenderTapeV4CapacityPolicy({kMiB, 512u * kMiB + 1u}, true) ==
            CpuTapePolicyAdmission::AggregateHeadroomOutOfRange,
        "aggregate capacity above five-hundred-twelve MiB rejects");
  check(admitRenderTapeV4CapacityPolicy({5u * kMiB, kMiB}, true) ==
            CpuTapePolicyAdmission::AggregateHeadroomBelowSlot,
        "aggregate cannot be smaller than its physical-payload ceiling");
  const auto maximum = std::numeric_limits<std::size_t>::max();
  check(!renderTapeV4CapacityPolicyValid({maximum, maximum}, true) &&
            !renderTapeV4CapacityPolicyValid({kMiB, maximum}, true),
        "maximum parsed integers cannot bypass either policy bound");
}

void existingCpuTapePolicyParity() {
  // The existing cold admission is the oracle; no queue/storage policy is
  // reproduced here, and this correspondence grants no execution authority.
  const std::array values{
      std::size_t{0u}, std::size_t{1u}, kMiB - 1u, kMiB, 5u * kMiB,
      64u * kMiB, 64u * kMiB + 1u, 512u * kMiB,
      512u * kMiB + 1u, std::numeric_limits<std::size_t>::max()};
  for (const bool productionSlot : {false, true}) {
    for (const auto slot : values) {
      for (const auto aggregate : values) {
        const RenderTapeV4CapacityPolicy v4{slot, aggregate};
        const CpuTapePolicy existing{
            .compatibility = !productionSlot, .drawCommandLimit = 0u,
            .slotHeadroomBytes = slot, .aggregateHeadroomBytes = aggregate};
        check(admitRenderTapeV4CapacityPolicy(v4, productionSlot) ==
                  admitCpuTapePolicy(existing),
              "V4 preserves every existing capacity admission reason");
        check(renderTapeV4CapacityPolicyValid(v4, productionSlot) ==
                  cpuTapePolicyValid(existing),
              "V4 valid predicate preserves existing cold policy admission");
      }
    }
  }
}

void groupingAndCapacityAdmission() {
  for (const bool productionSlot : {false, true}) {
    check(renderTapeV4ReplayPolicyValid({.productionSlot = productionSlot}),
        "zero grouping and exact-fit capacity retain either explicit route");
    for (const std::size_t limit : {std::size_t{3u}, std::size_t{4u}, std::size_t{63u}, std::size_t{64u}}) {
      const RenderTapeV4ReplayPolicy policy{.productionSlot = productionSlot, .drawCommandLimit = limit};
      check(admitRenderTapeV4ReplayPolicy(policy) == (productionSlot
                ? CpuTapePolicyAdmission::DrawLimitRequiresCompatibility : CpuTapePolicyAdmission::Accepted),
          "positive bounded grouping is Compatibility-only, never silently substituted");
    }
    for (const std::size_t limit : {std::size_t{1u}, std::size_t{2u}, std::size_t{65u},
                                    std::numeric_limits<std::size_t>::max()}) {
      check(admitRenderTapeV4ReplayPolicy({.productionSlot = productionSlot, .drawCommandLimit = limit}) ==
                (productionSlot ? CpuTapePolicyAdmission::DrawLimitRequiresCompatibility
                                : CpuTapePolicyAdmission::DrawLimitOutOfRange),
          "tiny/oversized grouping limits reject with the existing ordered admission reason");
    }
    for (const RenderTapeV4CapacityPolicy capacity : {RenderTapeV4CapacityPolicy{kMiB, kMiB},
             RenderTapeV4CapacityPolicy{kMiB, 0u}, RenderTapeV4CapacityPolicy{0u, kMiB},
             RenderTapeV4CapacityPolicy{64u * kMiB, 512u * kMiB}}) {
      check(admitRenderTapeV4ReplayPolicy({.productionSlot = productionSlot,
                .drawCommandLimit = 3u, .capacity = capacity}) ==
                CpuTapePolicyAdmission::DrawLimitConflictsWithHeadroom,
          "mixed positive grouping/capacity fails explicitly on either route before effects");
    }
  }
  for (const std::string_view text : {"0", "3", "0003", "64"}) {
    std::size_t limit = 17u;
    check(parseRenderTapeV4CapacityBytes(text, limit) &&
              renderTapeV4ReplayPolicyValid({.drawCommandLimit = limit}),
        "the shared strict integer parser retains the exact admitted grouping value");
  }
  std::size_t limit = 17u;
  check(!parseRenderTapeV4CapacityBytes("3suffix", limit) && limit == 17u &&
            !parseRenderTapeV4CapacityBytes("+3", limit) && limit == 17u,
        "loose production environment syntax is rejected by cold grouping admission");
  // The existing raw64 arithmetic is intentionally absent. Numeric policy
  // admission grants no Raw256 residency/progress or semantic authority.
}

void replayPolicyParity() {
  constexpr std::array limits{std::size_t{0u}, std::size_t{1u}, std::size_t{2u},
      std::size_t{3u}, std::size_t{4u}, std::size_t{63u}, std::size_t{64u}, std::size_t{65u},
      std::numeric_limits<std::size_t>::max()};
  constexpr std::array capacities{RenderTapeV4CapacityPolicy{}, RenderTapeV4CapacityPolicy{kMiB, kMiB},
      RenderTapeV4CapacityPolicy{kMiB, 0u}, RenderTapeV4CapacityPolicy{0u, kMiB},
      RenderTapeV4CapacityPolicy{5u * kMiB, kMiB}, RenderTapeV4CapacityPolicy{kMiB - 1u, kMiB},
      RenderTapeV4CapacityPolicy{64u * kMiB, 512u * kMiB},
      RenderTapeV4CapacityPolicy{64u * kMiB + 1u, 512u * kMiB},
      RenderTapeV4CapacityPolicy{kMiB, 512u * kMiB + 1u}};
  for (const bool productionSlot : {false, true}) {
    for (const auto limit : limits) {
      for (const auto capacity : capacities) {
        const RenderTapeV4ReplayPolicy replay{productionSlot, limit, capacity};
        const CpuTapePolicy existing{.compatibility = !productionSlot, .drawCommandLimit = limit,
            .slotHeadroomBytes = capacity.slotHeadroomBytes,
            .aggregateHeadroomBytes = capacity.aggregateHeadroomBytes};
        check(admitRenderTapeV4ReplayPolicy(replay) == admitCpuTapePolicy(existing) &&
                  renderTapeV4ReplayPolicyValid(replay) == cpuTapePolicyValid(existing),
            "shared replay policy preserves every existing grouping/capacity combination and failure reason");
      }
    }
  }
}

}  // namespace

int main() {
  try {
    decimalParsing();
    explicitPresencePairing();
    capacityAdmissionBoundaries();
    existingCpuTapePolicyParity();
    groupingAndCapacityAdmission();
    replayPolicyParity();
    std::cout << "V4 replay policy: pure parsing and bounded grouping/capacity admission passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "V4 capacity policy failed: " << error.what() << '\n';
    return 1;
  }
}
