#pragma once

#include "device_c_chunk_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

enum class CommandChunkValidationStatus : std::uint8_t {
  Valid,
  MissingHeader,
  OuterVersionMismatch,
  OuterCountMismatch,
  InvalidHeader,
  InvalidSourceIdentity,
  InvalidChunkRange,
  InvalidAlignment,
  NonCanonicalChunkLayout,
  NonZeroReserved,
  InvalidHandleEntry,
  InvalidRecordType,
  InvalidRecordFlags,
  NonCanonicalHandleSlice,
  InvalidPayloadRange,
  InvalidPayloadSize,
  NonCanonicalPayloadLayout,
  InvalidDrawHeader,
  InvalidSectionTable,
  InvalidSectionOrder,
  InvalidSectionSchema,
  InvalidSectionRange,
  NonZeroPadding,
  InvalidHandleReference,
  HandleSliceMismatch,
  InvalidFullSnapshot,
  InvalidConstantRange,
  InvalidUpData,
  ScratchAllocationFailed,
};

struct CommandChunkEnvelope {
  std::uint32_t version = D9C_COMMAND_CHUNK_VERSION;
  std::uint32_t recordCount = 0u;
  std::uint32_t handleCount = 0u;
  D9CCommandChunkProducerIdentity producerIdentity{};
};

struct ImportedSectionView {
  D9CCommandChunkWireSectionDesc descriptor{};
  std::span<const std::byte> payload{};
};

struct ImportedRecordView {
  D9CCommandChunkWireRecordHeader header{};
  std::span<const std::byte> payload{};
  std::span<const D9CCommandChunkWireHandleEntry> handles{};
  D9CCommandChunkWireDrawHeader drawHeader{};
  std::span<const D9CCommandChunkWireSectionDesc> sections{};

  bool sparseState() const noexcept {
    const auto* rule = recordRule(header.type);
    return rule && (rule->ruleFlags & RecordRuleSparseState) != 0u;
  }
  ImportedSectionView section(std::size_t index) const noexcept;
};

struct ImportedChunkView {
  D9CCommandChunkWireHeader header{};
  std::span<const D9CCommandChunkWireRecordHeader> records{};
  std::span<const D9CCommandChunkWireHandleEntry> handles{};
  std::span<const std::byte> payloadArena{};

  bool empty() const noexcept { return records.empty(); }
  ImportedRecordView record(std::size_t index) const noexcept;
};

struct CommandChunkValidationResult {
  CommandChunkValidationStatus status = CommandChunkValidationStatus::MissingHeader;
  std::uint32_t failedRecordIndex = 0xffffffffu;
  std::uint32_t failedSectionIndex = 0xffffffffu;
  std::uint32_t failedHandleIndex = 0xffffffffu;
  std::uint32_t byteOffset = 0u;

  bool valid() const noexcept { return status == CommandChunkValidationStatus::Valid; }
};

struct CommandChunkValidationScratch {
  std::vector<std::uint8_t> referencedHandles;
};

CommandChunkValidationResult validateCommandChunk(
    std::span<const std::byte> blob, const CommandChunkEnvelope& envelope,
    ImportedChunkView* out, CommandChunkValidationScratch& scratch) noexcept;

CommandChunkValidationResult validateCommandChunk(
    std::span<const std::byte> blob, const CommandChunkEnvelope& envelope,
    ImportedChunkView* out = nullptr) noexcept;

// Rebuilds the span-only imported view in constant time after the exact,
// immutable blob has already passed validateCommandChunk(). This performs
// only the bounds/alignment checks needed to construct safe spans; it is not
// a substitute for transactional admission validation.
bool importPrevalidatedCommandChunk(
    std::span<const std::byte> blob, const CommandChunkEnvelope& envelope,
    ImportedChunkView& out) noexcept;

CommandChunkValidationResult validateSegmentedCommandChunk(
    const D9CCommandChunkSegmentedTransportV1& transport,
    std::span<const std::byte> records, std::span<const std::byte> handles,
    std::span<const std::byte> payload, const CommandChunkEnvelope& envelope,
    ImportedChunkView* out = nullptr) noexcept;

bool importPrevalidatedSegmentedCommandChunk(
    const D9CCommandChunkWireHeader& header,
    std::span<const std::byte> records, std::span<const std::byte> handles,
    std::span<const std::byte> payload, const CommandChunkEnvelope& envelope,
    ImportedChunkView& out) noexcept;

}  // namespace dxmt9::d3d9
