#pragma once
#include "dxmt9/query_fence.hpp"
#include "util/log/log.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>

namespace dxmt9::query::polldiag {
enum class Phase : unsigned { QueueWait, Classify, Marker, RequestSetup,
  PendingWait, Decision, Wake, Exit, Count };
enum class Request : unsigned { None, Rejected, ExistingTarget, Submitted, New, Count };
inline constexpr std::array phaseNames{"queue_wait", "classify", "marker",
  "request_setup", "pending_wait", "decision", "wake", "exit"};
inline constexpr std::array requestNames{"none", "rejected", "existing_target", "submitted", "new"};
inline unsigned mode() noexcept {
  static const unsigned value = [] {
    const auto* s = std::getenv("DXMT9_QUERY_BACKEND_OBSERVER");
    return !s || !s[0] || s[0] == '0' ? 0u : s[0] == 'c' ? 1u : 2u;
  }();
  return value;
}
inline std::uint64_t now() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Sample {
  bool timed = false, invalid = false, marker = false, coveredPending = false;
  Request request = Request::None;
  std::uint64_t last = 0;
  std::array<std::uint64_t, phaseNames.size()> ns{};
  void at(Phase phase, std::uint64_t t) noexcept {
    if (!timed) return;
    if (t < last) invalid = true;
    else ns[static_cast<unsigned>(phase)] += t - last;
    last = t;
  }
};
struct Totals {
  std::uint64_t calls = 0, flush = 0, samples = 0, invalid = 0, markers = 0;
  std::uint64_t coveredPending = 0;
  std::array<std::uint64_t, requestNames.size()> requests{};
  std::array<std::uint64_t, 4> results{};
  std::array<std::uint64_t, phaseNames.size()> ns{}, maximum{};
  void add(const Sample& sample, bool f, Poll result) noexcept {
    ++calls; flush += f; markers += sample.marker;
    coveredPending += sample.coveredPending;
    ++requests[static_cast<unsigned>(sample.request)];
    ++results[static_cast<unsigned>(result)];
    if (!sample.timed) return;
    if (sample.invalid) { ++invalid; return; }
    ++samples;
    for (unsigned i = 0; i < ns.size(); ++i) {
      ns[i] += sample.ns[i];
      if (sample.ns[i] > maximum[i]) maximum[i] = sample.ns[i];
    }
  }
};
inline thread_local Sample* active = nullptr;
template<bool Observe> inline void mark(Phase p) noexcept {
  if constexpr (Observe) { if (active->timed) active->at(p, now()); }
}
template<bool Observe> inline void request(Request r) noexcept {
  if constexpr (Observe) active->request = r;
}
template<bool Observe> inline void marker() noexcept {
  if constexpr (Observe) active->marker = true;
}
class Ledger {
 public:
  ~Ledger() { std::lock_guard lock(mutex_); emit(); }
  bool sample() { return ticket_.fetch_add(1, std::memory_order_relaxed) % 64 == 0; }
  void add(const Sample& sample, bool flush, Poll result) {
    std::lock_guard lock(mutex_);
    totals_.add(sample, flush, result);
    if (totals_.calls % 4096 == 0 && reports_ < 64) emit();
  }
 private:
  void emit() {
    ++reports_;
    const auto u = [](std::uint64_t x) { return static_cast<unsigned long long>(x); };
    util::logf(util::LogLevel::Info, "dxmt9-query-backend",
        "calls=%llu flush=%llu samples=%llu invalid_clocks=%llu markers=%llu reports=%u covered_pending=%llu slow=%llu",
        u(totals_.calls), u(totals_.flush), u(totals_.samples), u(totals_.invalid),
        u(totals_.markers), reports_, u(totals_.coveredPending),
        u(totals_.calls - totals_.coveredPending));
    for (unsigned i = 0; i < requestNames.size(); ++i)
      util::logf(util::LogLevel::Info, "dxmt9-query-backend-request",
          "kind=%s calls=%llu", requestNames[i], u(totals_.requests[i]));
    for (unsigned i = 0; i < totals_.results.size(); ++i)
      util::logf(util::LogLevel::Info, "dxmt9-query-backend-result",
          "kind=%u calls=%llu", i, u(totals_.results[i]));
    for (unsigned i = 0; i < phaseNames.size(); ++i)
      util::logf(util::LogLevel::Info, "dxmt9-query-backend-phase",
          "phase=%s ns=%llu max_ns=%llu", phaseNames[i], u(totals_.ns[i]), u(totals_.maximum[i]));
  }
  std::mutex mutex_;
  Totals totals_{};
  std::atomic<std::uint64_t> ticket_{0};
  unsigned reports_ = 0;
};
inline Ledger& ledger() {
  static const bool initialized = [] {
    util::logf(util::LogLevel::Info, "dxmt9-query-backend-init",
               "mode=%u stride=64 scope=backend-poll elapsed_not_cpu=1", mode());
    return true;
  }();
  (void)initialized;
  static Ledger value;
  return value;
}
template<typename Work> Poll observe(bool flush, Work&& work) {
  auto& sink = ledger();
  Sample sample;
  sample.timed = mode() == 2 && sink.sample();
  if (sample.timed) sample.last = now();
  auto* previous = active;
  active = &sample;
  struct Restore { Sample* previous; ~Restore() { active = previous; } } restore{previous};
  const auto result = work(); // Production poll is noexcept and owns failure handling.
  mark<true>(Phase::Exit); // After the runtime queue mutex has been released.
  sink.add(sample, flush, result);
  return result;
}
} // namespace dxmt9::query::polldiag
