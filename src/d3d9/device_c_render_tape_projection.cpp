#include "device_c_render_tape_projection.hpp"

#include "device_c_render_tape_capture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {
namespace {

constexpr bool sameIdentity(const D9CWireObjectIdentity& left,
                            const D9CWireObjectIdentity& right) noexcept {
  return left.kind == right.kind && left.generation == right.generation &&
         left.objectId == right.objectId;
}

template <typename T>
bool load(std::span<const std::byte> bytes, std::size_t offset,
          T& value) noexcept {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
    return false;
  }
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return true;
}

bool isDraw(std::uint32_t type) noexcept {
  switch (type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
    return true;
  default:
    return false;
  }
}

bool containsIdentity(std::span<const D9CWireObjectIdentity> values,
                      const D9CWireObjectIdentity& identity) noexcept {
  return std::any_of(values.begin(), values.end(), [&](const auto& value) {
    return sameIdentity(value, identity);
  });
}

void addIdentity(std::vector<D9CWireObjectIdentity>& values,
                 const D9CWireObjectIdentity& identity) {
  if (!containsIdentity(values, identity)) {
    values.push_back(identity);
  }
}

struct DefinitionInfo {
  RenderTapeObjectDefineHeader fixed{};
  std::uint32_t eventIndex = kRenderTapeProjectionNoIndex;
  std::uint64_t eventOrdinal = 0u;
};

struct SeedState {
  D9CWireObjectIdentity identity{};
  std::uint64_t expectedBytes = 0u;
  std::uint32_t expectedCount = 0u;
  std::uint64_t recordedBytes = 0u;
  std::uint32_t recordedCount = 0u;
};

bool before(std::uint32_t eventIndex, std::uint32_t recordIndex,
            std::uint32_t selectedEvent,
            std::uint32_t firstSelectedRecord) noexcept {
  return eventIndex < selectedEvent ||
         (eventIndex == selectedEvent && recordIndex < firstSelectedRecord);
}

bool atOrAfterSelectedEnd(std::uint32_t eventIndex, std::uint32_t recordIndex,
                          std::uint32_t selectedEvent,
                          std::uint32_t selectedEndExclusive) noexcept {
  return eventIndex > selectedEvent ||
         (eventIndex == selectedEvent &&
          recordIndex >= selectedEndExclusive);
}

RenderTapeProjectionObject* findProjectedObject(
    std::vector<RenderTapeProjectionObject>& objects,
    const D9CWireObjectIdentity& identity) noexcept {
  const auto found =
      std::find_if(objects.begin(), objects.end(), [&](const auto& object) {
        return sameIdentity(object.identity, identity);
      });
  return found == objects.end() ? nullptr : &*found;
}

const DefinitionInfo* findDefinition(
    std::span<const DefinitionInfo> definitions,
    const D9CWireObjectIdentity& identity) noexcept {
  const auto found = std::find_if(
      definitions.begin(), definitions.end(), [&](const auto& definition) {
        return sameIdentity(definition.fixed.identity, identity);
      });
  return found == definitions.end() ? nullptr : &*found;
}

bool exactBlob(const RenderTapeBlobCatalogue& catalogue,
               const RenderTapeDigest& digest, std::uint64_t size) noexcept {
  return catalogue.lookup(
             std::span<const std::byte, kRenderTapeDigestSize>(digest), size) ==
         RenderTapeBlobLookup::Exact;
}

} // namespace

RenderTapeProjectionResult projectRenderTapeDrawSlice(
    std::span<const std::byte> source,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    const RenderTapeProjectionSelector& selector) noexcept {
  RenderTapeProjectionResult result{};
  result.selector = selector;
  result.sourceBytes = source.size();
  try {
    ImportedRenderTapeView tape;
    result.sourceValidation =
        validateRenderTape(source, verifiedCatalogue, &tape);
    if (!result.sourceValidation.valid()) {
      result.status = RenderTapeProjectionStatus::InvalidSource;
      result.failedEventIndex = result.sourceValidation.failedEventIndex;
      return result;
    }
    result.sourceDigest = RenderTapeCaptureSession::sha256(source);
    result.sourceEventCount = static_cast<std::uint32_t>(tape.events.size());
    if (tape.header.profile != kRenderTapeProfileFrame) {
      result.status = RenderTapeProjectionStatus::UnsupportedProfile;
      return result;
    }
    if (selector.commandEventOrdinal == 0u || selector.recordCount == 0u) {
      result.status = RenderTapeProjectionStatus::InvalidSelection;
      return result;
    }

    std::vector<DefinitionInfo> definitions;
    std::uint32_t selectedEvent = kRenderTapeProjectionNoIndex;
    for (std::uint32_t i = 0u; i < tape.events.size(); ++i) {
      const auto event = tape.event(i);
      const auto type = static_cast<RenderTapeEventType>(event.header.type);
      if (type == RenderTapeEventType::ObjectDefine) {
        RenderTapeObjectDefineHeader fixed{};
        if (!load(event.payload, 0u, fixed)) {
          result.status = RenderTapeProjectionStatus::InvalidSource;
          result.failedEventIndex = i;
          return result;
        }
        definitions.push_back(DefinitionInfo{
            .fixed = fixed,
            .eventIndex = i,
            .eventOrdinal = event.header.ordinal,
        });
      }
      if (event.header.ordinal == selector.commandEventOrdinal) {
        selectedEvent = i;
      }
      if (type == RenderTapeEventType::OrderedControl ||
          type == RenderTapeEventType::ObjectDestroy ||
          type == RenderTapeEventType::PresentComplete) {
        const auto excludedKind =
            type == RenderTapeEventType::OrderedControl
                ? RenderTapeProjectionExcludedKind::OrderedControl
                : type == RenderTapeEventType::ObjectDestroy
                      ? RenderTapeProjectionExcludedKind::ObjectDestroy
                      : RenderTapeProjectionExcludedKind::PresentComplete;
        result.excludedEvents.push_back(RenderTapeProjectionExcludedEvent{
            .kind = excludedKind,
            .sourceEventIndex = i,
            .eventOrdinal = event.header.ordinal,
            .eventType = event.header.type,
        });
      }
    }
    if (selectedEvent == kRenderTapeProjectionNoIndex ||
        static_cast<RenderTapeEventType>(tape.events[selectedEvent].type) !=
            RenderTapeEventType::CommandChunk) {
      result.status = RenderTapeProjectionStatus::InvalidSelection;
      result.failedEventIndex = selectedEvent;
      return result;
    }
    result.selectedCommandEventIndex = selectedEvent;
    result.failedEventIndex = selectedEvent;

    const auto selectedEventView = tape.event(selectedEvent);
    RenderTapeCommandChunkHeader selectedFixed{};
    if (!load(selectedEventView.payload, 0u, selectedFixed)) {
      result.status = RenderTapeProjectionStatus::InvalidSource;
      return result;
    }
    ImportedChunkView selectedChunk;
    const auto selectedChunkValidation = validateCommandChunk(
        selectedEventView.payload.subspan(sizeof(selectedFixed)),
        CommandChunkEnvelope{
            .version = selectedFixed.wireVersion,
            .recordCount = selectedFixed.recordCount,
            .handleCount = selectedFixed.handleCount,
        },
        &selectedChunk);
    const std::uint64_t selectedEnd64 =
        static_cast<std::uint64_t>(selector.firstRecordIndex) +
        selector.recordCount;
    if (!selectedChunkValidation.valid() ||
        selector.firstRecordIndex >= selectedChunk.records.size() ||
        selectedEnd64 > selectedChunk.records.size()) {
      result.status = RenderTapeProjectionStatus::InvalidSelection;
      result.sourceValidation.chunkStatus = selectedChunkValidation.status;
      result.failedRecordIndex = selector.firstRecordIndex;
      return result;
    }
    const auto selectedEnd = static_cast<std::uint32_t>(selectedEnd64);

    std::vector<D9CWireObjectIdentity> closure;
    for (std::uint32_t recordIndex = selector.firstRecordIndex;
         recordIndex < selectedEnd; ++recordIndex) {
      const auto record = selectedChunk.record(recordIndex);
      if (!isDraw(record.header.type)) {
        result.status = RenderTapeProjectionStatus::NonDrawRecord;
        result.failedRecordIndex = recordIndex;
        return result;
      }
      if (recordIndex == selector.firstRecordIndex &&
          (record.drawHeader.flags &
           D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT) == 0u) {
        result.status = RenderTapeProjectionStatus::MissingFullSnapshot;
        result.failedRecordIndex = recordIndex;
        return result;
      }
      result.selectedLocators.push_back(RenderTapeProjectionLocator{
          .eventOrdinal = selectedEventView.header.ordinal,
          .sourceEventIndex = selectedEvent,
          .recordIndex = recordIndex,
          .recordType = record.header.type,
      });
      for (const auto& handle : record.handles) {
        addIdentity(closure, D9CWireObjectIdentity{
                                 .kind = handle.kind,
                                 .generation = handle.generation,
                                 .objectId = handle.objectId,
                             });
      }
    }
    result.selectedDrawCount = selector.recordCount;

    bool foundClear = false;
    bool foundPresent = false;
    for (std::uint32_t eventIndex = 0u; eventIndex < tape.events.size();
         ++eventIndex) {
      const auto commandEvent = tape.event(eventIndex);
      if (static_cast<RenderTapeEventType>(commandEvent.header.type) !=
          RenderTapeEventType::CommandChunk) {
        continue;
      }
      RenderTapeCommandChunkHeader fixed{};
      if (!load(commandEvent.payload, 0u, fixed)) {
        result.status = RenderTapeProjectionStatus::InvalidSource;
        result.failedEventIndex = eventIndex;
        return result;
      }
      ImportedChunkView chunk;
      const auto validation = validateCommandChunk(
          commandEvent.payload.subspan(sizeof(fixed)),
          CommandChunkEnvelope{
              .version = fixed.wireVersion,
              .recordCount = fixed.recordCount,
              .handleCount = fixed.handleCount,
          },
          &chunk);
      if (!validation.valid()) {
        result.status = RenderTapeProjectionStatus::InvalidSource;
        result.failedEventIndex = eventIndex;
        result.sourceValidation.chunkStatus = validation.status;
        return result;
      }
      result.sourceRecordCount += chunk.records.size();
      for (std::uint32_t recordIndex = 0u; recordIndex < chunk.records.size();
           ++recordIndex) {
        const auto type = chunk.records[recordIndex].type;
        if (type == D9C_COMMAND_RECORD_CLEAR &&
            before(eventIndex, recordIndex, selectedEvent,
                   selector.firstRecordIndex)) {
          foundClear = true;
          result.clearLocator = RenderTapeProjectionLocator{
              .eventOrdinal = commandEvent.header.ordinal,
              .sourceEventIndex = eventIndex,
              .recordIndex = recordIndex,
              .recordType = type,
          };
        }
        if (!foundPresent && type == D9C_COMMAND_RECORD_PRESENT &&
            atOrAfterSelectedEnd(eventIndex, recordIndex, selectedEvent,
                                 selectedEnd)) {
          foundPresent = true;
          result.presentLocator = RenderTapeProjectionLocator{
              .eventOrdinal = commandEvent.header.ordinal,
              .sourceEventIndex = eventIndex,
              .recordIndex = recordIndex,
              .recordType = type,
          };
        }
      }
    }
    if (!foundClear || !foundPresent) {
      result.status = RenderTapeProjectionStatus::MissingFrameBoundary;
      return result;
    }
    result.excludedRecordCount =
        result.sourceRecordCount - result.selectedDrawCount;

    for (const auto& definition : definitions) {
      if (!containsIdentity(closure, definition.fixed.identity)) {
        continue;
      }
      std::uint32_t disposition = 0u;
      D9CWireObjectIdentity aliasParent{};
      std::uint32_t aliasSubresource = 0u;
      const auto definitionEvent = tape.event(definition.eventIndex);
      const auto descriptor = definitionEvent.payload.subspan(
          sizeof(definition.fixed));
      if (definition.fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        RenderTapeTextureDescriptorV2 texture{};
        if (load(descriptor, 0u, texture))
          disposition = texture.initialContentDisposition;
      } else if (definition.fixed.identity.kind ==
                 D9C_CHUNK_HANDLE_KIND_SURFACE) {
        RenderTapeSurfaceDescriptorV2 surface{};
        if (load(descriptor, 0u, surface)) {
          disposition = surface.initialContentDisposition;
          if (surface.storage == static_cast<std::uint32_t>(
                  RenderTapeSurfaceStorage::TextureSubresource)) {
            aliasParent = surface.parentTexture;
            aliasSubresource = surface.subresource;
          }
        }
      }
      result.objects.push_back(RenderTapeProjectionObject{
          .identity = definition.fixed.identity,
          .descriptorKind = definition.fixed.descriptorKind,
          .descriptorBytes = definition.fixed.descriptorBytes,
          .definitionEventIndex = definition.eventIndex,
          .definitionEventOrdinal = definition.eventOrdinal,
          .immutablePayloadBytes = definition.fixed.immutablePayloadBytes,
          .expectedContentBytes = definition.fixed.expectedContentBytes,
          .expectedContentCount = definition.fixed.expectedContentCount,
          .initialContentDisposition = disposition,
          .aliasParentTexture = aliasParent,
          .aliasSubresource = aliasSubresource,
      });
    }
    for (const auto& identity : closure) {
      if (!findDefinition(definitions, identity)) {
        result.status = RenderTapeProjectionStatus::MissingDefinition;
        result.failedIdentity = identity;
        return result;
      }
    }

    std::vector<SeedState> seedStates;
    for (const auto& definition : definitions) {
      if (definition.fixed.expectedContentBytes == 0u) {
        continue;
      }
      seedStates.push_back(SeedState{
          .identity = definition.fixed.identity,
          .expectedBytes = definition.fixed.expectedContentBytes,
          .expectedCount = definition.fixed.expectedContentCount,
      });
    }
    for (std::uint32_t eventIndex = 0u; eventIndex < tape.events.size();
         ++eventIndex) {
      const auto event = tape.event(eventIndex);
      const auto type = static_cast<RenderTapeEventType>(event.header.type);

      if (type == RenderTapeEventType::ObjectDefine) {
        RenderTapeObjectDefineHeader fixed{};
        if (!load(event.payload, 0u, fixed)) {
          result.status = RenderTapeProjectionStatus::InvalidSource;
          result.failedEventIndex = eventIndex;
          return result;
        }
        if (containsIdentity(closure, fixed.identity) &&
            fixed.payloadValidity == static_cast<std::uint32_t>(
                                         RenderTapeDigestValidity::Sha256)) {
          if (!exactBlob(verifiedCatalogue, fixed.immutablePayloadDigest,
                         fixed.immutablePayloadBytes)) {
            result.status = RenderTapeProjectionStatus::MissingBlobClosure;
            result.failedEventIndex = eventIndex;
            result.failedIdentity = fixed.identity;
            return result;
          }
          result.blobReferences.push_back(RenderTapeProjectionBlobReference{
              .kind = RenderTapeProjectionBlobKind::ImmutablePayload,
              .identity = fixed.identity,
              .digest = fixed.immutablePayloadDigest,
              .size = fixed.immutablePayloadBytes,
              .sourceEventIndex = eventIndex,
              .sourceEventOrdinal = event.header.ordinal,
          });
        }
        continue;
      }
      if (type != RenderTapeEventType::ResourceMutation) {
        continue;
      }

      RenderTapeResourceMutationHeader mutation{};
      if (!load(event.payload, 0u, mutation)) {
        result.status = RenderTapeProjectionStatus::InvalidSource;
        result.failedEventIndex = eventIndex;
        return result;
      }
      bool initialContent = false;
      const auto seed = std::find_if(
          seedStates.begin(), seedStates.end(), [&](const auto& state) {
            return sameIdentity(state.identity, mutation.identity);
          });
      if (seed != seedStates.end() &&
          (seed->recordedBytes != seed->expectedBytes ||
           seed->recordedCount != seed->expectedCount)) {
        seed->recordedBytes += mutation.byteSize;
        ++seed->recordedCount;
        initialContent = true;
      }

      if (eventIndex >= selectedEvent ||
          !containsIdentity(closure, mutation.identity)) {
        continue;
      }
      if (!exactBlob(verifiedCatalogue, mutation.digest, mutation.byteSize)) {
        result.status = RenderTapeProjectionStatus::MissingBlobClosure;
        result.failedEventIndex = eventIndex;
        result.failedIdentity = mutation.identity;
        return result;
      }
      result.blobReferences.push_back(RenderTapeProjectionBlobReference{
          .kind = RenderTapeProjectionBlobKind::ResourceMutation,
          .identity = mutation.identity,
          .digest = mutation.digest,
          .size = mutation.byteSize,
          .sourceEventIndex = eventIndex,
          .sourceEventOrdinal = event.header.ordinal,
          .mutationKind = mutation.kind,
          .subresource = mutation.subresource,
          .byteOffset = mutation.byteOffset,
          .initialContent = initialContent ? 1u : 0u,
      });
      if (initialContent) {
        auto* object = findProjectedObject(result.objects, mutation.identity);
        if (!object) {
          result.status = RenderTapeProjectionStatus::MissingDefinition;
          result.failedIdentity = mutation.identity;
          return result;
        }
        object->initialContentBytes += mutation.byteSize;
        ++object->initialContentCount;
      }
    }
    for (const auto& object : result.objects) {
      if (object.initialContentBytes != object.expectedContentBytes ||
          object.initialContentCount != object.expectedContentCount) {
        result.status = RenderTapeProjectionStatus::MissingInitialContent;
        result.failedIdentity = object.identity;
        return result;
      }
    }

    result.status = RenderTapeProjectionStatus::Valid;
    result.failedEventIndex = kRenderTapeProjectionNoIndex;
    result.failedRecordIndex = kRenderTapeProjectionNoIndex;
    return result;
  } catch (...) {
    result.status = RenderTapeProjectionStatus::AllocationFailed;
    result.selectedLocators.clear();
    result.objects.clear();
    result.blobReferences.clear();
    result.excludedEvents.clear();
    return result;
  }
}

const char* renderTapeProjectionStatusName(
    RenderTapeProjectionStatus status) noexcept {
  switch (status) {
  case RenderTapeProjectionStatus::Valid: return "valid";
  case RenderTapeProjectionStatus::InvalidSource: return "invalid-source";
  case RenderTapeProjectionStatus::UnsupportedProfile:
    return "unsupported-profile";
  case RenderTapeProjectionStatus::InvalidSelection:
    return "invalid-selection";
  case RenderTapeProjectionStatus::NonDrawRecord: return "non-draw-record";
  case RenderTapeProjectionStatus::MissingFullSnapshot:
    return "missing-full-snapshot";
  case RenderTapeProjectionStatus::MissingFrameBoundary:
    return "missing-frame-boundary";
  case RenderTapeProjectionStatus::MissingDefinition:
    return "missing-definition";
  case RenderTapeProjectionStatus::MissingInitialContent:
    return "missing-initial-content";
  case RenderTapeProjectionStatus::MissingBlobClosure:
    return "missing-blob-closure";
  case RenderTapeProjectionStatus::AllocationFailed:
    return "allocation-failed";
  }
  return "unknown";
}

const char* renderTapeProjectionBlobKindName(
    RenderTapeProjectionBlobKind kind) noexcept {
  switch (kind) {
  case RenderTapeProjectionBlobKind::ImmutablePayload:
    return "immutable-payload";
  case RenderTapeProjectionBlobKind::ResourceMutation:
    return "resource-mutation";
  }
  return "unknown";
}

const char* renderTapeProjectionExcludedKindName(
    RenderTapeProjectionExcludedKind kind) noexcept {
  switch (kind) {
  case RenderTapeProjectionExcludedKind::OrderedControl:
    return "ordered-control";
  case RenderTapeProjectionExcludedKind::ObjectDestroy:
    return "object-destroy";
  case RenderTapeProjectionExcludedKind::PresentComplete:
    return "present-complete";
  }
  return "unknown";
}

} // namespace dxmt9::d3d9
