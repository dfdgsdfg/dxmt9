#pragma once

#include "device_c_chunk_v2_validate.hpp"

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

bool isNonDrawRecordV2(std::uint32_t type) noexcept;

std::int32_t replayNonDrawRecordV2(
    const ResolvedRecordV2View& record,
    NonDrawReplaySinkV2& sink) noexcept;

}  // namespace dxmt9::d3d9
