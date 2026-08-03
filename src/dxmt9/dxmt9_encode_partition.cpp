#include "dxmt9_encode_partition.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace dxmt9::encoders {
namespace {

using Validation = EncodePartitionEntryValidation;

EncodePartitionEntryResolution reject(Validation validation) noexcept {
  return EncodePartitionEntryResolution{.validation = validation};
}

bool rangeEquals(core::DrawPayloadRange lhs,
                 core::DrawPayloadRange rhs) noexcept {
  return lhs.offset == rhs.offset && lhs.size == rhs.size;
}

bool absolutePayloadRange(const core::DrawRunCommandRecord& record,
                          core::DrawPayloadRange relative,
                          std::size_t arenaSize,
                          std::size_t schemaSize,
                          core::DrawPayloadRange& absolute) noexcept {
  if (relative.empty()) {
    absolute = {};
    return true;
  }
  if (relative.size != schemaSize ||
      relative.offset > record.payloadSize ||
      relative.size > record.payloadSize - relative.offset ||
      record.payloadOffset > arenaSize ||
      record.payloadSize > arenaSize - record.payloadOffset) {
    return false;
  }

  const std::uint64_t offset =
      static_cast<std::uint64_t>(record.payloadOffset) + relative.offset;
  if (offset > std::numeric_limits<std::uint32_t>::max() ||
      offset > arenaSize || relative.size > arenaSize - offset) {
    return false;
  }
  absolute = core::DrawPayloadRange{
      .offset = static_cast<std::uint32_t>(offset),
      .size = relative.size,
  };
  return true;
}

std::span<const core::u8> payloadBytes(
    const core::ChunkSlot& slot,
    core::DrawPayloadRange range) noexcept {
  if (range.empty()) {
    return {};
  }
  return std::span<const core::u8>(slot.drawPayloadArena.data() + range.offset,
                                   range.size);
}

}  // namespace

EncodePartitionEntryResolution resolveEncodePartitionEntry(
    const EncodePartitionEntrySnapshot& snapshot,
    std::span<const core::metalqueue::ReadySlotSnapshot> sources) noexcept {
  if (snapshot.source.sourceOrdinal >= sources.size()) {
    return reject(Validation::SourceOrdinalOutOfRange);
  }

  const auto& source = sources[snapshot.source.sourceOrdinal];
  const core::ChunkSlot* slot = source.slot;
  if (!slot || source.slotIndex != snapshot.source.slotIndex ||
      source.seqId == 0 || source.seqId != snapshot.source.seqId ||
      slot->seqId != snapshot.source.seqId) {
    return reject(Validation::SourceIdentityMismatch);
  }

  const std::size_t slotCommandCount = slot->commandCount();
  if (source.commandBegin > slotCommandCount ||
      source.commandCount > slotCommandCount - source.commandBegin) {
    return reject(Validation::SourceCommandRangeInvalid);
  }
  const std::size_t sourceCommandEnd =
      source.commandBegin + source.commandCount;
  if (snapshot.commandIndex < source.commandBegin ||
      snapshot.commandIndex >= sourceCommandEnd) {
    return reject(Validation::CommandIndexOutOfRange);
  }

  const auto command = slot->commandAt(snapshot.commandIndex);
  if (command.kind != core::MetalCommandKind::DrawRun ||
      !command.drawRunRecord) {
    return reject(Validation::CommandIsNotDrawRun);
  }
  const auto& header = slot->commandHeaders[snapshot.commandIndex];
  if (header.payloadIndex.value != snapshot.drawRunRecordIndex ||
      snapshot.drawRunRecordIndex >= slot->drawRunRecords.size() ||
      command.drawRunRecord !=
          &slot->drawRunRecords[snapshot.drawRunRecordIndex]) {
    return reject(Validation::DrawRunRecordMismatch);
  }

  const auto& record = *command.drawRunRecord;
  if (record.stateIndex != snapshot.stateIndex ||
      !command.drawState.hot || !command.drawState.shaderLayout ||
      !command.drawState.debug) {
    return reject(Validation::StateIndexMismatch);
  }
  if (snapshot.drawParamIndex < record.firstParam ||
      snapshot.drawParamIndex - record.firstParam >= record.paramCount ||
      snapshot.drawParamIndex >= slot->drawParams.size()) {
    return reject(Validation::DrawParamIndexMismatch);
  }

  const auto& param = slot->drawParams[snapshot.drawParamIndex];
  const core::DrawUniformHandle effectiveUniform =
      param.uniformHandle.valid() ? param.uniformHandle : record.uniformHandle;
  const auto* uniform = slot->drawUniformPayloadRecord(effectiveUniform);
  if (!uniform || !(effectiveUniform == snapshot.uniformHandle)) {
    return reject(Validation::UniformHandleMismatch);
  }

  core::DrawPayloadRange bindingOverrideBytes{};
  if (!absolutePayloadRange(record, param.bindingOverrideRange,
                            slot->drawPayloadArena.size(),
                            sizeof(core::DrawBindingOverride),
                            bindingOverrideBytes) ||
      !rangeEquals(bindingOverrideBytes, snapshot.bindingOverrideBytes)) {
    return reject(Validation::BindingOverrideRangeMismatch);
  }
  core::DrawPayloadRange bindingSnapshotBytes{};
  if (!absolutePayloadRange(record, param.bindingSnapshotRange,
                            slot->drawPayloadArena.size(),
                            sizeof(core::DrawBindingSnapshot),
                            bindingSnapshotBytes) ||
      !rangeEquals(bindingSnapshotBytes, snapshot.bindingSnapshotBytes)) {
    return reject(Validation::BindingSnapshotRangeMismatch);
  }

  return EncodePartitionEntryResolution{
      .validation = Validation::Valid,
      .entry = ResolvedEncodePartitionEntry{
          .slot = slot,
          .command = command,
          .drawRunRecord = &record,
          .drawState = command.drawState,
          .drawParam = &param,
          .uniform = uniform,
          .bindingOverrideBytes = payloadBytes(*slot, bindingOverrideBytes),
          .bindingSnapshotBytes = payloadBytes(*slot, bindingSnapshotBytes),
      },
  };
}

}  // namespace dxmt9::encoders
