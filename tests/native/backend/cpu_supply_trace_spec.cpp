#include "../../../src/d3d9/device_c_replay_offload.hpp"
#include "dxmt9/dxmt9_cpu_supply_trace.hpp"

#include <atomic>
#include <thread>

int main() {
  using namespace dxmt9::d3d9;
  if (dxmt9::supply::windowActive()) return 4;
  dxmt9::supply::present(1);
  // Failed/existing output admission may keep the sink inactive in-window.
  dxmt9::supply::record(dxmt9::supply::Event::ImportBegin, nullptr);
  const auto start = std::chrono::steady_clock::now();
  volatile std::uint64_t work = 1;
  while (std::chrono::steady_clock::now()-start < std::chrono::milliseconds(40)) {
    for (unsigned i=0; i<1024; ++i) work = work*3u + i;
  }
  dxmt9::supply::record(dxmt9::supply::Event::ImportEnd, nullptr);
  ReplayOffloadQueue queue(8, 4096);
  ReplayDrainLedger ledger;
  const auto reservation = queue.reserveMutation(4, ledger);
  if (!reservation.valid()) return 1;
  RawCommandChunk chunk;
  chunk.replaySeq = 2;
  if (!queue.push(std::move(chunk))) return 2;
  // Exercise the actual FIFO transitions; queue semantics have independent
  // exhaustive/native coverage in replay_offload_queue_spec.
  std::atomic<bool> popped{false};
  queue.releaseMutation(reservation);
  std::thread worker([&] {
    ReplayQueueItem item;
    if (queue.pop(item)) {
      popped = true;
      queue.markReplayDone();
    }
  });
  worker.join();
  if (!popped) return 3;
  dxmt9::supply::present(2);
  dxmt9::supply::present(4);
  if (dxmt9::supply::windowActive()) return 6;
  return 0;
}
