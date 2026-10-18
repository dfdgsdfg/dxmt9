#include "dxmt9_wait_trace.hpp"
#include "observations/bounded_records.hpp"
#include "observations/trace_sink.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <new>
#include <pthread.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace dxmt9::waittrace {
namespace {
constexpr std::size_t capacity = 32768, ownerCapacity = 256;
thread_local Context context;

std::uint64_t setting(const char* key, std::uint64_t fallback) noexcept {
  const auto* v = std::getenv(key);
  if (!v || !*v || *v == '-') return fallback;
  char* end = nullptr;
  const auto n = std::strtoull(v, &end, 10);
  return end && !*end && n && n < 1000000 ? n : fallback;
}
struct Sink {
  std::array<std::atomic<const void*>, ownerCapacity> owners{};
  std::atomic<std::uint64_t> ownerCount{0}, frame{0}, active{0};
  std::atomic<std::uint64_t> unknown{0};
  std::atomic<std::uint64_t> waits{0};
  std::atomic<std::uint64_t> traceAttempted{0}, traceAccepted{0};
  std::atomic<bool> closed{false};
  observations::BoundedRecords<Row, capacity> entries;
  const std::uint64_t first = setting("DXMT9_WAIT_TRACE_FIRST_PRESENT", 1750);
  const std::uint64_t last = setting("DXMT9_WAIT_TRACE_LAST_PRESENT", 2100);
  const std::uint64_t mutexMinNs =
      setting("DXMT9_WAIT_TRACE_MUTEX_MIN_US", 50) * 1000;
  FILE* file = nullptr;
  Sink() noexcept {
    const auto* path = std::getenv("DXMT9_WAIT_TRACE");
    if (!path || last < first) return;
    file = std::fopen(path, "wx");
    if (file) entries.allocate();
    if (!file || !entries)
      std::fprintf(stderr, "[dxmt9-wait-trace] output unavailable\n");
  }
  void close() noexcept {
    if (closed.exchange(true) || !file) return;
    std::uint64_t incomplete = active.load();
    // Read active before next: a writer that passed the close gate either
    // remains counted, or completed its reservation before this count load.
    // The opposite order can silently omit a finishing mutex-only row.
    const auto count = entries.size();
    for (std::size_t i = 0; entries && i < count && i < capacity; ++i)
      incomplete += !entries.read(i);
    std::fprintf(file,
        "# wait-trace-v1 pid=%d first=%llu last=%llu closed=1 overflow=%llu incomplete=%llu unknown=%llu owners=%llu capacity=%zu mutex_min_ns=%llu storage=%u cv_entries=%llu acquisition_version=1 trace_attempted=%llu trace_accepted=%llu\n",
        getpid(), first, last, count > capacity ? count - capacity : 0ull,
        incomplete, unknown.load(), ownerCount.load(), capacity, mutexMinNs,
        entries ? 1u : 0u, waits.load(), traceAttempted.load(), traceAccepted.load());
    std::fprintf(file, "site,owner,target,query,queue,sequence,initializer,tid,first_frame,last_frame,begin_ns,acquired_ns,end_ns,waits,raw_before,queued_before,completed_before,state_before,raw_after,queued_after,completed_after,state_after,requested_queue\n");
    for (std::size_t i = 0; entries && i < count && i < capacity; ++i) {
      if (!entries.read(i)) continue;
      const auto& r = *entries.read(i);
      std::fprintf(file, "%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%llu,%llu,%llu,%u,%llu\n",
          static_cast<unsigned>(r.site), r.owner, r.target, r.query,
          r.queue, r.sequence, r.initializer, r.tid, r.firstFrame, r.lastFrame, r.begin,
          r.acquired, r.end, r.waits, r.before.raw, r.before.queued,
          r.before.completed, r.before.state, r.after.raw, r.after.queued,
          r.after.completed, r.after.state, r.requestedQueue);
    }
    std::fprintf(file, "# end rows=%llu\n", count < capacity ? count : capacity);
    if (std::fclose(file))
      std::fprintf(stderr, "[dxmt9-wait-trace] close failed\n");
    file = nullptr;
  }
};
Sink& sink() noexcept {
  // Intentionally process-lifetime; no static teardown races with Wine workers.
  alignas(Sink) static std::byte storage[sizeof(Sink)];
  static auto* value = new (storage) Sink;
  return *value;
}
} // namespace

ContextScope::ContextScope(Context value) noexcept : previous_(context) {
  context = value;
}
ContextScope::~ContextScope() { context = previous_; }

void registerOwner(const void* owner) noexcept {
  auto& s = sink();
  const auto i = s.ownerCount.fetch_add(1);
  if (i < ownerCapacity) s.owners[i].store(owner, std::memory_order_release);
}
std::uint64_t ownerIdentity(const void* owner) noexcept {
  auto& s = sink();
  const auto count = s.ownerCount.load();
  // A reused address is a new incarnation; capacity failure is unqualified.
  if (count > ownerCapacity) return 0;
  for (auto i = count; i > 0; --i)
    if (s.owners[i - 1].load(std::memory_order_acquire) == owner) return i;
  return 0;
}
std::uint64_t now() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool inWindow() noexcept {
  auto& s = sink();
  const auto frame = s.frame.load(std::memory_order_relaxed);
  return s.entries && !s.closed.load() && frame >= s.first && frame <= s.last;
}
Row begin(Site site, const void* owner, const void* target) noexcept {
  auto& s = sink();
  s.active.fetch_add(1);
  Row r;
  r.traceGeneration = tracing::enabled() ? tracing::capture_generation() : 0;
  r.site = site == Site::Resource ? context.site : site;
  r.owner = owner ? ownerIdentity(owner) : context.owner;
  r.target = target ? reinterpret_cast<std::uintptr_t>(target) : context.target;
  r.query = context.query;
  r.before.raw = r.after.raw = context.raw;
  r.firstFrame = s.frame.load();
#if defined(__APPLE__)
  pthread_threadid_np(nullptr, &r.tid);
#elif defined(__linux__)
  r.tid = static_cast<std::uint64_t>(syscall(SYS_gettid));
#endif
  r.beginPoint = observations::endpoint(r.traceGeneration, now);
  r.begin = r.beginPoint.source;
  return r;
}
void finish(Row& row) noexcept {
  auto& s = sink();
  row.lastFrame = s.frame.load();
  if (!s.closed.load() && s.entries &&
      (row.waits || row.acquired - row.begin >= s.mutexMinNs)) {
    if ((!row.owner && !row.queue) || !row.tid) s.unknown.fetch_add(1);
    const auto i = s.entries.reserve();
    if (i < capacity) {
      s.entries.writable(i) = row;
      s.entries.publish(i);
      if (row.traceGeneration) {
        observations::DiagnosticContext c;
        c.generation = row.traceGeneration; c.record = i + 1; c.work = row.sequence;
        const bool qualified = row.beginPoint.qualified() && row.acquiredPoint.qualified() &&
            row.endPoint.qualified() && row.begin == row.beginPoint.source &&
            row.acquired == row.acquiredPoint.source && row.end == row.endPoint.source &&
            row.begin <= row.acquired && row.acquired <= row.end &&
            row.beginPoint.after <= row.acquiredPoint.before &&
            (row.acquiredPoint.after <= row.endPoint.before ||
             row.acquiredPoint.source == row.endPoint.source);
        char details[2048]{};
        const auto written = std::snprintf(details, sizeof(details),
            "owner=%llu target=%llu query=%llu queue=%llu sequence=%llu initializer=%llu requested_queue=%llu begin=%llu acquired=%llu end=%llu tid=%llu first_frame=%llu last_frame=%llu waits=%llu site=%u before_raw=%llu before_queued=%llu before_completed=%llu before_state=%u after_raw=%llu after_queued=%llu after_completed=%llu after_state=%u qualified=%u begin_native_before=%llu begin_native_after=%llu acquired_native_before=%llu acquired_native_after=%llu end_native_before=%llu end_native_after=%llu resolution_ns=%llu source_clock=steady_clock",
            row.owner, row.target, row.query, row.queue, row.sequence, row.initializer,
            row.requestedQueue, row.begin, row.acquired, row.end, row.tid,
            row.firstFrame, row.lastFrame, row.waits, static_cast<unsigned>(row.site),
            row.before.raw, row.before.queued, row.before.completed, row.before.state,
            row.after.raw, row.after.queued, row.after.completed, row.after.state, qualified ? 1u : 0u,
            row.beginPoint.before, row.beginPoint.after, row.acquiredPoint.before, row.acquiredPoint.after,
            row.endPoint.before, row.endPoint.after, row.beginPoint.resolution);
        observations::report(observations::Source::WaitDetail, "wait-row-v1", details, c,
            written < 0 || static_cast<size_t>(written) >= sizeof(details));
        if (qualified) {
          using tracing::Phase;
          const auto mutexScope = (i + 1) * 2 + (1ull << 62);
          const auto bodyScope = mutexScope + 1;
          auto a = observations::endpoint_event(observations::Source::WaitDetail, c, row.beginPoint,
              "wait.mutex", Phase::Begin, mutexScope);
          auto b = observations::endpoint_event(observations::Source::WaitDetail, c, row.acquiredPoint,
              "wait.mutex", Phase::End, mutexScope);
          auto d = observations::endpoint_event(observations::Source::WaitDetail, c, row.acquiredPoint,
              "wait.body", Phase::Begin, bodyScope);
          auto e = observations::endpoint_event(observations::Source::WaitDetail, c, row.endPoint,
              "wait.body", Phase::End, bodyScope);
          const tracing::TraceEvent endpoints[] = {a, b, d, e};
          tracing::emit_observations(endpoints, c.generation);
        }
      }
      if (row.traceGeneration && row.begin <= row.acquired && row.acquired <= row.end) {
        const auto observed = tracing::now_ns();
        s.traceAttempted.fetch_add(2, std::memory_order_relaxed);
        if (observations::counter(row.traceGeneration, observations::Source::Wait, i,
              observed, tracing::Metric::WaitMutexAcquireElapsedNs, row.acquired - row.begin))
          s.traceAccepted.fetch_add(1, std::memory_order_relaxed);
        if (observations::counter(row.traceGeneration, observations::Source::Wait, i,
              observed, tracing::Metric::WaitBodyElapsedNs, row.end - row.acquired))
          s.traceAccepted.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  s.active.fetch_sub(1);
}
void present(std::uint64_t ordinal) noexcept {
  auto& s = sink();
  s.frame.store(ordinal);
  if (ordinal > s.last + 1) s.close();
}

void bindReceipt(const void* owner, const void* target, std::uint64_t raw) noexcept {
  if (!context.query) return;
  context.owner = ownerIdentity(owner);
  context.target = reinterpret_cast<std::uintptr_t>(target);
  context.raw = raw;
}
void enteredWait() noexcept { sink().waits.fetch_add(1); }
std::uint64_t waitEntries() noexcept { return sink().waits.load(); }

std::unique_lock<std::mutex> queryLock(std::mutex& mutex,
    std::uint64_t queue, std::uint64_t sequence, std::uint64_t initializer,
    std::uint64_t requestedQueue) {
  auto row = begin(Site::QueryQueue, nullptr, nullptr);
  row.queue = queue;
  row.sequence = sequence;
  row.initializer = initializer;
  row.requestedQueue = requestedQueue;
  std::unique_lock lock(mutex);
  row.acquiredPoint = row.endPoint = observations::endpoint(row.traceGeneration, now);
  row.acquired = row.end = row.acquiredPoint.source;
  // Bounded row storage only, no output or sink lock under the runtime mutex.
  finish(row);
  return lock;
}
} // namespace dxmt9::waittrace
