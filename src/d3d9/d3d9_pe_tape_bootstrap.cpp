#include "d3d9_pe_tape_bootstrap.hpp"

#include "device_c_render_tape.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dxmt9::d3d9::pe {

namespace {

template <typename Wire, std::size_t Capacity, typename Visit, typename Set>
bool appendBootstrapTablePages(CommandChunkBuilder& builder,
                               std::uint32_t totalCount,
                               std::array<Wire, Capacity>& scratch,
                               Visit&& visit, Set&& set) noexcept {
  for (std::uint32_t pageIndex = 0u;; ++pageIndex) {
    const auto page = renderTapeBootstrapPageSpan(
        totalCount, static_cast<std::uint32_t>(Capacity), pageIndex);
    if (page.count == 0u) return true;
    std::uint32_t ordinal = 0u;
    std::uint32_t written = 0u;
    visit([&](const Wire& value) noexcept {
      if (ordinal >= page.first && written < page.count) {
        scratch[written++] = value;
      }
      ++ordinal;
    });
    if (ordinal != totalCount || written != page.count) return false;
    SparseStateInput state{};
    set(state, std::span<const Wire>(scratch).first(written));
    if (!appendApplyState(builder, 0u, state)) return false;
  }
}

}  // namespace

RenderTapeBootstrapBuildStatus appendRenderTapeBootstrapState(
    CommandChunkBuilder& builder, const PeHotStateShadow& shadow,
    PeConstShadowBlock& constants, const PeBindingView& bindings) noexcept {
  // Copy only the scalar domains into a zero-table private shadow. Public
  // Transition writes affect this local owner, never the live pending frontier.
  // Bindings (including stream frequency/FVF) remain authoritative in `bindings`.
  PeHotStateShadow anchorShadow{};
  auto scalar = anchorShadow.transition();
  scalar.setViewport(shadow.viewportShadow());
  scalar.setScissor(shadow.scissorShadow());
  scalar.setMaterial(shadow.materialShadow());
  for (std::uint32_t slot = 0u; slot < 6u; ++slot) {
    scalar.setClipPlane(slot, shadow.clipPlaneShadow() + slot * 4u);
  }
  for (std::uint32_t slot = 0u; slot < D9C_DRAW_PACKET_MAX_LIGHTS; ++slot) {
    scalar.setLight(slot, shadow.lightShadow()[slot]);
    scalar.setLightEnable(slot, (shadow.lightEnableShadow() & (1u << slot)) != 0u);
  }

  PeSparseScratch scratch{};
  SparseStateInput state{};
  D9CCommandChunkWireDrawHeader header{};
  if (!buildFullSnapshotState(anchorShadow, constants, bindings, scratch,
                              header, state)) {
    return RenderTapeBootstrapBuildStatus::SnapshotRejected;
  }
  // Ordinary APPLY_STATE follows draw-conditioned index selection. A capture
  // checkpoint must also preserve a clean bound index buffer or explicit null.
  scratch.indexBuffers[0] = {};
  scratch.indexBuffers[0].wire.valid = 1u;
  scratch.indexBuffers[0].object = bindings.indexBuffer;
  state.indexBuffers = scratch.indexBuffers;
  if (!appendApplyState(builder, header.flags, state)) {
    return RenderTapeBootstrapBuildStatus::AppendRejected;
  }

  const bool appended =
      appendBootstrapTablePages(
          builder, shadow.renderStateShadowTyped().size(), scratch.renderStates,
          [&](auto&& append) noexcept {
            shadow.renderStateShadowTyped().forEach(
                [&](RenderStateSlot key, std::uint32_t value) noexcept {
                  append(D9CCommandChunkWireRenderState{rawSlot(key), value});
                });
          },
          [](SparseStateInput& page, auto rows) noexcept { page.renderStates = rows; }) &&
      appendBootstrapTablePages(
          builder, shadow.tssShadowTyped().size(), scratch.textureStageStates,
          [&](auto&& append) noexcept {
            shadow.tssShadowTyped().forEach(
                [&](TextureStageIndex stage, TextureStageStateType type,
                    std::uint32_t value) noexcept {
                  append(D9CDrawPacketTextureStageState{
                      rawSlot(stage), rawSlot(type), value});
                });
          },
          [](SparseStateInput& page, auto rows) noexcept {
            page.textureStageStates = rows;
          }) &&
      appendBootstrapTablePages(
          builder, shadow.samplerStateShadowTyped().size(), scratch.samplerStates,
          [&](auto&& append) noexcept {
            shadow.samplerStateShadowTyped().forEach(
                [&](SamplerIndex sampler, SamplerStateType type,
                    std::uint32_t value) noexcept {
                  append(D9CDrawPacketSamplerState{
                      rawSlot(sampler), rawSlot(type), value});
                });
          },
          [](SparseStateInput& page, auto rows) noexcept { page.samplerStates = rows; }) &&
      appendBootstrapTablePages(
          builder, shadow.transformShadowTyped().size(), scratch.transforms,
          [&](auto&& append) noexcept {
            shadow.transformShadowTyped().forEach(
                [&](TransformState key, const D9CMatrix& value) noexcept {
                  append(D9CDrawPacketTransform{rawSlot(key), 0u, value});
                });
          },
          [](SparseStateInput& page, auto rows) noexcept { page.transforms = rows; });
  return appended ? RenderTapeBootstrapBuildStatus::Valid
                  : RenderTapeBootstrapBuildStatus::AppendRejected;
}

}  // namespace dxmt9::d3d9::pe
