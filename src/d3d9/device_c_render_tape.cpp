#include "device_c_render_tape.hpp"
#include "device_c_render_tape_descriptors.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace dxmt9::d3d9 {

namespace {

constexpr std::uint32_t kNoIndex = 0xffffffffu;

bool checkedMul(std::uint64_t a, std::uint64_t b,
                std::uint64_t& out) noexcept;
bool checkedAdd(std::uint64_t a, std::uint64_t b,
                std::uint64_t& out) noexcept;

bool validDescriptorContentDisposition(
    const RenderTapeObjectDefineHeader& fixed,
    std::span<const std::byte> descriptor) noexcept {
  if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE &&
      descriptor.size() >= sizeof(RenderTapeTextureDescriptorV2)) {
    RenderTapeTextureDescriptorV2 texture{};
    std::memcpy(&texture, descriptor.data(), sizeof(texture));
    if (texture.schemaVersion == kRenderTapeTextureDescriptorVersion2) {
      const auto dimension =
          static_cast<RenderTapeTextureDimension>(texture.dimension);
      const auto disposition = static_cast<RenderTapeInitialContentDisposition>(
          texture.initialContentDisposition);
      std::uint64_t expectedSubresources = texture.mipLevelCount;
      std::uint64_t subresourceBytes = 0u;
      std::uint64_t descriptorBytes = 0u;
      if (dimension == RenderTapeTextureDimension::Cube) {
        if (!checkedMul(texture.mipLevelCount, 6u, expectedSubresources))
          return false;
      } else if (dimension != RenderTapeTextureDimension::Texture2D &&
                 dimension != RenderTapeTextureDimension::Volume) {
        return false;
      }
      if (texture.mipLevelCount == 0u ||
          texture.subresourceCount != expectedSubresources ||
          texture.reserved0 != 0u ||
          !checkedMul(texture.subresourceCount, sizeof(D9CSurfaceDesc),
                      subresourceBytes) ||
          !checkedAdd(sizeof(texture), subresourceBytes, descriptorBytes) ||
          descriptorBytes != descriptor.size()) {
        return false;
      }
      if (disposition == RenderTapeInitialContentDisposition::CompleteSeed) {
        return fixed.expectedContentBytes != 0u &&
               fixed.expectedContentCount == texture.subresourceCount;
      }
      return disposition == RenderTapeInitialContentDisposition::Unavailable &&
             fixed.expectedContentBytes == 0u &&
             fixed.expectedContentCount == 0u;
    }
  }
  if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE &&
      descriptor.size() == sizeof(RenderTapeSurfaceDescriptorV2)) {
    RenderTapeSurfaceDescriptorV2 surface{};
    std::memcpy(&surface, descriptor.data(), sizeof(surface));
    if (surface.schemaVersion == kRenderTapeSurfaceDescriptorVersion2) {
      const auto storage = static_cast<RenderTapeSurfaceStorage>(surface.storage);
      const auto disposition = static_cast<RenderTapeInitialContentDisposition>(
          surface.initialContentDisposition);
      if (storage == RenderTapeSurfaceStorage::TextureSubresource) {
        return disposition == RenderTapeInitialContentDisposition::Unavailable &&
               surface.parentTexture.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE &&
               surface.parentTexture.generation != 0u &&
               surface.parentTexture.objectId != 0u &&
               fixed.expectedContentBytes == 0u &&
               fixed.expectedContentCount == 0u;
      }
      if (storage == RenderTapeSurfaceStorage::SwapchainBackbuffer) {
        return disposition ==
                   RenderTapeInitialContentDisposition::ProducedPresentOutput &&
               fixed.expectedContentBytes == 0u &&
               fixed.expectedContentCount == 0u;
      }
      if (storage == RenderTapeSurfaceStorage::Standalone) {
        return disposition == RenderTapeInitialContentDisposition::CompleteSeed
                   ? fixed.expectedContentBytes != 0u &&
                         fixed.expectedContentCount == 1u
                   : disposition ==
                             RenderTapeInitialContentDisposition::Unavailable &&
                         fixed.expectedContentBytes == 0u &&
                         fixed.expectedContentCount == 0u;
      }
      return false;
    }
  }
  return true;
}

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

auto findLiveSlot(std::vector<RenderTapeValidationScratch::LiveSlot>& live,
                  const D9CWireObjectIdentity& identity) noexcept {
  return std::find_if(live.begin(), live.end(), [&](const auto& entry) {
    return !entry.retired && sameIdentity(entry.identity, identity);
  });
}

const RenderTapeValidationScratch::ObjectDefinition* findDefinition(
    const std::vector<RenderTapeValidationScratch::ObjectDefinition>& definitions,
    const D9CWireObjectIdentity& identity) noexcept {
  const auto found = std::find_if(
      definitions.begin(), definitions.end(), [&](const auto& definition) {
        return sameIdentity(definition.identity, identity);
      });
  return found == definitions.end() ? nullptr : &*found;
}

auto findSeedContentExpectation(
    std::vector<RenderTapeValidationScratch::SeedContentExpectation>&
        expectations,
    const D9CWireObjectIdentity& identity) noexcept {
  return std::find_if(expectations.begin(), expectations.end(),
                      [&](const auto& expectation) {
                        return sameIdentity(expectation.identity, identity);
                      });
}

bool seedContentComplete(
    const std::vector<RenderTapeValidationScratch::SeedContentExpectation>&
        expectations) noexcept {
  return std::all_of(expectations.begin(), expectations.end(),
                     [](const auto& expectation) {
                       return expectation.recordedBytes ==
                                  expectation.expectedBytes &&
                              expectation.recordedCount ==
                                  expectation.expectedCount;
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

bool sameDigest(const RenderTapeDigest& a, const RenderTapeDigest& b) noexcept {
  return std::memcmp(a.data(), b.data(), kRenderTapeDigestSize) == 0;
}

bool containsIdentity(const std::vector<D9CWireObjectIdentity>& identities,
                      const D9CWireObjectIdentity& identity) noexcept {
  return std::any_of(identities.begin(), identities.end(),
                     [&](const auto& value) {
                       return sameIdentity(value, identity);
                     });
}

void addIdentity(std::vector<D9CWireObjectIdentity>& identities,
                 const D9CWireObjectIdentity& identity) {
  if (containsIdentity(identities, identity)) {
    return;
  }
  identities.push_back(identity);
}

bool addDigest(std::vector<RenderTapeDigest>& digests,
               const RenderTapeDigest& digest) {
  if (std::any_of(digests.begin(), digests.end(),
                  [&](const auto& value) { return sameDigest(value, digest); })) {
    return true;
  }
  digests.push_back(digest);
  return true;
}

struct ReductionCommandInfo {
  std::uint32_t eventIndex = kNoIndex;
  std::uint32_t presentCount = 0u;
  std::vector<D9CWireObjectIdentity> handles;
};

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
      (header.profile != kRenderTapeProfileFrame &&
       header.profile != kRenderTapeProfileSequence) ||
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
  scratch.objectDefinitions.clear();
  scratch.seedContentExpectations.clear();
  scratch.seedSubresources.clear();
  try {
    scratch.liveObjects.reserve(header.eventCount);
    scratch.objectDefinitions.reserve(header.eventCount);
    scratch.seedContentExpectations.reserve(header.eventCount);
    scratch.seedSubresources.reserve(header.eventCount);
  } catch (...) {
    return failure(RenderTapeValidationStatus::ScratchAllocationFailed);
  }

  // First build a value-owned definition index. BootstrapState is deliberately
  // event 1, so its FULL_SNAPSHOT overlay may refer to objects whose
  // ObjectDefine records are journaled immediately afterward. Parsing and
  // validating every definition, including immutable blob references, before
  // the ordered semantic pass makes that deferred shape safe for replay.
  for (std::uint32_t i = 0u; i < header.eventCount; ++i) {
    if (events[i].type !=
        static_cast<std::uint32_t>(RenderTapeEventType::ObjectDefine)) {
      continue;
    }
    const auto event = candidate.event(i);
    RenderTapeObjectDefineHeader fixed{};
    if (!load(event.payload, 0u, fixed) || !validIdentity(fixed.identity) ||
        fixed.descriptorKind != static_cast<std::uint32_t>(
            renderTapeDescriptorKindForObject(fixed.identity.kind)) ||
        fixed.descriptorBytes == 0u ||
        fixed.reserved0 != 0u || fixed.reserved1 != 0u ||
        ((fixed.expectedContentBytes == 0u) !=
         (fixed.expectedContentCount == 0u)) ||
        (fixed.expectedContentBytes != 0u &&
         !mutationCapableIdentity(fixed.identity)) ||
        event.payload.size() != sizeof(fixed) + fixed.descriptorBytes) {
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
    if (!validDescriptorContentDisposition(
            fixed, event.payload.subspan(sizeof(fixed)))) {
      return failure(RenderTapeValidationStatus::InvalidObjectDefine, i);
    }
    if (findDefinition(scratch.objectDefinitions, fixed.identity) != nullptr) {
      return failure(RenderTapeValidationStatus::DuplicateGeneration, i);
    }
    try {
      scratch.objectDefinitions.push_back(
          RenderTapeValidationScratch::ObjectDefinition{
              .identity = fixed.identity,
              .descriptorKind = fixed.descriptorKind,
              .eventIndex = i,
          });
      if (fixed.expectedContentBytes != 0u) {
        scratch.seedContentExpectations.push_back(
            RenderTapeValidationScratch::SeedContentExpectation{
                .identity = fixed.identity,
                .expectedBytes = fixed.expectedContentBytes,
                .expectedCount = fixed.expectedContentCount,
            });
      }
    } catch (...) {
      return failure(RenderTapeValidationStatus::ScratchAllocationFailed, i);
    }
  }

  bool sawBootstrap = false;
  const bool sequenceProfile = header.profile == kRenderTapeProfileSequence;
  std::uint32_t presentCompleteCount = 0u;
  bool sawChunkPresent = false;
  bool sawBetweenPresentMutation = false;
  bool secondIntervalStarted = false;
  bool sawReset = false;
  std::uint64_t presentCommandOrdinal = 0u;
  std::uint64_t previousCompletion = 0u;
  std::uint64_t expectedPayloadEnd = 0u;
  bool seedPhaseOpen = true;

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

    const auto eventType = static_cast<RenderTapeEventType>(eventHeader.type);
    const bool seedPrefixEvent =
        eventType == RenderTapeEventType::BootstrapState ||
        eventType == RenderTapeEventType::ObjectDefine ||
        eventType == RenderTapeEventType::ResourceMutation;
    if (seedPhaseOpen && !seedPrefixEvent) {
      seedPhaseOpen = false;
      if (!seedContentComplete(scratch.seedContentExpectations)) {
        return failure(RenderTapeValidationStatus::IncompleteFrame, i);
      }
    }

    switch (eventType) {
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
        // The canonical validator proves handle-table shape and references;
        // this tape-level pass proves that every referenced identity has an
        // exact, usable ObjectDefine somewhere in the complete journal.
        for (const auto& handle : chunk.handles) {
          const D9CWireObjectIdentity identity{
              .kind = handle.kind,
              .generation = handle.generation,
              .objectId = handle.objectId,
          };
          if (!validIdentity(identity) ||
              findDefinition(scratch.objectDefinitions, identity) == nullptr) {
            return failure(RenderTapeValidationStatus::UnknownIdentity, i);
          }
        }
      }
      if (walk != overlays.size()) {
        return failure(RenderTapeValidationStatus::InvalidBootstrapChunk, i);
      }
      break;
    }
    case RenderTapeEventType::ObjectDefine: {
      if (!sawBootstrap || presentCompleteCount != 0u) {
        return failure(RenderTapeValidationStatus::IncompleteFrame, i);
      }
      RenderTapeObjectDefineHeader fixed{};
      if (!load(event.payload, 0u, fixed) || !validIdentity(fixed.identity) ||
          fixed.descriptorKind != static_cast<std::uint32_t>(
              renderTapeDescriptorKindForObject(fixed.identity.kind)) ||
          fixed.descriptorBytes == 0u ||
          fixed.reserved0 != 0u || fixed.reserved1 != 0u ||
          ((fixed.expectedContentBytes == 0u) !=
           (fixed.expectedContentCount == 0u)) ||
          (fixed.expectedContentBytes != 0u &&
           !mutationCapableIdentity(fixed.identity))) {
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
      const auto descriptor = event.payload.subspan(sizeof(fixed));
      if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE &&
          descriptor.size() == sizeof(RenderTapeSurfaceDescriptorV2)) {
        RenderTapeSurfaceDescriptorV2 surface{};
        std::memcpy(&surface, descriptor.data(), sizeof(surface));
        if (surface.schemaVersion == kRenderTapeSurfaceDescriptorVersion2 &&
            surface.storage == static_cast<std::uint32_t>(
                RenderTapeSurfaceStorage::TextureSubresource)) {
          const auto* parent = findDefinition(
              scratch.objectDefinitions, surface.parentTexture);
          if (!parent || parent->eventIndex >= i ||
              findLiveSlot(scratch.liveObjects, surface.parentTexture) ==
                  scratch.liveObjects.end()) {
            return failure(RenderTapeValidationStatus::InvalidObjectDefine, i);
          }
          const auto parentEvent = candidate.event(parent->eventIndex);
          RenderTapeObjectDefineHeader parentFixed{};
          if (!load(parentEvent.payload, 0u, parentFixed)) {
            return failure(RenderTapeValidationStatus::InvalidObjectDefine, i);
          }
          const auto parentDescriptor =
              parentEvent.payload.subspan(sizeof(parentFixed));
          D9CSurfaceDesc parentSurface{};
          bool parentSurfaceLoaded = false;
          RenderTapeTextureDescriptorV2 parentTexture{};
          const bool parentIsVersioned =
              parentDescriptor.size() >= sizeof(parentTexture) &&
              load(parentDescriptor, 0u, parentTexture) &&
              parentTexture.schemaVersion ==
                  kRenderTapeTextureDescriptorVersion2;
          if (parentIsVersioned &&
              surface.subresource < parentTexture.subresourceCount) {
            const auto parentSurfaceOffset =
                sizeof(parentTexture) +
                static_cast<std::size_t>(surface.subresource) *
                    sizeof(parentSurface);
            parentSurfaceLoaded =
                load(parentDescriptor, parentSurfaceOffset, parentSurface);
          } else if (!parentIsVersioned) {
            // Legacy 2D texture descriptors remain valid and are still
            // sufficient for a level alias. Cube descriptors are always
            // versioned because their flat face*mip identity cannot be
            // represented by the legacy levelCount shape.
            parentSurfaceLoaded = renderTapeTextureSubresourceDescriptor(
                parentDescriptor, surface.subresource, parentSurface);
          }
          if (!parentSurfaceLoaded ||
              !renderTapeSurfaceDescriptorsEqual(surface.surface,
                                                  parentSurface)) {
            return failure(RenderTapeValidationStatus::InvalidObjectDefine, i);
          }
        }
      }
      // Exact definitions are globally unique (checked in the value-owned
      // prepass). Ordinary slots use kind/objectId, while texture-derived
      // surfaces use their generation-qualified parent and subresource.
      // Alias generations are numeric only within one wrapper objectId;
      // cross-object replacements are ordered by their journal events.
      const auto logicalSlot =
          renderTapeLogicalObjectSlot(fixed.identity, descriptor);
      if (logicalSlot.malformedSurfaceDescriptor) {
        return failure(RenderTapeValidationStatus::InvalidObjectDefine, i);
      }
      bool sameWireSlotSeen = false;
      std::uint32_t latestWireGeneration = 0u;
      for (const auto& priorSlot : scratch.liveObjects) {
        if (renderTapeSameWireObject(priorSlot.identity, fixed.identity)) {
          sameWireSlotSeen = true;
          latestWireGeneration = std::max(
              latestWireGeneration, priorSlot.identity.generation);
        }
        const auto slotRelation =
            renderTapeLogicalSlotRelation(priorSlot.logicalSlot, logicalSlot);
        if (slotRelation == RenderTapeLogicalSlotRelation::Different)
          continue;
        if (slotRelation ==
                RenderTapeLogicalSlotRelation::AliasDescriptorMismatch ||
            priorSlot.descriptorKind != fixed.descriptorKind) {
          return failure(RenderTapeValidationStatus::DescriptorMismatch, i);
        }
        if (!priorSlot.retired) {
          return failure(RenderTapeValidationStatus::DuplicateGeneration, i);
        }
      }
      if (sameWireSlotSeen &&
          fixed.identity.generation <= latestWireGeneration) {
        return failure(RenderTapeValidationStatus::RetainedSlotReuse, i);
      }
      scratch.liveObjects.push_back(RenderTapeValidationScratch::LiveSlot{
          .identity = fixed.identity,
          .logicalSlot = logicalSlot,
          .descriptorKind = fixed.descriptorKind,
          .lastUseOrdinal = eventHeader.ordinal,
          .retired = false,
      });
      break;
    }
    case RenderTapeEventType::ObjectDestroy: {
      if (!sawBootstrap || presentCompleteCount != 0u) {
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
      if (!sawBootstrap ||
          (!sequenceProfile && presentCompleteCount != 0u) ||
          presentCompleteCount >= 2u ||
          (presentCompleteCount == 1u &&
           (sawBetweenPresentMutation || secondIntervalStarted))) {
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
      if (sequenceProfile && presentCompleteCount == 1u) {
        sawBetweenPresentMutation = true;
      }
      if (seedPhaseOpen) {
        const auto expectation = findSeedContentExpectation(
            scratch.seedContentExpectations, fixed.identity);
        const bool advancesIncompleteSeed =
            expectation != scratch.seedContentExpectations.end() &&
            (expectation->recordedBytes != expectation->expectedBytes ||
             expectation->recordedCount != expectation->expectedCount);
        if (advancesIncompleteSeed) {
          if (fixed.kind !=
                  static_cast<std::uint32_t>(RenderTapeMutationKind::Upload) &&
              fixed.kind != static_cast<std::uint32_t>(
                                 RenderTapeMutationKind::CpuUnlock)) {
            return failure(RenderTapeValidationStatus::InvalidMutationKind, i);
          }
          const bool duplicate = std::any_of(
              scratch.seedSubresources.begin(),
              scratch.seedSubresources.end(), [&](const auto& seed) {
                return sameIdentity(seed.identity, fixed.identity) &&
                       seed.subresource == fixed.subresource;
              });
          std::uint64_t recordedBytes = 0u;
          if (fixed.byteOffset != 0u || duplicate ||
              expectation->recordedCount ==
                  std::numeric_limits<std::uint32_t>::max() ||
              !checkedAdd(expectation->recordedBytes, fixed.byteSize,
                          recordedBytes) ||
              recordedBytes > expectation->expectedBytes ||
              expectation->recordedCount + 1u > expectation->expectedCount) {
            return failure(RenderTapeValidationStatus::InvalidMutationRange, i);
          }
          try {
            scratch.seedSubresources.push_back(
                RenderTapeValidationScratch::SeedSubresource{
                    .identity = fixed.identity,
                    .subresource = fixed.subresource,
                });
          } catch (...) {
            return failure(RenderTapeValidationStatus::ScratchAllocationFailed,
                           i);
          }
          expectation->recordedBytes = recordedBytes;
          ++expectation->recordedCount;
          if (seedContentComplete(scratch.seedContentExpectations)) {
            seedPhaseOpen = false;
          }
        } else {
          // The first mutation that does not advance an incomplete declared
          // expectation is ordinary interval traffic. Close the seed prefix
          // before accepting it; a still-incomplete different identity fails
          // here and cannot be repaired by a later mutation.
          seedPhaseOpen = false;
          if (!seedContentComplete(scratch.seedContentExpectations)) {
            return failure(RenderTapeValidationStatus::IncompleteFrame, i);
          }
        }
      }
      break;
    }
    case RenderTapeEventType::CommandChunk: {
      if (!sawBootstrap ||
          (!sequenceProfile && presentCompleteCount != 0u) ||
          presentCompleteCount >= 2u ||
          (presentCompleteCount == 1u && !sawBetweenPresentMutation)) {
        return failure(RenderTapeValidationStatus::IncompleteFrame, i);
      }
      if (sequenceProfile && presentCompleteCount == 1u) {
        secondIntervalStarted = true;
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
      if (!sawBootstrap || presentCompleteCount != 0u) {
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
      if ((!sequenceProfile && presentCompleteCount != 0u) ||
          presentCompleteCount >= 2u) {
        return failure(RenderTapeValidationStatus::InvalidPresentComplete, i);
      }
      const bool finalCompletion = sequenceProfile
          ? presentCompleteCount == 1u
          : true;
      if (finalCompletion != (i == header.eventCount - 1u)) {
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
      if (sequenceProfile && presentCompleteCount == 1u &&
          (!sawBetweenPresentMutation || !secondIntervalStarted)) {
        return failure(RenderTapeValidationStatus::InvalidPresentComplete, i);
      }
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
            attachment.identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE ||
            attachment.descriptorKind !=
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface) ||
            attachment.reserved0 != 0u) {
          return failure(RenderTapeValidationStatus::InvalidPresentComplete, i);
        }
        const auto live = findLiveSlot(scratch.liveObjects, attachment.identity);
        if (live == scratch.liveObjects.end() ||
            live->descriptorKind != attachment.descriptorKind) {
          return failure(RenderTapeValidationStatus::InvalidPresentComplete, i);
        }
      }
      ++presentCompleteCount;
      previousCompletion = fixed.completionOrdinal;
      sawChunkPresent = false;
      presentCommandOrdinal = 0u;
      break;
    }
    }
  }

  if (!sawBootstrap) {
    return failure(RenderTapeValidationStatus::MissingBootstrap);
  }
  const auto expectedPresentCount = sequenceProfile ? 2u : 1u;
  if (presentCompleteCount != expectedPresentCount) {
    return failure(RenderTapeValidationStatus::IncompleteFrame);
  }
  if (!seedContentComplete(scratch.seedContentExpectations)) {
    return failure(RenderTapeValidationStatus::IncompleteFrame);
  }
  if (header.presentCount != expectedPresentCount) {
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
                 sink.bootstrap(fixed, tailAfter(event.payload, sizeof(fixed)),
                                RenderTapeBootstrapReplayMode::
                                    JournalOnlyDeferredProvider);
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

void RenderTapeBuilder::appendRawEvent(RenderTapeEventType type,
                                       std::span<const std::byte> payload) {
  append(type, std::vector<std::byte>(payload.begin(), payload.end()));
}

void RenderTapeBuilder::append(RenderTapeEventType type,
                               std::vector<std::byte> payload) {
  events_.push_back(PendingEvent{
      .type = type,
      .payload = std::move(payload),
  });
}

RenderTapeReductionResult reduceRenderTape(
    std::span<const std::byte> source,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    std::span<const std::uint32_t> selectedCommandEventIndices) noexcept {
  RenderTapeReductionResult result{};
  ImportedRenderTapeView tape;
  result.sourceValidation = validateRenderTape(source, verifiedCatalogue, &tape);
  result.validation = result.sourceValidation;
  if (!result.sourceValidation.valid()) {
    result.status = RenderTapeReductionStatus::InvalidSource;
    return result;
  }
  if (tape.header.profile != kRenderTapeProfileFrame) {
    result.status = RenderTapeReductionStatus::UnsupportedEvent;
    return result;
  }

  try {
    std::vector<ReductionCommandInfo> commands;
    std::vector<D9CWireObjectIdentity> closure;
    std::vector<D9CWireObjectIdentity> definitions;
    struct SeedNeed {
      D9CWireObjectIdentity identity{};
      std::uint64_t expectedBytes = 0u;
      std::uint32_t expectedCount = 0u;
      std::uint64_t recordedBytes = 0u;
      std::uint32_t recordedCount = 0u;
    };
    std::vector<SeedNeed> seedNeeds;
    std::vector<std::uint8_t> selected(tape.events.size(), 0u);
    std::uint32_t presentCommandEvent = kNoIndex;
    std::uint32_t presentCommandCount = 0u;
    std::uint32_t firstCommandEvent = kNoIndex;

    // Ordered controls and destruction cannot be reduced safely: their
    // effects are not represented by the selected command closure.
    for (std::uint32_t i = 0u; i < tape.events.size(); ++i) {
      const auto type = static_cast<RenderTapeEventType>(tape.events[i].type);
      if (type == RenderTapeEventType::OrderedControl ||
          type == RenderTapeEventType::ObjectDestroy) {
        result.status = RenderTapeReductionStatus::UnsupportedEvent;
        result.validation = result.sourceValidation;
        result.validation.failedEventIndex = i;
        return result;
      }
    }

    // Build the command index and identity references from the immutable,
    // already validated source view.
    for (std::uint32_t i = 0u; i < tape.events.size(); ++i) {
      if (static_cast<RenderTapeEventType>(tape.events[i].type) !=
          RenderTapeEventType::CommandChunk) {
        continue;
      }
      const auto event = tape.event(i);
      RenderTapeCommandChunkHeader fixed{};
      if (!load(event.payload, 0u, fixed)) {
        result.status = RenderTapeReductionStatus::ClosureFailure;
        result.validation.failedEventIndex = i;
        return result;
      }
      ImportedChunkView chunk;
      const auto chunkResult = validateCommandChunk(
          tailAfter(event.payload, sizeof(fixed)),
          CommandChunkEnvelope{
              .version = fixed.wireVersion,
              .recordCount = fixed.recordCount,
              .handleCount = fixed.handleCount,
          },
          &chunk);
      if (!chunkResult.valid()) {
        result.status = RenderTapeReductionStatus::ClosureFailure;
        result.validation.failedEventIndex = i;
        result.validation.chunkStatus = chunkResult.status;
        return result;
      }
      ReductionCommandInfo info{.eventIndex = i};
      for (const auto& handle : chunk.handles) {
        info.handles.push_back(D9CWireObjectIdentity{
            .kind = handle.kind,
            .generation = handle.generation,
            .objectId = handle.objectId,
        });
      }
      for (const auto& record : chunk.records) {
        if (record.type == D9C_COMMAND_RECORD_PRESENT) {
          ++info.presentCount;
        }
      }
      if (info.presentCount != 0u) {
        ++presentCommandCount;
        presentCommandEvent = i;
      }
      if (firstCommandEvent == kNoIndex) {
        firstCommandEvent = i;
      }
      commands.push_back(std::move(info));
    }

    if (commands.empty() || presentCommandCount != 1u) {
      result.status = RenderTapeReductionStatus::ClosureFailure;
      return result;
    }

    // Validate and normalize selection without sorting it: source order is
    // the canonical output order, while duplicate detection is independent
    // of caller ordering.
    for (const auto index : selectedCommandEventIndices) {
      if (index >= tape.events.size() || selected[index] != 0u ||
          static_cast<RenderTapeEventType>(tape.events[index].type) !=
              RenderTapeEventType::CommandChunk) {
        result.status = RenderTapeReductionStatus::InvalidSelection;
        result.validation.failedEventIndex = index;
        return result;
      }
      selected[index] = 1u;
    }
    std::uint32_t selectedPresentCount = 0u;
    for (const auto& command : commands) {
      if (selected[command.eventIndex] != 0u) {
        selectedPresentCount += command.presentCount;
      }
    }
    if (selectedPresentCount != 1u || selected[presentCommandEvent] == 0u) {
      result.status = RenderTapeReductionStatus::MissingPresentSelection;
      result.validation.failedEventIndex = presentCommandEvent;
      return result;
    }

    // Bootstrap overlays are always part of the closure.
    const auto bootstrap = tape.event(0u);
    RenderTapeBootstrapHeader bootstrapFixed{};
    if (!load(bootstrap.payload, 0u, bootstrapFixed)) {
      result.status = RenderTapeReductionStatus::ClosureFailure;
      result.validation.failedEventIndex = 0u;
      return result;
    }
    const auto overlays = tailAfter(bootstrap.payload, sizeof(bootstrapFixed));
    std::uint64_t overlayOffset = 0u;
    for (std::uint32_t o = 0u; o < bootstrapFixed.overlayCount; ++o) {
      std::uint64_t total = 0u;
      if (!chunkTotalBytes(overlays.subspan(overlayOffset), total)) {
        result.status = RenderTapeReductionStatus::ClosureFailure;
        result.validation.failedEventIndex = 0u;
        return result;
      }
      D9CCommandChunkWireHeader overlayHeader{};
      if (!load(overlays.subspan(overlayOffset), 0u, overlayHeader)) {
        result.status = RenderTapeReductionStatus::ClosureFailure;
        result.validation.failedEventIndex = 0u;
        return result;
      }
      ImportedChunkView chunk;
      const auto chunkResult = validateCommandChunk(
          overlays.subspan(overlayOffset, total),
          CommandChunkEnvelope{
              .version = overlayHeader.version,
              .recordCount = overlayHeader.recordCount,
              .handleCount = overlayHeader.handleCount,
          },
          &chunk);
      if (!chunkResult.valid()) {
        result.status = RenderTapeReductionStatus::ClosureFailure;
        result.validation.failedEventIndex = 0u;
        result.validation.chunkStatus = chunkResult.status;
        return result;
      }
      for (const auto& handle : chunk.handles) {
        addIdentity(closure, D9CWireObjectIdentity{
                                .kind = handle.kind,
                                .generation = handle.generation,
                                .objectId = handle.objectId,
                            });
      }
      overlayOffset += total;
    }

    for (const auto& command : commands) {
      if (selected[command.eventIndex] == 0u) {
        continue;
      }
      for (const auto& identity : command.handles) {
        addIdentity(closure, identity);
      }
    }

    // PresentComplete oracle attachments are part of the output's identity
    // closure even though they are not referenced by a command handle.
    const auto complete = tape.event(tape.events.size() - 1u);
    RenderTapePresentCompleteHeader completeFixed{};
    if (!load(complete.payload, 0u, completeFixed)) {
      result.status = RenderTapeReductionStatus::ClosureFailure;
      result.validation.failedEventIndex =
          static_cast<std::uint32_t>(tape.events.size() - 1u);
      return result;
    }
    for (std::uint32_t a = 0u; a < completeFixed.oracleCount; ++a) {
      RenderTapeOracleAttachment attachment{};
      if (!load(complete.payload,
                sizeof(completeFixed) +
                    a * sizeof(RenderTapeOracleAttachment),
                attachment)) {
        result.status = RenderTapeReductionStatus::ClosureFailure;
        result.validation.failedEventIndex =
            static_cast<std::uint32_t>(tape.events.size() - 1u);
        return result;
      }
      addIdentity(closure, attachment.identity);
    }

    // Definitions are retained exactly by generation-qualified identity.
    for (std::uint32_t i = 0u; i < tape.events.size(); ++i) {
      if (static_cast<RenderTapeEventType>(tape.events[i].type) !=
          RenderTapeEventType::ObjectDefine) {
        continue;
      }
      RenderTapeObjectDefineHeader fixed{};
      if (!load(tape.event(i).payload, 0u, fixed)) {
        result.status = RenderTapeReductionStatus::ClosureFailure;
        result.validation.failedEventIndex = i;
        return result;
      }
      if (containsIdentity(closure, fixed.identity)) {
        definitions.push_back(fixed.identity);
        seedNeeds.push_back(SeedNeed{
            .identity = fixed.identity,
            .expectedBytes = fixed.expectedContentBytes,
            .expectedCount = fixed.expectedContentCount,
        });
      }
    }

    // Construct the source-index retention set in source order. Resource
    // mutations after the first command are ordinary interval traffic; only
    // the declared seed prefix is safe to carry into a reduced frame.
    std::vector<std::uint8_t> retain(tape.events.size(), 0u);
    retain[0u] = 1u;
    for (std::uint32_t i = 1u; i < tape.events.size() - 1u; ++i) {
      const auto type = static_cast<RenderTapeEventType>(tape.events[i].type);
      if (type == RenderTapeEventType::ObjectDefine) {
        RenderTapeObjectDefineHeader fixed{};
        load(tape.event(i).payload, 0u, fixed);
        retain[i] = containsIdentity(closure, fixed.identity) ? 1u : 0u;
      } else if (type == RenderTapeEventType::ResourceMutation &&
                 (firstCommandEvent != kNoIndex && i >= firstCommandEvent)) {
        result.status = RenderTapeReductionStatus::UnsupportedEvent;
        result.validation.failedEventIndex = i;
        return result;
      } else if (type == RenderTapeEventType::ResourceMutation) {
        RenderTapeResourceMutationHeader fixed{};
        load(tape.event(i).payload, 0u, fixed);
        const auto need = std::find_if(
            seedNeeds.begin(), seedNeeds.end(), [&](const auto& value) {
              return sameIdentity(value.identity, fixed.identity);
            });
        if (need == seedNeeds.end()) {
          retain[i] = 0u;
          continue;
        }
        // Only the definition-declared initial-content prefix is reducible.
        // Any later live mutation, including one before the first command,
        // has interval semantics that this bounded reducer cannot project.
        if (need->expectedCount == 0u ||
            need->recordedCount >= need->expectedCount ||
            need->recordedBytes >= need->expectedBytes) {
          result.status = RenderTapeReductionStatus::UnsupportedEvent;
          result.validation.failedEventIndex = i;
          return result;
        }
        need->recordedBytes += fixed.byteSize;
        ++need->recordedCount;
        retain[i] = 1u;
      } else if (type == RenderTapeEventType::CommandChunk) {
        retain[i] = selected[i];
      }
    }
    retain[tape.events.size() - 1u] = 1u;
    if (std::any_of(seedNeeds.begin(), seedNeeds.end(), [](const auto& need) {
          return need.recordedBytes != need.expectedBytes ||
                 need.recordedCount != need.expectedCount;
        })) {
      result.status = RenderTapeReductionStatus::ClosureFailure;
      return result;
    }

    std::vector<std::uint32_t> retained;
    retained.reserve(tape.events.size());
    for (std::uint32_t i = 0u; i < retain.size(); ++i) {
      if (retain[i] != 0u) {
        retained.push_back(i);
      }
    }
    if (retained.size() < 3u) {
      result.status = RenderTapeReductionStatus::ClosureFailure;
      return result;
    }

    // Collect blob references in first-use/source order, and retain only the
    // exact catalogue entries needed to validate the reduced tape.
    std::vector<RenderTapeDigest> digests;
    struct BlobNeed {
      RenderTapeDigest digest{};
      std::uint64_t size = 0u;
    };
    std::vector<BlobNeed> needs;
    for (const auto i : retained) {
      const auto event = tape.event(i);
      if (static_cast<RenderTapeEventType>(event.header.type) ==
          RenderTapeEventType::ObjectDefine) {
        RenderTapeObjectDefineHeader fixed{};
        load(event.payload, 0u, fixed);
        if (fixed.payloadValidity ==
            static_cast<std::uint32_t>(RenderTapeDigestValidity::Sha256)) {
          digests.push_back(fixed.immutablePayloadDigest);
          needs.push_back(BlobNeed{fixed.immutablePayloadDigest,
                                   fixed.immutablePayloadBytes});
        }
      } else if (static_cast<RenderTapeEventType>(event.header.type) ==
                 RenderTapeEventType::ResourceMutation) {
        RenderTapeResourceMutationHeader fixed{};
        load(event.payload, 0u, fixed);
        addDigest(digests, fixed.digest);
        needs.push_back(BlobNeed{fixed.digest, fixed.byteSize});
      }
    }
    // Immutable definitions can legitimately share a digest; normalize that
    // list after preserving first-use order above.
    std::vector<RenderTapeDigest> uniqueDigests;
    for (const auto& digest : digests) addDigest(uniqueDigests, digest);

    RenderTapeBuilder builder;
    std::uint64_t outputOrdinal = 0u;
    std::uint64_t reducedPresentOrdinal = 0u;
    std::vector<std::byte> patchedComplete;
    for (const auto i : retained) {
      const auto event = tape.event(i);
      ++outputOrdinal;
      if (i == presentCommandEvent) {
        reducedPresentOrdinal = outputOrdinal;
      }
      if (i == tape.events.size() - 1u) {
        patchedComplete.assign(event.payload.begin(), event.payload.end());
        RenderTapePresentCompleteHeader fixed{};
        if (!load(patchedComplete, 0u, fixed)) {
          result.status = RenderTapeReductionStatus::ClosureFailure;
          return result;
        }
        fixed.presentOrdinal = reducedPresentOrdinal;
        std::memcpy(patchedComplete.data(), &fixed, sizeof(fixed));
        builder.appendRawEvent(RenderTapeEventType::PresentComplete,
                               patchedComplete);
      } else {
        builder.appendRawEvent(
            static_cast<RenderTapeEventType>(event.header.type), event.payload);
      }
    }
    auto reduced = builder.seal();

    RenderTapeBlobCatalogue reducedCatalogue;
    for (const auto& need : needs) {
      const auto found = std::find_if(
          verifiedCatalogue.blobs.begin(), verifiedCatalogue.blobs.end(),
          [&](const auto& blob) {
            return sameDigest(blob.digest, need.digest) &&
                   blob.size == need.size && blob.verified == 1u;
          });
      if (found == verifiedCatalogue.blobs.end()) {
        result.status = RenderTapeReductionStatus::ClosureFailure;
        return result;
      }
      const bool already = std::any_of(
          reducedCatalogue.blobs.begin(), reducedCatalogue.blobs.end(),
          [&](const auto& blob) {
            return sameDigest(blob.digest, found->digest) &&
                   blob.size == found->size;
          });
      if (!already) reducedCatalogue.blobs.push_back(*found);
    }
    ImportedRenderTapeView reducedView;
    const auto reducedValidation =
        validateRenderTape(reduced, reducedCatalogue, &reducedView);
    result.validation = reducedValidation;
    if (!reducedValidation.valid()) {
      result.status = RenderTapeReductionStatus::OutputValidationFailed;
      return result;
    }
    result.bytes = std::move(reduced);
    result.retainedSourceEventIndices = std::move(retained);
    result.referencedBlobDigests = std::move(uniqueDigests);
    result.status = RenderTapeReductionStatus::Valid;
    return result;
  } catch (...) {
    result.status = RenderTapeReductionStatus::AllocationFailed;
    result.bytes.clear();
    result.retainedSourceEventIndices.clear();
    result.referencedBlobDigests.clear();
    return result;
  }
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
                                           RenderTapeDigest immutablePayloadDigest,
                                           std::uint64_t expectedContentBytes,
                                           std::uint32_t expectedContentCount) {
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
                 .expectedContentBytes = expectedContentBytes,
                 .expectedContentCount = expectedContentCount,
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
  if (profile_ != kRenderTapeProfileFrame &&
      profile_ != kRenderTapeProfileSequence) {
    throw std::invalid_argument("unsupported render tape profile");
  }
  std::uint32_t completionCount = 0u;
  for (std::size_t i = 1u; i < events_.size(); ++i) {
    if (events_[i].type == RenderTapeEventType::BootstrapState) {
      throw std::invalid_argument("BootstrapState must be first and unique");
    }
    if (events_[i].type == RenderTapeEventType::PresentComplete) {
      ++completionCount;
    }
  }
  const auto expectedCompletionCount =
      profile_ == kRenderTapeProfileSequence ? 2u : 1u;
  if (completionCount != expectedCompletionCount) {
    throw std::invalid_argument("render tape profile has wrong completion count");
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
      .profile = profile_,
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

const char* renderTapeProfileName(std::uint32_t profile) noexcept {
  switch (profile) {
  case kRenderTapeProfileFrame:
    return "frame-tape";
  case kRenderTapeProfileSequence:
    return "sequence-tape";
  default:
    return "unknown";
  }
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
  case RenderTapeValidationStatus::DescriptorMismatch:
    return "descriptor-mismatch";
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

const char* renderTapeReductionStatusName(
    RenderTapeReductionStatus status) noexcept {
  switch (status) {
  case RenderTapeReductionStatus::Valid:
    return "valid";
  case RenderTapeReductionStatus::InvalidSource:
    return "invalid-source";
  case RenderTapeReductionStatus::InvalidSelection:
    return "invalid-selection";
  case RenderTapeReductionStatus::UnsupportedEvent:
    return "unsupported-event";
  case RenderTapeReductionStatus::MissingPresentSelection:
    return "missing-present-selection";
  case RenderTapeReductionStatus::ClosureFailure:
    return "closure-failure";
  case RenderTapeReductionStatus::OutputValidationFailed:
    return "output-validation-failed";
  case RenderTapeReductionStatus::AllocationFailed:
    return "allocation-failed";
  }
  return "unknown";
}

} // namespace dxmt9::d3d9
