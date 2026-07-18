#pragma once

#include "device_c_chunk_v2_validate.hpp"
#include "dxmt9/core_snapshots.hpp"

#include <cstdint>
#include <limits>
#include <span>

namespace dxmt9::d3d9 {

inline constexpr std::int32_t kCommandChunkV2DecodeFailure =
    std::numeric_limits<std::int32_t>::min();

struct ResolvedRecordV2View {
  ImportedRecordV2View wire{};
  std::span<void* const> objects{};

  void* objectForAbsoluteIndex(std::uint32_t index) const noexcept;
};

struct ResolvedChunkV2View {
  ImportedChunkV2View wire{};
  std::span<void* const> objects{};

  ResolvedRecordV2View record(std::size_t index) const noexcept;
};

class NonDrawReplaySinkV2 {
 public:
  virtual ~NonDrawReplaySinkV2() = default;

  virtual std::int32_t setConstants(
      std::uint32_t type, const D9CCommandChunkWireSetConstV2& fixed,
      std::span<const std::byte> registerBytes) = 0;
  virtual std::int32_t clear(
      const D9CCommandChunkWireClearV2& fixed,
      std::span<const D9CRect> rects) = 0;
  virtual std::int32_t present(
      const D9CCommandChunkWirePresentV2& fixed) = 0;
  virtual std::int32_t stretchRect(
      const D9CCommandChunkWireStretchRectV2& fixed, void* src,
      void* dst) = 0;
  virtual std::int32_t colorFill(
      const D9CCommandChunkWireColorFillV2& fixed, void* surface) = 0;
  virtual std::int32_t updateTexture(
      const D9CCommandChunkWireUpdateTextureV2& fixed, void* src,
      void* dst) = 0;
  virtual std::int32_t updateSurface(
      const D9CCommandChunkWireUpdateSurfaceV2& fixed, void* src,
      void* dst) = 0;
  virtual std::int32_t queryIssue(
      const D9CCommandChunkWireQueryIssueV2& fixed, void* query) = 0;
  virtual std::int32_t readback(
      const D9CCommandChunkWireReadbackV2& fixed, void* src,
      void* dst) = 0;
  virtual std::int32_t reszDepthResolve(
      const D9CCommandChunkWireReszDepthResolveV2& fixed, void* msaaDepth,
      void* intzDest) = 0;
  virtual std::int32_t applyState(
      const ResolvedRecordV2View& record) = 0;
};

struct SparseDrawCallV2 {
  dxmt9::core::DrawParam param{};
  dxmt9::core::DrawParamPayloadView payload{};
  std::uint32_t flags = 0u;
  std::uint32_t minVertex = 0u;
  std::uint32_t numVertices = 0u;
  std::uint32_t stride = 0u;
  std::uint32_t indexFormat = 0u;
};

class SparseReplaySinkV2 {
 public:
  virtual ~SparseReplaySinkV2() = default;

  virtual std::int32_t setRenderStates(
      std::span<const D9CCommandChunkWireRenderStateV2> values) = 0;
  virtual std::int32_t setTexture(std::uint32_t slot, void* texture) = 0;
  virtual std::int32_t setStream(
      const D9CCommandChunkWireStreamBindingV2& value, void* buffer) = 0;
  virtual std::int32_t setShader(std::uint32_t stage, void* shader) = 0;
  virtual std::int32_t setVertexInput(
      std::uint32_t kind, std::uint32_t value, void* declaration) = 0;
  virtual std::int32_t setIndexBuffer(void* buffer) = 0;
  virtual std::int32_t setRenderTarget(
      std::uint32_t slot, void* surface) = 0;
  virtual std::int32_t setDepthStencil(void* surface) = 0;
  virtual std::int32_t setViewport(const D9CViewport& value) = 0;
  virtual std::int32_t setScissor(const D9CRect& value) = 0;
  virtual std::int32_t setMaterial(const D9CMaterial& value) = 0;
  virtual std::int32_t setClipPlane(
      const D9CCommandChunkWireClipPlaneV2& value) = 0;
  virtual std::int32_t setTextureStageStates(
      std::span<const D9CDrawPacketTextureStageState> values) = 0;
  virtual std::int32_t setSamplerStates(
      std::span<const D9CDrawPacketSamplerState> values) = 0;
  virtual std::int32_t setTransforms(
      std::span<const D9CDrawPacketTransform> values) = 0;
  virtual std::int32_t setLights(
      std::span<const D9CCommandChunkWireLightV2> values) = 0;
  virtual std::int32_t setLightEnables(
      std::span<const D9CCommandChunkWireLightEnableV2> values) = 0;
  virtual std::int32_t setConstants(
      std::uint16_t sectionKind,
      const D9CCommandChunkWireConstantRangeV2& range,
      std::span<const std::byte> registerBytes) = 0;
  virtual std::int32_t finishApplyState(std::uint32_t flags) = 0;
  virtual std::int32_t draw(const SparseDrawCallV2& call) = 0;
};

bool isSparseRecordV2(std::uint32_t type) noexcept;

std::int32_t replaySparseRecordV2(
    const ResolvedRecordV2View& record,
    SparseReplaySinkV2& sink) noexcept;

bool isNonDrawRecordV2(std::uint32_t type) noexcept;

std::int32_t replayNonDrawRecordV2(
    const ResolvedRecordV2View& record,
    NonDrawReplaySinkV2& sink) noexcept;

}  // namespace dxmt9::d3d9
