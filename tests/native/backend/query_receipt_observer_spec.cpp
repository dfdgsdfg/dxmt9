#include "../../../src/d3d9/device_c_query_receipt_observer.hpp"
#include <iostream>
#include <stdexcept>
namespace diag = dxmt9::d3d9::query_receipt_diag;
void check(bool b, const char* m) { if (!b) throw std::runtime_error(m); }
int main() try {
  diag::Totals totals;
  diag::Active a;
  a.sample.timed = true;
  a.sample.route = diag::Route::Handled;
  a.sample.kind = 1;
  a.last = 100;
  a.markAt(diag::Phase::Capture, 110);
  a.markAt(diag::Phase::ReplayWait, 150);
  a.markAt(diag::Phase::Snapshot, 170);
  a.markAt(diag::Phase::Backend, 200);
  a.markAt(diag::Phase::Final, 205);
  totals.add(a.sample, 1, 1);
  check(totals.ns == std::array<std::uint64_t, 5>{10, 40, 20, 30, 5},
        "disjoint phase intervals");
  check(totals.calls == 1 && totals.flush == 1 && totals.samples == 1 &&
        totals.results[1] == 1 && totals.sampledResults[1] == 1 && totals.kinds[1] == 1,
        "pending population and sample denominator");
  diag::Sample fallback;
  fallback.route = diag::Route::Ineligible;
  totals.add(fallback, 0, std::nullopt);
  check(totals.calls == 2 && totals.results[3] == 1 && totals.samples == 1 &&
        totals.kinds[5] == 1, "unsampled fallback is not an HRESULT or timed call");
  a.markAt(diag::Phase::Final, 1);
  totals.add(a.sample, 0, -1);
  check(totals.invalidClocks == 1 && totals.samples == 1 && totals.results[2] == 1,
        "invalid clock preserves census, excludes timing");
  diag::mark<false>(diag::Phase::Capture);
  diag::route<false>(diag::Route::Handled);
  diag::kind<false>(1);
  check(diag::active == nullptr, "disabled template never accesses diagnostic TLS");
  unsigned calls = 0;
  const auto result = diag::observe(1, [&]() -> std::optional<std::int32_t> {
    ++calls;
    auto* outer = diag::active;
    const auto nested = diag::observe(0, []() -> std::optional<std::int32_t> {
      return std::nullopt;
    });
    check(!nested && diag::active == outer, "nested scope restores observer only");
    return 1;
  });
  check(result == 1 && calls == 1 && !diag::active, "exactly-once and HRESULT preserved");
  bool threw = false;
  try {
    diag::observe(0, []() -> std::optional<std::int32_t> { throw 42; });
  } catch (int value) { threw = value == 42; }
  check(threw && !diag::active, "exception and TLS restoration preserved");
  std::cout << "query receipt observer spec passed\n";
  return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
