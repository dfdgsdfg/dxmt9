#include "dxmt9/dxmt9_perf_counters.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using dxmt9::perf::test::snapshotBindingCounters;

template <std::size_t N, typename Read, typename Apply>
void checkFamily(Read read, Apply apply, bool enabled) {
  auto expected = read();
  const auto verify = [&] {
    if (read() != expected) throw std::runtime_error("binding counter mapping or accumulation");
  };
  // Exhaust zero/nonzero field masks, including all-zero batches. Distinct
  // nonzero values expose permutations; the first field also covers UINT_MAX.
  for (std::size_t mask = 0; mask < (std::size_t{1} << N); ++mask) {
    std::array<std::uint32_t, N> values{};
    for (std::size_t i = 0; i < N; ++i) {
      values[i] = mask & (std::size_t{1} << i) ? UINT32_MAX - i : 0u;
      if (enabled) expected[i] += values[i];
    }
    apply(values);
    verify();
  }
  std::array<std::uint32_t, N> values{};
  for (std::size_t i = 0; i < N; ++i) values[i] = i + 1u;
  const auto worker = [&] {
    for (unsigned i = 0; i < 1000u; ++i) apply(values);
  };
  std::thread first(worker), second(worker);
  first.join();
  second.join();
  if (enabled)
    for (std::size_t i = 0; i < N; ++i) expected[i] += 2000u * values[i];
  verify();
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2 || (std::string_view(argv[1]) != "on" &&
                     std::string_view(argv[1]) != "off")) return 2;
    const bool enabled = std::string_view(argv[1]) == "on";
    if (dxmt9::perf::enabled() != enabled) throw std::runtime_error("counter gate");
    checkFamily<10>([] { return snapshotBindingCounters().binds; }, [](const auto& v) {
      dxmt9::perf::countBaseStateBind(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
    }, enabled);
    checkFamily<2>([] { return snapshotBindingCounters().skips; }, [](const auto& v) {
      dxmt9::perf::countBaseStateBindSkip(v[0], v[1]);
    }, enabled);
    checkFamily<7>([] { return snapshotBindingCounters().extendedSkips; }, [](const auto& v) {
      dxmt9::perf::countBaseStateBindSkipExtended(v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
    }, enabled);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
