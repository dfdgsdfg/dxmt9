#include "d3d9_pe_constant_settlement.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace dxmt9::d3d9::pe;

struct TestFailure : std::runtime_error {
  explicit TestFailure(std::string message)
      : std::runtime_error(std::move(message)) {}
};

void check(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

template <typename Settlement>
concept HasConcreteSettle = requires(
    Settlement& settlement, PeSemanticProducerKind producer,
    const D9CCommandChunkWireSetConst& setConst,
    std::span<const std::byte> bytes) {
  { settlement.settle(producer, setConst, bytes) } -> std::same_as<bool>;
};

static_assert(HasConcreteSettle<PeSemanticConstantSettlement>);
static_assert(std::is_nothrow_constructible_v<PeSemanticConstantSettlement,
                                              PeConstShadowBlock&,
                                              const PeHotStateShadow&,
                                              PendingDeltaTicket>);

struct Family {
  PeSemanticProducerKind producer;
  ConstShadow PeConstShadowBlock::* shadow;
  std::size_t elementBytes;
};

constexpr std::array kFamilies{
    Family{PeSemanticProducerKind::VsFloatConstant, &PeConstShadowBlock::vsConstF,
           sizeof(float) * 4u},
    Family{PeSemanticProducerKind::VsIntConstant, &PeConstShadowBlock::vsConstI,
           sizeof(std::int32_t) * 4u},
    Family{PeSemanticProducerKind::VsBoolConstant, &PeConstShadowBlock::vsConstB,
           sizeof(std::int32_t)},
    Family{PeSemanticProducerKind::PsFloatConstant, &PeConstShadowBlock::psConstF,
           sizeof(float) * 4u},
    Family{PeSemanticProducerKind::PsIntConstant, &PeConstShadowBlock::psConstI,
           sizeof(std::int32_t) * 4u},
    Family{PeSemanticProducerKind::PsBoolConstant, &PeConstShadowBlock::psConstB,
           sizeof(std::int32_t)},
};

struct ShadowSnapshot {
  std::vector<std::uint8_t> values;
  std::vector<std::uint8_t> dirtyElems;
  std::uint32_t dirtyStart;
  std::uint32_t dirtyEnd;
};

ShadowSnapshot snapshot(const ConstShadow& shadow) {
  return {shadow.values, shadow.dirtyElems, shadow.dirtyStart, shadow.dirtyEnd};
}

bool same(const ConstShadow& shadow, const ShadowSnapshot& expected) {
  return shadow.values == expected.values &&
         shadow.dirtyElems == expected.dirtyElems &&
         shadow.dirtyStart == expected.dirtyStart &&
         shadow.dirtyEnd == expected.dirtyEnd;
}

void seed(ConstShadow& shadow, std::uint32_t start, std::uint32_t count,
          std::size_t elementBytes) {
  shadow.values.assign((start + count) * elementBytes, 0xa5u);
  shadow.dirtyElems.assign(start + count, 0u);
  std::fill(shadow.dirtyElems.begin() + start, shadow.dirtyElems.end(), 1u);
  shadow.dirtyStart = start;
  shadow.dirtyEnd = start + count;
}

std::vector<std::byte> bytesFor(std::size_t size) {
  std::vector<std::byte> bytes(size, std::byte{0xa5});
  return bytes;
}

void allSixFamiliesClearOnlySelectedShadow() {
  for (std::size_t familyIndex = 0u; familyIndex < kFamilies.size();
       ++familyIndex) {
    const auto& family = kFamilies[familyIndex];
    PeConstShadowBlock constants;
    PeHotStateShadow pendingState;
    constexpr std::uint32_t start = 3u;
    constexpr std::uint32_t count = 2u;
    for (const auto& other : kFamilies)
      seed(constants.*other.shadow, start, count, other.elementBytes);
    std::array<ShadowSnapshot, kFamilies.size()> before{};
    for (std::size_t i = 0u; i < kFamilies.size(); ++i)
      before[i] = snapshot(constants.*kFamilies[i].shadow);
    const auto bytes = bytesFor(family.elementBytes * count);

    PeSemanticConstantSettlement settlement(constants, pendingState,
                                            pendingState.pendingTicket());
    const D9CCommandChunkWireSetConst setConst{start, count};
    check(settlement.settle(family.producer, setConst, bytes),
          "valid six-family settlement succeeds");
    check((constants.*family.shadow).dirtyStart == 0u &&
              (constants.*family.shadow).dirtyEnd == 0u,
          "success clears only selected dirty range state");
    check((constants.*family.shadow).values == before[familyIndex].values,
          "success preserves selected values");
    check(std::all_of((constants.*family.shadow).dirtyElems.begin(),
                      (constants.*family.shadow).dirtyElems.end(),
                      [](auto bit) { return bit == 0u; }),
          "success clears every represented membership bit");
    for (std::size_t i = 0u; i < kFamilies.size(); ++i) {
      if (i == familyIndex) continue;
      check(same(constants.*kFamilies[i].shadow, before[i]),
            "success leaves unselected shadows untouched");
    }
  }
}

void failedValidationPreservesValuesAndDirty() {
  for (const auto& family : kFamilies) {
    const auto attempt = [&](D9CCommandChunkWireSetConst setConst,
                             std::span<const std::byte> bytes,
                             PendingDeltaTicket ticket) {
      PeConstShadowBlock constants;
      PeHotStateShadow pendingState;
      seed(constants.*family.shadow, 3u, 2u, family.elementBytes);
      const auto before = snapshot(constants.*family.shadow);
      PeSemanticConstantSettlement settlement(constants, pendingState, ticket);
      check(!settlement.settle(family.producer, setConst, bytes),
            "malformed settlement rejects");
      check(same(constants.*family.shadow, before),
            "failed settlement preserves values and dirty range");
    };

    const auto validBytes = bytesFor(family.elementBytes * 2u);
    attempt({3u, 2u}, std::span(validBytes).first(validBytes.size() - 1u), {});
    attempt({4u, 2u}, validBytes, {});
    attempt({3u, 1u}, validBytes, {});
    attempt({3u, 2u}, validBytes, PendingDeltaTicket{2u});
  }
}

void invalidProducerPreservesAllFamilies() {
  PeConstShadowBlock constants;
  PeHotStateShadow pendingState;
  for (const auto& family : kFamilies)
    seed(constants.*family.shadow, 3u, 2u, family.elementBytes);
  std::array<ShadowSnapshot, kFamilies.size()> before{};
  for (std::size_t i = 0u; i < kFamilies.size(); ++i)
    before[i] = snapshot(constants.*kFamilies[i].shadow);
  const auto bytes = bytesFor(32u);
  PeSemanticConstantSettlement settlement(constants, pendingState, {});
  check(!settlement.settle(PeSemanticProducerKind::Present, {3u, 2u}, bytes),
        "non-constant producer rejects");
  for (std::size_t i = 0u; i < kFamilies.size(); ++i)
    check(same(constants.*kFamilies[i].shadow, before[i]),
          "invalid producer preserves every family");
}

void retryIsReusableWithNoObservationInput() {
  const auto run = []() {
    PeConstShadowBlock constants;
    PeHotStateShadow pendingState;
    const auto& family = kFamilies[0];
    seed(constants.*family.shadow, 3u, 1u, family.elementBytes);
    const auto bytes = bytesFor(family.elementBytes);
    const D9CCommandChunkWireSetConst setConst{3u, 1u};
    PeSemanticConstantSettlement retry(constants, pendingState,
                                       PendingDeltaTicket{2u});
    check(!retry.settle(family.producer, setConst, bytes),
          "stale ticket leaves a reusable retry");
    PeSemanticConstantSettlement accepted(constants, pendingState,
                                          pendingState.pendingTicket());
    check(accepted.settle(family.producer, setConst, bytes),
          "same settlement retries after ticket refresh");
    check(!constants.vsConstF.dirty(), "retry clears after valid ticket");
  };
  run();
}

void emptyTicketIsAccepted() {
  PeConstShadowBlock constants;
  PeHotStateShadow pendingState;
  seed(constants.vsConstB, 1u, 1u, sizeof(std::int32_t));
  const auto bytes = bytesFor(sizeof(std::int32_t));
  PeSemanticConstantSettlement settlement(constants, pendingState, {});
  check(settlement.settle(PeSemanticProducerKind::VsBoolConstant, {1u, 1u},
                          bytes),
        "empty ticket remains accepted");
}

}  // namespace

int main() {
  try {
    allSixFamiliesClearOnlySelectedShadow();
    failedValidationPreservesValuesAndDirty();
    invalidProducerPreservesAllFamilies();
    retryIsReusableWithNoObservationInput();
    emptyTicketIsAccepted();
  } catch (const TestFailure& failure) {
    std::cerr << "pe_constant_settlement_spec failed: " << failure.what()
              << '\n';
    return 1;
  }
  return 0;
}
