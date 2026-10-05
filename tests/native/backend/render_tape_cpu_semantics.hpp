#pragma once

#include "render_tape_cpu_binding_selection.hpp"

#include "dxmt9/dxmt9_draw_encoder_draw_internal.hpp"
#include "dxmt9/dxmt9_draw_state.hpp"
#include "dxmt9/dxmt9_source_payload.hpp"
#include "dxmt9/device_c.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace dxmt9::test {

enum class CpuTapeResourceRoleKind : std::uint8_t {
  General, RenderTarget, DepthStencil, PresentSource,
};

// Payload role is a selector into an independently authenticated original
// context, not wire identity or proof that native aliases are interchangeable.
struct CpuTapeResourceRole {
  CpuTapeResourceRoleKind kind = CpuTapeResourceRoleKind::General;
  std::uint32_t slot = 0u;
  friend constexpr bool operator==(const CpuTapeResourceRole&, const CpuTapeResourceRole&) = default;
};

enum class CpuTapeBufferRevisionSource : std::uint8_t {
  BindingSnapshot, LiveBinding,
};

enum class CpuTapeSemanticProjection : std::uint8_t {
  Legacy, LogicalV4,
};

namespace detail {

inline constexpr std::uint64_t kCpuTapeFnvOffset = 1469598103934665603ull;
inline constexpr std::uint64_t kCpuTapeFnvPrime = 1099511628211ull;

inline void cpuTapeDigestBytes(std::uint64_t& digest, const void* data,
                               std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::byte*>(data);
  for (std::size_t index = 0; index < size; ++index) {
    digest ^= static_cast<std::uint8_t>(bytes[index]);
    digest *= kCpuTapeFnvPrime;
  }
}

template <typename T>
inline void cpuTapeDigestScalar(std::uint64_t& digest, const T value) noexcept {
  static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>,
                "CPU tape semantic digest accepts scalar values only");
  cpuTapeDigestBytes(digest, &value, sizeof(value));
}

inline void cpuTapeDigestRect(std::uint64_t& digest,
                              const core::Rect& value) noexcept {
  cpuTapeDigestScalar(digest, value.left);
  cpuTapeDigestScalar(digest, value.top);
  cpuTapeDigestScalar(digest, value.right);
  cpuTapeDigestScalar(digest, value.bottom);
}

inline void cpuTapeDigestViewport(std::uint64_t& digest,
                                  const core::Viewport& value) noexcept {
  cpuTapeDigestScalar(digest, value.x);
  cpuTapeDigestScalar(digest, value.y);
  cpuTapeDigestScalar(digest, value.width);
  cpuTapeDigestScalar(digest, value.height);
  cpuTapeDigestScalar(digest, value.minZ);
  cpuTapeDigestScalar(digest, value.maxZ);
}

inline void cpuTapeDigestColor(std::uint64_t& digest,
                               const core::ColorRGBA& value) noexcept {
  cpuTapeDigestScalar(digest, value.r);
  cpuTapeDigestScalar(digest, value.g);
  cpuTapeDigestScalar(digest, value.b);
  cpuTapeDigestScalar(digest, value.a);
}

template <typename T>
inline void cpuTapeDigestScalarArray(std::uint64_t& digest,
                                     const T& values) noexcept {
  for (const auto& value : values) {
    cpuTapeDigestScalar(digest, value);
  }
}

inline void cpuTapeDigestMatrix(std::uint64_t& digest,
                                const core::Matrix4x4& value) noexcept {
  cpuTapeDigestScalarArray(digest, value.m);
}

template <std::size_t Count>
inline void cpuTapeDigestStateSet(
    std::uint64_t& digest,
    const core::FlatStateSet<Count>& set) noexcept {
  cpuTapeDigestScalar(digest, set.count);
  cpuTapeDigestScalar(digest, set.overflow);
  const auto count = set.count <= set.entries.size() ? set.count
                                                      : set.entries.size();
  for (std::size_t index = 0; index < count; ++index) {
    cpuTapeDigestScalar(digest, set.entries[index].state);
    cpuTapeDigestScalar(digest, set.entries[index].value);
  }
}

inline void cpuTapeDigestRenderStates(
    std::uint64_t& digest, const core::FlatRenderStateSet& set,
    CpuTapeSemanticProjection projection) noexcept {
  if (projection != CpuTapeSemanticProjection::LogicalV4) {
    cpuTapeDigestStateSet(digest, set);
    return;
  }
  const auto count = set.count <= set.entries.size() ? set.count : set.entries.size();
  auto retainedCount = set.count;
  for (std::size_t index = 0u; index < count; ++index) {
    if (core::renderStateIsAlphaTestTrio(set.entries[index].state)) --retainedCount;
  }
  // FsVolatile already serializes this draw's effective alpha-test state,
  // including per-draw overrides and absent/default register equivalence.
  // LogicalV4 retains effective execution state, not the raw trio's presence
  // or the first draw's shared-run alpha representation. No other register
  // or overflow signal is projected away.
  cpuTapeDigestScalar(digest, retainedCount);
  cpuTapeDigestScalar(digest, set.overflow);
  for (std::size_t index = 0u; index < count; ++index) {
    if (core::renderStateIsAlphaTestTrio(set.entries[index].state)) continue;
    cpuTapeDigestScalar(digest, set.entries[index].state);
    cpuTapeDigestScalar(digest, set.entries[index].value);
  }
}

template <std::size_t Count>
inline void cpuTapeDigestStateTable(
    std::uint64_t& digest,
    const core::StateValueTable<Count>& table) noexcept {
  cpuTapeDigestScalar(digest, table.count);
  cpuTapeDigestScalarArray(digest, table.occupied);
  for (std::size_t index = 0; index < Count; ++index) {
    if (table.contains(static_cast<core::u32>(index))) {
      cpuTapeDigestScalar(digest, static_cast<core::u32>(index));
      cpuTapeDigestScalar(digest, table.values[index]);
    }
  }
}

inline void cpuTapeDigestAttachment(std::uint64_t& digest,
                                    const core::RenderTargetAttachment& value) noexcept {
  cpuTapeDigestScalar(digest, value.level);
  cpuTapeDigestScalar(digest, value.sampleCount);
}

template <typename ResourceFn>
inline bool cpuTapeResource(ResourceFn& resourceFn, std::uint32_t kind,
                            core::Handle handle, std::uint64_t& digest,
                            std::string& failure, CpuTapeResourceRole role = {}) {
  if (!handle) {
    if constexpr (std::is_invocable_r_v<bool, ResourceFn&, std::uint32_t,
                      core::Handle, std::uint64_t&, CpuTapeResourceRole>) {
      // A missing role-qualified surface must still be checked against the
      // original slot; the legacy/general zero marker cannot certify absence.
      if (role.kind != CpuTapeResourceRoleKind::General) {
        if (std::invoke(resourceFn, kind, handle, digest, role)) return true;
        failure = "resource-normalization-failed";
        return false;
      }
    }
    cpuTapeDigestScalar(digest, std::uint64_t{0});
    return true;
  }
  if constexpr (std::is_invocable_r_v<bool, ResourceFn&, std::uint32_t,
                    core::Handle, std::uint64_t&, CpuTapeResourceRole>) {
    if (std::invoke(resourceFn, kind, handle, digest, role)) return true;
  } else {
    if (std::invoke(resourceFn, kind, handle, digest)) return true;
  }
  failure = "resource-normalization-failed";
  return false;
}

template <typename ResourceFn>
inline bool cpuTapeAttachment(ResourceFn& resourceFn,
                              const core::RenderTargetAttachment& attachment,
                              std::uint64_t& digest, std::string& failure,
                              CpuTapeResourceRole role = {}) {
  cpuTapeDigestAttachment(digest, attachment);
  return cpuTapeResource(resourceFn, D9C_CHUNK_HANDLE_KIND_SURFACE,
                         attachment.handle, digest, failure, role);
}

inline bool cpuTapeDigestBindingSnapshot(
    std::uint64_t& digest, const core::DrawBufferBindingSnapshot& snapshot,
    std::string& failure) {
  if (snapshot.byteSize > std::numeric_limits<std::size_t>::max()) {
    failure = "binding-snapshot-size-overflow";
    return false;
  }
  const auto size = static_cast<std::size_t>(snapshot.byteSize);
  if (size != 0 && (!snapshot.valid() || snapshot.contentsAddress == 0)) {
    failure = "binding-snapshot-bytes-unavailable";
    return false;
  }
  if (size != 0 &&
      snapshot.contentsAddress >
          std::numeric_limits<std::uintptr_t>::max() - (size - 1u)) {
    failure = "binding-snapshot-address-overflow";
    return false;
  }
  // `contentsAddress` is only a borrowed source address. It is deliberately
  // never included in the digest; only the captured bytes are semantic.
  cpuTapeDigestScalar(digest, snapshot.byteSize);
  if (size != 0) {
    cpuTapeDigestBytes(
        digest,
        reinterpret_cast<const void*>(static_cast<std::uintptr_t>(
            snapshot.contentsAddress)),
        size);
  }
  return true;
}

inline void cpuTapeDigestVertexConstants(
    std::uint64_t& digest, const core::VertexShaderConstants& constants) noexcept {
  for (const auto& row : constants.float4) cpuTapeDigestScalarArray(digest, row);
  for (const auto& row : constants.int4) cpuTapeDigestScalarArray(digest, row);
  cpuTapeDigestScalarArray(digest, constants.bools);
}

inline void cpuTapeDigestPixelConstants(
    std::uint64_t& digest, const core::PixelShaderConstants& constants) noexcept {
  for (const auto& row : constants.float4) cpuTapeDigestScalarArray(digest, row);
  for (const auto& row : constants.int4) cpuTapeDigestScalarArray(digest, row);
  cpuTapeDigestScalarArray(digest, constants.bools);
}

inline void cpuTapeDigestMaterial(std::uint64_t& digest,
                                  const core::Material& material) noexcept {
  cpuTapeDigestColor(digest, material.emissive);
  cpuTapeDigestColor(digest, material.ambient);
  cpuTapeDigestColor(digest, material.diffuse);
  cpuTapeDigestColor(digest, material.specular);
  cpuTapeDigestScalar(digest, material.power);
}

inline void cpuTapeDigestLight(std::uint64_t& digest,
                               const core::Light& light) noexcept {
  cpuTapeDigestScalar(digest, light.type);
  cpuTapeDigestScalar(digest, light.enabled);
  cpuTapeDigestColor(digest, light.diffuse);
  cpuTapeDigestColor(digest, light.specular);
  cpuTapeDigestColor(digest, light.ambient);
  cpuTapeDigestScalarArray(digest, light.position);
  cpuTapeDigestScalarArray(digest, light.direction);
  cpuTapeDigestScalar(digest, light.range);
  cpuTapeDigestScalar(digest, light.falloff);
  cpuTapeDigestScalar(digest, light.attenuation0);
  cpuTapeDigestScalar(digest, light.attenuation1);
  cpuTapeDigestScalar(digest, light.attenuation2);
  cpuTapeDigestScalar(digest, light.theta);
  cpuTapeDigestScalar(digest, light.phi);
}

inline void cpuTapeDigestUniforms(std::uint64_t& digest,
                                  const core::DrawUniformPayload& uniforms,
                                  CpuTapeSemanticProjection projection =
                                      CpuTapeSemanticProjection::Legacy) noexcept {
  cpuTapeDigestVertexConstants(digest, uniforms.vsConst);
  cpuTapeDigestPixelConstants(digest, uniforms.psConst);
  cpuTapeDigestMatrix(digest, uniforms.worldViewProj);
  cpuTapeDigestMatrix(digest, uniforms.ffpView);
  cpuTapeDigestMatrix(digest, uniforms.ffpWorldView);
  cpuTapeDigestMatrix(digest, uniforms.ffpNormalMatrix);
  cpuTapeDigestMaterial(digest, uniforms.material);
  for (const auto& light : uniforms.lights) cpuTapeDigestLight(digest, light);
  for (const auto& matrix : uniforms.ffpBlendWorldViewProj) {
    cpuTapeDigestMatrix(digest, matrix);
  }
  for (const auto& matrix : uniforms.ffpBlendWorldView) {
    cpuTapeDigestMatrix(digest, matrix);
  }
  for (const auto& matrix : uniforms.ffpBlendNormalMatrix) {
    cpuTapeDigestMatrix(digest, matrix);
  }
  for (const auto& matrix : uniforms.textureTransforms) {
    cpuTapeDigestMatrix(digest, matrix);
  }
  cpuTapeDigestScalar(digest, uniforms.clipPlaneMask);
  for (const auto& plane : uniforms.clipPlanes) {
    cpuTapeDigestScalarArray(digest, plane);
  }
  // The complete resolved constants and fixed values above are authoritative.
  // These four cache hashes depend on the incremental/full construction path;
  // they are redundant metadata, not shader/declaration content identities.
  if (projection != CpuTapeSemanticProjection::LogicalV4) {
    cpuTapeDigestScalar(digest, uniforms.vertexConstantsHash);
    cpuTapeDigestScalar(digest, uniforms.pixelConstantsHash);
    cpuTapeDigestScalar(digest, uniforms.fixedPayloadHash);
    cpuTapeDigestScalar(digest, uniforms.hash);
  }
  cpuTapeDigestScalar(digest, uniforms.vertexFloatConstantCount);
  cpuTapeDigestScalar(digest, uniforms.vertexIntConstantCount);
  cpuTapeDigestScalar(digest, uniforms.vertexBoolConstantCount);
  cpuTapeDigestScalar(digest, uniforms.pixelFloatConstantCount);
  cpuTapeDigestScalar(digest, uniforms.pixelIntConstantCount);
  cpuTapeDigestScalar(digest, uniforms.pixelBoolConstantCount);
}

inline void cpuTapeDigestShaderLayout(
    std::uint64_t& digest, const core::DrawShaderLayoutContext* layout) noexcept {
  cpuTapeDigestScalar(digest, layout != nullptr);
  if (!layout) return;
  cpuTapeDigestScalar(digest, layout->vertexDecl.fvf);
  cpuTapeDigestScalar(digest, layout->vertexDecl.elements.count);
  for (const auto& element : layout->vertexDecl.elements) {
    cpuTapeDigestScalar(digest, element.stream);
    cpuTapeDigestScalar(digest, element.offset);
    cpuTapeDigestScalar(digest, element.type);
    cpuTapeDigestScalar(digest, element.method);
    cpuTapeDigestScalar(digest, element.usage);
    cpuTapeDigestScalar(digest, element.usageIndex);
  }
  // StreamBinding::buffer is a shared native object. The actual hot-state
  // buffer handles are normalized below; only the value fields belong here.
  for (const auto& stream : layout->vertexDecl.streams) {
    cpuTapeDigestScalar(digest, stream.offset);
    cpuTapeDigestScalar(digest, stream.stride);
  }
  cpuTapeDigestScalar(digest, layout->vertexShader.kind);
  cpuTapeDigestScalar(digest, layout->vertexShader.hash);
  cpuTapeDigestScalar(digest, layout->pixelShader.kind);
  cpuTapeDigestScalar(digest, layout->pixelShader.hash);
  cpuTapeDigestScalar(digest, layout->vertexConstantUsage.floatCount);
  cpuTapeDigestScalar(digest, layout->vertexConstantUsage.intCount);
  cpuTapeDigestScalar(digest, layout->vertexConstantUsage.boolCount);
  cpuTapeDigestScalar(digest, layout->vertexConstantUsage.indexedFloat);
  cpuTapeDigestScalar(digest, layout->vertexConstantUsage.indexedInt);
  cpuTapeDigestScalar(digest, layout->vertexConstantUsage.indexedBool);
  cpuTapeDigestScalar(digest, layout->vertexConstantUsage.unknown);
  cpuTapeDigestScalar(digest, layout->pixelConstantUsage.floatCount);
  cpuTapeDigestScalar(digest, layout->pixelConstantUsage.intCount);
  cpuTapeDigestScalar(digest, layout->pixelConstantUsage.boolCount);
  cpuTapeDigestScalar(digest, layout->pixelConstantUsage.indexedFloat);
  cpuTapeDigestScalar(digest, layout->pixelConstantUsage.indexedInt);
  cpuTapeDigestScalar(digest, layout->pixelConstantUsage.indexedBool);
  cpuTapeDigestScalar(digest, layout->pixelConstantUsage.unknown);
  cpuTapeDigestScalar(digest, layout->clipPlaneMask);
}

template <typename ResourceFn>
inline bool cpuTapeDigestHot(ResourceFn& resourceFn,
                             const core::FlatDrawStateRecord& hot,
                             const core::DrawShaderLayoutContext* layout,
                             std::uint64_t& digest, std::string& failure,
                             CpuTapeSemanticProjection projection =
                                 CpuTapeSemanticProjection::Legacy) {
  const auto resource = [&](std::uint32_t kind, core::Handle handle) {
    return cpuTapeResource(resourceFn, kind, handle, digest, failure);
  };

  // Key hashes are the semantic identities for shader and vertex-declaration
  // payloads. Never inspect/hash their native pointers or backing objects.
  cpuTapeDigestScalar(digest, hot.key.vertexElementCount);
  cpuTapeDigestScalar(digest, hot.key.fvf);
  cpuTapeDigestScalar(digest, hot.key.vertexDeclHash);
  cpuTapeDigestScalar(digest, hot.key.vertexShaderKind);
  cpuTapeDigestScalar(digest, hot.key.pixelShaderKind);
  cpuTapeDigestScalar(digest, hot.key.vertexShaderHash);
  cpuTapeDigestScalar(digest, hot.key.pixelShaderHash);
  cpuTapeDigestScalarArray(digest, hot.key.streamOffsets);
  cpuTapeDigestScalarArray(digest, hot.key.streamStrides);
  cpuTapeDigestScalarArray(digest, hot.key.streamFrequencies);
  cpuTapeDigestScalar(digest, hot.key.streamMask);
  cpuTapeDigestScalarArray(digest, hot.key.textureLods);
  cpuTapeDigestScalar(digest, hot.key.textureMask);
  cpuTapeDigestScalar(digest, hot.key.samplerStateMask);
  cpuTapeDigestScalar(digest, hot.key.renderTargetMask);
  cpuTapeDigestScalar(digest, hot.key.nonIdentityTextureTransformStageMask);
  cpuTapeDigestScalar(digest, hot.key.clipPlaneMask);

  cpuTapeDigestScalarArray(digest, hot.streamOffsets);
  cpuTapeDigestScalarArray(digest, hot.streamStrides);
  cpuTapeDigestScalarArray(digest, hot.streamFrequencies);
  cpuTapeDigestScalar(digest, hot.streamMask);
  for (const auto handle : hot.streamBuffers) {
    if (!resource(D9C_CHUNK_HANDLE_KIND_BUFFER, handle)) return false;
  }
  if (!resource(D9C_CHUNK_HANDLE_KIND_BUFFER, hot.indexBuffer)) return false;

  cpuTapeDigestScalarArray(digest, hot.textureLods);
  cpuTapeDigestScalar(digest, hot.textureMask);
  for (const auto& texture : hot.textures) {
    if (!resource(D9C_CHUNK_HANDLE_KIND_TEXTURE, texture)) return false;
  }
  cpuTapeDigestRenderStates(digest, hot.renderStates, projection);
  for (const auto& states : hot.textureStageStates) {
    cpuTapeDigestStateSet(digest, states);
  }
  for (const auto& states : hot.samplerStates) {
    cpuTapeDigestStateSet(digest, states);
  }

  cpuTapeDigestScalar(digest, hot.renderTargetMask);
  for (std::size_t slot = 0u; slot < hot.colorAttachments.size(); ++slot) {
    if (!cpuTapeAttachment(resourceFn, hot.colorAttachments[slot], digest, failure,
            {CpuTapeResourceRoleKind::RenderTarget, static_cast<std::uint32_t>(slot)})) return false;
  }
  cpuTapeDigestAttachment(digest, hot.depthStencil);
  if (!cpuTapeResource(resourceFn, D9C_CHUNK_HANDLE_KIND_SURFACE, hot.depthStencil.handle,
          digest, failure, {CpuTapeResourceRoleKind::DepthStencil, 0u})) {
    return false;
  }
  cpuTapeDigestViewport(digest, hot.viewport.viewport);
  cpuTapeDigestRect(digest, hot.viewport.scissor);
  cpuTapeDigestScalar(digest, hot.viewport.scissorEnabled);
  cpuTapeDigestScalar(digest, hot.nonIdentityTextureTransformStageMask);
  cpuTapeDigestScalar(digest, hot.clipPlaneMask);
  cpuTapeDigestShaderLayout(digest, layout);
  return true;
}

template <typename ResourceFn>
inline bool cpuTapeDigestClear(ResourceFn& resourceFn,
                               const core::ClearCommandView& clear,
                               std::uint64_t& digest, std::string& failure) {
  cpuTapeDigestScalar(digest, clear.clearColor);
  cpuTapeDigestScalar(digest, clear.clearDepth);
  cpuTapeDigestScalar(digest, clear.clearStencil);
  cpuTapeDigestColor(digest, clear.color);
  cpuTapeDigestScalar(digest, clear.depth);
  cpuTapeDigestScalar(digest, clear.stencil);
  if (clear.clearColor) {
    for (std::size_t slot = 0u; slot < clear.colorAttachments.size(); ++slot) {
      if (!cpuTapeAttachment(resourceFn, clear.colorAttachments[slot], digest, failure,
              {CpuTapeResourceRoleKind::RenderTarget, static_cast<std::uint32_t>(slot)})) return false;
    }
  }
  if (clear.clearDepth || clear.clearStencil) {
    if (!cpuTapeAttachment(resourceFn, clear.depthStencil, digest, failure,
            {CpuTapeResourceRoleKind::DepthStencil, 0u})) {
      return false;
    }
  }
  cpuTapeDigestScalar(digest, clear.rects.size());
  for (const auto& rect : clear.rects) cpuTapeDigestRect(digest, rect);
  return true;
}

template <typename ResourceFn>
inline bool cpuTapeDigestPresent(ResourceFn& resourceFn,
                                 const core::PresentCommandRecord& present,
                                 std::uint64_t& digest, std::string& failure) {
  const auto& swap = present.present;
  const auto source = present.presentSource ? present.presentSource
                                            : swap.sourceSurface;
  cpuTapeDigestScalar(digest, source ? true : false);
  if (!cpuTapeResource(resourceFn, D9C_CHUNK_HANDLE_KIND_SURFACE, source,
                       digest, failure, {CpuTapeResourceRoleKind::PresentSource, 0u})) return false;
  cpuTapeDigestScalar(digest, swap.width);
  cpuTapeDigestScalar(digest, swap.height);
  cpuTapeDigestScalar(digest, swap.format);
  cpuTapeDigestScalar(digest, swap.interval);
  cpuTapeDigestScalar(digest, swap.windowed);
  cpuTapeDigestScalar(digest, swap.backBufferCount);
  cpuTapeDigestScalar(digest, swap.displaySyncEnabled);
  cpuTapeDigestScalar(digest, swap.multiSampleType);
  cpuTapeDigestScalar(digest, swap.gammaRampIsIdentity);
  cpuTapeDigestScalarArray(digest, swap.gammaRamp.red);
  cpuTapeDigestScalarArray(digest, swap.gammaRamp.green);
  cpuTapeDigestScalarArray(digest, swap.gammaRamp.blue);
  cpuTapeDigestScalar(digest, swap.drawableTokenRequired);
  cpuTapeDigestScalar(digest, swap.maxFrameLatency);
  return true;
}

}  // namespace detail

inline constexpr std::size_t kCpuTapeNoSubdraw = std::numeric_limits<std::size_t>::max();

// Observe the actual final source payload used by CPU replay. ResourceFn owns
// native-handle qualification (kind, tape identity, generation, and logical
// content); DrawFn owns draw ordinal/capacity bookkeeping. Both callbacks run
// only on this cold diagnostic path. The optional LocationFn selects cold
// observation context before any digest or semantic callback for an item. It
// receives (physical command index, actual command kind, exact subdraw index),
// with kCpuTapeNoSubdraw for non-draw commands. These are only payload positions,
// not provenance: callers must separately join genuine V4 record origins.
// Payload only needs checked commandCount()/commandAt() projections; accepting
// it by reference also supports a scoped, noncopyable source-payload borrow.
// Optional ContentRevisionFn(buffer, source, physicalRevision, semanticRevision)
// projects only the final revision scalar. V4 may select its original immutable
// logical content epoch; the callback grants no physical-byte authority. Custom
// SnapshotFn/LiveBindingFn must normalize their own bytes/fields independently,
// rather than prehashing a physical revision they intend to replace here.
// Optional RoleResourceFn(kind, handle, digest, role) replaces ResourceFn for
// normalization, after LocationFn selection. RT/DS/Present roles include null
// handles so original-slot absence must be checked explicitly. General null
// handles retain the legacy zero marker/no callback. Roles are not new hashed
// fields. Omitting both seams preserves the legacy field stream and callbacks.
// SemanticProjection defaults to the exact legacy field stream. LogicalV4
// omits only the four redundant uniform cache hashes after serializing all
// resolved uniform values. It also excludes only the redundant raw alpha-test
// render-state trio and its presence/count, after the effective FsVolatile has
// been hashed. Shader/declaration content identities and other states remain.
template <typename Payload, typename ResourceFn, typename DrawFn, typename SnapshotFn = std::nullptr_t,
          typename LiveBindingFn = std::nullptr_t, typename LocationFn = std::nullptr_t,
          typename ContentRevisionFn = std::nullptr_t, typename RoleResourceFn = std::nullptr_t>
bool observeCpuTapePayload(const Payload& payload,
                           std::uint64_t& digest, ResourceFn&& resourceFn,
                           DrawFn&& drawFn, std::string& failure,
                           std::size_t* observedSnapshots = nullptr,
                           SnapshotFn&& snapshotFn = nullptr,
                           std::size_t* observedLiveBindings = nullptr,
                           LiveBindingFn&& liveBindingFn = nullptr,
                           std::size_t* observedMipmaps = nullptr,
                           LocationFn&& locationFn = nullptr,
                           ContentRevisionFn&& contentRevisionFn = nullptr,
                           RoleResourceFn&& roleResourceFn = nullptr,
                           CpuTapeSemanticProjection projection =
                               CpuTapeSemanticProjection::Legacy) {
  const auto resource = [&](std::uint32_t kind, core::Handle handle,
                            std::uint64_t& hash, CpuTapeResourceRole role) {
    if constexpr (!std::is_same_v<std::remove_cvref_t<RoleResourceFn>, std::nullptr_t>) {
      return std::invoke(roleResourceFn, kind, handle, hash, role);
    } else {
      if (!handle) {
        detail::cpuTapeDigestScalar(hash, std::uint64_t{0});
        return true;
      }
      return std::invoke(resourceFn, kind, handle, hash);
    }
  };
  auto&& draw = drawFn;
  const auto selectLocation = [&](std::size_t commandIndex,
                                  core::MetalCommandKind kind, std::size_t subdrawIndex) {
    if constexpr (!std::is_same_v<std::remove_cvref_t<LocationFn>, std::nullptr_t>) {
      if (!std::invoke(locationFn, commandIndex, kind, subdrawIndex)) {
        failure = "payload-location-selection-failed";
        return false;
      }
    }
    return true;
  };
  const auto snapshot = [&](core::Handle buffer,
                            const core::DrawBufferBindingSnapshot& value) {
    if constexpr (std::is_same_v<std::remove_cvref_t<SnapshotFn>, std::nullptr_t>) {
      return detail::cpuTapeDigestBindingSnapshot(digest, value, failure);
    } else {
      if (std::invoke(snapshotFn, buffer, value, digest)) return true;
      failure = "buffer-snapshot-content-mismatch";
      return false;
    }
  };
  const auto liveBinding = [&](core::Handle buffer, std::uint64_t& revision) {
    if constexpr (!std::is_same_v<std::remove_cvref_t<LiveBindingFn>, std::nullptr_t>) {
      if (std::invoke(liveBindingFn, buffer, digest, revision)) return true;
    }
    failure = "missing-or-invalid-live-buffer-binding";
    return false;
  };
  const auto digestRevision = [&](core::Handle buffer, CpuTapeBufferRevisionSource source,
                                  std::uint64_t physicalRevision) {
    auto semanticRevision = physicalRevision;
    if constexpr (!std::is_same_v<std::remove_cvref_t<ContentRevisionFn>, std::nullptr_t>) {
      if (!std::invoke(contentRevisionFn, buffer, source, physicalRevision, semanticRevision)) {
        failure = "buffer-content-revision-projection-failed";
        return false;
      }
    }
    detail::cpuTapeDigestScalar(digest, semanticRevision);
    return true;
  };
  for (std::size_t index = 0; index < payload.commandCount(); ++index) {
    const auto source = payload.commandAt(index);
    const auto& command = source.command;
    if (source.kind() != core::MetalCommandKind::DrawRun &&
        !selectLocation(index, source.kind(), kCpuTapeNoSubdraw)) return false;
    switch (source.kind()) {
    case core::MetalCommandKind::DrawRun: {
      if (!command.drawState.hot) {
        failure = "missing-first-draw-state";
        return false;
      }
      if (command.drawParams.empty()) {
        failure = "empty-draw-run";
        return false;
      }
      core::FlatDrawStateView stateView = command.drawState;
      for (std::size_t subdraw = 0u; subdraw < command.drawParams.size(); ++subdraw) {
        const auto& param = command.drawParams[subdraw];
        const auto validRange = [&](core::DrawPayloadRange range) {
          return !range.size || (range.offset <= command.drawPayloadBytes.size() &&
              range.size <= command.drawPayloadBytes.size() - range.offset);
        };
        // The shared byte accessor returns empty for both absent and invalid
        // ranges. Reject malformed nonempty input before any item context,
        // semantic callback or hash can mistake it for an absent/live binding.
        // Zero size keeps the legacy absent-range meaning regardless of offset.
        if (!validRange(param.userVertexRange)) {
          failure = "malformed-user-vertex-range";
          return false;
        }
        if (!validRange(param.userIndexRange)) {
          failure = "malformed-user-index-range";
          return false;
        }
        if (!validRange(param.bindingOverrideRange)) {
          failure = "malformed-binding-override-range";
          return false;
        }
        if (!validRange(param.bindingSnapshotRange)) {
          failure = "malformed-binding-snapshot-range";
          return false;
        }
        if (!selectLocation(index, source.kind(), subdraw)) return false;
        auto hot = *command.drawState.hot;
        std::optional<core::DrawShaderLayoutContext> shaderLayout;
        if (command.drawState.shaderLayout) {
          shaderLayout = *command.drawState.shaderLayout;
        }
        core::DrawBindingOverride binding{};
        const auto overrideBytes = core::drawPayloadRangeBytes(
            param.bindingOverrideRange, command.drawPayloadBytes);
        if (!overrideBytes.empty() &&
            overrideBytes.size() != sizeof(core::DrawBindingOverride)) {
          failure = "malformed-binding-override";
          return false;
        }
        const bool hasBindingOverride =
            !overrideBytes.empty() &&
            encoders::drawParamBindingOverride(param,
                                               command.drawPayloadBytes,
                                               binding);
        if (hasBindingOverride) {
          encoders::applyDrawBindingOverride(
              hot, shaderLayout ? &*shaderLayout : nullptr, binding);
        }
        stateView.hot = &hot;
        stateView.shaderLayout = shaderLayout ? &*shaderLayout
                                              : command.drawState.shaderLayout;
        const auto volatileState = [&] {
          auto value = state::buildFsVolatile(stateView);
          if (binding.alphaTestStateValid) {
            const auto alpha = state::makeFsVolatile(
                binding.alphaTestEnable, binding.alphaTestFunc,
                binding.alphaTestRef, value.sampleMask);
            value.alphaTest = alpha.alphaTest;
            value.alphaRef = alpha.alphaRef;
          }
          return value;
        }();
        detail::cpuTapeDigestScalar(digest, volatileState.alphaTest);
        detail::cpuTapeDigestScalar(digest, volatileState.alphaRef);
        detail::cpuTapeDigestScalar(digest, volatileState.sampleMask);
        detail::cpuTapeDigestScalar(digest, core::MetalCommandKind::DrawRun);
        detail::cpuTapeDigestScalar(digest, param.primitiveType);
        detail::cpuTapeDigestScalar(digest, param.primitiveCount);
        detail::cpuTapeDigestScalar(digest, param.startVertex);
        detail::cpuTapeDigestScalar(digest, param.baseVertexIndex);
        detail::cpuTapeDigestScalar(digest, param.startIndex);
        detail::cpuTapeDigestScalar(digest, param.indexType);
        detail::cpuTapeDigestScalar(digest, param.indexed);
        detail::cpuTapeDigestScalar(digest, param.instanceCount);
        const auto vertexBytes = core::drawPayloadRangeBytes(
            param.userVertexRange, command.drawPayloadBytes);
        const auto indexBytes = core::drawPayloadRangeBytes(
            param.userIndexRange, command.drawPayloadBytes);
        detail::cpuTapeDigestScalar(digest, vertexBytes.size());
        detail::cpuTapeDigestBytes(digest, vertexBytes.data(), vertexBytes.size());
        detail::cpuTapeDigestScalar(digest, indexBytes.size());
        detail::cpuTapeDigestBytes(digest, indexBytes.data(), indexBytes.size());
        if (!detail::cpuTapeDigestHot(resource, hot, stateView.shaderLayout,
                                      digest, failure, projection)) {
          return false;
        }

        core::DrawBindingSnapshot snapshots{};
        const auto snapshotBytes = core::drawPayloadRangeBytes(
            param.bindingSnapshotRange, command.drawPayloadBytes);
        if (!snapshotBytes.empty() &&
            snapshotBytes.size() != sizeof(core::DrawBindingSnapshot)) {
          failure = "malformed-binding-snapshot";
          return false;
        }
        const bool hasSnapshots =
            !snapshotBytes.empty() &&
            encoders::drawParamBindingSnapshot(param,
                                               command.drawPayloadBytes,
                                               snapshots);
        // Captures are sparse: non-versioned streams/index bindings legally
        // use the live Pool backing. Normalize both representations by the
        // effective draw binding; never fabricate a captured payload.
        detail::cpuTapeDigestScalar(digest, hot.streamMask);
        for (std::size_t stream = 0; stream < hot.streamBuffers.size(); ++stream) {
          const auto buffer = hot.streamBuffers[stream];
          if (!cpuTapeObservesStreamBuffer(static_cast<unsigned>(stream), hot.streamMask,
                                            bool(buffer), !vertexBytes.empty())) continue;
          if (!detail::cpuTapeResource(resource, D9C_CHUNK_HANDLE_KIND_BUFFER,
                                        buffer, digest, failure)) return false;
          detail::cpuTapeDigestScalar(digest, hot.streamOffsets[stream]);
          detail::cpuTapeDigestScalar(digest, hot.streamStrides[stream]);
          std::uint64_t revision = 0u;
          auto revisionSource = CpuTapeBufferRevisionSource::LiveBinding;
          if (hasSnapshots && (snapshots.streamMask & (1u << stream))) {
            const auto& item = snapshots.streams[stream];
            if (item.buffer != buffer || item.offset != hot.streamOffsets[stream] ||
                item.stride != hot.streamStrides[stream]) {
              failure = "captured-stream-binding-mismatch";
              return false;
            }
            if (!snapshot(buffer, item.snapshot)) return false;
            revision = item.snapshot.contentRevision;
            revisionSource = CpuTapeBufferRevisionSource::BindingSnapshot;
            if (observedSnapshots) ++*observedSnapshots;
          } else {
            if (!liveBinding(buffer, revision)) return false;
            if (observedLiveBindings) ++*observedLiveBindings;
          }
          if (!digestRevision(buffer, revisionSource, revision)) return false;
        }
        detail::cpuTapeDigestScalar(digest, param.indexed);
        if (cpuTapeObservesIndexBuffer(param.indexed, bool(hot.indexBuffer), !indexBytes.empty())) {
          if (!detail::cpuTapeResource(resource, D9C_CHUNK_HANDLE_KIND_BUFFER,
                                      hot.indexBuffer, digest, failure)) return false;
          detail::cpuTapeDigestScalar(digest, param.indexType);
          std::uint64_t revision = 0u;
          auto revisionSource = CpuTapeBufferRevisionSource::LiveBinding;
          if (hasSnapshots && snapshots.indexSnapshotValid) {
            if (snapshots.indexBuffer != hot.indexBuffer || snapshots.indexType != param.indexType) {
              failure = "captured-index-binding-mismatch";
              return false;
            }
            if (!snapshot(hot.indexBuffer, snapshots.indexSnapshot)) return false;
            revision = snapshots.indexSnapshot.contentRevision;
            revisionSource = CpuTapeBufferRevisionSource::BindingSnapshot;
            if (observedSnapshots) ++*observedSnapshots;
          } else {
            if (!liveBinding(hot.indexBuffer, revision)) return false;
            if (observedLiveBindings) ++*observedLiveBindings;
          }
          if (!digestRevision(hot.indexBuffer, revisionSource, revision)) return false;
        }

        core::DrawUniformPayload uniformScratch{};
        const auto* uniforms = core::drawRunUniformPayloadForParam(
            command, param, uniformScratch);
        if (!uniforms) {
          failure = "missing-draw-uniforms";
          return false;
        }
        detail::cpuTapeDigestUniforms(digest, *uniforms, projection);
        if (!std::invoke(draw, digest)) {
          failure = "draw-observation-failed";
          return false;
        }
      }
      break;
    }
    case core::MetalCommandKind::Clear: {
      const auto clear = source.clear
                             ? source.clear
                             : command.clear
                                   ? std::optional<core::ClearCommandView>(
                                         core::ClearCommandView{
                                             .colorAttachments =
                                                 command.clear->colorAttachments,
                                             .depthStencil = command.clear->depthStencil,
                                             .clearColor = command.clear->clearColor,
                                             .clearDepth = command.clear->clearDepth,
                                             .clearStencil = command.clear->clearStencil,
                                             .color = command.clear->color,
                                             .depth = command.clear->depth,
                                             .stencil = command.clear->stencil,
                                             .rects = std::span<const core::Rect>(
                                                 command.clear->rects)})
                                   : std::optional<core::ClearCommandView>{};
      if (!clear || !detail::cpuTapeDigestClear(resource, *clear, digest,
                                                failure)) {
        if (failure.empty()) failure = "missing-clear-record";
        return false;
      }
      break;
    }
    case core::MetalCommandKind::GenerateMipmaps:
      if (!command.generateMipmaps || !command.generateMipmaps->texture) {
        failure = "missing-mipmap-texture";
        return false;
      }
      // Ordered logical operation only: no CPU fixture claims that the GPU
      // generated the hidden pyramid or that its texel bytes were observed.
      detail::cpuTapeDigestScalar(digest, core::MetalCommandKind::GenerateMipmaps);
      if (!detail::cpuTapeResource(resource, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                   command.generateMipmaps->texture, digest,
                                   failure)) return false;
      if (observedMipmaps) ++*observedMipmaps;
      break;
    case core::MetalCommandKind::Present:
      if (!command.present ||
          !detail::cpuTapeDigestPresent(resource, *command.present, digest,
                                        failure)) {
        if (failure.empty()) failure = "missing-present-record";
        return false;
      }
      break;
    default:
      failure = "unsupported-command-kind";
      return false;
    }
  }
  return true;
}

}  // namespace dxmt9::test
