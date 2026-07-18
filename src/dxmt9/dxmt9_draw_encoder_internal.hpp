#pragma once

// Private header shared between the dxmt9_draw_encoder.mm core and the
// hazard / diagnostics sibling translation units (T7 split). Keeps the
// public dxmt9_draw_encoder.hpp surface frozen while letting encodeChunk
// reach hazard helpers and encodeDraw reach the geometry-trace recorder.

#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_draw_shader.hpp"
#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_format_convert.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

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

enum class RenderPassEntryDecision : u8 {
  ContinueActive,
  BeginPass,
  SplitRenderTargetChange,
  SplitHazard,
  SplitTileMidPassIneligible,
};

inline RenderPassEntryDecision classifyRenderPassEntry(
    bool hasActiveRender,
    bool attachmentKeyMatches,
    bool exactHazard,
    bool tileMidPassIneligible) noexcept {
  if (!hasActiveRender) {
    return RenderPassEntryDecision::BeginPass;
  }
  if (!attachmentKeyMatches) {
    return RenderPassEntryDecision::SplitRenderTargetChange;
  }
  if (exactHazard) {
    return RenderPassEntryDecision::SplitHazard;
  }
  if (tileMidPassIneligible) {
    return RenderPassEntryDecision::SplitTileMidPassIneligible;
  }
  return RenderPassEntryDecision::ContinueActive;
}

inline perf::EncoderSplitReason renderPassEntrySplitReason(
    RenderPassEntryDecision decision,
    perf::EncoderSplitReason nonSplitFallback) noexcept {
  switch (decision) {
  case RenderPassEntryDecision::SplitRenderTargetChange:
    return perf::EncoderSplitReason::RenderTargetChange;
  case RenderPassEntryDecision::SplitHazard:
    return perf::EncoderSplitReason::Hazard;
  case RenderPassEntryDecision::SplitTileMidPassIneligible:
    return perf::EncoderSplitReason::TileMidPassIneligible;
  case RenderPassEntryDecision::BeginPass:
  case RenderPassEntryDecision::ContinueActive:
    return nonSplitFallback;
  }
  return nonSplitFallback;
}

inline bool useSourceLocalStoreProofLookahead(
    bool externalEncodeSession,
    bool sessionMayContinue) noexcept {
  // R-BACK-2.48: source-local lookahead only proves the current source's
  // suffix. A carried EncodeSession that can accept later sources must stay
  // conservative, but the finalizing call has no future source; its current
  // suffix is the remaining logical stream suffix.
  return !externalEncodeSession || !sessionMayContinue;
}

struct EncodeChunkReplayRange {
  std::size_t commandBegin = 0;
  std::size_t commandEnd = 0;
  bool valid = true;

  std::size_t commandCount() const noexcept {
    return commandEnd >= commandBegin ? commandEnd - commandBegin : 0;
  }
};

inline EncodeChunkReplayRange encodeChunkReplayRange(
    std::size_t slotIndex,
    const core::ChunkSlot& slot,
    const EncodeChunkOptions& options) noexcept {
  const std::size_t slotCommandCount = slot.commandCount();
  if (!options.sessionSource.has_value()) {
    return EncodeChunkReplayRange{
        .commandBegin = 0,
        .commandEnd = slotCommandCount,
        .valid = true,
    };
  }

  const auto& source = *options.sessionSource;
  if (source.slotIndex != slotIndex || source.seqId != slot.seqId ||
      source.commandBegin > slotCommandCount ||
      source.commandCount > slotCommandCount - source.commandBegin) {
    return EncodeChunkReplayRange{
        .commandBegin = 0,
        .commandEnd = 0,
        .valid = false,
    };
  }
  return EncodeChunkReplayRange{
      .commandBegin = source.commandBegin,
      .commandEnd = source.commandBegin + source.commandCount,
      .valid = true,
  };
}

inline bool readySlotSnapshotMatchesCompletionSource(
    const core::metalqueue::ReadySlotSnapshot& snapshot,
    const core::metalqueue::QueueCompletionSource& source,
    std::size_t slotIndex,
    const core::ChunkSlot& slot) noexcept {
  return snapshot.slot == &slot &&
         snapshot.slotIndex == slotIndex &&
         snapshot.slotIndex == source.slotIndex &&
         snapshot.seqId == slot.seqId &&
         snapshot.seqId == source.seqId &&
         snapshot.hasPresent == source.hasPresent &&
         snapshot.commandBegin == source.commandBegin &&
         snapshot.commandCount == source.commandCount;
}

inline bool readySlotSnapshotMatchesReplayRange(
    const core::metalqueue::ReadySlotSnapshot& snapshot,
    std::size_t slotIndex,
    const core::ChunkSlot& slot,
    EncodeChunkReplayRange replayRange) noexcept {
  return snapshot.slot == &slot &&
         snapshot.slotIndex == slotIndex &&
         snapshot.seqId == slot.seqId &&
         snapshot.commandBegin == replayRange.commandBegin &&
         snapshot.commandCount == replayRange.commandCount();
}

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
  u32 instanceCount;
  std::span<const u8> userVertexData;
  std::span<const u8> userIndexData;
};

inline bool drawBindingOverrideRequiresBaseStateBind(
    const core::DrawBindingOverride& binding,
    const core::DrawShaderLayoutContext* baseShaderLayout) noexcept {
  if (!baseShaderLayout) {
    return false;
  }

  // Stream 0 stride is passed through DrawVolatile, but extra-stream strides
  // are baked into the generated VS input-load source. Rebind base state only
  // when an override can require a different PSO/source variant.
  for (u32 stream = 1; stream < core::kMaxStreams; ++stream) {
    if ((binding.streamMask & (1u << stream)) == 0u) {
      continue;
    }
    if (binding.streams[stream].stride !=
        baseShaderLayout->vertexDecl.streams[stream].stride) {
      return true;
    }
  }
  return false;
}

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
  u64 samplerStateHash = 0;
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
  u64 samplerStateHash = 0;
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
  u64 samplerStateHash = 0;
  core::FlatStateSet<core::kMaxSamplerStates> samplerStates{};

  friend constexpr bool operator==(const DrawBindingPacketTextureSamplerKey& lhs,
                                   const DrawBindingPacketTextureSamplerKey& rhs);
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

  friend constexpr bool operator==(const DrawBindingPacketKey& lhs,
                                   const DrawBindingPacketKey& rhs);
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

template <std::size_t MaxEntries>
constexpr bool drawBindingPacketFlatStateSetsEqual(
    const core::FlatStateSet<MaxEntries>& lhs,
    const core::FlatStateSet<MaxEntries>& rhs) noexcept {
  if (lhs.count != rhs.count ||
      lhs.hash != rhs.hash ||
      lhs.overflow != rhs.overflow) {
    return false;
  }
  const u32 count = std::min<u32>(lhs.count, static_cast<u32>(MaxEntries));
  for (u32 i = 0; i < count; ++i) {
    if (lhs.entries[i] != rhs.entries[i]) {
      return false;
    }
  }
  return true;
}

constexpr bool operator==(const DrawBindingPacketTextureSamplerKey& lhs,
                          const DrawBindingPacketTextureSamplerKey& rhs) {
  return lhs.stage == rhs.stage &&
         lhs.texture == rhs.texture &&
         lhs.textureLod == rhs.textureLod &&
         lhs.samplerStateHash == rhs.samplerStateHash &&
         drawBindingPacketFlatStateSetsEqual(lhs.samplerStates, rhs.samplerStates);
}

constexpr bool operator==(const DrawBindingPacketKey& lhs,
                          const DrawBindingPacketKey& rhs) {
  if (lhs.fragmentTextureSamplerCount != rhs.fragmentTextureSamplerCount ||
      lhs.vertexTextureSamplerCount != rhs.vertexTextureSamplerCount ||
      lhs.extraStreamCount != rhs.extraStreamCount ||
      lhs.raster != rhs.raster) {
    return false;
  }
  for (u32 i = 0; i < lhs.fragmentTextureSamplerCount; ++i) {
    if (lhs.fragmentTextureSamplers[i] != rhs.fragmentTextureSamplers[i]) {
      return false;
    }
  }
  for (u32 i = 0; i < lhs.vertexTextureSamplerCount; ++i) {
    if (lhs.vertexTextureSamplers[i] != rhs.vertexTextureSamplers[i]) {
      return false;
    }
  }
  for (u32 i = 0; i < lhs.extraStreamCount; ++i) {
    if (lhs.extraStreams[i] != rhs.extraStreams[i]) {
      return false;
    }
  }
  return true;
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
        .samplerStateHash = binding.samplerStateHash,
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
        .samplerStateHash = binding.samplerStateHash,
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
    seed = drawBindingPacketHashMix(seed, binding.samplerStateHash);
  }

  seed = drawBindingPacketHashMix(seed, key.vertexTextureSamplerCount);
  for (u32 i = 0; i < key.vertexTextureSamplerCount; ++i) {
    const auto& binding = key.vertexTextureSamplers[i];
    seed = drawBindingPacketHashMix(seed, binding.stage);
    seed = drawBindingPacketHashMix(seed, binding.texture);
    seed = drawBindingPacketHashMix(seed, binding.textureLod);
    seed = drawBindingPacketHashMix(seed, binding.samplerStateHash);
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

inline u64 hashDrawBindingPacketPlan(const DrawBindingPacketPlan& packet) noexcept {
  u64 seed = drawBindingPacketHashMix(
      0x5ad07b1f4c2e9638ull,
      static_cast<u64>(packet.fragmentTextureSamplers.size()));
  for (const auto& binding : packet.fragmentTextureSamplers) {
    seed = drawBindingPacketHashMix(seed, binding.stage);
    seed = drawBindingPacketHashMix(seed, binding.texture.value);
    seed = drawBindingPacketHashMix(seed, binding.textureLod);
    seed = drawBindingPacketHashMix(seed, binding.samplerStateHash);
  }

  seed = drawBindingPacketHashMix(
      seed, static_cast<u64>(packet.vertexTextureSamplers.size()));
  for (const auto& binding : packet.vertexTextureSamplers) {
    seed = drawBindingPacketHashMix(seed, binding.stage);
    seed = drawBindingPacketHashMix(seed, binding.texture.value);
    seed = drawBindingPacketHashMix(seed, binding.textureLod);
    seed = drawBindingPacketHashMix(seed, binding.samplerStateHash);
  }

  seed = drawBindingPacketHashMix(seed, static_cast<u64>(packet.extraStreams.size()));
  for (const auto& binding : packet.extraStreams) {
    seed = drawBindingPacketHashMix(seed, binding.stream);
    seed = drawBindingPacketHashMix(seed, binding.metalSlot);
    seed = drawBindingPacketHashMix(seed, binding.offset);
    seed = drawBindingPacketHashMix(seed, binding.stride);
  }

  const auto raster = makeDrawBindingPacketRasterKey(packet.raster);
  for (const auto bits : raster.viewportBits) {
    seed = drawBindingPacketHashMix(seed, bits);
  }
  seed = drawBindingPacketHashMix(seed, raster.scissorX);
  seed = drawBindingPacketHashMix(seed, raster.scissorY);
  seed = drawBindingPacketHashMix(seed, raster.scissorWidth);
  seed = drawBindingPacketHashMix(seed, raster.scissorHeight);
  seed = drawBindingPacketHashMix(seed, raster.cullMode);
  return seed;
}

inline bool drawBindingPacketTextureSamplerBindingsEqual(
    const FragmentTextureSamplerBinding& lhs,
    const FragmentTextureSamplerBinding& rhs) noexcept {
  return lhs.stage == rhs.stage &&
         lhs.texture == rhs.texture &&
         lhs.textureLod == rhs.textureLod &&
         lhs.samplerStateHash == rhs.samplerStateHash &&
         drawBindingPacketFlatStateSetsEqual(lhs.samplerStates, rhs.samplerStates);
}

inline bool drawBindingPacketTextureSamplerBindingsEqual(
    const VertexTextureSamplerBinding& lhs,
    const VertexTextureSamplerBinding& rhs) noexcept {
  return lhs.stage == rhs.stage &&
         lhs.texture == rhs.texture &&
         lhs.textureLod == rhs.textureLod &&
         lhs.samplerStateHash == rhs.samplerStateHash &&
         drawBindingPacketFlatStateSetsEqual(lhs.samplerStates, rhs.samplerStates);
}

inline bool drawBindingPacketExtraStreamBindingsEqual(
    const ProgrammableVsExtraStreamBinding& lhs,
    const ProgrammableVsExtraStreamBinding& rhs) noexcept {
  return lhs.stream == rhs.stream &&
         lhs.metalSlot == rhs.metalSlot &&
         lhs.offset == rhs.offset &&
         lhs.stride == rhs.stride;
}

inline bool drawBindingPacketPlansEqual(
    const DrawBindingPacketPlan& lhs,
    const DrawBindingPacketPlan& rhs) noexcept {
  if (lhs.fragmentTextureSamplers.size() != rhs.fragmentTextureSamplers.size() ||
      lhs.vertexTextureSamplers.size() != rhs.vertexTextureSamplers.size() ||
      lhs.extraStreams.size() != rhs.extraStreams.size() ||
      makeDrawBindingPacketRasterKey(lhs.raster) != makeDrawBindingPacketRasterKey(rhs.raster)) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.fragmentTextureSamplers.size(); ++i) {
    if (!drawBindingPacketTextureSamplerBindingsEqual(
            lhs.fragmentTextureSamplers[i], rhs.fragmentTextureSamplers[i])) {
      return false;
    }
  }
  for (std::size_t i = 0; i < lhs.vertexTextureSamplers.size(); ++i) {
    if (!drawBindingPacketTextureSamplerBindingsEqual(
            lhs.vertexTextureSamplers[i], rhs.vertexTextureSamplers[i])) {
      return false;
    }
  }
  for (std::size_t i = 0; i < lhs.extraStreams.size(); ++i) {
    if (!drawBindingPacketExtraStreamBindingsEqual(lhs.extraStreams[i], rhs.extraStreams[i])) {
      return false;
    }
  }
  return true;
}

struct DrawBindingPacketKeyHash {
  std::size_t operator()(const DrawBindingPacketKey& key) const noexcept {
    return static_cast<std::size_t>(hashDrawBindingPacketKey(key));
  }
};

struct DrawBindingPacketCacheEntry {
  bool valid = false;
  u64 hash = 0;
  DrawBindingPacketPlan packet{};
};

struct DrawBindingPacketCache {
  static constexpr std::size_t kCapacity = 128;
  std::array<DrawBindingPacketCacheEntry, kCapacity> entries{};
  u64 hits = 0;
  u64 misses = 0;
};

struct DrawBindingPacketCacheStats {
  u64 keyCpuNs = 0;
  u64 hashCpuNs = 0;
  u64 probeCpuNs = 0;
  u64 storeCpuNs = 0;
  u64 hits = 0;
  u64 misses = 0;
  u64 collisions = 0;
};

inline u64 drawBindingPacketNowNs() noexcept {
  return static_cast<u64>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline const DrawBindingPacketPlan& cacheDrawBindingPacket(
    DrawBindingPacketCache& cache,
    const DrawBindingPacketPlan& packet,
    DrawBindingPacketCacheStats* stats = nullptr) noexcept {
  const u64 hashStart = stats ? drawBindingPacketNowNs() : 0;
  const auto hash = hashDrawBindingPacketPlan(packet);
  const u64 hashEnd = stats ? drawBindingPacketNowNs() : 0;
  if (stats) {
    stats->hashCpuNs += hashEnd - hashStart;
  }

  const u64 probeStart = stats ? drawBindingPacketNowNs() : 0;
  auto& entry = cache.entries[hash % DrawBindingPacketCache::kCapacity];
  const bool entryValid = entry.valid;
  const bool hit = entryValid && entry.hash == hash &&
                   drawBindingPacketPlansEqual(entry.packet, packet);
  const u64 probeEnd = stats ? drawBindingPacketNowNs() : 0;
  if (stats) {
    stats->probeCpuNs += probeEnd - probeStart;
  }

  if (hit) {
    ++cache.hits;
    if (stats) {
      ++stats->hits;
    }
    return entry.packet;
  }

  ++cache.misses;
  if (stats) {
    ++stats->misses;
    if (entryValid) {
      ++stats->collisions;
    }
  }

  const u64 storeStart = stats ? drawBindingPacketNowNs() : 0;
  entry.valid = true;
  entry.hash = hash;
  entry.packet = packet;
  const u64 storeEnd = stats ? drawBindingPacketNowNs() : 0;
  if (stats) {
    stats->storeCpuNs += storeEnd - storeStart;
  }
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

inline bool expandIndexedStreamToFlatVertexBytes(std::span<const u8> sourceBytes,
                                                 std::span<const u8> indexBytes,
                                                 core::IndexType indexType,
                                                 u32 startIndex,
                                                 i32 baseVertexIndex,
                                                 u64 vertexCount,
                                                 std::size_t sourceBase,
                                                 std::size_t sourceStride,
                                                 std::vector<u8>& outBytes) {
  if (sourceBytes.empty() || indexBytes.empty() || sourceStride == 0) {
    return false;
  }

  const std::size_t indexSize = indexType == core::IndexType::UInt16 ? sizeof(u16) : sizeof(u32);
  const std::size_t firstIndexByte = static_cast<std::size_t>(startIndex) * indexSize;
  outBytes.assign(static_cast<std::size_t>(vertexCount) * sourceStride, 0);

  for (u64 i = 0; i < vertexCount; ++i) {
    i32 vertexIndex = baseVertexIndex;
    const std::size_t indexOffset = firstIndexByte + static_cast<std::size_t>(i) * indexSize;
    if (indexType == core::IndexType::UInt16) {
      if (indexOffset + sizeof(u16) > indexBytes.size()) {
        return false;
      }
      u16 index = 0;
      std::memcpy(&index, indexBytes.data() + indexOffset, sizeof(index));
      vertexIndex += static_cast<i32>(index);
    } else {
      if (indexOffset + sizeof(u32) > indexBytes.size()) {
        return false;
      }
      u32 index = 0;
      std::memcpy(&index, indexBytes.data() + indexOffset, sizeof(index));
      vertexIndex += static_cast<i32>(index);
    }

    if (vertexIndex < 0) {
      return false;
    }
    const std::size_t sourceOffset =
        sourceBase + static_cast<std::size_t>(vertexIndex) * sourceStride;
    if (sourceOffset + sourceStride > sourceBytes.size()) {
      return false;
    }
    std::memcpy(outBytes.data() + static_cast<std::size_t>(i) * sourceStride,
                sourceBytes.data() + sourceOffset,
                sourceStride);
  }

  return true;
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
    const bool instanced =
        (hot.streamFrequencies[stream] & core::kStreamSourceInstanceData) != 0;
    if (!pv.indexed && !instanced && stride != 0u) {
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
    const core::FlatDrawStateRecord& hot,
    const core::ShaderRef* pixelShader = nullptr) {
  FragmentTextureSamplerBindingList bindings;
  const u32 activeMask = pixelShader
      ? drawshader::activeFragmentTextureMaskForShader(*pixelShader, hot.textureMask)
      : (hot.textureMask & ((1u << core::kMaxFragmentSamplers) - 1u));
  for (u32 stage = 0; stage < core::kMaxFragmentSamplers; ++stage) {
    if ((activeMask & (1u << stage)) == 0u) {
      continue;
    }
    const auto textureHandle = hot.textures[stage];
    if (!textureHandle) {
      continue;
    }
    bindings.push_back(FragmentTextureSamplerBinding{
        .stage = stage,
        .texture = textureHandle,
        .textureLod = hot.textureLods[stage],
        .samplerStateHash = hot.key.samplerStateHashes[stage],
        .samplerStates = hot.samplerStates[stage],
    });
  }
  return bindings;
}

template <std::size_t MaxEntries>
inline bool shouldAutoExpandIndexedDraw(const core::FlatStateSet<MaxEntries>& renderStates,
                                        u32 textureMask,
                                        bool fixedFunctionPath,
                                        bool ffpDecodableLayout,
                                        bool texture0R32FCube) {
  const bool alphaBlendEnabled =
      core::flatStateOr(renderStates, core::RS_ALPHABLEND_ENABLE, 0u) != 0u;
  const bool invDestColorAddBlend =
      core::flatStateOr(renderStates, core::RS_SRC_BLEND, 0u) ==
          static_cast<u32>(core::BlendFactor::InvDestColor) &&
      core::flatStateOr(renderStates, core::RS_DEST_BLEND, 0u) ==
          static_cast<u32>(core::BlendFactor::One);
  if (!alphaBlendEnabled || !invDestColorAddBlend || !ffpDecodableLayout) {
    return false;
  }

  if (fixedFunctionPath && (textureMask & 0x3fu) == 0x3fu) {
    return true;
  }

  return texture0R32FCube && (textureMask & 0x1fu) == 0x1fu;
}

template <std::size_t MaxEntries>
inline bool shouldOptimizeOpaqueDepthIndexOrder(
    const core::FlatStateSet<MaxEntries>& renderStates,
    WMTTriangleFillMode fillMode,
    bool depthWriteGloballyDisabled = false) {
  if (fillMode != WMTTriangleFillModeFill) {
    return false;
  }
  if (core::flatStateOr(renderStates, core::RS_ALPHABLEND_ENABLE, 0u) != 0u) {
    return false;
  }
  if (core::flatStateOr(renderStates, core::RS_ALPHA_TEST_ENABLE, 0u) != 0u) {
    return false;
  }
  if (core::flatStateOr(renderStates, core::RS_STENCIL_ENABLE, 0u) != 0u) {
    return false;
  }
  if (core::flatStateOr(renderStates, core::RS_CLIP_PLANE_ENABLE, 0u) != 0u) {
    return false;
  }

  const bool depthEnabled =
      core::flatStateOr(renderStates, core::RS_Z_ENABLE, 0u) != 0u;
  const bool depthWrite =
      depthEnabled && !depthWriteGloballyDisabled &&
      core::flatStateOr(renderStates, core::RS_Z_WRITE_ENABLE, 0u) != 0u;
  if (!depthWrite) {
    return false;
  }

  switch (static_cast<core::CompareFunc>(core::flatStateOr(
      renderStates,
      core::RS_Z_FUNC,
      static_cast<u32>(core::CompareFunc::LessEqual)))) {
    case core::CompareFunc::Less:
    case core::CompareFunc::LessEqual:
      return true;
    default:
      return false;
  }
}

template <std::size_t MaxEntries>
inline bool shouldOptimizeScreenBlendIndexOrder(
    const core::FlatStateSet<MaxEntries>& renderStates) {
  const bool alphaBlendEnabled =
      core::flatStateOr(renderStates, core::RS_ALPHABLEND_ENABLE, 0u) != 0u;
  const bool screenBlend =
      core::flatStateOr(renderStates, core::RS_SRC_BLEND, 0u) ==
          static_cast<u32>(core::BlendFactor::InvDestColor) &&
      core::flatStateOr(renderStates, core::RS_DEST_BLEND, 0u) ==
          static_cast<u32>(core::BlendFactor::One) &&
      core::flatStateOr(renderStates,
                        core::RS_BLEND_OP,
                        static_cast<u32>(core::BlendOp::Add)) ==
          static_cast<u32>(core::BlendOp::Add);
  const bool separateAlpha =
      core::flatStateOr(renderStates,
                        core::RS_SEPARATE_ALPHA_BLEND_ENABLE,
                        0u) != 0u;
  const bool depthEnabled =
      core::flatStateOr(renderStates, core::RS_Z_ENABLE, 0u) != 0u;
  const bool depthWrite =
      depthEnabled &&
      core::flatStateOr(renderStates, core::RS_Z_WRITE_ENABLE, 0u) != 0u;
  const bool alphaTestEnabled =
      core::flatStateOr(renderStates, core::RS_ALPHA_TEST_ENABLE, 0u) != 0u;
  const bool stencilEnabled =
      core::flatStateOr(renderStates, core::RS_STENCIL_ENABLE, 0u) != 0u;
  const bool clipPlaneEnabled =
      core::flatStateOr(renderStates, core::RS_CLIP_PLANE_ENABLE, 0u) != 0u;

  return alphaBlendEnabled && screenBlend && !separateAlpha &&
         depthEnabled && !depthWrite && !alphaTestEnabled &&
         !stencilEnabled && !clipPlaneEnabled;
}

inline VertexTextureSamplerBindingList makeVertexTextureSamplerBindings(
    const core::FlatDrawStateRecord& hot) {
  VertexTextureSamplerBindingList bindings;
  for (u32 stage = 0; stage < core::kMaxVertexTextureSamplers; ++stage) {
    const u32 textureSlot = core::kVertexTextureSampler0 + stage;
    if ((hot.textureMask & (1u << textureSlot)) == 0u) {
      continue;
    }
    const auto textureHandle = hot.textures[textureSlot];
    if (!textureHandle) {
      continue;
    }
    bindings.push_back(VertexTextureSamplerBinding{
        .stage = stage,
        .texture = textureHandle,
        .textureLod = hot.textureLods[textureSlot],
        .samplerStateHash = hot.key.samplerStateHashes[textureSlot],
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
    bool cullDisabled,
    const core::ViewportScissor* viewportOverride = nullptr) {
  const auto& viewport = viewportOverride ? *viewportOverride : hot.viewport;
  const auto targetWidth = std::max(1u, surfaceWidth);
  const auto targetHeight = std::max(1u, surfaceHeight);
  double viewportWidth = static_cast<double>(std::max(1u, viewport.viewport.width));
  double viewportHeight = static_cast<double>(std::max(1u, viewport.viewport.height));
  double viewportOriginX = static_cast<double>(viewport.viewport.x);
  double viewportOriginY = static_cast<double>(viewport.viewport.y);
  if (preTransformed) {
    viewportOriginX = 0.0;
    viewportOriginY = 0.0;
    viewportWidth = static_cast<double>(targetWidth);
    viewportHeight = static_cast<double>(targetHeight);
  }

  WMTScissorRect scissor{};
  if (viewport.scissorEnabled && !scissorDisabled) {
    scissor.x = static_cast<std::uint64_t>(std::max(0, viewport.scissor.left));
    scissor.y = static_cast<std::uint64_t>(std::max(0, viewport.scissor.top));
    scissor.width = static_cast<std::uint64_t>(
        std::max(0, viewport.scissor.right - viewport.scissor.left));
    scissor.height = static_cast<std::uint64_t>(
        std::max(0, viewport.scissor.bottom - viewport.scissor.top));
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
          static_cast<double>(viewport.viewport.minZ),
          static_cast<double>(viewport.viewport.maxZ),
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
    bool cullDisabled,
    const core::ShaderRef* pixelShader = nullptr,
    const core::ViewportScissor* viewportOverride = nullptr) {
  return DrawBindingPacketPlan{
      .fragmentTextureSamplers = makeFragmentTextureSamplerBindings(hot, pixelShader),
      .vertexTextureSamplers = makeVertexTextureSamplerBindings(hot),
      .extraStreams = makeProgrammableVsExtraStreamBindings(vertexDecl, hot, pv),
      .raster = makeEncoderRasterStatePlan(
          hot,
          surfaceWidth,
          surfaceHeight,
          preTransformed,
          scissorDisabled,
          cullDisabled,
          viewportOverride),
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
