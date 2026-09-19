#include "../../../src/dxmt9/dxmt9_query_poll_observer.hpp"
#include <iostream>
#include <stdexcept>
namespace diag = dxmt9::query::polldiag;
void check(bool b, const char* m) { if (!b) throw std::runtime_error(m); }
int main() try {
  diag::Sample sample;
  sample.timed = true;
  sample.last = 10;
  sample.at(diag::Phase::QueueWait, 30);
  sample.at(diag::Phase::Classify, 40);
  sample.at(diag::Phase::PendingWait, 70);
  sample.at(diag::Phase::Decision, 75);
  sample.at(diag::Phase::Exit, 80);
  sample.request = diag::Request::ExistingTarget;
  diag::Totals total;
  total.add(sample, true, dxmt9::query::Poll::Pending);
  check(total.calls == 1 && total.samples == 1 && total.flush == 1, "sample census");
  check(total.ns == std::array<std::uint64_t, 8>{20,10,0,0,30,5,0,5},
        "disjoint outer and inner lock intervals");
  check(total.maximum == total.ns, "sample maxima");
  check(total.requests[2] == 1 && total.results[2] == 1, "request and result populations");
  sample.at(diag::Phase::Exit, 1);
  total.add(sample, false, dxmt9::query::Poll::Failed);
  check(total.invalid == 1 && total.samples == 1 && total.calls == 2, "bad clock excluded");
  diag::mark<false>(diag::Phase::QueueWait);
  diag::request<false>(diag::Request::New);
  diag::marker<false>();
  check(!diag::active, "disabled hooks do not access TLS");
  unsigned calls = 0;
  const auto result = diag::observe(true, [&] {
    ++calls;
    auto* outer = diag::active;
    check(diag::observe(false, [] { return dxmt9::query::Poll::Complete; }) ==
        dxmt9::query::Poll::Complete, "nested result unchanged");
    check(diag::active == outer, "nested observer restoration");
    return dxmt9::query::Poll::Pending;
  });
  check(result == dxmt9::query::Poll::Pending && calls == 1 && !diag::active,
        "exactly-once operation and result preservation");
  std::cout << "query backend observer spec passed\n";
  return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
