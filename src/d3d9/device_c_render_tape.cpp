#include "device_c_render_tape.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace dxmt9::d3d9 {

namespace {

constexpr std::uint32_t kNoIndex = 0xffffffffu;

bool checkedAdd(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return false;
  }
  out = a + b;
  return true;
}

bool checkedMul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a != 0u && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return false;
  }
  out = a * b;
  return true;
}

bool alignUp(std::uint64_t value, std::uint64_t alignment,
             std::uint64_t& out) noexcept {
  const auto mask = alignment - 1u;
  if ((alignment & mask) != 0u ||
      value > std::numeric_limits<std::uint64_t>::max() - mask) {
    return false;
  }
  out = (value + mask) & ~mask;
  return true;
}

bool rangeValid(std::size_t size, std::uint64_t offset,
                std::uint64_t count) noexcept {
  std::uint64_t end = 0u;
  return checkedAdd(offset, count, end) && end <= size;
}

bool pointerAligned(const std::byte* base, std::uint64_t offset,
                    std::size_t alignment) noexcept {
  return (reinterpret_cast<std::uintptr_t>(base) + offset) % alignment == 0u;
}

bool zeroBytes(std::span<const std::byte> bytes, std::uint64_t begin,
               std::uint64_t end) noexcept {
  if (begin > end || end > bytes.size()) {
    return false;
  }
  return std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                     bytes.begin() + static_cast<std::ptrdiff_t>(end),
                     [](std::byte value) { return value == std::byte{}; });
}

template <typename T>
bool load(std::span<const std::byte> bytes, std::size_t offset,
          T& value) noexcept {
  if (!rangeValid(bytes.size(), offset, sizeof(T))) {
    return false;
  }
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return true;
}

template <typename T>
std::vector<std::byte> payloadWithTail(const T& fixed,
                                       std::span<const std::byte> tail) {
  std::vector<std::byte> payload(sizeof(T) + tail.size());
  std::memcpy(payload.data(), &fixed, sizeof(T));
  if (!tail.empty()) {
    std::memcpy(payload.data() + sizeof(T), tail.data(), tail.size());
  }
  return payload;
}

bool validIdentity(const D9CWireObjectIdentity& identity) noexcept {
  return identity.kind <= D9C_CHUNK_HANDLE_KIND_QUERY &&
         identity.generation != 0u && identity.objectId != 0u;
}

bool sameIdentity(const D9CWireObjectIdentity& a,
                  const D9CWireObjectIdentity& b) noexcept {
  return a.kind == b.kind && a.generation == b.generation &&
         a.objectId == b.objectId;
}

auto findIdentity(std::vector<RenderTapeValidationScratch::LiveIdentity>& live,
                  const D9CWireObjectIdentity& identity) noexcept {
  return std::find_if(live.begin(), live.end(), [&](const auto& entry) {
    return sameIdentity(entry.identity, identity);
  });
}

auto findObjectSlot(
    std::vector<RenderTapeValidationScratch::LiveIdentity>& live,
    const D9CWireObjectIdentity& identity) noexcept {
  return std::find_if(live.begin(), live.end(), [&](const auto& entry) {
    return entry.identity.kind == identity.kind &&
           entry.identity.objectId == identity.objectId;
  });
}

RenderTapeValidationResult
failure(RenderTapeValidationStatus status, std::uint32_t eventIndex = kNoIndex,
        CommandChunkValidationStatus chunkStatus =
            CommandChunkValidationStatus::Valid) noexcept {
  return RenderTapeValidationResult{
      .status = status,
      .failedEventIndex = eventIndex,
      .chunkStatus = chunkStatus,
  };
}

bool knownEventType(std::uint32_t value) noexcept {
  return value >= static_cast<std::uint32_t>(RenderTapeEventType::Checkpoint) &&
         value <=
             static_cast<std::uint32_t>(RenderTapeEventType::PresentBoundary);
}

std::span<const std::byte> tailAfter(std::span<const std::byte> payload,
                                     std::size_t fixedSize) noexcept {
  return payload.size() < fixedSize ? std::span<const std::byte>{}
                                    : payload.subspan(fixedSize);
}

} // namespace

ImportedRenderTapeEventView
ImportedRenderTapeView::event(std::size_t index) const noexcept {
  if (index >= events.size()) {
    return {};
  }
  const auto& header = events[index];
  if (!rangeValid(payloadArena.size(), header.payloadOffset,
                  header.payloadSize)) {
    return {};
  }
  return ImportedRenderTapeEventView{
      .header = header,
      .payload = payloadArena.subspan(header.payloadOffset, header.payloadSize),
  };
}

RenderTapeValidationResult
validateRenderTape(std::span<const std::byte> blob, ImportedRenderTapeView* out,
                   RenderTapeValidationScratch& scratch) noexcept {
  if (out) {
    *out = {};
  }

  RenderTapeHeader header{};
  if (!load(blob, 0u, header)) {
    return failure(RenderTapeValidationStatus::MissingHeader);
  }
  if (header.magic != kRenderTapeMagic ||
      header.version != kRenderTapeVersion ||
      header.headerSize != sizeof(RenderTapeHeader) ||
      header.profile != kRenderTapeProfileFrame ||
      header.eventHeaderSize != sizeof(RenderTapeEventHeader) ||
      header.wireVersion != D9C_COMMAND_CHUNK_WIRE_VERSION) {
    return failure(RenderTapeValidationStatus::InvalidHeader);
  }
  if (header.reserved0 != 0u || header.reserved1 != 0u ||
      header.reserved2 != 0u || header.reserved3 != 0u) {
    return failure(RenderTapeValidationStatus::NonZeroReserved);
  }

  std::uint64_t eventBytes = 0u;
  std::uint64_t eventEnd = 0u;
  std::uint64_t expectedArenaOffset = 0u;
  std::uint64_t blobEnd = 0u;
  if (!checkedMul(header.eventCount, header.eventHeaderSize, eventBytes) ||
      !checkedAdd(header.eventTableOffset, eventBytes, eventEnd) ||
      !alignUp(eventEnd, alignof(RenderTapeEventHeader), expectedArenaOffset) ||
      !checkedAdd(header.payloadArenaOffset, header.payloadArenaSize,
                  blobEnd) ||
      header.eventTableOffset != header.headerSize ||
      header.payloadArenaOffset != expectedArenaOffset ||
      blobEnd != blob.size() ||
      !pointerAligned(blob.data(), 0u, alignof(RenderTapeHeader)) ||
      !pointerAligned(blob.data(), header.eventTableOffset,
                      alignof(RenderTapeEventHeader)) ||
      !pointerAligned(blob.data(), header.payloadArenaOffset,
                      kRenderTapePayloadAlignment) ||
      !rangeValid(blob.size(), header.eventTableOffset, eventBytes) ||
      !rangeValid(blob.size(), header.payloadArenaOffset,
                  header.payloadArenaSize) ||
      !zeroBytes(blob, eventEnd, header.payloadArenaOffset)) {
    return failure(RenderTapeValidationStatus::InvalidLayout);
  }

  const auto events = header.eventCount == 0u
                          ? std::span<const RenderTapeEventHeader>{}
                          : std::span<const RenderTapeEventHeader>{
                                reinterpret_cast<const RenderTapeEventHeader*>(
                                    blob.data() + header.eventTableOffset),
                                header.eventCount};
  const auto arena =
      blob.subspan(header.payloadArenaOffset, header.payloadArenaSize);
  ImportedRenderTapeView candidate{
      .header = header,
      .events = events,
      .payloadArena = arena,
  };

  scratch.liveObjects.clear();
  scratch.retainedObjects.clear();
  try {
    scratch.liveObjects.reserve(header.eventCount);
    scratch.retainedObjects.reserve(header.eventCount);
  } catch (...) {
    return failure(RenderTapeValidationStatus::ScratchAllocationFailed);
  }

  std::uint64_t expectedPayloadEnd = 0u;
  std::uint32_t checkpointCount = 0u;
  std::uint32_t presentRecordCount = 0u;
  std::uint32_t presentBoundaryCount = 0u;
  bool sawPresentRecord = false;
  for (std::uint32_t i = 0u; i < events.size(); ++i) {
    const auto& eventHeader = events[i];
    if (!knownEventType(eventHeader.type)) {
      return failure(RenderTapeValidationStatus::InvalidEventType, i);
    }
    if (eventHeader.flags != 0u) {
      return failure(RenderTapeValidationStatus::InvalidEventFlags, i);
    }
    if (eventHeader.reserved0 != 0u || eventHeader.reserved1 != 0u) {
      return failure(RenderTapeValidationStatus::NonZeroReserved, i);
    }
    if (eventHeader.ordinal != static_cast<std::uint64_t>(i) + 1u) {
      return failure(RenderTapeValidationStatus::InvalidEventOrdinal, i);
    }

    std::uint64_t expectedOffset = 0u;
    std::uint64_t payloadEnd = 0u;
    if (!alignUp(expectedPayloadEnd, kRenderTapePayloadAlignment,
                 expectedOffset) ||
        eventHeader.payloadOffset != expectedOffset ||
        !checkedAdd(eventHeader.payloadOffset, eventHeader.payloadSize,
                    payloadEnd) ||
        payloadEnd > arena.size()) {
      return failure(RenderTapeValidationStatus::NonCanonicalEventLayout, i);
    }
    if (!zeroBytes(arena, expectedPayloadEnd, eventHeader.payloadOffset)) {
      return failure(RenderTapeValidationStatus::NonZeroPadding, i);
    }
    expectedPayloadEnd = payloadEnd;

    const auto event = candidate.event(i);
    if (event.payload.size() != eventHeader.payloadSize) {
      return failure(RenderTapeValidationStatus::InvalidEventRange, i);
    }

    switch (static_cast<RenderTapeEventType>(eventHeader.type)) {
    case RenderTapeEventType::Checkpoint: {
      if (sawPresentRecord) {
        return failure(RenderTapeValidationStatus::IncompleteFrame, i);
      }
      ++checkpointCount;
      RenderTapeCheckpointHeader fixed{};
      if (i != 0u || checkpointCount != 1u || !load(event.payload, 0u, fixed) ||
          fixed.stateVersion == 0u || fixed.reserved0 != 0u) {
        return failure(RenderTapeValidationStatus::InvalidCheckpoint, i);
      }
      std::uint64_t objectBytes = 0u;
      std::uint64_t stateOffset = 0u;
      std::uint64_t expectedSize = 0u;
      if (!checkedMul(fixed.objectCount, sizeof(D9CWireObjectIdentity),
                      objectBytes) ||
          !checkedAdd(sizeof(fixed), objectBytes, stateOffset) ||
          !checkedAdd(stateOffset, fixed.stateBytes, expectedSize) ||
          expectedSize != event.payload.size() || fixed.stateBytes == 0u) {
        return failure(RenderTapeValidationStatus::InvalidCheckpoint, i);
      }
      for (std::uint32_t objectIndex = 0u; objectIndex < fixed.objectCount;
           ++objectIndex) {
        D9CWireObjectIdentity identity{};
        if (!load(event.payload, sizeof(fixed) + objectIndex * sizeof(identity),
                  identity) ||
            !validIdentity(identity)) {
          return failure(RenderTapeValidationStatus::InvalidIdentity, i);
        }
        if (findObjectSlot(scratch.liveObjects, identity) !=
            scratch.liveObjects.end()) {
          return failure(RenderTapeValidationStatus::DuplicateIdentity, i);
        }
        try {
          scratch.liveObjects.push_back({.identity = identity});
        } catch (...) {
          return failure(RenderTapeValidationStatus::ScratchAllocationFailed,
                         i);
        }
      }
      break;
    }
    case RenderTapeEventType::ObjectCreate: {
      if (sawPresentRecord) {
        return failure(RenderTapeValidationStatus::IncompleteFrame, i);
      }
      RenderTapeObjectCreateHeader fixed{};
      if (!load(event.payload, 0u, fixed) || !validIdentity(fixed.identity) ||
          fixed.descriptorKind == 0u || fixed.descriptorBytes == 0u ||
          event.payload.size() != sizeof(fixed) + fixed.descriptorBytes) {
        return failure(RenderTapeValidationStatus::InvalidObjectCreate, i);
      }
      if (findObjectSlot(scratch.liveObjects, fixed.identity) !=
              scratch.liveObjects.end() ||
          findObjectSlot(scratch.retainedObjects, fixed.identity) !=
              scratch.retainedObjects.end()) {
        return failure(RenderTapeValidationStatus::DuplicateIdentity, i);
      }
      try {
        scratch.liveObjects.push_back({.identity = fixed.identity});
      } catch (...) {
        return failure(RenderTapeValidationStatus::ScratchAllocationFailed, i);
      }
      break;
    }
    case RenderTapeEventType::ResourceWrite: {
      if (sawPresentRecord) {
        return failure(RenderTapeValidationStatus::IncompleteFrame, i);
      }
      RenderTapeResourceWriteHeader fixed{};
      std::uint64_t writeEnd = 0u;
      if (!load(event.payload, 0u, fixed) || !validIdentity(fixed.identity) ||
          fixed.flags != 0u || fixed.reserved0 != 0u || fixed.dataBytes == 0u ||
          !checkedAdd(fixed.byteOffset, fixed.dataBytes, writeEnd) ||
          event.payload.size() != sizeof(fixed) + fixed.dataBytes) {
        return failure(RenderTapeValidationStatus::InvalidResourceWrite, i);
      }
      if (findIdentity(scratch.liveObjects, fixed.identity) ==
          scratch.liveObjects.end()) {
        return failure(RenderTapeValidationStatus::UnknownIdentity, i);
      }
      break;
    }
    case RenderTapeEventType::CommandChunk: {
      if (sawPresentRecord) {
        return failure(RenderTapeValidationStatus::IncompleteFrame, i);
      }
      RenderTapeCommandChunkHeader fixed{};
      if (!load(event.payload, 0u, fixed) ||
          fixed.wireVersion != D9C_COMMAND_CHUNK_WIRE_VERSION ||
          fixed.chunkBytes == 0u ||
          event.payload.size() != sizeof(fixed) + fixed.chunkBytes) {
        return failure(RenderTapeValidationStatus::InvalidCommandChunk, i);
      }
      ImportedChunkView chunk;
      const auto chunkResult =
          validateCommandChunk(tailAfter(event.payload, sizeof(fixed)),
                               CommandChunkEnvelope{
                                   .version = fixed.wireVersion,
                                   .recordCount = fixed.recordCount,
                                   .handleCount = fixed.handleCount,
                               },
                               &chunk, scratch.chunk);
      if (!chunkResult.valid()) {
        return failure(RenderTapeValidationStatus::InvalidCommandChunk, i,
                       chunkResult.status);
      }
      for (const auto& handle : chunk.handles) {
        const D9CWireObjectIdentity identity{
            .kind = handle.kind,
            .generation = handle.generation,
            .objectId = handle.objectId,
        };
        if (findIdentity(scratch.liveObjects, identity) ==
            scratch.liveObjects.end()) {
          return failure(RenderTapeValidationStatus::UnknownIdentity, i);
        }
        if (findObjectSlot(scratch.retainedObjects, identity) ==
            scratch.retainedObjects.end()) {
          try {
            scratch.retainedObjects.push_back({.identity = identity});
          } catch (...) {
            return failure(RenderTapeValidationStatus::ScratchAllocationFailed,
                           i);
          }
        }
      }
      presentRecordCount += static_cast<std::uint32_t>(std::count_if(
          chunk.records.begin(), chunk.records.end(), [](const auto& record) {
            return record.type == D9C_COMMAND_RECORD_PRESENT;
          }));
      sawPresentRecord = presentRecordCount != 0u;
      break;
    }
    case RenderTapeEventType::ObjectDestroy: {
      if (sawPresentRecord) {
        return failure(RenderTapeValidationStatus::IncompleteFrame, i);
      }
      D9CWireObjectIdentity identity{};
      if (event.payload.size() != sizeof(identity) ||
          !load(event.payload, 0u, identity) || !validIdentity(identity)) {
        return failure(RenderTapeValidationStatus::InvalidObjectDestroy, i);
      }
      const auto it = findIdentity(scratch.liveObjects, identity);
      if (it == scratch.liveObjects.end()) {
        return failure(RenderTapeValidationStatus::UnknownIdentity, i);
      }
      scratch.liveObjects.erase(it);
      break;
    }
    case RenderTapeEventType::PresentBoundary: {
      RenderTapePresentBoundary fixed{};
      ++presentBoundaryCount;
      if (event.payload.size() != sizeof(fixed) ||
          !load(event.payload, 0u, fixed) || fixed.presentOrdinal == 0u ||
          fixed.flags != 0u || fixed.reserved0 != 0u ||
          i + 1u != events.size()) {
        return failure(RenderTapeValidationStatus::InvalidPresentBoundary, i);
      }
      break;
    }
    }
  }

  if (!zeroBytes(arena, expectedPayloadEnd, arena.size())) {
    return failure(RenderTapeValidationStatus::NonZeroPadding);
  }
  if (checkpointCount != 1u) {
    return failure(RenderTapeValidationStatus::MissingCheckpoint);
  }
  if (header.presentCount != 1u || presentRecordCount != 1u ||
      presentBoundaryCount != 1u || events.empty() ||
      events.back().type !=
          static_cast<std::uint32_t>(RenderTapeEventType::PresentBoundary)) {
    return failure(RenderTapeValidationStatus::IncompleteFrame);
  }

  if (out) {
    *out = candidate;
  }
  return RenderTapeValidationResult{
      .status = RenderTapeValidationStatus::Valid,
  };
}

RenderTapeValidationResult
validateRenderTape(std::span<const std::byte> blob,
                   ImportedRenderTapeView* out) noexcept {
  thread_local RenderTapeValidationScratch scratch;
  return validateRenderTape(blob, out, scratch);
}

bool importPrevalidatedRenderTape(std::span<const std::byte> blob,
                                  ImportedRenderTapeView& out) noexcept {
  out = {};
  RenderTapeHeader header{};
  if (!load(blob, 0u, header) || header.magic != kRenderTapeMagic ||
      header.version != kRenderTapeVersion ||
      header.headerSize != sizeof(RenderTapeHeader) ||
      header.eventHeaderSize != sizeof(RenderTapeEventHeader) ||
      !pointerAligned(blob.data(), 0u, alignof(RenderTapeHeader)) ||
      !pointerAligned(blob.data(), header.eventTableOffset,
                      alignof(RenderTapeEventHeader)) ||
      !pointerAligned(blob.data(), header.payloadArenaOffset,
                      kRenderTapePayloadAlignment) ||
      !rangeValid(blob.size(), header.eventTableOffset,
                  static_cast<std::uint64_t>(header.eventCount) *
                      sizeof(RenderTapeEventHeader)) ||
      !rangeValid(blob.size(), header.payloadArenaOffset,
                  header.payloadArenaSize)) {
    return false;
  }
  out = ImportedRenderTapeView{
      .header = header,
      .events =
          header.eventCount == 0u
              ? std::span<const RenderTapeEventHeader>{}
              : std::span<
                    const RenderTapeEventHeader>{reinterpret_cast<
                                                     const RenderTapeEventHeader*>(
                                                     blob.data() +
                                                     header.eventTableOffset),
                                                 header.eventCount},
      .payloadArena =
          blob.subspan(header.payloadArenaOffset, header.payloadArenaSize),
  };
  return true;
}

RenderTapeReplayResult
replayPrevalidatedRenderTape(const ImportedRenderTapeView& tape,
                             RenderTapeReplaySink& sink) noexcept {
  for (std::uint32_t i = 0u; i < tape.events.size(); ++i) {
    const auto event = tape.event(i);
    bool accepted = false;
    switch (static_cast<RenderTapeEventType>(event.header.type)) {
    case RenderTapeEventType::Checkpoint: {
      RenderTapeCheckpointHeader fixed{};
      if (!load(event.payload, 0u, fixed)) {
        break;
      }
      const auto* identities = reinterpret_cast<const D9CWireObjectIdentity*>(
          event.payload.data() + sizeof(fixed));
      const auto stateOffset =
          sizeof(fixed) + fixed.objectCount * sizeof(*identities);
      accepted = sink.checkpoint(
          fixed.stateVersion,
          std::span<const D9CWireObjectIdentity>(identities, fixed.objectCount),
          event.payload.subspan(stateOffset, fixed.stateBytes));
      break;
    }
    case RenderTapeEventType::ObjectCreate: {
      RenderTapeObjectCreateHeader fixed{};
      accepted =
          load(event.payload, 0u, fixed) &&
          sink.objectCreate(fixed, tailAfter(event.payload, sizeof(fixed)));
      break;
    }
    case RenderTapeEventType::ResourceWrite: {
      RenderTapeResourceWriteHeader fixed{};
      accepted =
          load(event.payload, 0u, fixed) &&
          sink.resourceWrite(fixed, tailAfter(event.payload, sizeof(fixed)));
      break;
    }
    case RenderTapeEventType::CommandChunk: {
      RenderTapeCommandChunkHeader fixed{};
      accepted = load(event.payload, 0u, fixed) &&
                 sink.commandChunk(
                     CommandChunkEnvelope{
                         .version = fixed.wireVersion,
                         .recordCount = fixed.recordCount,
                         .handleCount = fixed.handleCount,
                     },
                     tailAfter(event.payload, sizeof(fixed)));
      break;
    }
    case RenderTapeEventType::ObjectDestroy: {
      D9CWireObjectIdentity identity{};
      accepted =
          load(event.payload, 0u, identity) && sink.objectDestroy(identity);
      break;
    }
    case RenderTapeEventType::PresentBoundary: {
      RenderTapePresentBoundary fixed{};
      accepted = load(event.payload, 0u, fixed) && sink.presentBoundary(fixed);
      break;
    }
    }
    if (!accepted) {
      return RenderTapeReplayResult{
          .failedEventIndex = i,
      };
    }
  }
  return RenderTapeReplayResult{.complete = true};
}

void RenderTapeBuilder::append(RenderTapeEventType type,
                               std::vector<std::byte> payload) {
  events_.push_back(PendingEvent{
      .type = type,
      .payload = std::move(payload),
  });
}

void RenderTapeBuilder::appendCheckpoint(
    std::uint32_t stateVersion,
    std::span<const D9CWireObjectIdentity> initialObjects,
    std::span<const std::byte> state) {
  const RenderTapeCheckpointHeader fixed{
      .stateVersion = stateVersion,
      .objectCount = static_cast<std::uint32_t>(initialObjects.size()),
      .stateBytes = static_cast<std::uint32_t>(state.size()),
  };
  auto payload = payloadWithTail(fixed, std::as_bytes(initialObjects));
  payload.insert(payload.end(), state.begin(), state.end());
  append(RenderTapeEventType::Checkpoint, std::move(payload));
}

void RenderTapeBuilder::appendObjectCreate(
    const D9CWireObjectIdentity& identity, std::uint32_t descriptorKind,
    std::span<const std::byte> descriptor) {
  append(
      RenderTapeEventType::ObjectCreate,
      payloadWithTail(
          RenderTapeObjectCreateHeader{
              .identity = identity,
              .descriptorKind = descriptorKind,
              .descriptorBytes = static_cast<std::uint32_t>(descriptor.size()),
          },
          descriptor));
}

void RenderTapeBuilder::appendResourceWrite(
    const D9CWireObjectIdentity& identity, std::uint32_t subresource,
    std::uint64_t byteOffset, std::span<const std::byte> data) {
  append(RenderTapeEventType::ResourceWrite,
         payloadWithTail(
             RenderTapeResourceWriteHeader{
                 .identity = identity,
                 .subresource = subresource,
                 .byteOffset = byteOffset,
                 .dataBytes = static_cast<std::uint32_t>(data.size()),
             },
             data));
}

void RenderTapeBuilder::appendCommandChunk(const CommandChunkEnvelope& envelope,
                                           std::span<const std::byte> chunk) {
  append(RenderTapeEventType::CommandChunk,
         payloadWithTail(
             RenderTapeCommandChunkHeader{
                 .wireVersion = envelope.version,
                 .recordCount = envelope.recordCount,
                 .handleCount = envelope.handleCount,
                 .chunkBytes = static_cast<std::uint32_t>(chunk.size()),
             },
             chunk));
}

void RenderTapeBuilder::appendObjectDestroy(
    const D9CWireObjectIdentity& identity) {
  append(RenderTapeEventType::ObjectDestroy, payloadWithTail(identity, {}));
}

void RenderTapeBuilder::appendPresentBoundary(std::uint64_t presentOrdinal) {
  append(RenderTapeEventType::PresentBoundary,
         payloadWithTail(
             RenderTapePresentBoundary{
                 .presentOrdinal = presentOrdinal,
             },
             {}));
}

std::vector<std::byte> RenderTapeBuilder::seal() const {
  if (events_.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("render tape event count exceeds uint32_t");
  }
  std::vector<RenderTapeEventHeader> headers;
  std::vector<std::byte> arena;
  headers.reserve(events_.size());
  std::uint32_t presentCount = 0u;
  for (std::size_t i = 0u; i < events_.size(); ++i) {
    const auto aligned = (arena.size() + kRenderTapePayloadAlignment - 1u) &
                         ~(kRenderTapePayloadAlignment - 1u);
    arena.resize(aligned);
    const auto& event = events_[i];
    if (arena.size() > std::numeric_limits<std::uint32_t>::max() ||
        event.payload.size() > std::numeric_limits<std::uint32_t>::max() ||
        arena.size() + event.payload.size() >
            std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("render tape payload exceeds uint32_t");
    }
    headers.push_back(RenderTapeEventHeader{
        .type = static_cast<std::uint32_t>(event.type),
        .ordinal = static_cast<std::uint64_t>(i) + 1u,
        .payloadOffset = static_cast<std::uint32_t>(arena.size()),
        .payloadSize = static_cast<std::uint32_t>(event.payload.size()),
    });
    arena.insert(arena.end(), event.payload.begin(), event.payload.end());
    if (event.type == RenderTapeEventType::PresentBoundary) {
      ++presentCount;
    }
  }

  const auto eventTableOffset = sizeof(RenderTapeHeader);
  const auto eventTableBytes = headers.size() * sizeof(RenderTapeEventHeader);
  const auto payloadArenaOffset = (eventTableOffset + eventTableBytes +
                                   alignof(RenderTapeEventHeader) - 1u) &
                                  ~(alignof(RenderTapeEventHeader) - 1u);
  if (payloadArenaOffset + arena.size() >
      std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("render tape blob exceeds uint32_t");
  }
  const RenderTapeHeader header{
      .eventHeaderSize = sizeof(RenderTapeEventHeader),
      .eventTableOffset = static_cast<std::uint32_t>(eventTableOffset),
      .eventCount = static_cast<std::uint32_t>(headers.size()),
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadArenaOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(arena.size()),
      .presentCount = presentCount,
  };
  std::vector<std::byte> blob(payloadArenaOffset + arena.size());
  std::memcpy(blob.data(), &header, sizeof(header));
  if (!headers.empty()) {
    std::memcpy(blob.data() + eventTableOffset, headers.data(),
                headers.size() * sizeof(headers[0]));
  }
  if (!arena.empty()) {
    std::memcpy(blob.data() + payloadArenaOffset, arena.data(), arena.size());
  }
  const auto validation = validateRenderTape(blob);
  if (!validation.valid()) {
    throw std::invalid_argument(
        std::string("cannot seal invalid render tape: ") +
        renderTapeValidationStatusName(validation.status));
  }
  return blob;
}

const char*
renderTapeValidationStatusName(RenderTapeValidationStatus status) noexcept {
  switch (status) {
  case RenderTapeValidationStatus::Valid:
    return "valid";
  case RenderTapeValidationStatus::MissingHeader:
    return "missing-header";
  case RenderTapeValidationStatus::InvalidHeader:
    return "invalid-header";
  case RenderTapeValidationStatus::InvalidLayout:
    return "invalid-layout";
  case RenderTapeValidationStatus::NonZeroReserved:
    return "nonzero-reserved";
  case RenderTapeValidationStatus::InvalidEventType:
    return "invalid-event-type";
  case RenderTapeValidationStatus::InvalidEventFlags:
    return "invalid-event-flags";
  case RenderTapeValidationStatus::InvalidEventOrdinal:
    return "invalid-event-ordinal";
  case RenderTapeValidationStatus::InvalidEventRange:
    return "invalid-event-range";
  case RenderTapeValidationStatus::NonCanonicalEventLayout:
    return "noncanonical-event-layout";
  case RenderTapeValidationStatus::NonZeroPadding:
    return "nonzero-padding";
  case RenderTapeValidationStatus::MissingCheckpoint:
    return "missing-checkpoint";
  case RenderTapeValidationStatus::InvalidCheckpoint:
    return "invalid-checkpoint";
  case RenderTapeValidationStatus::InvalidIdentity:
    return "invalid-identity";
  case RenderTapeValidationStatus::DuplicateIdentity:
    return "duplicate-identity";
  case RenderTapeValidationStatus::UnknownIdentity:
    return "unknown-identity";
  case RenderTapeValidationStatus::InvalidObjectCreate:
    return "invalid-object-create";
  case RenderTapeValidationStatus::InvalidResourceWrite:
    return "invalid-resource-write";
  case RenderTapeValidationStatus::InvalidCommandChunk:
    return "invalid-command-chunk";
  case RenderTapeValidationStatus::InvalidObjectDestroy:
    return "invalid-object-destroy";
  case RenderTapeValidationStatus::InvalidPresentBoundary:
    return "invalid-present-boundary";
  case RenderTapeValidationStatus::IncompleteFrame:
    return "incomplete-frame";
  case RenderTapeValidationStatus::ScratchAllocationFailed:
    return "scratch-allocation-failed";
  }
  return "unknown";
}

} // namespace dxmt9::d3d9
