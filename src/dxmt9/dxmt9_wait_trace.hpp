#pragma once

#include <cstdint>
#include <cstdlib>
#include <mutex>

namespace dxmt9::waittrace {

inline bool enabled() noexcept {
  static const bool value = [] {
    const auto* v = std::getenv("DXMT9_WAIT_TRACE");
    return v && v[0] && v[0] != '0';
  }();
  return value;
}

enum class Site : std::uint32_t { Receipt, Resource, BufferLock, BufferUnlock,
                                  QueryQueue };
struct Context {
  std::uint64_t query = 0;
  Site site = Site::Resource;
  std::uint64_t owner = 0, raw = 0, target = 0;
};
// Construct only behind enabled(); never part of a production return type.
class ContextScope {
 public:
  explicit ContextScope(Context value) noexcept;
  ~ContextScope();
 private:
  Context previous_;
};
struct Snapshot {
  std::uint64_t raw = 0, queued = 0, completed = 0;
  std::uint32_t state = 0;
};
struct Row {
  std::uint64_t owner = 0, target = 0, query = 0, queue = 0, sequence = 0;
  std::uint64_t initializer = 0, requestedQueue = 0;
  std::uint64_t begin = 0, acquired = 0, end = 0, tid = 0;
  std::uint64_t firstFrame = 0, lastFrame = 0, waits = 0;
  Snapshot before{}, after{};
  std::uint64_t traceGeneration = 0;
  Site site = Site::Resource;
};

void registerOwner(const void* owner) noexcept;
std::uint64_t ownerIdentity(const void* owner) noexcept;
std::uint64_t now() noexcept;
bool inWindow() noexcept;
Row begin(Site site, const void* owner, const void* target) noexcept;
void finish(Row& row) noexcept;
void present(std::uint64_t ordinal) noexcept;
void bindReceipt(const void* owner, const void* target, std::uint64_t raw) noexcept;
void enteredWait() noexcept;
std::uint64_t waitEntries() noexcept;
std::unique_lock<std::mutex> queryLock(std::mutex& mutex,
    std::uint64_t queue, std::uint64_t sequence, std::uint64_t initializer,
    std::uint64_t requestedQueue);

// Caller enters only behind enabled() && inWindow(). All snapshots and
// predicate evaluations retain the production mutex and predicate semantics.
// CV duration includes mutex reacquisition and scheduling, not just sleep.
template<typename CV, typename Capture, typename Pending>
[[gnu::noinline]]
auto observeWait(std::mutex& mutex, CV& cv, const void* owner,
                 const void* target, Site site, Capture&& capture,
                 Pending&& pending) noexcept {
  auto row = begin(site, owner, target);
  std::unique_lock lock(mutex);
  row.acquired = now();
  auto value = capture();
  row.before = value.second;
  while (pending(value.first)) {
    ++row.waits;
    enteredWait();
    cv.wait(lock);
    value = capture();
  }
  value = capture(); // Preserve the production post-wait terminal recheck.
  row.after = value.second;
  row.end = now();
  lock.unlock();
  finish(row);
  return value.first;
}

} // namespace dxmt9::waittrace
