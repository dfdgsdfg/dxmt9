#pragma once

#include "util/log/log.hpp"
#include "../dxmt9/observations/snapshot_sink.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>

namespace dxmt9::d3d9::query_receipt_diag {

enum class Phase : unsigned { Capture, ReplayWait, Snapshot, Backend, Final, Count };
enum class Route : unsigned {
  MissingTarget, NonRaw, ReplayFailed, Superseded, NotReady,
  RawMismatch, Ineligible, IdentityMismatch, Handled, Count
};
constexpr unsigned phaseCount = static_cast<unsigned>(Phase::Count);
constexpr unsigned routeCount = static_cast<unsigned>(Route::Count);
inline constexpr const char* phaseNames[] = {
  "capture", "replay_wait", "snapshot", "backend", "final"
};
inline constexpr const char* routeNames[] = {
  "missing_target", "non_raw", "replay_failed", "superseded", "not_ready",
  "raw_mismatch", "ineligible", "identity_mismatch", "handled"
};

struct Sample {
  Route route = Route::MissingTarget;
  unsigned kind = 5; // Unknown until an immutable snapshot is qualified.
  bool timed = false;
  bool invalidClock = false;
  std::array<std::uint64_t, phaseCount> ns{};
};
struct Totals {
  std::uint64_t calls = 0, flush = 0, samples = 0, invalidClocks = 0;
  std::array<std::uint64_t, routeCount> routes{};
  std::array<std::uint64_t, 6> kinds{};
  // S_OK / S_FALSE / error / fallback (no HRESULT from receipt path).
  std::array<std::uint64_t, 4> results{}, sampledResults{};
  std::array<std::uint64_t, phaseCount> ns{};
  std::array<std::array<std::uint64_t, phaseCount>, 4> resultNs{};
  void add(const Sample& s, std::uint32_t flags,
           std::optional<std::int32_t> result) noexcept {
    ++calls;
    flush += (flags & 1u) != 0;
    ++routes[static_cast<unsigned>(s.route)];
    ++kinds[s.kind < 5 ? s.kind : 5];
    const unsigned bucket = !result ? 3 : *result == 0 ? 0 : *result == 1 ? 1 : 2;
    ++results[bucket];
    if (!s.timed) return;
    if (s.invalidClock) { ++invalidClocks; return; }
    ++samples;
    ++sampledResults[bucket];
    for (unsigned p = 0; p < phaseCount; ++p) {
      ns[p] += s.ns[p];
      resultNs[bucket][p] += s.ns[p];
    }
  }
};
// Separate from the legacy observer: enabling it must not restore global drains.
// counts = counters only; 1 = counters plus one-in-64 timing samples.
inline unsigned mode() noexcept {
  static const unsigned value = [] {
    const auto* s = std::getenv("DXMT9_QUERY_RECEIPT_OBSERVER");
    return !s || !s[0] || s[0] == '0' ? 0u : s[0] == 'c' ? 1u : 2u;
  }();
  return value;
}
inline std::uint64_t nowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Cold value projection: totals already contain the original sampled timings.
inline void project(const Totals& t, unsigned observerMode) noexcept {
  if (!tracing::enabled()) return;
  observations::SnapshotSink sink(observations::Source::QueryReceipt,
      observerMode, observerMode == 2 ? 64 : 0);
  std::uint16_t index = 0;
  auto put = [&](std::uint64_t v) { sink.value(index++, v); };
  put(t.calls); put(t.flush); put(t.samples); put(t.invalidClocks);
  for (auto v : t.routes) put(v);
  for (auto v : t.kinds) put(v);
  for (auto v : t.results) put(v);
  for (auto v : t.sampledResults) put(v);
  for (auto v : t.ns) put(v);
  for (const auto& row : t.resultNs) for (auto v : row) put(v);
}
class Ledger {
 public:
  ~Ledger() { std::lock_guard lock(mutex_); emit(); }
  bool sample() noexcept { return ticket_.fetch_add(1, std::memory_order_relaxed) % 64 == 0; }
  void add(const Sample& s, std::uint32_t flags,
           std::optional<std::int32_t> result) noexcept {
    std::lock_guard lock(mutex_);
    totals_.add(s, flags, result);
    if (totals_.calls == 1) project(totals_, mode());
    if (totals_.calls % 4096 == 0 && reports_ < 64) emit();
  }
 private:
  void emit() noexcept {
    ++reports_;
    project(totals_, mode());
    const auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    util::logf(util::LogLevel::Info, "dxmt9-query-receipt",
        "calls=%llu flush=%llu samples=%llu invalid_clocks=%llu ok=%llu pending=%llu error=%llu fallback=%llu reports=%llu",
        u(totals_.calls), u(totals_.flush), u(totals_.samples), u(totals_.invalidClocks),
        u(totals_.results[0]), u(totals_.results[1]), u(totals_.results[2]),
        u(totals_.results[3]), u(reports_));
    for (unsigned r = 0; r < routeCount; ++r)
      util::logf(util::LogLevel::Info, "dxmt9-query-receipt-route",
          "route=%s calls=%llu", routeNames[r], u(totals_.routes[r]));
    for (unsigned k = 0; k < 6; ++k)
      util::logf(util::LogLevel::Info, "dxmt9-query-receipt-kind",
          "kind=%u calls=%llu", k, u(totals_.kinds[k]));
    for (unsigned b = 0; b < 4; ++b)
      for (unsigned p = 0; p < phaseCount; ++p)
        util::logf(util::LogLevel::Info, "dxmt9-query-receipt-phase",
            "bucket=%u phase=%s samples=%llu ns=%llu", b, phaseNames[p],
            u(totals_.sampledResults[b]), u(totals_.resultNs[b][p]));
  }
  std::mutex mutex_;
  Totals totals_{};
  std::atomic<std::uint64_t> ticket_{0};
  unsigned reports_ = 0;
};
inline Ledger& ledger() {
  static const bool initialized = [] {
    util::logf(util::LogLevel::Info, "dxmt9-query-receipt-init",
        "mode=%u timing_stride=64 scope=receipt-attempt elapsed_not_thread_cpu=1", mode());
    return true;
  }();
  (void)initialized;
  static Ledger value;
  return value;
}
struct Active {
  Sample sample;
  std::uint64_t last = 0;
  void markAt(Phase p, std::uint64_t current) noexcept {
    if (!sample.timed) return;
    if (current < last) sample.invalidClock = true;
    else sample.ns[static_cast<unsigned>(p)] += current - last;
    last = current;
  }
  void mark(Phase p) noexcept {
    if (sample.timed) markAt(p, nowNs());
  }
};
inline thread_local Active* active = nullptr;
template<bool Observe> inline void mark(Phase p) noexcept {
  if constexpr (Observe) active->mark(p);
}
template<bool Observe> inline void route(Route r) noexcept {
  if constexpr (Observe) active->sample.route = r;
}
template<bool Observe> inline void kind(unsigned k) noexcept {
  if constexpr (Observe) active->sample.kind = k;
}
// Only the enabled wrapper constructs state. Disabled template calls compile out.
template<typename Poll> auto observe(std::uint32_t flags, Poll&& poll) {
  auto& sink = ledger();
  Active current;
  current.sample.timed = mode() == 2 && sink.sample();
  if (current.sample.timed) current.last = nowNs();
  auto* previous = active;
  active = &current;
  struct Restore { Active* previous; ~Restore() { active = previous; } } restore{previous};
  try {
    auto result = poll();
    current.mark(Phase::Final);
    sink.add(current.sample, flags, result);
    return result;
  } catch (...) {
    current.mark(Phase::Final);
    sink.add(current.sample, flags, std::int32_t{-1});
    throw;
  }
}
} // namespace dxmt9::d3d9::query_receipt_diag
