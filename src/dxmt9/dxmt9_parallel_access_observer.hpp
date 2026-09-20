#pragma once

#include "dxmt9_cpu_supply_trace.hpp"
#include "dxmt9_source_semantics.hpp"
#include <algorithm>
#include <array>
#include <cstdint>

namespace dxmt9::encoders {

// Diagnostic-owned exact dictionary. Never a producer result, Slot field, or
// execution authority. Hash collisions compare full identity values; exhaustion
// makes cardinality a lower bound rather than silently accepting truncation.
struct ParallelAccessObserver {
  static constexpr std::uint32_t kCapacity = 4096;
  static constexpr std::uint32_t kBuckets = 8192;
  std::array<std::uint64_t, kBuckets> identities{};
  std::array<std::uint32_t, kBuckets> uses{};
  core::ExactResourceSet previous{};
  const void* owner = nullptr;
  std::uint64_t sequence = 0;
  std::uint64_t touches = 0;
  std::uint32_t begin = 0, distinct = 0, commands = 0;
  std::uint32_t maximumUses = 0, adjacentReuse = 0;
  bool exhausted = false, incomplete = false;

  void start(std::uint32_t ordinal) noexcept {
    identities.fill(0);
    uses.fill(0);
    previous = {};
    touches = 0;
    begin = ordinal;
    distinct = commands = maximumUses = adjacentReuse = 0;
    exhausted = incomplete = false;
  }
  void append(const core::ExactResourceSet& reads, bool valid) noexcept {
    ++commands;
    if (!valid || !reads.complete() || !reads.canonicalized() ||
        reads.count > reads.handles.size()) {
      incomplete = true;
      previous = {};
      return;
    }
    for (std::uint32_t i = 0; i < reads.count; ++i) {
      const auto identity = reads.handles[i];
      if (!identity) { incomplete = true; continue; }
      ++touches;
      for (std::uint32_t j = 0; j < previous.count; ++j)
        if (previous.handles[j] == identity) {
          if (adjacentReuse == UINT32_MAX) incomplete = true;
          else ++adjacentReuse;
          break;
        }
      // Mix high generation/type bits and low resource indices alike.
      auto hash = identity ^ (identity >> 33);
      hash *= 0xff51afd7ed558ccdULL;
      hash ^= hash >> 33;
      auto bucket = static_cast<std::uint32_t>(hash) & (kBuckets - 1);
      while (identities[bucket] && identities[bucket] != identity)
        bucket = (bucket + 1) & (kBuckets - 1);
      if (!identities[bucket]) {
        if (distinct == kCapacity) { exhausted = true; continue; }
        identities[bucket] = identity;
        ++distinct;
      }
      maximumUses = std::max(maximumUses, ++uses[bucket]);
    }
    previous = reads;
  }
  void seal(std::uint32_t end, bool productionOverflow, bool rejected,
            bool boundaryComplete) const noexcept {
    if (!owner) return;
    using E = supply::Event;
    const auto emit = [&](E event, std::uint64_t value) {
      supply::record(event, owner, sequence, value);
    };
    emit(E::ParallelAccessRange, (std::uint64_t(begin) << 32) | end);
    emit(E::ParallelAccessCounts, (std::uint64_t(distinct) << 32) | commands);
    emit(E::ParallelAccessTouches, touches);
    emit(E::ParallelAccessReuse, (std::uint64_t(maximumUses) << 32) | adjacentReuse);
    emit(E::ParallelAccessFlags, (!exhausted && !incomplete ? 1u : 0u) |
        (exhausted ? 2u : 0u) | (incomplete ? 4u : 0u) |
        (productionOverflow ? 8u : 0u) | (rejected ? 16u : 0u) |
        (boundaryComplete ? 32u : 0u));
  }
};

inline thread_local ParallelAccessObserver* activeParallelAccessObserver = nullptr;

class ScopedParallelAccessObserver {
 public:
  explicit ScopedParallelAccessObserver(ParallelAccessObserver& observer) noexcept
      : previous_(activeParallelAccessObserver) {
    activeParallelAccessObserver = &observer;
  }
  ~ScopedParallelAccessObserver() { activeParallelAccessObserver = previous_; }
  ScopedParallelAccessObserver(const ScopedParallelAccessObserver&) = delete;
  ScopedParallelAccessObserver& operator=(const ScopedParallelAccessObserver&) = delete;
 private:
  ParallelAccessObserver* previous_;
};

} // namespace dxmt9::encoders
