#pragma once

#include "dxmt9_backend_types.hpp"
#include "dxmt9_source_payload.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>

namespace dxmt9::core {

inline constexpr std::size_t kCpuReadyPageArenaAlignment =
    kSourcePayloadByteAlignment;

struct CpuReadyAlignedByteDeleter {
  void operator()(std::byte* memory) const noexcept {
    ::operator delete[](memory,
                        std::align_val_t(kCpuReadyPageArenaAlignment));
  }
};

using CpuReadyAlignedBytes =
    std::unique_ptr<std::byte[], CpuReadyAlignedByteDeleter>;

inline CpuReadyAlignedBytes allocateCpuReadyAlignedBytes(
    std::size_t byteCount) {
  return CpuReadyAlignedBytes(static_cast<std::byte*>(::operator new[](
      byteCount, std::align_val_t(kCpuReadyPageArenaAlignment))));
}

struct alignas(ArenaSourcePayloadBlock) CpuReadyArenaOwnerSlot {
  std::array<std::byte, sizeof(ArenaSourcePayloadBlock)> storage{};
  bool constructed = false;

  // Raw address identity remains valid before construct_at and after
  // destroy_at. Only payload() launders, and callers may use it exclusively
  // while constructed is true and the owner has not been detached.
  ArenaSourcePayloadBlock* storageAddress() noexcept {
    return reinterpret_cast<ArenaSourcePayloadBlock*>(storage.data());
  }

  ArenaSourcePayloadBlock* payload() noexcept {
    return std::launder(storageAddress());
  }

  const ArenaSourcePayloadBlock* payload() const noexcept {
    return std::launder(
        reinterpret_cast<const ArenaSourcePayloadBlock*>(storage.data()));
  }
};
static_assert(alignof(CpuReadyArenaOwnerSlot) >=
              alignof(ArenaSourcePayloadBlock));

// Compatibility payload used while the existing ChunkSlot SoAs are migrated
// to arena-backed containers. CpuReadyTape owns this in a separate fixed lane;
// source descriptors never embed a payload-owning ChunkSlot.
using SourcePayloadBlock = ChunkSlot;

struct CpuReadySourceId {
  std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
  std::uint64_t generation = 0;

  constexpr bool valid() const noexcept {
    return generation != 0 &&
           index != std::numeric_limits<std::uint32_t>::max();
  }

  friend constexpr bool operator==(CpuReadySourceId,
                                   CpuReadySourceId) noexcept = default;
};

struct CpuReadyStorageRef {
  std::uint32_t firstPage = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t pageCount = 0;
  std::uint64_t generation = 0;

  constexpr bool valid() const noexcept {
    return generation != 0 && pageCount != 0 &&
           firstPage != std::numeric_limits<std::uint32_t>::max();
  }

  friend constexpr bool operator==(CpuReadyStorageRef,
                                   CpuReadyStorageRef) noexcept = default;
};

class CpuReadyTapeConfig {
 public:
  struct Values {
    std::size_t pageSize = 4096;
    std::size_t pageCount = 0;
    std::size_t sourceSlotCount = 0;
    std::size_t readyFifoCount = 0;
    std::size_t compatibilityPayloadCount = 0;
    std::size_t maxPagesPerSource = 0;
    std::size_t highWaterSources = 0;
    std::size_t lowWaterSources = 0;
    std::size_t highWaterPages = 0;
    std::size_t lowWaterPages = 0;
    std::size_t highWaterReady = 0;
    std::size_t lowWaterReady = 0;
  };

  static std::optional<CpuReadyTapeConfig> create(Values values) noexcept {
    if (values.pageSize == 0 || values.pageCount == 0 ||
        values.sourceSlotCount == 0 || values.readyFifoCount == 0 ||
        values.compatibilityPayloadCount == 0 ||
        values.pageSize % kCpuReadyPageArenaAlignment != 0 ||
        values.pageCount > std::numeric_limits<std::uint32_t>::max() ||
        values.sourceSlotCount >
            std::numeric_limits<std::uint32_t>::max() ||
        values.readyFifoCount > values.sourceSlotCount ||
        values.maxPagesPerSource == 0 ||
        values.maxPagesPerSource > values.pageCount ||
        values.highWaterSources == 0 ||
        values.highWaterSources > values.sourceSlotCount ||
        values.lowWaterSources >= values.highWaterSources ||
        values.highWaterPages == 0 ||
        values.highWaterPages > values.pageCount ||
        values.maxPagesPerSource > values.highWaterPages ||
        values.lowWaterPages >= values.highWaterPages ||
        values.highWaterReady == 0 ||
        values.highWaterReady > values.readyFifoCount ||
        values.lowWaterReady >= values.highWaterReady ||
        values.pageSize >
            std::numeric_limits<std::size_t>::max() / values.pageCount) {
      return std::nullopt;
    }
    return CpuReadyTapeConfig(values);
  }

  static CpuReadyTapeConfig compatibility(std::size_t capacity) {
    const auto config = create(Values{
        .pageSize = 4096,
        .pageCount = capacity,
        .sourceSlotCount = capacity,
        .readyFifoCount = capacity,
        .compatibilityPayloadCount = capacity,
        .maxPagesPerSource = capacity,
        .highWaterSources = capacity,
        .lowWaterSources = capacity > 1 ? capacity - 1 : 0,
        .highWaterPages = capacity,
        .lowWaterPages = capacity > 1 ? capacity - 1 : 0,
        .highWaterReady = capacity,
        .lowWaterReady = capacity > 1 ? capacity - 1 : 0,
    });
    if (!config) {
      throw std::invalid_argument("invalid CpuReadyTape compatibility capacity");
    }
    return *config;
  }

  static CpuReadyTapeConfig queueCompatibility(
      std::size_t controlCapacity) {
    if (controlCapacity > std::numeric_limits<std::size_t>::max() / 2) {
      throw std::invalid_argument("CpuReadyTape control capacity overflow");
    }
    const auto config = create(Values{
        .pageSize = 4096,
        .pageCount = controlCapacity * 2,
        .sourceSlotCount = controlCapacity * 2,
        .readyFifoCount = controlCapacity,
        .compatibilityPayloadCount = controlCapacity * 2,
        .maxPagesPerSource = controlCapacity * 2,
        .highWaterSources = controlCapacity * 2,
        .lowWaterSources = controlCapacity,
        .highWaterPages = controlCapacity * 2,
        .lowWaterPages = controlCapacity,
        .highWaterReady = controlCapacity,
        .lowWaterReady = controlCapacity / 2,
    });
    if (!config) {
      throw std::invalid_argument("invalid CpuReadyTape queue capacity");
    }
    return *config;
  }

  const Values& values() const noexcept { return values_; }

 private:
  explicit constexpr CpuReadyTapeConfig(Values values) noexcept
      : values_(values) {}

  Values values_{};
};

struct CpuReadyPublicationTicket {
  CpuReadySourceId id{};
  CpuReadyStorageRef storage{};
  std::uint64_t rawOrdinal = 0;
  std::uint64_t sourceOrdinal = 0;
  std::uint64_t seqId = 0;
  std::uint64_t buildGeneration = 0;

  constexpr bool valid() const noexcept {
    return id.valid() && storage.valid();
  }

  constexpr bool hasAdmissionIdentity() const noexcept {
    return rawOrdinal != 0 || sourceOrdinal != 0 || seqId != 0 ||
           buildGeneration != 0;
  }

  constexpr bool strictIdentityValid() const noexcept {
    return rawOrdinal != 0 && sourceOrdinal != 0 && seqId != 0 &&
           buildGeneration != 0;
  }

  friend constexpr bool operator==(CpuReadyPublicationTicket,
                                   CpuReadyPublicationTicket) noexcept = default;
};

struct CpuReadyAdmissionIdentity {
  std::uint64_t rawOrdinal = 0;
  std::uint64_t sourceOrdinal = 0;
  std::uint64_t seqId = 0;
  std::uint64_t buildGeneration = 0;

  constexpr bool valid() const noexcept {
    return rawOrdinal != 0 && sourceOrdinal != 0 && seqId != 0 &&
           buildGeneration != 0;
  }
};

struct CpuReadyLayoutRegionRequest {
  std::size_t byteCount = 0;
  std::size_t alignment = 1;
};

struct CpuReadyLayoutRegion {
  std::size_t offset = 0;
  std::size_t byteCount = 0;
  std::size_t alignment = 1;
};

struct CpuReadyPayloadLayout {
  static constexpr std::size_t kMaxRegions = 16;

  std::array<CpuReadyLayoutRegion, kMaxRegions> regions{};
  std::size_t regionCount = 0;
  std::size_t usedBytes = 0;
  std::size_t pageCount = 0;
};

inline std::optional<CpuReadyPayloadLayout> makeCpuReadyPayloadLayout(
    std::span<const CpuReadyLayoutRegionRequest> requests,
    std::size_t pageSize,
    std::size_t maxPages) noexcept {
  if (pageSize == 0 || maxPages == 0 ||
      requests.size() > CpuReadyPayloadLayout::kMaxRegions) {
    return std::nullopt;
  }

  CpuReadyPayloadLayout layout{};
  layout.regionCount = requests.size();
  std::size_t cursor = 0;
  for (std::size_t i = 0; i < requests.size(); ++i) {
    const auto request = requests[i];
    if (request.alignment == 0 ||
        (request.alignment & (request.alignment - 1)) != 0 ||
        cursor > std::numeric_limits<std::size_t>::max() -
                     (request.alignment - 1)) {
      return std::nullopt;
    }
    const std::size_t offset =
        (cursor + request.alignment - 1) & ~(request.alignment - 1);
    if (request.byteCount >
        std::numeric_limits<std::size_t>::max() - offset) {
      return std::nullopt;
    }
    layout.regions[i] = CpuReadyLayoutRegion{
        .offset = offset,
        .byteCount = request.byteCount,
        .alignment = request.alignment,
    };
    cursor = offset + request.byteCount;
  }
  if (cursor > std::numeric_limits<std::size_t>::max() - (pageSize - 1)) {
    return std::nullopt;
  }
  const std::size_t pages = (cursor + pageSize - 1) / pageSize;
  if (pages == 0 || pages > maxPages) {
    return std::nullopt;
  }
  layout.usedBytes = cursor;
  layout.pageCount = pages;
  return layout;
}

// Queue-ring control shell. The payload pointer is a compatibility-lane view,
// not ownership. Queue code must re-resolve sourceId + storage before use.
struct ChunkSlotControl {
  using State = ChunkSlot::State;

  State state = State::Free;
  std::uint64_t seqId = 0;
  CpuReadySourceId sourceId{};
  CpuReadyStorageRef storage{};
  SourcePayloadBlock* payload = nullptr;

  bool commandsEmpty() const noexcept {
    return !payload || payload->commandsEmpty();
  }

  std::size_t commandCount() const noexcept {
    return payload ? payload->commandCount() : 0;
  }
};

// Fixed source descriptors, a circular non-wrapping page allocator, and a
// separately bounded compatibility payload lane. Every method is called under
// the owning queue's scheduling mutex unless explicitly documented otherwise.
class CpuReadyTape {
 public:
  enum class PayloadKind {
    Legacy,
    Arena,
  };

  enum class State {
    Free,
    Writing,
    Sealed,
    Ready,
    Represented,
    Encoding = Represented,
    Submitted,
    GPU = Submitted,
    Completed,
    Reclaiming,
  };

  struct Reservation {
    CpuReadySourceId id{};
    CpuReadyStorageRef storage{};
    CpuReadyPublicationTicket ticket{};
    PayloadKind payloadKind = PayloadKind::Legacy;
    SourcePayloadBlock* payload = nullptr;
    ArenaSourcePayloadBlock* arenaPayload = nullptr;

    explicit operator bool() const noexcept {
      return ticket.valid() &&
             (payloadKind == PayloadKind::Legacy ? payload != nullptr
                                                 : arenaPayload != nullptr);
    }
  };

  struct SourceRef {
    CpuReadySourceId id{};
    CpuReadyStorageRef storage{};

    constexpr bool valid() const noexcept {
      return id.valid() && storage.valid();
    }

    friend constexpr bool operator==(SourceRef, SourceRef) noexcept = default;
  };

  struct ReadyEntry {
    SourceRef source{};
    std::size_t controlIndex = std::numeric_limits<std::size_t>::max();
    std::uint64_t seqId = 0;

    constexpr bool valid() const noexcept {
      return source.valid() &&
             controlIndex != std::numeric_limits<std::size_t>::max() &&
             seqId != 0;
    }

    friend constexpr bool operator==(ReadyEntry, ReadyEntry) noexcept = default;
  };

  struct ResidentEntry {
    SourceRef source{};
    std::uint64_t seqId = 0;
    State state = State::Free;
  };

  // Move-only capability returned while the source/page generations remain
  // pinned. destroy() is deliberately callable after releasing the queue
  // lock; finishing the transaction advances generations under the lock.
  class DetachedArenaOwner {
   public:
    DetachedArenaOwner() = default;
    ~DetachedArenaOwner() { destroy(); }

    DetachedArenaOwner(const DetachedArenaOwner&) = delete;
    DetachedArenaOwner& operator=(const DetachedArenaOwner&) = delete;

    DetachedArenaOwner(DetachedArenaOwner&& other) noexcept
        : payload_(other.payload_), destroyed_(other.destroyed_) {
      other.payload_ = nullptr;
      other.destroyed_ = false;
    }

    DetachedArenaOwner& operator=(DetachedArenaOwner&& other) noexcept {
      if (this == &other) {
        return *this;
      }
      destroy();
      payload_ = other.payload_;
      destroyed_ = other.destroyed_;
      other.payload_ = nullptr;
      other.destroyed_ = false;
      return *this;
    }

    explicit operator bool() const noexcept { return payload_ != nullptr; }
    bool destroyed() const noexcept { return destroyed_; }

    void destroy() noexcept {
      if (!payload_ || destroyed_) {
        return;
      }
      payload_->destroyConstructed();
      std::destroy_at(payload_);
      destroyed_ = true;
    }

   private:
    friend class CpuReadyTape;

    explicit DetachedArenaOwner(ArenaSourcePayloadBlock* payload) noexcept
        : payload_(payload) {}

    ArenaSourcePayloadBlock* payload_ = nullptr;
    bool destroyed_ = false;
  };

  enum class ReserveProbe {
    Ready,
    TemporaryPressure,
    InvalidRequest,
    Stopped,
    Corrupt,
  };

  struct Stats {
    std::size_t residentSources = 0;
    std::size_t residentPages = 0;
    std::size_t compatibilityPayloads = 0;
    std::size_t readyPublicationReservations = 0;
    std::size_t readyFifoEntries = 0;
    std::size_t wrapPaddingPages = 0;
    std::uint64_t admissionCloses = 0;
    std::uint64_t admissionReopens = 0;
    std::uint64_t staleRejects = 0;
    bool admissionClosed = false;
  };

  explicit CpuReadyTape(std::size_t capacity)
      : CpuReadyTape(CpuReadyTapeConfig::compatibility(capacity)) {}

  explicit CpuReadyTape(CpuReadyTapeConfig config)
      : config_(config),
        entries_(std::make_unique<Entry[]>(config.values().sourceSlotCount)),
        readyFifo_(std::make_unique<ReadyEntry[]>(
            config.values().readyFifoCount)),
        pages_(std::make_unique<Page[]>(config.values().pageCount)),
        pageArena_(allocateCpuReadyAlignedBytes(
            config.values().pageSize * config.values().pageCount)),
        arenaOwners_(std::make_unique<CpuReadyArenaOwnerSlot[]>(
            config.values().sourceSlotCount)),
        compatibilityPayloads_(std::make_unique<SourcePayloadBlock[]>(
            config.values().compatibilityPayloadCount)),
        compatibilityOwners_(std::make_unique<CpuReadySourceId[]>(
            config.values().compatibilityPayloadCount)) {}

  ~CpuReadyTape() {
    for (std::size_t i = 0; i < capacity(); ++i) {
      auto& owner = arenaOwners_[i];
      if (owner.constructed && !entries_[i].arenaOwnerDetached) {
        owner.payload()->destroyConstructed();
        std::destroy_at(owner.payload());
        owner.constructed = false;
      }
    }
  }

  CpuReadyTape(const CpuReadyTape&) = delete;
  CpuReadyTape& operator=(const CpuReadyTape&) = delete;

  const CpuReadyTapeConfig& config() const noexcept { return config_; }
  std::size_t capacity() const noexcept {
    return config_.values().sourceSlotCount;
  }
  std::size_t residentCount() const noexcept { return residentCount_; }
  std::size_t readyCount() const noexcept { return readyCount_; }
  bool readyEmpty() const noexcept { return readyCount_ == 0; }
  const Stats& stats() const noexcept { return stats_; }

  std::uint64_t sourceGenerationAt(std::size_t index) const noexcept {
    return index < capacity() ? entries_[index].generation : 0;
  }

  std::size_t copyReadyPrefix(std::span<ReadyEntry> out) const noexcept {
    const std::size_t count = std::min(out.size(), readyCount_);
    for (std::size_t i = 0; i < count; ++i) {
      const auto& ready = readyFifo_[readyIndex(i)];
      const auto* entry = resolveEntry(ready.source.id, ready.source.storage);
      if (!ready.valid() || !entry || entry->state != State::Ready ||
          entry->seqId != ready.seqId || !entry->readyPublicationReserved) {
        noteStaleReject();
        return 0;
      }
    }
    for (std::size_t i = 0; i < count; ++i) {
      out[i] = readyFifo_[readyIndex(i)];
    }
    return count;
  }

  std::optional<ResidentEntry> oldestResident() const noexcept {
    if (residentCount_ == 0) {
      return std::nullopt;
    }
    const auto& entry = entries_[sourceHead_];
    const SourceRef source{
        .id = CpuReadySourceId{
            .index = static_cast<std::uint32_t>(sourceHead_),
            .generation = entry.generation,
        },
        .storage = entry.storage,
    };
    if (!resolveEntry(source.id, source.storage)) {
      noteStaleReject();
      return std::nullopt;
    }
    return ResidentEntry{
        .source = source,
        .seqId = entry.seqId,
        .state = entry.state,
    };
  }

  std::optional<State> state(CpuReadySourceId id,
                             CpuReadyStorageRef storage) const noexcept {
    const auto* entry = resolveEntry(id, storage);
    if (!entry) {
      noteStaleReject();
      return std::nullopt;
    }
    return entry->state;
  }

  bool matches(SourceRef source, std::uint64_t seqId,
               State expected) const noexcept {
    const auto* entry = resolveEntry(source.id, source.storage);
    if (!entry || entry->state != expected || expected == State::Writing ||
        expected == State::Sealed || expected == State::Reclaiming ||
        seqId == 0 || entry->seqId != seqId ||
        (entry->payloadKind == PayloadKind::Legacy
             ? !compatibilityPayload(*entry) ||
                   compatibilityPayload(*entry)->seqId != seqId
             : !arenaPayload(*entry) || !arenaPayload(*entry)->readable())) {
      noteStaleReject();
      return false;
    }
    return true;
  }

  void stopAdmission() noexcept {
    const bool wasClosed = stats_.admissionClosed;
    stopped_ = true;
    stats_.admissionClosed = true;
    if (!wasClosed) {
      ++stats_.admissionCloses;
    }
  }

  bool canReserve(std::size_t pageCount = 1) const noexcept {
    return inspectReserve(pageCount, PayloadKind::Legacy) ==
           ReserveProbe::Ready;
  }

  ReserveProbe probeReserve(std::size_t pageCount = 1) noexcept {
    return probeReserve(pageCount, PayloadKind::Legacy);
  }

  ReserveProbe probeArenaReserve(std::size_t pageCount) noexcept {
    return probeReserve(pageCount, PayloadKind::Arena);
  }

  ReserveProbe probeReserve(std::size_t pageCount,
                            PayloadKind payloadKind) noexcept {
    const ReserveProbe probe = inspectReserve(pageCount, payloadKind);
    if (probe == ReserveProbe::TemporaryPressure) {
      closeAdmission(pressureDimensions(pageCount, payloadKind));
    } else if (probe == ReserveProbe::Corrupt) {
      // Allocator metadata disagreement is not recoverable pressure. Keep the
      // tape fail-closed so the owning queue can stop instead of waiting for
      // a low-water event that cannot repair corruption.
      stopAdmission();
    }
    return probe;
  }

  std::optional<Reservation> reserve(std::size_t pageCount = 1) noexcept {
    // Compatibility tickets acquire source/sequence identity only at seal.
    // Do not overlap that open interval with a strict admission whose identity
    // is consumed at reserve time.
    if (strictWritingActive_) {
      return std::nullopt;
    }
    // Invalid and permanently oversize candidates are not temporary pressure:
    // rejecting them must not latch admission closed for later valid work.
    if (probeReserve(pageCount, PayloadKind::Legacy) != ReserveProbe::Ready) {
      return std::nullopt;
    }
    return reserveStorageImpl(pageCount, PayloadKind::Legacy);
  }

  // Strict structural admission. plannedBytes must name this exact page run.
  // raw/source/seq are consumed globally by a successful reserve even if
  // construction aborts; buildGeneration is an exact-match freshness stamp.
  std::optional<Reservation> reserve(
      std::size_t pageCount,
      std::size_t plannedBytes,
      CpuReadyAdmissionIdentity identity) noexcept {
    if (!identity.valid() || plannedBytes == 0 ||
        1 + (plannedBytes - 1) / config_.values().pageSize != pageCount ||
        compatibilityWritingCount_ != 0 || strictWritingActive_ ||
        identity.rawOrdinal <= rawOrdinalHighWater_ ||
        identity.sourceOrdinal <= sourceOrdinalHighWater_ ||
        identity.seqId <= seqIdHighWater_) {
      return std::nullopt;
    }

    if (probeReserve(pageCount, PayloadKind::Arena) != ReserveProbe::Ready) {
      return std::nullopt;
    }
    auto reservation = reserveStorageImpl(pageCount, PayloadKind::Arena);
    if (!reservation) {
      return std::nullopt;
    }
    auto* entry = resolveEntry(reservation->id, reservation->storage);
    if (!entry || entry->payloadKind != PayloadKind::Arena) {
      stopAdmission();
      return std::nullopt;
    }

    strictWritingActive_ = true;
    entry->strictAdmission = true;
    entry->plannedBytes = plannedBytes;
    entry->rawOrdinal = identity.rawOrdinal;
    entry->sourceOrdinal = identity.sourceOrdinal;
    entry->seqId = identity.seqId;
    entry->buildGeneration = identity.buildGeneration;
    rawOrdinalHighWater_ = identity.rawOrdinal;
    sourceOrdinalHighWater_ = identity.sourceOrdinal;
    seqIdHighWater_ = identity.seqId;

    reservation->ticket = CpuReadyPublicationTicket{
        .id = reservation->id,
        .storage = reservation->storage,
        .rawOrdinal = identity.rawOrdinal,
        .sourceOrdinal = identity.sourceOrdinal,
        .seqId = identity.seqId,
        .buildGeneration = identity.buildGeneration,
    };
    return reservation;
  }

  std::span<std::byte> writableStorage(
      CpuReadyPublicationTicket ticket) noexcept {
    auto* entry = resolveEntry(ticket.id, ticket.storage);
    if (!entry || entry->state != State::Writing ||
        !ticketMatchesEntry(ticket, *entry)) {
      noteStaleReject();
      return {};
    }
    return storageSpan(ticket.storage);
  }

  std::span<const std::byte> resolveStorage(
      CpuReadySourceId id, CpuReadyStorageRef storage,
      State expected) const noexcept {
    const auto* entry = resolveEntry(id, storage);
    if (!entry || entry->state != expected ||
        expected == State::Writing || expected == State::Sealed ||
        expected == State::Reclaiming) {
      noteStaleReject();
      return {};
    }
    const auto bytes = storageSpan(storage);
    return bytes.first(std::min(entry->usedBytes, bytes.size()));
  }

  SourcePayloadBlock* resolveForWrite(
      CpuReadyPublicationTicket ticket) noexcept {
    auto* entry = resolveEntry(ticket.id, ticket.storage);
    if (!entry || entry->state != State::Writing ||
        !ticketMatchesEntry(ticket, *entry)) {
      noteStaleReject();
      return nullptr;
    }
    return entry->payloadKind == PayloadKind::Legacy
               ? compatibilityPayload(*entry)
               : nullptr;
  }

  ArenaSourcePayloadBlock* resolveArenaForWrite(
      CpuReadyPublicationTicket ticket) noexcept {
    auto* entry = resolveEntry(ticket.id, ticket.storage);
    if (!entry || entry->state != State::Writing ||
        entry->payloadKind != PayloadKind::Arena ||
        !ticketMatchesEntry(ticket, *entry)) {
      noteStaleReject();
      return nullptr;
    }
    return arenaPayload(*entry);
  }

  SourcePayloadBlock* resolve(CpuReadySourceId id,
                              CpuReadyStorageRef storage,
                              State expected) noexcept {
    auto* entry = resolveEntry(id, storage);
    return resolvePayload(entry, expected);
  }

  const SourcePayloadBlock* resolve(CpuReadySourceId id,
                                    CpuReadyStorageRef storage,
                                    State expected) const noexcept {
    const auto* entry = resolveEntry(id, storage);
    return resolvePayload(entry, expected);
  }

  ArenaSourcePayloadBlock* resolveArena(CpuReadySourceId id,
                                        CpuReadyStorageRef storage,
                                        State expected) noexcept {
    auto* entry = resolveEntry(id, storage);
    return resolveArenaPayload(entry, expected);
  }

  const ArenaSourcePayloadBlock* resolveArena(
      CpuReadySourceId id, CpuReadyStorageRef storage,
      State expected) const noexcept {
    const auto* entry = resolveEntry(id, storage);
    return resolveArenaPayload(entry, expected);
  }

  // Common call-local consumer view. This inspects payload kind once and does
  // not probe the wrong resolver (which would count a valid arena source as a
  // stale legacy lookup). Never retain the returned borrowed pointers beyond
  // the represented/submitted lifecycle operation that obtained the view.
  SourcePayloadView resolveSourcePayload(
      CpuReadySourceId id, CpuReadyStorageRef storage,
      State expected) const noexcept {
    const auto* entry = resolveEntry(id, storage);
    if (!entry || entry->state != expected || expected == State::Writing ||
        expected == State::Sealed || expected == State::Reclaiming) {
      noteStaleReject();
      return {};
    }
    if (entry->payloadKind == PayloadKind::Legacy) {
      const auto* payload = compatibilityPayload(*entry);
      return payload ? SourcePayloadView(*payload) : SourcePayloadView{};
    }
    const auto* payload = arenaPayload(*entry);
    return payload && payload->readable() ? SourcePayloadView(*payload)
                                          : SourcePayloadView{};
  }

  std::optional<PayloadKind> payloadKind(
      CpuReadySourceId id, CpuReadyStorageRef storage,
      State expected) const noexcept {
    const auto* entry = resolveEntry(id, storage);
    if (!entry || entry->state != expected) {
      noteStaleReject();
      return std::nullopt;
    }
    return entry->payloadKind;
  }

  bool sealAndPublish(CpuReadyPublicationTicket ticket,
                      std::uint64_t sourceOrdinal,
                      std::uint64_t seqId,
                      std::size_t controlIndex,
                      std::size_t usedBytes = 0,
                      std::uint64_t rawOrdinal = 0) noexcept {
    auto* entry = resolveEntry(ticket.id, ticket.storage);
    const std::size_t reservedBytes =
        static_cast<std::size_t>(ticket.storage.pageCount) *
        config_.values().pageSize;
    if (!entry || entry->state != State::Writing || entry->strictAdmission ||
        entry->payloadKind != PayloadKind::Legacy ||
        ticket.hasAdmissionIdentity() || sourceOrdinal == 0 || seqId == 0 ||
        (rawOrdinal != 0 && rawOrdinal <= rawOrdinalHighWater_) ||
        sourceOrdinal <= sourceOrdinalHighWater_ ||
        seqId <= seqIdHighWater_ || controlIndex == kInvalidIndex ||
        usedBytes > reservedBytes || !entry->readyPublicationReserved ||
        readyCount_ >= config_.values().readyFifoCount ||
        readyFifo_[readyTail_].valid()) {
      noteStaleReject();
      return false;
    }
    auto* payload = compatibilityPayload(*entry);
    if (!payload) {
      noteStaleReject();
      return false;
    }
    payload->seqId = seqId;
    entry->rawOrdinal = rawOrdinal;
    entry->sourceOrdinal = sourceOrdinal;
    entry->seqId = seqId;
    entry->usedBytes = usedBytes;
    entry->state = State::Ready;
    readyFifo_[readyTail_] = ReadyEntry{
        .source = SourceRef{.id = ticket.id, .storage = ticket.storage},
        .controlIndex = controlIndex,
        .seqId = seqId,
    };
    readyTail_ = (readyTail_ + 1) % config_.values().readyFifoCount;
    ++readyCount_;
    stats_.readyFifoEntries = readyCount_;
    --compatibilityWritingCount_;
    if (rawOrdinal != 0) {
      rawOrdinalHighWater_ = rawOrdinal;
    }
    sourceOrdinalHighWater_ = sourceOrdinal;
    seqIdHighWater_ = seqId;
    return true;
  }

  bool sealAndPublish(CpuReadyPublicationTicket ticket,
                      std::size_t controlIndex,
                      std::size_t usedBytes = 0) noexcept {
    auto* entry = resolveEntry(ticket.id, ticket.storage);
    const std::size_t reservedBytes =
        static_cast<std::size_t>(ticket.storage.pageCount) *
        config_.values().pageSize;
    if (!entry || entry->state != State::Writing ||
        !entry->strictAdmission || !strictWritingActive_ ||
        entry->payloadKind != PayloadKind::Arena ||
        !ticketMatchesEntry(ticket, *entry) ||
        controlIndex == kInvalidIndex || usedBytes != entry->plannedBytes ||
        entry->plannedBytes > reservedBytes ||
        !entry->readyPublicationReserved ||
        readyCount_ >= config_.values().readyFifoCount ||
        readyFifo_[readyTail_].valid()) {
      noteStaleReject();
      return false;
    }
    auto* payload = arenaPayload(*entry);
    const auto reservedStorage = storageSpan(ticket.storage);
    const auto plannedStorage = reservedStorage.first(entry->plannedBytes);
    if (!payload || !payload->published() ||
        !payload->boundTo(std::span<const std::byte>(plannedStorage))) {
      noteStaleReject();
      return false;
    }
    entry->usedBytes = usedBytes;
    entry->state = State::Ready;
    readyFifo_[readyTail_] = ReadyEntry{
        .source = SourceRef{.id = ticket.id, .storage = ticket.storage},
        .controlIndex = controlIndex,
        .seqId = entry->seqId,
    };
    readyTail_ = (readyTail_ + 1) % config_.values().readyFifoCount;
    ++readyCount_;
    stats_.readyFifoEntries = readyCount_;
    strictWritingActive_ = false;
    return true;
  }

  bool representReadyPrefix(std::span<const ReadyEntry> selected) noexcept {
    if (selected.empty() || selected.size() > readyCount_) {
      noteStaleReject();
      return false;
    }
    for (std::size_t i = 0; i < selected.size(); ++i) {
      const auto& ready = readyFifo_[readyIndex(i)];
      auto* entry = resolveEntry(ready.source.id, ready.source.storage);
      if (selected[i] != ready || !ready.valid() || !entry ||
          entry->state != State::Ready || entry->seqId != ready.seqId ||
          !entry->readyPublicationReserved) {
        noteStaleReject();
        return false;
      }
    }
    for (std::size_t i = 0; i < selected.size(); ++i) {
      auto& ready = readyFifo_[readyHead_];
      auto* entry = resolveEntry(ready.source.id, ready.source.storage);
      entry->state = State::Represented;
      entry->readyPublicationReserved = false;
      ready = {};
      readyHead_ = (readyHead_ + 1) % config_.values().readyFifoCount;
    }
    readyCount_ -= selected.size();
    readyPublicationReservations_ -= selected.size();
    stats_.readyFifoEntries = readyCount_;
    stats_.readyPublicationReservations = readyPublicationReservations_;
    refreshAdmissionAfterRelease();
    return true;
  }

  bool abort(CpuReadyPublicationTicket ticket) noexcept {
    auto* entry = resolveEntry(ticket.id, ticket.storage);
    if (!entry || entry->state != State::Writing || residentCount_ == 0 ||
        entry->payloadKind != PayloadKind::Legacy ||
        ticket.id.index != previousSourceIndex(sourceTail_) ||
        !ticketMatchesEntry(ticket, *entry) ||
        !entry->readyPublicationReserved ||
        readyPublicationReservations_ == 0) {
      noteStaleReject();
      return false;
    }
    auto* payload = compatibilityPayload(*entry);
    if (payload) {
      payload->clearCommands();
      payload->seqId = 0;
    }
    --readyPublicationReservations_;
    stats_.readyPublicationReservations = readyPublicationReservations_;
    --compatibilityWritingCount_;
    rollbackNewest(*entry);
    refreshAdmissionAfterRelease();
    return true;
  }

  std::optional<DetachedArenaOwner> beginArenaAbort(
      CpuReadyPublicationTicket ticket) noexcept {
    auto* entry = resolveEntry(ticket.id, ticket.storage);
    if (!entry || entry->state != State::Writing || residentCount_ == 0 ||
        entry->payloadKind != PayloadKind::Arena ||
        ticket.id.index != previousSourceIndex(sourceTail_) ||
        !ticketMatchesEntry(ticket, *entry) ||
        !entry->readyPublicationReserved ||
        readyPublicationReservations_ == 0 || entry->arenaOwnerDetached) {
      noteStaleReject();
      return std::nullopt;
    }
    auto* payload = arenaPayload(*entry);
    if (!payload) {
      noteStaleReject();
      return std::nullopt;
    }
    --readyPublicationReservations_;
    stats_.readyPublicationReservations = readyPublicationReservations_;
    entry->state = State::Reclaiming;
    entry->arenaOwnerDetached = true;
    entry->arenaDetachKind = Entry::ArenaDetachKind::Abort;
    return DetachedArenaOwner(payload);
  }

  bool finishArenaAbort(CpuReadyPublicationTicket ticket,
                        DetachedArenaOwner&& owner) noexcept {
    auto* entry = resolveEntry(ticket.id, ticket.storage);
    if (!entry || entry->state != State::Reclaiming ||
        entry->payloadKind != PayloadKind::Arena ||
        ticket.id.index != previousSourceIndex(sourceTail_) ||
        !ticketMatchesEntry(ticket, *entry) || !entry->arenaOwnerDetached ||
        entry->arenaDetachKind != Entry::ArenaDetachKind::Abort ||
        !owner.destroyed() ||
        owner.payload_ != arenaOwners_[ticket.id.index].storageAddress()) {
      noteStaleReject();
      return false;
    }
    owner.payload_ = nullptr;
    owner.destroyed_ = false;
    arenaOwners_[ticket.id.index].constructed = false;
    strictWritingActive_ = false;
    rollbackNewest(*entry);
    refreshAdmissionAfterRelease();
    return true;
  }

  bool transition(CpuReadySourceId id, CpuReadyStorageRef storage,
                  State before, State after) noexcept {
    const std::array sources{SourceRef{.id = id, .storage = storage}};
    return transitionAll(sources, before, after);
  }

  bool transitionAll(std::span<const SourceRef> sources,
                     State before, State after) noexcept {
    if (sources.empty() || !validTransition(before, after)) {
      noteStaleReject();
      return false;
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
      auto* entry = resolveEntry(sources[i].id, sources[i].storage);
      if (!entry || entry->state != before) {
        noteStaleReject();
        return false;
      }
      for (std::size_t j = 0; j < i; ++j) {
        if (sources[j].id == sources[i].id) {
          noteStaleReject();
          return false;
        }
      }
    }
    for (const auto& source : sources) {
      resolveEntry(source.id, source.storage)->state = after;
    }
    return true;
  }

  bool complete(CpuReadySourceId id, CpuReadyStorageRef storage) noexcept {
    return transition(id, storage, State::Submitted, State::Completed);
  }

  bool completeAll(std::span<const SourceRef> sources) noexcept {
    return transitionAll(sources, State::Submitted, State::Completed);
  }

  bool completeInline(CpuReadySourceId id,
                      CpuReadyStorageRef storage) noexcept {
    auto* entry = resolveEntry(id, storage);
    if (!entry || entry->state != State::Represented) {
      noteStaleReject();
      return false;
    }
    entry->state = State::Completed;
    return true;
  }

  bool beginReclaim(CpuReadySourceId id,
                    CpuReadyStorageRef storage) noexcept {
    auto* entry = resolveEntry(id, storage);
    if (!entry || id.index != sourceHead_ ||
        entry->state != State::Completed ||
        entry->readyPublicationReserved) {
      noteStaleReject();
      return false;
    }
    entry->state = State::Reclaiming;
    return true;
  }

  // Explicit two-phase reclaim capability. Generic resolve/matches/storage
  // APIs intentionally reject Reclaiming so only the reclaim transaction can
  // detach owners while the old source and page generations remain pinned.
  SourcePayloadBlock* reclaimingPayload(CpuReadySourceId id,
                                        CpuReadyStorageRef storage) noexcept {
    auto* entry = resolveEntry(id, storage);
    return entry && entry->state == State::Reclaiming
               ? compatibilityPayload(*entry)
               : nullptr;
  }

  bool finishReclaim(CpuReadySourceId id,
                     CpuReadyStorageRef storage) noexcept {
    auto* entry = resolveEntry(id, storage);
    if (!entry || entry->state != State::Reclaiming ||
        entry->payloadKind != PayloadKind::Legacy ||
        id.index != sourceHead_) {
      noteStaleReject();
      return false;
    }
    auto* payload = compatibilityPayload(*entry);
    if (!payload || !payload->commandsEmpty()) {
      return false;
    }
    releaseOldest(*entry);
    refreshAdmissionAfterRelease();
    return true;
  }

  std::optional<DetachedArenaOwner> detachReclaimingArenaOwner(
      CpuReadySourceId id, CpuReadyStorageRef storage) noexcept {
    auto* entry = resolveEntry(id, storage);
    if (!entry || entry->state != State::Reclaiming ||
        entry->payloadKind != PayloadKind::Arena ||
        id.index != sourceHead_ || entry->arenaOwnerDetached) {
      noteStaleReject();
      return std::nullopt;
    }
    auto* payload = arenaPayload(*entry);
    if (!payload) {
      noteStaleReject();
      return std::nullopt;
    }
    entry->arenaOwnerDetached = true;
    entry->arenaDetachKind = Entry::ArenaDetachKind::Reclaim;
    return DetachedArenaOwner(payload);
  }

  bool finishArenaReclaim(CpuReadySourceId id,
                          CpuReadyStorageRef storage,
                          DetachedArenaOwner&& owner) noexcept {
    auto* entry = resolveEntry(id, storage);
    if (!entry || entry->state != State::Reclaiming ||
        entry->payloadKind != PayloadKind::Arena || id.index != sourceHead_ ||
        !entry->arenaOwnerDetached ||
        entry->arenaDetachKind != Entry::ArenaDetachKind::Reclaim ||
        !owner.destroyed() ||
        owner.payload_ != arenaOwners_[id.index].storageAddress()) {
      noteStaleReject();
      return false;
    }
    owner.payload_ = nullptr;
    owner.destroyed_ = false;
    arenaOwners_[id.index].constructed = false;
    releaseOldest(*entry);
    refreshAdmissionAfterRelease();
    return true;
  }

  // Compatibility helper for callers not yet split into out-of-lock payload
  // destruction. New queue lifecycle code should use begin/finishReclaim.
  bool reclaim(CpuReadySourceId id, CpuReadyStorageRef storage) noexcept {
    auto* entry = resolveEntry(id, storage);
    if (!entry) {
      noteStaleReject();
      return false;
    }
    if (entry->payloadKind != PayloadKind::Legacy ||
        !beginReclaim(id, storage)) {
      return false;
    }
    auto* payload = reclaimingPayload(id, storage);
    payload->clearCommands();
    payload->seqId = 0;
    return finishReclaim(id, storage);
  }

 private:
  friend struct CpuReadyTapeTestAccess;

  static constexpr std::size_t kInvalidIndex =
      std::numeric_limits<std::size_t>::max();
  static constexpr std::uint8_t kPressureSources = 1u << 0;
  static constexpr std::uint8_t kPressurePages = 1u << 1;
  static constexpr std::uint8_t kPressureCompatibility = 1u << 2;
  static constexpr std::uint8_t kPressureReady = 1u << 3;

  struct Entry {
    std::uint64_t generation = 0;
    State state = State::Free;
    CpuReadyStorageRef storage{};
    std::size_t paddingFirstPage = 0;
    std::size_t paddingBefore = 0;
    std::size_t usedBytes = 0;
    std::size_t plannedBytes = 0;
    PayloadKind payloadKind = PayloadKind::Legacy;
    std::size_t compatibilityIndex = kInvalidIndex;
    std::uint64_t rawOrdinal = 0;
    std::uint64_t sourceOrdinal = 0;
    std::uint64_t seqId = 0;
    std::uint64_t buildGeneration = 0;
    bool strictAdmission = false;
    bool readyPublicationReserved = false;
    bool arenaOwnerDetached = false;
    enum class ArenaDetachKind : std::uint8_t {
      None,
      Abort,
      Reclaim,
    } arenaDetachKind = ArenaDetachKind::None;
  };

  struct Page {
    CpuReadySourceId owner{};
    std::uint64_t generation = 0;
    bool allocated = false;
  };

  static std::uint64_t nextGeneration(std::uint64_t current) noexcept {
    ++current;
    return current == 0 ? 1 : current;
  }

  static constexpr bool validTransition(State before, State after) noexcept {
    return (before == State::Represented && after == State::Submitted) ||
           (before == State::Submitted && after == State::Completed);
  }

  static bool ticketMatchesEntry(CpuReadyPublicationTicket ticket,
                                 const Entry& entry) noexcept {
    if (!entry.strictAdmission) {
      return !ticket.hasAdmissionIdentity();
    }
    return ticket.strictIdentityValid() &&
           ticket.rawOrdinal == entry.rawOrdinal &&
           ticket.sourceOrdinal == entry.sourceOrdinal &&
           ticket.seqId == entry.seqId &&
           ticket.buildGeneration == entry.buildGeneration;
  }

  std::size_t previousSourceIndex(std::size_t index) const noexcept {
    return index == 0 ? capacity() - 1 : index - 1;
  }

  std::size_t readyIndex(std::size_t offset) const noexcept {
    return (readyHead_ + offset) % config_.values().readyFifoCount;
  }

  std::size_t wrapPaddingFor(std::size_t pageCount) const noexcept {
    if (pageCount == 0 || pageCount > config_.values().pageCount) {
      return config_.values().pageCount;
    }
    return pageTail_ + pageCount > config_.values().pageCount
               ? config_.values().pageCount - pageTail_
               : 0;
  }

  bool validPageRequest(std::size_t pageCount) const noexcept {
    return pageCount != 0 &&
           pageCount <= config_.values().maxPagesPerSource &&
           pageCount <= config_.values().pageCount;
  }

  std::uint8_t pressureDimensions(std::size_t pageCount,
                                  PayloadKind payloadKind) const noexcept {
    const auto& values = config_.values();
    std::uint8_t dimensions = 0;
    if (residentCount_ >= values.highWaterSources) {
      dimensions |= kPressureSources;
    }
    if (payloadKind == PayloadKind::Legacy &&
        compatibilityResident_ >= values.compatibilityPayloadCount) {
      dimensions |= kPressureCompatibility;
    }
    if (readyPublicationReservations_ >= values.highWaterReady) {
      dimensions |= kPressureReady;
    }
    const std::size_t padding = wrapPaddingFor(pageCount);
    const std::size_t availablePages =
        values.highWaterPages -
        std::min(occupiedPages_, values.highWaterPages);
    if (padding > availablePages ||
        pageCount > availablePages - std::min(padding, availablePages)) {
      dimensions |= kPressurePages;
    }
    return dimensions;
  }

  std::uint8_t reachedDimensions(PayloadKind payloadKind) const noexcept {
    const auto& values = config_.values();
    std::uint8_t dimensions = 0;
    if (residentCount_ >= values.highWaterSources) {
      dimensions |= kPressureSources;
    }
    if (payloadKind == PayloadKind::Legacy &&
        compatibilityResident_ >= values.compatibilityPayloadCount) {
      dimensions |= kPressureCompatibility;
    }
    if (occupiedPages_ >= values.highWaterPages) {
      dimensions |= kPressurePages;
    }
    if (readyPublicationReservations_ >= values.highWaterReady) {
      dimensions |= kPressureReady;
    }
    return dimensions;
  }

  ReserveProbe inspectReserve(std::size_t pageCount,
                              PayloadKind payloadKind) const noexcept {
    if (!validPageRequest(pageCount)) {
      return ReserveProbe::InvalidRequest;
    }
    if (stopped_) {
      return ReserveProbe::Stopped;
    }
    std::uint8_t activePressure = closedPressureDimensions_;
    if (payloadKind == PayloadKind::Arena) {
      activePressure &= ~kPressureCompatibility;
    }
    if (activePressure != 0) {
      return ReserveProbe::TemporaryPressure;
    }

    const auto& values = config_.values();
    const std::size_t padding = wrapPaddingFor(pageCount);
    if (readyPublicationReservations_ >= values.highWaterReady ||
        residentCount_ >= values.highWaterSources ||
        (payloadKind == PayloadKind::Legacy &&
         compatibilityResident_ >= values.compatibilityPayloadCount) ||
        occupiedPages_ > values.highWaterPages) {
      return ReserveProbe::TemporaryPressure;
    }
    const std::size_t availablePages = values.highWaterPages - occupiedPages_;
    if (padding > availablePages ||
        pageCount > availablePages - padding) {
      return ReserveProbe::TemporaryPressure;
    }

    const std::size_t firstPage = padding != 0 ? 0 : pageTail_;
    if (entries_[sourceTail_].state != State::Free ||
        (payloadKind == PayloadKind::Legacy &&
         findFreeCompatibilityPayload() == kInvalidIndex) ||
        (payloadKind == PayloadKind::Arena &&
         arenaOwners_[sourceTail_].constructed) ||
        !allocationPagesFree(firstPage, pageCount, padding)) {
      return ReserveProbe::Corrupt;
    }
    return ReserveProbe::Ready;
  }

  std::optional<Reservation> reserveStorageImpl(
      std::size_t pageCount, PayloadKind payloadKind) noexcept {
    const std::size_t compatibilityIndex =
        payloadKind == PayloadKind::Legacy
            ? findFreeCompatibilityPayload()
            : kInvalidIndex;
    const std::size_t padding = wrapPaddingFor(pageCount);
    const std::size_t firstPage = padding != 0 ? 0 : pageTail_;
    auto& entry = entries_[sourceTail_];
    auto& arenaOwner = arenaOwners_[sourceTail_];
    if (entry.state != State::Free ||
        (payloadKind == PayloadKind::Legacy &&
         compatibilityIndex == kInvalidIndex) ||
        (payloadKind == PayloadKind::Arena && arenaOwner.constructed) ||
        !allocationPagesFree(firstPage, pageCount, padding)) {
      stopAdmission();
      return std::nullopt;
    }
    if (entry.generation == 0) {
      entry.generation = nextGeneration(entry.generation);
    }
    const CpuReadySourceId id{
        .index = static_cast<std::uint32_t>(sourceTail_),
        .generation = entry.generation,
    };
    const std::uint64_t pageGeneration =
        nextGeneration(nextPageAllocationGeneration_);
    nextPageAllocationGeneration_ = pageGeneration;
    const CpuReadyStorageRef storage{
        .firstPage = static_cast<std::uint32_t>(firstPage),
        .pageCount = static_cast<std::uint32_t>(pageCount),
        .generation = pageGeneration,
    };

    for (std::size_t i = 0; i < padding; ++i) {
      auto& page = pages_[pageTail_ + i];
      page.generation = pageGeneration;
      page.owner = id;
      page.allocated = true;
    }
    for (std::size_t i = 0; i < pageCount; ++i) {
      auto& page = pages_[firstPage + i];
      page.generation = pageGeneration;
      page.owner = id;
      page.allocated = true;
    }

    SourcePayloadBlock* compatibilityPayloadPtr = nullptr;
    ArenaSourcePayloadBlock* arenaPayloadPtr = nullptr;
    if (payloadKind == PayloadKind::Legacy) {
      compatibilityPayloadPtr = &compatibilityPayloads_[compatibilityIndex];
      compatibilityPayloadPtr->clearCommands();
      compatibilityPayloadPtr->seqId = 0;
      compatibilityOwners_[compatibilityIndex] = id;
      ++compatibilityResident_;
      ++compatibilityWritingCount_;
    } else {
      arenaPayloadPtr = std::construct_at(arenaOwner.storageAddress());
      arenaOwner.constructed = true;
    }

    entry.state = State::Writing;
    entry.storage = storage;
    entry.paddingFirstPage = pageTail_;
    entry.paddingBefore = padding;
    entry.usedBytes = 0;
    entry.plannedBytes = 0;
    entry.payloadKind = payloadKind;
    entry.compatibilityIndex = compatibilityIndex;
    entry.rawOrdinal = 0;
    entry.sourceOrdinal = 0;
    entry.seqId = 0;
    entry.buildGeneration = 0;
    entry.strictAdmission = false;
    entry.readyPublicationReserved = true;
    entry.arenaOwnerDetached = false;
    entry.arenaDetachKind = Entry::ArenaDetachKind::None;

    pageTail_ = (firstPage + pageCount) % config_.values().pageCount;
    occupiedPages_ += padding + pageCount;
    ++residentCount_;
    ++readyPublicationReservations_;
    stats_.residentSources = residentCount_;
    stats_.residentPages = occupiedPages_;
    stats_.compatibilityPayloads = compatibilityResident_;
    stats_.readyPublicationReservations = readyPublicationReservations_;
    stats_.wrapPaddingPages += padding;
    sourceTail_ = (sourceTail_ + 1) % capacity();

    // Reaching any high-water bound closes the latch immediately. Producers
    // call canReserve() before reserve(), so waiting for the next failed
    // reservation would otherwise bypass high/low hysteresis.
    closeAdmission(reachedDimensions(payloadKind));

    const CpuReadyPublicationTicket ticket{.id = id, .storage = storage};
    return Reservation{
        .id = id,
        .storage = storage,
        .ticket = ticket,
        .payloadKind = payloadKind,
        .payload = compatibilityPayloadPtr,
        .arenaPayload = arenaPayloadPtr,
    };
  }

  std::size_t findFreeCompatibilityPayload() const noexcept {
    const std::size_t count = config_.values().compatibilityPayloadCount;
    for (std::size_t offset = 0; offset < count; ++offset) {
      const std::size_t index = (compatibilityTail_ + offset) % count;
      if (!compatibilityOwners_[index].valid()) {
        return index;
      }
    }
    return kInvalidIndex;
  }

  Entry* resolveEntry(CpuReadySourceId id) noexcept {
    if (!id.valid() || id.index >= capacity()) {
      return nullptr;
    }
    auto& entry = entries_[id.index];
    return entry.generation == id.generation && entry.state != State::Free
               ? &entry
               : nullptr;
  }

  const Entry* resolveEntry(CpuReadySourceId id) const noexcept {
    if (!id.valid() || id.index >= capacity()) {
      return nullptr;
    }
    const auto& entry = entries_[id.index];
    return entry.generation == id.generation && entry.state != State::Free
               ? &entry
               : nullptr;
  }

  Entry* resolveEntry(CpuReadySourceId id,
                      CpuReadyStorageRef storage) noexcept {
    auto* entry = resolveEntry(id);
    return entry && entry->storage == storage && validatePages(id, storage)
               ? entry
               : nullptr;
  }

  const Entry* resolveEntry(CpuReadySourceId id,
                            CpuReadyStorageRef storage) const noexcept {
    const auto* entry = resolveEntry(id);
    return entry && entry->storage == storage && validatePages(id, storage)
               ? entry
               : nullptr;
  }

  bool validatePages(CpuReadySourceId id,
                     CpuReadyStorageRef storage) const noexcept {
    if (!storage.valid() || storage.firstPage >= config_.values().pageCount ||
        storage.pageCount >
            config_.values().pageCount - storage.firstPage) {
      return false;
    }
    for (std::size_t i = 0; i < storage.pageCount; ++i) {
      const auto& page = pages_[storage.firstPage + i];
      if (!page.allocated || page.owner != id ||
          page.generation != storage.generation) {
        return false;
      }
    }
    return true;
  }

  bool allocationPagesFree(std::size_t firstPage,
                           std::size_t pageCount,
                           std::size_t padding) const noexcept {
    if (firstPage > config_.values().pageCount ||
        pageCount > config_.values().pageCount - firstPage) {
      return false;
    }
    if (padding != 0) {
      if (padding > config_.values().pageCount - pageTail_) {
        return false;
      }
      for (std::size_t i = 0; i < padding; ++i) {
        if (pages_[pageTail_ + i].allocated) {
          return false;
        }
      }
    }
    for (std::size_t i = 0; i < pageCount; ++i) {
      if (pages_[firstPage + i].allocated) {
        return false;
      }
    }
    return true;
  }

  std::span<std::byte> storageSpan(CpuReadyStorageRef storage) noexcept {
    return std::span<std::byte>(
        pageArena_.get() +
            static_cast<std::size_t>(storage.firstPage) *
                config_.values().pageSize,
        static_cast<std::size_t>(storage.pageCount) *
            config_.values().pageSize);
  }

  std::span<const std::byte> storageSpan(
      CpuReadyStorageRef storage) const noexcept {
    return std::span<const std::byte>(
        pageArena_.get() +
            static_cast<std::size_t>(storage.firstPage) *
                config_.values().pageSize,
        static_cast<std::size_t>(storage.pageCount) *
            config_.values().pageSize);
  }

  SourcePayloadBlock* compatibilityPayload(Entry& entry) noexcept {
    if (entry.payloadKind != PayloadKind::Legacy ||
        entry.compatibilityIndex >=
        config_.values().compatibilityPayloadCount) {
      return nullptr;
    }
    return &compatibilityPayloads_[entry.compatibilityIndex];
  }

  const SourcePayloadBlock* compatibilityPayload(
      const Entry& entry) const noexcept {
    if (entry.payloadKind != PayloadKind::Legacy ||
        entry.compatibilityIndex >=
        config_.values().compatibilityPayloadCount) {
      return nullptr;
    }
    return &compatibilityPayloads_[entry.compatibilityIndex];
  }

  ArenaSourcePayloadBlock* arenaPayload(Entry& entry) noexcept {
    const std::size_t index = static_cast<std::size_t>(&entry - entries_.get());
    if (entry.payloadKind != PayloadKind::Arena || index >= capacity() ||
        !arenaOwners_[index].constructed || entry.arenaOwnerDetached) {
      return nullptr;
    }
    return arenaOwners_[index].payload();
  }

  const ArenaSourcePayloadBlock* arenaPayload(const Entry& entry) const noexcept {
    const std::size_t index = static_cast<std::size_t>(&entry - entries_.get());
    if (entry.payloadKind != PayloadKind::Arena || index >= capacity() ||
        !arenaOwners_[index].constructed || entry.arenaOwnerDetached) {
      return nullptr;
    }
    return arenaOwners_[index].payload();
  }

  SourcePayloadBlock* resolvePayload(Entry* entry,
                                     State expected) noexcept {
    if (!entry || entry->state != expected || expected == State::Writing ||
        expected == State::Sealed || expected == State::Reclaiming) {
      noteStaleReject();
      return nullptr;
    }
    return compatibilityPayload(*entry);
  }

  const SourcePayloadBlock* resolvePayload(const Entry* entry,
                                           State expected) const noexcept {
    if (!entry || entry->state != expected || expected == State::Writing ||
        expected == State::Sealed || expected == State::Reclaiming) {
      noteStaleReject();
      return nullptr;
    }
    return compatibilityPayload(*entry);
  }

  ArenaSourcePayloadBlock* resolveArenaPayload(
      Entry* entry, State expected) noexcept {
    if (!entry || entry->state != expected || expected == State::Writing ||
        expected == State::Sealed || expected == State::Reclaiming) {
      noteStaleReject();
      return nullptr;
    }
    auto* payload = arenaPayload(*entry);
    if (!payload || !payload->readable()) {
      noteStaleReject();
      return nullptr;
    }
    return payload;
  }

  const ArenaSourcePayloadBlock* resolveArenaPayload(
      const Entry* entry, State expected) const noexcept {
    if (!entry || entry->state != expected || expected == State::Writing ||
        expected == State::Sealed || expected == State::Reclaiming) {
      noteStaleReject();
      return nullptr;
    }
    const auto* payload = arenaPayload(*entry);
    if (!payload || !payload->readable()) {
      noteStaleReject();
      return nullptr;
    }
    return payload;
  }

  void closeAdmission(std::uint8_t dimensions) noexcept {
    if (dimensions == 0) {
      return;
    }
    const bool wasClosed = stats_.admissionClosed;
    closedPressureDimensions_ |= dimensions;
    stats_.admissionClosed = true;
    if (!wasClosed) {
      ++stats_.admissionCloses;
    }
  }

  void refreshAdmissionAfterRelease() noexcept {
    if (stopped_) {
      stats_.admissionClosed = true;
      return;
    }
    const bool wasClosed = stats_.admissionClosed;
    if (residentCount_ <= config_.values().lowWaterSources) {
      closedPressureDimensions_ &= ~kPressureSources;
    }
    if (compatibilityResident_ <
        config_.values().compatibilityPayloadCount) {
      closedPressureDimensions_ &= ~kPressureCompatibility;
    }
    if (occupiedPages_ <= config_.values().lowWaterPages) {
      closedPressureDimensions_ &= ~kPressurePages;
    }
    if (readyPublicationReservations_ <=
        config_.values().lowWaterReady) {
      closedPressureDimensions_ &= ~kPressureReady;
    }
    stats_.admissionClosed = closedPressureDimensions_ != 0;
    if (wasClosed && !stats_.admissionClosed) {
      ++stats_.admissionReopens;
    }
  }

  void noteStaleReject() const noexcept { ++stats_.staleRejects; }

  void rollbackNewest(Entry& entry) noexcept {
    const auto storage = entry.storage;
    for (std::size_t i = 0; i < entry.paddingBefore; ++i) {
      auto& page = pages_[entry.paddingFirstPage + i];
      page.allocated = false;
      page.owner = {};
      page.generation = nextGeneration(page.generation);
    }
    for (std::size_t i = 0; i < storage.pageCount; ++i) {
      auto& page = pages_[storage.firstPage + i];
      page.allocated = false;
      page.owner = {};
      page.generation = nextGeneration(page.generation);
    }
    if (entry.payloadKind == PayloadKind::Legacy) {
      compatibilityOwners_[entry.compatibilityIndex] = {};
      compatibilityTail_ = entry.compatibilityIndex;
      --compatibilityResident_;
    }
    pageTail_ = (storage.firstPage + config_.values().pageCount -
                 entry.paddingBefore) %
                config_.values().pageCount;
    occupiedPages_ -= entry.paddingBefore + storage.pageCount;
    sourceTail_ = entry.storage.valid() ? previousSourceIndex(sourceTail_)
                                        : sourceTail_;
    --residentCount_;
    entry = Entry{.generation = nextGeneration(entry.generation)};
    normalizeEmptyAllocator();
    stats_.residentSources = residentCount_;
    stats_.residentPages = occupiedPages_;
    stats_.compatibilityPayloads = compatibilityResident_;
  }

  void releaseOldest(Entry& entry) noexcept {
    const auto storage = entry.storage;
    for (std::size_t i = 0; i < entry.paddingBefore; ++i) {
      auto& page = pages_[entry.paddingFirstPage + i];
      page.allocated = false;
      page.owner = {};
      page.generation = nextGeneration(page.generation);
    }
    for (std::size_t i = 0; i < storage.pageCount; ++i) {
      auto& page = pages_[storage.firstPage + i];
      page.allocated = false;
      page.owner = {};
      page.generation = nextGeneration(page.generation);
    }
    if (entry.payloadKind == PayloadKind::Legacy) {
      compatibilityOwners_[entry.compatibilityIndex] = {};
      compatibilityTail_ =
          (entry.compatibilityIndex + 1) %
          config_.values().compatibilityPayloadCount;
      --compatibilityResident_;
    }
    occupiedPages_ -= entry.paddingBefore + storage.pageCount;
    --residentCount_;
    entry = Entry{.generation = nextGeneration(entry.generation)};
    sourceHead_ = (sourceHead_ + 1) % capacity();
    normalizeEmptyAllocator();
    stats_.residentSources = residentCount_;
    stats_.residentPages = occupiedPages_;
    stats_.compatibilityPayloads = compatibilityResident_;
  }

  void normalizeEmptyAllocator() noexcept {
    if (residentCount_ != 0) {
      return;
    }
    sourceHead_ = 0;
    sourceTail_ = 0;
    pageTail_ = 0;
    if (compatibilityResident_ == 0) {
      compatibilityTail_ = 0;
    }
  }

  const CpuReadyTapeConfig config_;
  std::unique_ptr<Entry[]> entries_{};
  std::unique_ptr<ReadyEntry[]> readyFifo_{};
  std::unique_ptr<Page[]> pages_{};
  CpuReadyAlignedBytes pageArena_{};
  std::unique_ptr<CpuReadyArenaOwnerSlot[]> arenaOwners_{};
  std::unique_ptr<SourcePayloadBlock[]> compatibilityPayloads_{};
  std::unique_ptr<CpuReadySourceId[]> compatibilityOwners_{};
  std::size_t sourceHead_ = 0;
  std::size_t sourceTail_ = 0;
  std::size_t readyHead_ = 0;
  std::size_t readyTail_ = 0;
  std::size_t readyCount_ = 0;
  std::size_t pageTail_ = 0;
  std::size_t compatibilityTail_ = 0;
  std::size_t residentCount_ = 0;
  std::size_t occupiedPages_ = 0;
  std::size_t compatibilityResident_ = 0;
  std::size_t compatibilityWritingCount_ = 0;
  std::size_t readyPublicationReservations_ = 0;
  std::uint64_t nextPageAllocationGeneration_ = 0;
  std::uint64_t rawOrdinalHighWater_ = 0;
  std::uint64_t sourceOrdinalHighWater_ = 0;
  std::uint64_t seqIdHighWater_ = 0;
  std::uint8_t closedPressureDimensions_ = 0;
  bool strictWritingActive_ = false;
  bool stopped_ = false;
  mutable Stats stats_{};
};

}  // namespace dxmt9::core
