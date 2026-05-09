#pragma once

// Private header shared between the dxmt9_draw_encoder.mm core and the
// hazard / diagnostics sibling translation units (T7 split). Keeps the
// public dxmt9_draw_encoder.hpp surface frozen while letting encodeChunk
// reach hazard helpers and encodeDraw reach the geometry-trace recorder.

#include "dxmt9_draw_encoder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dxmt9::encoders {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;

// Hazard tracking primitives shared with encodeChunk. The bloom filter
// is the cheap pre-screen; the exact handle list resolves false
// positives before deciding to split a render encoder. The mix helper
// is inline in the header so HazardBloom::add can keep its verbatim
// call site after T7's split.

inline u64 bloomMix64(u64 value, u64 salt) {
  u64 x = value + salt + 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

struct HazardBloom {
  std::array<u64, 2> bits{};
  void add(u64 value) {
    if (value == 0) return;
    const u64 hash0 = bloomMix64(value, 0x4d595df4d0f33173ull);
    const u64 hash1 = bloomMix64(value, 0x9e3779b97f4a7c15ull);
    bits[0] |= 1ull << (hash0 & 63u);
    bits[1] |= 1ull << (hash1 & 63u);
  }
  bool overlaps(const HazardBloom& other) const {
    return ((bits[0] & other.bits[0]) != 0) || ((bits[1] & other.bits[1]) != 0);
  }
};

struct HazardHandles {
  static constexpr std::size_t kCapacity =
      1u + core::kMaxRenderTargets + core::kMaxStreams + core::kMaxTextureStages;
  std::array<u64, kCapacity> handles{};
  std::size_t count = 0;

  void add(u64 value) {
    if (value == 0) return;
    for (std::size_t i = 0; i < count; ++i) {
      if (handles[i] == value) return;
    }
    if (count < handles.size()) {
      handles[count++] = value;
    }
  }

  bool overlaps(const HazardHandles& other) const {
    for (std::size_t i = 0; i < count; ++i) {
      for (std::size_t j = 0; j < other.count; ++j) {
        if (handles[i] == other.handles[j]) return true;
      }
    }
    return false;
  }
};

struct HazardProbe {
  HazardBloom bloom;
  HazardHandles exact;

  void add(u64 value) {
    bloom.add(value);
    exact.add(value);
  }

  bool bloomOverlaps(const HazardProbe& other) const {
    return bloom.overlaps(other.bloom);
  }

  bool exactOverlaps(const HazardProbe& other) const {
    return exact.overlaps(other.exact);
  }
};

HazardProbe makeAttachmentHazard(const core::FlatDrawStateRecord& hot);
HazardProbe makeAttachmentHazard(const core::ClearDesc& clear);
HazardProbe makeDrawReadHazard(core::FlatDrawStateView state);

// Per-draw view from DrawParam. Constructed once at encodeDraw entry; all
// per-draw field reads inside the function go through this view. Lives in
// the shared internal header because diagnostics consumes it by const-ref.
struct ParamView {
  core::PrimitiveType primitiveType;
  u32 primitiveCount;
  u32 startVertex;
  i32 baseVertexIndex;
  u32 startIndex;
  core::IndexType indexType;
  bool indexed;
  std::span<const u8> userVertexData;
  std::span<const u8> userIndexData;
};

// Geometry-trace recorder (env-gated). Called from encodeDraw for the
// indexed / expanded-indexed / non-indexed paths. Implementation lives
// in dxmt9_draw_encoder_diagnostics.mm.
void recordDrawGeometryDiagnostics(core::FlatDrawStateView drawState,
                                   const ParamView& pv,
                                   u64 seqId,
                                   u64 vertexCount,
                                   u64 vertexBufferOffset,
                                   u32 vertexStreamOffset,
                                   u32 vertexStreamStride,
                                   bool indexed,
                                   bool direct,
                                   bool up,
                                   bool expanded,
                                   bool fixedFunctionPath);

}  // namespace dxmt9::encoders
