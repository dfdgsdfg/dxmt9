#include "device_c_render_tape_origin_locator.hpp"

#include <cstring>

namespace dxmt9::d3d9 {
namespace {

template <typename T>
bool load(std::span<const std::byte> bytes, std::size_t offset, T& value) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
    return false;
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return true;
}

bool sameIdentity(const D9CWireObjectIdentity& a,
                 const D9CWireObjectIdentity& b) noexcept {
  return a.kind == b.kind && a.generation == b.generation &&
         a.objectId == b.objectId;
}

bool recordOwnsHandle(const ImportedRecordView& record,
                      std::uint32_t handleIndex) noexcept {
  return handleIndex >= record.header.firstHandle &&
         handleIndex - record.header.firstHandle < record.header.handleCount;
}

bool sparseSectionBinding(const ImportedSectionView& section,
                         std::uint32_t handleIndex,
                         std::uint32_t& bindingSlot, bool& matched) noexcept {
  const auto kind = section.descriptor.kind;
  const auto elementSize = section.descriptor.elementSize;
  matched = false;
  if (elementSize == 0u || section.payload.size() % elementSize != 0u)
    return false;

  for (std::uint32_t element = 0u;
       element < section.payload.size() / elementSize; ++element) {
    const auto offset = static_cast<std::size_t>(element) * elementSize;
    std::uint32_t referencedHandle = kRenderTapeOriginSentinel;
    std::uint32_t slot = kRenderTapeOriginSentinel;
    switch (kind) {
    case D9C_COMMAND_CHUNK_SECTION_TEXTURE: {
      D9CCommandChunkWireTextureBinding value{};
      if (!load(section.payload, offset, value))
        return false;
      referencedHandle = value.handleIndex;
      slot = value.slot;
      break;
    }
    case D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET: {
      D9CCommandChunkWireRenderTargetBinding value{};
      if (!load(section.payload, offset, value))
        return false;
      referencedHandle = value.handleIndex;
      slot = value.slot;
      break;
    }
    case D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL: {
      D9CCommandChunkWireDepthStencilBinding value{};
      if (!load(section.payload, offset, value))
        return false;
      referencedHandle = value.handleIndex;
      break;
    }
    default:
      continue;
    }
    if (referencedHandle == handleIndex) {
      matched = true;
      bindingSlot = slot;
      return true;
    }
  }
  return true;
}

RenderTapeCommandRole sparseRole(std::uint32_t recordType,
                                 std::uint32_t sectionKind) noexcept {
  switch (sectionKind) {
  case D9C_COMMAND_CHUNK_SECTION_TEXTURE:
    return recordType == D9C_COMMAND_RECORD_APPLY_STATE
               ? RenderTapeCommandRole::BindingOnly
               : RenderTapeCommandRole::ShaderReadCandidate;
  case D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET:
    return RenderTapeCommandRole::RenderTargetBinding;
  case D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL:
    return RenderTapeCommandRole::DepthStencilBinding;
  default:
    return RenderTapeCommandRole::Unknown;
  }
}

} // namespace

RenderTapeStorageRole renderTapeStorageRoleForCommandRole(
    RenderTapeCommandRole role) noexcept {
  switch (role) {
  case RenderTapeCommandRole::BindingOnly:
    return RenderTapeStorageRole::BindingStorage;
  case RenderTapeCommandRole::ShaderReadCandidate:
    return RenderTapeStorageRole::ShaderReadCandidate;
  case RenderTapeCommandRole::RenderTargetBinding:
    return RenderTapeStorageRole::RenderTargetCandidate;
  case RenderTapeCommandRole::DepthStencilBinding:
    return RenderTapeStorageRole::DepthStencilCandidate;
  case RenderTapeCommandRole::CopySource:
    return RenderTapeStorageRole::CopySourceCandidate;
  case RenderTapeCommandRole::CopyDestination:
    return RenderTapeStorageRole::CopyDestinationCandidate;
  case RenderTapeCommandRole::ReadbackSource:
    return RenderTapeStorageRole::ReadbackSourceCandidate;
  case RenderTapeCommandRole::Unknown:
    return RenderTapeStorageRole::Unknown;
  }
  return RenderTapeStorageRole::Unknown;
}

namespace {

bool nonDrawBinding(const ImportedRecordView& record,
                    std::uint32_t handleIndex, std::uint32_t& sectionKind,
                    std::uint32_t& bindingSlot,
                    RenderTapeCommandRole& role) noexcept {
  sectionKind = kRenderTapeOriginSentinel;
  bindingSlot = kRenderTapeOriginSentinel;
  role = RenderTapeCommandRole::Unknown;

  switch (record.header.type) {
  case D9C_COMMAND_RECORD_STRETCH_RECT: {
    D9CCommandChunkWireStretchRect value{};
    if (!load(record.payload, 0u, value))
      return false;
    if (value.srcHandleIndex == handleIndex) {
      role = RenderTapeCommandRole::CopySource;
      return true;
    }
    if (value.dstHandleIndex == handleIndex) {
      role = RenderTapeCommandRole::CopyDestination;
      return true;
    }
    return true;
  }
  case D9C_COMMAND_RECORD_COLOR_FILL: {
    D9CCommandChunkWireColorFill value{};
    if (!load(record.payload, 0u, value))
      return false;
    if (value.surfaceHandleIndex == handleIndex)
      role = RenderTapeCommandRole::CopyDestination;
    return true;
  }
  case D9C_COMMAND_RECORD_UPDATE_TEXTURE: {
    D9CCommandChunkWireUpdateTexture value{};
    if (!load(record.payload, 0u, value))
      return false;
    if (value.srcHandleIndex == handleIndex) {
      role = RenderTapeCommandRole::CopySource;
      return true;
    }
    if (value.dstHandleIndex == handleIndex) {
      role = RenderTapeCommandRole::CopyDestination;
      return true;
    }
    return true;
  }
  case D9C_COMMAND_RECORD_UPDATE_SURFACE: {
    D9CCommandChunkWireUpdateSurface value{};
    if (!load(record.payload, 0u, value))
      return false;
    if (value.srcHandleIndex == handleIndex) {
      role = RenderTapeCommandRole::CopySource;
      return true;
    }
    if (value.dstHandleIndex == handleIndex) {
      role = RenderTapeCommandRole::CopyDestination;
      return true;
    }
    return true;
  }
  case D9C_COMMAND_RECORD_READBACK: {
    D9CCommandChunkWireReadback value{};
    if (!load(record.payload, 0u, value))
      return false;
    if (value.srcHandleIndex == handleIndex) {
      role = RenderTapeCommandRole::ReadbackSource;
      return true;
    }
    if (value.dstHandleIndex == handleIndex) {
      role = RenderTapeCommandRole::CopyDestination;
      return true;
    }
    return true;
  }
  case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE: {
    D9CCommandChunkWireReszDepthResolve value{};
    if (!load(record.payload, 0u, value))
      return false;
    if (value.msaaDepthHandleIndex == handleIndex) {
      role = RenderTapeCommandRole::CopySource;
      return true;
    }
    if (value.intzDestHandleIndex == handleIndex) {
      role = RenderTapeCommandRole::CopyDestination;
      return true;
    }
    return true;
  }
  case D9C_COMMAND_RECORD_GENERATE_MIPMAPS: {
    D9CCommandChunkWireGenerateMipmaps value{};
    if (!load(record.payload, 0u, value))
      return false;
    if (value.textureHandleIndex == handleIndex)
      role = RenderTapeCommandRole::CopySource;
    return true;
  }
  default:
    return true;
  }
}

} // namespace

RenderTapeOriginLocator renderTapeLocateOrigin(
    const ImportedChunkView& chunk, std::uint32_t handleIndex,
    const D9CWireObjectIdentity& resolvedIdentity) noexcept {
  RenderTapeOriginLocator result{
      .resolvedIdentity = resolvedIdentity,
      .handleIndex = handleIndex,
  };
  if (handleIndex >= chunk.handles.size()) {
    result.status = RenderTapeOriginLocatorStatus::InvalidHandle;
    return result;
  }

  const auto& handle = chunk.handles[handleIndex];
  result.originIdentity = D9CWireObjectIdentity{
      .kind = handle.kind,
      .generation = handle.generation,
      .objectId = handle.objectId,
  };
  result.aliasOrigin = !sameIdentity(result.originIdentity, resolvedIdentity);

  bool foundOwner = false;
  for (std::size_t index = 0u; index < chunk.records.size(); ++index) {
    const auto record = chunk.record(index);
    if (record.header.type == 0u) {
      result.status = RenderTapeOriginLocatorStatus::MalformedRecord;
      return result;
    }
    if (!recordOwnsHandle(record, handleIndex))
      continue;
    foundOwner = true;
    result.recordIndex = static_cast<std::uint32_t>(index);
    result.recordType = record.header.type;

    if (record.sparseState()) {
      if (record.payload.size() < sizeof(D9CCommandChunkWireDrawHeader)) {
        result.status = RenderTapeOriginLocatorStatus::MalformedRecord;
        return result;
      }
      for (std::size_t sectionIndex = 0u;
           sectionIndex < record.sections.size(); ++sectionIndex) {
        const auto section = record.section(sectionIndex);
        if (section.payload.empty()) {
          result.status = RenderTapeOriginLocatorStatus::MalformedRecord;
          return result;
        }
        std::uint32_t slot = kRenderTapeOriginSentinel;
        bool matched = false;
        if (!sparseSectionBinding(section, handleIndex, slot, matched)) {
          result.status = RenderTapeOriginLocatorStatus::MalformedRecord;
          return result;
        }
        if (!matched)
          continue;
        result.status = RenderTapeOriginLocatorStatus::Accepted;
        result.sectionKind = section.descriptor.kind;
        result.bindingSlot = slot;
        result.role = sparseRole(record.header.type, result.sectionKind);
        result.storageRole =
            renderTapeStorageRoleForCommandRole(result.role);
        return result;
      }
      result.status = RenderTapeOriginLocatorStatus::Accepted;
      return result;
    }

    if (!nonDrawBinding(record, handleIndex, result.sectionKind,
                        result.bindingSlot, result.role)) {
      result.status = RenderTapeOriginLocatorStatus::MalformedRecord;
      return result;
    }
    result.storageRole = renderTapeStorageRoleForCommandRole(result.role);
    result.status = RenderTapeOriginLocatorStatus::Accepted;
    return result;
  }
  result.status = foundOwner ? RenderTapeOriginLocatorStatus::Accepted
                             : RenderTapeOriginLocatorStatus::NotReferenced;
  return result;
}

const char* renderTapeCommandRoleName(RenderTapeCommandRole role) noexcept {
  switch (role) {
  case RenderTapeCommandRole::Unknown:
    return "unknown";
  case RenderTapeCommandRole::BindingOnly:
    return "binding_only";
  case RenderTapeCommandRole::ShaderReadCandidate:
    return "shader_read_candidate";
  case RenderTapeCommandRole::RenderTargetBinding:
    return "render_target_binding";
  case RenderTapeCommandRole::DepthStencilBinding:
    return "depth_stencil_binding";
  case RenderTapeCommandRole::CopySource:
    return "copy_source";
  case RenderTapeCommandRole::CopyDestination:
    return "copy_destination";
  case RenderTapeCommandRole::ReadbackSource:
    return "readback_source";
  }
  return "unknown";
}

const char* renderTapeStorageRoleName(RenderTapeStorageRole role) noexcept {
  switch (role) {
  case RenderTapeStorageRole::Unknown:
    return "unknown";
  case RenderTapeStorageRole::BindingStorage:
    return "binding_storage";
  case RenderTapeStorageRole::ShaderReadCandidate:
    return "shader_read_candidate";
  case RenderTapeStorageRole::RenderTargetCandidate:
    return "render_target_candidate";
  case RenderTapeStorageRole::DepthStencilCandidate:
    return "depth_stencil_candidate";
  case RenderTapeStorageRole::CopySourceCandidate:
    return "copy_source_candidate";
  case RenderTapeStorageRole::CopyDestinationCandidate:
    return "copy_destination_candidate";
  case RenderTapeStorageRole::ReadbackSourceCandidate:
    return "readback_source_candidate";
  }
  return "unknown";
}

const char* renderTapeOriginLocatorStatusName(
    RenderTapeOriginLocatorStatus status) noexcept {
  switch (status) {
  case RenderTapeOriginLocatorStatus::InvalidHandle:
    return "invalid_handle";
  case RenderTapeOriginLocatorStatus::NotReferenced:
    return "not_referenced";
  case RenderTapeOriginLocatorStatus::MalformedRecord:
    return "malformed_record";
  case RenderTapeOriginLocatorStatus::Accepted:
    return "accepted";
  }
  return "unknown";
}

} // namespace dxmt9::d3d9
