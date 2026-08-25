#include "d3d9_pe_chunk_builder.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>

namespace dxmt9::d3d9::pe {

namespace {

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

std::uint32_t constantRecordElementSize(std::uint32_t type) noexcept {
  switch (type) {
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
      return 16u;
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
    case D9C_COMMAND_RECORD_SET_PS_CONST_B:
      return 4u;
    default:
      return 0u;
  }
}

std::uint32_t constantRecordLimit(std::uint32_t type) noexcept {
  switch (type) {
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
      return D9C_DRAW_PACKET_MAX_CONST_VS_F;
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
      return D9C_DRAW_PACKET_MAX_CONST_VS_I;
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
      return D9C_DRAW_PACKET_MAX_CONST_VS_B;
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
      return D9C_DRAW_PACKET_MAX_CONST_PS_F;
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
      return D9C_DRAW_PACKET_MAX_CONST_PS_I;
    case D9C_COMMAND_RECORD_SET_PS_CONST_B:
      return D9C_DRAW_PACKET_MAX_CONST_PS_B;
    default:
      return 0u;
  }
}

}  // namespace

void noteWireIdentityGetterCall() noexcept {
  g_wireIdentityGetterCalls.fetch_add(1u, std::memory_order_relaxed);
}

std::uint64_t wireIdentityGetterCallCount() noexcept {
  return g_wireIdentityGetterCalls.load(std::memory_order_relaxed);
}

CommandChunkBuilder::CommandChunkBuilder(
    const CommandChunkBuilderCapacities& capacities)
    : retainer_(capacities.handles) {
  records_.reserve(capacities.records);
  handles_.reserve(capacities.handles);
  handleObjects_.reserve(capacities.handles);
  payload_.reserve(capacities.payloadBytes);
  sealedBlob_.reserve(capacities.sealedBytes);
  handlePresence_.init(capacities.handles);
  recordLocalDedup_.init(capacities.handles);
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
      // Assigned unconditionally, before this record's outcome (commit vs.
      // rollback) is known: RecordLocalDedupTable's self-invalidation
      // depends on every beginRecord() handing out a fresh, never-repeated
      // ordinal regardless of what happens to the record.
      .recordOrdinal = nextRecordOrdinal_++,
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

bool CommandChunkBuilder::reservePayload(
    std::size_t byteCount, std::uint32_t alignment,
    std::uint32_t* recordRelativeOffset) noexcept {
  if (!active_.active || sealed_) {
    return false;
  }
  std::size_t start = 0u;
  if (!alignUp(payload_.size(), alignment, start) ||
      start < active_.payloadStart ||
      start - active_.payloadStart >
          std::numeric_limits<std::uint32_t>::max() ||
      byteCount > std::numeric_limits<std::uint32_t>::max() -
                      (start - active_.payloadStart) ||
      byteCount > std::numeric_limits<std::size_t>::max() - start) {
    return failActiveRecord();
  }
  try {
    payload_.resize(start, std::byte{0});
    payload_.resize(start + byteCount, std::byte{0});
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

bool CommandChunkBuilder::appendConstantSectionPayload(
    std::uint16_t kind, std::uint32_t startRegister,
    std::uint32_t registerCount, std::span<const std::byte> registerBytes,
    std::uint32_t* recordRelativeOffset) noexcept {
  const auto* rule = sectionRule(kind);
  const auto* activeRule = active_.active ? recordRule(active_.type) : nullptr;
  const auto end = static_cast<std::uint64_t>(startRegister) + registerCount;
  const auto byteSize = static_cast<std::uint64_t>(registerCount) *
                        (rule ? rule->elementSize : 0u);
  if (!rule || !activeRule ||
      (activeRule->ruleFlags & RecordRuleSparseState) == 0u ||
      (rule->ruleFlags & SectionRuleConstantRange) == 0u ||
      registerCount == 0u || registerCount > rule->maxCount ||
      end > rule->maxCount || byteSize != registerBytes.size()) {
    return failActiveRecord();
  }
  const D9CCommandChunkWireConstantRange range{
      .startRegister = startRegister,
      .registerCount = registerCount,
  };
  return appendPayloadValue(range, recordRelativeOffset) &&
         appendPayload(registerBytes, rule->payloadAlignment);
}

bool CommandChunkBuilder::appendUpDataSectionPayload(
    std::uint16_t kind, std::span<const std::byte> bytes,
    std::uint32_t* recordRelativeOffset) noexcept {
  const auto* rule = sectionRule(kind);
  const bool kindMatchesRecord =
      kind == D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA
          ? active_.active &&
                active_.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP
          : kind == D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA &&
                active_.active &&
                (active_.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP ||
                 active_.type ==
                     D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP);
  if (!rule || !kindMatchesRecord ||
      (rule->ruleFlags & SectionRuleRawBytes) == 0u ||
      bytes.empty() || bytes.size() > rule->maxCount ||
      bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
    return failActiveRecord();
  }
  return appendPayload(bytes, rule->payloadAlignment, recordRelativeOffset);
}

bool CommandChunkBuilder::appendSectionTable(
    std::span<const D9CCommandChunkWireSectionDesc> sections) noexcept {
  const auto* activeRule = active_.active ? recordRule(active_.type) : nullptr;
  D9CCommandChunkWireDrawHeader draw{};
  const auto tableBytes = static_cast<std::uint64_t>(sections.size()) *
                          sizeof(D9CCommandChunkWireSectionDesc);
  const auto currentOffset = active_.active
      ? payload_.size() - active_.payloadStart
      : 0u;
  if (!activeRule ||
      (activeRule->ruleFlags & RecordRuleSparseState) == 0u ||
      sections.size() > D9C_COMMAND_CHUNK_SECTION_COUNT ||
      !readActivePayloadValue(0u, draw) ||
      draw.sectionCount != sections.size() ||
      draw.sectionTableOffset != currentOffset ||
      draw.sectionTableOffset != sizeof(draw) ||
      tableBytes > std::numeric_limits<std::uint32_t>::max() ||
      static_cast<std::uint64_t>(draw.sectionTableOffset) + tableBytes !=
          draw.sectionPayloadOffset) {
    return failActiveRecord();
  }
  return appendPayload(std::as_bytes(sections),
                       alignof(D9CCommandChunkWireSectionDesc));
}

bool CommandChunkBuilder::overwriteSectionTable(
    std::uint32_t recordRelativeOffset,
    std::span<const D9CCommandChunkWireSectionDesc> sections) noexcept {
  const auto* activeRule = active_.active ? recordRule(active_.type) : nullptr;
  D9CCommandChunkWireDrawHeader draw{};
  if (!activeRule ||
      (activeRule->ruleFlags & RecordRuleSparseState) == 0u ||
      sections.size() > D9C_COMMAND_CHUNK_SECTION_COUNT ||
      !readActivePayloadValue(0u, draw) ||
      recordRelativeOffset != draw.sectionTableOffset ||
      draw.sectionTableOffset != sizeof(draw) ||
      draw.sectionCount != sections.size() ||
      recordRelativeOffset % alignof(D9CCommandChunkWireSectionDesc) != 0u ||
      static_cast<std::uint64_t>(recordRelativeOffset) +
              static_cast<std::uint64_t>(sections.size_bytes()) !=
          draw.sectionPayloadOffset ||
      draw.sectionPayloadOffset > payload_.size() - active_.payloadStart) {
    return failActiveRecord();
  }

  std::uint64_t expectedOffset = draw.sectionPayloadOffset;
  std::uint16_t previousKind = 0u;
  for (const auto& desc : sections) {
    const auto* rule = sectionRule(desc.kind);
    const auto expectedBytes = static_cast<std::uint64_t>(desc.count) *
        (rule ? rule->elementSize : 0u);
    const auto totalBytes = expectedBytes +
        (rule && (rule->ruleFlags & SectionRuleConstantRange) != 0u
             ? sizeof(D9CCommandChunkWireConstantRange)
             : 0u);
    std::size_t alignedOffset = 0u;
    if (!rule || desc.kind <= previousKind ||
        desc.elementSize != rule->elementSize || desc.count == 0u ||
        desc.count > rule->maxCount ||
        ((rule->ruleFlags & SectionRuleSingle) != 0u && desc.count != 1u) ||
        desc.byteSize != totalBytes ||
        !alignUp(static_cast<std::size_t>(expectedOffset),
                 rule->payloadAlignment, alignedOffset) ||
        desc.payloadOffset != alignedOffset ||
        desc.payloadOffset % rule->payloadAlignment != 0u ||
        desc.payloadOffset > payload_.size() - active_.payloadStart ||
        desc.byteSize > payload_.size() - active_.payloadStart -
                            desc.payloadOffset) {
      return failActiveRecord();
    }
    if ((rule->ruleFlags & SectionRuleConstantRange) != 0u) {
      D9CCommandChunkWireConstantRange range{};
      if (!readActivePayloadValue(desc.payloadOffset, range) ||
          range.registerCount != desc.count ||
          static_cast<std::uint64_t>(range.startRegister) +
                  range.registerCount >
              rule->maxCount) {
        return failActiveRecord();
      }
    }
    previousKind = desc.kind;
    expectedOffset = static_cast<std::uint64_t>(desc.payloadOffset) +
                     desc.byteSize;
  }
  if (expectedOffset != payload_.size() - active_.payloadStart) {
    return failActiveRecord();
  }
  return overwritePayload(recordRelativeOffset, std::as_bytes(sections));
}

bool CommandChunkBuilder::appendConstantRecordTail(
    std::uint32_t registerCount,
    std::span<const std::byte> registerBytes) noexcept {
  const auto elementSize = active_.active
      ? constantRecordElementSize(active_.type)
      : 0u;
  const auto limit = active_.active ? constantRecordLimit(active_.type) : 0u;
  D9CCommandChunkWireSetConst fixed{};
  const auto expected = static_cast<std::uint64_t>(registerCount) *
                        elementSize;
  if (elementSize == 0u || registerCount > limit ||
      expected != registerBytes.size() ||
      !readActivePayloadValue(0u, fixed) ||
      fixed.registerCount != registerCount ||
      static_cast<std::uint64_t>(fixed.startRegister) +
              fixed.registerCount >
          limit ||
      payload_.size() - active_.payloadStart !=
          sizeof(D9CCommandChunkWireSetConst)) {
    return failActiveRecord();
  }
  return appendPayload(registerBytes, alignof(std::uint32_t));
}

bool CommandChunkBuilder::appendClearRectTail(
    std::span<const D9CRect> rects) noexcept {
  D9CCommandChunkWireClear fixed{};
  if (!active_.active || active_.type != D9C_COMMAND_RECORD_CLEAR ||
      rects.size() > std::numeric_limits<std::uint32_t>::max() ||
      !readActivePayloadValue(0u, fixed) ||
      fixed.rectCount != rects.size() ||
      fixed.rectOffset != sizeof(D9CCommandChunkWireClear) ||
      payload_.size() - active_.payloadStart !=
          sizeof(D9CCommandChunkWireClear)) {
    return failActiveRecord();
  }
  return appendPayload(std::as_bytes(rects), alignof(D9CRect));
}

bool CommandChunkBuilder::appendNewHandleEntry(
    const PeWireObjectRef& object, std::uint32_t& absoluteIndex) noexcept {
  if (handles_.size() >= std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  const auto local = PeLocalObjectIdentity{
      .kind = object.identity.kind, .object = object.object};
  try {
    handles_.push_back(D9CCommandChunkWireHandleEntry{
        .kind = object.identity.kind,
        .generation = object.identity.generation,
        .objectId = object.identity.objectId,
    });
    try {
      handleObjects_.push_back(local);
    } catch (...) {
      handles_.pop_back();
      throw;
    }
    // Count this qualified local identity's chunk-lifetime multiplicity so
    // referencesObject()
    // can answer in O(1); handlePresence_'s findOrInsert() never throws, and
    // rollbackRecord() undoes exactly this increment if a later step in this
    // same append fails.
    if (auto* slot = handlePresence_.findOrInsert(local)) {
      ++slot->count;
    }
    retainer_.retainWireObject(object.identity.kind, object.object,
                               active_.retainedCheckpoint);
  } catch (...) {
    return false;
  }
  absoluteIndex = static_cast<std::uint32_t>(handles_.size() - 1u);
  return true;
}

bool CommandChunkBuilder::appendHandle(const PeWireObjectRef& object,
                                         std::uint32_t expectedKind,
                                         std::uint32_t& absoluteIndex) noexcept {
  absoluteIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
  if (!active_.active || sealed_ || !object.valid(expectedKind)) {
    return failActiveRecord();
  }

  if (!recordLocalDedup_.overflowed) {
    RecordLocalDedupTable::Slot* insertAt = nullptr;
    std::uint32_t hitIndex = 0u;
    void* hitObject = nullptr;
    const auto lookup = recordLocalDedup_.findForRecord(
        object.identity, active_.recordOrdinal, &insertAt, &hitIndex,
        &hitObject);
    if (lookup == RecordLocalDedupTable::Lookup::kHit) {
      if (hitObject != object.object) {
        return failActiveRecord();
      }
      absoluteIndex = hitIndex;
      return true;
    }
    if (lookup == RecordLocalDedupTable::Lookup::kMiss) {
      if (!appendNewHandleEntry(object, absoluteIndex)) {
        return failActiveRecord();
      }
      recordLocalDedup_.insert(*insertAt, object.identity,
                               active_.recordOrdinal, absoluteIndex,
                               object.object);
      return true;
    }
    // kOverflowed: fall through to the linear scan below, which stays
    // correct (just O(record window)) for the remainder of this and every
    // future record in the chunk's lifetime.
  }

  for (std::size_t i = active_.handleCheckpoint; i < handles_.size(); ++i) {
    if (!identityEqual(handles_[i], object.identity)) {
      continue;
    }
    if (handleObjects_[i].object != object.object) {
      return failActiveRecord();
    }
    absoluteIndex = static_cast<std::uint32_t>(i);
    return true;
  }
  if (!appendNewHandleEntry(object, absoluteIndex)) {
    return failActiveRecord();
  }
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
  lastCommittedRecordOrdinal_ = active_.recordOrdinal;
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
  // so it is cleared in lockstep every time handleObjects_ is. Same for
  // recordLocalDedup_: its self-invalidation is ordinal-based, but clearing
  // it here keeps `occupied` bounded to one chunk's distinct-identity count
  // instead of accumulating across every chunk the builder ever seals
  // (nextRecordOrdinal_ itself intentionally keeps counting across resets).
  handlePresence_.clear();
  recordLocalDedup_.clear();
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
  recordLocalDedup_.clear();
  payload_.clear();
  sealedBlob_.clear();
  sealed_ = false;
}

bool CommandChunkBuilder::referencesObject(
    PeLocalObjectIdentity identity) const noexcept {
  // Texture is the wire kind 0, so object presence—not a nonzero kind—is
  // the validity check for a local query. An absent object remains invalid.
  if (!identity.object) {
    return false;
  }
  if (!handlePresence_.overflowed) {
    const auto* slot = handlePresence_.find(identity);
    return slot != nullptr && slot->count > 0u;
  }
  // Overflow fallback: handleObjects_ is always kept complete, so the
  // original linear scan is still correct, just the pre-overflow O(n).
  return std::find(handleObjects_.begin(), handleObjects_.end(), identity) !=
         handleObjects_.end();
}

}  // namespace dxmt9::d3d9::pe
