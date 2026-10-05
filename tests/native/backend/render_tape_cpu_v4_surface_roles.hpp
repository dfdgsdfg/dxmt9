#pragma once

#include "render_tape_cpu_resource_context.hpp"
#include "render_tape_cpu_semantics.hpp"
#include "device_c_chunk_validate.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace dxmt9::test {

enum class CpuTapeV4SurfaceRoleStatus : std::uint8_t {
  Accepted, InvalidRecord, InvalidSection, InvalidHandle, DuplicateBinding,
  InvalidBaseline, UnsupportedPresent, InvalidRole, RoleNotApplicable, UnknownRole,
  MissingVersion, NativeMismatch,
};

// Unknown is not an implicit null. Known nulls come from an explicit canonical
// valid binding with NULL_HANDLE_INDEX, or a caller-qualified initial baseline.
struct CpuTapeV4SurfaceRoleBinding {
  bool known = false;
  D9CWireObjectIdentity identity{};

  bool null() const noexcept {
    return known && !identity.kind && !identity.generation && !identity.objectId;
  }
  friend bool operator==(const CpuTapeV4SurfaceRoleBinding& a,
                         const CpuTapeV4SurfaceRoleBinding& b) noexcept {
    return a.known == b.known && a.identity.kind == b.identity.kind &&
        a.identity.generation == b.identity.generation &&
        a.identity.objectId == b.identity.objectId;
  }
};

// Value-owned original roles only. This snapshot has no physical command index,
// wrapper, native handle, resource epoch, payload borrow or semantic authority.
// Its owner separately binds it to the authenticated original record locator.
struct CpuTapeV4SurfaceRoles {
  std::array<CpuTapeV4SurfaceRoleBinding, D9C_DRAW_PACKET_MAX_RENDER_TARGETS> renderTargets{};
  CpuTapeV4SurfaceRoleBinding depthStencil{};
  CpuTapeV4SurfaceRoleBinding presentSource{};
  std::uint32_t recordType = 0u;
  friend bool operator==(const CpuTapeV4SurfaceRoles&, const CpuTapeV4SurfaceRoles&) = default;
};

namespace cpu_v4_surface_roles_detail {

inline bool drawRecord(std::uint32_t type) noexcept {
  return type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE ||
      type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE ||
      type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP ||
      type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP;
}

inline CpuTapeV4SurfaceRoleStatus binding(
    const d3d9::ImportedRecordView& record, std::uint32_t index,
    CpuTapeV4SurfaceRoleBinding& out) noexcept {
  using Status = CpuTapeV4SurfaceRoleStatus;
  if (index == D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX) {
    out = {.known = true};
    return Status::Accepted;
  }
  if (index < record.header.firstHandle) return Status::InvalidHandle;
  const auto local = index - record.header.firstHandle;
  if (local >= record.handles.size()) return Status::InvalidHandle;
  const auto& wire = record.handles[local];
  if (wire.kind != D9C_CHUNK_HANDLE_KIND_SURFACE || !wire.generation || !wire.objectId)
    return Status::InvalidHandle;
  out = {.known = true, .identity = {wire.kind, wire.generation, wire.objectId}};
  return Status::Accepted;
}

} // namespace cpu_v4_surface_roles_detail

// Cold canonical-order fold of ONLY surface roles. The caller first admits the
// complete immutable chunk, then visits its bootstrap/body records in order.
// FULL_SNAPSHOT does not reset RT/DS: production sparse replay skips valid==0,
// and absent surface sections preserve the previous binding. Draw sections
// update roles before that draw, exactly like APPLY_STATE sections. No span is
// retained. Every rejection preserves both the fold and the caller's output.
class CpuTapeV4SurfaceRoleFold {
public:
  CpuTapeV4SurfaceRoleFold() = default;
  explicit CpuTapeV4SurfaceRoleFold(const CpuTapeV4SurfaceRoles& initial) noexcept
      : current_(initial), baselineSeeded_(true) {
    current_.presentSource = {};
    current_.recordType = 0u;
  }
  CpuTapeV4SurfaceRoleFold(const CpuTapeV4SurfaceRoleFold&) = delete;
  CpuTapeV4SurfaceRoleFold& operator=(const CpuTapeV4SurfaceRoleFold&) = delete;

  // An explicit cold-fixture baseline, never inferred from observed payloads.
  // The caller separately proves fresh exclusive device ownership and disabled
  // auto-depth. The default swapchain may supply RT0, so RT0 remains unknown.
  // Seeding cannot reset a used fold or override its canonical binding history.
  CpuTapeV4SurfaceRoleStatus seedFreshNoAutoDepthDefaults() noexcept {
    if (baselineSeeded_ || folded_) return CpuTapeV4SurfaceRoleStatus::InvalidBaseline;
    for (std::size_t slot = 1u; slot < current_.renderTargets.size(); ++slot)
      current_.renderTargets[slot] = {.known = true};
    current_.depthStencil = {.known = true};
    baselineSeeded_ = true;
    return CpuTapeV4SurfaceRoleStatus::Accepted;
  }

  CpuTapeV4SurfaceRoleStatus fold(const d3d9::ImportedRecordView& record,
                               CpuTapeV4SurfaceRoles& out) noexcept {
    using Status = CpuTapeV4SurfaceRoleStatus;
    namespace detail = cpu_v4_surface_roles_detail;
    if (!d3d9::recordRule(record.header.type) ||
        record.header.payloadSize != record.payload.size() ||
        record.header.handleCount != record.handles.size()) return Status::InvalidRecord;
    auto candidate = current_;
    candidate.recordType = record.header.type;
    candidate.presentSource = {};
    if (record.sparseState()) {
      if (record.payload.size() < sizeof(D9CCommandChunkWireDrawHeader) ||
          record.drawHeader.sectionCount != record.sections.size() ||
          (record.drawHeader.flags & ~D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT))
        return Status::InvalidRecord;
      std::array<bool, D9C_DRAW_PACKET_MAX_RENDER_TARGETS> seenTargets{};
      bool seenDepth = false;
      std::uint16_t previousKind = 0u;
      for (const auto& section : record.sections) {
        if (section.kind <= previousKind) return Status::InvalidSection;
        previousKind = section.kind;
        if (section.payloadOffset > record.payload.size() ||
            section.byteSize > record.payload.size() - section.payloadOffset)
          return Status::InvalidSection;
        const auto bytes = record.payload.subspan(section.payloadOffset, section.byteSize);
        if (section.kind == D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET) {
          if (!section.count || section.count > candidate.renderTargets.size() ||
              section.elementSize != sizeof(D9CCommandChunkWireRenderTargetBinding) ||
              bytes.size() != section.count * sizeof(D9CCommandChunkWireRenderTargetBinding))
            return Status::InvalidSection;
          for (std::size_t index = 0u; index < section.count; ++index) {
            D9CCommandChunkWireRenderTargetBinding value{};
            std::memcpy(&value, bytes.data() + index * sizeof(value), sizeof(value));
            if (value.slot >= candidate.renderTargets.size() || value.valid > 1u ||
                value.reserved0 || (!value.valid &&
                    value.handleIndex != D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX))
              return Status::InvalidSection;
            if (seenTargets[value.slot]) return Status::DuplicateBinding;
            seenTargets[value.slot] = true;
            if (!value.valid) continue;
            const auto status = detail::binding(record, value.handleIndex,
                candidate.renderTargets[value.slot]);
            if (status != Status::Accepted) return status;
          }
        } else if (section.kind == D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL) {
          if (seenDepth) return Status::DuplicateBinding;
          seenDepth = true;
          if (section.count != 1u ||
              section.elementSize != sizeof(D9CCommandChunkWireDepthStencilBinding) ||
              bytes.size() != sizeof(D9CCommandChunkWireDepthStencilBinding))
            return Status::InvalidSection;
          D9CCommandChunkWireDepthStencilBinding value{};
          std::memcpy(&value, bytes.data(), sizeof(value));
          if (value.valid > 1u || (!value.valid &&
                  value.handleIndex != D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX))
            return Status::InvalidSection;
          if (value.valid) {
            const auto status = detail::binding(record, value.handleIndex, candidate.depthStencil);
            if (status != Status::Accepted) return status;
          }
        }
      }
    } else if (record.header.type == D9C_COMMAND_RECORD_PRESENT) {
      if (record.payload.size() != sizeof(D9CCommandChunkWirePresent)) return Status::InvalidRecord;
      D9CCommandChunkWirePresent present{};
      std::memcpy(&present, record.payload.data(), sizeof(present));
      // V4 role authority requires the explicit captured source. The legacy
      // handle-free Present does not identify a wire surface, even if replay
      // later supplies an implicit swapchain/native fallback.
      if (record.handles.empty() || present.sourceHandleIndex == D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX)
        return Status::UnsupportedPresent;
      const auto status = detail::binding(record, present.sourceHandleIndex, candidate.presentSource);
      if (status != Status::Accepted) return status;
    }
    current_ = candidate;
    current_.presentSource = {};
    current_.recordType = 0u;
    folded_ = true;
    out = candidate;
    return Status::Accepted;
  }

  const CpuTapeV4SurfaceRoles& current() const noexcept { return current_; }

private:
  CpuTapeV4SurfaceRoles current_{};
  bool baselineSeeded_ = false, folded_ = false;
};

// Role identity chooses an exact historical wire version; the native key only
// checks its actual payload association. Other wire aliases sharing that native
// surface remain preserved, never merged or picked arbitrarily. Acceptance is
// not unique-native-wire authority or proof of attachment pixels/lifetime.
// Explicit null acceptance replaces out with null; every rejection preserves it.
inline CpuTapeV4SurfaceRoleStatus resolveCpuTapeV4SurfaceRole(
    const CpuTapeV4SurfaceRoles& roles, const CpuTapeResourceContext& context,
    CpuTapeResourceRole role, core::Handle actual,
    std::shared_ptr<const CpuTapeResourceVersion>& out) noexcept {
  using Status = CpuTapeV4SurfaceRoleStatus;
  namespace detail = cpu_v4_surface_roles_detail;
  const CpuTapeV4SurfaceRoleBinding* binding = nullptr;
  switch (role.kind) {
  case CpuTapeResourceRoleKind::RenderTarget:
    if (role.slot >= roles.renderTargets.size()) return Status::InvalidRole;
    if (!detail::drawRecord(roles.recordType) && roles.recordType != D9C_COMMAND_RECORD_CLEAR)
      return Status::RoleNotApplicable;
    binding = &roles.renderTargets[role.slot];
    break;
  case CpuTapeResourceRoleKind::DepthStencil:
    if (role.slot) return Status::InvalidRole;
    if (!detail::drawRecord(roles.recordType) && roles.recordType != D9C_COMMAND_RECORD_CLEAR)
      return Status::RoleNotApplicable;
    binding = &roles.depthStencil;
    break;
  case CpuTapeResourceRoleKind::PresentSource:
    if (role.slot) return Status::InvalidRole;
    if (roles.recordType != D9C_COMMAND_RECORD_PRESENT) return Status::RoleNotApplicable;
    binding = &roles.presentSource;
    break;
  default: return Status::InvalidRole;
  }
  if (!binding->known) return Status::UnknownRole;
  if (binding->null()) {
    if (actual) return Status::NativeMismatch;
    out.reset();
    return Status::Accepted;
  }
  const auto version = context.findIdentity(binding->identity);
  if (!version) return Status::MissingVersion;
  if (!actual || version->nativeKind() != D9C_CHUNK_HANDLE_KIND_SURFACE ||
      version->nativeHandle() != actual) return Status::NativeMismatch;
  out = version;
  return Status::Accepted;
}

} // namespace dxmt9::test
