#pragma once

#include "dxmt9_encode_session.hpp"

#include <span>

namespace dxmt9::encoders {

// A resolved entry borrows queue-owned source storage only for the synchronous
// encode call. It must never be retained by EncodeSession or a Metal callback.
struct ResolvedEncodePartitionEntry {
  const core::ChunkSlot* slot = nullptr;
  core::MetalCommandView command{};
  const core::DrawRunCommandRecord* drawRunRecord = nullptr;
  core::FlatDrawStateView drawState{};
  const core::DrawParam* drawParam = nullptr;
  const core::DrawUniformPayloadRecord* uniform = nullptr;
  std::span<const core::u8> bindingOverrideBytes{};
  std::span<const core::u8> bindingSnapshotBytes{};
};

enum class EncodePartitionEntryValidation {
  Valid,
  SourceOrdinalOutOfRange,
  SourceIdentityMismatch,
  SourceCommandRangeInvalid,
  CommandIndexOutOfRange,
  CommandIsNotDrawRun,
  DrawRunRecordMismatch,
  StateIndexMismatch,
  DrawParamIndexMismatch,
  UniformHandleMismatch,
  BindingOverrideRangeMismatch,
  BindingSnapshotRangeMismatch,
};

struct EncodePartitionEntryResolution {
  EncodePartitionEntryValidation validation =
      EncodePartitionEntryValidation::SourceOrdinalOutOfRange;
  ResolvedEncodePartitionEntry entry{};

  explicit operator bool() const noexcept {
    return validation == EncodePartitionEntryValidation::Valid;
  }
};

// Resolves locator-only partition metadata against the call-local retained
// source table. Every identity, command, SoA index, uniform handle, and payload
// range must agree with immutable ChunkSlot storage. Rejection is side-effect
// free so the caller can fail open to source-order serial encoding.
EncodePartitionEntryResolution resolveEncodePartitionEntry(
    const EncodePartitionEntrySnapshot& snapshot,
    std::span<const core::metalqueue::ReadySlotSnapshot> sources) noexcept;

}  // namespace dxmt9::encoders
