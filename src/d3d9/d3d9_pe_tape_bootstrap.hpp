#pragma once

#include "d3d9_pe_producer.hpp"

#include <cstdint>

namespace dxmt9::d3d9::pe {

// A capture checkpoint is bounded by the complete shadow domains, not by
// one canonical section. These are record counts, never enlarged section caps.
inline constexpr std::uint32_t kRenderTapeBootstrapMaxRecords =
    1u +
    (kPeRenderStateSlots + D9C_DRAW_PACKET_MAX_RENDER_STATES - 1u) /
        D9C_DRAW_PACKET_MAX_RENDER_STATES +
    (kPeTextureStageSlots * kPeTextureStageStateSlots +
     D9C_DRAW_PACKET_MAX_TSS - 1u) / D9C_DRAW_PACKET_MAX_TSS +
    (kPeSamplerSlots * kPeSamplerStateSlots +
     D9C_DRAW_PACKET_MAX_SAMPLER - 1u) / D9C_DRAW_PACKET_MAX_SAMPLER +
    (kPeTransformSlots + D9C_DRAW_PACKET_MAX_TRANSFORMS - 1u) /
        D9C_DRAW_PACKET_MAX_TRANSFORMS;

enum class RenderTapeBootstrapBuildStatus : std::uint8_t {
  Valid,
  SnapshotRejected,
  AppendRejected,
};

// Cold, capture-only producer for the version-3 bootstrap contract: one
// FULL_SNAPSHOT anchor followed by ordered nonempty scalar-table deltas.
// Private scratch and a private scalar shadow keep the live recorder byte-
// intact. Failure requires discarding the temporary builder; no partial
// checkpoint may be published. The caller reserves the bound above.
RenderTapeBootstrapBuildStatus appendRenderTapeBootstrapState(
    CommandChunkBuilder& builder, const PeHotStateShadow& shadow,
    PeConstShadowBlock& constants, const PeBindingView& bindings) noexcept;

}  // namespace dxmt9::d3d9::pe
