#include "../../../src/d3d9/device_c_query_poll_observer.hpp"

#include <iostream>
#include <stdexcept>

namespace diag = dxmt9::d3d9::query_poll_diag;
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

int main() try {
  diag::Ledger ledger(false);
  std::uint64_t clock = 0;
  unsigned drainCalls = 0, providerCalls = 0;
  const auto drain = [&] { ++drainCalls; return true; };
  const auto now = [&] { return clock += 10; };
  for (std::int32_t result : {0, 1, -1}) {
    const auto actual = diag::observeCall(ledger, 1, -2, drain,
        [&] { ++providerCalls; return result; }, now);
    check(actual == result, "normal HRESULT preserved");
  }
  check(diag::observeCall(ledger, 0, -2, [] { return false; },
      [&] { ++providerCalls; return 0; }, now) == -2,
      "drain rejection preserved");
  const auto result = ledger.snapshot();
  check(drainCalls == 3 && providerCalls == 3, "exactly once; rejected drain never enters provider");
  check(result.calls == 4 && result.flush == 3 && result.drainFailures == 1,
        "call populations");
  check(result.totalNs == 70 && result.drainNs == 40 && result.providerNs == 30,
        "exact disjoint intervals");
  check(result.results == std::array<std::uint64_t, 3>{1, 1, 2}, "HRESULT populations");
  check(result.resultDrainNs == std::array<std::uint64_t, 3>{10, 10, 20} &&
        result.resultProviderNs == std::array<std::uint64_t, 3>{10, 10, 10},
        "matching per-result intervals");
  diag::Totals invalid;
  invalid.add(true, 0, 0, 20, 10, 30);
  invalid.add(true, 0, 1, 0, 20, 10);
  invalid.add(false, 0, -1, 0, 10, 20);
  check(invalid.invalidClocks == 3 && invalid.totalNs == 0,
        "invalid ordering never wraps into a huge duration");
  std::cout << "query_bridge_poll_observer_spec passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
