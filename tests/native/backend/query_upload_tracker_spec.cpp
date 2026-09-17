#include "dxmt9/query_upload_tracker.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
using dxmt9::query::UploadStatus;
using Tracker = dxmt9::query::UploadTracker<std::uint32_t, 64>;

void testCapacityAndErrors() {
  std::array<UploadStatus, 64> statuses{};
  auto status = [&](std::uint32_t id) { return statuses.at(id); };
  Tracker full;
  for (unsigned i = 0; i != 64; ++i) {
    check(full.admit(true, i + 1), "all 64 entries admitted");
    check(full.append(i, i + 1), "all 64 entries retained");
  }
  check(full.poll(64, status) == 0, "signals cannot authorize incomplete uploads");
  check(!full.admit(true, 65) && full.failed(), "65th upload fails closed before allocation");
  check(full.pending() == 64 && full.published() == 64, "overflow preserves every receipt");
  statuses.fill(UploadStatus::Complete);
  check(full.poll(64, status) == 0, "terminal tracker cannot recover into successful completion");

  // Every possible FIFO position can fail; later successful buffers/signals
  // must not skip that failed prefix. This is the production reaper, with a
  // fake status provider instead of Metal's nondeterministic status reads.
  for (unsigned fault = 0; fault != 64; ++fault) {
    Tracker tracker;
    statuses.fill(UploadStatus::Complete);
    statuses[fault] = UploadStatus::Failed;
    for (unsigned i = 0; i != 64; ++i) check(tracker.append(i, i + 1), "append fault fixture");
    check(tracker.poll(64, status) == fault, "failed upload never advances completion");
    check(tracker.failed() && tracker.pending() == 64 - fault, "unobserved suffix stays retained");
  }

  Tracker reusable;
  statuses.fill(UploadStatus::Complete);
  for (unsigned i = 0; i != 192; ++i) {
    check(reusable.admit(true, i + 1) && reusable.append(i % 64, i + 1), "FIFO wraps without loss");
    check(reusable.poll(i, status) == i, "status cannot outrun observed event");
    check(reusable.poll(i + 1, status) == i + 1 && reusable.pending() == 0, "reap exact prefix");
  }
  check(!reusable.append(1, 192) && reusable.failed(), "stale/duplicate ordinal rejected");
  Tracker absent;
  check(!absent.admit(false, 1) && absent.failed(), "missing event fails closed");
  Tracker exhausted;
  check(!exhausted.admit(true, std::numeric_limits<std::uint64_t>::max()), "counter cannot wrap");
  Tracker allocationFailure;
  allocationFailure.fail();
  check(!allocationFailure.append(0, 1) && allocationFailure.failed(), "allocation failure is terminal");
}
} // namespace

int main() {
  try {
    testCapacityAndErrors();
    std::cout << "query_upload_tracker_spec passed (64 positions, overflow, 3 wraps, failures)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
