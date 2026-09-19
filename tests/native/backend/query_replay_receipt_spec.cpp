#include "../../../include/dxmt9/query_replay_receipt.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

dxmt9::query::ReplayReceiptState expected(
    std::uint64_t captured, std::uint64_t admitted,
    std::uint64_t replayed, bool terminal) {
  // Independent reference relation: terminal/invalid frontier has priority,
  // then same-admission identity, then completion of the fixed prefix.
  if (terminal || captured > admitted || replayed > admitted)
    return dxmt9::query::ReplayReceiptState::Failed;
  if (captured != admitted)
    return dxmt9::query::ReplayReceiptState::Superseded;
  return replayed >= captured ? dxmt9::query::ReplayReceiptState::Ready
                              : dxmt9::query::ReplayReceiptState::Pending;
}

void exhaustivePredicateTable() {
  using dxmt9::query::ReplayReceiptState;
  std::size_t cases = 0;
  for (std::uint64_t captured = 0; captured <= 3; ++captured) {
    for (std::uint64_t admitted = 0; admitted <= 3; ++admitted) {
      for (std::uint64_t replayed = 0; replayed <= 3; ++replayed) {
        for (bool terminal : {false, true}) {
          const auto actual = dxmt9::query::classifyReplayReceipt(
              captured, admitted, replayed, terminal);
          check(actual == expected(captured, admitted, replayed, terminal),
                "query replay receipt predicate drifted");
          ++cases;
        }
      }
    }
  }
  check(cases == 128u, "query replay receipt table did not exhaust domain");

  check(dxmt9::query::classifyReplayReceipt(2, 1, 0, false) ==
            ReplayReceiptState::Failed,
        "captured frontier is invalid before supersession");
  check(dxmt9::query::classifyReplayReceipt(1, 2, 2, true) ==
            ReplayReceiptState::Failed,
        "terminal failure has priority over ready completion");
  check(dxmt9::query::classifyReplayReceipt(1, 2, 3, false) ==
            ReplayReceiptState::Failed,
        "replayed frontier cannot outrun admission");
  check(dxmt9::query::classifyReplayReceipt(2, 3, 0, false) ==
            ReplayReceiptState::Superseded,
        "newer admission supersedes without extending the old wait");
  check(dxmt9::query::classifyReplayReceipt(2, 2, 1, false) ==
            ReplayReceiptState::Pending,
        "fixed prefix remains pending before replay completion");
  check(dxmt9::query::classifyReplayReceipt(2, 2, 2, false) ==
            ReplayReceiptState::Ready,
        "exact captured prefix is ready after replay completion");
}

}  // namespace

int main() {
  try {
    exhaustivePredicateTable();
  } catch (const TestFailure& failure) {
    std::cerr << "query_replay_receipt_spec: " << failure.what() << '\n';
    return 1;
  }
  std::cout << "query_replay_receipt_spec passed (128 predicate cases)\n";
  return 0;
}
