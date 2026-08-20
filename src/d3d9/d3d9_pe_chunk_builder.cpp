#include "d3d9_pe_chunk_builder.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace dxmt9::d3d9::pe {

namespace {

std::mutex g_wireObjectCacheMutex;
std::unordered_map<void*, PeWireObjectRef> g_wireObjectCache;
std::atomic<std::uint64_t> g_wireIdentityGetterCalls{0u};

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

bool identityEqual(const D9CCommandChunkWireHandleEntry& entry,
                   const D9CWireObjectIdentity& identity) {
  return entry.kind == identity.kind &&
         entry.generation == identity.generation &&
         entry.objectId == identity.objectId;
}

}  // namespace

void noteWireIdentityGetterCall() noexcept {
  g_wireIdentityGetterCalls.fetch_add(1u, std::memory_order_relaxed);
}

std::uint64_t wireIdentityGetterCallCount() noexcept {
  return g_wireIdentityGetterCalls.load(std::memory_order_relaxed);
}

void publishCachedWireObjectRef(const PeWireObjectRef& object) noexcept {
  if (!object.object) {
    return;
  }
  try {
    std::lock_guard lock(g_wireObjectCacheMutex);
    g_wireObjectCache[object.object] = object;
  } catch (...) {
  }
}

void unpublishCachedWireObjectRef(const PeWireObjectRef& object) noexcept {
  if (!object.object) {
    return;
  }
  std::lock_guard lock(g_wireObjectCacheMutex);
  const auto it = g_wireObjectCache.find(object.object);
  if (it != g_wireObjectCache.end() &&
      it->second.identity.kind == object.identity.kind &&
      it->second.identity.generation == object.identity.generation &&
      it->second.identity.objectId == object.identity.objectId) {
    g_wireObjectCache.erase(it);
  }
}

CommandChunkBuilder::CommandChunkBuilder(
    const CommandChunkBuilderCapacities& capacities) {
  records_.reserve(capacities.records);
  handles_.reserve(capacities.handles);
  handleObjects_.reserve(capacities.handles);
  payload_.reserve(capacities.payloadBytes);
  sealedBlob_.reserve(capacities.sealedBytes);
  handlePresence_.init(capacities.handles);
}

bool CommandChunkBuilder::beginRecord(std::uint32_t type) noexcept {
  const auto* rule = recordRule(type);
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

bool CommandChunkBuilder::appendPayload(
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

bool CommandChunkBuilder::overwritePayload(
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

bool CommandChunkBuilder::appendHandle(const PeWireObjectRef& object,
                                         std::uint32_t expectedKind,
                                         std::uint32_t& absoluteIndex) noexcept {
  absoluteIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
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
    handles_.push_back(D9CCommandChunkWireHandleEntry{
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
    // Count this pointer's chunk-lifetime multiplicity so referencesObject()
    // can answer in O(1); handlePresence_'s findOrInsert() never throws, and
    // rollbackRecord() below undoes exactly this increment if a later step
    // in this same append fails.
    if (auto* slot = handlePresence_.findOrInsert(object.object)) {
      ++slot->count;
    }
    retainer_.retainWireObject(object.identity.kind, object.object,
                               active_.retainedCheckpoint);
  } catch (...) {
    return failActiveRecord();
  }
  absoluteIndex = static_cast<std::uint32_t>(handles_.size() - 1u);
  return true;
}

bool CommandChunkBuilder::commitRecord() noexcept {
  if (!active_.active || sealed_) {
    return false;
  }
  const auto* rule = recordRule(active_.type);
  const auto payloadBytes = payload_.size() - active_.payloadStart;
  const auto handleCount = handles_.size() - active_.handleCheckpoint;
  if (!rule || payloadBytes < rule->fixedPayloadSize ||
      payloadBytes > std::numeric_limits<std::uint32_t>::max() ||
      active_.payloadStart > std::numeric_limits<std::uint32_t>::max() ||
      active_.handleCheckpoint > std::numeric_limits<std::uint32_t>::max() ||
      handleCount > std::numeric_limits<std::uint32_t>::max()) {
    return failActiveRecord();
  }
  const D9CCommandChunkWireRecordHeader record{
      .type = active_.type,
      .flags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
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

void CommandChunkBuilder::rollbackRecord() noexcept {
  if (!active_.active) {
    return;
  }
  retainer_.rollback(active_.retainedCheckpoint);
  // Undo this record's handlePresence_ increments before handleObjects_ is
  // truncated below — the range [handleCheckpoint, handleObjects_.size())
  // is exactly what this failed record added. Skipped once the table has
  // overflowed: referencesObject() has already committed to the linear
  // fallback for the rest of this chunk's lifetime, so the counts no longer
  // matter.
  if (!handlePresence_.overflowed) {
    for (std::size_t i = active_.handleCheckpoint; i < handleObjects_.size();
         ++i) {
      if (auto* slot = handlePresence_.find(handleObjects_[i])) {
        if (slot->count > 0u) {
          --slot->count;
        }
      }
    }
  }
  records_.resize(active_.recordCheckpoint);
  handles_.resize(active_.handleCheckpoint);
  handleObjects_.resize(active_.handleCheckpoint);
  payload_.resize(active_.payloadCheckpoint);
  active_ = {};
}

bool CommandChunkBuilder::failActiveRecord() noexcept {
  rollbackRecord();
  return false;
}

SealedCommandChunk CommandChunkBuilder::seal() noexcept {
  if (active_.active) {
    return {};
  }
  if (sealed_) {
    return SealedCommandChunk{
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
      static_cast<std::size_t>(D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE);
  if (records_.size() >
      (std::numeric_limits<std::size_t>::max() - recordTableOffset) /
          sizeof(D9CCommandChunkWireRecordHeader)) {
    return {};
  }
  const auto recordEnd =
      recordTableOffset +
      records_.size() * sizeof(D9CCommandChunkWireRecordHeader);
  std::size_t handleTableOffset = 0u;
  if (!alignUp(recordEnd, alignof(D9CCommandChunkWireHandleEntry),
               handleTableOffset) ||
      handles_.size() >
          (std::numeric_limits<std::size_t>::max() - handleTableOffset) /
              sizeof(D9CCommandChunkWireHandleEntry)) {
    return {};
  }
  const auto handleEnd =
      handleTableOffset +
      handles_.size() * sizeof(D9CCommandChunkWireHandleEntry);
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
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
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
  return SealedCommandChunk{
      .blob = sealedBlob_,
      .recordCount = header.recordCount,
      .handleCount = header.handleCount,
  };
}

void CommandChunkBuilder::reset() noexcept {
  rollbackRecord();
  // Chunk boundary, not a discard: the committed chunk's handles are now the
  // unix side's problem, but the objects this chunk named are overwhelmingly
  // the objects the next one will name, so their pins stay warm for a bounded
  // number of epochs instead of being dropped and re-taken over the bridge.
  // See D3D9PePendingCommandRetainer's header comment.
  retainer_.endEpoch();
  records_.clear();
  handles_.clear();
  handleObjects_.clear();
  // handlePresence_ tracks exactly handleObjects_'s chunk-lifetime contents,
  // so it is cleared in lockstep every time handleObjects_ is.
  handlePresence_.clear();
  payload_.clear();
  sealedBlob_.clear();
  sealed_ = false;
}

void CommandChunkBuilder::resetAndReleaseRetained() noexcept {
  rollbackRecord();
  // Discard, not a chunk boundary: drop every pin including the warm ones the
  // previous epochs are still holding. This is what device teardown, Reset and
  // ResetEx run, so no retainer pin can survive into a dxmt9c_device_reset*
  // call or outlive the device.
  retainer_.clear();
  records_.clear();
  handles_.clear();
  handleObjects_.clear();
  handlePresence_.clear();
  payload_.clear();
  sealedBlob_.clear();
  sealed_ = false;
}

bool CommandChunkBuilder::referencesObject(void* object) const noexcept {
  if (!object) {
    return false;
  }
  if (!handlePresence_.overflowed) {
    const auto* slot = handlePresence_.find(object);
    return slot != nullptr && slot->count > 0u;
  }
  // Overflow fallback: handleObjects_ is always kept complete, so the
  // original linear scan is still correct, just the pre-overflow O(n).
  return std::find(handleObjects_.begin(), handleObjects_.end(), object) !=
         handleObjects_.end();
}

}  // namespace dxmt9::d3d9::pe
