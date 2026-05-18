#pragma once

// Private header shared between the dxmt9_draw_encoder.mm core and the
// hazard / diagnostics sibling translation units (T7 split). Keeps the
// public dxmt9_draw_encoder.hpp surface frozen while letting encodeChunk
// reach hazard helpers and encodeDraw reach the geometry-trace recorder.

#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_format_convert.hpp"

#include <algorithm>
#include <array>
#include <bit>
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
      1u + core::kMaxRenderTargets + core::kMaxStreams + core::kMaxTextures;
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

struct ProgrammableVsExtraStreamBinding {
  u32 stream = 0;
  u32 metalSlot = 0;
  u64 offset = 0;
  u32 stride = 0;
};

struct ProgrammableVsExtraStreamBindingList {
  std::array<ProgrammableVsExtraStreamBinding, core::kMaxStreams - 1u> entries{};
  std::size_t count = 0;

  void push_back(ProgrammableVsExtraStreamBinding binding) {
    if (count < entries.size()) {
      entries[count++] = binding;
    }
  }

  bool empty() const noexcept { return count == 0; }
  std::size_t size() const noexcept { return count; }

  const ProgrammableVsExtraStreamBinding& operator[](std::size_t index) const noexcept {
    return entries[index];
  }

  const ProgrammableVsExtraStreamBinding* begin() const noexcept {
    return entries.data();
  }

  const ProgrammableVsExtraStreamBinding* end() const noexcept {
    return entries.data() + count;
  }
};

struct FragmentTextureSamplerBinding {
  u32 stage = 0;
  core::Handle texture{};
  u32 textureLod = 0;
  core::FlatStateSet<core::kMaxSamplerStates> samplerStates{};
};

struct FragmentTextureSamplerBindingList {
  std::array<FragmentTextureSamplerBinding, core::kMaxSamplers> entries{};
  std::size_t count = 0;

  void push_back(FragmentTextureSamplerBinding binding) {
    if (count < entries.size()) {
      entries[count++] = binding;
    }
  }

  bool empty() const noexcept { return count == 0; }
  std::size_t size() const noexcept { return count; }

  const FragmentTextureSamplerBinding& operator[](std::size_t index) const noexcept {
    return entries[index];
  }

  const FragmentTextureSamplerBinding* begin() const noexcept {
    return entries.data();
  }

  const FragmentTextureSamplerBinding* end() const noexcept {
    return entries.data() + count;
  }
};

struct VertexTextureSamplerBinding {
  u32 stage = 0;
  core::Handle texture{};
  u32 textureLod = 0;
  core::FlatStateSet<core::kMaxSamplerStates> samplerStates{};
};

struct VertexTextureSamplerBindingList {
  std::array<VertexTextureSamplerBinding, core::kMaxVertexTextureSamplers> entries{};
  std::size_t count = 0;

  void push_back(VertexTextureSamplerBinding binding) {
    if (count < entries.size()) {
      entries[count++] = binding;
    }
  }

  bool empty() const noexcept { return count == 0; }
  std::size_t size() const noexcept { return count; }

  const VertexTextureSamplerBinding& operator[](std::size_t index) const noexcept {
    return entries[index];
  }

  const VertexTextureSamplerBinding* begin() const noexcept {
    return entries.data();
  }

  const VertexTextureSamplerBinding* end() const noexcept {
    return entries.data() + count;
  }
};

struct EncoderRasterStatePlan {
  WMTViewport viewport{};
  WMTScissorRect scissor{};
  WMTCullMode cullMode = WMTCullModeNone;
};

struct DrawBindingPacketPlan {
  FragmentTextureSamplerBindingList fragmentTextureSamplers{};
  VertexTextureSamplerBindingList vertexTextureSamplers{};
  ProgrammableVsExtraStreamBindingList extraStreams{};
  EncoderRasterStatePlan raster{};
};

struct DrawBindingPacketTextureSamplerKey {
  u32 stage = 0;
  u64 texture = 0;
  u32 textureLod = 0;
  core::FlatStateSet<core::kMaxSamplerStates> samplerStates{};

  friend constexpr bool operator==(const DrawBindingPacketTextureSamplerKey&,
                                   const DrawBindingPacketTextureSamplerKey&) = default;
};

struct DrawBindingPacketExtraStreamKey {
  u32 stream = 0;
  u32 metalSlot = 0;
  u64 offset = 0;
  u32 stride = 0;

  friend constexpr bool operator==(const DrawBindingPacketExtraStreamKey&,
                                   const DrawBindingPacketExtraStreamKey&) = default;
};

struct DrawBindingPacketRasterKey {
  std::array<u64, 6> viewportBits{};
  u64 scissorX = 0;
  u64 scissorY = 0;
  u64 scissorWidth = 0;
  u64 scissorHeight = 0;
  u32 cullMode = 0;

  friend constexpr bool operator==(const DrawBindingPacketRasterKey&,
                                   const DrawBindingPacketRasterKey&) = default;
};

struct DrawBindingPacketKey {
  std::array<DrawBindingPacketTextureSamplerKey, core::kMaxSamplers> fragmentTextureSamplers{};
  u32 fragmentTextureSamplerCount = 0;
  std::array<DrawBindingPacketTextureSamplerKey, core::kMaxVertexTextureSamplers> vertexTextureSamplers{};
  u32 vertexTextureSamplerCount = 0;
  std::array<DrawBindingPacketExtraStreamKey, core::kMaxStreams - 1u> extraStreams{};
  u32 extraStreamCount = 0;
  DrawBindingPacketRasterKey raster{};

  friend constexpr bool operator==(const DrawBindingPacketKey&,
                                   const DrawBindingPacketKey&) = default;
};

inline u64 drawBindingPacketHashMix(u64 seed, u64 value) noexcept {
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  value ^= value >> 31;
  seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
  return seed;
}

inline u64 drawBindingPacketDoubleBits(double value) noexcept {
  return std::bit_cast<u64>(value);
}

inline u64 hashDrawBindingPacketSamplerStates(
    const core::FlatStateSet<core::kMaxSamplerStates>& states) noexcept {
  u64 seed = drawBindingPacketHashMix(0x9f6c2a3b5d7e1c8full, states.count);
  seed = drawBindingPacketHashMix(seed, states.hash);
  seed = drawBindingPacketHashMix(seed, states.overflow ? 1ull : 0ull);
  for (u32 i = 0; i < states.count && i < core::kMaxSamplerStates; ++i) {
    seed = drawBindingPacketHashMix(seed, states.entries[i].state);
    seed = drawBindingPacketHashMix(seed, states.entries[i].value);
  }
  return seed;
}

inline DrawBindingPacketRasterKey makeDrawBindingPacketRasterKey(
    const EncoderRasterStatePlan& raster) noexcept {
  return DrawBindingPacketRasterKey{
      .viewportBits = {
          drawBindingPacketDoubleBits(raster.viewport.originX),
          drawBindingPacketDoubleBits(raster.viewport.originY),
          drawBindingPacketDoubleBits(raster.viewport.width),
          drawBindingPacketDoubleBits(raster.viewport.height),
          drawBindingPacketDoubleBits(raster.viewport.znear),
          drawBindingPacketDoubleBits(raster.viewport.zfar),
      },
      .scissorX = raster.scissor.x,
      .scissorY = raster.scissor.y,
      .scissorWidth = raster.scissor.width,
      .scissorHeight = raster.scissor.height,
      .cullMode = static_cast<u32>(raster.cullMode),
  };
}

inline DrawBindingPacketKey makeDrawBindingPacketKey(
    const DrawBindingPacketPlan& packet) noexcept {
  DrawBindingPacketKey key{};
  key.fragmentTextureSamplerCount = static_cast<u32>(packet.fragmentTextureSamplers.size());
  for (u32 i = 0; i < key.fragmentTextureSamplerCount; ++i) {
    const auto& binding = packet.fragmentTextureSamplers[i];
    key.fragmentTextureSamplers[i] = DrawBindingPacketTextureSamplerKey{
        .stage = binding.stage,
        .texture = binding.texture.value,
        .textureLod = binding.textureLod,
        .samplerStates = binding.samplerStates,
    };
  }

  key.vertexTextureSamplerCount = static_cast<u32>(packet.vertexTextureSamplers.size());
  for (u32 i = 0; i < key.vertexTextureSamplerCount; ++i) {
    const auto& binding = packet.vertexTextureSamplers[i];
    key.vertexTextureSamplers[i] = DrawBindingPacketTextureSamplerKey{
        .stage = binding.stage,
        .texture = binding.texture.value,
        .textureLod = binding.textureLod,
        .samplerStates = binding.samplerStates,
    };
  }

  key.extraStreamCount = static_cast<u32>(packet.extraStreams.size());
  for (u32 i = 0; i < key.extraStreamCount; ++i) {
    const auto& binding = packet.extraStreams[i];
    key.extraStreams[i] = DrawBindingPacketExtraStreamKey{
        .stream = binding.stream,
        .metalSlot = binding.metalSlot,
        .offset = binding.offset,
        .stride = binding.stride,
    };
  }

  key.raster = makeDrawBindingPacketRasterKey(packet.raster);
  return key;
}

inline u64 hashDrawBindingPacketKey(const DrawBindingPacketKey& key) noexcept {
  u64 seed = drawBindingPacketHashMix(0x5ad07b1f4c2e9638ull,
                                      key.fragmentTextureSamplerCount);
  for (u32 i = 0; i < key.fragmentTextureSamplerCount; ++i) {
    const auto& binding = key.fragmentTextureSamplers[i];
    seed = drawBindingPacketHashMix(seed, binding.stage);
    seed = drawBindingPacketHashMix(seed, binding.texture);
    seed = drawBindingPacketHashMix(seed, binding.textureLod);
    seed = drawBindingPacketHashMix(
        seed, hashDrawBindingPacketSamplerStates(binding.samplerStates));
  }

  seed = drawBindingPacketHashMix(seed, key.vertexTextureSamplerCount);
  for (u32 i = 0; i < key.vertexTextureSamplerCount; ++i) {
    const auto& binding = key.vertexTextureSamplers[i];
    seed = drawBindingPacketHashMix(seed, binding.stage);
    seed = drawBindingPacketHashMix(seed, binding.texture);
    seed = drawBindingPacketHashMix(seed, binding.textureLod);
    seed = drawBindingPacketHashMix(
        seed, hashDrawBindingPacketSamplerStates(binding.samplerStates));
  }

  seed = drawBindingPacketHashMix(seed, key.extraStreamCount);
  for (u32 i = 0; i < key.extraStreamCount; ++i) {
    const auto& binding = key.extraStreams[i];
    seed = drawBindingPacketHashMix(seed, binding.stream);
    seed = drawBindingPacketHashMix(seed, binding.metalSlot);
    seed = drawBindingPacketHashMix(seed, binding.offset);
    seed = drawBindingPacketHashMix(seed, binding.stride);
  }

  for (const auto bits : key.raster.viewportBits) {
    seed = drawBindingPacketHashMix(seed, bits);
  }
  seed = drawBindingPacketHashMix(seed, key.raster.scissorX);
  seed = drawBindingPacketHashMix(seed, key.raster.scissorY);
  seed = drawBindingPacketHashMix(seed, key.raster.scissorWidth);
  seed = drawBindingPacketHashMix(seed, key.raster.scissorHeight);
  seed = drawBindingPacketHashMix(seed, key.raster.cullMode);
  return seed;
}

struct DrawBindingPacketKeyHash {
  std::size_t operator()(const DrawBindingPacketKey& key) const noexcept {
    return static_cast<std::size_t>(hashDrawBindingPacketKey(key));
  }
};

struct DrawBindingPacketCacheEntry {
  bool valid = false;
  u64 hash = 0;
  DrawBindingPacketKey key{};
  DrawBindingPacketPlan packet{};
};

struct DrawBindingPacketCache {
  static constexpr std::size_t kCapacity = 128;
  std::array<DrawBindingPacketCacheEntry, kCapacity> entries{};
  u64 hits = 0;
  u64 misses = 0;
};

inline const DrawBindingPacketPlan& cacheDrawBindingPacket(
    DrawBindingPacketCache& cache,
    const DrawBindingPacketPlan& packet) noexcept {
  const auto key = makeDrawBindingPacketKey(packet);
  const auto hash = hashDrawBindingPacketKey(key);
  auto& entry = cache.entries[hash % DrawBindingPacketCache::kCapacity];
  if (entry.valid && entry.hash == hash && entry.key == key) {
    ++cache.hits;
    return entry.packet;
  }

  ++cache.misses;
  entry.valid = true;
  entry.hash = hash;
  entry.key = key;
  entry.packet = packet;
  return entry.packet;
}

inline bool vertexDeclUsesStream(const core::VertexDeclSnapshot& vertexDecl, u32 stream) {
  for (const auto& element : vertexDecl.elements) {
    if (element.stream == stream) {
      return true;
    }
  }
  return false;
}

inline ProgrammableVsExtraStreamBindingList makeProgrammableVsExtraStreamBindings(
    const core::VertexDeclSnapshot& vertexDecl,
    const core::FlatDrawStateRecord& hot,
    const ParamView& pv) {
  ProgrammableVsExtraStreamBindingList bindings;
  if (!pv.userVertexData.empty()) {
    return bindings;
  }

  for (u32 stream = 1; stream < vertexDecl.streams.size(); ++stream) {
    if (!vertexDeclUsesStream(vertexDecl, stream)) {
      continue;
    }

    const u32 stride = ffp::computeVertexDeclStreamStride(vertexDecl, stream);
    u64 offset = hot.streamOffsets[stream];
    if (!pv.indexed && stride != 0u) {
      offset += static_cast<u64>(pv.startVertex) * static_cast<u64>(stride);
    }
    bindings.push_back(ProgrammableVsExtraStreamBinding{
        .stream = stream,
        .metalSlot = ffp::vertexShaderStreamBufferSlot(stream),
        .offset = offset,
        .stride = stride,
    });
  }
  return bindings;
}

inline FragmentTextureSamplerBindingList makeFragmentTextureSamplerBindings(
    const core::FlatDrawStateRecord& hot) {
  FragmentTextureSamplerBindingList bindings;
  for (u32 stage = 0; stage < core::kMaxFragmentSamplers; ++stage) {
    const auto textureHandle = hot.textures[stage];
    if (!textureHandle) {
      continue;
    }
    bindings.push_back(FragmentTextureSamplerBinding{
        .stage = stage,
        .texture = textureHandle,
        .textureLod = hot.textureLods[stage],
        .samplerStates = hot.samplerStates[stage],
    });
  }
  return bindings;
}

inline VertexTextureSamplerBindingList makeVertexTextureSamplerBindings(
    const core::FlatDrawStateRecord& hot) {
  VertexTextureSamplerBindingList bindings;
  for (u32 stage = 0; stage < core::kMaxVertexTextureSamplers; ++stage) {
    const u32 textureSlot = core::kVertexTextureSampler0 + stage;
    const auto textureHandle = hot.textures[textureSlot];
    if (!textureHandle) {
      continue;
    }
    bindings.push_back(VertexTextureSamplerBinding{
        .stage = stage,
        .texture = textureHandle,
        .textureLod = hot.textureLods[textureSlot],
        .samplerStates = hot.samplerStates[textureSlot],
    });
  }
  return bindings;
}

inline EncoderRasterStatePlan makeEncoderRasterStatePlan(
    const core::FlatDrawStateRecord& hot,
    u32 surfaceWidth,
    u32 surfaceHeight,
    bool preTransformed,
    bool scissorDisabled,
    bool cullDisabled) {
  const auto targetWidth = std::max(1u, surfaceWidth);
  const auto targetHeight = std::max(1u, surfaceHeight);
  double viewportWidth = static_cast<double>(std::max(1u, hot.viewport.viewport.width));
  double viewportHeight = static_cast<double>(std::max(1u, hot.viewport.viewport.height));
  double viewportOriginX = static_cast<double>(hot.viewport.viewport.x);
  double viewportOriginY = static_cast<double>(hot.viewport.viewport.y);
  if (preTransformed) {
    viewportOriginX = 0.0;
    viewportOriginY = 0.0;
    viewportWidth = static_cast<double>(targetWidth);
    viewportHeight = static_cast<double>(targetHeight);
  }

  WMTScissorRect scissor{};
  if (hot.viewport.scissorEnabled && !scissorDisabled) {
    scissor.x = static_cast<std::uint64_t>(std::max(0, hot.viewport.scissor.left));
    scissor.y = static_cast<std::uint64_t>(std::max(0, hot.viewport.scissor.top));
    scissor.width = static_cast<std::uint64_t>(
        std::max(0, hot.viewport.scissor.right - hot.viewport.scissor.left));
    scissor.height = static_cast<std::uint64_t>(
        std::max(0, hot.viewport.scissor.bottom - hot.viewport.scissor.top));
  } else {
    scissor.x = 0;
    scissor.y = 0;
    scissor.width = static_cast<std::uint64_t>(targetWidth);
    scissor.height = static_cast<std::uint64_t>(targetHeight);
  }

  WMTCullMode cullMode = WMTCullModeNone;
  if (!preTransformed && !cullDisabled) {
    cullMode = static_cast<WMTCullMode>(convert::toCullMode(
        core::flatStateOr(hot.renderStates, core::RS_CULL_MODE, 1u)));
  }

  return EncoderRasterStatePlan{
      .viewport = WMTViewport{
          viewportOriginX,
          viewportOriginY,
          viewportWidth,
          viewportHeight,
          static_cast<double>(hot.viewport.viewport.minZ),
          static_cast<double>(hot.viewport.viewport.maxZ),
      },
      .scissor = scissor,
      .cullMode = cullMode,
  };
}

inline DrawBindingPacketPlan makeDrawBindingPacketPlan(
    const core::VertexDeclSnapshot& vertexDecl,
    const core::FlatDrawStateRecord& hot,
    const ParamView& pv,
    u32 surfaceWidth,
    u32 surfaceHeight,
    bool preTransformed,
    bool scissorDisabled,
    bool cullDisabled) {
  return DrawBindingPacketPlan{
      .fragmentTextureSamplers = makeFragmentTextureSamplerBindings(hot),
      .vertexTextureSamplers = makeVertexTextureSamplerBindings(hot),
      .extraStreams = makeProgrammableVsExtraStreamBindings(vertexDecl, hot, pv),
      .raster = makeEncoderRasterStatePlan(
          hot, surfaceWidth, surfaceHeight, preTransformed, scissorDisabled, cullDisabled),
  };
}

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
