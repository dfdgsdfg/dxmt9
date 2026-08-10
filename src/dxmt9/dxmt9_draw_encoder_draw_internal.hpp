#pragma once

// Call-local native state consumed by encodeDraw. Every pointer is a
// non-owning synchronous borrow; no field may escape the encodeDraw call.

#include "dxmt9_draw_encoder.hpp"

#include <cstdint>
#include <limits>
#include <span>

namespace dxmt9::encoders {

struct ActiveEncoderBreakdown;
struct VisibilityScoutPass;

namespace encode_session {
struct ArgbufCbufCache;
struct StreamIbStagingCache;
}  // namespace encode_session

struct DrawNativeShadowView {
  uniform::DirtyState* uniformDirty = nullptr;
  TextureSamplerBindShadow* textureSampler = nullptr;
  ActiveEncoderBreakdown* encoderBreakdown = nullptr;
  encode_session::ArgbufCbufCache* argbufCbufCache = nullptr;
  encode_session::StreamIbStagingCache* streamIbStagingCache = nullptr;
  VisibilityScoutPass* visibilityScout = nullptr;
  core::PsoHandle renderPsoHandle{};
  core::PsoHandle tilePsoHandle{};
  core::DepthStencilHandle depthStencilHandle{};
  std::uint32_t commandIndex =
      std::numeric_limits<std::uint32_t>::max();
  std::uint64_t commandDrawIndex = 0;
  std::uint64_t commandDrawCount = 0;
  bool argbufVsPayloadSourceChanged = false;
  bool argbufPsPayloadSourceChanged = false;
  bool bindingOverridePrefetchedPsoCompatible = false;
};

inline std::uint64_t psoHandleBucket(core::PsoHandle handle) noexcept {
  return handle.valid()
             ? (static_cast<std::uint64_t>(handle.generation) << 32) |
                   static_cast<std::uint64_t>(handle.slot)
             : 0ull;
}

inline std::uint64_t drawStateVertexCbufSourceHash(
    core::FlatDrawStateView drawState) noexcept {
  if (drawState.hasUniformPayload() &&
      drawState.uniformPayload().vertexConstantsHash != 0) {
    return drawState.uniformPayload().vertexConstantsHash;
  }
  return drawState.hot ? drawState.hot->vertexConstantsHash : 0;
}

inline std::uint64_t drawStatePixelCbufSourceHash(
    core::FlatDrawStateView drawState) noexcept {
  if (drawState.hasUniformPayload() &&
      drawState.uniformPayload().pixelConstantsHash != 0) {
    return drawState.uniformPayload().pixelConstantsHash;
  }
  return drawState.hot ? drawState.hot->pixelConstantsHash : 0;
}

bool encodeDraw(EncodeContext& ctx,
                WMT::CommandBuffer& commandBuffer,
                WMT::RenderCommandEncoder& encoder,
                core::FlatDrawStateView drawState,
                std::uint64_t seqId,
                bool skipBaseStateBind,
                const PreUploadedDrawData* preUploaded,
                const core::DrawParam* paramOverride,
                std::span<const std::uint8_t> paramPayloadArena,
                const core::DrawBindingOverride* paramBindingOverride,
                const core::DrawBindingSnapshot* bindingSnapshot,
                bool tileFfpMode,
                bool argbufHybridMode,
                bool argbufResourceArray,
                bool argbufDirectCbufMode,
                bool reopenArgbufHybrid,
                DrawNativeShadowView native);

std::span<const std::uint8_t> drawParamVertexBytes(
    const core::DrawParam& param,
    std::span<const std::uint8_t> arena);
std::span<const std::uint8_t> drawParamIndexBytes(
    const core::DrawParam& param,
    std::span<const std::uint8_t> arena);
bool drawParamBindingOverride(const core::DrawParam& param,
                              std::span<const std::uint8_t> arena,
                              core::DrawBindingOverride& out);
bool drawParamBindingSnapshot(const core::DrawParam& param,
                              std::span<const std::uint8_t> arena,
                              core::DrawBindingSnapshot& out);
void applyDrawBindingOverride(core::FlatDrawStateRecord& hot,
                              core::DrawShaderLayoutContext* shaderLayout,
                              const core::DrawBindingOverride& binding);

}  // namespace dxmt9::encoders
