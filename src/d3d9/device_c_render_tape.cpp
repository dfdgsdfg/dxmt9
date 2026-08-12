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

bool sameSlot(const D9CWireObjectIdentity& a,
              const D9CWireObjectIdentity& b) noexcept {
  return a.kind == b.kind && a.objectId == b.objectId;
}

auto findLiveSlot(std::vector<RenderTapeValidationScratch::LiveSlot>& live,
                  const D9CWireObjectIdentity& identity) noexcept {
  return std::find_if(live.begin(), live.end(), [&](const auto& entry) {
    return !entry.retired && sameIdentity(entry.identity, identity);
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
  return value >=
             static_cast<std::uint32_t>(RenderTapeEventType::BootstrapState) &&
         value <=
             static_cast<std::uint32_t>(RenderTapeEventType::PresentComplete);
}

bool knownControlKind(std::uint32_t value) noexcept {
  return value >=
             static_cast<std::uint32_t>(RenderTapeControlKind::QueryGetData) &&
         value <= static_cast<std::uint32_t>(RenderTapeControlKind::DeviceLost);
}

bool knownControlDisposition(std::uint32_t value) noexcept {
  return value >= static_cast<std::uint32_t>(
                      RenderTapeControlDisposition::Completed) &&
         value <= static_cast<std::uint32_t>(
                      RenderTapeControlDisposition::Terminal);
}

bool validControlDisposition(RenderTapeControlKind kind,
                             RenderTapeControlDisposition disposition) noexcept {
  switch (kind) {
  case RenderTapeControlKind::QueryGetData:
    return disposition == RenderTapeControlDisposition::Completed ||
           disposition == RenderTapeControlDisposition::Pending ||
           disposition == RenderTapeControlDisposition::Failed;
  case RenderTapeControlKind::CpuRead:
  case RenderTapeControlKind::FlushWait:
    return disposition == RenderTapeControlDisposition::Completed ||
           disposition == RenderTapeControlDisposition::Failed;
  case RenderTapeControlKind::Reset:
  case RenderTapeControlKind::DeviceLost:
    return disposition == RenderTapeControlDisposition::Terminal ||
           disposition == RenderTapeControlDisposition::Failed;
  }
  return false;
}

bool nullIdentity(const D9CWireObjectIdentity& identity) noexcept {
  return identity.kind == 0u && identity.generation == 0u &&
         identity.objectId == 0u;
}

bool zeroDigest(const RenderTapeDigest& digest) noexcept {
  return std::all_of(digest.begin(), digest.end(),
                     [](std::byte value) { return value == std::byte{}; });
}

bool knownMutationKind(std::uint32_t value) noexcept {
  return value >=
             static_cast<std::uint32_t>(RenderTapeMutationKind::CpuUnlock) &&
         value <=
             static_cast<std::uint32_t>(RenderTapeMutationKind::MipmapClass);
}

bool mutationCapableIdentity(const D9CWireObjectIdentity& identity) noexcept {
  return identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE ||
         identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE ||
         identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER;
}

bool requiresImmutablePayload(
    const D9CWireObjectIdentity& identity) noexcept {
  return identity.kind == D9C_CHUNK_HANDLE_KIND_SHADER ||
         identity.kind == D9C_CHUNK_HANDLE_KIND_VERTEX_DECL;
}

// DERIVES total byte length of a canonical command chunk from its own wire
// header (payloadArenaOffset + payloadArenaSize).
bool chunkTotalBytes(std::span<const std::byte> blob,
                     std::uint64_t& total) noexcept {
  D9CCommandChunkWireHeader wire{};
  if (!load(blob, 0u, wire)) {
    return false;
  }
  std::uint64_t end = 0u;
  if (!checkedAdd(wire.payloadArenaOffset, wire.payloadArenaSize, end)) {
    return false;
  }
  total = end;
  return end <= blob.size() && end >= sizeof(D9CCommandChunkWireHeader);
}

std::span<const std::byte> tailAfter(std::span<const std::byte> payload,
                                     std::size_t fixedSize) noexcept {
  return payload.size() < fixedSize ? std::span<const std::byte>{}
                                    : payload.subspan(fixedSize);
}

} // namespace

RenderTapeBlobLookup RenderTapeBlobCatalogue::lookup(
    std::span<const std::byte, kRenderTapeDigestSize> digest,
    std::uint64_t size) const noexcept {
  bool foundDigest = false;
  bool foundSize = false;
  for (const auto& blob : blobs) {
    if (std::memcmp(blob.digest.data(), digest.data(),
                    kRenderTapeDigestSize) != 0) {
      continue;
    }
    foundDigest = true;
    if (blob.size != size) {
      continue;
    }
    foundSize = true;
    if (blob.verified == 1u) {
      return RenderTapeBlobLookup::Exact;
    }
  }
  if (foundSize) {
    return RenderTapeBlobLookup::Unverified;
  }
  return foundDigest ? RenderTapeBlobLookup::SizeMismatch
                     : RenderTapeBlobLookup::UnknownDigest;
}

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
validateRenderTape(std::span<const std::byte> blob,
                   const RenderTapeBlobCatalogue& catalogue,
                   ImportedRenderTapeView* out,
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
  try {
    scratch.liveObjects.reserve(header.eventCount);
  } catch (...) {
    return failure(RenderTapeValidationStatus::ScratchAllocationFailed);
  }

  bool sawBootstrap = false;
  bool sawPresent = false;
  bool sawChunkPresent = false;
  bool sawReset = false;
  std::uint64_t presentCommandOrdinal = 0u;
  std::uint64_t previousCompletion = 0u;
  std::uint64_t expectedPayloadEnd = 0u;

  for (std::uint32_t i = 0u; i < header.eventCount; ++i) {
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
    case RenderTapeEventType::BootstrapState: {
      if (i != 0u) {
        return failure(RenderTapeValidationStatus::BootstrapNotFirst, i);
      }
      if (sawBootstrap) {
        return failure(RenderTapeValidationStatus::DuplicateBootstrap, i);
      }
      sawBootstrap = true;
      RenderTapeBootstrapHeader fixed{};
      if (!load(event.payload, 0u, fixed) ||
          fixed.stateCategoryCount != kRenderTapeStateCategoryCount ||
          fixed.reserved0 != 0u ||
          fixed.baselineProfileVersion != kRenderTapeBaselineProfileVersion ||
          fixed.overlayCount == 0u) {
        return failure(RenderTapeValidationStatus::InvalidBootstrap, i);
      }
      if (fixed.requiredCategoryMask != kRenderTapeRequiredCategoryMask) {
        // Unknown bits (>= category count) or missing bits both land here.
        return failure(RenderTapeValidationStatus::BootstrapCoverageIncomplete,
                       i);
      }
      // Walk the concatenated overlay chunks. Each must be a canonical
      // APPLY_STATE-only command chunk (state-only; no draw/clear/present/
      // update/query/readback/control records).
      const auto overlays = tailAfter(event.payload, sizeof(fixed));
      std::uint64_t walk = 0u;
      for (std::uint32_t o = 0u; o < fixed.overlayCount; ++o) {
        std::uint64_t total = 0u;
        if (!chunkTotalBytes(overlays.subspan(walk), total)) {
          return failure(RenderTapeValidationStatus::InvalidBootstrapChunk, i);
        }
        const auto overlay = overlays.subspan(walk, total);
        walk += total;
        D9CCommandChunkWireHeader overlayHeader{};
        if (!load(overlay, 0u, overlayHeader)) {
          return failure(RenderTapeValidationStatus::InvalidBootstrapChunk, i);
        }
        ImportedChunkView chunk;
        const auto chunkResult =
            validateCommandChunk(overlay,
                                 CommandChunkEnvelope{
                                     .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                                     .recordCount = overlayHeader.recordCount,
                                     .handleCount = overlayHeader.handleCount,
                                 },
                                 &chunk, scratch.chunk);
        if (!chunkResult.valid()) {
          return failure(RenderTapeValidationStatus::InvalidBootstrapChunk, i,
                         chunkResult.status);
        }
        for (std::size_t r = 0u; r < chunk.records.size(); ++r) {
          const auto record = chunk.record(r);
          if (record.header.type != D9C_COMMAND_RECORD_APPLY_STATE) {
            return failure(RenderTapeValidationStatus::BootstrapForbiddenRecord,
                           i);
          }
          if ((record.drawHeader.flags &
               D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT) == 0u) {
            return failure(RenderTapeValidationStatus::InvalidBootstrapChunk,
                           i);
          }
        }
      }
      if (walk != overlays.size()) {
        return failure(RenderTapeValidationStatus::InvalidBootstrapChunk, i);
      }
      break;
    }
    case RenderTapeEventType::ObjectDefine: {
      if (!sawBootstrap || sawPresent) {
        return failure(RenderTapeValidationStatus::IncompleteFrame, i);
      }
      RenderTapeObjectDefineHeader fixed{};
      if (!load(event.payload, 0u, fixed) || !validIdentity(fixed.identity) ||
          fixed.descriptorKind == 0u || fixed.descriptorBytes == 0u ||
          fixed.reserved0 != 0u) {
        return failure(RenderTapeValidationStatus::InvalidObjectDefine, i);
      }
      const auto payloadValidity =
          static_cast<RenderTapeDigestValidity>(fixed.payloadValidity);
      if (payloadValidity != RenderTapeDigestValidity::NotCaptured &&
          payloadValidity != RenderTapeDigestValidity::Sha256) {
        return failure(RenderTapeValidationStatus::InvalidObjectDefine, i);
      }
      if (requiresImmutablePayload(fixed.identity) &&
          payloadValidity != RenderTapeDigestValidity::Sha256) {
        return failure(RenderTapeValidationStatus::InvalidObjectDefine, i);
      }
      const std::uint64_t expectedSize = sizeof(fixed) + fixed.descriptorBytes;
      if (event.payload.size() != expectedSize) {
        return failure(RenderTapeValidationStatus::InvalidObjectDefine, i);
      }
      if (payloadValidity == RenderTapeDigestValidity::Sha256) {
        if (fixed.immutablePayloadBytes == 0u) {
          return failure(RenderTapeValidationStatus::InvalidObjectDefine, i);
        }
        switch (catalogue.lookup(fixed.immutablePayloadDigest,
                                 fixed.immutablePayloadBytes)) {
        case RenderTapeBlobLookup::Exact:
          break;
        case RenderTapeBlobLookup::UnknownDigest:
          return failure(RenderTapeValidationStatus::UnknownBlob, i);
        case RenderTapeBlobLookup::SizeMismatch:
          return failure(RenderTapeValidationStatus::BlobSizeMismatch, i);
        case RenderTapeBlobLookup::Unverified:
          return failure(RenderTapeValidationStatus::BlobDigestMismatch, i);
        }
      } else if (fixed.immutablePayloadBytes != 0u ||
                 !zeroDigest(fixed.immutablePayloadDigest)) {
        return failure(RenderTapeValidationStatus::InvalidObjectDefine, i);
      }
      // Prevent duplicate live generations and reuse while a live slot holds
      // the same {kind,objectId}.
      const auto priorSlot = std::find_if(
          scratch.liveObjects.begin(), scratch.liveObjects.end(),
          [&](const auto& entry) { return sameSlot(entry.identity, fixed.identity); });
      if (priorSlot != scratch.liveObjects.end()) {
        return failure(priorSlot->retired
                           ? RenderTapeValidationStatus::RetainedSlotReuse
                           : RenderTapeValidationStatus::DuplicateGeneration,
                       i);
      }
      scratch.liveObjects.push_back(RenderTapeValidationScratch::LiveSlot{
          .identity = fixed.identity,
          .descriptorKind = fixed.descriptorKind,
          .lastUseOrdinal = eventHeader.ordinal,
          .retired = false,
      });
      break;
    }
    case RenderTapeEventType::ObjectDestroy: {
      if (!sawBootstrap || sawPresent) {
        return failure(RenderTapeValidationStatus::IncompleteFrame, i);
      }
      RenderTapeObjectDestroyHeader fixed{};
      if (!load(event.payload, 0u, fixed) || !validIdentity(fixed.identity) ||
          event.payload.size() != sizeof(fixed)) {
        return failure(RenderTapeValidationStatus::InvalidObjectDestroy, i);
      }
      const auto live = findLiveSlot(scratch.liveObjects, fixed.identity);
      if (live == scratch.liveObjects.end()) {
        return failure(RenderTapeValidationStatus::UnknownIdentity, i);
      }
      live->retired = true;
      break;
    }
    case RenderTapeEventType::ResourceMutation: {
      if (!sawBootstrap || sawPresent) {
        return failure(RenderTapeValidationStatus::IncompleteFrame, i);
      }
      RenderTapeResourceMutationHeader fixed{};
      if (!load(event.payload, 0u, fixed) || !validIdentity(fixed.identity) ||
          !mutationCapableIdentity(fixed.identity) ||
          !knownMutationKind(fixed.kind) || fixed.reserved0 != 0u ||
          event.payload.size() != sizeof(fixed)) {
        return failure(RenderTapeValidationStatus::InvalidMutationKind, i);
      }
      const auto live = findLiveSlot(scratch.liveObjects, fixed.identity);
      if (live == scratch.liveObjects.end()) {
        return failure(RenderTapeValidationStatus::UnknownIdentity, i);
      }
      std::uint64_t regionEnd = 0u;
      if (!checkedAdd(fixed.byteOffset, fixed.byteSize, regionEnd)) {
        return failure(RenderTapeValidationStatus::InvalidMutationRange, i);
      }
      if (fixed.byteSize == 0u) {
        return failure(RenderTapeValidationStatus::InvalidMutationRange, i);
      }
      switch (catalogue.lookup(fixed.digest, fixed.byteSize)) {
      case RenderTapeBlobLookup::Exact:
        break;
      case RenderTapeBlobLookup::UnknownDigest:
        return failure(RenderTapeValidationStatus::UnknownBlob, i);
      case RenderTapeBlobLookup::SizeMismatch:
        return failure(RenderTapeValidationStatus::BlobSizeMismatch, i);
      case RenderTapeBlobLookup::Unverified:
        return failure(RenderTapeValidationStatus::BlobDigestMismatch, i);
      }
      break;
    }
    case RenderTapeEventType::CommandChunk: {
      if (!sawBootstrap || sawPresent) {
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
      // Every handle referenced by the chunk must name a live object.
      for (const auto& handle : chunk.handles) {
        const D9CWireObjectIdentity identity{
            .kind = handle.kind,
            .generation = handle.generation,
            .objectId = handle.objectId,
        };
        if (!validIdentity(identity) ||
            findLiveSlot(scratch.liveObjects, identity) ==
                scratch.liveObjects.end()) {
          return failure(RenderTapeValidationStatus::UnknownIdentity, i);
        }
      }
      for (const auto& record : chunk.records) {
        if (record.type == D9C_COMMAND_RECORD_PRESENT) {
          if (sawChunkPresent) {
            return failure(RenderTapeValidationStatus::InvalidCommandChunk, i);
          }
          sawChunkPresent = true;
          presentCommandOrdinal = eventHeader.ordinal;
        }
      }
      break;
    }
    case RenderTapeEventType::OrderedControl: {
      if (!sawBootstrap || sawPresent) {
        return failure(RenderTapeValidationStatus::IncompleteFrame, i);
      }
      if (sawReset) {
        return failure(RenderTapeValidationStatus::ResetNotTerminal, i);
      }
      RenderTapeOrderedControlHeader fixed{};
      if (!load(event.payload, 0u, fixed) || !knownControlKind(fixed.kind) ||
          !knownControlDisposition(fixed.disposition) ||
          fixed.reserved0 != 0u || fixed.reserved1 != 0u) {
        return failure(RenderTapeValidationStatus::InvalidControlKind, i);
      }
      const auto controlKind = static_cast<RenderTapeControlKind>(fixed.kind);
      const auto disposition =
          static_cast<RenderTapeControlDisposition>(fixed.disposition);
      if (!validControlDisposition(controlKind, disposition)) {
        return failure(RenderTapeValidationStatus::InvalidControlKind, i);
      }
      const bool objectScoped = controlKind == RenderTapeControlKind::QueryGetData ||
                                controlKind == RenderTapeControlKind::CpuRead;
      if (objectScoped) {
        const bool expectedIdentityKind =
            controlKind == RenderTapeControlKind::QueryGetData
                ? fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_QUERY
                : mutationCapableIdentity(fixed.identity);
        if (!expectedIdentityKind || !validIdentity(fixed.identity) ||
            findLiveSlot(scratch.liveObjects, fixed.identity) ==
                scratch.liveObjects.end()) {
          return failure(RenderTapeValidationStatus::UnknownIdentity, i);
        }
      } else if (!nullIdentity(fixed.identity)) {
        return failure(RenderTapeValidationStatus::InvalidControlKind, i);
      }
      std::size_t expectedTail = 0u;
      switch (static_cast<RenderTapeControlKind>(fixed.kind)) {
      case RenderTapeControlKind::QueryGetData:
        expectedTail = sizeof(RenderTapeQueryGetDataControl);
        break;
      case RenderTapeControlKind::CpuRead:
        expectedTail = sizeof(RenderTapeCpuReadControl);
        break;
      case RenderTapeControlKind::FlushWait:
        expectedTail = sizeof(RenderTapeFlushWaitControl);
        break;
      case RenderTapeControlKind::Reset:
        expectedTail = sizeof(RenderTapeResetControl);
        break;
      case RenderTapeControlKind::DeviceLost:
        expectedTail = sizeof(RenderTapeDeviceLostControl);
        break;
      }
      if (fixed.controlBytes != expectedTail ||
          event.payload.size() != sizeof(fixed) + expectedTail) {
        return failure(RenderTapeValidationStatus::InvalidControlSize, i);
      }
      if (fixed.completionOrdinal < previousCompletion) {
        return failure(RenderTapeValidationStatus::NonMonotoneCompletion, i);
      }
      previousCompletion = fixed.completionOrdinal;
      if (controlKind == RenderTapeControlKind::FlushWait) {
        RenderTapeFlushWaitControl control{};
        if (!load(event.payload, sizeof(fixed), control) ||
            control.waitedSeqId > fixed.completionOrdinal) {
          return failure(RenderTapeValidationStatus::InvalidControlSize, i);
        }
      } else if (controlKind == RenderTapeControlKind::Reset) {
        RenderTapeResetControl control{};
        if (!load(event.payload, sizeof(fixed), control) ||
            control.terminal != 1u) {
          return failure(RenderTapeValidationStatus::InvalidControlSize, i);
        }
        if (disposition == RenderTapeControlDisposition::Terminal) {
          sawReset = true;
        }
      } else if (controlKind == RenderTapeControlKind::DeviceLost) {
        RenderTapeDeviceLostControl control{};
        if (!load(event.payload, sizeof(fixed), control) ||
            control.reserved0 != 0u ||
            control.hrCode != static_cast<std::uint32_t>(fixed.resultCode)) {
          return failure(RenderTapeValidationStatus::InvalidControlSize, i);
        }
        if (disposition == RenderTapeControlDisposition::Terminal) {
          sawReset = true;
        }
      }
      break;
    }
    case RenderTapeEventType::PresentComplete: {
      if (sawPresent) {
        return failure(RenderTapeValidationStatus::InvalidPresentComplete, i);
      }
      if (i != header.eventCount - 1u) {
        return failure(RenderTapeValidationStatus::PresentNotLast, i);
      }
      RenderTapePresentCompleteHeader fixed{};
      if (!sawBootstrap || sawReset || !sawChunkPresent ||
          !load(event.payload, 0u, fixed) || fixed.presentOrdinal == 0u ||
          fixed.presentOrdinal != presentCommandOrdinal ||
          fixed.completionOrdinal == 0u || fixed.reserved0 != 0u ||
          fixed.completionOrdinal < previousCompletion ||
          fixed.oracleCount == 0u) {
        return failure(RenderTapeValidationStatus::InvalidPresentComplete, i);
      }
      const auto digestValidity =
          static_cast<RenderTapeDigestValidity>(fixed.digestValidity);
      if (digestValidity != RenderTapeDigestValidity::NotCaptured &&
          digestValidity != RenderTapeDigestValidity::Sha256) {
        return failure(RenderTapeValidationStatus::InvalidPresentComplete, i);
      }
      if (digestValidity == RenderTapeDigestValidity::NotCaptured &&
          !zeroDigest(fixed.expectedDigest)) {
        return failure(RenderTapeValidationStatus::InvalidPresentComplete, i);
      }
      sawPresent = true;
      std::uint64_t oracleBytes = 0u;
      if (!checkedMul(fixed.oracleCount, sizeof(RenderTapeOracleAttachment),
                      oracleBytes) ||
          fixed.oracleBytes != oracleBytes ||
          event.payload.size() != sizeof(fixed) + fixed.oracleBytes) {
        return failure(RenderTapeValidationStatus::InvalidPresentComplete, i);
      }
      for (std::uint32_t a = 0u; a < fixed.oracleCount; ++a) {
        RenderTapeOracleAttachment attachment{};
        if (!load(event.payload, sizeof(fixed) +
                                     a * sizeof(RenderTapeOracleAttachment),
                  attachment) ||
            !validIdentity(attachment.identity) ||
            attachment.descriptorKind == 0u || attachment.reserved0 != 0u) {
          return failure(RenderTapeValidationStatus::InvalidPresentComplete, i);
        }
        const auto live = findLiveSlot(scratch.liveObjects, attachment.identity);
        if (live == scratch.liveObjects.end() ||
            live->descriptorKind != attachment.descriptorKind) {
          return failure(RenderTapeValidationStatus::InvalidPresentComplete, i);
        }
      }
      break;
    }
    }
  }

  if (!sawBootstrap) {
    return failure(RenderTapeValidationStatus::MissingBootstrap);
  }
  if (!sawPresent) {
    return failure(RenderTapeValidationStatus::IncompleteFrame);
  }
  if (sawPresent && header.presentCount != 1u) {
    return failure(RenderTapeValidationStatus::InvalidPresentComplete);
  }
  if (expectedPayloadEnd != arena.size()) {
    return failure(RenderTapeValidationStatus::NonCanonicalEventLayout);
  }

  if (out) {
    *out = candidate;
  }
  return failure(RenderTapeValidationStatus::Valid);
}

RenderTapeValidationResult
validateRenderTape(std::span<const std::byte> blob,
                   const RenderTapeBlobCatalogue& catalogue,
                   ImportedRenderTapeView* out) noexcept {
  thread_local RenderTapeValidationScratch scratch;
  return validateRenderTape(blob, catalogue, out, scratch);
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
                             const RenderTapeBlobCatalogue& catalogue,
                             RenderTapeReplaySink& sink) noexcept {
  static_cast<void>(catalogue);
  RenderTapeReplayResult result{};
  for (std::uint32_t i = 0u; i < tape.events.size(); ++i) {
    const auto event = tape.event(i);
    bool accepted = false;
    switch (static_cast<RenderTapeEventType>(event.header.type)) {
    case RenderTapeEventType::BootstrapState: {
      RenderTapeBootstrapHeader fixed{};
      accepted = load(event.payload, 0u, fixed) &&
                 sink.bootstrap(fixed, tailAfter(event.payload, sizeof(fixed)));
      break;
    }
    case RenderTapeEventType::ObjectDefine: {
      RenderTapeObjectDefineHeader fixed{};
      accepted = load(event.payload, 0u, fixed);
      if (accepted) {
        std::span<const std::byte> descriptor =
            event.payload.subspan(sizeof(fixed), fixed.descriptorBytes);
        accepted = sink.objectDefine(fixed, descriptor);
      }
      break;
    }
    case RenderTapeEventType::ObjectDestroy: {
      RenderTapeObjectDestroyHeader fixed{};
      accepted =
          load(event.payload, 0u, fixed) && sink.objectDestroy(fixed);
      break;
    }
    case RenderTapeEventType::ResourceMutation: {
      RenderTapeResourceMutationHeader fixed{};
      accepted = load(event.payload, 0u, fixed) &&
                 sink.resourceMutation(fixed);
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
    case RenderTapeEventType::OrderedControl: {
      RenderTapeOrderedControlHeader fixed{};
      accepted = load(event.payload, 0u, fixed) &&
                 sink.orderedControl(
                     fixed, tailAfter(event.payload, sizeof(fixed)));
      break;
    }
    case RenderTapeEventType::PresentComplete: {
      RenderTapePresentCompleteHeader fixed{};
      accepted = load(event.payload, 0u, fixed) &&
                 sink.presentComplete(
                     fixed, tailAfter(event.payload, sizeof(fixed)));
      break;
    }
    }
    if (!accepted) {
      result.failedEventIndex = i;
      return result;
    }
  }
  result.complete = true;
  return result;
}

void RenderTapeBuilder::append(RenderTapeEventType type,
                               std::vector<std::byte> payload) {
  events_.push_back(PendingEvent{
      .type = type,
      .payload = std::move(payload),
  });
}

void RenderTapeBuilder::appendBootstrapState(
    std::span<const std::byte> overlayChunks) {
  std::uint32_t overlayCount = 0u;
  std::uint64_t walk = 0u;
  while (walk < overlayChunks.size()) {
    std::uint64_t total = 0u;
    if (!chunkTotalBytes(overlayChunks.subspan(walk), total)) {
      throw std::invalid_argument("bootstrap overlay is not a canonical chunk");
    }
    walk += total;
    ++overlayCount;
  }
  if (overlayCount == 0u) {
    throw std::invalid_argument("bootstrap requires at least one overlay");
  }
  append(
      RenderTapeEventType::BootstrapState,
      payloadWithTail(
          RenderTapeBootstrapHeader{
              .baselineProfileVersion = kRenderTapeBaselineProfileVersion,
              .stateCategoryCount = kRenderTapeStateCategoryCount,
              .overlayCount = overlayCount,
          },
          overlayChunks));
}

void RenderTapeBuilder::appendObjectDefine(const D9CWireObjectIdentity& identity,
                                           std::uint32_t descriptorKind,
                                           std::span<const std::byte> descriptor,
                                           std::uint64_t immutablePayloadBytes,
                                           RenderTapeDigest immutablePayloadDigest) {
  append(RenderTapeEventType::ObjectDefine,
         payloadWithTail(
             RenderTapeObjectDefineHeader{
                 .identity = identity,
                 .descriptorKind = descriptorKind,
                 .descriptorBytes =
                     static_cast<std::uint32_t>(descriptor.size()),
                 .payloadValidity = static_cast<std::uint32_t>(
                     immutablePayloadBytes == 0u
                         ? RenderTapeDigestValidity::NotCaptured
                         : RenderTapeDigestValidity::Sha256),
                 .immutablePayloadBytes = immutablePayloadBytes,
                 .immutablePayloadDigest = immutablePayloadDigest,
             },
             descriptor));
}

void RenderTapeBuilder::appendObjectDestroy(
    const D9CWireObjectIdentity& identity) {
  append(RenderTapeEventType::ObjectDestroy,
         payloadWithTail(RenderTapeObjectDestroyHeader{.identity = identity},
                         {}));
}

void RenderTapeBuilder::appendResourceMutation(
    const D9CWireObjectIdentity& identity, RenderTapeMutationKind kind,
    std::uint32_t subresource, std::uint64_t byteOffset, std::uint64_t byteSize,
    std::span<const std::byte, kRenderTapeDigestSize> digest) {
  RenderTapeResourceMutationHeader fixed{
      .identity = identity,
      .kind = static_cast<std::uint32_t>(kind),
      .subresource = subresource,
      .byteOffset = byteOffset,
      .byteSize = byteSize,
      .digest = {},
  };
  std::memcpy(fixed.digest.data(), digest.data(), kRenderTapeDigestSize);
  append(RenderTapeEventType::ResourceMutation, payloadWithTail(fixed, {}));
}

void RenderTapeBuilder::appendCommandChunk(const CommandChunkEnvelope& envelope,
                                           std::span<const std::byte> chunk) {
  append(RenderTapeEventType::CommandChunk,
         payloadWithTail(
             RenderTapeCommandChunkHeader{
                 .wireVersion = D9C_COMMAND_CHUNK_WIRE_VERSION,
                 .recordCount = envelope.recordCount,
                 .handleCount = envelope.handleCount,
                 .chunkBytes = static_cast<std::uint32_t>(chunk.size()),
             },
             chunk));
}

void RenderTapeBuilder::appendOrderedControl(
    const RenderTapeOrderedControlHeader& fixed,
    std::span<const std::byte> controlPayload) {
  append(RenderTapeEventType::OrderedControl,
         payloadWithTail(fixed, controlPayload));
}

void RenderTapeBuilder::appendPresentComplete(
    std::uint64_t presentOrdinal, std::uint64_t completionOrdinal,
    RenderTapeDigestValidity digestValidity, RenderTapeDigest expectedDigest,
    std::span<const std::byte> oracleAttachments) {
  std::uint32_t oracleCount =
      static_cast<std::uint32_t>(oracleAttachments.size() /
                                 sizeof(RenderTapeOracleAttachment));
  append(RenderTapeEventType::PresentComplete,
         payloadWithTail(
             RenderTapePresentCompleteHeader{
                 .presentOrdinal = presentOrdinal,
                 .completionOrdinal = completionOrdinal,
                 .digestValidity = static_cast<std::uint32_t>(digestValidity),
                 .oracleCount = oracleCount,
                 .oracleBytes =
                     static_cast<std::uint32_t>(oracleAttachments.size()),
                 .expectedDigest = expectedDigest,
             },
             oracleAttachments));
}

std::vector<std::byte> RenderTapeBuilder::seal() const {
  if (events_.empty()) {
    throw std::invalid_argument("render tape has no events");
  }
  if (events_[0].type != RenderTapeEventType::BootstrapState) {
    throw std::invalid_argument("render tape must begin with BootstrapState");
  }
  if (events_.back().type != RenderTapeEventType::PresentComplete) {
    throw std::invalid_argument("render tape must end with PresentComplete");
  }
  for (std::size_t i = 1u; i < events_.size() - 1u; ++i) {
    if (events_[i].type == RenderTapeEventType::BootstrapState ||
        events_[i].type == RenderTapeEventType::PresentComplete) {
      throw std::invalid_argument("BootstrapState / PresentComplete must be "
                                  "first / last and unique");
    }
  }
  if (events_.size() >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
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
    if (event.type == RenderTapeEventType::PresentComplete) {
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
      .wireVersion = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .presentCount = presentCount,
  };

  std::vector<std::byte> blob(payloadArenaOffset + arena.size());
  std::memcpy(blob.data(), &header, sizeof(header));
  if (!headers.empty()) {
    std::memcpy(blob.data() + eventTableOffset, headers.data(),
                headers.size() * sizeof(RenderTapeEventHeader));
  }
  if (!arena.empty()) {
    std::memcpy(blob.data() + payloadArenaOffset, arena.data(), arena.size());
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
  case RenderTapeValidationStatus::MissingBootstrap:
    return "missing-bootstrap";
  case RenderTapeValidationStatus::BootstrapNotFirst:
    return "bootstrap-not-first";
  case RenderTapeValidationStatus::DuplicateBootstrap:
    return "duplicate-bootstrap";
  case RenderTapeValidationStatus::InvalidBootstrap:
    return "invalid-bootstrap";
  case RenderTapeValidationStatus::BootstrapCoverageIncomplete:
    return "bootstrap-coverage-incomplete";
  case RenderTapeValidationStatus::BootstrapForbiddenRecord:
    return "bootstrap-forbidden-record";
  case RenderTapeValidationStatus::InvalidBootstrapChunk:
    return "invalid-bootstrap-chunk";
  case RenderTapeValidationStatus::DuplicateGeneration:
    return "duplicate-generation";
  case RenderTapeValidationStatus::UnknownIdentity:
    return "unknown-identity";
  case RenderTapeValidationStatus::RetainedSlotReuse:
    return "retained-slot-reuse";
  case RenderTapeValidationStatus::InvalidObjectDefine:
    return "invalid-object-define";
  case RenderTapeValidationStatus::InvalidObjectDestroy:
    return "invalid-object-destroy";
  case RenderTapeValidationStatus::InvalidMutationKind:
    return "invalid-mutation-kind";
  case RenderTapeValidationStatus::InvalidMutationRange:
    return "invalid-mutation-range";
  case RenderTapeValidationStatus::UnknownBlob:
    return "unknown-blob";
  case RenderTapeValidationStatus::BlobSizeMismatch:
    return "blob-size-mismatch";
  case RenderTapeValidationStatus::BlobDigestMismatch:
    return "blob-digest-mismatch";
  case RenderTapeValidationStatus::InvalidCommandChunk:
    return "invalid-command-chunk";
  case RenderTapeValidationStatus::InvalidControlKind:
    return "invalid-control-kind";
  case RenderTapeValidationStatus::InvalidControlSize:
    return "invalid-control-size";
  case RenderTapeValidationStatus::NonMonotoneCompletion:
    return "non-monotone-completion";
  case RenderTapeValidationStatus::ResetNotTerminal:
    return "reset-not-terminal";
  case RenderTapeValidationStatus::InvalidPresentComplete:
    return "invalid-present-complete";
  case RenderTapeValidationStatus::PresentNotLast:
    return "present-not-last";
  case RenderTapeValidationStatus::IncompleteFrame:
    return "incomplete-frame";
  case RenderTapeValidationStatus::ScratchAllocationFailed:
    return "scratch-allocation-failed";
  }
  return "unknown";
}

} // namespace dxmt9::d3d9
