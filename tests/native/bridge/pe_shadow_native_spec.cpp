// pe_shadow_native_spec
//
// Proves d3d9_pe_state_shadow.hpp compiles and behaves natively, with no
// windows.h / d3d9.h in its transitive include set, and pins the mirrored
// D3D9 constants against the literal values they replaced.
//
// The header declares everything at global scope -- there is no namespace to
// alias here.

#include "d3d9_pe_state_shadow.hpp"

#include <cstdint>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

struct TestFailure : std::runtime_error {
  explicit TestFailure(std::string message)
      : std::runtime_error(std::move(message)) {}
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

dxmt9::d3d9::pe::AppendPlan acceptedAppend() {
  return dxmt9::d3d9::pe::settleRecorderAppend({
      .phase = dxmt9::d3d9::pe::AppendSettlement::Prepared,
      .appendSucceeded = true,
  });
}

dxmt9::d3d9::pe::AppendPlan failedAppend() {
  return dxmt9::d3d9::pe::settleRecorderAppend({
      .phase = dxmt9::d3d9::pe::AppendSettlement::Prepared,
      .appendSucceeded = false,
  });
}

void transformSlotsMatchD3DConstants() {
  std::uint32_t slot = 0;

  // D3DTS_VIEW == 2, D3DTS_PROJECTION == 3 (d3d9types.h:1201-1202).
  check(FixedTransformTable::slotForState(2u, slot), "D3DTS_VIEW must map");
  check(slot == 0u, "D3DTS_VIEW is slot 0");
  check(FixedTransformTable::slotForState(3u, slot),
        "D3DTS_PROJECTION must map");
  check(slot == 1u, "D3DTS_PROJECTION is slot 1");

  // D3DTS_TEXTURE0 == 16 .. D3DTS_TEXTURE7 == 23 (d3d9types.h:1203-1210).
  check(FixedTransformTable::slotForState(16u, slot),
        "D3DTS_TEXTURE0 must map");
  check(slot == kPeTransformTextureBaseSlot,
        "D3DTS_TEXTURE0 is the texture transform base slot");
  check(FixedTransformTable::slotForState(23u, slot),
        "D3DTS_TEXTURE7 must map");
  check(slot == kPeTransformTextureBaseSlot + 7u,
        "texture transform slots are contiguous");

  // D3DTS_WORLD == D3DTS_WORLDMATRIX(0) == 256 (d3d9types.h:102).
  check(FixedTransformTable::slotForState(256u, slot), "D3DTS_WORLD must map");
  check(slot == kPeTransformWorldBaseSlot,
        "D3DTS_WORLD is the world matrix base slot");
  check(FixedTransformTable::slotForState(259u, slot),
        "D3DTS_WORLDMATRIX(3) must map");
  check(slot == kPeTransformWorldBaseSlot + 3u,
        "world matrix slots are contiguous");

  for (const std::uint32_t state : {2u, 3u, 16u, 23u, 256u, 259u}) {
    check(FixedTransformTable::slotForState(state, slot), "state must map");
    check(FixedTransformTable::stateForSlot(slot) == state,
          "transform slot must round-trip to its state id");
  }
}

void vertexTextureSamplerSlotsMatchD3DConstants() {
  std::uint32_t slot = 0;

  // Vertex texture samplers land ABOVE the fragment sampler block:
  // slot = kPeFragmentSamplerSlots + (sampler - D3DVERTEXTEXTURESAMPLER0).
  // See d3d9_pe_state_shadow.hpp's vertexTextureSamplerSlot(). They are NOT
  // slots 0..3.
  check(!vertexTextureSamplerSlot(256u, slot),
        "D3DDMAPSAMPLER (256) is not a vertex texture sampler");
  check(vertexTextureSamplerSlot(257u, slot),
        "D3DVERTEXTEXTURESAMPLER0 must map");
  check(slot == kPeFragmentSamplerSlots,
        "D3DVERTEXTEXTURESAMPLER0 sits just above the fragment sampler block");
  check(vertexTextureSamplerSlot(260u, slot),
        "D3DVERTEXTEXTURESAMPLER3 must map");
  check(slot == kPeFragmentSamplerSlots + 3u,
        "D3DVERTEXTEXTURESAMPLER3 is three above it");
  check(!vertexTextureSamplerSlot(261u, slot),
        "261 is past D3DVERTEXTEXTURESAMPLER3");
}

void pendingMasksAreThirtyTwoBitUnsigned() {
  PeHotStateShadow shadow{};
  check(!shadow.hasPendingHotState(), "a fresh shadow has nothing pending");

  // The pending masks were DWORD. After the change they must still hold all
  // 32 bits without sign extension.
  shadow.writer().pendingTextureMask() = 0xFFFFFFFFu;
  check(shadow.pendingTextureMask() == 0xFFFFFFFFu,
        "pendingTextureMask must hold 32 bits unsigned");
  check(shadow.hasPendingHotState(), "a set mask must be pending");

  shadow.consume().clearPendingHotState();
  check(!shadow.hasPendingHotState(),
        "clearPendingHotState must clear the mask");
  check(shadow.pendingTextureMask() == 0u, "mask must be zero after clear");
}

void oversizedBatchFailureIsNonConsuming() {
  PeHotStateShadow shadow{};
  auto pending = shadow.writer().pendingRenderStatesTyped();
  constexpr std::uint32_t total =
      D9C_DRAW_PACKET_MAX_RENDER_STATES + 1u;
  for (std::uint32_t state = 0u; state < total; ++state) {
    pending.set(renderStateSlotKey(state), state + 100u);
  }

  std::array<D9CCommandChunkWireRenderState,
             D9C_DRAW_PACKET_MAX_RENDER_STATES> batch{};
  const std::size_t prepared = shadow.prepareRenderStateBatch(batch);
  check(prepared == batch.size(), "oversized preparation fills one wire batch");
  check(pending.size() == total,
        "injected append rejection must leave every prepared row pending");
  for (std::size_t i = 0u; i < prepared; ++i) {
    check(batch[i].state == i && batch[i].value == i + 100u,
          "prepared oversized rows stay in canonical key order");
  }

  check(!shadow.consume().acceptRenderStateBatch(
            std::span(batch).first(prepared), failedAppend()) &&
            pending.size() == total,
        "failed settlement consumes no render-state rows");

  shadow.consume().acceptRenderStateBatch(
      std::span(batch).first(prepared), acceptedAppend());
  check(pending.size() == 1u,
        "acceptance consumes exactly the represented oversized rows");
  std::uint32_t remaining = 0u;
  check(pending.get(renderStateSlotKey(total - 1u), remaining) &&
            remaining == total - 1u + 100u,
        "unrepresented oversized tail row remains retryable");
}

void allTypedBatchAdaptersAreExact() {
  {
    PeHotStateShadow shadow{};
    auto pending = shadow.writer().pendingTssTyped();
    constexpr std::uint32_t total = D9C_DRAW_PACKET_MAX_TSS + 1u;
    for (std::uint32_t i = 0u; i < total; ++i) {
      pending.set(textureStageIndexKey(i / kPeTextureStageStateSlots),
                  textureStageStateTypeKey(i % kPeTextureStageStateSlots),
                  1000u + i);
    }
    std::array<D9CDrawPacketTextureStageState,
               D9C_DRAW_PACKET_MAX_TSS> batch{};
    const std::size_t count = shadow.prepareTextureStageStateBatch(batch);
    check(count == batch.size(), "TSS prepare fills one capped batch");
    for (std::size_t i = 1u; i < count; ++i) {
      check(std::pair(batch[i - 1u].stage, batch[i - 1u].type) <
                std::pair(batch[i].stage, batch[i].type),
            "TSS prepare order is canonical");
    }
    check(!shadow.consume().acceptTextureStageStateBatch(
              std::span(batch).first(count), failedAppend()) &&
              pending.size() == total,
          "failed TSS settlement is non-consuming");
    check(shadow.consume().acceptTextureStageStateBatch(
              std::span(batch).first(count), acceptedAppend()) &&
              pending.size() == 1u,
          "accepted TSS settlement preserves one oversized tail");
  }

  {
    PeHotStateShadow shadow{};
    auto pending = shadow.writer().pendingSamplerStatesTyped();
    constexpr std::uint32_t total = D9C_DRAW_PACKET_MAX_SAMPLER + 1u;
    for (std::uint32_t i = 0u; i < total; ++i) {
      pending.set(SamplerIndex::fromRaw(i / kPeSamplerStateSlots),
                  SamplerStateType::fromRaw(i % kPeSamplerStateSlots),
                  2000u + i);
    }
    std::array<D9CDrawPacketSamplerState,
               D9C_DRAW_PACKET_MAX_SAMPLER> batch{};
    const std::size_t count = shadow.prepareSamplerStateBatch(batch);
    check(count == batch.size(), "sampler prepare fills one capped batch");
    for (std::size_t i = 1u; i < count; ++i) {
      check(std::pair(batch[i - 1u].sampler, batch[i - 1u].type) <
                std::pair(batch[i].sampler, batch[i].type),
            "sampler prepare order is canonical");
    }
    check(!shadow.consume().acceptSamplerStateBatch(
              std::span(batch).first(count), failedAppend()) &&
              pending.size() == total,
          "failed sampler settlement is non-consuming");
    check(shadow.consume().acceptSamplerStateBatch(
              std::span(batch).first(count), acceptedAppend()) &&
              pending.size() == 1u,
          "accepted sampler settlement preserves one oversized tail");
  }

  {
    PeHotStateShadow shadow{};
    auto pending = shadow.writer().pendingTransformsTyped();
    constexpr std::uint32_t total = D9C_DRAW_PACKET_MAX_TRANSFORMS + 1u;
    for (std::uint32_t i = 0u; i < total; ++i) {
      D9CMatrix matrix{};
      matrix.m[0] = static_cast<float>(3000u + i);
      pending.set(transformStateKey(FixedTransformTable::stateForSlot(i)),
                  matrix);
    }
    std::array<D9CDrawPacketTransform,
               D9C_DRAW_PACKET_MAX_TRANSFORMS> batch{};
    const std::size_t count = shadow.prepareTransformBatch(batch);
    check(count == batch.size(), "transform prepare fills one capped batch");
    for (std::size_t i = 1u; i < count; ++i) {
      std::uint32_t priorSlot = 0u;
      std::uint32_t nextSlot = 0u;
      check(FixedTransformTable::slotForState(batch[i - 1u].state,
                                               priorSlot) &&
                FixedTransformTable::slotForState(batch[i].state, nextSlot) &&
                priorSlot < nextSlot,
            "transform prepare order is canonical slot order");
    }
    check(!shadow.consume().acceptTransformBatch(
              std::span(batch).first(count), failedAppend()) &&
              pending.size() == total,
          "failed transform settlement is non-consuming");
    check(shadow.consume().acceptTransformBatch(
              std::span(batch).first(count), acceptedAppend()) &&
              pending.size() == 1u,
          "accepted transform settlement preserves one oversized tail");
  }
}

void malformedAcceptedBatchIsFailClosedAndNonConsuming() {
  {
    PeHotStateShadow shadow{};
    auto pending = shadow.writer().pendingRenderStatesTyped();
    pending.set(renderStateSlotKey(7u), 17u);
    const std::array<D9CCommandChunkWireRenderState, 2u> accepted{{
        {7u, 17u},
        {static_cast<std::uint32_t>(kPeRenderStateSlots), 99u},
    }};
    check(!shadow.consume().acceptRenderStateBatch(accepted,
                                                    acceptedAppend()) &&
              pending.size() == 1u,
          "one invalid render-state key rejects the whole accepted batch");
  }

  {
    PeHotStateShadow shadow{};
    auto pending = shadow.writer().pendingTssTyped();
    pending.set(textureStageIndexKey(0u), textureStageStateTypeKey(1u), 21u);
    const std::array<D9CDrawPacketTextureStageState, 2u> accepted{{
        {0u, 1u, 21u},
        {static_cast<std::uint32_t>(kPeTextureStageSlots), 1u, 99u},
    }};
    check(!shadow.consume().acceptTextureStageStateBatch(
              accepted, acceptedAppend()) &&
              pending.size() == 1u,
          "one invalid TSS key rejects the whole accepted batch");
  }

  {
    PeHotStateShadow shadow{};
    auto pending = shadow.writer().pendingSamplerStatesTyped();
    pending.set(SamplerIndex::fromRaw(0u), SamplerStateType::fromRaw(1u),
                31u);
    const std::array<D9CDrawPacketSamplerState, 2u> accepted{{
        {0u, 1u, 31u},
        {static_cast<std::uint32_t>(kPeSamplerSlots), 1u, 99u},
    }};
    check(!shadow.consume().acceptSamplerStateBatch(accepted,
                                                     acceptedAppend()) &&
              pending.size() == 1u,
          "one invalid sampler key rejects the whole accepted batch");
  }

  {
    PeHotStateShadow shadow{};
    auto pending = shadow.writer().pendingTransformsTyped();
    D9CMatrix matrix{};
    matrix.m[0] = 41.0f;
    pending.set(transformStateKey(2u), matrix);
    const std::array<D9CDrawPacketTransform, 2u> accepted{{
        {2u, 0u, matrix},
        {999u, 0u, matrix},
    }};
    check(!shadow.consume().acceptTransformBatch(accepted,
                                                  acceptedAppend()) &&
              pending.size() == 1u,
          "one invalid transform key rejects the whole accepted batch");
  }
}

void scalarWriterAbaRetryPreservesLatestValue() {
  using dxmt9::d3d9::pe::RecorderPhase;
  using dxmt9::d3d9::pe::StateWriteKind;
  using dxmt9::d3d9::pe::StateWriteFacts;
  using dxmt9::d3d9::pe::WriteOrigin;
  check(dxmt9::d3d9::pe::planRecorderStateWrite(
            StateWriteFacts{RecorderPhase::Live, WriteOrigin::ExplicitSet,
                             true, false, false})
                .kind() == StateWriteKind::QueueDelta,
        "scalar transition plan queues a changed live value");
  check(dxmt9::d3d9::pe::planRecorderStateWrite(
            StateWriteFacts{RecorderPhase::Live, WriteOrigin::ExplicitSet,
                             true, true, true})
                .kind() == StateWriteKind::RetainPending,
        "scalar transition plan retains an already-pending equal value");
  PeHotStateShadow shadow{};
  const D9CViewport a{1u, 2u, 640u, 480u, 0.0f, 1.0f};
  const D9CViewport b{3u, 4u, 800u, 600u, 0.1f, 0.9f};
  const D9CViewport c{5u, 6u, 1024u, 768u, 0.2f, 0.8f};
  shadow.transition().setViewport(a);
  shadow.transition().setViewport(b);
  check(shadow.viewportShadow().width == b.width &&
            shadow.viewportShadow().height == b.height &&
            shadow.pendingViewport(),
        "scalar A-B writes retain the latest live value and pending bit");
  // A failed append leaves the pending projection untouched; a retry can
  // replace it with C before the eventual accepted consume.
  shadow.transition().setViewport(c);
  check(shadow.viewportShadow().width == c.width && shadow.pendingViewport(),
        "retry keeps the newest scalar value pending");
  shadow.consume().acceptViewport();
  check(shadow.viewportShadow().width == c.width &&
            !shadow.pendingViewport(),
        "accepted consume clears only the represented scalar pending bit");
}

int main() {
  try {
    transformSlotsMatchD3DConstants();
    vertexTextureSamplerSlotsMatchD3DConstants();
    pendingMasksAreThirtyTwoBitUnsigned();
    oversizedBatchFailureIsNonConsuming();
    allTypedBatchAdaptersAreExact();
    malformedAcceptedBatchIsFailClosedAndNonConsuming();
    scalarWriterAbaRetryPreservesLatestValue();
  } catch (const TestFailure& failure) {
    std::cerr << "pe_shadow_native_spec FAILED: " << failure.what() << "\n";
    return 1;
  }
  std::cout << "pe_shadow_native_spec OK\n";
  return 0;
}
