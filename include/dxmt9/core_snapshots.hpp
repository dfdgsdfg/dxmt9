#pragma once

#include "dxmt9/assert.hpp"
#include "dxmt9/core_constants.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dxmt9 {
class PresentOutput;
}

namespace dxmt9::core {

template <std::size_t MaxEntries>
struct StateValueTable {
  struct Entry {
    u32 first = 0;
    u32 second = 0;
  };

  struct ValueRef {
    StateValueTable* table = nullptr;
    u32 key = 0;

    constexpr operator u32() const noexcept {
      return table ? table->at(key) : 0u;
    }

    constexpr ValueRef& operator=(u32 value) noexcept {
      if (table) {
        table->set(key, value);
      }
      return *this;
    }

    constexpr ValueRef& operator=(const ValueRef& value) noexcept {
      return *this = static_cast<u32>(value);
    }
  };

  std::array<u32, MaxEntries> values{};
  std::array<u64, (MaxEntries + 63u) / 64u> occupied{};
  std::array<u64, (MaxEntries + 63u) / 64u> dirty{};
  u32 count = 0;
  u64 rollingHash = 0;

  static constexpr bool validKey(u32 key) noexcept {
    return key < MaxEntries;
  }

  static constexpr u64 entryHash(u32 key, u32 value) noexcept {
    u64 hash = 1469598103934665603ull;
    hash ^= key;
    hash *= 1099511628211ull;
    hash ^= value;
    hash *= 1099511628211ull;
    return hash;
  }

  static constexpr u64 bit(u32 key) noexcept {
    return 1ull << (key % 64u);
  }

  static constexpr std::size_t word(u32 key) noexcept {
    return key / 64u;
  }

  constexpr bool contains(u32 key) const noexcept {
    return validKey(key) && (occupied[word(key)] & bit(key)) != 0;
  }

  constexpr bool empty() const noexcept {
    return count == 0;
  }

  constexpr std::size_t size() const noexcept {
    return count;
  }

  constexpr u32 at(u32 key) const noexcept {
    return contains(key) ? values[key] : 0u;
  }

  constexpr u32 valueOr(u32 key, u32 fallback = 0u) const noexcept {
    return contains(key) ? values[key] : fallback;
  }

  constexpr ValueRef operator[](u32 key) noexcept {
    return ValueRef{.table = this, .key = key};
  }

  constexpr u32 operator[](u32 key) const noexcept {
    return at(key);
  }

  constexpr void set(u32 key, u32 value) noexcept {
    if (!validKey(key)) {
      return;
    }
    const auto mask = bit(key);
    const auto slot = word(key);
    if ((occupied[slot] & mask) != 0) {
      if (values[key] == value) {
        return;
      }
      rollingHash ^= entryHash(key, values[key]);
    } else {
      occupied[slot] |= mask;
      ++count;
    }
    values[key] = value;
    dirty[slot] |= mask;
    rollingHash ^= entryHash(key, value);
  }

  constexpr void erase(u32 key) noexcept {
    if (!contains(key)) {
      return;
    }
    const auto mask = bit(key);
    const auto slot = word(key);
    rollingHash ^= entryHash(key, values[key]);
    values[key] = 0;
    occupied[slot] &= ~mask;
    dirty[slot] |= mask;
    --count;
  }

  constexpr void clear() noexcept {
    values = {};
    occupied = {};
    dirty = {};
    count = 0;
    rollingHash = 0;
  }

  constexpr void clearDirty() noexcept {
    dirty = {};
  }

  class const_iterator {
   public:
    using value_type = Entry;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    constexpr const_iterator() = default;
    constexpr const_iterator(const StateValueTable* table, u32 index)
        : table_(table), index_(index) {
      advance();
    }

    constexpr value_type operator*() const noexcept {
      return Entry{.first = index_, .second = table_->values[index_]};
    }

    constexpr const value_type* operator->() const noexcept {
      cached_ = **this;
      return &cached_;
    }

    constexpr const_iterator& operator++() noexcept {
      ++index_;
      advance();
      return *this;
    }

    constexpr bool operator==(const const_iterator& other) const noexcept {
      return table_ == other.table_ && index_ == other.index_;
    }

    constexpr bool operator!=(const const_iterator& other) const noexcept {
      return !(*this == other);
    }

   private:
    constexpr void advance() noexcept {
      if (!table_) {
        return;
      }
      while (index_ < MaxEntries && !table_->contains(index_)) {
        ++index_;
      }
    }

    const StateValueTable* table_ = nullptr;
    u32 index_ = MaxEntries;
    mutable value_type cached_{};
  };

  constexpr const_iterator begin() const noexcept {
    return const_iterator{this, 0};
  }

  constexpr const_iterator end() const noexcept {
    return const_iterator{this, static_cast<u32>(MaxEntries)};
  }

  constexpr const_iterator find(u32 key) const noexcept {
    return contains(key) ? const_iterator{this, key} : end();
  }

  friend constexpr bool operator==(const StateValueTable& a, const StateValueTable& b) noexcept {
    return a.values == b.values &&
           a.occupied == b.occupied &&
           a.count == b.count &&
           a.rollingHash == b.rollingHash;
  }
};

using RenderStateTable = StateValueTable<kMaxStateSlots>;
using TextureStageStateTable = StateValueTable<kMaxTextureStageStates>;
using SamplerStateTable = StateValueTable<kMaxSamplerStates>;

enum DrawStateInvalidationBits : u32 {
  DrawStateInvalidationUnknown = 0,
  DrawStateInvalidationMutableState = 1u << 0,
  DrawStateInvalidationDrawPacket = 1u << 1,
  DrawStateInvalidationRenderState = 1u << 2,
  DrawStateInvalidationTexture = 1u << 3,
  DrawStateInvalidationStream = 1u << 4,
  DrawStateInvalidationIndexBuffer = 1u << 5,
  DrawStateInvalidationFvfVdecl = 1u << 6,
  DrawStateInvalidationShader = 1u << 7,
  DrawStateInvalidationRenderTargetDepth = 1u << 8,
  DrawStateInvalidationViewportScissor = 1u << 9,
  DrawStateInvalidationTextureStageSampler = 1u << 10,
  DrawStateInvalidationFfpState = 1u << 11,
  DrawStateInvalidationClipPlane = 1u << 12,
  DrawStateInvalidationStateBlock = 1u << 13,
  DrawStateInvalidationReset = 1u << 14,
  DrawStateInvalidationSwapChain = 1u << 15,
  DrawStateInvalidationTextureLod = 1u << 16,
  DrawStateInvalidationTextureStageState = 1u << 17,
  DrawStateInvalidationSamplerState = 1u << 18,
};

struct TextureBinding {
  Handle handle{};
  u32 lod = 0;
  StateValueTable<kMaxTextureStageStates> stageStates{};

  friend bool operator==(const TextureBinding&, const TextureBinding&) = default;
};

struct SamplerSnapshot {
  StateValueTable<kMaxSamplerStates> states{};

  friend bool operator==(const SamplerSnapshot&, const SamplerSnapshot&) = default;
};

template <size_t FloatCount>
struct ShaderConstantSnapshot {
  std::array<std::array<f32, 4>, FloatCount> float4{};
  std::array<std::array<i32, 4>, kMaxIntegerConstants> int4{};
  std::array<bool, kMaxBoolConstants> bools{};

  friend bool operator==(const ShaderConstantSnapshot&, const ShaderConstantSnapshot&) = default;
};

using VertexShaderConstants = ShaderConstantSnapshot<kMaxVertexConstants>;
using PixelShaderConstants = ShaderConstantSnapshot<kMaxPixelConstants>;

// Fixed-capacity vertex element list. DOD-flat substitute for the previous
// std::vector<VertexElement>: same observable surface (size/empty/range
// iteration/indexing/initializer-list assignment, equality compares only the
// first `count` elements) but inline storage with a hard cap of
// kMaxVertexElements. Extra elements past the cap are dropped — production
// fixed-function and shader pipelines emit far fewer than this bound.
struct VertexElementArray {
  std::array<VertexElement, kMaxVertexElements> data{};
  u8 count = 0;

  constexpr std::size_t size() const noexcept { return count; }
  constexpr bool empty() const noexcept { return count == 0; }
  static constexpr std::size_t max_size() noexcept { return kMaxVertexElements; }
  constexpr void clear() noexcept {
    data = {};
    count = 0;
  }

  constexpr VertexElement& operator[](std::size_t index) noexcept { return data[index]; }
  constexpr const VertexElement& operator[](std::size_t index) const noexcept { return data[index]; }

  constexpr auto begin() noexcept { return data.begin(); }
  constexpr auto end() noexcept { return data.begin() + count; }
  constexpr auto begin() const noexcept { return data.begin(); }
  constexpr auto end() const noexcept { return data.begin() + count; }
  constexpr auto cbegin() const noexcept { return data.cbegin(); }
  constexpr auto cend() const noexcept { return data.cbegin() + count; }

  constexpr void push_back(const VertexElement& element) noexcept {
    if (count < kMaxVertexElements) {
      data[count++] = element;
    }
  }

  constexpr void assign(std::span<const VertexElement> source) noexcept {
    const std::size_t n = std::min<std::size_t>(source.size(), kMaxVertexElements);
    data = {};
    for (std::size_t i = 0; i < n; ++i) {
      data[i] = source[i];
    }
    count = static_cast<u8>(n);
  }

  VertexElementArray() = default;

  VertexElementArray(std::initializer_list<VertexElement> source) noexcept {
    assign(std::span<const VertexElement>(source.begin(), source.size()));
  }

  VertexElementArray& operator=(std::initializer_list<VertexElement> source) noexcept {
    assign(std::span<const VertexElement>(source.begin(), source.size()));
    return *this;
  }

  VertexElementArray& operator=(const std::vector<VertexElement>& source) noexcept {
    assign(std::span<const VertexElement>(source.data(), source.size()));
    return *this;
  }

  friend constexpr bool operator==(const VertexElementArray& a,
                                   const VertexElementArray& b) noexcept {
    if (a.count != b.count) {
      return false;
    }
    for (u8 i = 0; i < a.count; ++i) {
      if (!(a.data[i] == b.data[i])) {
        return false;
      }
    }
    return true;
  }

  friend bool operator==(const VertexElementArray& a,
                         const std::vector<VertexElement>& b) noexcept {
    if (a.count != b.size()) {
      return false;
    }
    for (u8 i = 0; i < a.count; ++i) {
      if (!(a.data[i] == b[i])) {
        return false;
      }
    }
    return true;
  }

  friend bool operator==(const std::vector<VertexElement>& a,
                         const VertexElementArray& b) noexcept {
    return b == a;
  }
};

struct VertexDeclSnapshot {
  VertexElementArray elements{};
  std::array<StreamBinding, kMaxStreams> streams{};
  u32 fvf = 0;

  friend bool operator==(const VertexDeclSnapshot&, const VertexDeclSnapshot&) = default;
};

struct RenderTargetSnapshot {
  std::array<RenderTargetAttachment, kMaxRenderTargets> color{};
  RenderTargetAttachment depthStencil{};
};

struct RenderStateSnapshot {
  StateValueTable<kMaxStateSlots> values{};

  friend bool operator==(const RenderStateSnapshot&, const RenderStateSnapshot&) = default;
};

struct TransformTable {
  struct Entry {
    u32 first = 0;
    Matrix4x4 second{};
  };

  struct ValueRef {
    TransformTable* table = nullptr;
    u32 key = 0;

    ValueRef& operator=(const Matrix4x4& value) noexcept {
      if (table) {
        table->set(key, value);
      }
      return *this;
    }

    ValueRef& operator=(const ValueRef& value) noexcept {
      return *this = static_cast<Matrix4x4>(value);
    }

    operator Matrix4x4() const noexcept {
      return table ? table->valueOr(key, Matrix4x4{}) : Matrix4x4{};
    }
  };

  std::array<Matrix4x4, kMaxTransformSlots> values{};
  std::array<u64, (kMaxTransformSlots + 63u) / 64u> occupied{};
  std::array<u64, (kMaxTransformSlots + 63u) / 64u> dirty{};
  u32 count = 0;
  u64 rollingHash = 0;

  static constexpr bool validKey(u32 key) noexcept {
    return key < kMaxTransformSlots;
  }

  static constexpr u64 bit(u32 key) noexcept {
    return 1ull << (key % 64u);
  }

  static constexpr std::size_t word(u32 key) noexcept {
    return key / 64u;
  }

  static u64 matrixHash(const Matrix4x4& matrix) noexcept {
    u64 hash = 1469598103934665603ull;
    for (f32 value : matrix.m) {
      hash ^= static_cast<u64>(std::bit_cast<u32>(value));
      hash *= 1099511628211ull;
    }
    return hash;
  }

  static u64 entryHash(u32 key, const Matrix4x4& value) noexcept {
    u64 hash = 1469598103934665603ull;
    hash ^= key;
    hash *= 1099511628211ull;
    hash ^= matrixHash(value);
    hash *= 1099511628211ull;
    return hash;
  }

  bool contains(u32 key) const noexcept {
    return validKey(key) && (occupied[word(key)] & bit(key)) != 0;
  }

  bool empty() const noexcept {
    return count == 0;
  }

  std::size_t size() const noexcept {
    return count;
  }

  const Matrix4x4& at(u32 key) const noexcept {
    return values[key];
  }

  Matrix4x4 valueOr(u32 key, const Matrix4x4& fallback) const noexcept {
    return contains(key) ? values[key] : fallback;
  }

  ValueRef operator[](u32 key) noexcept {
    return ValueRef{.table = this, .key = key};
  }

  Matrix4x4 operator[](u32 key) const noexcept {
    return contains(key) ? values[key] : Matrix4x4{};
  }

  void set(u32 key, const Matrix4x4& value) noexcept {
    if (!validKey(key)) {
      return;
    }
    const auto mask = bit(key);
    const auto slot = word(key);
    if ((occupied[slot] & mask) != 0) {
      if (values[key] == value) {
        return;
      }
      rollingHash ^= entryHash(key, values[key]);
    } else {
      occupied[slot] |= mask;
      ++count;
    }
    values[key] = value;
    dirty[slot] |= mask;
    rollingHash ^= entryHash(key, value);
  }

  void erase(u32 key) noexcept {
    if (!contains(key)) {
      return;
    }
    const auto mask = bit(key);
    const auto slot = word(key);
    rollingHash ^= entryHash(key, values[key]);
    values[key] = {};
    occupied[slot] &= ~mask;
    dirty[slot] |= mask;
    --count;
  }

  void clear() noexcept {
    values = {};
    occupied = {};
    dirty = {};
    count = 0;
    rollingHash = 0;
  }

  void clearDirty() noexcept {
    dirty = {};
  }

  class const_iterator {
   public:
    using value_type = Entry;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    const_iterator() = default;
    const_iterator(const TransformTable* table, u32 index)
        : table_(table), index_(index) {
      advance();
    }

    value_type operator*() const noexcept {
      return Entry{.first = index_, .second = table_->values[index_]};
    }

    const value_type* operator->() const noexcept {
      cached_ = **this;
      return &cached_;
    }

    const_iterator& operator++() noexcept {
      ++index_;
      advance();
      return *this;
    }

    bool operator==(const const_iterator& other) const noexcept {
      return table_ == other.table_ && index_ == other.index_;
    }

    bool operator!=(const const_iterator& other) const noexcept {
      return !(*this == other);
    }

   private:
    void advance() noexcept {
      if (!table_) {
        return;
      }
      while (index_ < kMaxTransformSlots && !table_->contains(index_)) {
        ++index_;
      }
    }

    const TransformTable* table_ = nullptr;
    u32 index_ = kMaxTransformSlots;
    mutable value_type cached_{};
  };

  const_iterator begin() const noexcept {
    return const_iterator{this, 0};
  }

  const_iterator end() const noexcept {
    return const_iterator{this, kMaxTransformSlots};
  }

  const_iterator find(u32 key) const noexcept {
    return contains(key) ? const_iterator{this, key} : end();
  }

  friend bool operator==(const TransformTable& a, const TransformTable& b) noexcept {
    return a.values == b.values &&
           a.occupied == b.occupied &&
           a.count == b.count &&
           a.rollingHash == b.rollingHash;
  }
};

struct FlatStateEntry {
  u32 state = 0;
  u32 value = 0;

  friend constexpr bool operator==(const FlatStateEntry&, const FlatStateEntry&) = default;
};

// Sorted, fixed-capacity state-id → value set produced by
// `makeFlatStateSet`. Invariant: `entries[0..count)` is sorted in
// ascending order by `state`; `entries[count..MaxEntries)` is zero-
// initialized. `findFlatState` relies on the sorted prefix to binary
// search in O(log N). Anything that constructs a FlatStateSet without
// going through `makeFlatStateSet` must preserve the same order.
template <std::size_t MaxEntries>
struct FlatStateSet {
  std::array<FlatStateEntry, MaxEntries> entries{};
  u32 count = 0;
  u64 hash = 0;
  bool overflow = false;

  friend constexpr bool operator==(const FlatStateSet&, const FlatStateSet&) = default;
};

using FlatRenderStateSet = FlatStateSet<kMaxFlatRenderStates>;

template <std::size_t MaxEntries>
constexpr const FlatStateEntry* findFlatState(const FlatStateSet<MaxEntries>& set,
                                              u32 state) noexcept {
  const auto* first = set.entries.data();
  const auto* last = first + (set.count <= MaxEntries ? set.count : MaxEntries);
  const auto* hit = std::lower_bound(
      first, last, state,
      [](const FlatStateEntry& entry, u32 needle) noexcept {
        return entry.state < needle;
      });
  if (hit == last || hit->state != state) {
    return nullptr;
  }
  return hit;
}

template <std::size_t MaxEntries>
constexpr u32 flatStateOr(const FlatStateSet<MaxEntries>& set,
                          u32 state,
                          u32 fallback) noexcept {
  if (const auto* entry = findFlatState(set, state)) {
    return entry->value;
  }
  return fallback;
}

struct FlatDrawStateKey {
  std::array<Handle, kMaxStreams> streamBuffers{};
  std::array<u32, kMaxStreams> streamOffsets{};
  std::array<u32, kMaxStreams> streamStrides{};
  std::array<u32, kMaxStreams> streamFrequencies{};
  u32 streamMask = 0;
  Handle indexBuffer{};
  u32 vertexElementCount = 0;
  u32 fvf = 0;
  u64 vertexDeclHash = 0;
  ShaderRef::Kind vertexShaderKind = ShaderRef::Kind::None;
  ShaderRef::Kind pixelShaderKind = ShaderRef::Kind::None;
  u64 vertexShaderHash = 0;
  u64 pixelShaderHash = 0;
  u64 vertexConstantsHash = 0;
  u64 pixelConstantsHash = 0;
  std::array<Handle, kMaxTextures> textures{};
  std::array<u32, kMaxTextures> textureLods{};
  u32 textureMask = 0;
  std::array<u64, kMaxTextureStages> textureStageStateHashes{};
  std::array<u64, kMaxSamplers> samplerStateHashes{};
  u32 samplerStateMask = 0;
  u64 renderStateHash = 0;
  std::array<RenderTargetAttachment, kMaxRenderTargets> colorAttachments{};
  RenderTargetAttachment depthStencil{};
  u32 renderTargetMask = 0;
  u64 viewportHash = 0;
  u64 worldViewProjHash = 0;
  u64 ffpBlendWorldViewProjHash = 0;
  u64 textureTransformsHash = 0;
  u32 nonIdentityTextureTransformStageMask = 0;
  u32 clipPlaneMask = 0;
  u64 clipPlanesHash = 0;

  friend constexpr bool operator==(const FlatDrawStateKey&, const FlatDrawStateKey&) = default;
};

using FlatBaseDrawStateSummary = FlatDrawStateKey;

struct FlatDrawStateRecord {
  FlatDrawStateKey key{};
  std::array<Handle, kMaxStreams> streamBuffers{};
  std::array<u32, kMaxStreams> streamOffsets{};
  std::array<u32, kMaxStreams> streamStrides{};
  std::array<u32, kMaxStreams> streamFrequencies{};
  u32 streamMask = 0;
  Handle indexBuffer{};
  std::array<Handle, kMaxTextures> textures{};
  std::array<u32, kMaxTextures> textureLods{};
  u32 textureMask = 0;
  FlatRenderStateSet renderStates{};
  std::array<FlatStateSet<kMaxFlatTextureStageStates>, kMaxTextureStages> textureStageStates{};
  std::array<FlatStateSet<kMaxSamplerStates>, kMaxSamplers> samplerStates{};
  std::array<RenderTargetAttachment, kMaxRenderTargets> colorAttachments{};
  RenderTargetAttachment depthStencil{};
  u32 renderTargetMask = 0;
  ViewportScissor viewport{};
  u64 vertexConstantsHash = 0;
  u64 pixelConstantsHash = 0;
  u64 worldViewProjHash = 0;
  u64 ffpBlendWorldViewProjHash = 0;
  u64 textureTransformsHash = 0;
  u32 nonIdentityTextureTransformStageMask = 0;
  u32 clipPlaneMask = 0;
  u64 clipPlanesHash = 0;

  friend constexpr bool operator==(const FlatDrawStateRecord&, const FlatDrawStateRecord&) = default;
};

struct DrawShaderLayoutContext {
  VertexDeclSnapshot vertexDecl{};
  ShaderRef vertexShader{};
  ShaderRef pixelShader{};
  ShaderConstantUsageBounds vertexConstantUsage{};
  ShaderConstantUsageBounds pixelConstantUsage{};
  u32 clipPlaneMask = 0;

  friend bool operator==(const DrawShaderLayoutContext&, const DrawShaderLayoutContext&) = default;
};

struct DrawUniformPayload {
  VertexShaderConstants vsConst{};
  PixelShaderConstants psConst{};
  Matrix4x4 worldViewProj{};
  Matrix4x4 ffpView{};
  Matrix4x4 ffpWorldView{};
  Matrix4x4 ffpNormalMatrix{};
  Material material{};
  std::array<Light, kMaxLights> lights{};
  std::array<Matrix4x4, 4> ffpBlendWorldViewProj{};
  std::array<Matrix4x4, 4> ffpBlendWorldView{};
  std::array<Matrix4x4, 4> ffpBlendNormalMatrix{};
  std::array<Matrix4x4, kMaxTextureStages> textureTransforms{};
  u32 clipPlaneMask = 0;
  std::array<ClipPlane, kMaxClipPlanes> clipPlanes{};
  u64 vertexConstantsHash = 0;
  u64 pixelConstantsHash = 0;
  u64 fixedPayloadHash = 0;
  u64 hash = 0;
  u16 vertexFloatConstantCount = 0;
  u16 vertexIntConstantCount = 0;
  u16 vertexBoolConstantCount = 0;
  u16 pixelFloatConstantCount = 0;
  u16 pixelIntConstantCount = 0;
  u16 pixelBoolConstantCount = 0;

  friend bool operator==(const DrawUniformPayload&, const DrawUniformPayload&) = default;
};

struct DrawUniformFixedPayload {
  Matrix4x4 worldViewProj{};
  Matrix4x4 ffpView{};
  Matrix4x4 ffpWorldView{};
  Matrix4x4 ffpNormalMatrix{};
  Material material{};
  std::array<Light, kMaxLights> lights{};
  std::array<Matrix4x4, 4> ffpBlendWorldViewProj{};
  std::array<Matrix4x4, 4> ffpBlendWorldView{};
  std::array<Matrix4x4, 4> ffpBlendNormalMatrix{};
  std::array<Matrix4x4, kMaxTextureStages> textureTransforms{};
  u32 clipPlaneMask = 0;
  std::array<ClipPlane, kMaxClipPlanes> clipPlanes{};

  friend bool operator==(const DrawUniformFixedPayload&,
                         const DrawUniformFixedPayload&) = default;
};

inline DrawUniformFixedPayload
makeDrawUniformFixedPayload(const DrawUniformPayload& payload) noexcept {
  return DrawUniformFixedPayload{
      .worldViewProj = payload.worldViewProj,
      .ffpView = payload.ffpView,
      .ffpWorldView = payload.ffpWorldView,
      .ffpNormalMatrix = payload.ffpNormalMatrix,
      .material = payload.material,
      .lights = payload.lights,
      .ffpBlendWorldViewProj = payload.ffpBlendWorldViewProj,
      .ffpBlendWorldView = payload.ffpBlendWorldView,
      .ffpBlendNormalMatrix = payload.ffpBlendNormalMatrix,
      .textureTransforms = payload.textureTransforms,
      .clipPlaneMask = payload.clipPlaneMask,
      .clipPlanes = payload.clipPlanes,
  };
}

struct DrawUniformStageConstantsSpan {
  std::uint32_t byteOffset = 0;
  std::uint32_t byteSize = 0;
  u16 floatCount = 0;
  u16 intCount = 0;
  u16 boolCount = 0;

  friend constexpr bool operator==(const DrawUniformStageConstantsSpan&,
                                   const DrawUniformStageConstantsSpan&) = default;
};

template <std::size_t FloatCount>
inline DrawUniformStageConstantsSpan makeDrawUniformStageConstantsSpan(
    const ShaderConstantSnapshot<FloatCount>& constants,
    u16 floatCount,
    u16 intCount,
    u16 boolCount,
    std::uint32_t byteOffset) noexcept {
  const auto clampedFloatCount =
      static_cast<u16>(std::min<std::size_t>(floatCount, constants.float4.size()));
  const auto clampedIntCount =
      static_cast<u16>(std::min<std::size_t>(intCount, constants.int4.size()));
  const auto clampedBoolCount =
      static_cast<u16>(std::min<std::size_t>(boolCount, constants.bools.size()));
  const auto byteSize =
      static_cast<std::uint64_t>(clampedFloatCount) * sizeof(constants.float4[0]) +
      static_cast<std::uint64_t>(clampedIntCount) * sizeof(constants.int4[0]) +
      static_cast<std::uint64_t>(clampedBoolCount) * sizeof(constants.bools[0]);
  constexpr auto kMaxU32 =
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
  return DrawUniformStageConstantsSpan{
      .byteOffset = byteOffset,
      .byteSize = byteSize <= kMaxU32
          ? static_cast<std::uint32_t>(byteSize)
          : std::numeric_limits<std::uint32_t>::max(),
      .floatCount = clampedFloatCount,
      .intCount = clampedIntCount,
      .boolCount = clampedBoolCount,
  };
}

inline DrawUniformStageConstantsSpan makeDrawUniformVertexConstantsSpan(
    const DrawUniformPayload& payload, std::uint32_t byteOffset) noexcept {
  u16 floatCount = payload.vertexFloatConstantCount;
  u16 intCount = payload.vertexIntConstantCount;
  const u16 boolCount = payload.vertexBoolConstantCount;
  if (boolCount != 0) {
    floatCount = static_cast<u16>(payload.vsConst.float4.size());
    intCount = static_cast<u16>(payload.vsConst.int4.size());
  } else if (intCount != 0) {
    floatCount = static_cast<u16>(payload.vsConst.float4.size());
  }
  return makeDrawUniformStageConstantsSpan(
      payload.vsConst,
      floatCount,
      intCount,
      boolCount,
      byteOffset);
}

inline DrawUniformStageConstantsSpan makeDrawUniformPixelConstantsSpan(
    const DrawUniformPayload& payload, std::uint32_t byteOffset) noexcept {
  u16 floatCount = payload.pixelFloatConstantCount;
  u16 intCount = payload.pixelIntConstantCount;
  const u16 boolCount = payload.pixelBoolConstantCount;
  if (boolCount != 0) {
    floatCount = static_cast<u16>(payload.psConst.float4.size());
    intCount = static_cast<u16>(payload.psConst.int4.size());
  } else if (intCount != 0) {
    floatCount = static_cast<u16>(payload.psConst.float4.size());
  }
  return makeDrawUniformStageConstantsSpan(
      payload.psConst,
      floatCount,
      intCount,
      boolCount,
      byteOffset);
}

struct DrawUniformPayloadHashes {
  u64 vertexConstantsHash = 0;
  u64 pixelConstantsHash = 0;
  std::uint64_t vertexConstantsBytes = 0;
  std::uint64_t pixelConstantsBytes = 0;
  u16 vertexFloatConstantCount = 0;
  u16 vertexIntConstantCount = 0;
  u16 vertexBoolConstantCount = 0;
  u16 pixelFloatConstantCount = 0;
  u16 pixelIntConstantCount = 0;
  u16 pixelBoolConstantCount = 0;
  u64 worldViewProjHash = 0;
  u64 ffpViewHash = 0;
  u64 ffpWorldViewHash = 0;
  u64 ffpNormalMatrixHash = 0;
  u64 materialHash = 0;
  std::array<u64, kMaxLights> lightHashes{};
  u64 ffpBlendWorldViewProjHash = 0;
  u64 ffpBlendWorldViewHash = 0;
  u64 ffpBlendNormalMatrixHash = 0;
  u64 textureTransformsHash = 0;
  u32 nonIdentityTextureTransformStageMask = 0;
  u64 clipPlanesHash = 0;
};

struct DrawUniformHandle {
  u32 index = 0;
  u32 generation = 0;
  u64 hash = 0;

  constexpr bool valid() const noexcept { return generation != 0; }
  friend constexpr bool operator==(const DrawUniformHandle&, const DrawUniformHandle&) = default;
};

struct DrawDebugSnapshot {
  PrimitiveType primitiveType = PrimitiveType::TriangleList;
  u32 primitiveCount = 0;
  u32 startVertex = 0;
  i32 baseVertexIndex = 0;
  u32 startIndex = 0;
  IndexType indexType = IndexType::UInt16;
  u32 userVertexBytes = 0;
  u32 userIndexBytes = 0;
  u32 streamMask = 0;
  u32 textureMask = 0;
  u32 samplerStateMask = 0;
  u32 renderTargetMask = 0;
  u64 renderStateHash = 0;
  u64 vertexDeclHash = 0;
  u64 vertexShaderHash = 0;
  u64 pixelShaderHash = 0;

  friend constexpr bool operator==(const DrawDebugSnapshot&, const DrawDebugSnapshot&) = default;
};

struct FlatDrawStateView {
  const FlatDrawStateRecord* hot = nullptr;
  const DrawShaderLayoutContext* shaderLayout = nullptr;
  const DrawUniformPayload* uniforms = nullptr;
  const DrawDebugSnapshot* debug = nullptr;

  constexpr const FlatDrawStateKey& key() const noexcept { return hot->key; }
  constexpr bool hasShaderContext() const noexcept { return shaderLayout != nullptr; }
  constexpr bool hasUniformPayload() const noexcept { return uniforms != nullptr; }
  constexpr bool hasDebugSnapshot() const noexcept { return debug != nullptr; }
  constexpr const DrawShaderLayoutContext& shaderContext() const noexcept { return *shaderLayout; }
  constexpr const DrawUniformPayload& uniformPayload() const noexcept { return *uniforms; }
  constexpr const DrawDebugSnapshot& debugSnapshot() const noexcept { return *debug; }
};

struct DrawPayloadRange {
  u32 offset = 0;
  u32 size = 0;

  constexpr bool empty() const noexcept { return size == 0; }
};

inline std::span<const u8> drawPayloadRangeBytes(
    DrawPayloadRange range,
    std::span<const u8> arena) noexcept {
  const std::size_t offset = range.offset;
  const std::size_t size = range.size;
  if (size == 0) {
    return {};
  }
  if (offset > arena.size() || size > arena.size() - offset) {
    return {};
  }
  return arena.subspan(offset, size);
}

struct DrawBufferBindingSnapshot {
  u64 metalHandle = 0;
  u64 contentsAddress = 0;
  u64 byteSize = 0;
  u64 contentRevision = 0;

  constexpr bool valid() const noexcept { return metalHandle != 0; }
};

enum class ChunkBufferBindingCaptureResult : u8 {
  Unsupported,
  Complete,
  MissingRequired,
};

struct ChunkBufferBindingSnapshot {
  Handle buffer{};
  DrawBufferBindingSnapshot snapshot{};
  // Opaque lease on the concrete rename-ring entry. The upper runtime owns
  // the token type; keeping this shared reference in RawCommandChunk makes
  // the pool reject that backing as a rename target until raw replay ends.
  std::shared_ptr<void> backingResidency;
  bool requiresCapturedBacking = false;
};

struct DrawStreamBindingOverride {
  Handle buffer{};
  u32 offset = 0;
  u32 stride = 0;
};

struct DrawBindingOverride {
  std::array<DrawStreamBindingOverride, kMaxStreams> streams{};
  u32 streamMask = 0;
  Handle indexBuffer{};
  IndexType indexType = IndexType::UInt16;
  bool indexBufferValid = false;
  // H228 — per-draw alpha-test immediate override. Raw D3DRS DWORD values
  // (RS_ALPHA_TEST_ENABLE / RS_ALPHA_FUNC / RS_ALPHA_REF); the dxmt9 encoder
  // converts them into the fragment FsVolatile immediate with
  // state::makeFsVolatile, single-sourcing the 0-255 -> float alphaRef
  // conversion with the FfpPsConsts upload. When alphaTestStateValid is set,
  // this draw's alpha-test trio is authoritative; when clear, the draw falls
  // back to the run's shared canonical render state. This is what lets draw
  // runs and submission batches span per-draw alpha-test toggles without a
  // PSO switch (alpha test is no longer a shader-variant key bit).
  u32 alphaTestEnable = 0;
  u32 alphaTestFunc = 0;
  u32 alphaTestRef = 0;
  bool alphaTestStateValid = false;
};
static_assert(std::is_trivially_copyable_v<DrawBindingOverride>,
              "DrawBindingOverride is serialized into draw-run payload bytes.");

struct DrawStreamBindingSnapshot {
  Handle buffer{};
  u32 offset = 0;
  u32 stride = 0;
  DrawBufferBindingSnapshot snapshot{};
};

struct DrawBindingSnapshot {
  std::array<DrawStreamBindingSnapshot, kMaxStreams> streams{};
  u32 streamMask = 0;
  Handle indexBuffer{};
  IndexType indexType = IndexType::UInt16;
  DrawBufferBindingSnapshot indexSnapshot{};
  bool indexSnapshotValid = false;
};
static_assert(std::is_trivially_copyable_v<DrawBindingSnapshot>,
              "DrawBindingSnapshot is serialized into draw-run payload bytes.");

struct CanonicalDrawState {
  FlatDrawStateRecord hot{};
  DrawShaderLayoutContext shaderLayout{};
  DrawDebugSnapshot debug{};

  CanonicalDrawState() = default;
  CanonicalDrawState(FlatDrawStateRecord hotState,
                     DrawShaderLayoutContext shaderLayoutState,
                     DrawDebugSnapshot debugState)
      : hot(std::move(hotState)),
        shaderLayout(std::move(shaderLayoutState)),
        debug(std::move(debugState)) {}

  constexpr FlatDrawStateView view() const noexcept {
    return FlatDrawStateView{
        .hot = &hot,
        .shaderLayout = &shaderLayout,
        .uniforms = nullptr,
        .debug = &debug,
    };
  }
};

// Per-draw parameters within an immediate draw-run submission: only what
// differs between draws sharing the same canonical state. Encoder emits one
// Metal draw call per DrawParam, reusing the bound state.
struct DrawParam {
  PrimitiveType primitiveType = PrimitiveType::TriangleList;
  u32 primitiveCount = 0;
  u32 startVertex = 0;
  i32 baseVertexIndex = 0;
  u32 startIndex = 0;
  IndexType indexType = IndexType::UInt16;
  bool indexed = false;                    // true when using drawIndexedPrimitive
  u32 instanceCount = 1;
  DrawPayloadRange userVertexRange{};      // draw-run payload slice, if present
  DrawPayloadRange userIndexRange{};       // draw-run payload slice, if present
  DrawPayloadRange bindingOverrideRange{}; // DrawBindingOverride payload slice
  DrawPayloadRange bindingSnapshotRange{}; // DrawBindingSnapshot payload slice
  DrawUniformHandle uniformHandle{};       // optional per-draw uniform snapshot
};
static_assert(std::is_trivially_copyable_v<DrawParam>,
              "DrawParam is hot-path draw metadata and must remain flat.");

struct DrawParamPayloadView {
  std::span<const u8> userVertexData{};
  std::span<const u8> userIndexData{};
  std::span<const u8> bindingOverrideData{};
  std::span<const u8> bindingSnapshotData{};
};

enum class DrawRunSubmissionStateLane : u8 {
  Unknown = 0,
  BindingAgnostic = 1,
  FullNoIndex = 2,
  FullWithIndex = 3,
};

struct DrawRunSubmission {
  std::optional<CanonicalDrawState> state{};
  std::optional<DrawUniformPayload> uniforms{};
  DrawParam draw{};
  DrawParamPayloadView payload{};
  DrawBindingOverride bindingOverride{};
  u64 stateGeneration = 0;
  u64 uniformGeneration = 0;
  u64 uniformFixedPayloadGeneration = 0;
  u64 uniformPayloadHash = 0;
  DrawRunSubmissionStateLane stateLane = DrawRunSubmissionStateLane::Unknown;
  bool stateMaterialized = true;

  CanonicalDrawState& materializedState() noexcept {
    DXMT_ASSERT(stateMaterialized && state.has_value());
    return *state;
  }
  const CanonicalDrawState& materializedState() const noexcept {
    DXMT_ASSERT(stateMaterialized && state.has_value());
    return *state;
  }
  DrawUniformPayload& uniformPayload() noexcept {
    DXMT_ASSERT(uniforms.has_value());
    return *uniforms;
  }
  const DrawUniformPayload& uniformPayload() const noexcept {
    DXMT_ASSERT(uniforms.has_value());
    return *uniforms;
  }
};

inline std::span<const u8> drawBindingOverrideBytes(
    const DrawBindingOverride& binding) noexcept {
  return std::span<const u8>(reinterpret_cast<const u8*>(&binding),
                             sizeof(binding));
}

inline std::span<const u8> drawBindingSnapshotBytes(
    const DrawBindingSnapshot& binding) noexcept {
  return std::span<const u8>(reinterpret_cast<const u8*>(&binding),
                             sizeof(binding));
}

inline bool drawBindingOverrideEmpty(
    const DrawBindingOverride& binding) noexcept {
  return binding.streamMask == 0 && !binding.indexBufferValid &&
         !binding.alphaTestStateValid;
}

// H228 — true when the override carries stream/index binding content (as
// opposed to only a per-draw alpha-test record). Run replay uses this to
// skip the hot-state copy + binding rewrite for alpha-only overrides.
inline bool drawBindingOverrideHasBindings(
    const DrawBindingOverride& binding) noexcept {
  return binding.streamMask != 0 || binding.indexBufferValid;
}

inline bool drawBindingSnapshotEmpty(
    const DrawBindingSnapshot& binding) noexcept {
  return binding.streamMask == 0 && !binding.indexSnapshotValid;
}

inline void clearDrawStateBindingFields(FlatDrawStateRecord& hot) noexcept {
  hot.streamBuffers = {};
  hot.streamOffsets = {};
  hot.streamStrides = {};
  hot.streamMask = 0;
  hot.indexBuffer = {};
  hot.key.streamBuffers = {};
  hot.key.streamOffsets = {};
  hot.key.streamStrides = {};
  hot.key.streamMask = 0;
  hot.key.indexBuffer = {};
}

inline void clearDrawShaderLayoutBindingFields(
    DrawShaderLayoutContext& shaderLayout) noexcept {
  shaderLayout.vertexDecl.streams = {};
}

template <typename Submission>
inline bool drawRunSubmissionHasExternalBindingOverride(
    const Submission& submission) noexcept {
  return !submission.payload.bindingOverrideData.empty() ||
         !drawBindingOverrideEmpty(submission.bindingOverride);
}

template <typename Submission>
inline void ensureDrawRunSubmissionBindingOverridePayload(
    Submission& submission) noexcept {
  if (submission.payload.bindingOverrideData.empty() &&
      !drawBindingOverrideEmpty(submission.bindingOverride)) {
    submission.payload.bindingOverrideData =
        drawBindingOverrideBytes(submission.bindingOverride);
  }
}

// H228 — the alpha-test render-state trio is exempt from draw-run batch
// compatibility: alpha test is evaluated from per-draw FsVolatile immediates
// in a single fragment-shader variant (the PSO no longer splits on it), and
// each batched submission carries its own trio through DrawBindingOverride.
// The exemption is bounded to exactly these three registers; every other
// render state still breaks a batch.
inline bool renderStateIsAlphaTestTrio(u32 rs) noexcept {
  return rs == RS_ALPHA_TEST_ENABLE || rs == RS_ALPHA_REF ||
         rs == RS_ALPHA_FUNC;
}

inline bool flatRenderStatesEqualExceptAlphaTest(
    const FlatRenderStateSet& a,
    const FlatRenderStateSet& b) noexcept {
  if (a.overflow || b.overflow) {
    // Overflowed sets dropped entries; only exact equality is provable.
    return a == b;
  }
  const auto* pa = a.entries.data();
  const auto* ea = pa + (a.count <= a.entries.size() ? a.count : a.entries.size());
  const auto* pb = b.entries.data();
  const auto* eb = pb + (b.count <= b.entries.size() ? b.count : b.entries.size());
  while (pa != ea || pb != eb) {
    if (pb == eb || (pa != ea && pa->state < pb->state)) {
      if (!renderStateIsAlphaTestTrio(pa->state)) return false;
      ++pa;
    } else if (pa == ea || pb->state < pa->state) {
      if (!renderStateIsAlphaTestTrio(pb->state)) return false;
      ++pb;
    } else {
      if (pa->value != pb->value && !renderStateIsAlphaTestTrio(pa->state)) {
        return false;
      }
      ++pa;
      ++pb;
    }
  }
  return true;
}

// Key comparison shared by the exact and alpha-trio-exempt predicates. The
// renderStateHash term is intentionally excluded here: the batch predicate
// (H228) compares the actual flat render-state sets with the alpha trio
// exempt instead, while drawStateKeysCompatibleForDrawRunBatch re-adds the
// exact hash term for key-only callers (e.g. the encoder's skipBaseStateBind
// check, which has no entry sets to compare).
inline bool drawStateKeysCompatibleForDrawRunBatchExceptRenderState(
    const FlatDrawStateKey& a,
    const FlatDrawStateKey& b) noexcept {
  return a.vertexElementCount == b.vertexElementCount &&
         a.fvf == b.fvf &&
         a.vertexDeclHash == b.vertexDeclHash &&
         a.vertexShaderKind == b.vertexShaderKind &&
         a.pixelShaderKind == b.pixelShaderKind &&
         a.vertexShaderHash == b.vertexShaderHash &&
         a.pixelShaderHash == b.pixelShaderHash &&
         a.textures == b.textures &&
         a.textureLods == b.textureLods &&
         a.textureMask == b.textureMask &&
         a.textureStageStateHashes == b.textureStageStateHashes &&
         a.samplerStateHashes == b.samplerStateHashes &&
         a.samplerStateMask == b.samplerStateMask &&
         a.colorAttachments == b.colorAttachments &&
         a.depthStencil == b.depthStencil &&
         a.renderTargetMask == b.renderTargetMask &&
         a.viewportHash == b.viewportHash &&
         a.clipPlaneMask == b.clipPlaneMask;
}

inline bool drawStateKeysCompatibleForDrawRunBatch(
    const FlatDrawStateKey& a,
    const FlatDrawStateKey& b) noexcept {
  return drawStateKeysCompatibleForDrawRunBatchExceptRenderState(a, b) &&
         a.renderStateHash == b.renderStateHash;
}

inline bool drawStatesCompatibleForDrawRunBatch(
    const FlatDrawStateRecord& a,
    const FlatDrawStateRecord& b) noexcept {
  // H228 — render state is compared entry-wise with the alpha trio exempt;
  // the key renderStateHash term is replaced by that comparison (PSO identity
  // no longer depends on the trio because the alpha-test variant-key bit is
  // gone). Every other group keeps exact equality.
  return drawStateKeysCompatibleForDrawRunBatchExceptRenderState(a.key, b.key) &&
         a.textures == b.textures &&
         a.textureLods == b.textureLods &&
         a.textureMask == b.textureMask &&
         flatRenderStatesEqualExceptAlphaTest(a.renderStates, b.renderStates) &&
         a.textureStageStates == b.textureStageStates &&
         a.samplerStates == b.samplerStates &&
         a.colorAttachments == b.colorAttachments &&
         a.depthStencil == b.depthStencil &&
         a.renderTargetMask == b.renderTargetMask &&
         a.viewport == b.viewport &&
         a.clipPlaneMask == b.clipPlaneMask;
}

inline bool shaderLayoutsCompatibleForDrawRunBatch(
    const DrawShaderLayoutContext& a,
    const DrawShaderLayoutContext& b) noexcept {
  return a.vertexDecl.elements == b.vertexDecl.elements &&
         a.vertexDecl.fvf == b.vertexDecl.fvf &&
         a.vertexShader == b.vertexShader &&
         a.pixelShader == b.pixelShader &&
         a.vertexConstantUsage == b.vertexConstantUsage &&
         a.pixelConstantUsage == b.pixelConstantUsage &&
         a.clipPlaneMask == b.clipPlaneMask;
}

// Diagnostic classification for batch-incompatible submission pairs: names
// the first differing state group (ordered roughly by expected churn) so
// run-break attribution can decide which group is worth an override/widening
// mechanism. `textureOnly` is the precise claim that extending the existing
// binding-override machinery to texture bindings would have kept the pair
// batchable: every non-texture group is equal and a texture group differs.
enum class DrawRunBatchIncompatClass : std::uint8_t {
  Texture,
  Sampler,
  TextureStageState,
  RenderState,
  Shader,
  VertexDecl,
  Attachment,
  Viewport,
  ClipPlane,
  LayoutUsage,
  Unknown,
};

// Sub-classification for RenderState incompatibility: names the register
// group the differing entries fall into, so run-break attribution can tell a
// per-draw alpha-test/blend toggle (PSO-relevant, genuinely unbatchable) from
// an encoder-dynamic register that an override lane could absorb.
enum class DrawRunBatchRenderStateDiffClass : std::uint8_t {
  AlphaTestOnly,
  BlendOnly,
  CullOnly,
  DepthOnly,
  FogOnly,
  TextureFactorOnly,
  SingleOther,
  Mixed,
  None,
};

struct DrawRunBatchIncompat {
  DrawRunBatchIncompatClass firstDiff = DrawRunBatchIncompatClass::Unknown;
  bool textureOnly = false;
  DrawRunBatchRenderStateDiffClass renderStateDiff =
      DrawRunBatchRenderStateDiffClass::None;
  u32 renderStateDiffFirstRegister = 0;
};

inline DrawRunBatchRenderStateDiffClass classifyDrawRunBatchRenderStateDiff(
    const FlatRenderStateSet& a,
    const FlatRenderStateSet& b,
    u32* firstRegister) noexcept {
  const auto groupOf = [](u32 rs) -> std::uint8_t {
    switch (rs) {
    case RS_ALPHA_TEST_ENABLE:
    case RS_ALPHA_REF:
    case RS_ALPHA_FUNC:
      return 0;  // alpha test
    case RS_ALPHABLEND_ENABLE:
    case RS_SRC_BLEND:
    case RS_DEST_BLEND:
    case RS_BLEND_OP:
    case RS_BLEND_FACTOR:
    case RS_SEPARATE_ALPHA_BLEND_ENABLE:
    case RS_SRC_BLEND_ALPHA:
    case RS_DEST_BLEND_ALPHA:
    case RS_BLEND_OP_ALPHA:
      return 1;  // blend
    case RS_CULL_MODE:
      return 2;  // cull
    case RS_Z_ENABLE:
    case RS_Z_WRITE_ENABLE:
    case RS_Z_FUNC:
    case RS_DEPTH_BIAS:
    case RS_SLOPE_SCALE_DEPTH_BIAS:
      return 3;  // depth
    case RS_FOG_ENABLE:
    case RS_FOG_TABLE_MODE:
    case RS_FOG_FROM_VERTEX:
    case RS_FOG_COLOR:
    case RS_FOG_START:
    case RS_FOG_END:
    case RS_FOG_DENSITY:
      return 4;  // fog
    case RS_TEXTURE_FACTOR:
      return 5;  // tfactor
    default:
      return 6;  // other
    }
  };
  std::uint32_t groupMask = 0;
  std::uint32_t diffCount = 0;
  u32 first = 0;
  const auto note = [&](u32 rs) {
    if (diffCount == 0) first = rs;
    ++diffCount;
    groupMask |= 1u << groupOf(rs);
  };
  const auto* pa = a.entries.data();
  const auto* ea = pa + (a.count <= a.entries.size() ? a.count : a.entries.size());
  const auto* pb = b.entries.data();
  const auto* eb = pb + (b.count <= b.entries.size() ? b.count : b.entries.size());
  while (pa != ea || pb != eb) {
    if (pb == eb || (pa != ea && pa->state < pb->state)) {
      note(pa->state); ++pa;
    } else if (pa == ea || pb->state < pa->state) {
      note(pb->state); ++pb;
    } else {
      if (pa->value != pb->value) note(pa->state);
      ++pa; ++pb;
    }
  }
  if (firstRegister) *firstRegister = first;
  if (diffCount == 0) return DrawRunBatchRenderStateDiffClass::None;
  const bool single = (groupMask & (groupMask - 1)) == 0;
  if (!single) return DrawRunBatchRenderStateDiffClass::Mixed;
  switch (groupMask) {
  case 1u << 0: return DrawRunBatchRenderStateDiffClass::AlphaTestOnly;
  case 1u << 1: return DrawRunBatchRenderStateDiffClass::BlendOnly;
  case 1u << 2: return DrawRunBatchRenderStateDiffClass::CullOnly;
  case 1u << 3: return DrawRunBatchRenderStateDiffClass::DepthOnly;
  case 1u << 4: return DrawRunBatchRenderStateDiffClass::FogOnly;
  case 1u << 5: return DrawRunBatchRenderStateDiffClass::TextureFactorOnly;
  default:
    return diffCount == 1 ? DrawRunBatchRenderStateDiffClass::SingleOther
                          : DrawRunBatchRenderStateDiffClass::Mixed;
  }
}

inline DrawRunBatchIncompat classifyDrawRunBatchIncompatibility(
    const FlatDrawStateRecord& a,
    const FlatDrawStateRecord& b,
    const DrawShaderLayoutContext& la,
    const DrawShaderLayoutContext& lb) noexcept {
  const bool textureEq = a.key.textures == b.key.textures &&
                         a.key.textureLods == b.key.textureLods &&
                         a.key.textureMask == b.key.textureMask &&
                         a.textures == b.textures &&
                         a.textureLods == b.textureLods &&
                         a.textureMask == b.textureMask;
  const bool samplerEq = a.key.samplerStateHashes == b.key.samplerStateHashes &&
                         a.key.samplerStateMask == b.key.samplerStateMask &&
                         a.samplerStates == b.samplerStates;
  const bool tssEq = a.key.textureStageStateHashes == b.key.textureStageStateHashes &&
                     a.textureStageStates == b.textureStageStates;
  const bool renderStateEq = a.key.renderStateHash == b.key.renderStateHash &&
                             a.renderStates == b.renderStates;
  const bool shaderEq = a.key.vertexShaderKind == b.key.vertexShaderKind &&
                        a.key.pixelShaderKind == b.key.pixelShaderKind &&
                        a.key.vertexShaderHash == b.key.vertexShaderHash &&
                        a.key.pixelShaderHash == b.key.pixelShaderHash &&
                        la.vertexShader == lb.vertexShader &&
                        la.pixelShader == lb.pixelShader;
  const bool declEq = a.key.vertexElementCount == b.key.vertexElementCount &&
                      a.key.fvf == b.key.fvf &&
                      a.key.vertexDeclHash == b.key.vertexDeclHash &&
                      la.vertexDecl.elements == lb.vertexDecl.elements &&
                      la.vertexDecl.fvf == lb.vertexDecl.fvf;
  const bool attachmentEq = a.key.colorAttachments == b.key.colorAttachments &&
                            a.key.depthStencil == b.key.depthStencil &&
                            a.key.renderTargetMask == b.key.renderTargetMask &&
                            a.colorAttachments == b.colorAttachments &&
                            a.depthStencil == b.depthStencil &&
                            a.renderTargetMask == b.renderTargetMask;
  const bool viewportEq = a.key.viewportHash == b.key.viewportHash &&
                          a.viewport == b.viewport;
  const bool clipEq = a.key.clipPlaneMask == b.key.clipPlaneMask &&
                      a.clipPlaneMask == b.clipPlaneMask &&
                      la.clipPlaneMask == lb.clipPlaneMask;
  const bool usageEq = la.vertexConstantUsage == lb.vertexConstantUsage &&
                       la.pixelConstantUsage == lb.pixelConstantUsage;
  DrawRunBatchIncompat result{};
  if (!textureEq) result.firstDiff = DrawRunBatchIncompatClass::Texture;
  else if (!samplerEq) result.firstDiff = DrawRunBatchIncompatClass::Sampler;
  else if (!tssEq) result.firstDiff = DrawRunBatchIncompatClass::TextureStageState;
  else if (!renderStateEq) result.firstDiff = DrawRunBatchIncompatClass::RenderState;
  else if (!shaderEq) result.firstDiff = DrawRunBatchIncompatClass::Shader;
  else if (!declEq) result.firstDiff = DrawRunBatchIncompatClass::VertexDecl;
  else if (!attachmentEq) result.firstDiff = DrawRunBatchIncompatClass::Attachment;
  else if (!viewportEq) result.firstDiff = DrawRunBatchIncompatClass::Viewport;
  else if (!clipEq) result.firstDiff = DrawRunBatchIncompatClass::ClipPlane;
  else if (!usageEq) result.firstDiff = DrawRunBatchIncompatClass::LayoutUsage;
  result.textureOnly = !textureEq && samplerEq && tssEq && renderStateEq &&
                       shaderEq && declEq && attachmentEq && viewportEq &&
                       clipEq && usageEq;
  // Sub-classify the render-state diff only when render state is the SOLE
  // differing group; otherwise the rs_* counters overstate rescuable pairs
  // (H228 lesson: GT2's "alpha-test-only" pairs also switched shaders).
  if (!renderStateEq && textureEq && samplerEq && tssEq && shaderEq &&
      declEq && attachmentEq && viewportEq && clipEq && usageEq) {
    result.renderStateDiff = classifyDrawRunBatchRenderStateDiff(
        a.renderStates, b.renderStates, &result.renderStateDiffFirstRegister);
  }
  return result;
}

template <typename Submission>
inline bool drawRunSubmissionStatesCompatibleForBatch(
    const Submission& a,
    const Submission& b) noexcept {
  const auto& aState = a.materializedState();
  const auto& bState = b.materializedState();
  return drawStatesCompatibleForDrawRunBatch(aState.hot, bState.hot) &&
         shaderLayoutsCompatibleForDrawRunBatch(aState.shaderLayout,
                                                bState.shaderLayout);
}

template <typename SubmissionA, typename SubmissionB>
inline bool drawRunSubmissionSameStateGenerationLane(
    const SubmissionA& a,
    const SubmissionB& b) noexcept {
  return a.stateGeneration != 0 &&
         a.stateGeneration == b.stateGeneration &&
         a.stateLane != DrawRunSubmissionStateLane::Unknown &&
         a.stateLane == b.stateLane;
}

template <typename Submission>
inline bool drawRunSubmissionUsesAcceptedPreviousStateGenerationLane(
    const Submission& base,
    const Submission& previous,
    const Submission& candidate) noexcept {
  return !drawRunSubmissionSameStateGenerationLane(base, candidate) &&
         !candidate.stateMaterialized &&
         drawRunSubmissionSameStateGenerationLane(previous, candidate);
}

inline std::uint64_t drawRunSubmissionStateCopyBytes() noexcept {
  return sizeof(FlatDrawStateRecord) + sizeof(DrawShaderLayoutContext);
}

inline std::uint64_t drawRunSubmissionUniformCopyBytes() noexcept {
  return sizeof(DrawUniformPayload);
}

inline std::uint64_t drawRunSubmissionCarrierBytes() noexcept {
  return sizeof(DrawRunSubmission);
}

inline std::uint64_t drawRunSubmissionCarrierStateStorageBytes() noexcept {
  return sizeof(std::optional<CanonicalDrawState>);
}

inline std::uint64_t drawRunSubmissionCarrierUniformStorageBytes() noexcept {
  return sizeof(std::optional<DrawUniformPayload>);
}

template <typename SubmissionA, typename SubmissionB>
inline bool drawRunSubmissionSameUniformGeneration(
    const SubmissionA& a,
    const SubmissionB& b) noexcept {
  return a.uniformGeneration != 0 &&
         a.uniformGeneration == b.uniformGeneration;
}

template <typename SubmissionA, typename SubmissionB>
inline bool drawRunSubmissionSameUniformPayloadHash(
    const SubmissionA& a,
    const SubmissionB& b) noexcept {
  return a.uniformPayloadHash != 0 &&
         a.uniformPayloadHash == b.uniformPayloadHash;
}

template <typename Submission>
inline void prepareDrawRunSubmissionBindingOverride(
    const Submission& base,
    Submission& submission) noexcept {
  if (drawRunSubmissionHasExternalBindingOverride(submission)) {
    ensureDrawRunSubmissionBindingOverridePayload(submission);
    return;
  }

  const auto& baseState = base.materializedState();
  const auto& submissionState = submission.materializedState();
  submission.bindingOverride = {};
  for (u32 stream = 0; stream < kMaxStreams; ++stream) {
    if (baseState.hot.streamBuffers[stream] == submissionState.hot.streamBuffers[stream] &&
        baseState.hot.streamOffsets[stream] == submissionState.hot.streamOffsets[stream] &&
        baseState.hot.streamStrides[stream] == submissionState.hot.streamStrides[stream]) {
      continue;
    }
    submission.bindingOverride.streamMask |= 1u << stream;
    submission.bindingOverride.streams[stream] = DrawStreamBindingOverride{
        .buffer = submissionState.hot.streamBuffers[stream],
        .offset = submissionState.hot.streamOffsets[stream],
        .stride = submissionState.hot.streamStrides[stream],
    };
  }

  if (baseState.hot.indexBuffer != submissionState.hot.indexBuffer ||
      base.draw.indexType != submission.draw.indexType) {
    submission.bindingOverride.indexBuffer = submissionState.hot.indexBuffer;
    submission.bindingOverride.indexType = submission.draw.indexType;
    submission.bindingOverride.indexBufferValid = true;
  }

  // H228 — batched submissions share the base's canonical state, so a draw
  // whose alpha-test trio differs from the base must carry its own values as
  // a per-draw immediate override (the batch predicate exempts exactly this
  // trio). Raw D3DRS values; defaults mirror fillFfpPsConsts' reads.
  const auto alphaTrioOf = [](const FlatRenderStateSet& rs) {
    struct Trio { u32 enable, func, ref; };
    return Trio{
        flatStateOr(rs, RS_ALPHA_TEST_ENABLE, 0u),
        flatStateOr(rs, RS_ALPHA_FUNC, static_cast<u32>(CompareFunc::Always)),
        flatStateOr(rs, RS_ALPHA_REF, 0u),
    };
  };
  const auto baseTrio = alphaTrioOf(baseState.hot.renderStates);
  const auto submissionTrio = alphaTrioOf(submissionState.hot.renderStates);
  if (baseTrio.enable != submissionTrio.enable ||
      baseTrio.func != submissionTrio.func ||
      baseTrio.ref != submissionTrio.ref) {
    submission.bindingOverride.alphaTestEnable = submissionTrio.enable;
    submission.bindingOverride.alphaTestFunc = submissionTrio.func;
    submission.bindingOverride.alphaTestRef = submissionTrio.ref;
    submission.bindingOverride.alphaTestStateValid = true;
  }

  if (!drawBindingOverrideEmpty(submission.bindingOverride)) {
    ensureDrawRunSubmissionBindingOverridePayload(submission);
  }
}

namespace fixture {

// Fixture/offline draw shape kept out of production draw submission. Runtime
// code should use CanonicalDrawState + DrawParam spans instead.
struct DrawDesc {
  PrimitiveType primitiveType = PrimitiveType::TriangleList;
  u32 primitiveCount = 0;
  u32 startVertex = 0;
  i32 baseVertexIndex = 0;
  u32 startIndex = 0;
  Handle indexBuffer{};
  IndexType indexType = IndexType::UInt16;
  VertexDeclSnapshot vertexDecl{};
  std::array<u32, kMaxStreams> streamFrequencies = [] {
    std::array<u32, kMaxStreams> frequencies{};
    frequencies.fill(1);
    return frequencies;
  }();
  ShaderRef vertexShader{};
  ShaderRef pixelShader{};
  VertexShaderConstants vsConst{};
  PixelShaderConstants psConst{};
  std::array<TextureBinding, kMaxTextures> textures{};
  std::array<SamplerSnapshot, kMaxSamplers> samplers{};
  RenderStateSnapshot rs{};
  RenderTargetSnapshot rts{};
  ViewportScissor viewport{};
  Matrix4x4 worldViewProj{};
  Matrix4x4 ffpView{};
  Matrix4x4 ffpWorldView{};
  Matrix4x4 ffpNormalMatrix{};
  Material material{};
  std::array<Light, kMaxLights> lights{};
  std::array<Matrix4x4, 4> ffpBlendWorldViewProj{};
  std::array<Matrix4x4, 4> ffpBlendWorldView{};
  std::array<Matrix4x4, 4> ffpBlendNormalMatrix{};
  std::array<Matrix4x4, kMaxTextureStages> textureTransforms{};
  u32 clipPlaneMask = 0;
  std::array<ClipPlane, kMaxClipPlanes> clipPlanes{};
  std::vector<u8> userVertexData;
  std::vector<u8> userIndexData;
};

}  // namespace fixture

namespace detail {

constexpr std::size_t kDrawRunInlineParamCapacity = 4;
constexpr std::size_t kDrawRunInlinePayloadCapacity = 512;

struct DrawParamInlineStorage {
  std::array<DrawParam, kDrawRunInlineParamCapacity> inlineData{};
  std::vector<DrawParam> overflow;
  std::size_t inlineSize = 0;
  bool overflowMode = false;
};

struct DrawPayloadArenaStorage {
  std::array<u8, kDrawRunInlinePayloadCapacity> inlineData{};
  std::vector<u8> overflow;
  std::size_t inlineSize = 0;
  bool overflowMode = false;
};

struct DrawRunScratchStorage {
  DrawParamInlineStorage draws{};
  DrawPayloadArenaStorage payload{};
};

}  // namespace detail

namespace fixture {

struct DrawRunView {
  std::span<const DrawParam> draws{};
  std::span<const u8> payloadArena{};
  const DrawUniformPayload* uniforms = nullptr;
  DrawUniformHandle uniformHandleCandidate{};
};

// Fixture/offline packed draw-run shape. Production submission uses
// CanonicalDrawState + DrawParam spans directly; this type only exercises
// packing/range helpers in tests.
struct DrawRunDesc {
  CanonicalDrawState state{};              // applied once at run start

 private:
  detail::DrawRunScratchStorage scratch_{}; // packed per-draw args + UP payload bytes
  const DrawUniformPayload* uniforms_ = nullptr; // borrowed by fixture packing helpers
  DrawUniformHandle uniformHandleCandidate_{};

  friend void drawRunClear(DrawRunDesc& run);
  friend void drawRunReserve(DrawRunDesc& run, std::size_t drawCount, std::size_t payloadBytes);
  friend void drawRunSetUniformPayload(DrawRunDesc& run, const DrawUniformPayload& payload) noexcept;
  friend void drawRunSetUniformHandleCandidate(DrawRunDesc& run, DrawUniformHandle handle) noexcept;
  friend bool drawRunAppend(DrawRunDesc& run, DrawParam param, DrawParamPayloadView payload);
  friend DrawRunView drawRunView(const DrawRunDesc& run) noexcept;
  friend const DrawUniformPayload& drawRunUniformPayload(const DrawRunDesc& run) noexcept;
  friend bool drawRunEmpty(const DrawRunDesc& run) noexcept;
  friend std::size_t drawRunDrawCount(const DrawRunDesc& run) noexcept;
  friend std::size_t drawRunPayloadSize(const DrawRunDesc& run) noexcept;
  friend std::span<const DrawParam> drawRunDraws(const DrawRunDesc& run) noexcept;
  friend std::span<const u8> drawRunPayloadArena(const DrawRunDesc& run) noexcept;
};

}  // namespace fixture

class Device;
class Buffer;
class Texture;
class Surface;
class Query;
class StateBlock;
class SwapChain;

class BackendDevice {
 public:
  using DeviceLostObserver = std::function<void(bool)>;
  using PresentationStatusObserver = std::function<void(bool)>;

  virtual ~BackendDevice() = default;

  // Resource CRUD: all default to no-op returning empty handles. Production
  // routes through dxmt9::Device (DeviceImpl implements these against the
  // dxmt9::resources::Pool it owns). BackendDevice-side overrides remain
  // only on MockBackendDevice for tests that assert on BackendDevice
  // behavior directly.
  virtual BufferHandle createBuffer(const BufferDesc& desc) {
    (void)desc;
    return {};
  }
  virtual TextureHandle createTexture(const TextureDesc& desc) {
    (void)desc;
    return {};
  }
  virtual SurfaceHandle createSurface(const SurfaceDesc& desc) {
    (void)desc;
    return {};
  }
  virtual SurfaceHandle createSurfaceForTexture(TextureHandle texture, u32 level, const SurfaceDesc& desc) {
    (void)texture;
    (void)level;
    (void)desc;
    return {};
  }
  virtual void destroyBuffer(BufferHandle handle) { (void)handle; }
  virtual void destroyTexture(TextureHandle handle) { (void)handle; }
  virtual void destroySurface(SurfaceHandle handle) { (void)handle; }
  virtual void* mapBuffer(BufferHandle handle, u32 flags) {
    (void)handle;
    (void)flags;
    return nullptr;
  }
  virtual void unmapBuffer(BufferHandle handle) { (void)handle; }
  virtual void uploadBufferData(BufferHandle handle, std::span<const u8> bytes) {
    (void)handle;
    (void)bytes;
  }
  // Upload a bounded byte range into a buffer. The complete shadow is passed
  // so legacy backends can safely retain their full-upload behavior; the
  // production device consumes only [offset, offset + byteCount).
  virtual void uploadBufferDataRange(BufferHandle handle,
                                     std::span<const u8> fullBytes,
                                     u64 offset,
                                     u64 byteCount) {
    (void)offset;
    (void)byteCount;
    uploadBufferData(handle, fullBytes);
  }
  virtual void uploadTextureLevel(TextureHandle handle, u32 level, u32 width, u32 height,
                                  u32 depth, u32 pitch, u32 slicePitch,
                                  std::span<const u8> bytes) {
    (void)handle;
    (void)level;
    (void)width;
    (void)height;
    (void)depth;
    (void)pitch;
    (void)slicePitch;
    (void)bytes;
  }
  // Immediate draw-run submission. `draws`, `payloads`, and `uniforms` are
  // borrowed for this call only; implementations must copy them into their own
  // queue/storage before returning and must never retain the spans.
  virtual void submitDrawRun(CanonicalDrawState state, const DrawUniformPayload& uniforms,
                             std::span<const DrawParam> draws,
                             std::span<const DrawParamPayloadView> payloads) {
    (void)state;
    (void)uniforms;
    (void)draws;
    (void)payloads;
  }
  virtual void submitClear(const ClearDesc& desc) { (void)desc; }
  virtual void submitSurfaceCopy(const SurfaceCopyDesc& desc) { (void)desc; }
  virtual void submitStretchRect(const StretchRectDesc& desc) { (void)desc; }
  virtual void submitReadback(const ReadbackDesc& desc) { (void)desc; }
  virtual void submitColorFill(const ColorFillDesc& desc) { (void)desc; }
  virtual void submitDepthResolve(const DepthResolveDesc& desc) { (void)desc; }
  virtual void present(const SwapDesc& desc) { (void)desc; }
  virtual void setDeviceLostObserver(DeviceLostObserver observer) { (void)observer; }
  virtual void setPresentationStatusObserver(PresentationStatusObserver observer) { (void)observer; }
  virtual void setMaxFrameLatency(u32 latency) { (void)latency; }
  virtual HResult waitForVBlank(const SwapDesc& desc) {
    (void)desc;
    return D3D_OK;
  }
  virtual bool readbackSurface(const ReadbackDesc& desc, ReadbackPixels& pixels) {
    (void)desc;
    (void)pixels;
    return false;
  }
  virtual void flush() {}
};

struct DeviceState;

// Canonical FFP variant-key builders. Determinism contract: for any
// `DeviceState` value `s`, repeated invocations `makeFfpVertexKey(s)` /
// `makeFfpPixelKey(s)` produce keys whose member bytes and `hash` are
// equal, and whose `operator==` returns true. Conversely, any single
// D3D9 render-state / texture-stage-state bit that the key reads MUST
// flip the resulting key to compare unequal — otherwise PSO cache lookups
// would silently merge unrelated shader variants. See the comments on
// `FfpVertexKey` / `FfpPixelKey` in `core_constants.hpp`. Implementation
// lives in `src/d3d9/core_draw.cpp`; regression-guarded by
// `tests/native/core/core_ffp_state_key_spec.cpp` and
// `tests/native/backend/ffp_key_determinism_spec.cpp`.
FfpVertexKey makeFfpVertexKey(const DeviceState& state);
FfpPixelKey makeFfpPixelKey(const DeviceState& state);

namespace fixture {

DrawDesc makeDrawDescFromState(const DeviceState& state, const DrawCallArgs& args);
FlatDrawStateKey makeFlatDrawStateKey(const DrawDesc& desc);
FlatDrawStateRecord makeFlatDrawStateRecord(const DrawDesc& desc);
DrawShaderLayoutContext makeDrawShaderLayoutContext(const DrawDesc& desc);
DrawUniformPayload makeDrawUniformPayload(const DrawDesc& desc);
DrawDebugSnapshot makeDrawDebugSnapshot(const DrawDesc& desc, const FlatDrawStateRecord& hot);

void drawRunClear(DrawRunDesc& run);
void drawRunReserve(DrawRunDesc& run, std::size_t drawCount, std::size_t payloadBytes);
bool drawRunAppend(DrawRunDesc& run, DrawParam param,
                   DrawParamPayloadView payload = {});
DrawRunView drawRunView(const DrawRunDesc& run) noexcept;
void drawRunSetUniformPayload(DrawRunDesc& run, const DrawUniformPayload& payload) noexcept;
void drawRunSetUniformPayload(DrawRunDesc& run, DrawUniformPayload&& payload) = delete;
void drawRunSetUniformHandleCandidate(DrawRunDesc& run, DrawUniformHandle handle) noexcept;
const DrawUniformPayload& drawRunUniformPayload(const DrawRunDesc& run) noexcept;
bool drawRunEmpty(const DrawRunDesc& run) noexcept;
std::size_t drawRunDrawCount(const DrawRunDesc& run) noexcept;
std::size_t drawRunPayloadSize(const DrawRunDesc& run) noexcept;
std::span<const DrawParam> drawRunDraws(const DrawRunDesc& run) noexcept;
std::span<const u8> drawRunPayloadArena(const DrawRunDesc& run) noexcept;
std::span<const u8> drawRunPayloadBytes(const DrawRunDesc& run,
                                        DrawPayloadRange range) noexcept;
bool drawRunValidate(const DrawRunDesc& run) noexcept;

}  // namespace fixture

CanonicalDrawState makeCanonicalDrawStateFromState(const DeviceState& state, const DrawCallArgs& args);

inline std::span<const u8> drawRunPayloadBytes(DrawPayloadRange range,
                                               std::span<const u8> arena) noexcept {
  const std::size_t offset = range.offset;
  const std::size_t size = range.size;
  if (size == 0) {
    return {};
  }
  if (offset > arena.size() || size > arena.size() - offset) {
    return {};
  }
  return arena.subspan(offset, size);
}

struct DeviceState {
  Viewport viewport{};
  Rect scissorRect{};
  bool scissorEnabled = false;
  RenderStateTable renderStates;
  std::array<TextureStageStateTable, kMaxTextureStages> textureStageStates{};
  std::array<SamplerStateTable, kMaxSamplers> samplerStates{};
  TransformTable transforms;
  std::array<Light, kMaxLights> lights{};
  std::array<bool, kMaxLights> lightEnabled{};
  Material material{};
  std::array<std::shared_ptr<Buffer>, kMaxStreams> streamBuffers{};
  std::array<u32, kMaxStreams> streamOffsets{};
  std::array<u32, kMaxStreams> streamStrides{};
  std::array<u32, kMaxStreams> streamFrequencies = [] {
    std::array<u32, kMaxStreams> frequencies{};
    frequencies.fill(1);
    return frequencies;
  }();
  std::shared_ptr<Buffer> indexBuffer;
  IndexType indexType = IndexType::UInt16;
  VertexDeclSnapshot vertexDecl{};
  u32 fvf = 0;
  ShaderRef vertexShader{};
  ShaderRef pixelShader{};
  VertexShaderConstants vsConst{};
  PixelShaderConstants psConst{};
  std::array<std::shared_ptr<Texture>, kMaxTextures> textures{};
  std::array<RenderTargetAttachment, kMaxRenderTargets> renderTargets{};
  RenderTargetAttachment depthStencil{};
  std::array<ClipPlane, kMaxClipPlanes> clipPlanes{};
  bool inScene = false;

  void reset();
};

class Buffer {
 public:
  Buffer(std::shared_ptr<Device> owner, BufferHandle handle, BufferDesc desc);
  ~Buffer();

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  BufferHandle handle() const noexcept { return handle_; }
  const BufferDesc& desc() const noexcept { return desc_; }
  bool valid() const noexcept { return valid_; }
  std::shared_ptr<Device> device() const noexcept { return owner_.lock(); }
  LockedRegion lock(u64 offset, u64 size, u32 flags);
  void unlock(bool upload = true);
  void invalidate();
  std::span<const u8> bytes() const noexcept { return storage_; }

 private:
  friend class Device;

  std::weak_ptr<Device> owner_;
  std::shared_ptr<dxmt9::Device> backend_;
  BufferHandle handle_{};
  BufferDesc desc_{};
  std::vector<u8> storage_;
  bool valid_ = true;
  bool locked_ = false;
  u64 lockedOffset_ = 0;
  u64 lockedSize_ = 0;
  u32 lockedFlags_ = 0;
};

class Texture : public std::enable_shared_from_this<Texture> {
 public:
  Texture(std::shared_ptr<Device> owner, TextureHandle handle, TextureDesc desc);
  ~Texture();

  Texture(const Texture&) = delete;
  Texture& operator=(const Texture&) = delete;

  TextureHandle handle() const noexcept { return handle_; }
  const TextureDesc& desc() const noexcept { return desc_; }
  bool valid() const noexcept { return valid_; }
  u32 lod() const noexcept { return lod_; }
  std::shared_ptr<Device> device() const noexcept { return owner_.lock(); }
  u32 levelCount() const noexcept;
  u32 setLod(u32 lod);
  u32 subresourceCount() const noexcept { return static_cast<u32>(levels_.size()); }
  u32 mipLevelForSubresource(u32 subresource) const noexcept;
  LockedRegion lockRect(u32 subresource, const Rect* rect, u32 flags);
  void unlockRect(u32 subresource);
  std::shared_ptr<Surface> surfaceLevel(u32 subresource);
  std::span<const u8> levelBytes(u32 subresource) const;
  void fillColor(u32 subresource, const Rect* rect, ColorRGBA color);
  void fillColor(const Rect* rect, ColorRGBA color);
  void copyFrom(const Texture& src);
  HResult generateMipSubLevels();
  void invalidate();

 private:
  friend class Device;

  struct LevelStorage {
    u32 width = 0;
    u32 height = 0;
    u32 depth = 1;
    u32 pitch = 0;
    u32 slicePitch = 0;
    std::vector<u8> bytes;
    bool dirty = false;
  };

  std::weak_ptr<Device> owner_;
  std::shared_ptr<dxmt9::Device> backend_;
  TextureHandle handle_{};
  TextureDesc desc_{};
  std::vector<LevelStorage> levels_;
  std::vector<std::weak_ptr<Surface>> surfaces_;
  u32 lod_ = 0;
  bool valid_ = true;
  bool locked_ = false;

  void syncLevelToBackend(u32 level);
};

class Surface : public std::enable_shared_from_this<Surface> {
 public:
  enum class ContainerKind {
    None,
    Texture,
    Device,
  };

  Surface(std::shared_ptr<Device> owner, SurfaceHandle handle, SurfaceDesc desc);
  Surface(std::shared_ptr<Device> owner, SurfaceHandle handle, std::shared_ptr<Texture> texture, u32 level);
  ~Surface();

  Surface(const Surface&) = delete;
  Surface& operator=(const Surface&) = delete;

  SurfaceHandle handle() const noexcept { return handle_; }
  const SurfaceDesc& desc() const noexcept { return desc_; }
  bool valid() const noexcept {
    if (!valid_) {
      return false;
    }
    if (containerKind_ == ContainerKind::Texture) {
      auto texture = textureContainer_.lock();
      return texture && texture->valid();
    }
    return true;
  }
  ContainerKind containerKind() const noexcept { return containerKind_; }
  // R-FORMAT-12: a D3DFMT_NULL render target is colorless — it carries no
  // color backing storage and the backend render pass omits its color
  // attachment, leaving the depth/stencil attachment as the effective
  // target. Derived purely from the surface format so the marker stays in
  // lockstep with classification (Format::NullRt) at zero storage cost.
  bool isNullRenderTarget() const noexcept {
    return desc_.format == Format::NullRt;
  }
  // Bytes of CPU-side color backing allocated for a device-owned surface.
  // Zero for a NULL render target (no color storage) and for any
  // texture-container surface (the container owns the bytes). Exposed for
  // the colorless-RT contract test.
  std::size_t colorBackingByteSize() const noexcept {
    return standaloneBytes_.size();
  }
  u32 multiSampleCount() const noexcept { return dxmt9::core::sampleCount(desc_.multiSampleType); }
  std::shared_ptr<Texture> textureContainer() const noexcept { return textureContainer_.lock(); }
  std::shared_ptr<Device> deviceContainer() const noexcept { return owner_.lock(); }
  u32 level() const noexcept { return level_; }
  LockedRegion lockRect(const Rect* rect, u32 flags);
  void unlockRect();
  void fillColor(const Rect* rect, ColorRGBA color);
  void copyFrom(const Surface& src);
  void invalidate();

 private:
  friend class Device;

  std::weak_ptr<Device> owner_;
  std::shared_ptr<dxmt9::Device> backend_;
  std::weak_ptr<Texture> textureContainer_;
  SurfaceHandle handle_{};
  SurfaceDesc desc_{};
  u32 level_ = 0;
  ContainerKind containerKind_ = ContainerKind::None;
  bool valid_ = true;
  std::vector<u8> standaloneBytes_;
  u32 standalonePitch_ = 0;
  bool locked_ = false;
};

class Query : public std::enable_shared_from_this<Query> {
 public:
  explicit Query(QueryType type);

  Query(const Query&) = delete;
  Query& operator=(const Query&) = delete;

  QueryType type() const noexcept { return type_; }
  void begin(u64 sequenceId);
  void end(u64 sequenceId);
  void resolve(u64 value);
  bool resolved() const noexcept { return resolvedValue_.has_value(); }
  std::optional<u64> resolvedValue() const noexcept { return resolvedValue_; }
  u64 issuedSequenceId() const noexcept { return issuedSequenceId_; }
  HRESULT getData(void* output, size_t size, u32 flags, u64 completedSequenceId) const;

 private:
  QueryType type_;
  u64 issuedSequenceId_ = 0;
  bool active_ = false;
  std::optional<u64> resolvedValue_;
};

class StateBlock : public std::enable_shared_from_this<StateBlock> {
 public:
  enum class CaptureMode {
    FullSnapshot,
    Delta,
  };

  explicit StateBlock(StateBlockType type = StateBlockType::All) : type_(type) {}

  void capture(const DeviceState& state);
  void captureDelta(const DeviceState& before, const DeviceState& after);
  void captureDelta(const DeviceState& before, const DeviceState& after,
                    const std::unordered_set<u32>& recordedRenderStates);
  void apply(Device& device) const;
  const DeviceState& snapshot() const noexcept { return snapshot_; }
  StateBlockType type() const noexcept { return type_; }

 private:
  CaptureMode mode_ = CaptureMode::FullSnapshot;
  StateBlockType type_ = StateBlockType::All;
  DeviceState snapshot_;
  DeviceState baseline_;
  std::unordered_set<u32> recordedRenderStates_{};
};

class SwapChain : public std::enable_shared_from_this<SwapChain> {
 public:
  SwapChain(std::shared_ptr<Device> owner, SwapChainHandle handle, PresentParameters params,
            std::shared_ptr<Surface> backBuffer, std::shared_ptr<Surface> depthStencil);
  ~SwapChain();

  SwapChain(const SwapChain&) = delete;
  SwapChain& operator=(const SwapChain&) = delete;

  SwapChainHandle handle() const noexcept { return handle_; }
  const PresentParameters& params() const noexcept { return params_; }
  std::shared_ptr<Surface> backBuffer() const noexcept { return backBuffer_; }
  std::shared_ptr<Surface> depthStencilSurface() const noexcept { return depthStencilSurface_; }
  std::shared_ptr<Device> device() const noexcept { return owner_.lock(); }
  bool displaySyncEnabled() const noexcept;
  void resize(const PresentParameters& params);
  HResult present(std::shared_ptr<dxmt9::Device> device, const SwapDesc& desc);

  // Per-window Presenter (WMT::MetalLayer-centric upper object). Owned by
  // this swap chain; nullptr on test paths where the upper dxmt9::Device
  // has no WMT::Device. Production submit/present paths thread the
  // queue-local presentId() instead — the raw Presenter pointer is kept
  // here only for the d3d9-internal accessors that pre-date the
  // registry (none of them cross the PE/unix wire).
  dxmt9::Presenter* presenter() const noexcept { return presenter_.get(); }

  // Queue-local opaque binding registered by WSI adoption. Zero when
  // no Presenter could be constructed (test path / hwnd=0). Travels on
  // core::SwapDesc; the unix-side CommandQueue resolves it back to a
  // Presenter* (and any pending drawable token).
  PresentId presentId() const noexcept { return presentId_; }

  // Installs a typed cold-path output target for provider identity replay.
  // Existing queue binding and Presenter encode semantics remain in use.
  bool installPresentOutput(std::shared_ptr<dxmt9::PresentOutput> output);
  void restoreWindowPresenter();
  HResult adoptWsiSurface(
      u32 protocol, u64 hwnd, u64 surfaceToken, u64 layerToken);
  HResult teardownWsiSurface();

 private:
  std::unique_ptr<dxmt9::Presenter> makeWindowPresenter(
      u32 protocol, u64 hwnd, u64 layerToken) const;
  void unregisterPresenter();

  std::weak_ptr<Device> owner_;
  SwapChainHandle handle_{};
  PresentParameters params_{};
  std::shared_ptr<Surface> backBuffer_;
  std::shared_ptr<Surface> depthStencilSurface_;
  std::unique_ptr<dxmt9::Presenter> presenter_{};
  PresentId presentId_{};
  u32 wsiProtocol_ = 0u;
  u64 wsiHwnd_ = 0u;
  u64 wsiLayerToken_ = 0u;
};

namespace detail {

struct ShaderConstantHashDirtyRange {
  u16 begin = 0;
  u16 end = 0;
  bool dirty = false;
};

template <std::size_t FloatCount>
struct ShaderConstantHashIndex {
  std::array<u64, FloatCount + 1u> floatTree{};
  std::array<u64, kMaxIntegerConstants + 1u> intTree{};
  std::array<u64, kMaxBoolConstants + 1u> boolTree{};
  ShaderConstantHashDirtyRange floatDirty{};
  ShaderConstantHashDirtyRange intDirty{};
  ShaderConstantHashDirtyRange boolDirty{};
  u64 generation = 0;
  bool valid = false;
};

enum class ShaderConstantRegisterClass : u8 {
  Float,
  Int,
  Bool,
};

}  // namespace detail

class Device : public std::enable_shared_from_this<Device> {
 public:
  Device(AdapterInfo adapter, BackendLimits limits,
         PresentParameters params, u32 behaviorFlags,
         std::shared_ptr<dxmt9::Device> upperDevice = {},
         bool extendedDevice = false);
  ~Device();

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  const AdapterInfo& adapter() const noexcept { return adapter_; }
  const BackendLimits& limits() const noexcept { return limits_; }
  const DeviceCaps& caps() const noexcept { return caps_; }
  const DeviceState& state() const noexcept { return state_; }
  DeviceState& mutableState() noexcept {
    return mutableState(DrawStateInvalidationMutableState);
  }
  DeviceState& mutableState(u32 invalidationReasonMask) noexcept {
    invalidateDrawStateCache(invalidationReasonMask);
    return state_;
  }
  DeviceState& mutableShaderConstantsState() noexcept {
    invalidateDrawShaderConstantsCache();
    return state_;
  }
  DeviceState& mutableVertexShaderConstantsState() noexcept {
    invalidateDrawVertexShaderConstantsCache();
    return state_;
  }
  DeviceState& mutablePixelShaderConstantsState() noexcept {
    invalidateDrawPixelShaderConstantsCache();
    return state_;
  }
  DeviceState& mutableVertexShaderFloatConstantsState(u32 start,
                                                       u32 count) noexcept;
  DeviceState& mutableVertexShaderIntConstantsState(u32 start,
                                                     u32 count) noexcept;
  DeviceState& mutableVertexShaderBoolConstantsState(u32 start,
                                                      u32 count) noexcept;
  DeviceState& mutablePixelShaderFloatConstantsState(u32 start,
                                                      u32 count) noexcept;
  DeviceState& mutablePixelShaderIntConstantsState(u32 start,
                                                    u32 count) noexcept;
  DeviceState& mutablePixelShaderBoolConstantsState(u32 start,
                                                     u32 count) noexcept;
  const PresentParameters& presentParameters() const noexcept { return presentParameters_; }
  // Transitional accessor — returns the cached upper-device ptr, which
  // exposes both resource-ops + submit/present now (via the dxmt9::Device
  // interface promoted in Step 2a/2b). Name kept for back-compat; will be
  // renamed to upperDevice() once all call sites migrate.
  std::shared_ptr<dxmt9::Device> backend() const noexcept { return backend_; }

  [[nodiscard]] std::shared_ptr<Buffer> createBuffer(const BufferDesc& desc);
  [[nodiscard]] std::shared_ptr<Texture> createTexture(const TextureDesc& desc);
  [[nodiscard]] std::shared_ptr<Surface> createSurface(const SurfaceDesc& desc);
  [[nodiscard]] std::shared_ptr<Buffer> openSharedBuffer(const std::shared_ptr<Buffer>& source);
  [[nodiscard]] std::shared_ptr<Texture> openSharedTexture(const std::shared_ptr<Texture>& source);
  [[nodiscard]] std::shared_ptr<Surface> openSharedSurface(const std::shared_ptr<Surface>& source);
  [[nodiscard]] std::shared_ptr<Query> createQuery(QueryType type);
  [[nodiscard]] std::shared_ptr<StateBlock> createStateBlock(StateBlockType type = StateBlockType::All) const;
  [[nodiscard]] std::shared_ptr<SwapChain> createAdditionalSwapChain(const PresentParameters& params);
  std::shared_ptr<SwapChain> swapChain(size_t index = 0) const;
  size_t swapChainCount() const noexcept { return swapChains_.size(); }
  HResult testCooperativeLevel() const;
  HResult checkDeviceState() const;
  HResult resetEx(const PresentParameters& params, const DisplayModeEx* fullscreenMode = nullptr);
  HResult presentEx(const Rect* sourceRect = nullptr, const Rect* destRect = nullptr,
                    Handle destinationWindowOverride = {}, const void* dirtyRegion = nullptr,
                    u32 flags = 0);
  // R-BACK-2.51(g) — consumed (read + reset to false) by the very next
  // presentEx() call, which stamps it onto that call's snapshotted
  // SwapDesc::pacedByPresentOrdinal. The D3D9 chunk-replay path
  // (device_c_chunk_replay.cpp's D9C_COMMAND_RECORD_PRESENT case) calls
  // this immediately before dispatching a Present record whose ordinal
  // pacing already ran via dxmt9::Device::waitPresentOrdinalBoundary, so
  // CommandQueue::submitPresent knows to skip its own inline boundary for
  // that specific present. Not reentrant across multiple pending presents —
  // the caller sets it and dispatches the paced present synchronously, with
  // no other presentEx() call able to interleave.
  void setNextPresentPacedByOrdinal(bool paced) noexcept { nextPresentPacedByOrdinal_ = paced; }
  HResult setMaximumFrameLatency(u32 latency);
  u32 maximumFrameLatency() const noexcept { return maximumFrameLatency_; }
  HResult waitForVBlank(size_t swapChainIndex = 0);
  HResult checkResourceResidency(std::span<void* const> resources = {}) const;
  DisplayModeEx getDisplayModeEx(size_t swapChainIndex = 0) const;
  HResult getGPUThreadPriority(i32* priority) const;
  HResult setGPUThreadPriority(i32 priority);
  HResult setConvolutionMonoKernel();
  HResult composeRects();
  void setDeviceLost(bool lost) noexcept { deviceLost_ = lost; }
  void setPresentOccluded(bool occluded) noexcept { presentOccluded_ = occluded; }

  HResult setRenderState(u32 key, u32 value);
  HResult setRenderStateFloat(u32 key, f32 value);
  u32 getRenderState(u32 key) const;
  f32 getRenderStateFloat(u32 key, f32 defaultValue = 0.0f) const;
  HResult setTextureStageState(u32 stage, u32 key, u32 value);
  u32 getTextureStageState(u32 stage, u32 key) const;
  HResult setSamplerState(u32 sampler, u32 key, u32 value);
  u32 getSamplerState(u32 sampler, u32 key) const;
  HResult setTransform(u32 key, const Matrix4x4& matrix);
  HResult setLight(u32 index, const Light& light);
  HResult lightEnable(u32 index, bool enable);
  HResult setMaterial(const Material& material);
  HResult setTexture(u32 stage, std::shared_ptr<Texture> texture);
  HResult setStreamSource(u32 stream, std::shared_ptr<Buffer> buffer, u32 offset, u32 stride);
  HResult setStreamSourceFreq(u32 stream, u32 frequency);
  HResult setIndices(std::shared_ptr<Buffer> buffer, IndexType indexType = IndexType::UInt16);
  HResult setFVF(u32 fvf);
  HResult setVertexDeclaration(std::vector<VertexElement> elements);
  HResult setVertexShader(const ShaderRef& shader);
  HResult setPixelShader(const ShaderRef& shader);
  HResult setClipPlane(u32 index, const ClipPlane& plane);
  HResult setViewport(const Viewport& viewport);
  Viewport viewport() const noexcept { return state_.viewport; }
  HResult setScissorRect(const Rect& rect);
  Rect scissorRect() const noexcept { return state_.scissorRect; }
  HResult setRenderTarget(u32 index, std::shared_ptr<Surface> surface);
  HResult setDepthStencilSurface(std::shared_ptr<Surface> surface);

  HResult beginScene();
  HResult endScene();
  HResult clear(const ClearDesc& desc);
  HResult drawPrimitive(PrimitiveType type, u32 primitiveCount, u32 startVertex = 0);
  HResult drawIndexedPrimitive(PrimitiveType type, u32 primitiveCount, u32 startVertex,
                               i32 baseVertexIndex, u32 startIndex, IndexType indexType);
  HResult drawPrimitiveUP(PrimitiveType type, u32 primitiveCount, std::span<const u8> vertexData,
                          u32 vertexStride = 0);
  HResult drawIndexedPrimitiveUP(PrimitiveType type, u32 primitiveCount,
                                 std::span<const u8> vertexData, std::span<const u8> indexData,
                                 IndexType indexType, u32 vertexStride = 0);
  // Compact draw-run: snapshots BaseDrawState ONCE from current state_ and
  // submits the supplied DrawParam span through the immediate flat path. Used
  // by the chunk importer when consecutive D9C_COMMAND_RECORD_DRAW_* records
  // resolve to the same run-invariant state after applying the first delta.
  HResult drawPrimitiveRun(std::span<const DrawParam> draws);
  HResult drawPrimitiveRun(std::span<const DrawParam> draws,
                           std::span<const DrawParamPayloadView> payloads);
  HResult snapshotDrawSubmissionFromCurrentState(
      DrawParam draw, DrawRunSubmission& submission,
      const DrawRunSubmission* previousSubmission = nullptr);
  void submitDrawSubmissionBatch(std::span<DrawRunSubmission> submissions);
  HResult present();
  HResult reset(const PresentParameters& params);
  HResult checkDeviceMultiSampleType(Format format, MultiSampleType type) const;

  HResult issueQuery(const std::shared_ptr<Query>& query, bool begin);
  HResult getQueryData(const std::shared_ptr<Query>& query, void* output, size_t size,
                       u32 flags);
  void completeUpTo(u64 sequenceId);
  u64 submittedSequenceId() const noexcept { return submittedSequenceId_; }
  u64 completedSequenceId() const noexcept { return completedSequenceId_; }
  void initializeDefaultSwapChain();

  ClearDesc snapshotClearDesc(const ClearDesc& desc) const;
  SwapDesc snapshotSwapDesc() const;
  std::shared_ptr<StateBlock> captureStateBlock() const;
  HResult applyStateBlock(const StateBlock& block);

  // Per-device gamma ramp shadow — D3D9 SetGammaRamp transports the 1.5 KB
  // payload through the D9C bridge to here; snapshotSwapDesc embeds the
  // current ramp into every present so the unix-side Presenter can apply
  // it without a parallel ABI entry. `setGammaRamp(nullptr)` is a no-op
  // (D3D9 has no error channel — defensive copy of wined3d shape). The
  // identity check is recomputed every Set; default state is identity
  // (entries[i] = i << 8 per channel) so an app that never calls
  // SetGammaRamp never trips the apply pass.
  void setGammaRamp(const GammaRamp* ramp) noexcept;
  const GammaRamp& gammaRamp() const noexcept { return gammaRamp_; }
  bool gammaRampIsIdentity() const noexcept { return gammaRampIsIdentity_; }

  HResult fillSurface(const std::shared_ptr<Surface>& surface, const Rect* rect, ColorRGBA color);
  HResult stretchRect(const std::shared_ptr<Surface>& src, const Rect* srcRect,
                      const std::shared_ptr<Surface>& dst, const Rect* dstRect, bool linear);
  HResult updateSurface(const std::shared_ptr<Surface>& src, const Rect* srcRect,
                        const std::shared_ptr<Surface>& dst, i32 dstX, i32 dstY);
  HResult updateSurface(const std::shared_ptr<Surface>& src,
                        const std::shared_ptr<Surface>& dst) {
    return updateSurface(src, nullptr, dst, 0, 0);
  }
  HResult updateTexture(const std::shared_ptr<Texture>& src, const std::shared_ptr<Texture>& dst);
  HResult getRenderTargetData(const std::shared_ptr<Surface>& src, const std::shared_ptr<Surface>& dst);
  // R-FORMAT-11 — RESZ MSAA depth resolve. `msaaDepth` is the bound
  // multisampled depth-stencil surface; `intzDest` is the stage-0 INTZ
  // destination texture. Builds a DepthResolveDesc from the surface handle +
  // the texture's level-0 surface handle and submits it to the backend
  // queue, mirroring stretchRect's submit path. Fire-and-forget surface op.
  HResult reszDepthResolve(const std::shared_ptr<Surface>& msaaDepth,
                           const std::shared_ptr<Texture>& intzDest);

 private:
  struct ExperimentCaptureConfig {
    std::string path;
    std::string dir;
    u32 frame = 0;
    std::vector<u32> frames;
    u32 rangeStart = 0;
    u32 rangeEnd = 0;
    u32 rangeInterval = 0;
    bool captured = false;
    u32 capturedCount = 0;
    u32 droppedCount = 0;
  };

  friend class StateBlock;
  friend class SwapChain;
  friend class Texture;

  void registerBuffer(const std::shared_ptr<Buffer>& buffer);
  void registerTexture(const std::shared_ptr<Texture>& texture);
  void registerSurface(const std::shared_ptr<Surface>& surface);
  void invalidateDefaultPoolResources();
  void submitClearInternal(const ClearDesc& desc);
  void submitDrawRunInternal(CanonicalDrawState state, const DrawUniformPayload& uniforms,
                             std::span<const DrawParam> draws,
                             std::span<const DrawParamPayloadView> payloads = {});
  void submitDrawRunInternalFromCurrentState(
      std::span<const DrawParam> draws,
      std::span<const DrawParamPayloadView> payloads = {});
  void submitDrawRunInternalFromState(
      DeviceState baseState,
      std::span<const DrawParam> draws,
      std::span<const DrawParamPayloadView> payloads = {});
  void submitPresentInternal(const SwapDesc& desc);
  void maybeCaptureExperimentFrame();
  u32 experimentCaptureRequestedCount() const;
  void recordExperimentCaptureSkip(const std::string& capturePath,
                                   const char* reason);
  void resetState();
  HResult resetValidated(const PresentParameters& params);
  void notifyTextureLodChanged(const Texture& texture);

  struct CachedBaseDrawState {
    u64 generation = 0;
    // Guards shaderLayout reuse across rebuilds. The accumulated
    // drawStateInvalidationReasonMask_ is shared by BOTH cache lanes and is
    // cleared by whichever lane rebuilds first, so a mask-based "layout
    // unaffected" reuse decision can lose another lane's Shader/FvfVdecl
    // bits and stamp a stale shader layout with a fresh generation (GT1
    // t=40s giant-triangle bug). Layout reuse must key off this dedicated
    // generation instead.
    u64 shaderLayoutGeneration = 0;
    u64 uniformGeneration = 0;
    u64 vertexShaderConstantGeneration = 0;
    u64 pixelShaderConstantGeneration = 0;
    u64 uniformNonConstantGeneration = 0;
    u64 renderStateFlatGeneration = 0;
    u64 textureStageStateFlatGeneration = 0;
    u64 samplerStateFlatGeneration = 0;
    bool valid = false;
    FlatDrawStateRecord hot{};
    DrawShaderLayoutContext shaderLayout{};
    DrawUniformPayload uniforms{};
    DrawUniformFixedPayload uniformFixedPayload{};
    DrawUniformPayloadHashes uniformHashes{};
    u64 uniformPayloadHash = 0;
    u64 uniformFixedPayloadHash = 0;
    bool fullUniformsValid = false;
    bool uniformFixedPayloadValid = false;
  };

  struct BatchMissSemanticProbeEntry {
    FlatDrawStateKey key{};
    u64 hash = 0;
    bool valid = false;
  };

  struct BatchMissShaderConstantHashMemoEntry {
    ShaderConstantUsageBounds usage{};
    u64 generation = 0;
    u64 hash = 0;
    bool valid = false;
  };

  void invalidateDrawStateCache(u32 reasonMask = DrawStateInvalidationUnknown) noexcept;
  void invalidateDrawUniformCache() noexcept;
  void invalidateDrawShaderConstantsCache() noexcept;
  void invalidateDrawVertexShaderConstantsCache() noexcept;
  void invalidateDrawPixelShaderConstantsCache() noexcept;
  void invalidateDrawVertexShaderConstantsCacheRange(
      detail::ShaderConstantRegisterClass registerClass,
      u32 start, u32 count) noexcept;
  void invalidateDrawPixelShaderConstantsCacheRange(
      detail::ShaderConstantRegisterClass registerClass,
      u32 start, u32 count) noexcept;
  void invalidateDrawUniformNonConstantCache() noexcept;
  void invalidateDrawFlatStateSetCache(u32 reasonMask) noexcept;
  const CachedBaseDrawState& cachedBaseDrawState(bool includeIndexBuffer);
  const CachedBaseDrawState& cachedBaseDrawStateForSubmissionBatch();

  AdapterInfo adapter_{};
  BackendLimits limits_{};
  DeviceCaps caps_{};
  std::shared_ptr<dxmt9::Device> backend_;
  std::shared_ptr<dxmt9::Device> upperDevice_{};

 public:
  const std::shared_ptr<dxmt9::Device>& upperDevice() const noexcept { return upperDevice_; }

 private:
  PresentParameters presentParameters_{};
  [[maybe_unused]] u32 behaviorFlags_ = 0;
  bool extendedDevice_ = false;
  // R-BACK-2.51(g) — see setNextPresentPacedByOrdinal() doc above.
  bool nextPresentPacedByOrdinal_ = false;
  DeviceState state_{};
  u64 drawStateGeneration_ = 1;
  u64 drawStableStateGeneration_ = 1;
  // Bumped by invalidateDrawStateCache whenever the reason can affect the
  // derived shader layout (Shader, FvfVdecl, Unknown, ...). Independent of
  // the shared reason-mask accumulator so per-lane layout reuse cannot be
  // poisoned by another lane consuming/clearing the mask.
  u64 drawShaderLayoutGeneration_ = 1;
  u64 drawUniformGeneration_ = 1;
  u64 drawVertexShaderConstantGeneration_ = 1;
  u64 drawPixelShaderConstantGeneration_ = 1;
  u64 drawUniformNonConstantGeneration_ = 1;
  u64 drawRenderStateFlatGeneration_ = 1;
  u64 drawTextureStageStateFlatGeneration_ = 1;
  u64 drawSamplerStateFlatGeneration_ = 1;
  detail::ShaderConstantHashIndex<kMaxVertexConstants>
      drawVertexShaderConstantHashIndex_{};
  detail::ShaderConstantHashIndex<kMaxPixelConstants>
      drawPixelShaderConstantHashIndex_{};
  u32 drawStateInvalidationReasonMask_ = DrawStateInvalidationUnknown;
  CachedBaseDrawState drawStateCacheWithIndex_{};
  CachedBaseDrawState drawStateCacheNoIndex_{};
  CachedBaseDrawState drawStateCacheBindingAgnostic_{};
  std::array<BatchMissSemanticProbeEntry, 8> drawStateCacheBatchMissSemanticProbe_{};
  u32 drawStateCacheBatchMissSemanticProbeCursor_ = 0;
  std::array<BatchMissShaderConstantHashMemoEntry, 16>
      drawStateCacheBatchMissVsConstHashMemo_{};
  std::array<BatchMissShaderConstantHashMemoEntry, 16>
      drawStateCacheBatchMissPsConstHashMemo_{};
  u32 drawStateCacheBatchMissVsConstHashMemoCursor_ = 0;
  u32 drawStateCacheBatchMissPsConstHashMemoCursor_ = 0;
  std::vector<std::weak_ptr<Buffer>> buffers_;
  std::vector<std::weak_ptr<Texture>> textures_;
  std::vector<std::weak_ptr<Surface>> surfaces_;
  std::vector<std::shared_ptr<SwapChain>> swapChains_;
  std::vector<std::shared_ptr<Query>> queries_;
  std::shared_ptr<Query> activeOcclusionQuery_;
  u64 activeOcclusionCount_ = 0;
  std::vector<u8> upVertexScratch_;
  std::vector<u8> upIndexScratch_;
  u64 nextHandle_ = 1;
  u64 submittedSequenceId_ = 0;
  u64 completedSequenceId_ = 0;
  u32 presentCount_ = 0;
  u32 maximumFrameLatency_ = kDefaultFrameLatency;
  bool inScene_ = false;
  bool deviceLost_ = false;
  bool presentOccluded_ = false;
  ExperimentCaptureConfig experimentCapture_{};
  // Gamma ramp shadow — initialized to the identity ramp in the
  // constructor (see core.cpp). Recomputed on every setGammaRamp; the
  // identity flag is the fast-path that lets the unix presenter skip
  // the gamma-apply encoder altogether.
  GammaRamp gammaRamp_{};
  bool gammaRampIsIdentity_ = true;
};

class Factory {
 public:
  // Consume an upper dxmt9::Device (retained as shared_ptr so child D3D9
  // objects can share it). The Factory derives limits_ and backend_ from
  // the upper Device for the existing resource-creation code paths.
  explicit Factory(std::shared_ptr<dxmt9::Device> device);

  // Test-only convenience: wrap an existing BackendDevice in a stub
  // dxmt9::Device. Used by unit tests that inject a recording backend
  // without going through WMT device selection.
  Factory(BackendLimits limits, std::shared_ptr<BackendDevice> backend);

  // Test-only convenience: build a factory with the given limits and no
  // backend — for tests that exercise adapter enumeration without ever
  // creating a Device.
  explicit Factory(BackendLimits limits);

  size_t adapterCount() const noexcept { return adapters_.size(); }
  const AdapterInfo& adapter(size_t index) const;
  const DeviceCaps& caps(size_t index) const;
  AdapterIdentifier getAdapterIdentifier(size_t index) const;
  std::vector<DisplayMode> enumAdapterModes(size_t index, Format format) const;
  DisplayMode getAdapterDisplayMode(size_t index) const;
  u32 getAdapterMonitor(size_t index) const;
  HRESULT checkDeviceType(size_t adapterIndex, DeviceType deviceType, Format adapterFormat,
                          Format backBufferFormat, bool windowed) const;
  HRESULT checkDeviceFormat(size_t adapterIndex, Format format, u32 usage) const;
  HRESULT checkDeviceMultiSampleType(size_t adapterIndex, Format format, MultiSampleType type) const;
  std::shared_ptr<Device> createDevice(size_t adapterIndex, const PresentParameters& params,
                                       u32 behaviorFlags = 0);
  std::shared_ptr<Device> createDeviceEx(size_t adapterIndex, const PresentParameters& params,
                                         const DisplayModeEx* fullscreenMode = nullptr,
                                         u32 behaviorFlags = 0);

  std::shared_ptr<dxmt9::Device> upperDevice() const noexcept { return device_; }

 private:
  std::shared_ptr<Device> createDeviceValidated(size_t adapterIndex, const PresentParameters& params,
                                                u32 behaviorFlags, bool extendedDevice);

  std::shared_ptr<dxmt9::Device> device_;
  BackendLimits limits_{};
  std::vector<AdapterInfo> adapters_;
  std::vector<DeviceCaps> adapterCaps_;
};

}  // namespace dxmt9::core
