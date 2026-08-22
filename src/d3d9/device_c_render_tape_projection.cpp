#include "device_c_render_tape_projection.hpp"

#include "device_c_render_tape_capture.hpp"

#include <algorithm>
#include <array>
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

std::size_t alignUp(std::size_t value, std::size_t alignment) noexcept {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

bool sameDigest(const RenderTapeDigest& left,
                const RenderTapeDigest& right) noexcept {
  return std::equal(left.begin(), left.end(), right.begin());
}

struct ProjectionRecordCopy {
  std::uint32_t type = 0u;
  std::uint32_t flags = 0u;
  std::uint32_t oldFirstHandle = 0u;
  std::vector<std::byte> payload{};
  std::vector<D9CCommandChunkWireHandleEntry> handles{};
};

bool remapProjectionRecordPayload(ProjectionRecordCopy& record,
                                  std::uint32_t newFirstHandle) noexcept {
  const auto remap = [&](std::uint32_t& value) {
    if (value == D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX) return true;
    if (value < record.oldFirstHandle ||
        value - record.oldFirstHandle >= record.handles.size()) {
      return false;
    }
    value = newFirstHandle + (value - record.oldFirstHandle);
    return true;
  };
  for (const auto& field : kRecordHandleFieldRules) {
    if (field.recordType != record.type) continue;
    if (record.handles.empty()) continue;
    std::uint32_t value = 0u;
    if (!load(record.payload, field.payloadOffset, value) || !remap(value)) {
      return false;
    }
    std::memcpy(record.payload.data() + field.payloadOffset, &value,
                sizeof(value));
  }
  const auto* rule = recordRule(record.type);
  if (!rule || (rule->ruleFlags & RecordRuleSparseState) == 0u) return true;
  D9CCommandChunkWireDrawHeader draw{};
  if (!load(record.payload, 0u, draw)) return false;
  for (std::uint32_t sectionIndex = 0u;
       sectionIndex < draw.sectionCount; ++sectionIndex) {
    D9CCommandChunkWireSectionDesc section{};
    const auto sectionOffset = draw.sectionTableOffset +
        sectionIndex * sizeof(D9CCommandChunkWireSectionDesc);
    if (!load(record.payload, sectionOffset, section)) return false;
    const auto* field = sectionHandleFieldRule(section.kind);
    if (!field) continue;
    for (std::uint32_t element = 0u; element < section.count; ++element) {
      const auto offset = static_cast<std::size_t>(section.payloadOffset) +
          static_cast<std::size_t>(element) * section.elementSize +
          field->payloadOffset;
      std::uint32_t value = 0u;
      if (!load(record.payload, offset, value) || !remap(value)) return false;
      std::memcpy(record.payload.data() + offset, &value, sizeof(value));
    }
  }
  return true;
}

bool copyProjectionRecord(const ImportedRenderTapeView& tape,
                          const RenderTapeProjectionLocator& locator,
                          ProjectionRecordCopy& out) {
  if (locator.sourceEventIndex >= tape.events.size()) return false;
  const auto event = tape.event(locator.sourceEventIndex);
  RenderTapeCommandChunkHeader fixed{};
  if (!load(event.payload, 0u, fixed)) return false;
  ImportedChunkView chunk;
  if (!importPrevalidatedCommandChunk(
          event.payload.subspan(sizeof(fixed), fixed.chunkBytes),
          CommandChunkEnvelope{fixed.wireVersion, fixed.recordCount,
                               fixed.handleCount},
          chunk) || locator.recordIndex >= chunk.records.size()) {
    return false;
  }
  const auto record = chunk.record(locator.recordIndex);
  out.type = record.header.type;
  out.flags = record.header.flags;
  out.oldFirstHandle = record.header.firstHandle;
  out.payload.assign(record.payload.begin(), record.payload.end());
  out.handles.assign(record.handles.begin(), record.handles.end());
  return true;
}

bool buildProjectionChunk(std::span<ProjectionRecordCopy> copies,
                          std::vector<std::byte>& bytes,
                          CommandChunkEnvelope& envelope,
                          CommandChunkValidationStatus* failedStatus = nullptr) {
  std::vector<D9CCommandChunkWireRecordHeader> records;
  std::vector<D9CCommandChunkWireHandleEntry> handles;
  std::vector<std::byte> payload;
  records.reserve(copies.size());
  for (auto& copy : copies) {
    const auto* rule = recordRule(copy.type);
    if (!rule) return false;
    const auto firstHandle = static_cast<std::uint32_t>(handles.size());
    if (!remapProjectionRecordPayload(copy, firstHandle)) return false;
    payload.resize(alignUp(payload.size(), rule->payloadAlignment));
    records.push_back(D9CCommandChunkWireRecordHeader{
        .type = copy.type,
        .flags = copy.flags,
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .payloadSize = static_cast<std::uint32_t>(copy.payload.size()),
        .firstHandle = firstHandle,
        .handleCount = static_cast<std::uint32_t>(copy.handles.size()),
        .reserved0 = 0u,
        .reserved1 = 0u,
    });
    handles.insert(handles.end(), copy.handles.begin(), copy.handles.end());
    payload.insert(payload.end(), copy.payload.begin(), copy.payload.end());
  }
  const auto recordTableOffset = sizeof(D9CCommandChunkWireHeader);
  const auto handleTableOffset = alignUp(
      recordTableOffset + records.size() * sizeof(records[0]),
      alignof(D9CCommandChunkWireHandleEntry));
  const auto payloadArenaOffset = alignUp(
      handleTableOffset + handles.size() * sizeof(handles[0]),
      alignof(std::uint32_t));
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordTableOffset),
      .recordCount = static_cast<std::uint32_t>(records.size()),
      .handleTableOffset = static_cast<std::uint32_t>(handleTableOffset),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadArenaOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
      .reserved0 = 0u,
      .reserved1 = 0u,
  };
  bytes.assign(payloadArenaOffset + payload.size(), std::byte{0});
  std::memcpy(bytes.data(), &header, sizeof(header));
  if (!records.empty()) {
    std::memcpy(bytes.data() + recordTableOffset, records.data(),
                records.size() * sizeof(records[0]));
  }
  if (!handles.empty()) {
    std::memcpy(bytes.data() + handleTableOffset, handles.data(),
                handles.size() * sizeof(handles[0]));
  }
  if (!payload.empty()) {
    std::memcpy(bytes.data() + payloadArenaOffset, payload.data(),
                payload.size());
  }
  envelope = CommandChunkEnvelope{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .recordCount = static_cast<std::uint32_t>(records.size()),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
  };
  const auto validation = validateCommandChunk(bytes, envelope);
  if (failedStatus) *failedStatus = validation.status;
  return validation.valid();
}

bool identityIn(std::span<const D9CWireObjectIdentity> identities,
                const D9CWireObjectIdentity& identity) noexcept {
  return containsIdentity(identities, identity);
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

    result.stateFold = foldRenderTapeStateForDraw(
        source, verifiedCatalogue, selectedEventView.header.ordinal,
        selector.firstRecordIndex);
    if (!result.stateFold.valid()) {
      result.status = RenderTapeProjectionStatus::StateFoldFailed;
      result.failedEventIndex = result.stateFold.failedEventIndex;
      result.failedRecordIndex = result.stateFold.failedRecordIndex;
      return result;
    }
    for (const auto& identity : result.stateFold.referencedIdentities) {
      addIdentity(closure, identity);
    }

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
    const auto addBoundaryHandles = [&](const RenderTapeProjectionLocator& locator) {
      const auto event = tape.event(locator.sourceEventIndex);
      RenderTapeCommandChunkHeader fixed{};
      if (!load(event.payload, 0u, fixed)) return false;
      ImportedChunkView chunk;
      if (!importPrevalidatedCommandChunk(
              event.payload.subspan(sizeof(fixed), fixed.chunkBytes),
              CommandChunkEnvelope{fixed.wireVersion, fixed.recordCount,
                                   fixed.handleCount},
              chunk) ||
          locator.recordIndex >= chunk.records.size()) {
        return false;
      }
      for (const auto& handle : chunk.record(locator.recordIndex).handles) {
        addIdentity(closure, D9CWireObjectIdentity{
                                 .kind = handle.kind,
                                 .generation = handle.generation,
                                 .objectId = handle.objectId,
                             });
      }
      return true;
    };
    if (!addBoundaryHandles(result.clearLocator) ||
        !addBoundaryHandles(result.presentLocator)) {
      result.status = RenderTapeProjectionStatus::InvalidSource;
      return result;
    }
    for (std::uint32_t eventIndex = 0u; eventIndex < tape.events.size();
         ++eventIndex) {
      const auto event = tape.event(eventIndex);
      if (static_cast<RenderTapeEventType>(event.header.type) !=
          RenderTapeEventType::PresentComplete) {
        continue;
      }
      RenderTapePresentCompleteHeader completion{};
      if (!load(event.payload, 0u, completion)) {
        result.status = RenderTapeProjectionStatus::InvalidSource;
        result.failedEventIndex = eventIndex;
        return result;
      }
      const auto attachments = event.payload.subspan(sizeof(completion));
      for (std::uint32_t attachmentIndex = 0u;
           attachmentIndex < completion.oracleCount; ++attachmentIndex) {
        RenderTapeOracleAttachment attachment{};
        if (!load(attachments,
                  attachmentIndex * sizeof(RenderTapeOracleAttachment),
                  attachment)) {
          result.status = RenderTapeProjectionStatus::InvalidSource;
          result.failedEventIndex = eventIndex;
          return result;
        }
        addIdentity(closure, attachment.identity);
      }
    }
    bool closureChanged = true;
    while (closureChanged) {
      closureChanged = false;
      for (const auto& definition : definitions) {
        if (!containsIdentity(closure, definition.fixed.identity) ||
            definition.fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE) {
          continue;
        }
        const auto event = tape.event(definition.eventIndex);
        RenderTapeSurfaceDescriptorV2 surface{};
        if (!load(event.payload.subspan(sizeof(definition.fixed)), 0u, surface)) {
          result.status = RenderTapeProjectionStatus::InvalidSource;
          result.failedEventIndex = definition.eventIndex;
          return result;
        }
        if (surface.storage != static_cast<std::uint32_t>(
                                   RenderTapeSurfaceStorage::TextureSubresource) ||
            containsIdentity(closure, surface.parentTexture)) {
          continue;
        }
        addIdentity(closure, surface.parentTexture);
        closureChanged = true;
      }
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
  case RenderTapeProjectionStatus::StateFoldFailed:
    return "state-fold-failed";
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

RenderTapeProjectionBundleResult materializeRenderTapeProjectionBundle(
    std::span<const std::byte> source,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    std::span<const std::byte> identitySidecar,
    const RenderTapeProjectionSelector& selector,
    RenderTapeDigestValidity outputDigestValidity,
    RenderTapeDigest outputDigest) noexcept {
  RenderTapeProjectionBundleResult result{};
  try {
    result.projection =
        projectRenderTapeDrawSlice(source, verifiedCatalogue, selector);
    if (!result.projection.valid()) {
      result.status = RenderTapeProjectionBundleStatus::InvalidProjection;
      return result;
    }
    RenderTapeIdentityView identity;
    result.identityValidation = validateRenderTapeIdentity(
        source, verifiedCatalogue, identitySidecar, &identity);
    if (!result.identityValidation.valid() ||
        identity.header.authority != static_cast<std::uint32_t>(
                                         RenderTapeIdentityAuthority::Capture)) {
      result.status = RenderTapeProjectionBundleStatus::InvalidIdentity;
      return result;
    }
    if (!renderTapeIdentityOwnsSelection(
            identity, selector.commandEventOrdinal, selector.firstRecordIndex,
            selector.recordCount, &result.logicalPassId)) {
      result.status = RenderTapeProjectionBundleStatus::SelectionOutsidePass;
      return result;
    }

    ImportedRenderTapeView tape;
    if (!importPrevalidatedRenderTape(source, tape)) {
      result.status = RenderTapeProjectionBundleStatus::InvalidProjection;
      return result;
    }
    CommandChunkValidationStatus chunkStatus{};

    std::vector<ProjectionRecordCopy> commandCopies;
    commandCopies.reserve(result.projection.selectedLocators.size() + 2u);
    commandCopies.emplace_back();
    if (!copyProjectionRecord(tape, result.projection.clearLocator,
                              commandCopies.back())) {
      result.status = RenderTapeProjectionBundleStatus::ChunkBuildFailure;
      return result;
    }
    for (const auto& locator : result.projection.selectedLocators) {
      commandCopies.emplace_back();
      if (!copyProjectionRecord(tape, locator, commandCopies.back())) {
        result.status = RenderTapeProjectionBundleStatus::ChunkBuildFailure;
        return result;
      }
    }
    commandCopies.emplace_back();
    if (!copyProjectionRecord(tape, result.projection.presentLocator,
                              commandCopies.back())) {
      result.status = RenderTapeProjectionBundleStatus::ChunkBuildFailure;
      return result;
    }
    std::vector<std::byte> commandChunk;
    CommandChunkEnvelope commandEnvelope{};
    if (!buildProjectionChunk(commandCopies, commandChunk, commandEnvelope,
                              &chunkStatus)) {
      result.status = RenderTapeProjectionBundleStatus::ChunkBuildFailure;
      result.projection.sourceValidation.chunkStatus = chunkStatus;
      return result;
    }

    std::vector<D9CWireObjectIdentity> retainedIdentities;
    retainedIdentities.reserve(result.projection.objects.size());
    for (const auto& object : result.projection.objects) {
      retainedIdentities.push_back(object.identity);
    }
    std::vector<std::uint32_t> retainedMutationEvents;
    for (const auto& reference : result.projection.blobReferences) {
      if (reference.kind == RenderTapeProjectionBlobKind::ResourceMutation) {
        retainedMutationEvents.push_back(reference.sourceEventIndex);
      }
      if (std::none_of(result.referencedBlobDigests.begin(),
                       result.referencedBlobDigests.end(),
                       [&](const auto& digest) {
                         return sameDigest(digest, reference.digest);
                       })) {
        result.referencedBlobDigests.push_back(reference.digest);
      }
    }

    RenderTapeBuilder builder;
    builder.appendBootstrapState(result.projection.stateFold.bootstrapChunk,
                                 result.projection.stateFold.gammaRamp);
    for (std::uint32_t eventIndex = 0u; eventIndex < tape.events.size();
         ++eventIndex) {
      const auto event = tape.event(eventIndex);
      const auto type = static_cast<RenderTapeEventType>(event.header.type);
      if (type == RenderTapeEventType::ObjectDefine) {
        RenderTapeObjectDefineHeader fixed{};
        if (!load(event.payload, 0u, fixed)) {
          result.status = RenderTapeProjectionBundleStatus::ClosureFailure;
          return result;
        }
        if (identityIn(retainedIdentities, fixed.identity)) {
          builder.appendRawEvent(type, event.payload);
        }
      } else if (type == RenderTapeEventType::ResourceMutation &&
                 std::find(retainedMutationEvents.begin(),
                           retainedMutationEvents.end(), eventIndex) !=
                     retainedMutationEvents.end()) {
        builder.appendRawEvent(type, event.payload);
      }
    }
    builder.appendCommandChunk(commandEnvelope, commandChunk);

    RenderTapePresentCompleteHeader completion{};
    std::span<const std::byte> attachments;
    bool foundCompletion = false;
    for (std::uint32_t eventIndex = 0u; eventIndex < tape.events.size();
         ++eventIndex) {
      const auto event = tape.event(eventIndex);
      if (static_cast<RenderTapeEventType>(event.header.type) !=
          RenderTapeEventType::PresentComplete) {
        continue;
      }
      if (!load(event.payload, 0u, completion)) {
        result.status = RenderTapeProjectionBundleStatus::ClosureFailure;
        return result;
      }
      attachments = event.payload.subspan(sizeof(completion));
      foundCompletion = true;
      break;
    }
    if (!foundCompletion) {
      result.status = RenderTapeProjectionBundleStatus::ClosureFailure;
      return result;
    }
    builder.appendPresentComplete(
        builder.eventCount(), 1u,
        outputDigestValidity, outputDigest, attachments);
    result.bytes = builder.seal();
    const auto validation = validateRenderTape(
        result.bytes, verifiedCatalogue);
    if (!validation.valid()) {
      result.status = RenderTapeProjectionBundleStatus::OutputValidationFailed;
      result.projection.sourceValidation = validation;
      result.bytes.clear();
      result.referencedBlobDigests.clear();
      return result;
    }
    result.status = RenderTapeProjectionBundleStatus::Valid;
    return result;
  } catch (...) {
    result.status = RenderTapeProjectionBundleStatus::AllocationFailed;
    result.bytes.clear();
    result.referencedBlobDigests.clear();
    return result;
  }
}

const char* renderTapeProjectionBundleStatusName(
    RenderTapeProjectionBundleStatus status) noexcept {
  switch (status) {
  case RenderTapeProjectionBundleStatus::Valid: return "valid";
  case RenderTapeProjectionBundleStatus::InvalidProjection:
    return "invalid-projection";
  case RenderTapeProjectionBundleStatus::InvalidIdentity:
    return "invalid-identity";
  case RenderTapeProjectionBundleStatus::SelectionOutsidePass:
    return "selection-outside-pass";
  case RenderTapeProjectionBundleStatus::ClosureFailure:
    return "closure-failure";
  case RenderTapeProjectionBundleStatus::ChunkBuildFailure:
    return "chunk-build-failure";
  case RenderTapeProjectionBundleStatus::OutputValidationFailed:
    return "output-validation-failed";
  case RenderTapeProjectionBundleStatus::AllocationFailed:
    return "allocation-failed";
  }
  return "unknown";
}

} // namespace dxmt9::d3d9
