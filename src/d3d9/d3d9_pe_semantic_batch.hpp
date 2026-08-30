#pragma once

// Host-buildable semantic-batch owner for the fixed-role CPU Tape seam.
//
// This owner is intentionally not wired into D3D9DeviceImpl or CPU-ready Tape
// in this increment. The production adapter may synchronously translate a
// validated segmented batch into BatchRecordInput values and bind the copied
// FixedRoleOwnership witness. Inputs must already use canonical record-local
// unique identities; duplicate identities reject instead of silently changing
// payload handle indices. The pure pass verifies/counts that canonical form.
// No span, COM pointer, or role pointer is retained by the owner. The later
// ExactFixed pass writes one caller-owned contiguous extent.
// Keeping this algebra separate also makes the transaction boundary testable
// without windows.h, Wine, Metal, or an allocator-faulting bridge.

#include "d3d9_pe_semantic_tokens.hpp"
#include "d3d9_pe_wire_handle.hpp"
#include "device_c_chunk_schema.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

namespace dxmt9::d3d9::pe::semantic_batch {

inline constexpr std::size_t kDefaultMaxRecords = 64u;
inline constexpr std::size_t kDefaultMaxPayloadBytes = 1u << 20u;
inline constexpr std::size_t kDefaultMaxIdentities = 1024u;

struct FixedRoleOwnership {
  std::uint32_t recordCount = 0u;
  std::uint32_t handleCount = 0u;
  std::uint32_t recordBytes = 0u;
  std::uint32_t handleBytes = 0u;
  std::uint32_t payloadBytes = 0u;
  std::uint64_t captureToken = 0u;
  std::uint64_t eventOrdinal = 0u;
  bool bound = false;

  bool valid() const noexcept {
    return bound && recordCount != 0u &&
           (captureToken == 0u) == (eventOrdinal == 0u) &&
           static_cast<std::uint64_t>(recordBytes) + handleBytes +
                   payloadBytes <=
               D9C_COMMAND_CHUNK_MAX_TOTAL_WIRE_BYTES;
  }
};

// This input is borrowed only while append() copies it into the owner. The
// pending fields are a witness, not a second value owner: the integration
// adapter supplies the exact PendingDelta key/value it already owns.
struct BatchRecordInput {
  PeSemanticProducerKind producer = PeSemanticProducerKind::DrawPrimitive;
  std::uint32_t recordType = 0u;
  std::uint32_t recordFlags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE;
  std::span<const std::byte> exactPayload{};
  std::span<const PeCommittedSemanticIdentityValue> identities{};
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;
  bool pendingWitness = false;
  std::uint64_t pendingKey = 0u;
  std::uint64_t pendingValue = 0u;
  bool captureWitness = false;
  bool retainerWitness = false;
};

struct OwnedBatchRecord {
  PeSemanticProducerKind producer = PeSemanticProducerKind::DrawPrimitive;
  PeSemanticProducerCategory category = PeSemanticProducerCategory::Draw;
  std::uint32_t recordType = 0u;
  std::uint32_t recordFlags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE;
  std::uint32_t payloadOffset = 0u;
  std::uint32_t payloadBytes = 0u;
  std::uint32_t identityOffset = 0u;
  std::uint32_t identityCount = 0u;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;
  std::uint64_t pendingKey = 0u;
  std::uint64_t pendingValue = 0u;
  bool pendingWitness = false;
  bool captureWitness = false;
  bool retainerWitness = false;
};

template <std::size_t MaxRecords = kDefaultMaxRecords,
          std::size_t MaxPayloadBytes = kDefaultMaxPayloadBytes,
          std::size_t MaxIdentities = kDefaultMaxIdentities>
class ImmutableSemanticBatchOwner final {
 public:
  static constexpr std::size_t maxRecords = MaxRecords;
  static constexpr std::size_t maxPayloadBytes = MaxPayloadBytes;
  static constexpr std::size_t maxIdentities = MaxIdentities;

  ImmutableSemanticBatchOwner() = default;
  ImmutableSemanticBatchOwner(const ImmutableSemanticBatchOwner&) = delete;
  ImmutableSemanticBatchOwner& operator=(const ImmutableSemanticBatchOwner&) = delete;

  bool bindSegmentedOwnership(
      const D9CCommandChunkSegmentedTransportV1& transport) noexcept {
    const auto alignUp = [](std::uint64_t value,
                            std::uint64_t alignment) noexcept {
      return (value + alignment - 1u) & ~(alignment - 1u);
    };
    const auto expectedRecordBytes =
        static_cast<std::uint64_t>(transport.header.recordCount) *
        D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE;
    const auto expectedHandleBytes =
        static_cast<std::uint64_t>(transport.header.handleCount) *
        D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE;
    const auto expectedHandleOffset = alignUp(
        transport.header.headerSize + expectedRecordBytes,
        alignof(D9CCommandChunkWireHandleEntry));
    const auto expectedPayloadOffset = alignUp(
        expectedHandleOffset + expectedHandleBytes, alignof(std::uint32_t));
    const auto expectedWireBytes = expectedPayloadOffset +
        transport.header.payloadArenaSize;
    const auto roleMatches = [](D9CWireHandle role, std::uint32_t bytes) {
      return (bytes == 0u) == (d9cWireHandleValue(role) == 0u);
    };
    if (frozen_ || role_.bound ||
        transport.header.version != D9C_COMMAND_CHUNK_WIRE_VERSION ||
        transport.header.headerSize != D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE ||
        transport.header.recordHeaderSize !=
            D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE ||
        transport.header.handleEntrySize !=
            D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE ||
        transport.header.recordTableOffset != transport.header.headerSize ||
        transport.recordBytes != expectedRecordBytes ||
        transport.handleBytes != expectedHandleBytes ||
        transport.payloadBytes != transport.header.payloadArenaSize ||
        transport.header.handleTableOffset != expectedHandleOffset ||
        transport.header.payloadArenaOffset != expectedPayloadOffset ||
        expectedWireBytes > D9C_COMMAND_CHUNK_MAX_TOTAL_WIRE_BYTES ||
        transport.recordReserved != 0u || transport.handleReserved != 0u ||
        transport.payloadReserved != 0u ||
        !roleMatches(transport.records, transport.recordBytes) ||
        !roleMatches(transport.handles, transport.handleBytes) ||
        !roleMatches(transport.payload, transport.payloadBytes)) {
      return false;
    }
    role_ = FixedRoleOwnership{
        .recordCount = transport.header.recordCount,
        .handleCount = transport.header.handleCount,
        .recordBytes = transport.recordBytes,
        .handleBytes = transport.handleBytes,
        .payloadBytes = transport.payloadBytes,
        .captureToken = transport.renderTapeCaptureToken,
        .eventOrdinal = transport.renderTapeEventOrdinal,
        .bound = true,
    };
    return role_.valid();
  }

  bool append(const BatchRecordInput& input) noexcept {
    if (frozen_ || count_ == MaxRecords || input.exactPayload.empty() ||
        input.exactPayload.size() > MaxPayloadBytes - payloadBytes_ ||
        input.identities.size() > MaxIdentities - identityCount_ ||
        input.sourceOrdinal == 0u || input.recordOrdinal == 0u) {
      return false;
    }
    const auto* policy = peSemanticProducerPolicy(input.recordType);
    const auto* wireRule = recordRule(input.recordType);
    if (!policy || !wireRule || policy->kind != input.producer ||
        (input.recordFlags & ~wireRule->allowedRecordFlags) != 0u ||
        input.exactPayload.size() < wireRule->fixedPayloadSize ||
        input.exactPayload.size() > std::numeric_limits<std::uint32_t>::max() ||
        (input.pendingWitness && input.pendingKey == 0u) ||
        (input.captureWitness && !role_.bound) ||
        (input.retainerWitness && input.identities.empty())) {
      return false;
    }
    if (count_ != 0u && input.recordOrdinal <= records_[count_ - 1u].recordOrdinal) {
      return false;
    }
    if (input.captureWitness &&
        (role_.captureToken == 0u || role_.eventOrdinal == 0u)) {
      return false;
    }
    for (std::size_t i = 0u; i < input.identities.size(); ++i) {
      const auto& identity = input.identities[i];
      if (identity.kind > D9C_CHUNK_HANDLE_KIND_QUERY ||
          identity.generation == 0u || identity.objectId == 0u) {
        return false;
      }
      for (std::size_t prior = 0u; prior < i; ++prior) {
        if (input.identities[prior] == identity) return false;
      }
    }
    const auto alignment = static_cast<std::size_t>(wireRule->payloadAlignment);
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u ||
        canonicalPayloadBytes_ >
            std::numeric_limits<std::size_t>::max() - (alignment - 1u)) {
      return false;
    }
    const auto alignedPayload =
        (canonicalPayloadBytes_ + alignment - 1u) & ~(alignment - 1u);
    if (alignedPayload > MaxPayloadBytes ||
        input.exactPayload.size() > MaxPayloadBytes - alignedPayload) {
      return false;
    }
    std::copy(input.exactPayload.begin(), input.exactPayload.end(),
              payload_.begin() + payloadBytes_);
    std::copy(input.identities.begin(), input.identities.end(),
              identities_.begin() + identityCount_);
    records_[count_++] = OwnedBatchRecord{
        .producer = input.producer,
        .category = policy->category,
        .recordType = input.recordType,
        .recordFlags = input.recordFlags,
        .payloadOffset = static_cast<std::uint32_t>(payloadBytes_),
        .payloadBytes = static_cast<std::uint32_t>(input.exactPayload.size()),
        .identityOffset = static_cast<std::uint32_t>(identityCount_),
        .identityCount = static_cast<std::uint32_t>(input.identities.size()),
        .sourceOrdinal = input.sourceOrdinal,
        .recordOrdinal = input.recordOrdinal,
        .pendingKey = input.pendingKey,
        .pendingValue = input.pendingValue,
        .pendingWitness = input.pendingWitness,
        .captureWitness = input.captureWitness,
        .retainerWitness = input.retainerWitness,
    };
    payloadBytes_ += input.exactPayload.size();
    canonicalPayloadBytes_ = alignedPayload + input.exactPayload.size();
    identityCount_ += input.identities.size();
    return true;
  }

  bool freeze() noexcept {
    if (frozen_ || count_ == 0u || !role_.valid() ||
        role_.recordCount != count_ || role_.handleCount != identityCount_ ||
        role_.recordBytes != count_ * sizeof(D9CCommandChunkWireRecordHeader) ||
        role_.handleBytes !=
            identityCount_ * sizeof(D9CCommandChunkWireHandleEntry) ||
        role_.payloadBytes != canonicalPayloadBytes_) {
      return false;
    }
    frozen_ = true;
    return true;
  }

  bool frozen() const noexcept { return frozen_; }
  std::size_t size() const noexcept { return frozen_ ? count_ : 0u; }
  const OwnedBatchRecord& record(std::size_t index) const noexcept {
    return records_[index];
  }
  std::span<const std::byte> payload(const OwnedBatchRecord& record) const noexcept {
    return std::span<const std::byte>(payload_.data() + record.payloadOffset,
                                      record.payloadBytes);
  }
  std::span<const PeCommittedSemanticIdentityValue> identities(
      const OwnedBatchRecord& record) const noexcept {
    return std::span<const PeCommittedSemanticIdentityValue>(
        identities_.data() + record.identityOffset, record.identityCount);
  }
  const FixedRoleOwnership& role() const noexcept { return role_; }

 private:
  std::array<OwnedBatchRecord, MaxRecords> records_{};
  std::array<std::byte, MaxPayloadBytes> payload_{};
  std::array<PeCommittedSemanticIdentityValue, MaxIdentities> identities_{};
  FixedRoleOwnership role_{};
  std::size_t count_ = 0u;
  std::size_t payloadBytes_ = 0u;
  std::size_t canonicalPayloadBytes_ = 0u;
  std::size_t identityCount_ = 0u;
  bool frozen_ = false;
};

struct SemanticBatchPlanRecord {
  std::uint32_t payloadOffset = 0u;
  std::uint32_t payloadBytes = 0u;
  std::uint32_t firstHandle = 0u;
  std::uint32_t handleCount = 0u;

  friend bool operator==(const SemanticBatchPlanRecord&,
                         const SemanticBatchPlanRecord&) = default;
};

template <std::size_t MaxRecords = kDefaultMaxRecords,
          std::size_t MaxPayloadBytes = kDefaultMaxPayloadBytes,
          std::size_t MaxIdentities = kDefaultMaxIdentities>
struct SemanticBatchCountPlan {
  std::array<SemanticBatchPlanRecord, MaxRecords> records{};
  std::array<PeCommittedSemanticIdentityValue, MaxIdentities> handles{};
  std::size_t recordCount = 0u;
  std::size_t handleCount = 0u;
  std::size_t payloadBytes = 0u;
  std::size_t wireBytes = 0u;
  std::size_t recordTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
  std::size_t handleTableOffset = 0u;
  std::size_t payloadArenaOffset = 0u;
  std::size_t uniqueRetainerCount = 0u;
  std::size_t pendingWitnessCount = 0u;
  std::size_t captureWitnessCount = 0u;

  bool valid() const noexcept {
    return recordCount != 0u && recordCount <= MaxRecords &&
           handleCount <= MaxIdentities && payloadBytes <= MaxPayloadBytes &&
           wireBytes != 0u && wireBytes <= D9C_COMMAND_CHUNK_MAX_TOTAL_WIRE_BYTES;
  }
};

inline bool semanticBatchAlignUp(std::size_t value, std::size_t alignment,
                                 std::size_t& out) noexcept {
  if (alignment == 0u || (alignment & (alignment - 1u)) != 0u ||
      value > std::numeric_limits<std::size_t>::max() - (alignment - 1u)) {
    return false;
  }
  out = (value + alignment - 1u) & ~(alignment - 1u);
  return true;
}

template <std::size_t MaxRecords, std::size_t MaxPayloadBytes,
          std::size_t MaxIdentities>
bool planSemanticBatch(
    const ImmutableSemanticBatchOwner<MaxRecords, MaxPayloadBytes,
                                      MaxIdentities>& owner,
    SemanticBatchCountPlan<MaxRecords, MaxPayloadBytes, MaxIdentities>& out) noexcept {
  out = {};
  if (!owner.frozen()) return false;
  std::size_t payloadEnd = 0u;
  for (std::size_t i = 0u; i < owner.size(); ++i) {
    const auto& source = owner.record(i);
    const auto* rule = recordRule(source.recordType);
    std::size_t alignedPayload = 0u;
    if (!rule || !semanticBatchAlignUp(payloadEnd, rule->payloadAlignment,
                                       alignedPayload) ||
        alignedPayload > MaxPayloadBytes ||
        source.payloadBytes > MaxPayloadBytes - alignedPayload ||
        out.recordCount == MaxRecords) {
      return false;
    }
    const auto identities = owner.identities(source);
    const auto firstHandle = out.handleCount;
    for (const auto& identity : identities) {
      if (out.handleCount == MaxIdentities) return false;
      out.handles[out.handleCount++] = identity;
    }
    if (source.retainerWitness) out.uniqueRetainerCount += identities.size();
    if (source.pendingWitness) ++out.pendingWitnessCount;
    if (source.captureWitness) ++out.captureWitnessCount;
    out.records[out.recordCount++] = SemanticBatchPlanRecord{
        .payloadOffset = static_cast<std::uint32_t>(alignedPayload),
        .payloadBytes = source.payloadBytes,
        .firstHandle = static_cast<std::uint32_t>(firstHandle),
        .handleCount = static_cast<std::uint32_t>(identities.size()),
    };
    payloadEnd = alignedPayload + source.payloadBytes;
  }
  std::size_t recordEnd = 0u;
  if (out.recordCount >
          (std::numeric_limits<std::size_t>::max() - out.recordTableOffset) /
              sizeof(D9CCommandChunkWireRecordHeader) ||
      !semanticBatchAlignUp(
          out.recordTableOffset +
              out.recordCount * sizeof(D9CCommandChunkWireRecordHeader),
          alignof(D9CCommandChunkWireHandleEntry), out.handleTableOffset) ||
      out.handleCount >
          (std::numeric_limits<std::size_t>::max() - out.handleTableOffset) /
              sizeof(D9CCommandChunkWireHandleEntry)) {
    return false;
  }
  recordEnd = out.handleTableOffset +
      out.handleCount * sizeof(D9CCommandChunkWireHandleEntry);
  if (!semanticBatchAlignUp(recordEnd, alignof(std::uint32_t),
                            out.payloadArenaOffset) ||
      out.payloadArenaOffset > std::numeric_limits<std::size_t>::max() -
                                    payloadEnd) {
    return false;
  }
  out.payloadBytes = payloadEnd;
  out.wireBytes = out.payloadArenaOffset + payloadEnd;
  return out.valid();
}

struct ExactFixedEmission {
  D9CCommandChunkSegmentedTransportV1 transport{};
  std::span<const std::byte> wire{};
  std::size_t wireBytes = 0u;

  bool valid() const noexcept {
    return wireBytes != 0u && wire.size() == wireBytes &&
           transport.header.recordCount != 0u;
  }
};

template <std::size_t MaxRecords, std::size_t MaxPayloadBytes,
          std::size_t MaxIdentities>
bool emitExactFixed(
    const ImmutableSemanticBatchOwner<MaxRecords, MaxPayloadBytes,
                                      MaxIdentities>& owner,
    const SemanticBatchCountPlan<MaxRecords, MaxPayloadBytes, MaxIdentities>& plan,
    std::span<std::byte> destination, ExactFixedEmission& out) noexcept {
  out = {};
  SemanticBatchCountPlan<MaxRecords, MaxPayloadBytes, MaxIdentities>
      verifiedPlan;
  if (!planSemanticBatch(owner, verifiedPlan) || !plan.valid() ||
      plan.recordCount != verifiedPlan.recordCount ||
      plan.handleCount != verifiedPlan.handleCount ||
      plan.payloadBytes != verifiedPlan.payloadBytes ||
      plan.wireBytes != verifiedPlan.wireBytes ||
      plan.recordTableOffset != verifiedPlan.recordTableOffset ||
      plan.handleTableOffset != verifiedPlan.handleTableOffset ||
      plan.payloadArenaOffset != verifiedPlan.payloadArenaOffset ||
      plan.uniqueRetainerCount != verifiedPlan.uniqueRetainerCount ||
      plan.pendingWitnessCount != verifiedPlan.pendingWitnessCount ||
      plan.captureWitnessCount != verifiedPlan.captureWitnessCount ||
      !std::equal(plan.records.begin(),
                  plan.records.begin() + plan.recordCount,
                  verifiedPlan.records.begin()) ||
      !std::equal(plan.handles.begin(),
                  plan.handles.begin() + plan.handleCount,
                  verifiedPlan.handles.begin()) ||
      destination.size() < plan.wireBytes ||
      reinterpret_cast<std::uintptr_t>(destination.data()) %
              alignof(D9CCommandChunkWireHandleEntry) != 0u) {
    return false;
  }
  std::fill(destination.begin(), destination.begin() + plan.wireBytes,
            std::byte{0});
  const auto header = D9CCommandChunkWireHeader{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(plan.recordTableOffset),
      .recordCount = static_cast<std::uint32_t>(plan.recordCount),
      .handleTableOffset = static_cast<std::uint32_t>(plan.handleTableOffset),
      .handleCount = static_cast<std::uint32_t>(plan.handleCount),
      .payloadArenaOffset = static_cast<std::uint32_t>(plan.payloadArenaOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(plan.payloadBytes),
      .reserved0 = 0u,
      .reserved1 = 0u,
  };
  std::copy(std::as_bytes(std::span{&header, 1u}).begin(),
            std::as_bytes(std::span{&header, 1u}).end(), destination.begin());
  for (std::size_t i = 0u; i < plan.recordCount; ++i) {
    const auto& source = owner.record(i);
    const auto& record = plan.records[i];
    const auto wireRecord = D9CCommandChunkWireRecordHeader{
        .type = source.recordType,
        .flags = source.recordFlags,
        .payloadOffset = record.payloadOffset,
        .payloadSize = record.payloadBytes,
        .firstHandle = record.firstHandle,
        .handleCount = record.handleCount,
        .reserved0 = 0u,
        .reserved1 = 0u,
    };
    std::memcpy(destination.data() + plan.recordTableOffset +
                    i * sizeof(D9CCommandChunkWireRecordHeader),
                &wireRecord, sizeof(wireRecord));
    std::copy(owner.payload(source).begin(), owner.payload(source).end(),
              destination.begin() + plan.payloadArenaOffset + record.payloadOffset);
  }
  for (std::size_t i = 0u; i < plan.handleCount; ++i) {
    const auto wireHandle = D9CCommandChunkWireHandleEntry{
        .kind = plan.handles[i].kind,
        .generation = plan.handles[i].generation,
        .objectId = plan.handles[i].objectId,
    };
    std::memcpy(destination.data() + plan.handleTableOffset +
                    i * sizeof(D9CCommandChunkWireHandleEntry),
                &wireHandle, sizeof(wireHandle));
  }
  out.transport.header = header;
  out.transport.records = toWireHandle(destination.data() + plan.recordTableOffset);
  out.transport.recordBytes = static_cast<std::uint32_t>(
      plan.recordCount * sizeof(D9CCommandChunkWireRecordHeader));
  out.transport.handles = toWireHandle(destination.data() + plan.handleTableOffset);
  out.transport.handleBytes = static_cast<std::uint32_t>(
      plan.handleCount * sizeof(D9CCommandChunkWireHandleEntry));
  out.transport.payload = toWireHandle(destination.data() + plan.payloadArenaOffset);
  out.transport.payloadBytes = static_cast<std::uint32_t>(plan.payloadBytes);
  out.transport.renderTapeCaptureToken = owner.role().captureToken;
  out.transport.renderTapeEventOrdinal = owner.role().eventOrdinal;
  out.wire = std::span<const std::byte>(destination.data(), plan.wireBytes);
  out.wireBytes = plan.wireBytes;
  return out.valid();
}

enum class BatchPhase : std::uint8_t {
  Checkpoint,
  Reserved,
  Adopted,
  Emitted,
  Settled,
  RolledBack,
  Poisoned,
};

// Typed settlement owner for the role boundary. It is deliberately separate
// from ImmutableSemanticBatchOwner: the latter becomes const after freeze,
// while this small state machine is the only mutable transaction witness.
class SemanticBatchSettlement final {
 public:
  BatchPhase phase() const noexcept { return phase_; }
  bool fallbackUsed() const noexcept { return fallbackUsed_; }

  bool reserveAll(bool capacityAvailable) noexcept {
    if (phase_ != BatchPhase::Checkpoint) return false;
    if (!capacityAvailable) return false;  // CapacityPre wait, no effect.
    phase_ = BatchPhase::Reserved;
    return true;
  }

  bool adoptAll(bool complete) noexcept {
    if (phase_ != BatchPhase::Reserved) return false;
    phase_ = complete ? BatchPhase::Adopted : BatchPhase::Poisoned;
    return complete;
  }

  bool exactFixed(bool succeeded) noexcept {
    if (phase_ != BatchPhase::Adopted) return false;
    phase_ = succeeded ? BatchPhase::Emitted : BatchPhase::Poisoned;
    return succeeded;
  }

  bool settle(bool captureSettled, bool retainerSettled,
              bool capacityPostSucceeded) noexcept {
    if (phase_ != BatchPhase::Emitted) return false;
    phase_ = captureSettled && retainerSettled && capacityPostSucceeded
        ? BatchPhase::Settled
        : BatchPhase::Poisoned;
    return phase_ == BatchPhase::Settled;
  }

  bool rollbackPreEffect() noexcept {
    if (phase_ != BatchPhase::Checkpoint && phase_ != BatchPhase::Reserved)
      return false;
    phase_ = BatchPhase::RolledBack;
    return true;
  }

  bool fallbackOnce() noexcept {
    if (phase_ != BatchPhase::RolledBack || fallbackUsed_) return false;
    fallbackUsed_ = true;
    return true;
  }

 private:
  BatchPhase phase_ = BatchPhase::Checkpoint;
  bool fallbackUsed_ = false;
};

static_assert(std::is_trivially_copyable_v<BatchRecordInput>);
static_assert(std::is_trivially_copyable_v<OwnedBatchRecord>);
static_assert(sizeof(D9CCommandChunkSegmentedTransportV1) == 112u);

}  // namespace dxmt9::d3d9::pe::semantic_batch
