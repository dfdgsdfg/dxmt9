#include "dxmt9_trace_bindings.hpp"

#include <array>
#include <atomic>
#include <mutex>

namespace dxmt9::tracing {
namespace {
struct Owner {
  std::atomic<const void*> address{nullptr};
  std::atomic<uint64_t> token{0};
  std::atomic<uint64_t> generation{0};
};
std::array<Owner, 256> owners;
std::atomic<uint64_t> nextOwner{1};
std::atomic<uint64_t> nextObservation{1};
std::mutex ownerMutex;
}

void register_owner(const void* owner) noexcept {
  if (!owner) return;
  std::lock_guard lock(ownerMutex);
  const uint64_t generation =
      g_trace_owner_registration_generation.load(std::memory_order_acquire);
  if (!generation) return;
  for (auto& entry : owners) {
    if (entry.address.load(std::memory_order_relaxed) == owner) return;
  }
  for (auto& entry : owners) {
    // Tokens mark slots as permanently consumed. Clearing an address at
    // destruction never permits an old owner token or slot to be reused.
    if (entry.token.load(std::memory_order_relaxed) != 0) continue;
    entry.token.store(nextOwner.fetch_add(1, std::memory_order_relaxed),
                      std::memory_order_relaxed);
    entry.generation.store(generation, std::memory_order_relaxed);
    entry.address.store(owner, std::memory_order_release);
    return;
  }
  record_loss(1);
}

void enable_owner_registration(uint64_t generation) noexcept {
  std::lock_guard lock(ownerMutex);
  g_trace_owner_registration_generation.store(generation,
                                               std::memory_order_release);
}

void disable_owner_registration() noexcept {
  std::lock_guard lock(ownerMutex);
  g_trace_owner_registration_generation.store(0, std::memory_order_release);
}

void unregister_owner(const void* owner) noexcept {
  bool last = false;
  uint64_t ownerGeneration = 0;
  {
    std::lock_guard lock(ownerMutex);
    bool removed = false;
    for (auto& entry : owners) {
      if (entry.address.load(std::memory_order_acquire) == owner) {
        ownerGeneration = entry.generation.load(std::memory_order_relaxed);
        entry.address.store(nullptr, std::memory_order_release);
        removed = true;
      }
    }
    last = removed;
    for (auto& entry : owners)
      if (entry.address.load(std::memory_order_relaxed)) last = false;
    if (last) {
      uint64_t expected =
          g_trace_owner_registration_generation.load(std::memory_order_acquire);
      if (expected == ownerGeneration)
        (void)g_trace_owner_registration_generation.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
  }
  // Called after the device's normal replay/queue teardown, outside its locks.
  // Short workloads close before the duration controller's deadline.
  if (last) (void)stop_and_finalize(ownerGeneration);
}

uint64_t next_observation_id() noexcept {
  return nextObservation.fetch_add(1, std::memory_order_relaxed);
}

uint64_t owner_id(const void* owner) noexcept {
  for (const auto& entry : owners) {
    if (entry.address.load(std::memory_order_acquire) == owner)
      return entry.token.load(std::memory_order_relaxed);
  }
  return 0; // Explicitly unknown; consumers must reject owner-dependent joins.
}

void observe(Category category, EventId event, uint64_t owner, uint64_t work,
             uint64_t related, Phase phase, uint64_t related_owner) noexcept {
  (void)emit({.timestamp_ns = now_ns(), .work_id = work,
              .arg0 = owner, .arg1 = related,
              .related_owner_id = related_owner,
              .event_id = static_cast<uint16_t>(event),
              .category = static_cast<uint8_t>(category),
              .phase = static_cast<uint8_t>(phase)});
}
} // namespace dxmt9::tracing
