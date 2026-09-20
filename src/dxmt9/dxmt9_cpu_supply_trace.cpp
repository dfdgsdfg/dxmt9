#include "dxmt9_cpu_supply_trace.hpp"

#include <atomic>
#include <cstdio>
#include <cstddef>
#include <memory>
#include <new>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#endif

namespace dxmt9::supply {
namespace {
constexpr std::size_t kCapacity = 262144;
struct Row {
  std::atomic<bool> ready{false};
  std::uint64_t wall = 0, cpu = 0, tid = 0, owner = 0, id = 0, value = 0;
  std::uint64_t frame = 0;
  Event event{};
};

std::uint64_t clockNs(clockid_t clock) noexcept {
  timespec t{};
  if (clock_gettime(clock, &t) != 0) return 0;
  return static_cast<std::uint64_t>(t.tv_sec) * 1000000000ull + t.tv_nsec;
}

std::uint64_t nativeCpuNs(std::uint64_t tid) noexcept {
#if defined(__APPLE__)
  // The Rosetta POSIX thread clock is not accepted as a nanosecond authority.
  // Use the same native-ID/libproc surface independently calibrated by
  // dxmt9-thread-cpu against THREAD_BASIC_INFO, including kernel CPU time.
  proc_threadinfo info{};
  static const auto pid = getpid();
  if (proc_pidinfo(pid, PROC_PIDTHREADID64INFO, tid, &info, sizeof(info)) !=
      sizeof(info)) return 0;
  return info.pth_user_time + info.pth_system_time;
#else
  return 0;
#endif
}

std::uint64_t setting(const char* name, std::uint64_t fallback) noexcept {
  const char* v = std::getenv(name);
  if (!v || !*v) return fallback;
  char* end = nullptr;
  const auto n = std::strtoull(v, &end, 10);
  return end && !*end && n > 0 && n < 1000000 ? n : fallback;
}

struct Sink {
  std::unique_ptr<Row[]> rows;
  std::atomic<std::uint64_t> next{0}, frame{0};
  std::atomic<std::uint64_t> writers{0};
  std::atomic<bool> finished{false};
  std::uint64_t first = setting("DXMT9_CPU_SUPPLY_TRACE_FIRST_PRESENT", 300);
  std::uint64_t last = setting("DXMT9_CPU_SUPPLY_TRACE_LAST_PRESENT", 600);
  FILE* file = nullptr;

  Sink() noexcept {
    const char* path = std::getenv("DXMT9_CPU_SUPPLY_TRACE");
    if (!path || last <= first) return;
    // Never overwrite another run's evidence. Opening/allocation precedes
    // the warm-up window; failure produces no apparently valid transcript.
    file = std::fopen(path, "wx");
    if (file) rows.reset(new (std::nothrow) Row[kCapacity]);
  }
  void finish(bool closed) noexcept {
    if (finished.exchange(true) || !file) return;
    std::uint64_t incomplete = writers.load();
    const auto count = next.load();
    for (std::size_t i = 0; i < count && i < kCapacity; ++i) {
      if (!rows[i].ready.load(std::memory_order_acquire)) ++incomplete;
    }
    std::fprintf(file, "# cpu-supply-v2 first=%llu last=%llu closed=%u overflow=%llu incomplete=%llu cpu_clock=1 pid=%d\n",
        first, last, closed ? 1u : 0u,
        count > kCapacity ? count - kCapacity : 0ull, incomplete, getpid());
    std::fprintf(file, "wall_ns,cpu_ns,tid,owner,id,value,frame,event\n");
    for (std::size_t i = 0; i < count && i < kCapacity; ++i) {
      const auto& r = rows[i];
      if (!r.ready.load(std::memory_order_acquire)) continue;
      std::fprintf(file, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u\n",
          r.wall, r.cpu, r.tid, r.owner, r.id, r.value, r.frame,
          static_cast<unsigned>(r.event));
    }
    if (std::fclose(file) != 0) {
      // The parser also requires complete endpoint/event coverage; a failed
      // close must not silently be called a qualified run.
      std::fprintf(stderr, "[cpu-supply] trace close failed\n");
    }
    file = nullptr;
  }
};

Sink& sink() noexcept {
  // Process-lifetime diagnostic: the explicit bounded-window close exports it.
  // Do not race live Wine workers through C++ static destruction at exit.
  alignas(Sink) static std::byte storage[sizeof(Sink)];
  static Sink* instance = new (storage) Sink;
  return *instance;
}
}  // namespace

void record(Event event, const void* owner, std::uint64_t id,
            std::uint64_t value) noexcept {
  auto& s = sink();
  const auto frame = s.frame.load(std::memory_order_relaxed);
  if (!s.rows || frame < s.first || frame > s.last) return;
  s.writers.fetch_add(1);
  if (s.finished.load()) {
    s.writers.fetch_sub(1);
    return;
  }
  const auto index = s.next.fetch_add(1, std::memory_order_relaxed);
  if (index >= kCapacity) {
    s.writers.fetch_sub(1);
    return;
  }
  auto& r = s.rows[index];
  r.wall = clockNs(CLOCK_MONOTONIC);
  static thread_local const std::uint64_t tid = [] {
    std::uint64_t result = 0;
#if defined(__APPLE__)
    pthread_threadid_np(nullptr, &result);
#endif
    return result;
  }();
  r.tid = tid;
  r.cpu = nativeCpuNs(tid);
  r.owner = reinterpret_cast<std::uintptr_t>(owner);
  r.id = id;
  r.value = value;
  r.frame = frame;
  r.event = event;
  r.ready.store(true, std::memory_order_release);
  s.writers.fetch_sub(1);
}

void present(std::uint64_t ordinal) noexcept {
  auto& s = sink();
  s.frame.store(ordinal, std::memory_order_relaxed);
  record(Event::Present, nullptr, ordinal);
  // Export outside the measured interval. Readers acquire each row's ready
  // flag; an in-flight writer is counted as incomplete, never raced or waited.
  if (ordinal > s.last + 1) s.finish(true);
}
}  // namespace dxmt9::supply
