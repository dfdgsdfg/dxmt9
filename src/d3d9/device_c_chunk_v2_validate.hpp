#pragma once

#include "device_c_chunk_v2_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

enum class V2ValidationStatus : std::uint8_t {
  Valid,
  MissingHeader,
  OuterVersionMismatch,
  OuterCountMismatch,
  InvalidHeader,
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

struct V2ChunkEnvelope {
  std::uint32_t version = D9C_COMMAND_CHUNK_VERSION_V2;
  std::uint32_t recordCount = 0u;
  std::uint32_t handleCount = 0u;
};

struct ImportedSectionV2View {
  D9CCommandChunkWireSectionDescV2 descriptor{};
  std::span<const std::byte> payload{};
};

struct ImportedRecordV2View {
  D9CCommandChunkWireRecordHeaderV2 header{};
  std::span<const std::byte> payload{};
  std::span<const D9CCommandChunkWireHandleEntryV2> handles{};
  D9CCommandChunkWireDrawHeaderV2 drawHeader{};
  std::span<const D9CCommandChunkWireSectionDescV2> sections{};

  bool sparseState() const noexcept {
    const auto* rule = v2RecordRule(header.type);
    return rule && (rule->ruleFlags & V2RecordRuleSparseState) != 0u;
  }
  ImportedSectionV2View section(std::size_t index) const noexcept;
};

struct ImportedChunkV2View {
  D9CCommandChunkWireHeaderV2 header{};
  std::span<const D9CCommandChunkWireRecordHeaderV2> records{};
  std::span<const D9CCommandChunkWireHandleEntryV2> handles{};
  std::span<const std::byte> payloadArena{};

  bool empty() const noexcept { return records.empty(); }
  ImportedRecordV2View record(std::size_t index) const noexcept;
};

struct V2ValidationResult {
  V2ValidationStatus status = V2ValidationStatus::MissingHeader;
  std::uint32_t failedRecordIndex = 0xffffffffu;
  std::uint32_t failedSectionIndex = 0xffffffffu;
  std::uint32_t failedHandleIndex = 0xffffffffu;
  std::uint32_t byteOffset = 0u;

  bool valid() const noexcept { return status == V2ValidationStatus::Valid; }
};

struct V2ValidationScratch {
  std::vector<std::uint8_t> referencedHandles;
};

V2ValidationResult validateCommandChunkV2(
    std::span<const std::byte> blob, const V2ChunkEnvelope& envelope,
    ImportedChunkV2View* out, V2ValidationScratch& scratch) noexcept;

V2ValidationResult validateCommandChunkV2(
    std::span<const std::byte> blob, const V2ChunkEnvelope& envelope,
    ImportedChunkV2View* out = nullptr) noexcept;

}  // namespace dxmt9::d3d9
