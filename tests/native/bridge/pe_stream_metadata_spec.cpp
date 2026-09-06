#include "d3d9_pe_swvp_stream_override.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

namespace {

bool countAllocations = false;
std::size_t allocationCount = 0u;

bool check(bool condition, const char* message) {
  if (!condition) std::cerr << "pe_stream_metadata_spec failed: " << message << '\n';
  return condition;
}

struct Binding {
  std::uint32_t object = 0u;
  std::uint32_t offset = 0u;
  std::uint32_t stride = 0u;
  std::uint32_t marker = 0u;

  bool operator==(const Binding&) const = default;
};

using Bindings = std::array<Binding, 16u>;

Bindings makeBindings(std::uint32_t base) {
  Bindings bindings{};
  for (std::size_t stream = 0; stream < bindings.size(); ++stream) {
    bindings[stream] = Binding{
      base + static_cast<std::uint32_t>(stream),
      base + static_cast<std::uint32_t>(stream * 3u),
      16u + static_cast<std::uint32_t>(stream),
      0xabc00000u + static_cast<std::uint32_t>(stream),
    };
  }
  return bindings;
}

bool testAbaAndExactRestore() {
  Bindings bindings = makeBindings(10u);
  const Bindings initial = bindings;
  {
    dxmt9::d3d9::pe::PeSwvpStreamOverrideGuard guard(bindings);
    bindings[0] = Binding{200u, 700u, 99u, 0xfeedu};
    bindings[1].offset = 701u;
    bindings[2].offset = 702u;
    bindings[1].object = 900u;
    bindings[1].stride = 901u;
    const Binding b = bindings[0];
    bindings[0] = initial[0];
    bindings[1].offset = 703u;
    bindings[2].offset = 704u;
    bindings[0] = b;
    guard.restore();
  }
  bool offsetsRestored = true;
  for (std::size_t stream = 0; stream < bindings.size(); ++stream) {
    offsetsRestored &= bindings[stream].offset == initial[stream].offset;
  }
  return check(bindings[0] == initial[0] && offsetsRestored,
        "A-B-A restores stream 0 and all offsets") &&
    check(bindings[1].object == 900u && bindings[1].stride == 901u &&
       bindings[1].marker == initial[1].marker,
       "stream 1 identity and stride remain outside the restore scope");
}

bool testPartialOverflow() {
  std::uint32_t overflowResult = 0xdeadbeefu;
  const bool overflow = !dxmt9::d3d9::pe::peSwvpCheckedInstanceOffset(
    std::numeric_limits<std::uint32_t>::max() - 1u, 1u, 2u,
    overflowResult);
  std::uint32_t result = 0u;
  const bool valid = dxmt9::d3d9::pe::peSwvpCheckedInstanceOffset(
    7u, 3u, 5u, result);
  return check(overflow && overflowResult == 0xdeadbeefu,
        "partial overflow fails without corrupting result") &&
    check(valid && result == 22u, "bounded offset succeeds exactly");
}

bool testFailureRestoration() {
  Bindings bindings = makeBindings(30u);
  const Binding originalStream0 = bindings[0];
  const auto originalOffsets = [&] {
    std::array<std::uint32_t, 16u> result{};
    for (std::size_t stream = 0; stream < result.size(); ++stream) {
      result[stream] = bindings[stream].offset;
    }
    return result;
  }();
  try {
    dxmt9::d3d9::pe::PeSwvpStreamOverrideGuard guard(bindings);
    bindings[0] = Binding{99u, 100u, 101u, 102u};
    for (auto& binding : bindings) binding.offset += 1000u;
    throw std::runtime_error("bounded failure");
  } catch (const std::runtime_error&) {
  }
  bool offsetsRestored = true;
  for (std::size_t stream = 0; stream < bindings.size(); ++stream) {
    offsetsRestored &= bindings[stream].offset == originalOffsets[stream];
  }
  return check(bindings[0].object == originalStream0.object &&
        bindings[0].stride == originalStream0.stride &&
        bindings[0].marker == originalStream0.marker,
        "exception restores complete stream 0 binding") &&
    check(offsetsRestored, "exception restores all stream offsets");
}

bool testBoundedOverrideRefinement() {
  using namespace dxmt9::d3d9::pe;
  constexpr auto max = std::numeric_limits<std::uint32_t>::max();
  constexpr std::array<std::uint32_t, 5u> domain{0u, 1u, 16u, max - 1u, max};
  for (auto base : domain) for (auto element : domain) for (auto stride : domain) {
    std::uint32_t result = 0x12345678u;
    const std::uint64_t expected = std::uint64_t{base} +
        std::uint64_t{element} * stride;
    const bool fits = peSwvpCheckedInstanceOffset(base, element, stride, result);
    if (!check(fits == (expected <= max) &&
                   result == (fits ? static_cast<std::uint32_t>(expected) : 0x12345678u),
               "bounded offset truth table matches wide arithmetic")) return false;
  }
  for (unsigned mask = 0u; mask < 16u; ++mask) {
    Bindings bindings = makeBindings(70u);
    const auto initial = bindings;
    bool sawOverflow = false;
    {
      PeSwvpStreamOverrideGuard guard(bindings);
      bindings[0] = Binding{99u, 3u, 5u, 7u};
      for (unsigned slot = 1u; slot <= 4u; ++slot) {
        if ((mask & (1u << (slot - 1u))) == 0u) continue;
        std::uint32_t next = 0u;
        if (!peSwvpCheckedInstanceOffset(bindings[slot].offset, 2u,
                                        bindings[slot].stride, next)) return false;
        bindings[slot].offset = next;
      }
      // A later slot fails after the prefix already changed; RAII must restore
      // that prefix, not merely leave the failed scalar output untouched.
      std::uint32_t failed = 99u;
      sawOverflow = !peSwvpCheckedInstanceOffset(max, 1u, 1u, failed) && failed == 99u;
    }
    if (!check(sawOverflow && bindings == initial,
               "partial instance update plus late overflow restores all prior values")) return false;
  }
  return true;
}

bool testNoAllocation() {
  Bindings bindings = makeBindings(50u);
  allocationCount = 0u;
  countAllocations = true;
  {
    dxmt9::d3d9::pe::PeSwvpStreamOverrideGuard guard(bindings);
    bindings[0].offset = 1u;
    bindings[15].offset = 2u;
  }
  countAllocations = false;
  return check(allocationCount == 0u, "override snapshot allocates nothing");
}

}  // namespace

void* operator new(std::size_t size) {
  if (void* value = std::malloc(size ? size : 1u)) {
    if (countAllocations) ++allocationCount;
    return value;
  }
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

int main() {
  const bool okay = testAbaAndExactRestore() &&
    testPartialOverflow() &&
    testBoundedOverrideRefinement() &&
    testFailureRestoration() &&
    testNoAllocation();
  return okay ? 0 : 1;
}
