#include "d3d9_pe_chunk_v2_builder.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace dxmt9::d3d9::pe {

namespace {

bool alignUp(std::size_t value, std::uint32_t alignment, std::size_t& out) {
  if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) {
    return false;
  }
  const auto mask = static_cast<std::size_t>(alignment - 1u);
  if (value > std::numeric_limits<std::size_t>::max() - mask) {
    return false;
  }
  out = (value + mask) & ~mask;
  return true;
}

bool identityEqual(const D9CCommandChunkWireHandleEntryV2& entry,
                   const D9CWireObjectIdentity& identity) {
  return entry.kind == identity.kind &&
         entry.generation == identity.generation &&
         entry.objectId == identity.objectId;
}

}  // namespace

CommandChunkV2Builder::CommandChunkV2Builder(
    const CommandChunkV2BuilderCapacities& capacities) {
  records_.reserve(capacities.records);
  handles_.reserve(capacities.handles);
  handleObjects_.reserve(capacities.handles);
  payload_.reserve(capacities.payloadBytes);
  sealedBlob_.reserve(capacities.sealedBytes);
}

bool CommandChunkV2Builder::beginRecord(std::uint32_t type) noexcept {
  const auto* rule = v2RecordRule(type);
  if (active_.active || sealed_ || !rule) {
    return false;
  }
  std::size_t payloadStart = 0u;
  if (!alignUp(payload_.size(), rule->payloadAlignment, payloadStart) ||
      payloadStart > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  const auto payloadCheckpoint = payload_.size();
  try {
    payload_.resize(payloadStart, std::byte{0});
  } catch (...) {
    return false;
  }
  active_ = ActiveRecord{
      .active = true,
      .type = type,
      .recordCheckpoint = records_.size(),
      .handleCheckpoint = handles_.size(),
      .payloadCheckpoint = payloadCheckpoint,
      .payloadStart = payloadStart,
      .retainedCheckpoint = retainer_.beginAcquire(),
  };
  return true;
}

bool CommandChunkV2Builder::appendPayload(
    std::span<const std::byte> bytes, std::uint32_t alignment,
    std::uint32_t* recordRelativeOffset) noexcept {
  if (!active_.active || sealed_) {
    return false;
  }
  std::size_t start = 0u;
  if (!alignUp(payload_.size(), alignment, start) ||
      start < active_.payloadStart ||
      start - active_.payloadStart >
          std::numeric_limits<std::uint32_t>::max() ||
      bytes.size() > std::numeric_limits<std::uint32_t>::max() -
                         (start - active_.payloadStart)) {
    return failActiveRecord();
  }
  try {
    payload_.resize(start, std::byte{0});
    const auto oldSize = payload_.size();
    payload_.resize(oldSize + bytes.size());
    if (!bytes.empty()) {
      std::memcpy(payload_.data() + oldSize, bytes.data(), bytes.size());
    }
  } catch (...) {
    return failActiveRecord();
  }
  if (recordRelativeOffset) {
    *recordRelativeOffset =
        static_cast<std::uint32_t>(start - active_.payloadStart);
  }
  return true;
}

bool CommandChunkV2Builder::overwritePayload(
    std::uint32_t recordRelativeOffset,
    std::span<const std::byte> bytes) noexcept {
  if (!active_.active || sealed_) {
    return false;
  }
  const auto offset = active_.payloadStart + recordRelativeOffset;
  if (offset < active_.payloadStart || offset > payload_.size() ||
      bytes.size() > payload_.size() - offset) {
    return failActiveRecord();
  }
  if (!bytes.empty()) {
    std::memcpy(payload_.data() + offset, bytes.data(), bytes.size());
  }
  return true;
}

bool CommandChunkV2Builder::appendHandle(const PeWireObjectRef& object,
                                         std::uint32_t expectedKind,
                                         std::uint32_t& absoluteIndex) noexcept {
  absoluteIndex = D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX;
  if (!active_.active || sealed_ || !object.valid(expectedKind)) {
    return failActiveRecord();
  }
  for (std::size_t i = active_.handleCheckpoint; i < handles_.size(); ++i) {
    if (!identityEqual(handles_[i], object.identity)) {
      continue;
    }
    if (handleObjects_[i] != object.object) {
      return failActiveRecord();
    }
    absoluteIndex = static_cast<std::uint32_t>(i);
    return true;
  }
  if (handles_.size() >= std::numeric_limits<std::uint32_t>::max()) {
    return failActiveRecord();
  }

  try {
    handles_.push_back(D9CCommandChunkWireHandleEntryV2{
        .kind = object.identity.kind,
        .generation = object.identity.generation,
        .objectId = object.identity.objectId,
    });
    try {
      handleObjects_.push_back(object.object);
    } catch (...) {
      handles_.pop_back();
      throw;
    }
    retainer_.retainWireObject(object.identity.kind, object.object,
                               active_.retainedCheckpoint);
  } catch (...) {
    return failActiveRecord();
  }
  absoluteIndex = static_cast<std::uint32_t>(handles_.size() - 1u);
  return true;
}

bool CommandChunkV2Builder::commitRecord() noexcept {
  if (!active_.active || sealed_) {
    return false;
  }
  const auto* rule = v2RecordRule(active_.type);
  const auto payloadBytes = payload_.size() - active_.payloadStart;
  const auto handleCount = handles_.size() - active_.handleCheckpoint;
  if (!rule || payloadBytes < rule->fixedPayloadSize ||
      payloadBytes > std::numeric_limits<std::uint32_t>::max() ||
      active_.payloadStart > std::numeric_limits<std::uint32_t>::max() ||
      active_.handleCheckpoint > std::numeric_limits<std::uint32_t>::max() ||
      handleCount > std::numeric_limits<std::uint32_t>::max()) {
    return failActiveRecord();
  }
  const D9CCommandChunkWireRecordHeaderV2 record{
      .type = active_.type,
      .flags = D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
      .payloadOffset = static_cast<std::uint32_t>(active_.payloadStart),
      .payloadSize = static_cast<std::uint32_t>(payloadBytes),
      .firstHandle = static_cast<std::uint32_t>(active_.handleCheckpoint),
      .handleCount = static_cast<std::uint32_t>(handleCount),
      .reserved0 = 0u,
      .reserved1 = 0u,
  };
  try {
    records_.push_back(record);
  } catch (...) {
    return failActiveRecord();
  }
  active_ = {};
  return true;
}

void CommandChunkV2Builder::rollbackRecord() noexcept {
  if (!active_.active) {
    return;
  }
  retainer_.rollback(active_.retainedCheckpoint);
  records_.resize(active_.recordCheckpoint);
  handles_.resize(active_.handleCheckpoint);
  handleObjects_.resize(active_.handleCheckpoint);
  payload_.resize(active_.payloadCheckpoint);
  active_ = {};
}

bool CommandChunkV2Builder::failActiveRecord() noexcept {
  rollbackRecord();
  return false;
}

SealedCommandChunkV2 CommandChunkV2Builder::seal() noexcept {
  if (active_.active) {
    return {};
  }
  if (sealed_) {
    return SealedCommandChunkV2{
        .blob = sealedBlob_,
        .recordCount = static_cast<std::uint32_t>(records_.size()),
        .handleCount = static_cast<std::uint32_t>(handles_.size()),
    };
  }
  if (records_.size() > std::numeric_limits<std::uint32_t>::max() ||
      handles_.size() > std::numeric_limits<std::uint32_t>::max() ||
      payload_.size() > std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }

  const auto recordTableOffset =
      static_cast<std::size_t>(D9C_COMMAND_CHUNK_WIRE_HEADER_V2_SIZE);
  if (records_.size() >
      (std::numeric_limits<std::size_t>::max() - recordTableOffset) /
          sizeof(D9CCommandChunkWireRecordHeaderV2)) {
    return {};
  }
  const auto recordEnd =
      recordTableOffset +
      records_.size() * sizeof(D9CCommandChunkWireRecordHeaderV2);
  std::size_t handleTableOffset = 0u;
  if (!alignUp(recordEnd, alignof(D9CCommandChunkWireHandleEntryV2),
               handleTableOffset) ||
      handles_.size() >
          (std::numeric_limits<std::size_t>::max() - handleTableOffset) /
              sizeof(D9CCommandChunkWireHandleEntryV2)) {
    return {};
  }
  const auto handleEnd =
      handleTableOffset +
      handles_.size() * sizeof(D9CCommandChunkWireHandleEntryV2);
  std::size_t payloadArenaOffset = 0u;
  if (!alignUp(handleEnd, alignof(std::uint32_t), payloadArenaOffset) ||
      payload_.size() >
          std::numeric_limits<std::size_t>::max() - payloadArenaOffset) {
    return {};
  }
  const auto totalBytes = payloadArenaOffset + payload_.size();
  if (recordTableOffset > std::numeric_limits<std::uint32_t>::max() ||
      handleTableOffset > std::numeric_limits<std::uint32_t>::max() ||
      payloadArenaOffset > std::numeric_limits<std::uint32_t>::max() ||
      totalBytes > std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }

  try {
    sealedBlob_.assign(totalBytes, std::byte{0});
  } catch (...) {
    return {};
  }
  const D9CCommandChunkWireHeaderV2 header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION_V2,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_V2_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_V2_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_V2_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordTableOffset),
      .recordCount = static_cast<std::uint32_t>(records_.size()),
      .handleTableOffset = static_cast<std::uint32_t>(handleTableOffset),
      .handleCount = static_cast<std::uint32_t>(handles_.size()),
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadArenaOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload_.size()),
      .reserved0 = 0u,
      .reserved1 = 0u,
  };
  std::memcpy(sealedBlob_.data(), &header, sizeof(header));
  if (!records_.empty()) {
    std::memcpy(sealedBlob_.data() + recordTableOffset, records_.data(),
                records_.size() * sizeof(records_[0]));
  }
  if (!handles_.empty()) {
    std::memcpy(sealedBlob_.data() + handleTableOffset, handles_.data(),
                handles_.size() * sizeof(handles_[0]));
  }
  if (!payload_.empty()) {
    std::memcpy(sealedBlob_.data() + payloadArenaOffset, payload_.data(),
                payload_.size());
  }
  sealed_ = true;
  return SealedCommandChunkV2{
      .blob = sealedBlob_,
      .recordCount = header.recordCount,
      .handleCount = header.handleCount,
  };
}

void CommandChunkV2Builder::reset() noexcept {
  rollbackRecord();
  retainer_.clear();
  records_.clear();
  handles_.clear();
  handleObjects_.clear();
  payload_.clear();
  sealedBlob_.clear();
  sealed_ = false;
}

}  // namespace dxmt9::d3d9::pe
