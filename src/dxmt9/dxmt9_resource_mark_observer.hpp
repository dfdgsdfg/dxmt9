#pragma once

#include "dxmt9/core_constants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dxmt9::resources {

// Cold, observation-only distinction between the two residency walks. The
// ingress walk sees the producer's resolved handle table; the publish walk
// sees the final slot's effective command stream. Neither walk changes its
// resource policy when this ledger is enabled.
enum class ResourceMarkObservationPhase : std::uint8_t {
  Ingress,
  PublishScan,
};

struct ResourceMarkObservationKey {
  core::ChunkHandleKind kind = core::ChunkHandleKind::Texture;
  std::uint64_t handle = 0;
  // Buffer snapshot stamping targets one concrete rename-ring backing rather
  // than only the logical buffer record. Zero denotes a logical/active-backing
  // mark. Keeping the domains distinct prevents a generic ingress mark from
  // being treated as proof that a later publish stamped the required captured
  // backing.
  std::uint64_t bufferBacking = 0;

  friend constexpr bool operator==(const ResourceMarkObservationKey&,
                                   const ResourceMarkObservationKey&) =
      default;
};

constexpr std::uint64_t mixResourceMarkIdentity(std::uint64_t value) noexcept {
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27;
  value *= 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

constexpr std::size_t hashResourceMarkIdentity(
    ResourceMarkObservationKey key) noexcept {
  return static_cast<std::size_t>(
      mixResourceMarkIdentity(
          key.handle ^
          mixResourceMarkIdentity(key.bufferBacking +
                                  0x9e3779b97f4a7c15ULL) ^
          (static_cast<std::uint64_t>(key.kind) << 56)));
}

struct ResourceMarkObservation {
  bool recorded = false;
  bool newIdentity = false;
  bool samePhaseDuplicate = false;
  bool covered = false;
  bool stale = false;
  bool noIngress = false;
  bool overflow = false;
  std::uint32_t collisionProbes = 0;
};

struct ResourceMarkOverlapSnapshot {
  std::uint64_t ingressEntries = 0;
  std::uint64_t publishEntries = 0;
  std::uint64_t uniqueIdentities = 0;
  std::uint64_t ingressDuplicates = 0;
  std::uint64_t publishDuplicates = 0;
  std::uint64_t publishCovered = 0;
  std::uint64_t publishStale = 0;
  std::uint64_t publishNoIngress = 0;
  std::uint64_t collisionProbes = 0;
  std::uint64_t overflow = 0;
};

// Fixed-capacity open-addressed ledger. The safety question is resource
// coverage at a publish sequence, not retention of every source/slot pair:
// each (kind, handle, concrete-buffer-backing-or-zero) stores the greatest
// ingress sequence and the most recent sequence seen by each phase. A publish
// is covered only when the same exact identity has an ingress waterline at or
// above publishSeq. No entry is evicted, and a full table reports overflow
// instead of claiming coverage from an approximation.
class ResourceMarkOverlapLedger {
 public:
  // 65,536 entries keeps the enabled diagnostic useful for a long run while
  // remaining bounded. The object is constructed only behind the opt-in gate
  // in dxmt9_command_queue.cpp.
  static constexpr std::size_t kCapacity = 65536;

  static constexpr std::size_t hashIndexForTest(
      ResourceMarkObservationKey key) noexcept {
    return hashResourceMarkIdentity(key) & (kCapacity - 1);
  }

  ResourceMarkObservation observe(ResourceMarkObservationPhase phase,
                                  ResourceMarkObservationKey key,
                                  std::uint64_t sequence) noexcept {
    if (key.handle == 0) {
      return {};
    }
    std::lock_guard lock(mutex_);
    const std::size_t start = hashIndexForTest(key);
    for (std::size_t probe = 0; probe < kCapacity; ++probe) {
      auto& entry = entries_[(start + probe) & (kCapacity - 1)];
      if (!entry.occupied) {
        entry = Entry{.key = key, .occupied = true};
        const auto collisionProbes = static_cast<std::uint32_t>(probe);
        ++snapshot_.uniqueIdentities;
        incrementPhaseCount(phase);
        snapshot_.collisionProbes += collisionProbes;
        if (phase == ResourceMarkObservationPhase::Ingress) {
          entry.hasIngress = true;
          entry.maxIngressSeq = sequence;
          entry.lastIngressSeq = sequence;
        } else {
          entry.hasPublish = true;
          entry.lastPublishSeq = sequence;
          classifyPublish(entry, sequence);
        }
        return ResourceMarkObservation{
            .recorded = true,
            .newIdentity = true,
            .covered = phase == ResourceMarkObservationPhase::PublishScan &&
                       entry.hasIngress && entry.maxIngressSeq >= sequence,
            .stale = phase == ResourceMarkObservationPhase::PublishScan &&
                     entry.hasIngress && entry.maxIngressSeq < sequence,
            .noIngress = phase == ResourceMarkObservationPhase::PublishScan &&
                         !entry.hasIngress,
            .collisionProbes = collisionProbes};
      }
      if (!(entry.key == key)) {
        continue;
      }
      const auto collisionProbes = static_cast<std::uint32_t>(probe);
      ResourceMarkObservation result{.recorded = true,
                                     .collisionProbes = collisionProbes};
      snapshot_.collisionProbes += collisionProbes;
      if (phase == ResourceMarkObservationPhase::Ingress) {
        if (entry.hasIngress && entry.lastIngressSeq == sequence) {
          result.samePhaseDuplicate = true;
          ++snapshot_.ingressDuplicates;
        }
        entry.hasIngress = true;
        entry.lastIngressSeq = sequence;
        if (entry.maxIngressSeq < sequence) {
          entry.maxIngressSeq = sequence;
        }
        ++snapshot_.ingressEntries;
      } else {
        if (entry.hasPublish && entry.lastPublishSeq == sequence) {
          result.samePhaseDuplicate = true;
          ++snapshot_.publishDuplicates;
        }
        entry.hasPublish = true;
        entry.lastPublishSeq = sequence;
        classifyPublish(entry, sequence);
        ++snapshot_.publishEntries;
        result.covered = entry.hasIngress && entry.maxIngressSeq >= sequence;
        result.stale = entry.hasIngress && entry.maxIngressSeq < sequence;
        result.noIngress = !entry.hasIngress;
      }
      return result;
    }
    ++snapshot_.overflow;
    snapshot_.collisionProbes += kCapacity;
    return ResourceMarkObservation{.overflow = true,
                                   .collisionProbes = kCapacity};
  }

  ResourceMarkOverlapSnapshot snapshot() const noexcept {
    std::lock_guard lock(mutex_);
    return snapshot_;
  }

  void reset() noexcept {
    std::lock_guard lock(mutex_);
    entries_ = {};
    snapshot_ = {};
  }

 private:
  struct Entry {
    ResourceMarkObservationKey key{};
    std::uint64_t maxIngressSeq = 0;
    std::uint64_t lastIngressSeq = 0;
    std::uint64_t lastPublishSeq = 0;
    bool hasIngress = false;
    bool hasPublish = false;
    bool occupied = false;
  };

  void incrementPhaseCount(ResourceMarkObservationPhase phase) noexcept {
    if (phase == ResourceMarkObservationPhase::Ingress) {
      ++snapshot_.ingressEntries;
    } else {
      ++snapshot_.publishEntries;
    }
  }

  void classifyPublish(const Entry& entry, std::uint64_t sequence) noexcept {
    if (!entry.hasIngress) {
      ++snapshot_.publishNoIngress;
    } else if (entry.maxIngressSeq >= sequence) {
      ++snapshot_.publishCovered;
    } else {
      ++snapshot_.publishStale;
    }
  }

  mutable std::mutex mutex_;
  std::array<Entry, kCapacity> entries_{};
  ResourceMarkOverlapSnapshot snapshot_{};
};

}  // namespace dxmt9::resources
