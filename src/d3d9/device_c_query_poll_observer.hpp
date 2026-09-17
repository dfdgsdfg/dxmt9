#pragma once

#include "util/log/log.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>

namespace dxmt9::d3d9::query_poll_diag {

// Aggregate Unix shim intervals, NOT query/issue identities. No opaque pointer
// is treated as identity. The timeline observer separately owns qualified ENDs.
struct Totals {
  std::uint64_t calls = 0;
  std::uint64_t flush = 0;
  std::uint64_t drainFailures = 0;
  std::uint64_t invalidClocks = 0;
  std::uint64_t totalNs = 0;
  std::uint64_t drainNs = 0;
  std::uint64_t providerNs = 0;
  // S_OK, S_FALSE, all other HRESULTs. All three timing populations match.
  std::array<std::uint64_t, 3> results{};
  std::array<std::uint64_t, 3> resultDrainNs{};
  std::array<std::uint64_t, 3> resultProviderNs{};

  void add(bool drained, std::uint32_t flags, std::int32_t result,
           std::uint64_t begin, std::uint64_t drainEnd,
           std::uint64_t end) noexcept {
    ++calls;
    flush += (flags & 1u) != 0;
    drainFailures += !drained;
    const std::size_t bucket = result == 0 ? 0 : result == 1 ? 1 : 2;
    ++results[bucket];
    if (drainEnd < begin || end < drainEnd || (!drained && end != drainEnd)) {
      ++invalidClocks;
      return;
    }
    totalNs += end - begin;
    drainNs += drainEnd - begin;
    providerNs += end - drainEnd;
    resultDrainNs[bucket] += drainEnd - begin;
    resultProviderNs[bucket] += end - drainEnd;
  }
};

class Ledger final {
 public:
  explicit Ledger(bool logging = true) noexcept : logging_(logging) {}
  ~Ledger() { if (logging_) report(); }

  void add(bool drained, std::uint32_t flags, std::int32_t result,
           std::uint64_t begin, std::uint64_t drainEnd,
           std::uint64_t end) noexcept {
    std::lock_guard lock(mutex_);
    totals_.add(drained, flags, result, begin, drainEnd, end);
    if (logging_ && totals_.calls % 4096 == 0 && reports_ < 64) emit();
  }

  Totals snapshot() const noexcept {
    std::lock_guard lock(mutex_);
    return totals_;
  }

  void report() noexcept {
    std::lock_guard lock(mutex_);
    emit();
  }

 private:
  void emit() noexcept {
    ++reports_;
    const auto& t = totals_;
    util::logf(util::LogLevel::Info, "dxmt9-query-bridge-poll",
        "calls=%llu flush=%llu drain_failures=%llu invalid_clocks=%llu "
        "total_ns=%llu drain_ns=%llu provider_ns=%llu reports=%llu",
        static_cast<unsigned long long>(t.calls),
        static_cast<unsigned long long>(t.flush),
        static_cast<unsigned long long>(t.drainFailures),
        static_cast<unsigned long long>(t.invalidClocks),
        static_cast<unsigned long long>(t.totalNs),
        static_cast<unsigned long long>(t.drainNs),
        static_cast<unsigned long long>(t.providerNs),
        static_cast<unsigned long long>(reports_));
    for (std::size_t i = 0; i < t.results.size(); ++i) {
      util::logf(util::LogLevel::Info, "dxmt9-query-bridge-poll-result",
          "bucket=%u calls=%llu drain_ns=%llu provider_ns=%llu",
          static_cast<unsigned>(i),
          static_cast<unsigned long long>(t.results[i]),
          static_cast<unsigned long long>(t.resultDrainNs[i]),
          static_cast<unsigned long long>(t.resultProviderNs[i]));
    }
  }
  mutable std::mutex mutex_;
  Totals totals_{};
  bool logging_;
  std::uint64_t reports_ = 0;
};

inline bool enabled() noexcept {
  static const bool value = [] {
    const char* setting = std::getenv("DXMT9_QUERY_OBSERVER");
    return setting && setting[0] && setting[0] != '0';
  }();
  return value;
}

inline Ledger& ledger() {
  // Establish the logger before the final-report destructor is registered.
  static const bool initialized = [] {
    util::logf(util::LogLevel::Info, "dxmt9-query-bridge-poll-init",
               "enabled=1 scope=unix-shim aggregate=1");
    return true;
  }();
  (void)initialized;
  static Ledger value;
  return value;
}

inline std::uint64_t nowNs() noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<
      std::chrono::nanoseconds>(std::chrono::steady_clock::now()
          .time_since_epoch()).count());
}

// Called only behind the cached gate. The observer consumes timestamps after
// normal work; it has no authority to skip drains, submit, wait, or retry.
template <typename Drain, typename Provider, typename Clock>
std::int32_t observeCall(Ledger& destination, std::uint32_t flags,
                         std::int32_t drainFailure, Drain&& drain,
                         Provider&& provider, Clock&& clock) {
  const auto begin = clock();
  const bool drained = drain();
  const auto drainEnd = clock();
  if (!drained) {
    destination.add(false, flags, drainFailure, begin, drainEnd, drainEnd);
    return drainFailure;
  }
  const auto result = provider();
  const auto end = clock();
  destination.add(true, flags, result, begin, drainEnd, end);
  return result;
}

}  // namespace dxmt9::d3d9::query_poll_diag
