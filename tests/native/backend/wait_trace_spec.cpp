#include "../../../src/d3d9/device_c_replay_offload.hpp"
#include "dxmt9/dxmt9_wait_trace.hpp"

#include <atomic>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace wt = dxmt9::waittrace;
using namespace dxmt9::d3d9;
using dxmt9::query::ReplayReceiptState;
// Queue lock failure must propagate to the existing pollQueryFenceImpl catch,
// not terminate inside the observer's acquisition helper.
static_assert(!noexcept(wt::queryLock(std::declval<std::mutex&>(), 1, 1, 0, 1)));
void check(bool value) { if (!value) throw std::runtime_error("wait trace invariant"); }

int main(int argc, char** argv) {
  const std::string_view mode = argc > 1 ? argv[1] : "normal";
  ReplayDrainLedger ledger;
  auto target = std::make_shared<ReplayDrainTarget>();
  RawCommandChunk first;
  first.ledgerTargets.push_back(target.get());
  check(ledger.publishInline(first));
  const auto receipt = ledger.captureReceipt(target);
  if (!wt::enabled()) {
    ledger.publishReplayed(first);
    check(ledger.waitReceipt(receipt) == ReplayReceiptState::Ready);
    return 0;
  }
  wt::present(1);
  if (!wt::inWindow()) {
    ledger.publishReplayed(first);
    check(ledger.waitReceipt(receipt) == ReplayReceiptState::Ready);
    return 0;
  }
  // Incarnation must change even when an address is reused.
  int owner = 0;
  wt::registerOwner(&owner);
  const auto old = wt::ownerIdentity(&owner);
  wt::registerOwner(&owner);
  check(wt::ownerIdentity(&owner) > old);
  std::atomic<bool> returned{false};
  std::thread waiter([&] {
    wt::ContextScope scope({71, wt::Site::Receipt});
    check(ledger.waitReceipt(receipt) == (mode == "terminal"
        ? ReplayReceiptState::Failed : mode == "superseded"
        ? ReplayReceiptState::Superseded : ReplayReceiptState::Ready));
    returned.store(true);
  });
  // Diagnostic-only entry counter establishes that the production pending
  // predicate entered its wait, without relying on a sleep duration.
  while (wt::waitEntries() == 0) std::this_thread::yield();
  check(!returned.load());
  if (mode == "cross") wt::present(3);
  if (mode == "incomplete") wt::present(4);
  if (mode == "terminal") ledger.poison();
  else if (mode == "superseded") {
    RawCommandChunk next;
    next.ledgerTargets.push_back(target.get());
    check(ledger.publishInline(next));
  } else ledger.publishReplayed(first);
  waiter.join();
  check(returned.load());
  if (mode == "normal") {
    {
      std::mutex queueMutex;
      wt::ContextScope scope({72, wt::Site::Receipt});
      auto held = wt::queryLock(queueMutex, 5, 7, 0, 5);
      check(held.owns_lock());
    }
    // Already-ready paths preserve results without entering a CV wait.
    const auto waits = wt::waitEntries();
    check(ledger.waitReceipt(receipt) == ReplayReceiptState::Ready);
    check(ledger.wait(*target) == ReplayDrainResult::CaughtUp);
    check(wt::waitEntries() == waits);
    struct SpuriousCV {
      unsigned& state;
      unsigned count = 0;
      void wait(std::unique_lock<std::mutex>&) {
        if (++count == 2) state = 1;
      }
    };
    unsigned state = 0;
    SpuriousCV cv{state};
    std::mutex mutex;
    check(wt::observeWait(mutex, cv, &ledger, target.get(), wt::Site::Resource,
        [&] { return std::pair{state, wt::Snapshot{1, 1, state, state}}; },
        [](auto s) { return s == 0; }) == 1);
    check(cv.count == 2);
  }
  if (mode == "overflow") {
    for (unsigned i = 0; i < 32769; ++i) {
      auto row = wt::begin(wt::Site::Receipt, &ledger, target.get());
      row.acquired = row.end = wt::now();
      row.waits = 1;
      row.before = {1, 1, 0, 0}; row.after = {1, 1, 1, 1};
      wt::enteredWait();
      wt::finish(row);
    }
  }
  if (mode != "unclosed") wt::present(4);
  return 0;
}
