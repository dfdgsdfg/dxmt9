#pragma once

#include "dxmt9_backend_types.hpp"
#include "dxmt9_chunk_slot_capacity.hpp"
#include "dxmt9_source_payload.hpp"
#include "dxmt9_source_semantics.hpp"

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
        values.sourceSlotCount >
            std::numeric_limits<std::size_t>::max() /
                kMaxArenaSourcePayloadSegments ||
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

  static CpuReadyTapeConfig queueSessionStreaming(
      std::size_t controlCapacity) {
    constexpr std::size_t kPageCapacityMultiplier = 20;
    constexpr std::size_t kOrdinaryDirectPages = 64;
    if (controlCapacity >
        std::numeric_limits<std::size_t>::max() /
            kPageCapacityMultiplier) {
      throw std::invalid_argument(
          "CpuReadyTape session page capacity overflow");
    }
    const auto config = create(Values{
        .pageSize = 4096,
        .pageCount = controlCapacity * kPageCapacityMultiplier,
        .sourceSlotCount = controlCapacity * 2,
        .readyFifoCount = controlCapacity,
        .compatibilityPayloadCount = controlCapacity * 2,
        // Sources beyond the ordinary Direct footprint take the deterministic
        // ordered legacy rollback disposition. Keeping the physical arena
        // larger reserves a fixed session prefix plus one non-wrapping
        // successor (payload + worst-case circular tail padding).
        .maxPagesPerSource = std::min(
            kOrdinaryDirectPages,
            controlCapacity * kPageCapacityMultiplier),
        .highWaterSources = controlCapacity * 2,
        .lowWaterSources = controlCapacity,
        .highWaterPages = controlCapacity * kPageCapacityMultiplier,
        .lowWaterPages = controlCapacity * (kPageCapacityMultiplier / 2),
        .highWaterReady = controlCapacity,
        .lowWaterReady = controlCapacity / 2,
    });
    if (!config) {
      throw std::invalid_argument(
          "invalid CpuReadyTape session streaming capacity");
    }
    return *config;
  }

  static CpuReadyTapeConfig queueCaptureStreaming(
      std::size_t controlCapacity) {
    constexpr std::size_t kPageCapacityMultiplier = 64;
    constexpr std::size_t kCapturePagesPerSource = 512;
    if (controlCapacity >
        std::numeric_limits<std::size_t>::max() /
            kPageCapacityMultiplier) {
      throw std::invalid_argument(
          "CpuReadyTape capture page capacity overflow");
    }
    const std::size_t pageCount =
        controlCapacity * kPageCapacityMultiplier;
    const auto config = create(Values{
        .pageSize = 4096,
        .pageCount = pageCount,
        .sourceSlotCount = controlCapacity * 2,
        .readyFifoCount = controlCapacity,
        .compatibilityPayloadCount = controlCapacity * 2,
        // SegmentSerial's 64-page source grouping is a call-local planner
        // bound for authenticated capture events. Queue admission remains
        // the established 512-page/source contract for every capture and
        // identity mode, including startup/non-capture records.
        .maxPagesPerSource = std::min(kCapturePagesPerSource, pageCount),
        .highWaterSources = controlCapacity * 2,
        .lowWaterSources = controlCapacity,
        .highWaterPages = pageCount,
        .lowWaterPages = pageCount / 2,
        .highWaterReady = controlCapacity,
        .lowWaterReady = controlCapacity / 2,
    });
    if (!config) {
      throw std::invalid_argument(
          "invalid CpuReadyTape capture streaming capacity");
    }
    return *config;
  }

  const Values& values() const noexcept { return values_; }

 private:
  explicit constexpr CpuReadyTapeConfig(Values values) noexcept
      : values_(values) {}

  Values values_{};
};

struct CpuReadyProducerIdentity {
  std::uint64_t firstEventOrdinal = 0;
  std::uint64_t lastEventOrdinal = 0;
  std::uint64_t firstSourceOrdinal = 0;
  std::uint64_t lastSourceOrdinal = 0;

  constexpr bool absent() const noexcept {
    return firstEventOrdinal == 0u && lastEventOrdinal == 0u &&
        firstSourceOrdinal == 0u && lastSourceOrdinal == 0u;
  }
  constexpr bool valid() const noexcept {
    return firstEventOrdinal != 0u &&
        lastEventOrdinal >= firstEventOrdinal && firstSourceOrdinal != 0u &&
        lastSourceOrdinal >= firstSourceOrdinal;
  }
  constexpr bool importable() const noexcept { return absent() || valid(); }

  friend constexpr bool operator==(CpuReadyProducerIdentity,
                                   CpuReadyProducerIdentity) noexcept = default;
};

// An open compatibility source may represent several PE semantic batches only
// when their closed producer intervals are exactly adjacent. Keep this check
// value-only so Direct admission and the Tape commit guard cannot drift.
constexpr bool compatibilityProducerIdentityAppendable(
    CpuReadyProducerIdentity current,
    CpuReadyProducerIdentity next) noexcept {
  if (!next.importable()) return false;
  if (next.absent() || current.absent()) return true;
  return current.valid() &&
      current.lastEventOrdinal != std::numeric_limits<std::uint64_t>::max() &&
      current.lastSourceOrdinal != std::numeric_limits<std::uint64_t>::max() &&
      next.firstEventOrdinal == current.lastEventOrdinal + 1u &&
      next.firstSourceOrdinal == current.lastSourceOrdinal + 1u;
}

// One raw may own several consecutive final-slot lease spans when an ordered
// control or a compatibility range cuts it. Every span of that raw presents
// the SAME closed producer interval, so the cross-raw adjacency rule above --
// which demands a strict +1 in both dimensions -- can never describe them,
// and must not be weakened to try: it is what makes a *different* raw
// silently appending onto a populated slot impossible.
//
// Keep the two questions separate and typed. The witness below is the exact
// active-raw identity of the slot: which raw is currently extending it, how
// far its span sequence has got, and whether that sequence has settled. A
// same-raw continuation is admitted only for the immediate successor span of
// the exact witnessed raw, before settlement, carrying the identical closed
// interval. Duplicate, skipped, out-of-order and post-settlement spans are
// each rejected with their own reason.
struct CpuReadySpanWitness {
  // The raw whose spans are extending this slot. Zero means no span sequence
  // is active, so the cross-raw predicate is the only authority.
  std::uint64_t rawOrdinal = 0;
  // Highest span ordinal of that raw already committed into this slot.
  std::uint32_t lastSpanOrdinal = 0;
  // The raw published its final span; nothing further may extend it.
  bool settled = false;

  constexpr bool active() const noexcept { return rawOrdinal != 0u; }
  friend constexpr bool operator==(CpuReadySpanWitness,
                                   CpuReadySpanWitness) noexcept = default;
};

enum class CpuReadySpanAdmission : std::uint8_t {
  // No active witness, or a different raw. The unchanged cross-raw adjacency
  // predicate decides, exactly as before span identity existed.
  CrossRaw = 0,
  // The exact witnessed raw is extending its own prefix with the immediate
  // successor span.
  SameRawSpan,
  // The witnessed raw re-presented a span ordinal it already committed.
  DuplicateSpan,
  // The witnessed raw skipped at least one span ordinal.
  SkippedSpan,
  // The witnessed raw already committed its final span.
  SettledRaw,
  // The witnessed raw presented a different closed producer interval.
  IdentityMismatch,
};

constexpr CpuReadySpanAdmission compatibilitySpanAdmission(
    CpuReadySpanWitness witness, CpuReadyProducerIdentity witnessIdentity,
    std::uint64_t rawOrdinal, std::uint32_t spanOrdinal,
    CpuReadyProducerIdentity identity) noexcept {
  if (!witness.active() || rawOrdinal == 0u ||
      witness.rawOrdinal != rawOrdinal) {
    return CpuReadySpanAdmission::CrossRaw;
  }
  if (witness.settled) {
    return CpuReadySpanAdmission::SettledRaw;
  }
  // A raw carries one closed producer interval, so every span of it presents
  // the identical value. Exact equality, not adjacency, is the same-raw rule.
  if (witnessIdentity != identity) {
    return CpuReadySpanAdmission::IdentityMismatch;
  }
  if (spanOrdinal <= witness.lastSpanOrdinal) {
    return CpuReadySpanAdmission::DuplicateSpan;
  }
  if (witness.lastSpanOrdinal == std::numeric_limits<std::uint32_t>::max() ||
      spanOrdinal != witness.lastSpanOrdinal + 1u) {
    return CpuReadySpanAdmission::SkippedSpan;
  }
  return CpuReadySpanAdmission::SameRawSpan;
}

// The cross-raw rule is untouched by span identity: a span sequence never
// widens which *other* raw may extend a populated slot.
static_assert(compatibilitySpanAdmission(
                  CpuReadySpanWitness{}, CpuReadyProducerIdentity{}, 0u, 0u,
                  CpuReadyProducerIdentity{}) ==
              CpuReadySpanAdmission::CrossRaw);
static_assert(
    compatibilitySpanAdmission(
        CpuReadySpanWitness{.rawOrdinal = 5u, .lastSpanOrdinal = 0u},
        CpuReadyProducerIdentity{7u, 7u, 11u, 11u}, 6u, 1u,
        CpuReadyProducerIdentity{8u, 8u, 12u, 12u}) ==
    CpuReadySpanAdmission::CrossRaw);
static_assert(
    compatibilitySpanAdmission(
        CpuReadySpanWitness{.rawOrdinal = 5u, .lastSpanOrdinal = 0u},
        CpuReadyProducerIdentity{7u, 7u, 11u, 11u}, 5u, 1u,
        CpuReadyProducerIdentity{7u, 7u, 11u, 11u}) ==
    CpuReadySpanAdmission::SameRawSpan);
static_assert(
    compatibilitySpanAdmission(
        CpuReadySpanWitness{.rawOrdinal = 5u, .lastSpanOrdinal = 1u},
        CpuReadyProducerIdentity{7u, 7u, 11u, 11u}, 5u, 1u,
        CpuReadyProducerIdentity{7u, 7u, 11u, 11u}) ==
    CpuReadySpanAdmission::DuplicateSpan);
static_assert(
    compatibilitySpanAdmission(
        CpuReadySpanWitness{.rawOrdinal = 5u, .lastSpanOrdinal = 1u},
        CpuReadyProducerIdentity{7u, 7u, 11u, 11u}, 5u, 3u,
        CpuReadyProducerIdentity{7u, 7u, 11u, 11u}) ==
    CpuReadySpanAdmission::SkippedSpan);
static_assert(
    compatibilitySpanAdmission(
        CpuReadySpanWitness{
            .rawOrdinal = 5u, .lastSpanOrdinal = 1u, .settled = true},
        CpuReadyProducerIdentity{7u, 7u, 11u, 11u}, 5u, 2u,
        CpuReadyProducerIdentity{7u, 7u, 11u, 11u}) ==
    CpuReadySpanAdmission::SettledRaw);
static_assert(
    compatibilitySpanAdmission(
        CpuReadySpanWitness{.rawOrdinal = 5u, .lastSpanOrdinal = 0u},
        CpuReadyProducerIdentity{7u, 7u, 11u, 11u}, 5u, 1u,
        CpuReadyProducerIdentity{7u, 9u, 11u, 13u}) ==
    CpuReadySpanAdmission::IdentityMismatch);
// A same-raw span is exactly the case the cross-raw predicate refuses, which
// is why it needs its own witnessed rule rather than a relaxation.
static_assert(!compatibilityProducerIdentityAppendable(
    CpuReadyProducerIdentity{7u, 7u, 11u, 11u},
    CpuReadyProducerIdentity{7u, 7u, 11u, 11u}));

struct CpuReadyPublicationTicket {
  CpuReadySourceId id{};
  CpuReadyStorageRef storage{};
  std::uint64_t rawOrdinal = 0;
  std::uint64_t sourceOrdinal = 0;
  std::uint64_t seqId = 0;
  std::uint64_t buildGeneration = 0;
  CpuReadyProducerIdentity producerIdentity{};

  constexpr bool valid() const noexcept {
    return id.valid() && storage.valid();
  }

  constexpr bool hasAdmissionIdentity() const noexcept {
    return rawOrdinal != 0 || sourceOrdinal != 0 || seqId != 0 ||
           buildGeneration != 0 || !producerIdentity.absent();
  }

  constexpr bool strictIdentityValid() const noexcept {
    return rawOrdinal != 0 && sourceOrdinal != 0 && seqId != 0 &&
           buildGeneration != 0 && producerIdentity.importable();
  }

  friend constexpr bool operator==(CpuReadyPublicationTicket,
                                   CpuReadyPublicationTicket) noexcept = default;
};

struct CpuReadyAdmissionIdentity {
  std::uint64_t rawOrdinal = 0;
  std::uint64_t sourceOrdinal = 0;
  std::uint64_t seqId = 0;
  std::uint64_t buildGeneration = 0;
  CpuReadyProducerIdentity producerIdentity{};

  constexpr bool valid() const noexcept {
    return rawOrdinal != 0 && sourceOrdinal != 0 && seqId != 0 &&
           buildGeneration != 0 && producerIdentity.importable();
  }
};

// Immutable scheduling facts fixed by source admission/publication.  The
// source/storage generations remain the authority for resolving this value;
// consumers must revalidate the metadata against the live Tape entry before
// using it to make an ordering or capacity decision.
struct CpuReadySourceMetadata {
  std::uint64_t rawOrdinal = 0;
  std::uint64_t sourceOrdinal = 0;
  std::uint64_t seqId = 0;
  std::uint64_t buildGeneration = 0;
  // Exact planned storage extent for Arena sources. Legacy callers may supply
  // an explicit page-backed extent; the production default is the immutable
  // payload's deterministic logical replay footprint.
  std::size_t usedBytes = 0;
  std::uint32_t pageCount = 0;
  // Physical circular-arena pages owned by this source. This includes the
  // discarded tail immediately preceding a wrapped non-contiguous request.
  std::uint32_t paddingPagesBefore = 0;
  bool strictAdmission = false;

  constexpr bool valid() const noexcept {
    return sourceOrdinal != 0 && seqId != 0 && pageCount != 0;
  }

  friend constexpr bool operator==(CpuReadySourceMetadata,
                                   CpuReadySourceMetadata) noexcept = default;
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
    return state == State::Retiring || !payload || payload->commandsEmpty();
  }

  std::size_t commandCount() const noexcept {
    return state == State::Retiring || !payload ? 0 : payload->commandCount();
  }
};

// Fixed source descriptors, a circular non-wrapping page allocator, and a
// separately bounded compatibility payload lane. Every method is called under
// the owning queue's scheduling mutex unless explicitly documented otherwise.
class CpuReadyTape {
 private:
  enum class GroupCompletionStatus : std::uint8_t {
    Invalid,
    Pending,
    Complete,
  };

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
    TentativeRepresented,
    Represented,
    Encoding = Represented,
    Submitted,
    GPU = Submitted,
    Completed,
    Reclaiming,
  };

  enum class ReclaimDisposition : std::uint8_t {
    Invalid,
    AwaitingGroupCompletion,
    Ready,
  };

  struct ReclaimStatus {
    ReclaimDisposition disposition = ReclaimDisposition::Invalid;
    std::uint64_t groupTailSeqId = 0;
    std::uint32_t groupSourceCount = 0;
    std::uint32_t groupSourceIndex = 0;

    bool grouped() const noexcept { return groupSourceCount > 1u; }
  };

  struct Reservation {
    CpuReadySourceId id{};
    CpuReadyStorageRef storage{};
    CpuReadyPublicationTicket ticket{};
    PayloadKind payloadKind = PayloadKind::Legacy;
    SourcePayloadBlock* payload = nullptr;
    ArenaSourcePayloadBlock* arenaPayload = nullptr;
    std::array<ArenaSourcePayloadBlock*,
               kMaxArenaSourcePayloadSegments> arenaPayloads{};
    std::size_t arenaPayloadCount = 0;

    explicit operator bool() const noexcept {
      return ticket.valid() &&
             (payloadKind == PayloadKind::Legacy ? payload != nullptr
                                                 : arenaPayload != nullptr &&
                                                       arenaPayloadCount != 0);
    }
  };

  // A bounded admission transaction.  Reservations remain Writing until the
  // caller seals the complete batch; no Ready entry is exposed per source.
  static constexpr std::size_t kMaxArenaBatchSources = 8;
  struct ArenaBatchReservation {
    std::array<Reservation, kMaxArenaBatchSources> reservations{};
    std::size_t count = 0;
    std::uint64_t rawHighWaterBefore = 0;
    std::uint64_t sourceHighWaterBefore = 0;
    std::uint64_t seqHighWaterBefore = 0;
    std::size_t sourceTailBefore = 0;
    std::size_t pageTailBefore = 0;
    std::size_t residentCountBefore = 0;
    std::size_t occupiedPagesBefore = 0;
    std::size_t readyCountBefore = 0;
    std::size_t readyReservationsBefore = 0;

    bool valid() const noexcept {
      return count != 0 && count <= reservations.size();
    }
  };

  // Value-owned settlement emitted exactly once when the final member of a
  // strict SegmentSerial group reaches Completed.  This is intentionally
  // independent of Present: non-Present raw events still get a durable
  // event-tail status while Present waiters continue using their own queue.
  struct ArenaGroupSettlement {
    std::uint64_t rawOrdinal = 0;
    std::uint64_t buildGeneration = 0;
    std::uint64_t firstSourceOrdinal = 0;
    std::uint64_t tailSeqId = 0;
    std::uint32_t sourceCount = 0;
    bool hasPresent = false;

    bool valid() const noexcept {
      return rawOrdinal != 0 && buildGeneration != 0 &&
             firstSourceOrdinal != 0 && tailSeqId != 0 && sourceCount != 0;
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
    CpuReadySourceMetadata metadata{};
    SourceSemanticSummary semantic{};

    constexpr bool valid() const noexcept {
      return source.valid() &&
             controlIndex != std::numeric_limits<std::size_t>::max() &&
             seqId != 0 && metadata.valid() && metadata.seqId == seqId &&
             metadata.pageCount == source.storage.pageCount &&
             semantic.sealed() && semantic.byteCount == metadata.usedBytes &&
             semantic.pageCount == metadata.pageCount;
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
        : payloads_(other.payloads_), count_(other.count_),
          destroyed_(other.destroyed_) {
      other.payloads_ = {};
      other.count_ = 0;
      other.destroyed_ = false;
    }

    DetachedArenaOwner& operator=(DetachedArenaOwner&&) = delete;

    explicit operator bool() const noexcept { return count_ != 0; }
    bool destroyed() const noexcept { return destroyed_; }

    void destroy() noexcept {
      if (count_ == 0 || destroyed_) {
        return;
      }
      for (std::size_t i = count_; i != 0; --i) {
        payloads_[i - 1]->destroyConstructed();
        std::destroy_at(payloads_[i - 1]);
      }
      destroyed_ = true;
    }

   private:
    friend class CpuReadyTape;

    explicit DetachedArenaOwner(
        std::span<ArenaSourcePayloadBlock* const> payloads) noexcept
        : count_(payloads.size()) {
      std::copy(payloads.begin(), payloads.end(), payloads_.begin());
    }

    std::array<ArenaSourcePayloadBlock*,
               kMaxArenaSourcePayloadSegments> payloads_{};
    std::size_t count_ = 0;
    bool destroyed_ = false;
  };

  enum class ReserveProbe {
    Ready,
    TemporaryPressure,
    InvalidRequest,
    Stopped,
    Corrupt,
  };

  enum class ArenaBatchReserveFailure : std::uint8_t {
    None,
    Recoverable,
    TemporaryPressure,
    Invalid,
    Stopped,
    Corrupt,
  };

  static bool checkedExclusiveSeqTail(std::uint64_t firstSeqId,
                                      std::size_t count,
                                      std::uint64_t& exclusiveTail) noexcept {
    static_assert(std::numeric_limits<std::size_t>::digits <=
                  std::numeric_limits<std::uint64_t>::digits);
    const auto count64 = static_cast<std::uint64_t>(count);
    if (firstSeqId > std::numeric_limits<std::uint64_t>::max() - count64) {
      return false;
    }
    exclusiveTail = firstSeqId + count64;
    return true;
  }

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

  struct LeaseCapacityClaim {
    std::uint64_t sources = 0;
    std::uint64_t pages = 0;
    std::uint64_t bytes = 0;
    std::uint64_t payloadBlocks = 0;
    std::uint64_t readyEntries = 0;
    std::uint64_t retentionEntries = 0;
    std::uint64_t allocatorTickets = 0;

    friend constexpr bool operator==(const LeaseCapacityClaim&,
                                     const LeaseCapacityClaim&) = default;
  };

  struct OrderedTailWritingCapacity {
    SourceRef source{};
    LeaseCapacityClaim claim{};

    constexpr bool valid() const noexcept { return source.valid(); }
  };

  // Physical residency observed before the first session lease acquisition.
  // Ready sources are excluded because the lease charges them as they join.
  // Exactly one structurally valid ordered-tail Writing publication may be
  // separated so the session coordinator can prove it is already covered by
  // successorHeadroom. A malformed or non-unique Writing observation makes
  // the snapshot invalid; every non-Writing, non-Ready state remains
  // olderUnavailable.
  struct LeaseAcquisitionCapacitySnapshot {
    LeaseCapacityClaim olderUnavailable{};
    std::optional<OrderedTailWritingCapacity>
        orderedTailWritingSuccessor{};
    bool valid = true;
  };

  explicit CpuReadyTape(std::size_t capacity)
      : CpuReadyTape(CpuReadyTapeConfig::compatibility(capacity)) {}

  explicit CpuReadyTape(CpuReadyTapeConfig config)
      : config_(config),
        entries_(std::make_unique<Entry[]>(config.values().sourceSlotCount)),
        readyFifo_(std::make_unique<ReadyEntry[]>(
            config.values().readyFifoCount)),
        tentativeReadyPrefix_(std::make_unique<ReadyEntry[]>(
            config.values().readyFifoCount)),
        pages_(std::make_unique<Page[]>(config.values().pageCount)),
        pageArena_(allocateCpuReadyAlignedBytes(
            config.values().pageSize * config.values().pageCount)),
        arenaOwners_(std::make_unique<CpuReadyArenaOwnerSlot[]>(
            config.values().sourceSlotCount *
            kMaxArenaSourcePayloadSegments)),
        compatibilityPayloads_(std::make_unique<SourcePayloadBlock[]>(
            config.values().compatibilityPayloadCount)),
        compatibilityOwners_(std::make_unique<CpuReadySourceId[]>(
            config.values().compatibilityPayloadCount)) {}

  ~CpuReadyTape() {
    for (std::size_t i = 0; i < capacity(); ++i) {
      for (std::size_t segment = entries_[i].arenaPayloadCount;
           segment != 0; --segment) {
        auto& owner = arenaOwner(i, segment - 1);
        if (owner.constructed && !entries_[i].arenaOwnerDetached) {
          owner.payload()->destroyConstructed();
          std::destroy_at(owner.payload());
          owner.constructed = false;
        }
      }
    }
  }

  CpuReadyTape(const CpuReadyTape&) = delete;
  CpuReadyTape& operator=(const CpuReadyTape&) = delete;

  const CpuReadyTapeConfig& config() const noexcept { return config_; }
  std::size_t capacity() const noexcept {
    return config_.values().sourceSlotCount;
  }
  std::size_t compatibilityPayloadCount() const noexcept {
    return config_.values().compatibilityPayloadCount;
  }
  // Scalar snapshot only: callers cannot borrow or race a payload's storage.
  // This is intentionally owned by the tape because compatibility payloads
  // persist across source/control reuse.
  std::optional<std::uint64_t> compatibilityPayloadRetainedBytes(
      std::size_t index) const noexcept {
    if (index >= config_.values().compatibilityPayloadCount) {
      return std::nullopt;
    }
    return static_cast<std::uint64_t>(
        chunkSlotPhysicalRetainedBytes(compatibilityPayloads_[index]));
  }
  std::optional<std::size_t> compatibilityPayloadIndex(
      SourceRef source) const noexcept {
    const auto* entry = resolveEntry(source.id, source.storage);
    if (!entry || entry->payloadKind != PayloadKind::Legacy ||
        entry->compatibilityIndex >=
            config_.values().compatibilityPayloadCount) {
      return std::nullopt;
    }
    return entry->compatibilityIndex;
  }
  std::size_t residentCount() const noexcept { return residentCount_; }
  std::size_t readyCount() const noexcept { return readyCount_; }
  bool readyEmpty() const noexcept { return readyCount_ == 0; }
  const Stats& stats() const noexcept { return stats_; }
  ArenaBatchReserveFailure lastArenaBatchReserveFailure() const noexcept {
    return lastArenaBatchReserveFailure_;
  }

  LeaseAcquisitionCapacitySnapshot leaseAcquisitionCapacitySnapshot()
      const noexcept {
    LeaseAcquisitionCapacitySnapshot result{};
    std::size_t writingCount = 0;
    std::size_t writingIndex = kInvalidIndex;
    LeaseCapacityClaim writingClaim{};
    std::array<std::size_t, kMaxArenaBatchSources> writingIndices{};
    std::array<LeaseCapacityClaim, kMaxArenaBatchSources> writingClaims{};
    for (std::size_t i = 0; i < capacity(); ++i) {
      const auto& entry = entries_[i];
      if (entry.state == State::Free || entry.state == State::Ready) {
        continue;
      }
      LeaseCapacityClaim claim{};
      if (!leaseCapacityClaimFor(entry, claim)) {
        result.valid = false;
        return result;
      }
      if (entry.state == State::Writing) {
        ++writingCount;
        if (writingCount > writingIndices.size()) {
          result.valid = false;
          return result;
        }
        writingIndices[writingCount - 1u] = i;
        writingClaims[writingCount - 1u] = claim;
        continue;
      }
      if (!addLeaseCapacityClaim(result.olderUnavailable, claim)) {
        result.valid = false;
        return result;
      }
    }
    if (writingCount == 1) {
      writingIndex = writingIndices[0];
      writingClaim = writingClaims[0];
      const auto& entry = entries_[writingIndex];
      if (!orderedTailWritingEntryValid(writingIndex, entry)) {
        result.valid = false;
        return result;
      }
      result.orderedTailWritingSuccessor = OrderedTailWritingCapacity{
          .source = SourceRef{
              .id = CpuReadySourceId{
                  .index = static_cast<std::uint32_t>(writingIndex),
                  .generation = entry.generation,
              },
              .storage = entry.storage,
          },
          .claim = writingClaim,
      };
    } else if (writingCount > 1) {
      // SegmentSerial admission pins several strict Writing entries before
      // any Ready publication.  They are unavailable to encode as a group,
      // but must not make the historical exactly-one Writing snapshot
      // spuriously invalid.
      std::array<std::size_t, kMaxArenaBatchSources> orderedIndices{};
      std::array<LeaseCapacityClaim, kMaxArenaBatchSources> orderedClaims{};
      for (std::size_t i = 0; i < writingCount; ++i) {
        const auto expectedIndex =
            (sourceTail_ + capacity() - writingCount + i) % capacity();
        std::size_t found = writingCount;
        for (std::size_t candidate = 0; candidate < writingCount;
             ++candidate) {
          if (writingIndices[candidate] == expectedIndex) {
            found = candidate;
            break;
          }
        }
        if (found == writingCount) {
          result.valid = false;
          return result;
        }
        orderedIndices[i] = writingIndices[found];
        orderedClaims[i] = writingClaims[found];
      }
      const auto& firstWriting = entries_[orderedIndices[0]];
      for (std::size_t i = 0; i < writingCount; ++i) {
        const auto index = orderedIndices[i];
        const auto& entry = entries_[index];
        if (!entry.strictAdmission ||
            entry.rawOrdinal != firstWriting.rawOrdinal ||
            entry.buildGeneration !=
                firstWriting.buildGeneration ||
            entry.sourceOrdinal !=
                firstWriting.sourceOrdinal + i ||
            entry.seqId != firstWriting.seqId + i ||
            !entry.readyPublicationReserved) {
          result.valid = false;
          return result;
        }
        if (!addLeaseCapacityClaim(result.olderUnavailable,
                                   orderedClaims[i])) {
          result.valid = false;
          return result;
        }
      }
    }
    return result;
  }

  std::uint64_t sourceGenerationAt(std::size_t index) const noexcept {
    return index < capacity() ? entries_[index].generation : 0;
  }

  std::size_t copyReadyPrefix(std::span<ReadyEntry> out) const noexcept {
    const std::size_t count = std::min(out.size(), readyCount_);
    for (std::size_t i = 0; i < count; ++i) {
      const auto& ready = readyFifo_[readyIndex(i)];
      const auto* entry = resolveEntry(ready.source.id, ready.source.storage);
      if (!ready.valid() || !entry || entry->state != State::Ready ||
          !metadataMatchesEntry(ready.metadata, *entry) ||
          !semanticMatchesEntry(ready.semantic, *entry) ||
          !entry->readyPublicationReserved) {
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
    if (!entryMatches(entry, seqId, expected)) {
      noteStaleReject();
      return false;
    }
    return true;
  }

  bool matches(SourceRef source, CpuReadySourceMetadata metadata,
               State expected) const noexcept {
    const auto* entry = resolveEntry(source.id, source.storage);
    if (!entry || !metadataMatchesEntry(metadata, *entry) ||
        !entryMatches(entry, metadata.seqId, expected)) {
      noteStaleReject();
      return false;
    }
    return true;
  }

  bool matches(SourceRef source, CpuReadySourceMetadata metadata,
               const SourceSemanticSummary& semantic,
               State expected) const noexcept {
    const auto* entry = resolveEntry(source.id, source.storage);
    if (!entry || !metadataMatchesEntry(metadata, *entry) ||
        !semanticMatchesEntry(semantic, *entry) ||
        !entryMatches(entry, metadata.seqId, expected)) {
      noteStaleReject();
      return false;
    }
    return true;
  }

  std::optional<CpuReadySourceMetadata> sourceMetadata(
      CpuReadySourceId id, CpuReadyStorageRef storage,
      State expected) const noexcept {
    const auto* entry = resolveEntry(id, storage);
    if (!entry || entry->state != expected ||
        expected == State::Writing || expected == State::Sealed ||
        expected == State::Reclaiming) {
      noteStaleReject();
      return std::nullopt;
    }
    return metadataForEntry(*entry);
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

  ReserveProbe probeArenaReserve(
      const ArenaSourcePayloadLayout& layout) noexcept {
    if (!layout.valid()) {
      return ReserveProbe::InvalidRequest;
    }
    const ReserveProbe probe = inspectReserve(
        layout.pageCount, PayloadKind::Arena, layout.segmentCount);
    if (probe == ReserveProbe::TemporaryPressure) {
      closeAdmission(pressureDimensions(layout.pageCount,
                                        PayloadKind::Arena));
    } else if (probe == ReserveProbe::Corrupt) {
      stopAdmission();
    }
    return probe;
  }

  // The batch reserve path reports TemporaryPressure only while a previously
  // closed Arena pressure dimension remains latched. Other aggregate batch
  // failures are recoverable/corrupt begin results and must be retried there,
  // not turned into an admission wait. Validate every source so the wait gate
  // never projects a multi-source request through layouts.front().
  ReserveProbe probeArenaBatchAdmission(
      std::span<const ArenaSourcePayloadLayout> layouts) const noexcept {
    if (layouts.empty() || layouts.size() > kMaxArenaBatchSources) {
      return ReserveProbe::InvalidRequest;
    }
    for (const auto& layout : layouts) {
      if (!layout.valid() ||
          layout.pageCount > config_.values().maxPagesPerSource) {
        return ReserveProbe::InvalidRequest;
      }
    }
    if (stopped_) {
      return ReserveProbe::Stopped;
    }
    return (closedPressureDimensions_ & ~kPressureCompatibility) != 0
        ? ReserveProbe::TemporaryPressure
        : ReserveProbe::Ready;
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
    SourcePayloadLayout segment{};
    segment.usedBytes = plannedBytes;
    segment.pageCount = pageCount;
    segment.requiredBaseAlignment = 1;
    ArenaSourcePayloadLayout layout{};
    layout.segments[0] = ArenaSourcePayloadSegmentLayout{
        .layout = segment,
        .byteOffset = 0,
    };
    layout.segmentCount = 1;
    layout.usedBytes = plannedBytes;
    layout.pageCount = pageCount;
    return reserve(layout, identity);
  }

  std::optional<Reservation> reserve(
      const ArenaSourcePayloadLayout& layout,
      CpuReadyAdmissionIdentity identity) noexcept {
    const std::size_t pageCount = layout.pageCount;
    const std::size_t plannedBytes = layout.usedBytes;
    if (!identity.valid() || plannedBytes == 0 ||
        !layout.valid() ||
        1 + (plannedBytes - 1) / config_.values().pageSize != pageCount ||
        compatibilityWritingCount_ != 0 || strictWritingActive_ ||
        identity.rawOrdinal <= rawOrdinalHighWater_ ||
        identity.sourceOrdinal <= sourceOrdinalHighWater_ ||
        identity.seqId <= seqIdHighWater_) {
      return std::nullopt;
    }

    if (probeArenaReserve(layout) != ReserveProbe::Ready) {
      return std::nullopt;
    }
    auto reservation = reserveStorageImpl(
        pageCount, PayloadKind::Arena, layout.segmentCount);
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
    entry->producerIdentity = identity.producerIdentity;
    entry->arenaPayloadCount = layout.segmentCount;
    entry->groupRawOrdinal = identity.rawOrdinal;
    entry->groupHeadSourceOrdinal = identity.sourceOrdinal;
    entry->groupBuildGeneration = identity.buildGeneration;
    entry->groupSourceCount = 1;
    entry->groupSourceIndex = 0;
    entry->groupTailSeqId = identity.seqId;
    for (std::size_t i = 0; i < layout.segmentCount; ++i) {
      entry->arenaExtents[i] = Entry::ArenaExtent{
          .byteOffset = layout.segments[i].byteOffset,
          .byteCount = layout.segments[i].layout.usedBytes,
      };
    }
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
        .producerIdentity = identity.producerIdentity,
    };
    return reservation;
  }

  // Reserves every source in one bounded transaction.  This deliberately does
  // not call the single-source reserve() path: that path advances the raw
  // high-water on every source and therefore cannot represent one raw event
  // with several source identities.
  std::optional<ArenaBatchReservation> reserveArenaBatch(
      std::span<const ArenaSourcePayloadLayout> layouts,
      std::uint64_t rawOrdinal, std::uint64_t firstSourceOrdinal,
      std::uint64_t firstSeqId, std::uint64_t buildGeneration,
      CpuReadyProducerIdentity producerIdentity = {}) noexcept {
    lastArenaBatchReserveFailure_ = ArenaBatchReserveFailure::None;
    if (layouts.empty() || layouts.size() > kMaxArenaBatchSources ||
        rawOrdinal == 0 || firstSourceOrdinal == 0 || firstSeqId == 0 ||
        buildGeneration == 0 || !producerIdentity.importable() ||
        compatibilityWritingCount_ != 0 ||
        strictWritingActive_ || rawOrdinal <= rawOrdinalHighWater_ ||
        firstSourceOrdinal <= sourceOrdinalHighWater_ ||
        firstSeqId <= seqIdHighWater_) {
      lastArenaBatchReserveFailure_ = ArenaBatchReserveFailure::Invalid;
      return std::nullopt;
    }
    if (stopped_) {
      lastArenaBatchReserveFailure_ = ArenaBatchReserveFailure::Stopped;
      return std::nullopt;
    }
    if ((closedPressureDimensions_ & ~kPressureCompatibility) != 0) {
      lastArenaBatchReserveFailure_ =
          ArenaBatchReserveFailure::TemporaryPressure;
      return std::nullopt;
    }
    std::uint64_t exclusiveSeqTail = 0;
    if (layouts.size() - 1u >
            std::numeric_limits<std::uint64_t>::max() - firstSourceOrdinal ||
        !checkedExclusiveSeqTail(firstSeqId, layouts.size(),
                                 exclusiveSeqTail)) {
      lastArenaBatchReserveFailure_ = ArenaBatchReserveFailure::Recoverable;
      return std::nullopt;
    }
    const auto& values = config_.values();
    if (layouts.size() > values.highWaterSources -
                             std::min(residentCount_, values.highWaterSources) ||
        layouts.size() > values.highWaterReady -
                             std::min(readyPublicationReservations_,
                                      values.highWaterReady)) {
      lastArenaBatchReserveFailure_ = ArenaBatchReserveFailure::Recoverable;
      return std::nullopt;
    }
    std::size_t aggregatePages = occupiedPages_;
    std::size_t simulatedPageTail = pageTail_;
    std::array<std::size_t, kMaxArenaBatchSources> simulatedFirstPages{};
    std::array<std::size_t, kMaxArenaBatchSources> simulatedPadding{};
    for (std::size_t i = 0; i < layouts.size(); ++i) {
      const auto& layout = layouts[i];
      if (!layout.valid() || layout.pageCount > values.maxPagesPerSource ||
          layout.pageCount > values.pageCount) {
        lastArenaBatchReserveFailure_ =
            layout.valid() ? ArenaBatchReserveFailure::Corrupt
                           : ArenaBatchReserveFailure::Invalid;
        return std::nullopt;
      }
      const std::size_t entryIndex = (sourceTail_ + i) % capacity();
      const auto& entry = entries_[entryIndex];
      if (entry.state != State::Free) {
        lastArenaBatchReserveFailure_ = ArenaBatchReserveFailure::Corrupt;
        return std::nullopt;
      }
      for (std::size_t segment = 0; segment < layout.segmentCount;
           ++segment) {
        if (arenaOwner(entryIndex, segment).constructed) {
          lastArenaBatchReserveFailure_ = ArenaBatchReserveFailure::Corrupt;
          return std::nullopt;
        }
      }
      const std::size_t padding =
          simulatedPageTail + layout.pageCount > values.pageCount
          ? values.pageCount - simulatedPageTail
          : 0;
      if (padding > values.highWaterPages -
                        std::min(aggregatePages, values.highWaterPages) ||
          layout.pageCount > values.highWaterPages -
                                  std::min(aggregatePages + padding,
                                           values.highWaterPages)) {
        lastArenaBatchReserveFailure_ =
            ArenaBatchReserveFailure::Recoverable;
        return std::nullopt;
      }
      if (aggregatePages > std::numeric_limits<std::size_t>::max() -
                                padding - layout.pageCount) {
        lastArenaBatchReserveFailure_ =
            ArenaBatchReserveFailure::Recoverable;
        return std::nullopt;
      }
      aggregatePages += padding + layout.pageCount;
      const std::size_t firstPage = padding != 0 ? 0 : simulatedPageTail;
      for (std::size_t page = 0; page < padding; ++page) {
        if (pages_[simulatedPageTail + page].allocated) {
          lastArenaBatchReserveFailure_ = ArenaBatchReserveFailure::Corrupt;
          return std::nullopt;
        }
      }
      for (std::size_t page = 0; page < layout.pageCount; ++page) {
        if (pages_[firstPage + page].allocated) {
          lastArenaBatchReserveFailure_ = ArenaBatchReserveFailure::Corrupt;
          return std::nullopt;
        }
      }
      for (std::size_t prior = 0; prior < i; ++prior) {
        const auto overlaps = [](std::size_t aFirst, std::size_t aCount,
                                 std::size_t bFirst,
                                 std::size_t bCount) noexcept {
          return aFirst < bFirst + bCount && bFirst < aFirst + aCount;
        };
        if (overlaps(firstPage, layout.pageCount,
                     simulatedFirstPages[prior], layouts[prior].pageCount) ||
            (padding != 0 &&
             overlaps(0, padding, simulatedFirstPages[prior],
                      layouts[prior].pageCount)) ||
            (simulatedPadding[prior] != 0 &&
             overlaps(firstPage, layout.pageCount, 0,
                      simulatedPadding[prior]))) {
          lastArenaBatchReserveFailure_ = ArenaBatchReserveFailure::Corrupt;
          return std::nullopt;
        }
      }
      simulatedFirstPages[i] = firstPage;
      simulatedPadding[i] = padding;
      simulatedPageTail = (firstPage + layout.pageCount) % values.pageCount;
      // The conservative aggregate check catches capacity exhaustion before
      // any allocator mutation; exact wrap/page overlap is rechecked below by
      // reserveStorageImpl and rolled back as one transaction if needed.
      if (aggregatePages > values.highWaterPages) {
        lastArenaBatchReserveFailure_ = ArenaBatchReserveFailure::Recoverable;
        return std::nullopt;
      }
    }
    ArenaBatchReservation result{};
    result.rawHighWaterBefore = rawOrdinalHighWater_;
    result.sourceHighWaterBefore = sourceOrdinalHighWater_;
    result.seqHighWaterBefore = seqIdHighWater_;
    result.sourceTailBefore = sourceTail_;
    result.pageTailBefore = pageTail_;
    result.residentCountBefore = residentCount_;
    result.occupiedPagesBefore = occupiedPages_;
    result.readyCountBefore = readyCount_;
    result.readyReservationsBefore = readyPublicationReservations_;
    for (std::size_t i = 0; i < layouts.size(); ++i) {
      auto reservation = reserveStorageImpl(
          layouts[i].pageCount, PayloadKind::Arena, layouts[i].segmentCount);
      if (!reservation) {
        bool cleanupFailed = false;
        for (std::size_t j = result.count; j != 0; --j) {
          auto detached = beginArenaAbort(
              result.reservations[j - 1].ticket);
          if (!detached) {
            stopAdmission();
            lastArenaBatchReserveFailure_ =
                ArenaBatchReserveFailure::Corrupt;
            return std::nullopt;
          }
          detached->destroy();
          if (!finishArenaAbort(result.reservations[j - 1].ticket,
                                std::move(*detached))) {
            cleanupFailed = true;
          }
        }
        strictWritingActive_ = false;
        if (cleanupFailed) {
          stopAdmission();
        }
        lastArenaBatchReserveFailure_ = ArenaBatchReserveFailure::Corrupt;
        return std::nullopt;
      }
      auto* entry = resolveEntry(reservation->id, reservation->storage);
      if (!entry) {
        stopAdmission();
        lastArenaBatchReserveFailure_ = ArenaBatchReserveFailure::Corrupt;
        return std::nullopt;
      }
      entry->strictAdmission = true;
      entry->plannedBytes = layouts[i].usedBytes;
      entry->rawOrdinal = rawOrdinal;
      entry->sourceOrdinal = firstSourceOrdinal + i;
      entry->seqId = firstSeqId + i;
      entry->buildGeneration = buildGeneration;
      entry->producerIdentity = producerIdentity;
      entry->arenaPayloadCount = layouts[i].segmentCount;
      entry->groupRawOrdinal = rawOrdinal;
      entry->groupHeadSourceOrdinal = firstSourceOrdinal;
      entry->groupBuildGeneration = buildGeneration;
      entry->groupSourceCount = static_cast<std::uint32_t>(layouts.size());
      entry->groupSourceIndex = static_cast<std::uint32_t>(i);
      entry->groupTailSeqId = exclusiveSeqTail - 1u;
      for (std::size_t segment = 0; segment < layouts[i].segmentCount;
           ++segment) {
        entry->arenaExtents[segment] = Entry::ArenaExtent{
            .byteOffset = layouts[i].segments[segment].byteOffset,
            .byteCount = layouts[i].segments[segment].layout.usedBytes,
        };
      }
      reservation->ticket = CpuReadyPublicationTicket{
          .id = reservation->id,
          .storage = reservation->storage,
          .rawOrdinal = rawOrdinal,
          .sourceOrdinal = firstSourceOrdinal + i,
          .seqId = firstSeqId + i,
          .buildGeneration = buildGeneration,
          .producerIdentity = producerIdentity,
      };
      result.reservations[i] = *reservation;
      ++result.count;
    }
    // Consume the event ordinal exactly once, and expose the contiguous tail
    // identities only after every Writing entry has been constructed.
    rawOrdinalHighWater_ = rawOrdinal;
    sourceOrdinalHighWater_ = firstSourceOrdinal + layouts.size() - 1u;
    seqIdHighWater_ = exclusiveSeqTail - 1u;
    strictWritingActive_ = true;
    return result;
  }

  // Recoverable pre-effect abort only. Post-effect callers deliberately do
  // not invoke this and retain the fail-stop high-water discipline.
  bool restoreArenaBatchHighWaters(
      const ArenaBatchReservation& batch) noexcept {
    if (!batch.valid()) {
      return false;
    }
    if (sourceTail_ != batch.sourceTailBefore ||
        pageTail_ != batch.pageTailBefore ||
        residentCount_ != batch.residentCountBefore ||
        occupiedPages_ != batch.occupiedPagesBefore ||
        readyCount_ != batch.readyCountBefore ||
        readyPublicationReservations_ != batch.readyReservationsBefore) {
      noteStaleReject();
      return false;
    }
    const auto& lastTicket = batch.reservations[batch.count - 1u].ticket;
    if (rawOrdinalHighWater_ != lastTicket.rawOrdinal ||
        sourceOrdinalHighWater_ != lastTicket.sourceOrdinal ||
        seqIdHighWater_ != lastTicket.seqId || strictWritingActive_) {
      noteStaleReject();
      return false;
    }
    for (std::size_t i = 0; i < batch.count; ++i) {
      const auto& reservation = batch.reservations[i];
      if (reservation.id.index >= capacity() ||
          entries_[reservation.id.index].state != State::Free ||
          entries_[reservation.id.index].generation == reservation.id.generation) {
        noteStaleReject();
        return false;
      }
    }
    rawOrdinalHighWater_ = batch.rawHighWaterBefore;
    sourceOrdinalHighWater_ = batch.sourceHighWaterBefore;
    seqIdHighWater_ = batch.seqHighWaterBefore;
    strictWritingActive_ = false;
    refreshAdmissionAfterRelease();
    return true;
  }

  // All structural and payload checks happen before the first Ready state is
  // written.  The individual seal operation is then deterministic and cannot
  // encounter pressure because ready capacity was preflighted as a batch.
  bool sealAndPublishArenaBatch(
      const ArenaBatchReservation& batch,
      std::span<const std::size_t> controlIndices) noexcept {
    if (!batch.valid() || controlIndices.size() != batch.count ||
        batch.count > config_.values().readyFifoCount ||
        readyCount_ > config_.values().readyFifoCount - batch.count) {
      noteStaleReject();
      return false;
    }
    const auto& first = batch.reservations[0].ticket;
    if (!first.strictIdentityValid()) {
      noteStaleReject();
      return false;
    }
    if (batch.count - 1u >
        std::numeric_limits<std::uint64_t>::max() - first.seqId) {
      noteStaleReject();
      return false;
    }
    const auto expectedGroupTailSeqId =
        first.seqId + static_cast<std::uint64_t>(batch.count - 1u);
    std::array<SourceSemanticSummary, kMaxArenaBatchSources> semantics{};
    std::array<std::size_t, kMaxArenaBatchSources> usedBytes{};
    for (std::size_t i = 0; i < batch.count; ++i) {
      const auto& reservation = batch.reservations[i];
      const auto& current = reservation.ticket;
      auto* entry = resolveEntry(reservation.id, reservation.storage);
      if (i > std::numeric_limits<std::uint64_t>::max() - first.sourceOrdinal ||
          i > std::numeric_limits<std::uint64_t>::max() - first.seqId) {
        noteStaleReject();
        return false;
      }
      if (!entry || entry->state != State::Writing ||
          !entry->strictAdmission || !entry->readyPublicationReserved ||
          !reservation.ticket.hasAdmissionIdentity() ||
          controlIndices[i] == kInvalidIndex ||
          reservation.ticket.id != reservation.id ||
          reservation.ticket.storage != reservation.storage ||
          current.rawOrdinal != first.rawOrdinal ||
          current.buildGeneration != first.buildGeneration ||
          current.sourceOrdinal != first.sourceOrdinal + i ||
          current.seqId != first.seqId + i ||
          entry->groupRawOrdinal != first.rawOrdinal ||
          entry->groupHeadSourceOrdinal != first.sourceOrdinal ||
          entry->groupBuildGeneration != first.buildGeneration ||
          entry->groupTailSeqId != expectedGroupTailSeqId ||
          entry->groupSourceCount != batch.count ||
          entry->groupSourceIndex != i) {
        noteStaleReject();
        return false;
      }
      for (std::size_t prior = 0; prior < i; ++prior) {
        if (controlIndices[prior] == controlIndices[i]) {
          noteStaleReject();
          return false;
        }
      }
      const auto reservedStorage = storageSpan(reservation.storage);
      const auto plannedStorage = reservedStorage.first(entry->plannedBytes);
      std::array<const ArenaSourcePayloadBlock*,
                 kMaxArenaSourcePayloadSegments> payloads{};
      for (std::size_t segment = 0; segment < entry->arenaPayloadCount;
           ++segment) {
        const auto extent = entry->arenaExtents[segment];
        if (extent.byteOffset > plannedStorage.size() ||
            extent.byteCount > plannedStorage.size() - extent.byteOffset) {
          noteStaleReject();
          return false;
        }
        const auto& owner = arenaOwner(reservation.id.index, segment);
        const auto* payload = owner.constructed ? owner.payload() : nullptr;
        if (!payload || !payload->published() ||
            !payload->boundTo(std::span<const std::byte>(
                plannedStorage.subspan(extent.byteOffset, extent.byteCount)))) {
          noteStaleReject();
          return false;
        }
        payloads[segment] = payload;
      }
      if (!entry->arenaChain.initialize(
              std::span(payloads).first(entry->arenaPayloadCount))) {
        noteStaleReject();
        return false;
      }
      usedBytes[i] = entry->plannedBytes;
      const SourcePayloadView payloadView = entry->arenaPayloadCount == 1
          ? SourcePayloadView(*payloads[0])
          : SourcePayloadView(entry->arenaChain);
      semantics[i] = makeSealedSemanticSummary(
          payloadView, usedBytes[i], reservation.storage.pageCount);
    }
    // From this point onward every operation is assignment/ring advancement
    // against already validated storage; it cannot fail or expose a partial
    // Ready prefix.
    for (std::size_t i = 0; i < batch.count; ++i) {
      auto* entry = resolveEntry(batch.reservations[i].id,
                                  batch.reservations[i].storage);
      entry->usedBytes = usedBytes[i];
      entry->semantic = semantics[i];
      entry->state = State::Ready;
      readyFifo_[readyTail_] = ReadyEntry{
          .source = SourceRef{.id = batch.reservations[i].id,
                              .storage = batch.reservations[i].storage},
          .controlIndex = controlIndices[i],
          .seqId = entry->seqId,
          .metadata = metadataForEntry(*entry),
          .semantic = entry->semantic,
      };
      readyTail_ = (readyTail_ + 1u) % config_.values().readyFifoCount;
      ++readyCount_;
    }
    stats_.readyFifoEntries = readyCount_;
    strictWritingActive_ = false;
    return true;
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

  std::span<std::byte> writableArenaSegment(
      CpuReadyPublicationTicket ticket,
      std::size_t segmentIndex) noexcept {
    auto* entry = resolveEntry(ticket.id, ticket.storage);
    if (!entry || entry->state != State::Writing ||
        entry->payloadKind != PayloadKind::Arena ||
        !ticketMatchesEntry(ticket, *entry) ||
        segmentIndex >= entry->arenaPayloadCount) {
      noteStaleReject();
      return {};
    }
    const auto extent = entry->arenaExtents[segmentIndex];
    auto storage = storageSpan(ticket.storage);
    if (extent.byteOffset > storage.size() ||
        extent.byteCount > storage.size() - extent.byteOffset) {
      noteStaleReject();
      return {};
    }
    return storage.subspan(extent.byteOffset, extent.byteCount);
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
    if (entry->arenaPayloadCount == 1) {
      const auto* payload = arenaPayload(*entry);
      return payload && payload->readable() ? SourcePayloadView(*payload)
                                            : SourcePayloadView{};
    }
    return entry->arenaChain.readable()
               ? SourcePayloadView(entry->arenaChain)
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

  std::optional<CpuReadyProducerIdentity> producerIdentity(
      CpuReadySourceId id, CpuReadyStorageRef storage,
      State expected) const noexcept {
    const auto* entry = resolveEntry(id, storage);
    if (!entry || entry->state != expected ||
        !entry->producerIdentity.importable()) {
      noteStaleReject();
      return std::nullopt;
    }
    return entry->producerIdentity;
  }

  // Cold lifecycle-observer lookup.  The queue owner already serializes this
  // call with every Tape transition, so the observer can read the immutable
  // producer interval without guessing the current state.  A lookup miss is
  // not a stale-capability rejection and therefore must not perturb the Tape
  // staleRejects counter used by the production ownership contract.
  std::optional<CpuReadyProducerIdentity> inspectProducerIdentity(
      SourceRef source) const noexcept {
    const auto* entry = resolveEntry(source.id, source.storage);
    if (!entry || entry->state == State::Free ||
        !entry->producerIdentity.importable()) {
      return std::nullopt;
    }
    return entry->producerIdentity;
  }

  // The exact active-raw span witness of a live slot. Callers use it before
  // opening a transaction so a same-raw continuation is decided pre-effect.
  std::optional<CpuReadySpanWitness> inspectSpanWitness(
      SourceRef source) const noexcept {
    const auto* entry = resolveEntry(source.id, source.storage);
    if (!entry || entry->state == State::Free) {
      return std::nullopt;
    }
    return entry->spanWitness;
  }

  bool sealAndPublish(CpuReadyPublicationTicket ticket,
                      std::uint64_t sourceOrdinal,
                      std::uint64_t seqId,
                      std::size_t controlIndex,
                      std::size_t usedBytes = 0,
                      std::uint64_t rawOrdinal = 0,
                      CpuReadyProducerIdentity producerIdentity = {}) noexcept {
    auto* entry = resolveEntry(ticket.id, ticket.storage);
    const std::size_t reservedBytes =
        static_cast<std::size_t>(ticket.storage.pageCount) *
        config_.values().pageSize;
    if (!entry || entry->state != State::Writing || entry->strictAdmission ||
        entry->payloadKind != PayloadKind::Legacy ||
        ticket.hasAdmissionIdentity() || sourceOrdinal == 0 || seqId == 0 ||
        !producerIdentity.importable() ||
        (!entry->producerIdentity.absent() && !producerIdentity.absent() &&
         entry->producerIdentity != producerIdentity) ||
        (rawOrdinal != 0 && rawOrdinal <= rawOrdinalHighWater_) ||
        sourceOrdinal <= sourceOrdinalHighWater_ ||
        seqId <= seqIdHighWater_ || controlIndex == kInvalidIndex ||
        !entry->readyPublicationReserved ||
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
    if (usedBytes != 0) {
      // Explicit extents continue to describe caller-written Tape storage and
      // must fit the reserved page run.
      if (usedBytes > reservedBytes) {
        noteStaleReject();
        return false;
      }
    } else {
      // Legacy compatibility payloads live in their ChunkSlot vectors rather
      // than the page arena. Preserve their deterministic logical replay
      // extent for validation and telemetry; session admission derives its
      // distinct bounded residency charge from representation metadata.
      // Arena publication retains its exact planned storage extent.
      usedBytes = measureSourcePayloadLogicalExtent(SourcePayloadView(*payload));
    }
    const auto semantic = makeSealedSemanticSummary(
        SourcePayloadView(*payload), usedBytes, ticket.storage.pageCount);
    payload->seqId = seqId;
    entry->rawOrdinal = rawOrdinal;
    entry->sourceOrdinal = sourceOrdinal;
    entry->seqId = seqId;
    if (entry->producerIdentity.absent()) {
      entry->producerIdentity = producerIdentity;
    }
    entry->usedBytes = usedBytes;
    entry->semantic = semantic;
    entry->state = State::Ready;
    readyFifo_[readyTail_] = ReadyEntry{
        .source = SourceRef{.id = ticket.id, .storage = ticket.storage},
        .controlIndex = controlIndex,
        .seqId = seqId,
        .metadata = metadataForEntry(*entry),
        .semantic = entry->semantic,
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

  // Direct replay may append several consecutive PE events into one open
  // compatibility ChunkSlot. Preserve the exact closed producer interval
  // without forcing a physical publish boundary. Any gap, overlap, or
  // reordering is a post-effect ownership failure for the caller.
  // `spanRawOrdinal == 0` is the historical whole-raw caller and is decided
  // entirely by the unchanged cross-raw predicate. A span-qualified caller
  // additionally presents which raw it belongs to, that raw's span ordinal,
  // and whether the span is the raw's last; only the immediate successor span
  // of the exact witnessed raw extends the slot without an interval advance.
  bool extendCompatibilityProducerIdentity(
      CpuReadyPublicationTicket ticket,
      CpuReadyProducerIdentity next,
      std::uint64_t spanRawOrdinal = 0,
      std::uint32_t spanOrdinal = 0,
      bool finalSpan = true) noexcept {
    auto* entry = resolveEntry(ticket.id, ticket.storage);
    if (!entry || entry->state != State::Writing || entry->strictAdmission ||
        entry->payloadKind != PayloadKind::Legacy ||
        ticket.hasAdmissionIdentity() || !next.importable()) {
      noteStaleReject();
      return false;
    }
    const auto admission = compatibilitySpanAdmission(
        entry->spanWitness, entry->producerIdentity, spanRawOrdinal,
        spanOrdinal, next);
    switch (admission) {
    case CpuReadySpanAdmission::SameRawSpan:
      // The interval is already exactly this raw's; only the witness moves.
      entry->spanWitness.lastSpanOrdinal = spanOrdinal;
      entry->spanWitness.settled = finalSpan;
      return true;
    case CpuReadySpanAdmission::CrossRaw:
      break;
    case CpuReadySpanAdmission::DuplicateSpan:
    case CpuReadySpanAdmission::SkippedSpan:
    case CpuReadySpanAdmission::SettledRaw:
    case CpuReadySpanAdmission::IdentityMismatch:
      noteStaleReject();
      return false;
    }
    if (!compatibilityProducerIdentityAppendable(entry->producerIdentity,
                                                  next)) {
      noteStaleReject();
      return false;
    }
    // A cross-raw extension always replaces the witness: the previous raw's
    // span sequence can never resume behind a newer raw's prefix.
    entry->spanWitness = spanRawOrdinal != 0u
        ? CpuReadySpanWitness{.rawOrdinal = spanRawOrdinal,
                              .lastSpanOrdinal = spanOrdinal,
                              .settled = finalSpan}
        : CpuReadySpanWitness{};
    if (next.absent()) return true;
    if (entry->producerIdentity.absent()) {
      entry->producerIdentity = next;
      return true;
    }
    auto& current = entry->producerIdentity;
    current.lastEventOrdinal = next.lastEventOrdinal;
    current.lastSourceOrdinal = next.lastSourceOrdinal;
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
    const auto reservedStorage = storageSpan(ticket.storage);
    const auto plannedStorage = reservedStorage.first(entry->plannedBytes);
    std::array<const ArenaSourcePayloadBlock*,
               kMaxArenaSourcePayloadSegments> payloads{};
    for (std::size_t i = 0; i < entry->arenaPayloadCount; ++i) {
      const auto extent = entry->arenaExtents[i];
      if (extent.byteOffset > plannedStorage.size() ||
          extent.byteCount > plannedStorage.size() - extent.byteOffset) {
        noteStaleReject();
        return false;
      }
      const auto& owner = arenaOwner(ticket.id.index, i);
      const auto* payload = owner.constructed ? owner.payload() : nullptr;
      const auto segmentStorage =
          plannedStorage.subspan(extent.byteOffset, extent.byteCount);
      if (!payload || !payload->published() ||
          !payload->boundTo(
              std::span<const std::byte>(segmentStorage))) {
        noteStaleReject();
        return false;
      }
      payloads[i] = payload;
    }
    if (!entry->arenaChain.initialize(
            std::span(payloads).first(entry->arenaPayloadCount))) {
      noteStaleReject();
      return false;
    }
    const SourcePayloadView payloadView = entry->arenaPayloadCount == 1
        ? SourcePayloadView(*payloads[0])
        : SourcePayloadView(entry->arenaChain);
    const auto semantic = makeSealedSemanticSummary(
        payloadView, usedBytes, ticket.storage.pageCount);
    entry->usedBytes = usedBytes;
    entry->semantic = semantic;
    entry->state = State::Ready;
    readyFifo_[readyTail_] = ReadyEntry{
        .source = SourceRef{.id = ticket.id, .storage = ticket.storage},
        .controlIndex = controlIndex,
        .seqId = entry->seqId,
        .metadata = metadataForEntry(*entry),
        .semantic = entry->semantic,
    };
    readyTail_ = (readyTail_ + 1) % config_.values().readyFifoCount;
    ++readyCount_;
    stats_.readyFifoEntries = readyCount_;
    strictWritingActive_ = false;
    return true;
  }

  bool representReadyPrefix(std::span<const ReadyEntry> selected) noexcept {
    if (!reserveReadyPrefixForRepresentation(selected)) {
      return false;
    }
    if (!commitReservedReadyPrefix(selected)) {
      const bool restored = restoreReservedReadyPrefix(selected);
      DXMT_ASSERT(restored);
      (void)restored;
      return false;
    }
    return true;
  }

  // Removes one validated FIFO prefix from Ready without making it
  // irrevocably Represented.  The caller retains the copied ReadyEntry values
  // as the generation-stamped capability passed to commit or restore.  Only
  // one tentative prefix may exist, so restore always returns it ahead of
  // every younger Ready publication.
  bool reserveReadyPrefixForRepresentation(
      std::span<const ReadyEntry> selected) noexcept {
    if (selected.empty() || selected.size() > readyCount_ ||
        tentativeRepresentationCount_ != 0) {
      noteStaleReject();
      return false;
    }
    for (std::size_t i = 0; i < selected.size(); ++i) {
      const auto& ready = readyFifo_[readyIndex(i)];
      auto* entry = resolveEntry(ready.source.id, ready.source.storage);
      if (selected[i] != ready || !ready.valid() || !entry ||
          entry->state != State::Ready ||
          !metadataMatchesEntry(ready.metadata, *entry) ||
          !semanticMatchesEntry(ready.semantic, *entry) ||
          !entry->readyPublicationReserved) {
        noteStaleReject();
        return false;
      }
    }
    for (std::size_t i = 0; i < selected.size(); ++i) {
      tentativeReadyPrefix_[i] = selected[i];
      auto& ready = readyFifo_[readyHead_];
      auto* entry = resolveEntry(ready.source.id, ready.source.storage);
      entry->state = State::TentativeRepresented;
      ready = {};
      readyHead_ = (readyHead_ + 1) % config_.values().readyFifoCount;
    }
    readyCount_ -= selected.size();
    tentativeRepresentationCount_ = selected.size();
    stats_.readyFifoEntries = readyCount_;
    return true;
  }

  bool commitReservedReadyPrefix(
      std::span<const ReadyEntry> selected) noexcept {
    if (!validateTentativePrefix(selected)) {
      noteStaleReject();
      return false;
    }
    for (const auto& ready : selected) {
      auto* entry = resolveEntry(ready.source.id, ready.source.storage);
      entry->state = State::Represented;
      entry->readyPublicationReserved = false;
    }
    readyPublicationReservations_ -= selected.size();
    for (std::size_t i = 0; i < tentativeRepresentationCount_; ++i) {
      tentativeReadyPrefix_[i] = {};
    }
    tentativeRepresentationCount_ = 0;
    stats_.readyPublicationReservations = readyPublicationReservations_;
    refreshAdmissionAfterRelease();
    return true;
  }

  bool restoreReservedReadyPrefix(
      std::span<const ReadyEntry> selected) noexcept {
    if (!validateTentativePrefix(selected) ||
        selected.size() > config_.values().readyFifoCount - readyCount_) {
      noteStaleReject();
      return false;
    }
    std::size_t restoredHead = readyHead_;
    for (std::size_t i = 0; i < selected.size(); ++i) {
      restoredHead = restoredHead == 0
          ? config_.values().readyFifoCount - 1u
          : restoredHead - 1u;
    }
    for (std::size_t i = 0; i < selected.size(); ++i) {
      const std::size_t index =
          (restoredHead + i) % config_.values().readyFifoCount;
      if (readyFifo_[index].valid()) {
        noteStaleReject();
        return false;
      }
    }
    for (std::size_t i = 0; i < selected.size(); ++i) {
      const std::size_t index =
          (restoredHead + i) % config_.values().readyFifoCount;
      const auto& ready = tentativeReadyPrefix_[i];
      readyFifo_[index] = ready;
      resolveEntry(ready.source.id, ready.source.storage)->state = State::Ready;
      tentativeReadyPrefix_[i] = {};
    }
    readyHead_ = restoredHead;
    readyCount_ += selected.size();
    tentativeRepresentationCount_ = 0;
    stats_.readyFifoEntries = readyCount_;
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
    // Detach is intentionally a two-phase operation: owners are destroyed
    // outside the tape lock and then finished in reverse order.  During that
    // first phase sourceTail_ still names the end of the original batch, so
    // skip the already-detached Reclaiming suffix when proving that this
    // ticket is the next newest Writing entry.
    std::size_t expectedIndex = previousSourceIndex(sourceTail_);
    for (std::size_t detached = 0; detached < residentCount_; ++detached) {
      const auto& prior = entries_[expectedIndex];
      if (prior.state != State::Reclaiming || !prior.arenaOwnerDetached ||
          prior.arenaDetachKind != Entry::ArenaDetachKind::Abort) {
        break;
      }
      expectedIndex = previousSourceIndex(expectedIndex);
    }
    if (!entry || entry->state != State::Writing || residentCount_ == 0 ||
        entry->payloadKind != PayloadKind::Arena ||
        ticket.id.index != expectedIndex ||
        !ticketMatchesEntry(ticket, *entry) ||
        !entry->readyPublicationReserved ||
        readyPublicationReservations_ == 0 || entry->arenaOwnerDetached) {
      noteStaleReject();
      return std::nullopt;
    }
    std::array<ArenaSourcePayloadBlock*,
               kMaxArenaSourcePayloadSegments> payloads{};
    for (std::size_t i = 0; i < entry->arenaPayloadCount; ++i) {
      auto& owner = arenaOwner(ticket.id.index, i);
      if (!owner.constructed) {
        noteStaleReject();
        return std::nullopt;
      }
      payloads[i] = owner.payload();
    }
    --readyPublicationReservations_;
    stats_.readyPublicationReservations = readyPublicationReservations_;
    entry->state = State::Reclaiming;
    entry->arenaChain.clear();
    entry->arenaOwnerDetached = true;
    entry->arenaDetachKind = Entry::ArenaDetachKind::Abort;
    return DetachedArenaOwner(
        std::span(payloads).first(entry->arenaPayloadCount));
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
        owner.count_ != entry->arenaPayloadCount) {
      noteStaleReject();
      return false;
    }
    for (std::size_t i = 0; i < owner.count_; ++i) {
      auto& slot = arenaOwner(ticket.id.index, i);
      if (owner.payloads_[i] != slot.storageAddress()) {
        noteStaleReject();
        return false;
      }
    }
    for (std::size_t i = 0; i < owner.count_; ++i) {
      arenaOwner(ticket.id.index, i).constructed = false;
    }
    owner.payloads_ = {};
    owner.count_ = 0;
    owner.destroyed_ = false;
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

  std::optional<ArenaGroupSettlement> takeCompletedArenaGroupSettlement(
      SourceRef source) noexcept {
    auto* entry = resolveEntry(source.id, source.storage);
    if (!entry || entry->state != State::Completed ||
        !entry->strictAdmission || entry->seqId != entry->groupTailSeqId ||
        entry->groupSettlementEmitted || !groupMembersCompleted(*entry)) {
      return std::nullopt;
    }
    entry->groupSettlementEmitted = true;
    return ArenaGroupSettlement{
        .rawOrdinal = entry->groupRawOrdinal,
        .buildGeneration = entry->groupBuildGeneration,
        .firstSourceOrdinal = entry->groupHeadSourceOrdinal,
        .tailSeqId = entry->groupTailSeqId,
        .sourceCount = entry->groupSourceCount,
        .hasPresent = entry->semantic.hasPresent(),
    };
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

  bool canBeginPostEncodeRetire(CpuReadySourceId id,
                                CpuReadyStorageRef storage) const noexcept {
    const auto* entry = resolveEntry(id, storage);
    // SegmentSerial keeps the complete group's physical page/control credit
    // live until the tail has settled.  This intentionally disables the
    // locator-backed early-retirement shortcut for grouped entries; the
    // ordinary FIFO reclaim path then releases every member exactly once.
    return entry && entry->groupSourceCount <= 1u && id.index == sourceHead_ &&
           entry->state == State::Represented &&
           !entry->readyPublicationReserved && !entry->arenaOwnerDetached;
  }

  // Post-encode retirement is completion-neutral: it releases only payload,
  // page, and source-control residency after a queue receipt has become the
  // completion authority. The caller clears this detached Legacy payload
  // outside the queue lock and finishes through finishReclaim().
  SourcePayloadBlock* beginPostEncodeLegacyRetire(
      CpuReadySourceId id, CpuReadyStorageRef storage) noexcept {
    auto* entry = resolveEntry(id, storage);
    if (!canBeginPostEncodeRetire(id, storage) ||
        entry->payloadKind != PayloadKind::Legacy) {
      noteStaleReject();
      return nullptr;
    }
    auto* payload = compatibilityPayload(*entry);
    if (!payload) {
      noteStaleReject();
      return nullptr;
    }
    entry->state = State::Reclaiming;
    return payload;
  }

  std::optional<DetachedArenaOwner> beginPostEncodeArenaRetire(
      CpuReadySourceId id, CpuReadyStorageRef storage) noexcept {
    auto* entry = resolveEntry(id, storage);
    if (!canBeginPostEncodeRetire(id, storage) ||
        entry->payloadKind != PayloadKind::Arena) {
      noteStaleReject();
      return std::nullopt;
    }
    entry->state = State::Reclaiming;
    return detachReclaimingArenaOwner(id, storage);
  }

  bool beginReclaim(CpuReadySourceId id,
                    CpuReadyStorageRef storage) noexcept {
    auto* entry = resolveEntry(id, storage);
    const auto status = reclaimStatus(id, storage);
    if (!entry || id.index != sourceHead_ ||
        status.disposition != ReclaimDisposition::Ready) {
      if (status.disposition == ReclaimDisposition::Invalid ||
          !entry || id.index != sourceHead_) {
        noteStaleReject();
      }
      return false;
    }
    entry->state = State::Reclaiming;
    return true;
  }

  // Pure lifecycle observation used by QueueLifecycle to distinguish the
  // expected SegmentSerial group hold from a stale/corrupt reclaim locator.
  // It does not make a failed probe count as a stale access; beginReclaim()
  // retains that accounting for genuinely invalid transitions.
  ReclaimStatus reclaimStatus(CpuReadySourceId id,
                              CpuReadyStorageRef storage) const noexcept {
    const auto* entry = resolveEntry(id, storage);
    if (!entry || entry->state != State::Completed ||
        entry->readyPublicationReserved || entry->arenaOwnerDetached) {
      return {};
    }
    if (!entry->strictAdmission) {
      if (entry->groupRawOrdinal != 0u ||
          entry->groupHeadSourceOrdinal != 0u ||
          entry->groupBuildGeneration != 0u ||
          entry->groupTailSeqId != 0u ||
          entry->groupSourceCount != 0u || entry->groupSourceIndex != 0u) {
        return {};
      }
      return ReclaimStatus{
          .disposition = ReclaimDisposition::Ready,
          .groupTailSeqId = entry->seqId,
          .groupSourceCount = 1u,
          .groupSourceIndex = 0u,
      };
    }
    if (!groupIdentityValid(*entry)) {
      return {};
    }
    const auto groupStatus = groupCompletionStatus(*entry);
    if (groupStatus == GroupCompletionStatus::Invalid) {
      return {};
    }
    return ReclaimStatus{
        .disposition = groupStatus == GroupCompletionStatus::Complete
            ? ReclaimDisposition::Ready
            : ReclaimDisposition::AwaitingGroupCompletion,
        .groupTailSeqId = entry->groupTailSeqId,
        .groupSourceCount = entry->groupSourceCount,
        .groupSourceIndex = entry->groupSourceIndex,
    };
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
    std::array<ArenaSourcePayloadBlock*,
               kMaxArenaSourcePayloadSegments> payloads{};
    for (std::size_t i = 0; i < entry->arenaPayloadCount; ++i) {
      auto& owner = arenaOwner(id.index, i);
      if (!owner.constructed) {
        noteStaleReject();
        return std::nullopt;
      }
      payloads[i] = owner.payload();
    }
    entry->arenaChain.clear();
    entry->arenaOwnerDetached = true;
    entry->arenaDetachKind = Entry::ArenaDetachKind::Reclaim;
    return DetachedArenaOwner(
        std::span(payloads).first(entry->arenaPayloadCount));
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
        owner.count_ != entry->arenaPayloadCount) {
      noteStaleReject();
      return false;
    }
    for (std::size_t i = 0; i < owner.count_; ++i) {
      auto& slot = arenaOwner(id.index, i);
      if (owner.payloads_[i] != slot.storageAddress()) {
        noteStaleReject();
        return false;
      }
    }
    for (std::size_t i = 0; i < owner.count_; ++i) {
      arenaOwner(id.index, i).constructed = false;
    }
    owner.payloads_ = {};
    owner.count_ = 0;
    owner.destroyed_ = false;
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
    struct ArenaExtent {
      std::size_t byteOffset = 0;
      std::size_t byteCount = 0;
    };
    std::array<ArenaExtent,
               kMaxArenaSourcePayloadSegments> arenaExtents{};
    ArenaSourcePayloadChain arenaChain{};
    std::size_t arenaPayloadCount = 0;
    std::uint64_t rawOrdinal = 0;
    std::uint64_t sourceOrdinal = 0;
    std::uint64_t seqId = 0;
    std::uint64_t buildGeneration = 0;
    CpuReadyProducerIdentity producerIdentity{};
    // Which raw's lease-span sequence is currently extending this slot. Reset
    // with the entry, so a slot rotation or reuse cannot carry a stale
    // same-raw admission across a publication boundary.
    CpuReadySpanWitness spanWitness{};
    std::uint64_t groupRawOrdinal = 0;
    std::uint64_t groupHeadSourceOrdinal = 0;
    std::uint64_t groupBuildGeneration = 0;
    std::uint64_t groupTailSeqId = 0;
    std::uint32_t groupSourceCount = 0;
    std::uint32_t groupSourceIndex = 0;
    bool groupSettlementEmitted = false;
    SourceSemanticSummary semantic{};
    bool strictAdmission = false;
    bool readyPublicationReserved = false;
    bool arenaOwnerDetached = false;
    enum class ArenaDetachKind : std::uint8_t {
      None,
      Abort,
      Reclaim,
    } arenaDetachKind = ArenaDetachKind::None;
  };

  static bool groupIdentityValid(const Entry& entry) noexcept {
    if (!entry.strictAdmission || entry.seqId == 0u ||
        entry.groupRawOrdinal == 0u ||
        entry.groupHeadSourceOrdinal == 0u ||
        entry.groupBuildGeneration == 0u || entry.groupSourceCount == 0u ||
        entry.groupTailSeqId == 0u ||
        entry.groupSourceCount > kMaxArenaBatchSources ||
        entry.groupSourceIndex >= entry.groupSourceCount) {
      return false;
    }
    const auto remaining = static_cast<std::uint64_t>(
        entry.groupSourceCount - 1u - entry.groupSourceIndex);
    return remaining <= std::numeric_limits<std::uint64_t>::max() -
                            entry.seqId &&
           entry.groupTailSeqId == entry.seqId + remaining &&
           entry.groupSourceIndex <=
               std::numeric_limits<std::uint64_t>::max() -
                   entry.groupHeadSourceOrdinal &&
           entry.sourceOrdinal ==
               entry.groupHeadSourceOrdinal + entry.groupSourceIndex;
  }

  GroupCompletionStatus groupCompletionStatus(
      const Entry& group) const noexcept {
    if (!groupIdentityValid(group)) {
      return GroupCompletionStatus::Invalid;
    }
    if (group.groupSourceCount <= 1u) {
      return GroupCompletionStatus::Complete;
    }
    std::size_t observed = 0;
    std::size_t firstObserved = group.groupSourceCount;
    bool complete = true;
    std::array<bool, kMaxArenaBatchSources> indices{};
    for (std::size_t i = 0; i < capacity(); ++i) {
      const auto& member = entries_[i];
      if (member.groupRawOrdinal != group.groupRawOrdinal ||
          member.groupHeadSourceOrdinal != group.groupHeadSourceOrdinal ||
          member.groupBuildGeneration != group.groupBuildGeneration ||
          member.groupTailSeqId != group.groupTailSeqId ||
          member.groupSourceCount != group.groupSourceCount) {
        continue;
      }
      ++observed;
      if (!groupIdentityValid(member) ||
          member.groupSourceIndex >= group.groupSourceCount ||
          member.groupSourceIndex >= indices.size() ||
          indices[member.groupSourceIndex]) {
        return GroupCompletionStatus::Invalid;
      }
      indices[member.groupSourceIndex] = true;
      firstObserved = std::min(firstObserved,
                               static_cast<std::size_t>(
                                   member.groupSourceIndex));
      if (member.state == State::Free || member.state == State::Writing ||
          member.state == State::Sealed) {
        return GroupCompletionStatus::Invalid;
      }
      if (member.state != State::Completed &&
          member.state != State::Reclaiming) {
        complete = false;
      }
    }
    // Members already reclaimed are intentionally absent from the ring, but
    // only a reclaimed prefix may be absent.  A missing future member must
    // fail closed rather than accidentally turning a forged/ABA group into a
    // reclaimable tail.
    if (observed == 0 || firstObserved >= group.groupSourceCount) {
      return GroupCompletionStatus::Invalid;
    }
    for (std::size_t i = firstObserved; i < group.groupSourceCount; ++i) {
      if (!indices[i]) {
        return GroupCompletionStatus::Invalid;
      }
    }
    return complete ? GroupCompletionStatus::Complete
                    : GroupCompletionStatus::Pending;
  }

  bool groupMembersCompleted(const Entry& group) const noexcept {
    return groupCompletionStatus(group) == GroupCompletionStatus::Complete;
  }

  struct Page {
    CpuReadySourceId owner{};
    std::uint64_t generation = 0;
    bool allocated = false;
  };

  static std::uint64_t nextGeneration(std::uint64_t current) noexcept {
    ++current;
    return current == 0 ? 1 : current;
  }

  static bool addLeaseCapacityClaim(
      LeaseCapacityClaim& total,
      const LeaseCapacityClaim& claim) noexcept {
    const auto add = [](std::uint64_t& value,
                        std::uint64_t increment) noexcept {
      if (increment > std::numeric_limits<std::uint64_t>::max() - value) {
        return false;
      }
      value += increment;
      return true;
    };
    return add(total.sources, claim.sources) &&
           add(total.pages, claim.pages) &&
           add(total.bytes, claim.bytes) &&
           add(total.payloadBlocks, claim.payloadBlocks) &&
           add(total.readyEntries, claim.readyEntries) &&
           add(total.retentionEntries, claim.retentionEntries) &&
           add(total.allocatorTickets, claim.allocatorTickets);
  }

  bool leaseCapacityClaimFor(const Entry& entry,
                             LeaseCapacityClaim& claim) const noexcept {
    const std::uint64_t padding = entry.paddingBefore;
    const std::uint64_t pages = entry.storage.pageCount;
    if (!entry.storage.valid() ||
        pages > std::numeric_limits<std::uint64_t>::max() - padding) {
      return false;
    }
    std::uint64_t bytes = entry.plannedBytes;
    if (entry.payloadKind == PayloadKind::Legacy) {
      if (pages != 0 && config_.values().pageSize >
                            std::numeric_limits<std::uint64_t>::max() / pages) {
        return false;
      }
      bytes = pages * config_.values().pageSize;
    }
    if (bytes == 0) {
      return false;
    }
    claim = {
        .sources = 1,
        .pages = padding + pages,
        .bytes = bytes,
        .payloadBlocks =
            std::max<std::uint64_t>(1, entry.arenaPayloadCount),
        .readyEntries = entry.readyPublicationReserved ? 1u : 0u,
        .retentionEntries = 1,
        .allocatorTickets = 1,
    };
    return true;
  }

  bool orderedTailWritingEntryValid(std::size_t index,
                                    const Entry& entry) const noexcept {
    if (residentCount_ == 0 || index != previousSourceIndex(sourceTail_) ||
        !entry.readyPublicationReserved ||
        readyPublicationReservations_ == 0 || entry.arenaOwnerDetached) {
      return false;
    }
    const CpuReadySourceId id{
        .index = static_cast<std::uint32_t>(index),
        .generation = entry.generation,
    };
    CpuReadyPublicationTicket ticket{
        .id = id,
        .storage = entry.storage,
    };
    if (entry.strictAdmission) {
      ticket.rawOrdinal = entry.rawOrdinal;
      ticket.sourceOrdinal = entry.sourceOrdinal;
      ticket.seqId = entry.seqId;
      ticket.buildGeneration = entry.buildGeneration;
      ticket.producerIdentity = entry.producerIdentity;
    }
    if (resolveEntry(id, entry.storage) != &entry ||
        !ticketMatchesEntry(ticket, entry)) {
      return false;
    }
    if (entry.payloadKind == PayloadKind::Legacy) {
      return compatibilityPayload(entry) != nullptr;
    }
    if (entry.arenaPayloadCount == 0) {
      return false;
    }
    for (std::size_t i = 0; i < entry.arenaPayloadCount; ++i) {
      if (!arenaOwner(index, i).constructed) {
        return false;
      }
    }
    return true;
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
           ticket.buildGeneration == entry.buildGeneration &&
           ticket.producerIdentity == entry.producerIdentity;
  }

  static CpuReadySourceMetadata metadataForEntry(
      const Entry& entry) noexcept {
    return CpuReadySourceMetadata{
        .rawOrdinal = entry.rawOrdinal,
        .sourceOrdinal = entry.sourceOrdinal,
        .seqId = entry.seqId,
        .buildGeneration = entry.buildGeneration,
        .usedBytes = entry.usedBytes,
        .pageCount = entry.storage.pageCount,
        .paddingPagesBefore = static_cast<std::uint32_t>(entry.paddingBefore),
        .strictAdmission = entry.strictAdmission,
    };
  }

  static bool metadataMatchesEntry(CpuReadySourceMetadata metadata,
                                   const Entry& entry) noexcept {
    return metadata.valid() && metadata == metadataForEntry(entry);
  }

  static SourceSemanticSummary makeSealedSemanticSummary(
      SourcePayloadView payload,
      std::size_t usedBytes,
      std::uint32_t pageCount) noexcept {
    // Publication is the first point where the complete immutable payload is
    // visible. Keep route and resource canonicalization conservative here;
    // exact replay remains the authority for render-pass continuation.
    return summarizeSourcePayload(
        payload,
        SourceSemanticSummaryContext{
            .byteCount = usedBytes,
            .pageCount = pageCount,
            .firstRenderRoute = RenderRoute::Unknown,
            .passActionEpoch = 1,
            .sealed = true,
            .entryStable = payload.valid(),
            .resourcesCanonicalized = false,
        });
  }

  static bool semanticMatchesEntry(
      const SourceSemanticSummary& semantic,
      const Entry& entry) noexcept {
    return semantic.sealed() && semantic == entry.semantic &&
           semantic.byteCount == entry.usedBytes &&
           semantic.pageCount == entry.storage.pageCount;
  }

  bool entryMatches(const Entry* entry, std::uint64_t seqId,
                    State expected) const noexcept {
    return entry && entry->state == expected &&
           expected != State::Writing && expected != State::Sealed &&
           expected != State::Reclaiming && seqId != 0 &&
           entry->seqId == seqId &&
           (entry->payloadKind == PayloadKind::Legacy
                ? compatibilityPayload(*entry) &&
                      compatibilityPayload(*entry)->seqId == seqId
                : entry->arenaPayloadCount != 0 &&
                      (entry->arenaPayloadCount == 1
                           ? arenaPayload(*entry) &&
                                 arenaPayload(*entry)->readable()
                           : entry->arenaChain.readable()));
  }

  bool validateTentativePrefix(
      std::span<const ReadyEntry> selected) const noexcept {
    if (selected.empty() ||
        selected.size() != tentativeRepresentationCount_) {
      return false;
    }
    for (std::size_t i = 0; i < selected.size(); ++i) {
      const auto& ready = selected[i];
      if (ready != tentativeReadyPrefix_[i]) {
        return false;
      }
      const auto* entry = resolveEntry(ready.source.id, ready.source.storage);
      if (!ready.valid() || !entry ||
          entry->state != State::TentativeRepresented ||
          !entry->readyPublicationReserved ||
          !metadataMatchesEntry(ready.metadata, *entry) ||
          !semanticMatchesEntry(ready.semantic, *entry)) {
        return false;
      }
      for (std::size_t j = 0; j < i; ++j) {
        if (selected[j].source.id == ready.source.id) {
          return false;
        }
      }
    }
    return true;
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

  ReserveProbe inspectReserve(
      std::size_t pageCount, PayloadKind payloadKind,
      std::size_t arenaPayloadCount = 1) const noexcept {
    if (!validPageRequest(pageCount) ||
        (payloadKind == PayloadKind::Arena &&
         (arenaPayloadCount == 0 ||
          arenaPayloadCount > kMaxArenaSourcePayloadSegments))) {
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
    bool arenaOwnersFree = true;
    if (payloadKind == PayloadKind::Arena) {
      for (std::size_t i = 0; i < arenaPayloadCount; ++i) {
        arenaOwnersFree &= !arenaOwner(sourceTail_, i).constructed;
      }
    }
    if (entries_[sourceTail_].state != State::Free ||
        (payloadKind == PayloadKind::Legacy &&
         findFreeCompatibilityPayload() == kInvalidIndex) ||
        !arenaOwnersFree ||
        !allocationPagesFree(firstPage, pageCount, padding)) {
      return ReserveProbe::Corrupt;
    }
    return ReserveProbe::Ready;
  }

  std::optional<Reservation> reserveStorageImpl(
      std::size_t pageCount, PayloadKind payloadKind,
      std::size_t arenaPayloadCount = 1) noexcept {
    const std::size_t compatibilityIndex =
        payloadKind == PayloadKind::Legacy
            ? findFreeCompatibilityPayload()
            : kInvalidIndex;
    const std::size_t padding = wrapPaddingFor(pageCount);
    const std::size_t firstPage = padding != 0 ? 0 : pageTail_;
    auto& entry = entries_[sourceTail_];
    bool arenaOwnersFree = true;
    if (payloadKind == PayloadKind::Arena) {
      for (std::size_t i = 0; i < arenaPayloadCount; ++i) {
        arenaOwnersFree &= !arenaOwner(sourceTail_, i).constructed;
      }
    }
    if (entry.state != State::Free ||
        (payloadKind == PayloadKind::Legacy &&
         compatibilityIndex == kInvalidIndex) ||
        (payloadKind == PayloadKind::Arena &&
         (arenaPayloadCount == 0 ||
          arenaPayloadCount > kMaxArenaSourcePayloadSegments ||
          !arenaOwnersFree)) ||
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
    std::array<ArenaSourcePayloadBlock*,
               kMaxArenaSourcePayloadSegments> arenaPayloadPtrs{};
    if (payloadKind == PayloadKind::Legacy) {
      compatibilityPayloadPtr = &compatibilityPayloads_[compatibilityIndex];
      compatibilityPayloadPtr->clearCommands();
      compatibilityPayloadPtr->seqId = 0;
      compatibilityOwners_[compatibilityIndex] = id;
      ++compatibilityResident_;
      ++compatibilityWritingCount_;
    } else {
      for (std::size_t i = 0; i < arenaPayloadCount; ++i) {
        auto& owner = arenaOwner(sourceTail_, i);
        arenaPayloadPtrs[i] = std::construct_at(owner.storageAddress());
        owner.constructed = true;
      }
    }

    entry.state = State::Writing;
    entry.storage = storage;
    entry.paddingFirstPage = pageTail_;
    entry.paddingBefore = padding;
    entry.usedBytes = 0;
    entry.plannedBytes = 0;
    entry.payloadKind = payloadKind;
    entry.compatibilityIndex = compatibilityIndex;
    entry.arenaPayloadCount = payloadKind == PayloadKind::Arena
                                  ? arenaPayloadCount
                                  : 0;
    entry.arenaExtents = {};
    entry.arenaChain.clear();
    entry.rawOrdinal = 0;
    entry.sourceOrdinal = 0;
    entry.seqId = 0;
    entry.buildGeneration = 0;
    entry.producerIdentity = {};
    entry.spanWitness = {};
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
        .arenaPayload = arenaPayloadPtrs[0],
        .arenaPayloads = arenaPayloadPtrs,
        .arenaPayloadCount = payloadKind == PayloadKind::Arena
                                 ? arenaPayloadCount
                                 : 0,
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

  CpuReadyArenaOwnerSlot& arenaOwner(
      std::size_t sourceIndex, std::size_t segmentIndex) noexcept {
    return arenaOwners_[sourceIndex * kMaxArenaSourcePayloadSegments +
                        segmentIndex];
  }

  const CpuReadyArenaOwnerSlot& arenaOwner(
      std::size_t sourceIndex,
      std::size_t segmentIndex) const noexcept {
    return arenaOwners_[sourceIndex * kMaxArenaSourcePayloadSegments +
                        segmentIndex];
  }

  ArenaSourcePayloadBlock* arenaPayload(Entry& entry) noexcept {
    const std::size_t index = static_cast<std::size_t>(&entry - entries_.get());
    if (entry.payloadKind != PayloadKind::Arena || index >= capacity() ||
        entry.arenaPayloadCount == 0 ||
        !arenaOwner(index, 0).constructed || entry.arenaOwnerDetached) {
      return nullptr;
    }
    return arenaOwner(index, 0).payload();
  }

  const ArenaSourcePayloadBlock* arenaPayload(const Entry& entry) const noexcept {
    const std::size_t index = static_cast<std::size_t>(&entry - entries_.get());
    if (entry.payloadKind != PayloadKind::Arena || index >= capacity() ||
        entry.arenaPayloadCount == 0 ||
        !arenaOwner(index, 0).constructed || entry.arenaOwnerDetached) {
      return nullptr;
    }
    return arenaOwner(index, 0).payload();
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
  std::unique_ptr<ReadyEntry[]> tentativeReadyPrefix_{};
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
  std::size_t tentativeRepresentationCount_ = 0;
  std::uint64_t nextPageAllocationGeneration_ = 0;
  std::uint64_t rawOrdinalHighWater_ = 0;
  std::uint64_t sourceOrdinalHighWater_ = 0;
  std::uint64_t seqIdHighWater_ = 0;
  std::uint8_t closedPressureDimensions_ = 0;
  bool strictWritingActive_ = false;
  ArenaBatchReserveFailure lastArenaBatchReserveFailure_ =
      ArenaBatchReserveFailure::None;
  bool stopped_ = false;
  mutable Stats stats_{};
};

}  // namespace dxmt9::core
