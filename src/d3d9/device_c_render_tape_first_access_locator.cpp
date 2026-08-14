#include "device_c_render_tape_first_access_locator.hpp"

#include <cstring>

namespace dxmt9::d3d9 {
namespace {

constexpr std::uint32_t kD3DClearTarget = 0x1u;
constexpr std::uint32_t kD3DClearZBuffer = 0x2u;
constexpr std::uint32_t kD3DClearStencil = 0x4u;

bool sameIdentity(const D9CWireObjectIdentity& a,
                  const D9CWireObjectIdentity& b) noexcept {
  return a.kind == b.kind && a.generation == b.generation &&
         a.objectId == b.objectId;
}

bool targetIdentity(const RenderTapeFirstAccessLedger& ledger,
                    const D9CWireObjectIdentity& identity) noexcept {
  return sameIdentity(identity, ledger.originIdentity) ||
         sameIdentity(identity, ledger.resolvedIdentity);
}

template <typename T>
bool load(std::span<const std::byte> bytes, std::size_t offset,
          T& value) noexcept {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
    return false;
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return true;
}

bool recordPayload(const ImportedRecordView& record, std::size_t bytes) noexcept {
  return record.payload.size() >= bytes;
}

bool validRectTail(const ImportedRecordView& record, std::uint32_t count,
                   std::uint32_t offset) noexcept {
  if (offset != sizeof(D9CCommandChunkWireClear))
    return false;
  const auto rectBytes = static_cast<std::uint64_t>(count) * sizeof(D9CRect);
  return rectBytes <= std::numeric_limits<std::size_t>::max() - offset &&
         record.payload.size() == offset + rectBytes;
}

RenderTapeFirstAccessObservation baseObservation(
    const RenderTapeFirstAccessLedger& ledger) noexcept {
  RenderTapeFirstAccessObservation result{};
  result.originIdentity = ledger.originIdentity;
  result.resolvedIdentity = ledger.resolvedIdentity;
  result.aliasOrigin = !sameIdentity(ledger.originIdentity,
                                     ledger.resolvedIdentity);
  return result;
}

RenderTapeFirstAccessObservation malformed(
    const RenderTapeFirstAccessLedger& ledger) noexcept {
  auto result = baseObservation(ledger);
  result.status = RenderTapeFirstAccessStatus::Malformed;
  result.classification = RenderTapeFirstAccessClass::Unknown;
  return result;
}

RenderTapeFirstAccessObservation terminal(
    const RenderTapeFirstAccessLedger& ledger,
    RenderTapeFirstAccessClass classification,
    const D9CWireObjectIdentity& observedIdentity,
    std::uint32_t recordIndex, std::uint32_t recordType,
    std::uint32_t handleIndex, std::uint32_t sectionKind,
    std::uint32_t bindingSlot) noexcept {
  auto result = baseObservation(ledger);
  result.status = RenderTapeFirstAccessStatus::Terminal;
  result.classification = classification;
  result.observedIdentity = observedIdentity;
  result.recordIndex = recordIndex;
  result.recordType = recordType;
  result.handleIndex = handleIndex;
  result.sectionKind = sectionKind;
  result.bindingSlot = bindingSlot;
  return result;
}

bool bindingForHandle(const ImportedChunkView& chunk, std::uint32_t handleIndex,
                      RenderTapeFirstAccessBinding& result) noexcept {
  if (handleIndex >= chunk.handles.size())
    return false;
  const auto& handle = chunk.handles[handleIndex];
  result.valid = true;
  result.identity = {handle.kind, handle.generation, handle.objectId};
  result.handleIndex = handleIndex;
  return true;
}

bool recordOwnsHandle(const ImportedRecordView& record,
                      std::uint32_t handleIndex) noexcept {
  return handleIndex >= record.header.firstHandle &&
         handleIndex - record.header.firstHandle < record.header.handleCount;
}

bool observeSparseRecord(RenderTapeFirstAccessLedger& ledger,
                         const ImportedChunkView& chunk,
                         const ImportedRecordView& record,
                         std::uint32_t recordIndex,
                         RenderTapeFirstAccessObservation& result) noexcept {
  if (!recordPayload(record, sizeof(D9CCommandChunkWireDrawHeader)) ||
      record.sections.size() != record.drawHeader.sectionCount)
    return false;

  RenderTapeFirstAccessObservation candidate{};
  bool haveCandidate = false;
  for (std::size_t sectionIndex = 0u;
       sectionIndex < record.sections.size(); ++sectionIndex) {
    const auto section = record.section(sectionIndex);
    const auto& descriptor = record.sections[sectionIndex];
    if (section.payload.size() != descriptor.byteSize ||
        descriptor.elementSize == 0u ||
        descriptor.byteSize % descriptor.elementSize != 0u ||
        descriptor.count != descriptor.byteSize / descriptor.elementSize)
      return false;

    for (std::uint32_t element = 0u;
         element < descriptor.count; ++element) {
      const auto offset = static_cast<std::size_t>(element) *
                          descriptor.elementSize;
      std::uint32_t handleIndex = kRenderTapeFirstAccessSentinel;
      std::uint32_t bindingSlot = kRenderTapeFirstAccessSentinel;
      bool hasHandle = false;
      if (descriptor.kind == D9C_COMMAND_CHUNK_SECTION_TEXTURE) {
        D9CCommandChunkWireTextureBinding binding{};
        if (descriptor.elementSize != sizeof(binding) ||
            !load(section.payload, offset, binding))
          return false;
        handleIndex = binding.handleIndex;
        bindingSlot = binding.slot;
        hasHandle = binding.valid != 0u;
      } else if (descriptor.kind == D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET) {
        D9CCommandChunkWireRenderTargetBinding binding{};
        if (descriptor.elementSize != sizeof(binding) ||
            !load(section.payload, offset, binding))
          return false;
        handleIndex = binding.handleIndex;
        bindingSlot = binding.slot;
        hasHandle = binding.valid != 0u;
        if (bindingSlot >= ledger.renderTargets.size())
          return false;
        auto& state = ledger.renderTargets[bindingSlot];
        if (!hasHandle) {
          state = {};
          continue;
        }
        if (!recordOwnsHandle(record, handleIndex) ||
            !bindingForHandle(chunk, handleIndex, state))
          return false;
        state.sectionKind = descriptor.kind;
        state.bindingSlot = bindingSlot;
      } else if (descriptor.kind == D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL) {
        D9CCommandChunkWireDepthStencilBinding binding{};
        if (descriptor.elementSize != sizeof(binding) ||
            !load(section.payload, offset, binding))
          return false;
        handleIndex = binding.handleIndex;
        hasHandle = binding.valid != 0u;
        if (!hasHandle) {
          ledger.depthStencil = {};
          continue;
        }
        if (!recordOwnsHandle(record, handleIndex) ||
            !bindingForHandle(chunk, handleIndex, ledger.depthStencil))
          return false;
        ledger.depthStencil.sectionKind = descriptor.kind;
      } else {
        continue;
      }

      if (!hasHandle || handleIndex >= chunk.handles.size())
        continue;
      const auto identity = D9CWireObjectIdentity{
          chunk.handles[handleIndex].kind, chunk.handles[handleIndex].generation,
          chunk.handles[handleIndex].objectId};
      if (!targetIdentity(ledger, identity))
        continue;

      const auto classification = record.header.type ==
                                          D9C_COMMAND_RECORD_APPLY_STATE
                                      ? RenderTapeFirstAccessClass::BindingOnly
                                      : descriptor.kind ==
                                                D9C_COMMAND_CHUNK_SECTION_TEXTURE
                                            ? RenderTapeFirstAccessClass::ShaderReadCandidate
                                            : RenderTapeFirstAccessClass::DrawWriteUnknownCoverage;
      if (classification == RenderTapeFirstAccessClass::BindingOnly) {
        result = terminal(ledger, classification, identity, recordIndex,
                          record.header.type, handleIndex, descriptor.kind,
                          bindingSlot);
        result.status = RenderTapeFirstAccessStatus::Observing;
        continue;
      }
      if (!haveCandidate) {
        candidate = terminal(ledger, classification, identity, recordIndex,
                             record.header.type, handleIndex, descriptor.kind,
                             bindingSlot);
        haveCandidate = true;
      } else {
        candidate = terminal(ledger, RenderTapeFirstAccessClass::Unknown,
                             identity, recordIndex, record.header.type,
                             handleIndex, descriptor.kind, bindingSlot);
      }
    }
  }
  if (record.header.type != D9C_COMMAND_RECORD_APPLY_STATE) {
    const RenderTapeFirstAccessBinding* carried = nullptr;
    for (const auto& attachment : ledger.renderTargets) {
      if (attachment.valid && targetIdentity(ledger, attachment.identity)) {
        carried = &attachment;
        break;
      }
    }
    if (!carried && ledger.depthStencil.valid &&
        targetIdentity(ledger, ledger.depthStencil.identity)) {
      carried = &ledger.depthStencil;
    }
    if (carried) {
      if (haveCandidate) {
        candidate = terminal(ledger, RenderTapeFirstAccessClass::Unknown,
                             carried->identity, recordIndex, record.header.type,
                             carried->handleIndex, carried->sectionKind,
                             carried->bindingSlot);
      } else {
        candidate = terminal(
            ledger, RenderTapeFirstAccessClass::DrawWriteUnknownCoverage,
            carried->identity, recordIndex, record.header.type,
            carried->handleIndex, carried->sectionKind, carried->bindingSlot);
        haveCandidate = true;
      }
    }
  }
  if (haveCandidate) {
    result = candidate;
    return true;
  }
  result = baseObservation(ledger);
  result.status = RenderTapeFirstAccessStatus::Observing;
  result.classification = RenderTapeFirstAccessClass::BindingOnly;
  return true;
}

RenderTapeFirstAccessObservation candidateForHandle(
    const RenderTapeFirstAccessLedger& ledger, const ImportedChunkView& chunk,
    const ImportedRecordView& record, std::uint32_t recordIndex,
    std::uint32_t handleIndex, RenderTapeFirstAccessClass classification,
    std::uint32_t sectionKind = kRenderTapeFirstAccessSentinel,
    std::uint32_t bindingSlot = kRenderTapeFirstAccessSentinel) noexcept {
  if (handleIndex >= chunk.handles.size() ||
      !recordOwnsHandle(record, handleIndex))
    return malformed(ledger);
  const auto identity = D9CWireObjectIdentity{
      chunk.handles[handleIndex].kind, chunk.handles[handleIndex].generation,
      chunk.handles[handleIndex].objectId};
  if (!targetIdentity(ledger, identity)) {
    auto result = baseObservation(ledger);
    result.status = RenderTapeFirstAccessStatus::Observing;
    return result;
  }
  return terminal(ledger, classification, identity, recordIndex,
                  record.header.type, handleIndex, sectionKind, bindingSlot);
}

RenderTapeFirstAccessObservation observeFixedRecord(
    RenderTapeFirstAccessLedger& ledger, const ImportedChunkView& chunk,
    const ImportedRecordView& record, std::uint32_t recordIndex) noexcept {
  if (record.header.type == D9C_COMMAND_RECORD_CLEAR) {
    D9CCommandChunkWireClear clear{};
    if (!load(record.payload, 0u, clear) ||
        !validRectTail(record, clear.rectCount, clear.rectOffset))
      return malformed(ledger);
    const bool full = clear.rectCount == 0u;
    for (const auto& attachment : ledger.renderTargets) {
      if (attachment.valid && targetIdentity(ledger, attachment.identity) &&
          (clear.flags & kD3DClearTarget) != 0u) {
        return terminal(ledger,
                        full ? RenderTapeFirstAccessClass::FullClearWrite
                             : RenderTapeFirstAccessClass::PartialClearWrite,
                        attachment.identity, recordIndex, record.header.type,
                        attachment.handleIndex, attachment.sectionKind,
                        attachment.bindingSlot);
      }
    }
    if (ledger.depthStencil.valid &&
        targetIdentity(ledger, ledger.depthStencil.identity) &&
        (clear.flags & (kD3DClearZBuffer | kD3DClearStencil)) != 0u) {
      return terminal(ledger,
                      full ? RenderTapeFirstAccessClass::FullClearWrite
                           : RenderTapeFirstAccessClass::PartialClearWrite,
                      ledger.depthStencil.identity, recordIndex,
                      record.header.type, ledger.depthStencil.handleIndex,
                      ledger.depthStencil.sectionKind,
                      ledger.depthStencil.bindingSlot);
    }
    return {};
  }
  if (record.header.type == D9C_COMMAND_RECORD_PRESENT) {
    if (!recordPayload(record, sizeof(D9CCommandChunkWirePresent)))
      return malformed(ledger);
    const auto handleEnd = static_cast<std::uint64_t>(record.header.firstHandle) +
                           record.header.handleCount;
    for (std::uint32_t handleIndex = record.header.firstHandle;
         static_cast<std::uint64_t>(handleIndex) < handleEnd; ++handleIndex) {
      if (handleIndex >= chunk.handles.size())
        return malformed(ledger);
      const auto identity = D9CWireObjectIdentity{
          chunk.handles[handleIndex].kind, chunk.handles[handleIndex].generation,
          chunk.handles[handleIndex].objectId};
      if (targetIdentity(ledger, identity)) {
        return terminal(ledger, RenderTapeFirstAccessClass::PresentRead,
                        identity, recordIndex, record.header.type, handleIndex,
                        kRenderTapeFirstAccessSentinel,
                        kRenderTapeFirstAccessSentinel);
      }
    }
    for (const auto& attachment : ledger.renderTargets) {
      if (attachment.valid && targetIdentity(ledger, attachment.identity))
        return terminal(ledger, RenderTapeFirstAccessClass::PresentRead,
                        attachment.identity, recordIndex, record.header.type,
                        attachment.handleIndex, attachment.sectionKind,
                        attachment.bindingSlot);
    }
    return {};
  }

  if (record.header.type == D9C_COMMAND_RECORD_UPDATE_TEXTURE) {
    D9CCommandChunkWireUpdateTexture value{};
    if (!load(record.payload, 0u, value))
      return malformed(ledger);
    auto result = candidateForHandle(
        ledger, chunk, record, recordIndex, value.srcHandleIndex,
        RenderTapeFirstAccessClass::CopySource);
    if (result.status == RenderTapeFirstAccessStatus::Terminal)
      return result;
    return candidateForHandle(
        ledger, chunk, record, recordIndex, value.dstHandleIndex,
        RenderTapeFirstAccessClass::CopyDestinationPartial);
  }
  if (record.header.type == D9C_COMMAND_RECORD_UPDATE_SURFACE) {
    D9CCommandChunkWireUpdateSurface value{};
    if (!load(record.payload, 0u, value))
      return malformed(ledger);
    if (value.hasSrcRect != 0u && value.hasDstPoint == 0u)
      return malformed(ledger);
    auto result = candidateForHandle(
        ledger, chunk, record, recordIndex, value.srcHandleIndex,
        RenderTapeFirstAccessClass::CopySource);
    if (result.status == RenderTapeFirstAccessStatus::Terminal)
      return result;
    return candidateForHandle(
        ledger, chunk, record, recordIndex, value.dstHandleIndex,
        RenderTapeFirstAccessClass::CopyDestinationPartial);
  }
  if (record.header.type == D9C_COMMAND_RECORD_STRETCH_RECT) {
    D9CCommandChunkWireStretchRect value{};
    if (!load(record.payload, 0u, value))
      return malformed(ledger);
    auto result = candidateForHandle(
        ledger, chunk, record, recordIndex, value.srcHandleIndex,
        RenderTapeFirstAccessClass::CopySource);
    if (result.status == RenderTapeFirstAccessStatus::Terminal)
      return result;
    return candidateForHandle(
        ledger, chunk, record, recordIndex, value.dstHandleIndex,
        RenderTapeFirstAccessClass::CopyDestinationPartial);
  }
  if (record.header.type == D9C_COMMAND_RECORD_COLOR_FILL) {
    D9CCommandChunkWireColorFill value{};
    if (!load(record.payload, 0u, value) || value.hasRect > 1u)
      return malformed(ledger);
    return candidateForHandle(
        ledger, chunk, record, recordIndex, value.surfaceHandleIndex,
        RenderTapeFirstAccessClass::CopyDestinationPartial);
  }
  if (record.header.type == D9C_COMMAND_RECORD_READBACK) {
    D9CCommandChunkWireReadback value{};
    if (!load(record.payload, 0u, value))
      return malformed(ledger);
    auto result = candidateForHandle(
        ledger, chunk, record, recordIndex, value.srcHandleIndex,
        RenderTapeFirstAccessClass::CopySource);
    if (result.status == RenderTapeFirstAccessStatus::Terminal)
      return result;
    return candidateForHandle(
        ledger, chunk, record, recordIndex, value.dstHandleIndex,
        RenderTapeFirstAccessClass::CopyDestinationPartial);
  }
  if (record.header.type == D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE) {
    D9CCommandChunkWireReszDepthResolve value{};
    if (!load(record.payload, 0u, value))
      return malformed(ledger);
    auto result = candidateForHandle(
        ledger, chunk, record, recordIndex, value.msaaDepthHandleIndex,
        RenderTapeFirstAccessClass::CopySource);
    if (result.status == RenderTapeFirstAccessStatus::Terminal)
      return result;
    return candidateForHandle(
        ledger, chunk, record, recordIndex, value.intzDestHandleIndex,
        RenderTapeFirstAccessClass::CopyDestinationPartial);
  }
  return {};
}

} // namespace

void renderTapeFirstAccessArm(
    RenderTapeFirstAccessLedger& ledger,
    const D9CWireObjectIdentity& originIdentity,
    const D9CWireObjectIdentity& resolvedIdentity) noexcept {
  if (ledger.armed)
    return;
  ledger = {};
  ledger.armed = true;
  ledger.originIdentity = originIdentity;
  ledger.resolvedIdentity = resolvedIdentity;
}

RenderTapeFirstAccessObservation renderTapeFirstAccessObserve(
    RenderTapeFirstAccessLedger& ledger,
    const ImportedChunkView& chunk) noexcept {
  if (!ledger.armed)
    return {};
  if (ledger.terminal) {
    auto result = baseObservation(ledger);
    result.status = RenderTapeFirstAccessStatus::Complete;
    return result;
  }
  for (std::size_t index = 0u; index < chunk.records.size(); ++index) {
    const auto record = chunk.record(index);
    if (record.header.type == 0u || record.payload.empty()) {
      ledger.terminal = true;
      return malformed(ledger);
    }
    RenderTapeFirstAccessObservation result{};
    const auto* rule = recordRule(record.header.type);
    if (!rule || record.payload.size() < rule->fixedPayloadSize) {
      ledger.terminal = true;
      return malformed(ledger);
    }
    if ((rule->ruleFlags & RecordRuleSparseState) != 0u) {
      if (!observeSparseRecord(ledger, chunk, record,
                               static_cast<std::uint32_t>(index), result)) {
        ledger.terminal = true;
        return malformed(ledger);
      }
    } else {
      result = observeFixedRecord(ledger, chunk, record,
                                  static_cast<std::uint32_t>(index));
    }
    if (result.status == RenderTapeFirstAccessStatus::Malformed) {
      ledger.terminal = true;
      return result;
    }
    if (result.status == RenderTapeFirstAccessStatus::Terminal) {
      ledger.terminal = true;
      return result;
    }
  }
  auto result = baseObservation(ledger);
  result.status = RenderTapeFirstAccessStatus::Observing;
  result.classification = RenderTapeFirstAccessClass::BindingOnly;
  return result;
}

bool renderTapeProveProducedByCapturedPass(
    const ImportedChunkView& chunk,
    const D9CWireObjectIdentity& originIdentity,
    const D9CWireObjectIdentity& resolvedIdentity) noexcept {
  if (originIdentity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE ||
      resolvedIdentity.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE ||
      sameIdentity(originIdentity, resolvedIdentity)) {
    return false;
  }
  RenderTapeFirstAccessLedger ledger{};
  renderTapeFirstAccessArm(ledger, originIdentity, resolvedIdentity);
  const auto observation = renderTapeFirstAccessObserve(ledger, chunk);
  return observation.status == RenderTapeFirstAccessStatus::Terminal &&
         observation.classification == RenderTapeFirstAccessClass::FullClearWrite &&
         observation.aliasOrigin &&
         sameIdentity(observation.originIdentity, originIdentity) &&
         sameIdentity(observation.resolvedIdentity, resolvedIdentity) &&
         sameIdentity(observation.observedIdentity, originIdentity);
}

const char* renderTapeFirstAccessClassName(
    RenderTapeFirstAccessClass classification) noexcept {
  switch (classification) {
  case RenderTapeFirstAccessClass::BindingOnly:
    return "binding_only";
  case RenderTapeFirstAccessClass::FullClearWrite:
    return "full_clear_write";
  case RenderTapeFirstAccessClass::PartialClearWrite:
    return "partial_clear_write";
  case RenderTapeFirstAccessClass::DrawWriteUnknownCoverage:
    return "draw_write_unknown_coverage";
  case RenderTapeFirstAccessClass::ShaderReadCandidate:
    return "shader_read_candidate";
  case RenderTapeFirstAccessClass::CopySource:
    return "copy_source";
  case RenderTapeFirstAccessClass::CopyDestinationFull:
    return "copy_destination_full";
  case RenderTapeFirstAccessClass::CopyDestinationPartial:
    return "copy_destination_partial";
  case RenderTapeFirstAccessClass::PresentRead:
    return "present_read";
  case RenderTapeFirstAccessClass::Unknown:
    return "unknown";
  }
  return "unknown";
}

const char* renderTapeFirstAccessStatusName(
    RenderTapeFirstAccessStatus status) noexcept {
  switch (status) {
  case RenderTapeFirstAccessStatus::Idle:
    return "idle";
  case RenderTapeFirstAccessStatus::Observing:
    return "observing";
  case RenderTapeFirstAccessStatus::Terminal:
    return "terminal";
  case RenderTapeFirstAccessStatus::Malformed:
    return "malformed";
  case RenderTapeFirstAccessStatus::Complete:
    return "complete";
  }
  return "unknown";
}

} // namespace dxmt9::d3d9
