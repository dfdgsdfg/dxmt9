#include "../../../src/dxmt9/tracing/batch_transport.hpp"
#include "../../../src/dxmt9/tracing/diagnostic_record.hpp"
#include "../../../src/dxmt9/tracing/session_state.hpp"
#include "../../../src/dxmt9/observations/reservation.hpp"

#include <array>
#include <cassert>
#include <thread>
#include <vector>
#include <future>
#include <mutex>

using dxmt9::tracing::BatchTransport;

int main() {
  struct Diagnostic {
    char diagnostic_schema[128]{};
    char diagnostic_payload[192]{};
    uint16_t diagnostic_parts = 1, diagnostic_part = 0, diagnostic_size = 192;
    uint16_t observation_mode = 4;
  } diagnostic;
  using dxmt9::tracing::validDiagnosticRecord;
  assert(validDiagnosticRecord(diagnostic));
  diagnostic.diagnostic_size = 193;
  assert(!validDiagnosticRecord(diagnostic));
  diagnostic.diagnostic_size = 192;
  diagnostic.diagnostic_part = 1;
  assert(!validDiagnosticRecord(diagnostic));
  diagnostic.diagnostic_part = 0;
  diagnostic.diagnostic_parts = 65;
  assert(!validDiagnosticRecord(diagnostic));
  diagnostic.diagnostic_parts = 1;
  std::memset(diagnostic.diagnostic_schema, 65, sizeof(diagnostic.diagnostic_schema));
  assert(!validDiagnosticRecord(diagnostic));

  auto identity = [](uint64_t value) { return value; };
  auto include = [](uint64_t) { return true; };
  BatchTransport<uint64_t, 2> transport;
  assert(!transport.configure(3, 1, 7));
  assert(transport.configure(16, 2, 7));
  assert(transport.producer_count() == 2);
  std::array<uint64_t, 2> pair{11, 12};
  assert(!transport.append(0, pair, false, 6, identity, include));
  assert(transport.append(0, pair, false, 7, identity, include));
  assert(transport.append(0, pair, false, 7, identity, include));
  assert(!transport.append(0, pair, false, 7, identity, include));
  std::vector<uint64_t> persisted;
  transport.drain([&](auto records, uint64_t generation, uint64_t sequence) {
    assert(generation == 7 && sequence != 0);
    persisted.insert(persisted.end(), records.begin(), records.end());
    // A consumer-owned block cannot be reclaimed by its producer.
    if (persisted.size() == 2)
      assert(!transport.append(0, pair, false, 7, identity, include));
  });
  assert((persisted == std::vector<uint64_t>{11, 12, 11, 12}));
  std::array<uint64_t, 1> partial{31};
  assert(transport.append(0, partial, false, 7, identity, include));
  assert(!transport.drain([](auto, auto, auto) { assert(false); }));
  transport.request_partials();
  assert(transport.append(0, partial, false, 7, identity, include));
  unsigned partials = 0;
  transport.drain([&](auto records, auto, auto) {
    assert(records.size() == 1 && records[0] == 31);
    ++partials;
  });
  transport.close_partials();
  transport.drain([&](auto records, auto, auto) {
    assert(records.size() == 1 && records[0] == 31);
    ++partials;
  });
  assert(partials == 2);
  std::array<uint64_t, 5> oversized{};
  assert(!transport.append(0, oversized, true, 7, identity, include));
  std::array<uint64_t, 4> report{41, 42, 43, 44};
  assert(transport.append(0, report, true, 7, identity, include));
  assert(transport.append(1, report, true, 7, identity, include));
  assert(!transport.append(0, report, true, 7, identity, include));
  unsigned reports = 0;
  transport.drain([&](auto records, auto, auto) {
    assert(records.size() == report.size());
    assert(std::equal(records.begin(), records.end(), report.begin()));
    ++reports;
  });
  assert(reports == 2);

  // Small diagnostic/value batches share their producer block instead of
  // occupying one whole large-report slot per call. Sixteen observations
  // below fit the two owned blocks even before any consumer service.
  BatchTransport<uint64_t, 2> diagnostic_transport;
  assert(diagnostic_transport.configure(64, 8, 9));
  for (unsigned i = 0; i < 8; ++i)
    assert(diagnostic_transport.append(0, pair, pair.size() > 8, 9, identity, include));
  assert(!diagnostic_transport.append(0, pair, pair.size() > 8, 9, identity, include));
  unsigned diagnostic_records = 0;
  diagnostic_transport.drain([&](auto records, auto, auto) {
    diagnostic_records += records.size();
  });
  assert(diagnostic_records == 16);

  // Recycled blocks must carry fully initialized records under concurrent
  // publication, with no copies read from mutable producer scratch afterward.
  assert(transport.configure(16, 2, 8));
  std::atomic<bool> done{false};
  std::atomic<uint64_t> accepted{0};
  uint64_t consumed = 0;
  std::thread producer([&] {
    for (uint64_t i = 1; i <= 100000; ++i) {
      std::array<uint64_t, 2> scratch{i, i};
      if (transport.append(0, scratch, false, 8, identity, include)) ++accepted;
      scratch.fill(UINT64_MAX);
    }
    done.store(true, std::memory_order_release);
  });
  auto consume = [&](auto records, auto generation, auto) {
    assert(generation == 8 && records.size() == 2);
    assert(records[0] == records[1] && records[0] != UINT64_MAX);
    ++consumed;
  };
  while (!done.load(std::memory_order_acquire)) transport.drain(consume);
  producer.join();
  transport.close_partials();
  transport.drain(consume);
  assert(consumed == accepted.load());
  assert(!transport.append(0, pair, false, 7, identity, include));

  // Exercise the exact persistence helper used by the disk worker: bounded
  // capacity rejection, partial sink failure, and a sink blocked past close.
  unsigned writes = 0;
  assert(!dxmt9::tracing::persistBatch(std::span<const uint64_t>(pair), 3, 4,
      [&](const char*, size_t size) { ++writes; return static_cast<long>(size); }));
  assert(writes == 0);
  assert(!dxmt9::tracing::persistBatch(std::span<const uint64_t>(pair), 0, 4,
      [&](const char*, size_t) { return ++writes == 1 ? 1L : -1L; }));
  assert(writes == 2);
  std::promise<void> sink_entered, release_sink;
  auto released = release_sink.get_future();
  std::atomic<bool> worker_done{false};
  std::thread blocked_worker([&] {
    assert(dxmt9::tracing::persistBatch(std::span<const uint64_t>(pair), 0, 4,
        [&](const char*, size_t size) {
          sink_entered.set_value();
          released.wait();
          return static_cast<long>(size);
        }));
    worker_done.store(true, std::memory_order_release);
  });
  sink_entered.get_future().wait();
  using dxmt9::tracing::mayRearmDiagnostic;
  for (bool controller_done : {false, true}) {
    assert(!mayRearmDiagnostic(controller_done, worker_done.load(), 0));
  }
  release_sink.set_value();
  blocked_worker.join();
  assert(mayRearmDiagnostic(true, worker_done.load(std::memory_order_acquire), 0));
  assert(!mayRearmDiagnostic(true, true, 1));
  assert(!mayRearmDiagnostic(false, true, 0));

  std::mutex control;
  std::promise<void> control_held, release_control;
  auto control_released = release_control.get_future();
  std::thread lock_owner([&] {
    std::lock_guard lock(control);
    control_held.set_value();
    control_released.wait();
  });
  control_held.get_future().wait();
  std::unique_lock lock(control, std::defer_lock);
  assert(!dxmt9::tracing::lockDiagnosticBefore(lock,
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2)));
  assert(!lock.owns_lock());
  release_control.set_value();
  lock_owner.join();
  assert(dxmt9::tracing::lockDiagnosticBefore(lock,
      std::chrono::steady_clock::now() + std::chrono::seconds(1)));
  lock.unlock();

  for (uint8_t initial : {uint8_t{0}, uint8_t{1}, uint8_t{2}}) {
    for (bool durable : {false, true}) {
      for (bool sources : {false, true}) {
        for (uint64_t completed : {uint64_t{9}, uint64_t{10}, uint64_t{11}}) {
          std::atomic<uint8_t> outcome{initial};
          const bool expected = initial == 1 || (initial == 0 && durable && sources && completed <= 10);
          assert(dxmt9::tracing::decideDiagnosticCompletion(outcome, sources,
              durable, completed, 10) == expected);
        }
      }
    }
  }
  // An unregistered stop deadline from N cannot expire a rearmed N+1.
  std::atomic<uint64_t> tagged_outcome{uint64_t{8} << 2};
  uint64_t stale_pending = uint64_t{7} << 2;
  assert(!tagged_outcome.compare_exchange_strong(stale_pending, (uint64_t{7} << 2) | 2));
  assert(dxmt9::tracing::decideDiagnosticCompletion(tagged_outcome, true, true, 9, 10, 8));
  uint64_t late_pending = uint64_t{8} << 2;
  assert(!tagged_outcome.compare_exchange_strong(late_pending, (uint64_t{8} << 2) | 2));
  assert(tagged_outcome.load() == ((uint64_t{8} << 2) | 1));
  std::atomic<uint8_t> timedout{2};

  assert(!dxmt9::tracing::decideDiagnosticCompletion(timedout, true, true, 9, 10));

  std::atomic<uint32_t> cursor{0};



  uint32_t first = UINT32_MAX;
  assert(dxmt9::observations::reserve(cursor, 4, 4, first) && first == 0);
  assert(!dxmt9::observations::reserve(cursor, 4, 1, first));
  using namespace dxmt9::tracing;
  std::atomic<uint64_t> gate{makeAdmissionGate(7, 0, false)};
  assert(tryAcquireWriter(gate, TraceState::Collecting, 7));
  closeAdmission(gate);
  assert(!tryAcquireWriter(gate, TraceState::Collecting, 7));
  releaseWriter(gate);
  openAdmission(gate, 8);
  assert(!tryAcquireWriter(gate, TraceState::Collecting, 7));
}
