#pragma once

#include "d3d9_pe_const_shadow.hpp"
#include "d3d9_pe_transition_algebra.hpp"
#include "dxmt9/device_c.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

class D3D9DeviceImpl;

template<typename Public, typename Raw, typename Wire>
struct D3D9PeValidatedObject;

// D3D9 constant mirrors. This header is compiled natively (no windows.h /
// d3d9.h) so the sparse-state producer and its differential test can run
// without Wine, so the values are inlined from the D3D9 SDK headers rather
// than included. Verified against wine/include/d3d9types.h and d3d9.h;
// core_constants.hpp's XFORM_WORLD_BASE block sets the precedent for
// mirroring the D3DTS_WORLDMATRIX range this way.
static constexpr std::uint32_t kD3dTsView = 2u;         // D3DTS_VIEW
static constexpr std::uint32_t kD3dTsProjection = 3u;   // D3DTS_PROJECTION
static constexpr std::uint32_t kD3dTsTexture0 = 16u;    // D3DTS_TEXTURE0
static constexpr std::uint32_t kD3dTsTexture7 = 23u;    // D3DTS_TEXTURE7
static constexpr std::uint32_t kD3dTsWorld = 256u;      // D3DTS_WORLDMATRIX(0)
static constexpr std::uint32_t kD3dDmapSampler = 256u;  // D3DDMAPSAMPLER
static constexpr std::uint32_t kD3dVertexTextureSampler0 = kD3dDmapSampler + 1u;
static constexpr std::uint32_t kD3dVertexTextureSampler3 = kD3dDmapSampler + 4u;

static constexpr std::uint32_t kPeRenderStateSlots = 256;
static constexpr std::uint32_t kPeTextureStageSlots = 8;
static constexpr std::uint32_t kPeTextureStageStateSlots = 64;
static constexpr std::uint32_t kPeFragmentSamplerSlots = 16;
static constexpr std::uint32_t kPeVertexTextureSamplerSlots = 4;
static constexpr std::uint32_t kPeTextureSlots =
    kPeFragmentSamplerSlots + kPeVertexTextureSamplerSlots;
static constexpr std::uint32_t kPeSamplerSlots = kPeTextureSlots;
// D3DSAMP_* ordinals occupy 1..13. Keep the PE-side identity-mapped shadow
// compact while preserving all public sampler-state slots.
static constexpr std::uint32_t kPeSamplerStateSlots = 16;
static constexpr std::uint32_t kPeTransformTextureSlots = 8;
static constexpr std::uint32_t kPeTransformWorldSlots = 256;
static constexpr std::uint32_t kPeTransformTextureBaseSlot = 2;
static constexpr std::uint32_t kPeTransformWorldBaseSlot =
    kPeTransformTextureBaseSlot + kPeTransformTextureSlots;
static constexpr std::uint32_t kPeTransformSlots =
    kPeTransformWorldBaseSlot + kPeTransformWorldSlots;

template<std::size_t Slots>
class FixedStateTable {
public:
    static constexpr bool valid(std::uint32_t slot) noexcept {
        return slot < Slots;
    }
    static constexpr std::uint64_t bit(std::uint32_t slot) noexcept {
        return 1ull << (slot & 63u);
    }
    static constexpr std::size_t word(std::uint32_t slot) noexcept {
        return slot >> 6;
    }
    bool contains(std::uint32_t slot) const noexcept {
        return valid(slot) && (occupied[word(slot)] & bit(slot)) != 0;
    }
    bool empty() const noexcept {
        return count == 0;
    }
    std::uint32_t size() const noexcept {
        return count;
    }
    bool get(std::uint32_t slot, std::uint32_t& value) const noexcept {
        if (!contains(slot)) {
            return false;
        }
        value = values[slot];
        return true;
    }
    void set(std::uint32_t slot, std::uint32_t value) noexcept {
        if (!valid(slot)) {
            return;
        }
        const auto w = word(slot);
        const auto b = bit(slot);
        if ((occupied[w] & b) == 0) {
            occupied[w] |= b;
            ++count;
        }
        values[slot] = value;
    }
    void erase(std::uint32_t slot) noexcept {
        if (!contains(slot)) {
            return;
        }
        occupied[word(slot)] &= ~bit(slot);
        --count;
    }
    void clear() noexcept {
        occupied = {};
        count = 0;
    }
    template<typename Fn>
    void forEach(Fn&& fn) const {
        for (std::size_t w = 0; w < occupied.size(); ++w) {
            std::uint64_t bits = occupied[w];
            while (bits != 0) {
                const auto b = static_cast<std::uint32_t>(std::countr_zero(bits));
                const auto slot = static_cast<std::uint32_t>(w * 64u + b);
                if (slot < Slots) {
                    fn(slot, values[slot]);
                }
                bits &= bits - 1u;
            }
        }
    }
    bool popFirst(std::uint32_t& slot, std::uint32_t& value) noexcept {
        for (std::size_t w = 0; w < occupied.size(); ++w) {
            std::uint64_t bits = occupied[w];
            if (bits == 0) {
                continue;
            }
            const auto b = static_cast<std::uint32_t>(std::countr_zero(bits));
            slot = static_cast<std::uint32_t>(w * 64u + b);
            value = values[slot];
            occupied[w] &= ~bit(slot);
            --count;
            return true;
        }
        return false;
    }

private:
    std::array<std::uint32_t, Slots> values{};
    std::array<std::uint64_t, (Slots + 63u) / 64u> occupied{};
    std::uint32_t count = 0;
};

template<std::size_t Rows, std::size_t Slots>
struct FixedStateMatrix {
    std::array<FixedStateTable<Slots>, Rows> rows{};
    std::uint32_t count = 0;

    static constexpr bool valid(std::uint32_t row, std::uint32_t slot) noexcept {
        return row < Rows && slot < Slots;
    }
    bool contains(std::uint32_t row, std::uint32_t slot) const noexcept {
        return valid(row, slot) && rows[row].contains(slot);
    }
    bool empty() const noexcept {
        return count == 0;
    }
    std::uint32_t size() const noexcept {
        return count;
    }
    bool get(std::uint32_t row,
             std::uint32_t slot,
             std::uint32_t& value) const noexcept {
        return valid(row, slot) && rows[row].get(slot, value);
    }
    void set(std::uint32_t row,
             std::uint32_t slot,
             std::uint32_t value) noexcept {
        if (!valid(row, slot)) {
            return;
        }
        if (!rows[row].contains(slot)) {
            ++count;
        }
        rows[row].set(slot, value);
    }
    void erase(std::uint32_t row, std::uint32_t slot) noexcept {
        if (!contains(row, slot)) {
            return;
        }
        rows[row].erase(slot);
        --count;
    }
    void clear() noexcept {
        for (auto& row : rows) {
            row.clear();
        }
        count = 0;
    }
    template<typename Fn>
    void forEach(Fn&& fn) const {
        for (std::uint32_t row = 0; row < Rows; ++row) {
            rows[row].forEach([&](std::uint32_t slot, std::uint32_t value) {
                fn(row, slot, value);
            });
        }
    }
    bool popFirst(std::uint32_t& row,
                  std::uint32_t& slot,
                  std::uint32_t& value) noexcept {
        for (std::uint32_t r = 0; r < Rows; ++r) {
            if (rows[r].popFirst(slot, value)) {
                row = r;
                --count;
                return true;
            }
        }
        return false;
    }
};

struct FixedTransformTable {
    std::array<D9CMatrix, kPeTransformSlots> values{};
    std::array<std::uint64_t, (kPeTransformSlots + 63u) / 64u> occupied{};
    std::uint32_t count = 0;

    static constexpr bool validSlot(std::uint32_t slot) noexcept {
        return slot < kPeTransformSlots;
    }
    static constexpr std::uint64_t bit(std::uint32_t slot) noexcept {
        return 1ull << (slot & 63u);
    }
    static constexpr std::size_t word(std::uint32_t slot) noexcept {
        return slot >> 6;
    }
    static bool slotForState(std::uint32_t state, std::uint32_t& slot) noexcept {
        if (state == kD3dTsView) {
            slot = 0;
            return true;
        }
        if (state == kD3dTsProjection) {
            slot = 1;
            return true;
        }
        if (state >= kD3dTsTexture0 &&
            state <= kD3dTsTexture7) {
            slot = kPeTransformTextureBaseSlot +
                   (state - kD3dTsTexture0);
            return true;
        }
        if (state >= kD3dTsWorld &&
            state < kD3dTsWorld +
                        kPeTransformWorldSlots) {
            slot = kPeTransformWorldBaseSlot +
                   (state - kD3dTsWorld);
            return true;
        }
        return false;
    }
    static std::uint32_t stateForSlot(std::uint32_t slot) noexcept {
        if (slot == 0) {
            return kD3dTsView;
        }
        if (slot == 1) {
            return kD3dTsProjection;
        }
        if (slot < kPeTransformWorldBaseSlot) {
            return kD3dTsTexture0 +
                   (slot - kPeTransformTextureBaseSlot);
        }
        return kD3dTsWorld +
               (slot - kPeTransformWorldBaseSlot);
    }
    bool containsSlot(std::uint32_t slot) const noexcept {
        return validSlot(slot) && (occupied[word(slot)] & bit(slot)) != 0;
    }
    bool contains(std::uint32_t state) const noexcept {
        std::uint32_t slot = 0;
        return slotForState(state, slot) && containsSlot(slot);
    }
    bool empty() const noexcept {
        return count == 0;
    }
    std::uint32_t size() const noexcept {
        return count;
    }
    bool get(std::uint32_t state, D9CMatrix& value) const noexcept {
        std::uint32_t slot = 0;
        if (!slotForState(state, slot) || !containsSlot(slot)) {
            return false;
        }
        value = values[slot];
        return true;
    }
    void set(std::uint32_t state, const D9CMatrix& value) noexcept {
        std::uint32_t slot = 0;
        if (!slotForState(state, slot)) {
            return;
        }
        const auto w = word(slot);
        const auto b = bit(slot);
        if ((occupied[w] & b) == 0) {
            occupied[w] |= b;
            ++count;
        }
        values[slot] = value;
    }
    void erase(std::uint32_t state) noexcept {
        std::uint32_t slot = 0;
        if (!slotForState(state, slot) || !containsSlot(slot)) {
            return;
        }
        occupied[word(slot)] &= ~bit(slot);
        --count;
    }
    void clear() noexcept {
        occupied = {};
        count = 0;
    }
    template<typename Fn>
    void forEach(Fn&& fn) const {
        for (std::size_t w = 0; w < occupied.size(); ++w) {
            std::uint64_t bits = occupied[w];
            while (bits != 0) {
                const auto b = static_cast<std::uint32_t>(std::countr_zero(bits));
                const auto slot = static_cast<std::uint32_t>(w * 64u + b);
                if (slot < kPeTransformSlots) {
                    fn(stateForSlot(slot), values[slot]);
                }
                bits &= bits - 1u;
            }
        }
    }
    bool popFirst(std::uint32_t& state, D9CMatrix& value) noexcept {
        for (std::size_t w = 0; w < occupied.size(); ++w) {
            std::uint64_t bits = occupied[w];
            if (bits == 0) {
                continue;
            }
            const auto b = static_cast<std::uint32_t>(std::countr_zero(bits));
            const auto slot = static_cast<std::uint32_t>(w * 64u + b);
            state = stateForSlot(slot);
            value = values[slot];
            occupied[w] &= ~bit(slot);
            --count;
            return true;
        }
        return false;
    }
};

// A state-block candidate is deliberately value-only and fixed-size.  Pointer
// members below are non-owning observations; the PE owner takes/releases the
// corresponding COM reference at the recording boundary.  Keeping ownership
// out of this native-buildable header is what lets the category truth table
// exercise the same candidate without windows.h or a COM ABI.
template<typename T, std::size_t Slots>
class FixedTrackedState {
public:
    using value_type = T;

    bool contains(std::size_t slot) const noexcept {
        return slot < Slots &&
               (occupied[slot >> 6u] & (1ull << (slot & 63u))) != 0u;
    }
    bool get(std::size_t slot, T& value) const noexcept {
        if (!contains(slot)) return false;
        value = values[slot];
        return true;
    }
    void set(std::size_t slot, const T& value) noexcept {
        if (slot >= Slots) return;
        const auto word = slot >> 6u;
        const auto bit = 1ull << (slot & 63u);
        if ((occupied[word] & bit) == 0u) {
            occupied[word] |= bit;
            ++count;
        }
        values[slot] = value;
    }
    void erase(std::size_t slot) noexcept {
        if (!contains(slot)) return;
        occupied[slot >> 6u] &= ~(1ull << (slot & 63u));
        --count;
    }
    void clear() noexcept {
        occupied = {};
        count = 0u;
    }
    std::uint32_t size() const noexcept { return count; }
    bool empty() const noexcept { return count == 0u; }
    template<typename Fn>
    void forEach(Fn&& fn) const {
        for (std::size_t word = 0; word < occupied.size(); ++word) {
            auto bits = occupied[word];
            while (bits != 0u) {
                const auto bit = static_cast<std::size_t>(std::countr_zero(bits));
                const auto slot = word * 64u + bit;
                if (slot < Slots) fn(slot, values[slot]);
                bits &= bits - 1u;
            }
        }
    }

private:
    std::array<T, Slots> values{};
    std::array<std::uint64_t, (Slots + 63u) / 64u> occupied{};
    std::uint32_t count = 0u;
};

// These interfaces are deliberately only forward-declared here.  The shadow
// stores COM identity tokens, never dereferences them; this keeps the native
// state algebra independent of the Windows SDK while making each category's
// token type explicit.
struct IDirect3DBaseTexture9;
struct IDirect3DVertexBuffer9;
struct IDirect3DVertexShader9;
struct IDirect3DPixelShader9;
struct IDirect3DVertexDeclaration9;
struct IDirect3DIndexBuffer9;
struct IDirect3DSurface9;
struct IDirect3DQuery9;

struct StateBlockStreamSourceValue {
    struct BufferRef {
        constexpr BufferRef() noexcept = default;
        constexpr IDirect3DVertexBuffer9* raw() const noexcept { return value; }
        friend constexpr bool operator==(BufferRef a,
                                         IDirect3DVertexBuffer9* b) noexcept {
            return a.raw() == b;
        }
    private:
        IDirect3DVertexBuffer9* value = nullptr;
        constexpr explicit BufferRef(IDirect3DVertexBuffer9* rawValue) noexcept
            : value(rawValue) {}
        friend struct StateBlockBufferRefFactory;
#if defined(DXMT9_STATEBLOCK_TEST_SEAM)
        friend struct StateBlockBufferTestFactory;
#endif
    };
    BufferRef buffer{};
    std::uint32_t offset = 0u;
    std::uint32_t stride = 0u;
};

class StateBlockBufferRefCapability {
 public:
  constexpr IDirect3DVertexBuffer9* raw() const noexcept { return value_; }

 private:
  constexpr explicit StateBlockBufferRefCapability(
      IDirect3DVertexBuffer9* value) noexcept
      : value_(value) {}
  IDirect3DVertexBuffer9* value_ = nullptr;
  template<typename Public, typename Raw, typename Wire>
  friend struct D3D9PeValidatedObject;
};

struct StateBlockBufferRefFactory {
    static constexpr StateBlockStreamSourceValue::BufferRef fromValidated(
        StateBlockBufferRefCapability capability) noexcept {
        return StateBlockStreamSourceValue::BufferRef(capability.raw());
    }
};

#if defined(DXMT9_STATEBLOCK_TEST_SEAM)
struct StateBlockBufferTestFactory {
    template<typename P>
    static StateBlockStreamSourceValue::BufferRef fromFake(P* raw) noexcept {
        return StateBlockStreamSourceValue::BufferRef(
            reinterpret_cast<IDirect3DVertexBuffer9*>(raw));
    }
    static StateBlockStreamSourceValue::BufferRef fromFake(
        std::uintptr_t raw) noexcept {
        return StateBlockStreamSourceValue::BufferRef(
            reinterpret_cast<IDirect3DVertexBuffer9*>(raw));
    }
};
#endif

struct StateBlockTextureTag;
struct StateBlockVertexShaderTag;
struct StateBlockPixelShaderTag;
struct StateBlockVertexDeclarationTag;
struct StateBlockIndexBufferTag;
struct StateBlockRenderTargetTag;
struct StateBlockDepthStencilTag;

template<typename Tag>
struct StateBlockComRefTraits;
template<typename Tag>
struct StateBlockComRefFactory;
#if defined(DXMT9_STATEBLOCK_TEST_SEAM)
template<typename Tag>
struct StateBlockComTestFactory;
#endif

template<>
struct StateBlockComRefTraits<StateBlockTextureTag> {
    using raw_type = IDirect3DBaseTexture9;
};
template<>
struct StateBlockComRefTraits<StateBlockVertexShaderTag> {
    using raw_type = IDirect3DVertexShader9;
};
template<>
struct StateBlockComRefTraits<StateBlockPixelShaderTag> {
    using raw_type = IDirect3DPixelShader9;
};
template<>
struct StateBlockComRefTraits<StateBlockVertexDeclarationTag> {
    using raw_type = IDirect3DVertexDeclaration9;
};
template<>
struct StateBlockComRefTraits<StateBlockIndexBufferTag> {
    using raw_type = IDirect3DIndexBuffer9;
};
template<>
struct StateBlockComRefTraits<StateBlockRenderTargetTag> {
    using raw_type = IDirect3DSurface9;
};
template<>
struct StateBlockComRefTraits<StateBlockDepthStencilTag> {
    using raw_type = IDirect3DSurface9;
};

template<typename Tag>
class StateBlockComRefCapability {
 public:
    using raw_type = typename StateBlockComRefTraits<Tag>::raw_type;
    constexpr raw_type* raw() const noexcept { return value_; }

 private:
    constexpr explicit StateBlockComRefCapability(raw_type* value) noexcept
        : value_(value) {}
    raw_type* value_ = nullptr;
    template<typename Public, typename Raw, typename Wire>
    friend struct D3D9PeValidatedObject;
};

template<typename Tag>
struct StateBlockComRef {
    using raw_type = typename StateBlockComRefTraits<Tag>::raw_type;
    constexpr StateBlockComRef() noexcept = default;
    constexpr raw_type* raw() const noexcept { return value; }
    friend constexpr bool operator==(StateBlockComRef a,
                                     StateBlockComRef b) noexcept {
        return a.raw() == b.raw();
    }
    friend constexpr bool operator==(StateBlockComRef a,
                                     raw_type* b) noexcept {
        return a.raw() == b;
    }
private:
    raw_type* value = nullptr;
    constexpr explicit StateBlockComRef(raw_type* rawValue) noexcept
        : value(rawValue) {}
    friend struct StateBlockComRefFactory<Tag>;
#if defined(DXMT9_STATEBLOCK_TEST_SEAM)
    friend struct StateBlockComTestFactory<Tag>;
#endif
};

template<typename Tag>
struct StateBlockComRefFactory {
    using Ref = StateBlockComRef<Tag>;
    using raw_type = typename Ref::raw_type;
    static constexpr Ref fromValidated(
        StateBlockComRefCapability<Tag> capability) noexcept {
        return Ref(capability.raw());
    }
};

#if defined(DXMT9_STATEBLOCK_TEST_SEAM)
template<typename Tag>
struct StateBlockComTestFactory {
    using Ref = StateBlockComRef<Tag>;
    using raw_type = typename Ref::raw_type;
    template<typename P>
    static constexpr Ref fromFake(P* raw) noexcept {
        return Ref(reinterpret_cast<raw_type*>(raw));
    }
    static constexpr Ref fromFake(std::uintptr_t raw) noexcept {
        return Ref(reinterpret_cast<raw_type*>(raw));
    }
};
#endif
using StateBlockTextureRef = StateBlockComRef<StateBlockTextureTag>;
using StateBlockVertexShaderRef = StateBlockComRef<StateBlockVertexShaderTag>;
using StateBlockPixelShaderRef = StateBlockComRef<StateBlockPixelShaderTag>;
using StateBlockVertexDeclarationRef =
    StateBlockComRef<StateBlockVertexDeclarationTag>;
using StateBlockIndexBufferRef = StateBlockComRef<StateBlockIndexBufferTag>;
using StateBlockRenderTargetRef = StateBlockComRef<StateBlockRenderTargetTag>;
using StateBlockDepthStencilRef = StateBlockComRef<StateBlockDepthStencilTag>;

template<typename Ref>
struct StateBlockComRefTagFor;
#define DXMT9_STATEBLOCK_REF_TAG(ref_, tag_) \
    template<> struct StateBlockComRefTagFor<ref_> { using type = tag_; };
DXMT9_STATEBLOCK_REF_TAG(StateBlockTextureRef, StateBlockTextureTag)
DXMT9_STATEBLOCK_REF_TAG(StateBlockVertexShaderRef, StateBlockVertexShaderTag)
DXMT9_STATEBLOCK_REF_TAG(StateBlockPixelShaderRef, StateBlockPixelShaderTag)
DXMT9_STATEBLOCK_REF_TAG(StateBlockVertexDeclarationRef,
                         StateBlockVertexDeclarationTag)
DXMT9_STATEBLOCK_REF_TAG(StateBlockIndexBufferRef, StateBlockIndexBufferTag)
DXMT9_STATEBLOCK_REF_TAG(StateBlockRenderTargetRef, StateBlockRenderTargetTag)
DXMT9_STATEBLOCK_REF_TAG(StateBlockDepthStencilRef, StateBlockDepthStencilTag)
#undef DXMT9_STATEBLOCK_REF_TAG

enum class StateBlockApplyCategoryRole : std::uint8_t {
    Value,
    ImplicitFvf,
    CandidateOwnedVertexDeclaration,
    // Staged roles stay contiguous: Prepare and lifetime coverage use this
    // boundary to classify every pre-effect retain role exhaustively.
    StagedTexture,
    StagedStreamSource,
    StagedVertexShader,
    StagedPixelShader,
    StagedIndexBuffer,
    StagedRenderTarget,
    StagedDepthStencil,
};

using StateBlockClipPlaneValue = std::array<float, 4>;

enum class StateBlockApplyPhysicalKind : std::uint8_t {
    Keyed,
    Fixed,
    Constant,
};

// The one authoritative APPLY PHYSICAL inventory. Its 26 rows are the four
// keyed stores, sixteen fixed stores, and six constant stores physically
// owned by StateBlockRecorded. Clear, candidate lifetime, Apply Prepare, and
// Apply Commit all instantiate the typed physical visitor, so adding a row
// without defining every relevant behavior is a compile failure.
#define DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY(KEYED, FIXED, CONSTANT)     \
    KEYED(renderStates)                                                       \
    KEYED(textureStageStates)                                                 \
    KEYED(samplerStates)                                                      \
    KEYED(transforms)                                                         \
    FIXED(textures, textures_, StateBlockTextureRef, kPeTextureSlots,         \
      StagedTexture)                                                          \
    FIXED(streamSources, streamSources_, StateBlockStreamSourceValue,         \
      D9C_DRAW_PACKET_MAX_STREAMS, StagedStreamSource)                        \
    FIXED(streamFrequencies, streamFrequencies_, std::uint32_t,               \
      D9C_DRAW_PACKET_MAX_STREAMS, Value)                                     \
    FIXED(vertexShader, vertexShader_, StateBlockVertexShaderRef, 1,          \
      StagedVertexShader)                                                     \
    FIXED(pixelShader, pixelShader_, StateBlockPixelShaderRef, 1,             \
      StagedPixelShader)                                                      \
    FIXED(fvf, fvf_, std::uint32_t, 1, ImplicitFvf)                           \
    FIXED(vertexDeclaration, vertexDeclaration_,                             \
      StateBlockVertexDeclarationRef,                                        \
      1, CandidateOwnedVertexDeclaration)                                    \
    FIXED(indexBuffer, indexBuffer_, StateBlockIndexBufferRef, 1,             \
      StagedIndexBuffer)                                                      \
    FIXED(renderTargets, renderTargets_, StateBlockRenderTargetRef,           \
      D9C_DRAW_PACKET_MAX_RENDER_TARGETS, StagedRenderTarget)                 \
    FIXED(depthStencil, depthStencil_, StateBlockDepthStencilRef, 1,          \
      StagedDepthStencil)                                                     \
    FIXED(viewport, viewport_, D9CViewport, 1, Value)                         \
    FIXED(scissor, scissor_, D9CRect, 1, Value)                               \
    FIXED(material, material_, D9CMaterial, 1, Value)                         \
    FIXED(clipPlanes, clipPlanes_, StateBlockClipPlaneValue, 6, Value)        \
    FIXED(lights, lights_, D9CLight, D9C_DRAW_PACKET_MAX_LIGHTS, Value)       \
    FIXED(lightEnables, lightEnables_, std::uint32_t,                         \
      D9C_DRAW_PACKET_MAX_LIGHTS, Value)                                      \
    CONSTANT(vsConstF)                                                        \
    CONSTANT(vsConstI)                                                        \
    CONSTANT(vsConstB)                                                        \
    CONSTANT(psConstF)                                                        \
    CONSTANT(psConstI)                                                        \
    CONSTANT(psConstB)

enum class StateBlockApplyPhysicalStore : std::uint8_t {
#define DXMT9_STATEBLOCK_PHYSICAL_ENUM_KEYED(name) name,
#define DXMT9_STATEBLOCK_PHYSICAL_ENUM_FIXED(name, storage, type, slots, role) name,
#define DXMT9_STATEBLOCK_PHYSICAL_ENUM_CONSTANT(name) name,
    DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY(
        DXMT9_STATEBLOCK_PHYSICAL_ENUM_KEYED,
        DXMT9_STATEBLOCK_PHYSICAL_ENUM_FIXED,
        DXMT9_STATEBLOCK_PHYSICAL_ENUM_CONSTANT)
#undef DXMT9_STATEBLOCK_PHYSICAL_ENUM_KEYED
#undef DXMT9_STATEBLOCK_PHYSICAL_ENUM_FIXED
#undef DXMT9_STATEBLOCK_PHYSICAL_ENUM_CONSTANT
    Count,
};

struct StateBlockApplyPhysicalDescriptor {
    StateBlockApplyPhysicalStore store;
    StateBlockApplyPhysicalKind kind;
    StateBlockApplyCategoryRole role;
};

#define DXMT9_STATEBLOCK_PHYSICAL_DESC_KEYED(name)                           \
    StateBlockApplyPhysicalDescriptor{                                       \
        StateBlockApplyPhysicalStore::name,                                  \
        StateBlockApplyPhysicalKind::Keyed,                                  \
        StateBlockApplyCategoryRole::Value},
#define DXMT9_STATEBLOCK_PHYSICAL_DESC_FIXED(name, storage, type, slots, role) \
    StateBlockApplyPhysicalDescriptor{                                        \
        StateBlockApplyPhysicalStore::name,                                   \
        StateBlockApplyPhysicalKind::Fixed,                                   \
        StateBlockApplyCategoryRole::role},
#define DXMT9_STATEBLOCK_PHYSICAL_DESC_CONSTANT(name)                        \
    StateBlockApplyPhysicalDescriptor{                                       \
        StateBlockApplyPhysicalStore::name,                                  \
        StateBlockApplyPhysicalKind::Constant,                               \
        StateBlockApplyCategoryRole::Value},
inline constexpr auto kStateBlockApplyPhysicalInventory = std::array{
    DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY(
        DXMT9_STATEBLOCK_PHYSICAL_DESC_KEYED,
        DXMT9_STATEBLOCK_PHYSICAL_DESC_FIXED,
        DXMT9_STATEBLOCK_PHYSICAL_DESC_CONSTANT)
};
#undef DXMT9_STATEBLOCK_PHYSICAL_DESC_KEYED
#undef DXMT9_STATEBLOCK_PHYSICAL_DESC_FIXED
#undef DXMT9_STATEBLOCK_PHYSICAL_DESC_CONSTANT

static_assert(kStateBlockApplyPhysicalInventory.size() == 26u,
              "StateBlock APPLY PHYSICAL inventory must name 26 stores");

template<StateBlockApplyPhysicalStore Wanted>
consteval StateBlockApplyPhysicalDescriptor stateBlockApplyPhysicalDescriptor() {
    constexpr bool found = [] {
        for (const auto descriptor : kStateBlockApplyPhysicalInventory) {
            if (descriptor.store == Wanted) return true;
        }
        return false;
    }();
    static_assert(found, "physical store omitted");
    for (const auto descriptor : kStateBlockApplyPhysicalInventory) {
        if (descriptor.store == Wanted) return descriptor;
    }
    return {};
}

enum class StateBlockCaptureDisposition : std::uint8_t {
    All,
    VertexState,
    PixelState,
    Explicit,
};

enum class StateBlockApplyPublication : std::uint8_t {
    BackendAuthoritative,
    PeReplayRequired,
};

// CreateStateBlock blocks have a complete unix-side snapshot.  Recorded
// Begin/End blocks, however, are represented authoritatively by the typed PE
// snapshot: setters made while Recording never enter PendingDelta and the
// legacy unix recorder only owns an empty compatibility shell.  Applying an
// Explicit block must therefore republish its tracked values through the PE
// recorder so the next ordered chunk observes the same state as the PE live
// shadow.
constexpr StateBlockApplyPublication stateBlockApplyPublication(
    StateBlockCaptureDisposition disposition) noexcept {
    return disposition == StateBlockCaptureDisposition::Explicit
        ? StateBlockApplyPublication::PeReplayRequired
        : StateBlockApplyPublication::BackendAuthoritative;
}

enum class StateBlockCaptureCategory : std::uint8_t {
    RenderState,
    TextureStageState,
    SamplerState,
    Transform,
    Texture,
    StreamSource,
    StreamFrequency,
    IndexBuffer,
    VertexShader,
    PixelShader,
    Fvf,
    VertexDeclaration,
    RenderTarget,
    DepthStencil,
    Viewport,
    Scissor,
    Material,
    ClipPlane,
    Light,
    LightEnable,
    VertexConstants,
    PixelConstants,
};

constexpr StateBlockCaptureDisposition stateBlockCaptureDispositionFromType(
    std::uint32_t type) noexcept {
    return type == 2u ? StateBlockCaptureDisposition::PixelState
         : type == 3u ? StateBlockCaptureDisposition::VertexState
                      : StateBlockCaptureDisposition::All;
}

constexpr bool stateBlockCaptureCategorySelected(
    StateBlockCaptureDisposition disposition,
    StateBlockCaptureCategory category) noexcept {
    if (disposition == StateBlockCaptureDisposition::All ||
        disposition == StateBlockCaptureDisposition::Explicit) {
        return true;
    }
    if (disposition == StateBlockCaptureDisposition::VertexState) {
        switch (category) {
        case StateBlockCaptureCategory::RenderState:
        case StateBlockCaptureCategory::TextureStageState:
        case StateBlockCaptureCategory::SamplerState:
        case StateBlockCaptureCategory::VertexShader:
        case StateBlockCaptureCategory::Fvf:
        case StateBlockCaptureCategory::VertexDeclaration:
        case StateBlockCaptureCategory::Light:
        case StateBlockCaptureCategory::LightEnable:
        case StateBlockCaptureCategory::VertexConstants:
            return true;
        default:
            return false;
        }
    }
    switch (category) {
    case StateBlockCaptureCategory::RenderState:
    case StateBlockCaptureCategory::TextureStageState:
    case StateBlockCaptureCategory::SamplerState:
    case StateBlockCaptureCategory::PixelShader:
    case StateBlockCaptureCategory::PixelConstants:
        return true;
    default:
        return false;
    }
}

// D3D9's mixed render/TSS/sampler families have vertex and pixel subsets.
// These are the numeric D3D9 enum values, kept here so the host shadow header
// remains independent of Windows headers while matching the backend masks.
constexpr bool stateBlockRenderStateSelected(
    StateBlockCaptureDisposition disposition, std::uint32_t key) noexcept {
    if (disposition == StateBlockCaptureDisposition::All ||
        disposition == StateBlockCaptureDisposition::Explicit) return true;
    if (disposition == StateBlockCaptureDisposition::VertexState) {
        return key == 9u || key == 22u || key == 29u ||
               (key >= 34u && key <= 38u) ||
               key == 48u || (key >= 136u && key <= 137u) || key == 139u ||
               (key >= 140u && key <= 143u) || (key >= 145u && key <= 148u) ||
               key == 151u || key == 152u || (key >= 154u && key <= 163u) ||
               key == 166u || key == 167u || key == 170u ||
               (key >= 172u && key <= 173u) || (key >= 178u && key <= 184u);
    }
    return key == 7u || key == 8u || key == 9u || key == 14u || key == 15u ||
           key == 16u || key == 19u || key == 20u || (key >= 23u && key <= 27u) ||
           (key >= 52u && key <= 60u) ||
           (key >= 128u && key <= 135u) || key == 174u || key == 175u ||
           (key >= 185u && key <= 195u) ||
           (key >= 198u && key <= 209u);
}

constexpr bool stateBlockTextureStageStateSelected(
    StateBlockCaptureDisposition disposition, std::uint32_t key) noexcept {
    if (disposition == StateBlockCaptureDisposition::All ||
        disposition == StateBlockCaptureDisposition::Explicit) return true;
    if (disposition == StateBlockCaptureDisposition::VertexState)
        return key == 11u || key == 24u;
    return (key >= 1u && key <= 11u) || (key >= 22u && key <= 24u) ||
           (key >= 26u && key <= 28u) || key == 32u;
}

constexpr bool stateBlockSamplerStateSelected(
    StateBlockCaptureDisposition disposition, std::uint32_t key) noexcept {
    if (disposition == StateBlockCaptureDisposition::All ||
        disposition == StateBlockCaptureDisposition::Explicit) return true;
    return disposition == StateBlockCaptureDisposition::VertexState
        ? key == 13u
        : key >= 1u && key <= 12u;
}

inline std::uint32_t textureStageSlot(std::uint32_t stage) noexcept {
    return std::min<std::uint32_t>(stage, kPeTextureStageSlots - 1u);
}

inline bool vertexTextureSamplerSlot(std::uint32_t sampler, std::uint32_t& slot) noexcept {
    if (sampler < kD3dVertexTextureSampler0 || sampler > kD3dVertexTextureSampler3) {
        return false;
    }
    slot = kPeFragmentSamplerSlots + (sampler - kD3dVertexTextureSampler0);
    return true;
}

inline bool textureBindingSlot(std::uint32_t stage, std::uint32_t& slot) noexcept {
    if (stage < kPeFragmentSamplerSlots) {
        slot = stage;
        return true;
    }
    return vertexTextureSamplerSlot(stage, slot);
}

inline std::uint32_t textureStageStateSlot(std::uint32_t type) noexcept {
    return std::min<std::uint32_t>(type, kPeTextureStageStateSlots - 1u);
}

inline bool samplerSlot(std::uint32_t sampler, std::uint32_t& slot) noexcept {
    if (sampler < kPeFragmentSamplerSlots) {
        slot = sampler;
        return true;
    }
    return vertexTextureSamplerSlot(sampler, slot);
}

inline bool samplerStateSlot(std::uint32_t type,
                             std::uint32_t& slot) noexcept {
    slot = type;
    return slot < kPeSamplerStateSlots;
}

// ---------------------------------------------------------------------------
// Typed slot keys (additive, migration-only untyped surface stays above).
//
// R-PE-TYPED-SLOTS: the audit that motivated this section found the tables
// above keyed by a bare std::uint32_t, so nothing at the type level stops
// e.g. a sampler index from being passed where a render-state slot, TSS
// type, or transform state was expected -- the only thing that would catch
// it is differential-golden coverage happening to exercise that exact path.
//
// Six distinct index spaces are in play here, verified against the code
// above rather than assumed:
//   - RenderStateSlot        D3DRS_* value, used directly as the
//                             FixedStateTable<kPeRenderStateSlots> index
//                             (renderStateShadow / pendingRenderStates /
//                             stateBlock recorded render states).
//   - TextureStageIndex      texture-stage ordinal (0..7), the ROW of the
//                             kPeTextureStageSlots x kPeTextureStageStateSlots
//                             matrices (tssShadow / pendingTss), produced by
//                             clamping via textureStageSlot().
//   - TextureStageStateType  D3DTSS_* type, the COLUMN of the same TSS
//                             matrices, produced by textureStageStateSlot().
//   - SamplerIndex           internal sampler slot (0..kPeSamplerSlots-1,
//                             fragment samplers then vertex-texture
//                             samplers), the ROW of samplerStateShadow /
//                             pendingSamplerStates, produced by samplerSlot()
//                             / textureBindingSlot() from an external
//                             D3DSAMP_*/D3DDMAPSAMPLER-relative ordinal.
//   - SamplerStateType       D3DSAMPLERSTATETYPE value, the COLUMN of the
//                             same sampler matrices, produced by
//                             samplerStateSlot().
//   - TransformState         D3DTRANSFORMSTATETYPE value, the external key
//                             FixedTransformTable::get/set/erase/contains
//                             accept directly (it does its own internal
//                             slotForState()/stateForSlot() remapping).
//
// Each index space is a one-word bounded value. Construction is centralized:
// invalid external ordinals produce the invalid sentinel and every table API
// consequently fails closed. The private raw constructor prevents callers
// from laundering an arbitrary integer through static_cast as the former
// empty enum-class tags allowed.
template<typename Tag, std::uint32_t Limit>
class BoundedSlotKey {
public:
    constexpr BoundedSlotKey() noexcept = default;

    static constexpr BoundedSlotKey fromRaw(std::uint32_t value) noexcept {
        return BoundedSlotKey(value < Limit ? value : kInvalid);
    }
    constexpr bool valid() const noexcept { return value_ < Limit; }
    constexpr std::uint32_t raw() const noexcept { return value_; }
    friend constexpr bool operator==(BoundedSlotKey, BoundedSlotKey) = default;

private:
    static constexpr std::uint32_t kInvalid = UINT32_MAX;
    explicit constexpr BoundedSlotKey(std::uint32_t value) noexcept
        : value_(value) {}

    std::uint32_t value_ = kInvalid;
};

struct RenderStateSlotTag;
struct TextureStageIndexTag;
struct TextureStageStateTypeTag;
struct SamplerIndexTag;
struct SamplerStateTypeTag;
struct TransformStateTag;
using RenderStateSlot =
    BoundedSlotKey<RenderStateSlotTag, kPeRenderStateSlots>;
using TextureStageIndex =
    BoundedSlotKey<TextureStageIndexTag, kPeTextureStageSlots>;
using TextureStageStateType =
    BoundedSlotKey<TextureStageStateTypeTag, kPeTextureStageStateSlots>;
using SamplerIndex = BoundedSlotKey<SamplerIndexTag, kPeSamplerSlots>;
using SamplerStateType =
    BoundedSlotKey<SamplerStateTypeTag, kPeSamplerStateSlots>;
struct StateBlockTextureSlotTag;
struct StateBlockStreamSlotTag;
struct StateBlockRenderTargetSlotTag;
using StateBlockTextureSlot =
    BoundedSlotKey<StateBlockTextureSlotTag, kPeTextureSlots>;
using StateBlockStreamSlot =
    BoundedSlotKey<StateBlockStreamSlotTag, D9C_DRAW_PACKET_MAX_STREAMS>;
using StateBlockRenderTargetSlot =
    BoundedSlotKey<StateBlockRenderTargetSlotTag,
                   D9C_DRAW_PACKET_MAX_RENDER_TARGETS>;

class TransformState {
public:
    constexpr TransformState() noexcept = default;

    static TransformState fromRaw(std::uint32_t value) noexcept {
        std::uint32_t slot = 0u;
        return FixedTransformTable::slotForState(value, slot)
            ? TransformState(value) : TransformState();
    }
    bool valid() const noexcept {
        std::uint32_t slot = 0u;
        return FixedTransformTable::slotForState(value_, slot);
    }
    constexpr std::uint32_t raw() const noexcept { return value_; }
    friend constexpr bool operator==(TransformState, TransformState) = default;

private:
    explicit constexpr TransformState(std::uint32_t value) noexcept
        : value_(value) {}

    std::uint32_t value_ = UINT32_MAX;
};

template<typename Tag, std::uint32_t Limit>
constexpr std::uint32_t rawSlot(BoundedSlotKey<Tag, Limit> key) noexcept {
    return key.raw();
}
constexpr std::uint32_t rawSlot(TransformState key) noexcept {
    return key.raw();
}

// Typed constructors. These wrap the existing untyped clamp/lookup
// functions above (textureStageSlot, textureStageStateSlot, samplerSlot,
// samplerStateSlot) rather than duplicating their logic, so the typed and
// untyped surfaces can never disagree about what a given external D3D9
// ordinal maps to.
constexpr RenderStateSlot renderStateSlotKey(std::uint32_t state) noexcept {
    return RenderStateSlot::fromRaw(state);
}
constexpr TextureStageIndex textureStageIndexKey(std::uint32_t stage) noexcept {
    return TextureStageIndex::fromRaw(stage);
}
constexpr TextureStageStateType textureStageStateTypeKey(std::uint32_t type) noexcept {
    return TextureStageStateType::fromRaw(type);
}
inline bool samplerIndexKey(std::uint32_t sampler, SamplerIndex& out) noexcept {
    std::uint32_t slot = 0;
    if (!samplerSlot(sampler, slot)) {
        return false;
    }
    out = SamplerIndex::fromRaw(slot);
    return true;
}
constexpr std::uint32_t samplerForSlot(SamplerIndex sampler) noexcept {
    const std::uint32_t slot = rawSlot(sampler);
    return slot < kPeFragmentSamplerSlots
        ? slot
        : kD3dVertexTextureSampler0 +
              (slot - kPeFragmentSamplerSlots);
}
inline bool samplerStateTypeKey(std::uint32_t type, SamplerStateType& out) noexcept {
    std::uint32_t slot = 0;
    if (!samplerStateSlot(type, slot)) {
        return false;
    }
    out = SamplerStateType::fromRaw(slot);
    return true;
}
inline TransformState transformStateKey(std::uint32_t state) noexcept {
    return TransformState::fromRaw(state);
}
constexpr StateBlockTextureSlot stateBlockTextureSlotKey(
    std::uint32_t slot) noexcept {
    return StateBlockTextureSlot::fromRaw(slot);
}
constexpr StateBlockStreamSlot stateBlockStreamSlotKey(
    std::uint32_t slot) noexcept {
    return StateBlockStreamSlot::fromRaw(slot);
}
constexpr StateBlockRenderTargetSlot stateBlockRenderTargetSlotKey(
    std::uint32_t slot) noexcept {
    return StateBlockRenderTargetSlot::fromRaw(slot);
}

template<StateBlockApplyPhysicalStore Store>
struct StateBlockFixedSlotTag;

template<StateBlockApplyPhysicalStore Store>
consteval std::uint32_t stateBlockFixedSlotLimit() {
#define DXMT9_STATEBLOCK_FIXED_LIMIT_KEYED(name)
#define DXMT9_STATEBLOCK_FIXED_LIMIT(name, storage, type, slots, role)       \
    if constexpr (Store == StateBlockApplyPhysicalStore::name) {           \
        return static_cast<std::uint32_t>(slots);                           \
    } else
#define DXMT9_STATEBLOCK_FIXED_LIMIT_CONSTANT(name)
    DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY(
        DXMT9_STATEBLOCK_FIXED_LIMIT_KEYED,
        DXMT9_STATEBLOCK_FIXED_LIMIT,
        DXMT9_STATEBLOCK_FIXED_LIMIT_CONSTANT)
#undef DXMT9_STATEBLOCK_FIXED_LIMIT_KEYED
#undef DXMT9_STATEBLOCK_FIXED_LIMIT
#undef DXMT9_STATEBLOCK_FIXED_LIMIT_CONSTANT
    {
        static_assert(Store != Store,
                      "StateBlock store is not a fixed tracked store");
        return 0u;
    }
}

template<StateBlockApplyPhysicalStore Store>
using StateBlockFixedSlotKey = BoundedSlotKey<
    StateBlockFixedSlotTag<Store>, stateBlockFixedSlotLimit<Store>()>;

template<StateBlockApplyPhysicalStore Store>
constexpr StateBlockFixedSlotKey<Store> stateBlockFixedSlotKey(
    std::uint32_t slot) noexcept {
    return StateBlockFixedSlotKey<Store>::fromRaw(slot);
}

// Mutation-only façade for a single fixed store.  The store kind is part of
// the key type, and every operation validates that key before touching the
// flat occupancy/value arrays.  The façade itself is one reference and does
// not allocate or duplicate storage.
template<typename Key, typename T, std::size_t Slots>
class FixedTrackedStateWriter {
public:
    using value_type = T;

    explicit FixedTrackedStateWriter(FixedTrackedState<T, Slots>& state) noexcept
        : state_(state) {}

    bool contains(Key key) const noexcept {
        return key.valid() && state_.contains(rawSlot(key));
    }
    bool get(Key key, T& value) const noexcept {
        return key.valid() && state_.get(rawSlot(key), value);
    }
    bool set(Key key, const T& value) noexcept {
        if (!key.valid()) return false;
        state_.set(rawSlot(key), value);
        return true;
    }
    bool erase(Key key) noexcept {
        if (!key.valid()) return false;
        state_.erase(rawSlot(key));
        return true;
    }
    void clear() noexcept { state_.clear(); }
    std::uint32_t size() const noexcept { return state_.size(); }
    bool empty() const noexcept { return state_.empty(); }
    template<typename Fn>
    void forEach(Fn&& fn) const {
        state_.forEach([&](std::size_t slot, const T& value) {
            const Key key = Key::fromRaw(static_cast<std::uint32_t>(slot));
            if (key.valid()) fn(key, value);
        });
    }

private:
    FixedTrackedState<T, Slots>& state_;
};

// Typed façades over the existing untyped tables. Each view holds a single
// reference to the same storage the untyped surface uses (no duplicated
// state, nothing to keep in sync) and forwards every call straight to the
// underlying get/set/contains/erase; the Key (and, for the matrix view,
// RowKey/ColKey) template parameters are fixed by the alias a call site
// uses, so passing a foreign key type is a hard compile error rather than a
// silent cross-index-space bug.
template<typename Key, std::size_t Slots>
class TypedStateTableView {
public:
    explicit constexpr TypedStateTableView(FixedStateTable<Slots>& table) noexcept
        : table_(table) {}

    bool contains(Key key) const noexcept { return table_.contains(rawSlot(key)); }
    bool empty() const noexcept { return table_.empty(); }
    std::uint32_t size() const noexcept { return table_.size(); }
    bool get(Key key, std::uint32_t& value) const noexcept {
        return table_.get(rawSlot(key), value);
    }
    void set(Key key, std::uint32_t value) noexcept { table_.set(rawSlot(key), value); }
    void erase(Key key) noexcept { table_.erase(rawSlot(key)); }
    void clear() noexcept { table_.clear(); }
    template<typename Fn>
    void forEach(Fn&& fn) const {
        table_.forEach([&](std::uint32_t slot, std::uint32_t value) {
            const Key key = Key::fromRaw(slot);
            if (key.valid()) fn(key, value);
        });
    }

private:
    FixedStateTable<Slots>& table_;
};

template<typename RowKey, typename ColKey, std::size_t Rows, std::size_t Slots>
class TypedStateMatrixView {
public:
    explicit constexpr TypedStateMatrixView(FixedStateMatrix<Rows, Slots>& matrix) noexcept
        : matrix_(matrix) {}

    bool contains(RowKey row, ColKey col) const noexcept {
        return matrix_.contains(rawSlot(row), rawSlot(col));
    }
    bool empty() const noexcept { return matrix_.empty(); }
    std::uint32_t size() const noexcept { return matrix_.size(); }
    bool get(RowKey row, ColKey col, std::uint32_t& value) const noexcept {
        return matrix_.get(rawSlot(row), rawSlot(col), value);
    }
    void set(RowKey row, ColKey col, std::uint32_t value) noexcept {
        matrix_.set(rawSlot(row), rawSlot(col), value);
    }
    void erase(RowKey row, ColKey col) noexcept {
        matrix_.erase(rawSlot(row), rawSlot(col));
    }
    void clear() noexcept { matrix_.clear(); }
    template<typename Fn>
    void forEach(Fn&& fn) const {
        matrix_.forEach([&](std::uint32_t row, std::uint32_t col,
                            std::uint32_t value) {
            const RowKey rowKey = RowKey::fromRaw(row);
            const ColKey colKey = ColKey::fromRaw(col);
            if (rowKey.valid() && colKey.valid()) fn(rowKey, colKey, value);
        });
    }

private:
    FixedStateMatrix<Rows, Slots>& matrix_;
};

class TypedTransformTableView {
public:
    explicit constexpr TypedTransformTableView(FixedTransformTable& table) noexcept
        : table_(table) {}

    bool contains(TransformState state) const noexcept {
        return table_.contains(rawSlot(state));
    }
    bool empty() const noexcept { return table_.empty(); }
    std::uint32_t size() const noexcept { return table_.size(); }
    bool get(TransformState state, D9CMatrix& value) const noexcept {
        return table_.get(rawSlot(state), value);
    }
    void set(TransformState state, const D9CMatrix& value) noexcept {
        table_.set(rawSlot(state), value);
    }
    void erase(TransformState state) noexcept { table_.erase(rawSlot(state)); }
    void clear() noexcept { table_.clear(); }
    template<typename Fn>
    void forEach(Fn&& fn) const {
        table_.forEach([&](std::uint32_t state, const D9CMatrix& value) {
            const TransformState key = TransformState::fromRaw(state);
            if (key.valid()) fn(key, value);
        });
    }

private:
    FixedTransformTable& table_;
};

using RenderStateTableView = TypedStateTableView<RenderStateSlot, kPeRenderStateSlots>;
using TssTableView = TypedStateMatrixView<TextureStageIndex, TextureStageStateType,
                                          kPeTextureStageSlots, kPeTextureStageStateSlots>;
using SamplerStateTableView = TypedStateMatrixView<SamplerIndex, SamplerStateType,
                                                    kPeSamplerSlots, kPeSamplerStateSlots>;

// Read-only counterparts, needed wherever the shadow is only reachable
// through a `const PeHotStateShadow&` (e.g. mini-replay / process-vertices
// read paths). Same zero-overhead reasoning as the mutable views above;
// these simply omit set/erase/clear because a const reference cannot offer
// them.
template<typename Key, std::size_t Slots>
class ConstTypedStateTableView {
public:
    explicit constexpr ConstTypedStateTableView(const FixedStateTable<Slots>& table) noexcept
        : table_(table) {}

    bool contains(Key key) const noexcept { return table_.contains(rawSlot(key)); }
    bool empty() const noexcept { return table_.empty(); }
    std::uint32_t size() const noexcept { return table_.size(); }
    bool get(Key key, std::uint32_t& value) const noexcept {
        return table_.get(rawSlot(key), value);
    }
    template<typename Fn>
    void forEach(Fn&& fn) const {
        table_.forEach([&](std::uint32_t slot, std::uint32_t value) {
            const Key key = Key::fromRaw(slot);
            if (key.valid()) fn(key, value);
        });
    }

private:
    const FixedStateTable<Slots>& table_;
};

template<typename RowKey, typename ColKey, std::size_t Rows, std::size_t Slots>
class ConstTypedStateMatrixView {
public:
    explicit constexpr ConstTypedStateMatrixView(
        const FixedStateMatrix<Rows, Slots>& matrix) noexcept
        : matrix_(matrix) {}

    bool contains(RowKey row, ColKey col) const noexcept {
        return matrix_.contains(rawSlot(row), rawSlot(col));
    }
    bool empty() const noexcept { return matrix_.empty(); }
    std::uint32_t size() const noexcept { return matrix_.size(); }
    bool get(RowKey row, ColKey col, std::uint32_t& value) const noexcept {
        return matrix_.get(rawSlot(row), rawSlot(col), value);
    }
    template<typename Fn>
    void forEach(Fn&& fn) const {
        matrix_.forEach([&](std::uint32_t row, std::uint32_t col,
                            std::uint32_t value) {
            const RowKey rowKey = RowKey::fromRaw(row);
            const ColKey colKey = ColKey::fromRaw(col);
            if (rowKey.valid() && colKey.valid()) fn(rowKey, colKey, value);
        });
    }

private:
    const FixedStateMatrix<Rows, Slots>& matrix_;
};

class ConstTypedTransformTableView {
public:
    explicit constexpr ConstTypedTransformTableView(const FixedTransformTable& table) noexcept
        : table_(table) {}

    bool contains(TransformState state) const noexcept {
        return table_.contains(rawSlot(state));
    }
    bool empty() const noexcept { return table_.empty(); }
    std::uint32_t size() const noexcept { return table_.size(); }
    bool get(TransformState state, D9CMatrix& value) const noexcept {
        return table_.get(rawSlot(state), value);
    }
    template<typename Fn>
    void forEach(Fn&& fn) const {
        table_.forEach([&](std::uint32_t state, const D9CMatrix& value) {
            const TransformState key = TransformState::fromRaw(state);
            if (key.valid()) fn(key, value);
        });
    }

private:
    const FixedTransformTable& table_;
};

using ConstRenderStateTableView =
    ConstTypedStateTableView<RenderStateSlot, kPeRenderStateSlots>;
using ConstTssTableView =
    ConstTypedStateMatrixView<TextureStageIndex, TextureStageStateType,
                              kPeTextureStageSlots, kPeTextureStageStateSlots>;
using ConstSamplerStateTableView =
    ConstTypedStateMatrixView<SamplerIndex, SamplerStateType,
                              kPeSamplerSlots, kPeSamplerStateSlots>;

inline D9CMatrix identityTransformMatrix() noexcept {
    D9CMatrix matrix{};
    matrix.m[0] = 1.0f;
    matrix.m[5] = 1.0f;
    matrix.m[10] = 1.0f;
    matrix.m[15] = 1.0f;
    return matrix;
}

inline bool matrixEquals(const D9CMatrix& a, const D9CMatrix& b) noexcept {
    return std::memcmp(&a, &b, sizeof(D9CMatrix)) == 0;
}

struct PeHotStateShadow;

class LiveShadow {
private:
    D9CViewport viewportShadow_{};
    D9CRect scissorShadow_{};
    D9CMaterial materialShadow_{};
    float clipPlaneShadow_[6 * 4]{};
    D9CLight lightShadow_[D9C_DRAW_PACKET_MAX_LIGHTS]{};
    std::uint32_t lightEnableShadow_ = 0;

    RenderStateTableView renderStates() noexcept {
        return RenderStateTableView(renderStates_);
    }
    TssTableView textureStageStates() noexcept {
        return TssTableView(textureStageStates_);
    }
    SamplerStateTableView samplerStates() noexcept {
        return SamplerStateTableView(samplerStates_);
    }
    TypedTransformTableView transforms() noexcept {
        return TypedTransformTableView(transforms_);
    }
public:
    ConstRenderStateTableView renderStates() const noexcept {
        return ConstRenderStateTableView(renderStates_);
    }
    ConstTssTableView textureStageStates() const noexcept {
        return ConstTssTableView(textureStageStates_);
    }
    ConstSamplerStateTableView samplerStates() const noexcept {
        return ConstSamplerStateTableView(samplerStates_);
    }
    ConstTypedTransformTableView transforms() const noexcept {
        return ConstTypedTransformTableView(transforms_);
    }

private:
    void clearServerTables() noexcept {
        renderStates_.clear();
        textureStageStates_.clear();
        samplerStates_.clear();
        transforms_.clear();
    }

private:
    FixedStateTable<kPeRenderStateSlots> renderStates_{};
    FixedStateMatrix<kPeTextureStageSlots, kPeTextureStageStateSlots>
        textureStageStates_{};
    FixedStateMatrix<kPeSamplerSlots, kPeSamplerStateSlots> samplerStates_{};
    FixedTransformTable transforms_{};
    friend struct PeHotStateShadow;
};

// Generation-qualified witness for a PendingDelta snapshot.  The generation
// lives in PeHotStateShadow's existing layout reservation so this capability
// adds no hot-state storage.  Consumers must reject stale tickets instead of
// erasing a newer value by key.
struct PendingDeltaTicket {
    std::uint64_t generation = 0u;

    constexpr bool valid() const noexcept { return generation != 0u; }
};

class PendingDelta {
private:
    // Scalar pending categories stay flat and allocation-free. Keyed tables
    // are private so production code cannot bypass their typed category APIs.
    std::uint32_t pendingTextureMask_ = 0;
    std::uint32_t pendingStreamMask_ = 0;
    bool pendingFvf_ = false;
    bool pendingVs_ = false;
    bool pendingPs_ = false;
    bool pendingVdecl_ = false;
    bool pendingIb_ = false;
    std::uint32_t pendingRtMask_ = 0;
    bool pendingDs_ = false;
    bool pendingViewport_ = false;
    bool pendingScissor_ = false;
    bool pendingMaterial_ = false;
    std::uint32_t pendingClipPlaneMask_ = 0;
    std::uint32_t pendingLightSlotMask_ = 0;
    std::uint32_t pendingLightEnableValidMask_ = 0;
    std::uint32_t pendingLightEnableMask_ = 0;

private:
    RenderStateTableView renderStates() noexcept {
        return RenderStateTableView(renderStates_);
    }
    TssTableView textureStageStates() noexcept {
        return TssTableView(textureStageStates_);
    }
    SamplerStateTableView samplerStates() noexcept {
        return SamplerStateTableView(samplerStates_);
    }
    TypedTransformTableView transforms() noexcept {
        return TypedTransformTableView(transforms_);
    }
public:
    ConstRenderStateTableView renderStates() const noexcept {
        return ConstRenderStateTableView(renderStates_);
    }
    ConstTssTableView textureStageStates() const noexcept {
        return ConstTssTableView(textureStageStates_);
    }
    ConstSamplerStateTableView samplerStates() const noexcept {
        return ConstSamplerStateTableView(samplerStates_);
    }
    ConstTypedTransformTableView transforms() const noexcept {
        return ConstTypedTransformTableView(transforms_);
    }

    std::size_t prepareRenderStateBatch(
        std::span<D9CCommandChunkWireRenderState> out) const noexcept {
        std::size_t count = 0u;
        renderStates().forEach([&](RenderStateSlot key, std::uint32_t value) {
            if (count < out.size()) {
                out[count++] = {rawSlot(key), value};
            }
        });
        return count;
    }
private:
    bool acceptRenderStateBatch(
        std::span<const D9CCommandChunkWireRenderState> accepted,
        const dxmt9::d3d9::pe::AppendPlan& plan) noexcept {
        if (!plan.valid() || !plan.consumeRepresentedPending() ||
            !plan.recordDurable()) {
            return false;
        }
        for (const auto& entry : accepted) {
            if (!renderStateSlotKey(entry.state).valid()) return false;
        }
        auto table = renderStates();
        for (const auto& entry : accepted) {
            table.erase(renderStateSlotKey(entry.state));
        }
        return true;
    }
public:
    std::size_t prepareTextureStageStateBatch(
        std::span<D9CDrawPacketTextureStageState> out) const noexcept {
        std::size_t count = 0u;
        textureStageStates().forEach(
            [&](TextureStageIndex stage, TextureStageStateType type,
                std::uint32_t value) {
                if (count < out.size()) {
                    out[count++] = {rawSlot(stage), rawSlot(type), value};
                }
            });
        return count;
    }
private:
    bool acceptTextureStageStateBatch(
        std::span<const D9CDrawPacketTextureStageState> accepted,
        const dxmt9::d3d9::pe::AppendPlan& plan) noexcept {
        if (!plan.valid() || !plan.consumeRepresentedPending() ||
            !plan.recordDurable()) {
            return false;
        }
        for (const auto& entry : accepted) {
            if (!textureStageIndexKey(entry.stage).valid() ||
                !textureStageStateTypeKey(entry.type).valid()) {
                return false;
            }
        }
        auto table = textureStageStates();
        for (const auto& entry : accepted) {
            table.erase(textureStageIndexKey(entry.stage),
                        textureStageStateTypeKey(entry.type));
        }
        return true;
    }
public:
    std::size_t prepareSamplerStateBatch(
        std::span<D9CDrawPacketSamplerState> out) const noexcept {
        std::size_t count = 0u;
        samplerStates().forEach(
            [&](SamplerIndex sampler, SamplerStateType type,
                std::uint32_t value) {
                if (count < out.size()) {
                    out[count++] = {rawSlot(sampler), rawSlot(type), value};
                }
            });
        return count;
    }
private:
    bool acceptSamplerStateBatch(
        std::span<const D9CDrawPacketSamplerState> accepted,
        const dxmt9::d3d9::pe::AppendPlan& plan) noexcept {
        if (!plan.valid() || !plan.consumeRepresentedPending() ||
            !plan.recordDurable()) {
            return false;
        }
        for (const auto& entry : accepted) {
            if (!SamplerIndex::fromRaw(entry.sampler).valid() ||
                !SamplerStateType::fromRaw(entry.type).valid()) {
                return false;
            }
        }
        auto table = samplerStates();
        for (const auto& entry : accepted) {
            table.erase(SamplerIndex::fromRaw(entry.sampler),
                        SamplerStateType::fromRaw(entry.type));
        }
        return true;
    }
public:
    std::size_t prepareTransformBatch(
        std::span<D9CDrawPacketTransform> out) const noexcept {
        std::size_t count = 0u;
        transforms().forEach([&](TransformState state, const D9CMatrix& value) {
            if (count < out.size()) {
                out[count++] = {rawSlot(state), 0u, value};
            }
        });
        return count;
    }
private:
    bool acceptTransformBatch(
        std::span<const D9CDrawPacketTransform> accepted,
        const dxmt9::d3d9::pe::AppendPlan& plan) noexcept {
        if (!plan.valid() || !plan.consumeRepresentedPending() ||
            !plan.recordDurable()) {
            return false;
        }
        for (const auto& entry : accepted) {
            if (!transformStateKey(entry.state).valid()) return false;
        }
        auto table = transforms();
        for (const auto& entry : accepted) {
            table.erase(transformStateKey(entry.state));
        }
        return true;
    }

public:

    bool hasHotState() const noexcept {
        return !renderStates_.empty() || pendingTextureMask_ != 0 ||
               pendingStreamMask_ != 0 || pendingFvf_ ||
               pendingVs_ || pendingPs_ || pendingVdecl_ ||
               pendingIb_ || pendingRtMask_ != 0 || pendingDs_ ||
               pendingViewport_ || pendingScissor_ ||
               !textureStageStates_.empty() || !samplerStates_.empty() ||
               pendingMaterial_ || pendingClipPlaneMask_ != 0 ||
               !transforms_.empty() ||
               pendingLightSlotMask_ != 0 ||
               pendingLightEnableValidMask_ != 0;
    }

private:
    void clearHotState() noexcept {
        renderStates_.clear();
        pendingTextureMask_ = 0;
        pendingStreamMask_ = 0;
        pendingFvf_ = false;
        pendingVs_ = false;
        pendingPs_ = false;
        pendingVdecl_ = false;
        pendingIb_ = false;
        pendingRtMask_ = 0;
        pendingDs_ = false;
        pendingViewport_ = false;
        pendingScissor_ = false;
        textureStageStates_.clear();
        samplerStates_.clear();
        pendingMaterial_ = false;
        pendingClipPlaneMask_ = 0;
        transforms_.clear();
        pendingLightSlotMask_ = 0;
        pendingLightEnableValidMask_ = 0;
        pendingLightEnableMask_ = 0;
    }

private:
    FixedStateTable<kPeRenderStateSlots> renderStates_{};
    FixedStateMatrix<kPeTextureStageSlots, kPeTextureStageStateSlots>
        textureStageStates_{};
    FixedStateMatrix<kPeSamplerSlots, kPeSamplerStateSlots> samplerStates_{};
    FixedTransformTable transforms_{};
    friend struct PeHotStateShadow;
};

class StateBlockRecorded {
public:
    enum class Category : std::uint8_t {
#define DXMT9_STATEBLOCK_CATEGORY_ENUM_KEYED(name)
#define DXMT9_STATEBLOCK_CATEGORY_ENUM_FIXED(name, storage, type, slots, role) name,
#define DXMT9_STATEBLOCK_CATEGORY_ENUM_CONSTANT(name)
        DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY(
            DXMT9_STATEBLOCK_CATEGORY_ENUM_KEYED,
            DXMT9_STATEBLOCK_CATEGORY_ENUM_FIXED,
            DXMT9_STATEBLOCK_CATEGORY_ENUM_CONSTANT)
#undef DXMT9_STATEBLOCK_CATEGORY_ENUM_KEYED
#undef DXMT9_STATEBLOCK_CATEGORY_ENUM_FIXED
#undef DXMT9_STATEBLOCK_CATEGORY_ENUM_CONSTANT
        Count,
    };

    // The remaining categories are intentionally explicit rather than a
    // heterogeneous map.  Their occupancy bits are the tracked-key set;
    // values are written only by recording-phase setters and are never read
    // by ordinary getters or the producer's pending/live paths.
    using TextureState = FixedTrackedState<StateBlockTextureRef, kPeTextureSlots>;
    class Writer {
    public:
        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;
        Writer(Writer&&) = delete;
        Writer& operator=(Writer&&) = delete;

        RenderStateTableView renderStates() noexcept {
            return state_.renderStates();
        }
        TssTableView textureStageStates() noexcept {
            return state_.textureStageStates();
        }
        SamplerStateTableView samplerStates() noexcept {
            return state_.samplerStates();
        }
        TypedTransformTableView transforms() noexcept {
            return state_.transforms();
        }
#define DXMT9_STATEBLOCK_WRITER_ACCESSOR(name, storage, type, slots, role)   \
        auto name() noexcept {                                               \
            using Key = StateBlockFixedSlotKey<                              \
                StateBlockApplyPhysicalStore::name>;                         \
            return FixedTrackedStateWriter<Key, type, slots>(state_.storage); \
        }
#define DXMT9_STATEBLOCK_WRITER_ACCESSOR_KEYED(name)
#define DXMT9_STATEBLOCK_WRITER_ACCESSOR_CONSTANT(name)
        DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY(
            DXMT9_STATEBLOCK_WRITER_ACCESSOR_KEYED,
            DXMT9_STATEBLOCK_WRITER_ACCESSOR,
            DXMT9_STATEBLOCK_WRITER_ACCESSOR_CONSTANT)
#undef DXMT9_STATEBLOCK_WRITER_ACCESSOR_KEYED
#undef DXMT9_STATEBLOCK_WRITER_ACCESSOR_CONSTANT
#undef DXMT9_STATEBLOCK_WRITER_ACCESSOR
        PeStateBlockConstRecorded& constants() noexcept {
            return state_.constants;
        }
        template<typename Release>
        void selectFvf(std::uint32_t value, Release&& release) noexcept {
            auto declarations = vertexDeclaration();
            const auto declarationKey = stateBlockFixedSlotKey<
                StateBlockApplyPhysicalStore::vertexDeclaration>(0u);
            StateBlockVertexDeclarationRef prior{};
            if (declarations.get(declarationKey, prior)) {
                release(prior);
                declarations.erase(declarationKey);
            }
            state_.vertexDeclarationRecorded = false;
            fvf().set(
                stateBlockFixedSlotKey<StateBlockApplyPhysicalStore::fvf>(0u),
                value);
        }
        void selectVertexDeclaration() noexcept {
            fvf().erase(
                stateBlockFixedSlotKey<StateBlockApplyPhysicalStore::fvf>(0u));
            state_.vertexDeclarationRecorded = true;
        }
        void setVertexDeclarationRecorded(bool value) noexcept {
            state_.vertexDeclarationRecorded = value;
        }
        void clear() noexcept { state_.clearForBegin(); }

    private:
        explicit Writer(StateBlockRecorded& state) noexcept : state_(state) {}
        StateBlockRecorded& state_;
        friend class StateBlockRecorded;
    };

    Writer writer() noexcept { return Writer(*this); }
    const StateBlockRecorded& snapshot() const noexcept { return *this; }

#define DXMT9_STATEBLOCK_DECLARE_ACCESSOR(name, storage, type, slots, role) \
    using name##State = FixedTrackedState<type, slots>;                     \
    const name##State& name() const noexcept { return storage; }
#define DXMT9_STATEBLOCK_ACCESSOR_KEYED(name)
#define DXMT9_STATEBLOCK_ACCESSOR_CONSTANT(name)
    DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY(
        DXMT9_STATEBLOCK_ACCESSOR_KEYED,
        DXMT9_STATEBLOCK_DECLARE_ACCESSOR,
        DXMT9_STATEBLOCK_ACCESSOR_CONSTANT)
#undef DXMT9_STATEBLOCK_ACCESSOR_KEYED
#undef DXMT9_STATEBLOCK_ACCESSOR_CONSTANT
#undef DXMT9_STATEBLOCK_DECLARE_ACCESSOR

    bool vertexDeclarationWasRecorded() const noexcept {
        return vertexDeclarationRecorded;
    }
private:
    // Fixed, kind-qualified recorded sets. Explicit recording writes are
    // last-write-wins; MultiplyTransform never enters transforms_.
    bool vertexDeclarationRecorded = false;

    template<typename Fn>
    void forEachCategory(Fn&& fn) noexcept {
#define DXMT9_STATEBLOCK_VISIT_CATEGORY(name, storage, type, slots, role)    \
        fn(Category::name, storage);
#define DXMT9_STATEBLOCK_VISIT_CATEGORY_KEYED(name)
#define DXMT9_STATEBLOCK_VISIT_CATEGORY_CONSTANT(name)
        DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY(
            DXMT9_STATEBLOCK_VISIT_CATEGORY_KEYED,
            DXMT9_STATEBLOCK_VISIT_CATEGORY,
            DXMT9_STATEBLOCK_VISIT_CATEGORY_CONSTANT)
#undef DXMT9_STATEBLOCK_VISIT_CATEGORY_KEYED
#undef DXMT9_STATEBLOCK_VISIT_CATEGORY_CONSTANT
#undef DXMT9_STATEBLOCK_VISIT_CATEGORY
    }

public:
    template<typename Fn>
    void forEachCategory(Fn&& fn) const noexcept {
#define DXMT9_STATEBLOCK_VISIT_CATEGORY_CONST(name, storage, type, slots, role) \
        fn(Category::name, storage);
#define DXMT9_STATEBLOCK_VISIT_CATEGORY_CONST_KEYED(name)
#define DXMT9_STATEBLOCK_VISIT_CATEGORY_CONST_CONSTANT(name)
        DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY(
            DXMT9_STATEBLOCK_VISIT_CATEGORY_CONST_KEYED,
            DXMT9_STATEBLOCK_VISIT_CATEGORY_CONST,
            DXMT9_STATEBLOCK_VISIT_CATEGORY_CONST_CONSTANT)
#undef DXMT9_STATEBLOCK_VISIT_CATEGORY_CONST_KEYED
#undef DXMT9_STATEBLOCK_VISIT_CATEGORY_CONST_CONSTANT
#undef DXMT9_STATEBLOCK_VISIT_CATEGORY_CONST
    }

private:
    template<typename Fn>
    void forEachTypedCategory(Fn&& fn) noexcept {
#define DXMT9_STATEBLOCK_VISIT_TYPED(name, storage, type, slots, role)       \
        fn.template operator()<Category::name>(storage);
#define DXMT9_STATEBLOCK_VISIT_TYPED_KEYED(name)
#define DXMT9_STATEBLOCK_VISIT_TYPED_CONSTANT(name)
        DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY(
            DXMT9_STATEBLOCK_VISIT_TYPED_KEYED,
            DXMT9_STATEBLOCK_VISIT_TYPED,
            DXMT9_STATEBLOCK_VISIT_TYPED_CONSTANT)
#undef DXMT9_STATEBLOCK_VISIT_TYPED_KEYED
#undef DXMT9_STATEBLOCK_VISIT_TYPED_CONSTANT
#undef DXMT9_STATEBLOCK_VISIT_TYPED
    }

public:
    template<typename Fn>
    void forEachTypedCategory(Fn&& fn) const noexcept {
#define DXMT9_STATEBLOCK_VISIT_TYPED_CONST(name, storage, type, slots, role) \
        fn.template operator()<Category::name>(storage);
#define DXMT9_STATEBLOCK_VISIT_TYPED_CONST_KEYED(name)
#define DXMT9_STATEBLOCK_VISIT_TYPED_CONST_CONSTANT(name)
        DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY(
            DXMT9_STATEBLOCK_VISIT_TYPED_CONST_KEYED,
            DXMT9_STATEBLOCK_VISIT_TYPED_CONST,
            DXMT9_STATEBLOCK_VISIT_TYPED_CONST_CONSTANT)
#undef DXMT9_STATEBLOCK_VISIT_TYPED_CONST_KEYED
#undef DXMT9_STATEBLOCK_VISIT_TYPED_CONST_CONSTANT
#undef DXMT9_STATEBLOCK_VISIT_TYPED_CONST
    }

private:
    template<StateBlockApplyPhysicalStore Store>
    decltype(auto) applyPhysicalStorage() noexcept {
        if constexpr (Store == StateBlockApplyPhysicalStore::renderStates) {
            return renderStates();
        } else if constexpr (
            Store == StateBlockApplyPhysicalStore::textureStageStates) {
            return textureStageStates();
        } else if constexpr (
            Store == StateBlockApplyPhysicalStore::samplerStates) {
            return samplerStates();
        } else if constexpr (
            Store == StateBlockApplyPhysicalStore::transforms) {
            return transforms();
#define DXMT9_STATEBLOCK_STORAGE_KEYED(name)
#define DXMT9_STATEBLOCK_STORAGE_FIXED(name, storage, type, slots, role)      \
        } else if constexpr (Store == StateBlockApplyPhysicalStore::name) {  \
            return (storage);
#define DXMT9_STATEBLOCK_STORAGE_CONSTANT(name)                              \
        } else if constexpr (Store == StateBlockApplyPhysicalStore::name) {  \
            return (constants.name);
        DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY(
            DXMT9_STATEBLOCK_STORAGE_KEYED,
            DXMT9_STATEBLOCK_STORAGE_FIXED,
            DXMT9_STATEBLOCK_STORAGE_CONSTANT)
#undef DXMT9_STATEBLOCK_STORAGE_KEYED
#undef DXMT9_STATEBLOCK_STORAGE_FIXED
#undef DXMT9_STATEBLOCK_STORAGE_CONSTANT
        } else {
            []<bool handled = false>() {
                static_assert(handled, "StateBlock physical storage omitted");
            }();
        }
    }

public:
    template<StateBlockApplyPhysicalStore Store>
    decltype(auto) applyPhysicalStorage() const noexcept {
        if constexpr (Store == StateBlockApplyPhysicalStore::renderStates) {
            return renderStates();
        } else if constexpr (
            Store == StateBlockApplyPhysicalStore::textureStageStates) {
            return textureStageStates();
        } else if constexpr (
            Store == StateBlockApplyPhysicalStore::samplerStates) {
            return samplerStates();
        } else if constexpr (
            Store == StateBlockApplyPhysicalStore::transforms) {
            return transforms();
#define DXMT9_STATEBLOCK_CONST_STORAGE_KEYED(name)
#define DXMT9_STATEBLOCK_CONST_STORAGE_FIXED(name, storage, type, slots, role) \
        } else if constexpr (Store == StateBlockApplyPhysicalStore::name) {  \
            return std::as_const(storage);
#define DXMT9_STATEBLOCK_CONST_STORAGE_CONSTANT(name)                       \
        } else if constexpr (Store == StateBlockApplyPhysicalStore::name) { \
            return std::as_const(constants.name);
        DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY(
            DXMT9_STATEBLOCK_CONST_STORAGE_KEYED,
            DXMT9_STATEBLOCK_CONST_STORAGE_FIXED,
            DXMT9_STATEBLOCK_CONST_STORAGE_CONSTANT)
#undef DXMT9_STATEBLOCK_CONST_STORAGE_KEYED
#undef DXMT9_STATEBLOCK_CONST_STORAGE_FIXED
#undef DXMT9_STATEBLOCK_CONST_STORAGE_CONSTANT
        } else {
            []<bool handled = false>() {
                static_assert(handled, "StateBlock physical storage omitted");
            }();
        }
    }

private:
    template<typename Fn>
    void forEachApplyPhysical(Fn&& fn) noexcept {
#define DXMT9_STATEBLOCK_VISIT_PHYSICAL_KEYED(name)                          \
        fn.template operator()<StateBlockApplyPhysicalStore::name>(         \
            applyPhysicalStorage<StateBlockApplyPhysicalStore::name>());
#define DXMT9_STATEBLOCK_VISIT_PHYSICAL_FIXED(name, storage, type, slots, role) \
        DXMT9_STATEBLOCK_VISIT_PHYSICAL_KEYED(name)
#define DXMT9_STATEBLOCK_VISIT_PHYSICAL_CONSTANT(name)                       \
        DXMT9_STATEBLOCK_VISIT_PHYSICAL_KEYED(name)
        DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY(
            DXMT9_STATEBLOCK_VISIT_PHYSICAL_KEYED,
            DXMT9_STATEBLOCK_VISIT_PHYSICAL_FIXED,
            DXMT9_STATEBLOCK_VISIT_PHYSICAL_CONSTANT)
#undef DXMT9_STATEBLOCK_VISIT_PHYSICAL_KEYED
#undef DXMT9_STATEBLOCK_VISIT_PHYSICAL_FIXED
#undef DXMT9_STATEBLOCK_VISIT_PHYSICAL_CONSTANT
    }

public:
    template<typename Fn>
    void forEachApplyPhysical(Fn&& fn) const noexcept {
#define DXMT9_STATEBLOCK_VISIT_CONST_PHYSICAL_KEYED(name)                  \
        fn.template operator()<StateBlockApplyPhysicalStore::name>(        \
            applyPhysicalStorage<StateBlockApplyPhysicalStore::name>());
#define DXMT9_STATEBLOCK_VISIT_CONST_PHYSICAL_FIXED(                       \
    name, storage, type, slots, role)                                      \
        DXMT9_STATEBLOCK_VISIT_CONST_PHYSICAL_KEYED(name)
#define DXMT9_STATEBLOCK_VISIT_CONST_PHYSICAL_CONSTANT(name)               \
        DXMT9_STATEBLOCK_VISIT_CONST_PHYSICAL_KEYED(name)
        DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY(
            DXMT9_STATEBLOCK_VISIT_CONST_PHYSICAL_KEYED,
            DXMT9_STATEBLOCK_VISIT_CONST_PHYSICAL_FIXED,
            DXMT9_STATEBLOCK_VISIT_CONST_PHYSICAL_CONSTANT)
#undef DXMT9_STATEBLOCK_VISIT_CONST_PHYSICAL_KEYED
#undef DXMT9_STATEBLOCK_VISIT_CONST_PHYSICAL_FIXED
#undef DXMT9_STATEBLOCK_VISIT_CONST_PHYSICAL_CONSTANT
    }

    template<typename Fn>
    void forEachOwnedComRef(Fn&& fn) const noexcept {
        forEachApplyPhysical(
            [&]<StateBlockApplyPhysicalStore Store>(const auto& storage) {
                constexpr auto descriptor =
                    stateBlockApplyPhysicalDescriptor<Store>();
                if constexpr (descriptor.kind ==
                              StateBlockApplyPhysicalKind::Fixed) {
                    storage.forEach([&](std::size_t, const auto& value) {
                        visitOwnedValue<descriptor.role>(value, fn);
                    });
                }
            });
    }

    bool has(Category wanted) const noexcept {
        bool present = false;
        forEachCategory([&](Category category, const auto& values) {
            present = present || (category == wanted && !values.empty());
        });
        return present;
    }

private:
    RenderStateTableView renderStates() noexcept {
        return RenderStateTableView(renderStates_);
    }
    TssTableView textureStageStates() noexcept {
        return TssTableView(textureStageStates_);
    }
    SamplerStateTableView samplerStates() noexcept {
        return SamplerStateTableView(samplerStates_);
    }
    TypedTransformTableView transforms() noexcept {
        return TypedTransformTableView(transforms_);
    }
public:
    ConstRenderStateTableView renderStates() const noexcept {
        return ConstRenderStateTableView(renderStates_);
    }
    ConstTssTableView textureStageStates() const noexcept {
        return ConstTssTableView(textureStageStates_);
    }
    ConstSamplerStateTableView samplerStates() const noexcept {
        return ConstSamplerStateTableView(samplerStates_);
    }
    ConstTypedTransformTableView transforms() const noexcept {
        return ConstTypedTransformTableView(transforms_);
    }
    const PeStateBlockConstRecorded& constantSnapshot() const noexcept {
        return constants;
    }

private:
    void clearForBegin() noexcept {
        forEachApplyPhysical(
            []<StateBlockApplyPhysicalStore>(auto&& storage) {
                storage.clear();
            });
        vertexDeclarationRecorded = false;
    }

private:
    template<StateBlockApplyCategoryRole Role, typename T, typename Fn>
    static void visitOwnedValue(const T& value, Fn&& fn) noexcept {
        constexpr bool ownsComRef =
            Role == StateBlockApplyCategoryRole::
                        CandidateOwnedVertexDeclaration ||
            Role >= StateBlockApplyCategoryRole::StagedTexture;
        if constexpr (ownsComRef) {
            static_assert(requires { value.raw(); } ||
                          std::is_same_v<T, StateBlockStreamSourceValue>,
                          "owned StateBlock category needs a typed COM ref");
            if constexpr (requires { value.raw(); }) {
                fn(value);
            } else {
                fn(value.buffer);
            }
        } else {
            static_assert(!requires { value.raw(); } &&
                          !std::is_same_v<T, StateBlockStreamSourceValue>,
                          "COM-ref StateBlock category needs an ownership role");
        }
    }

#define DXMT9_STATEBLOCK_DECLARE_STORAGE(name, storage, type, slots, role)  \
    FixedTrackedState<type, slots> storage{};
#define DXMT9_STATEBLOCK_DECLARE_STORAGE_KEYED(name)
#define DXMT9_STATEBLOCK_DECLARE_STORAGE_CONSTANT(name)
    DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY(
        DXMT9_STATEBLOCK_DECLARE_STORAGE_KEYED,
        DXMT9_STATEBLOCK_DECLARE_STORAGE,
        DXMT9_STATEBLOCK_DECLARE_STORAGE_CONSTANT)
#undef DXMT9_STATEBLOCK_DECLARE_STORAGE_KEYED
#undef DXMT9_STATEBLOCK_DECLARE_STORAGE_CONSTANT
#undef DXMT9_STATEBLOCK_DECLARE_STORAGE
    FixedStateTable<kPeRenderStateSlots> renderStates_{};
    FixedStateMatrix<kPeTextureStageSlots, kPeTextureStageStateSlots>
        textureStageStates_{};
    FixedStateMatrix<kPeSamplerSlots, kPeSamplerStateSlots> samplerStates_{};
    FixedTransformTable transforms_{};
    PeStateBlockConstRecorded constants{};
};

#undef DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY

struct PeHotStateShadow : LiveShadow, PendingDelta {
private:
    class Maintenance {
    public:
        // Scalar domains are only mutable through this capability.  The
        // references remain flat members of the hot shadow; this wrapper is
        // a borrowed, allocation-free phase token for bounded production
        // writers.
        D9CViewport& viewportShadow() noexcept {
            return shadow_.LiveShadow::viewportShadow_;
        }
        D9CRect& scissorShadow() noexcept {
            return shadow_.LiveShadow::scissorShadow_;
        }
        D9CMaterial& materialShadow() noexcept {
            return shadow_.LiveShadow::materialShadow_;
        }
        float* clipPlaneShadow() noexcept {
            return shadow_.LiveShadow::clipPlaneShadow_;
        }
        D9CLight* lightShadow() noexcept {
            return shadow_.LiveShadow::lightShadow_;
        }
        std::uint32_t& lightEnableShadow() noexcept {
            return shadow_.LiveShadow::lightEnableShadow_;
        }
        std::uint32_t& pendingTextureMask() noexcept {
            return shadow_.PendingDelta::pendingTextureMask_;
        }
        std::uint32_t& pendingStreamMask() noexcept {
            return shadow_.PendingDelta::pendingStreamMask_;
        }
        bool& pendingFvf() noexcept { return shadow_.PendingDelta::pendingFvf_; }
        bool& pendingVs() noexcept { return shadow_.PendingDelta::pendingVs_; }
        bool& pendingPs() noexcept { return shadow_.PendingDelta::pendingPs_; }
        bool& pendingVdecl() noexcept {
            return shadow_.PendingDelta::pendingVdecl_;
        }
        bool& pendingIb() noexcept { return shadow_.PendingDelta::pendingIb_; }
        std::uint32_t& pendingRtMask() noexcept {
            return shadow_.PendingDelta::pendingRtMask_;
        }
        bool& pendingDs() noexcept { return shadow_.PendingDelta::pendingDs_; }
        bool& pendingViewport() noexcept {
            return shadow_.PendingDelta::pendingViewport_;
        }
        bool& pendingScissor() noexcept {
            return shadow_.PendingDelta::pendingScissor_;
        }
        bool& pendingMaterial() noexcept {
            return shadow_.PendingDelta::pendingMaterial_;
        }
        std::uint32_t& pendingClipPlaneMask() noexcept {
            return shadow_.PendingDelta::pendingClipPlaneMask_;
        }
        std::uint32_t& pendingLightSlotMask() noexcept {
            return shadow_.PendingDelta::pendingLightSlotMask_;
        }
        std::uint32_t& pendingLightEnableValidMask() noexcept {
            return shadow_.PendingDelta::pendingLightEnableValidMask_;
        }
        std::uint32_t& pendingLightEnableMask() noexcept {
            return shadow_.PendingDelta::pendingLightEnableMask_;
        }
        RenderStateTableView renderStateShadowTyped() noexcept {
            return shadow_.LiveShadow::renderStates();
        }
        RenderStateTableView pendingRenderStatesTyped() noexcept {
            return shadow_.PendingDelta::renderStates();
        }
        TssTableView tssShadowTyped() noexcept {
            return shadow_.LiveShadow::textureStageStates();
        }
        TssTableView pendingTssTyped() noexcept {
            return shadow_.PendingDelta::textureStageStates();
        }
        SamplerStateTableView samplerStateShadowTyped() noexcept {
            return shadow_.LiveShadow::samplerStates();
        }
        SamplerStateTableView pendingSamplerStatesTyped() noexcept {
            return shadow_.PendingDelta::samplerStates();
        }
        TypedTransformTableView transformShadowTyped() noexcept {
            return shadow_.LiveShadow::transforms();
        }
        TypedTransformTableView pendingTransformsTyped() noexcept {
            return shadow_.PendingDelta::transforms();
        }
        void clearServerShadowTables() noexcept {
            shadow_.LiveShadow::clearServerTables();
        }

    private:
        explicit Maintenance(PeHotStateShadow& shadow) noexcept : shadow_(shadow) {
            // Mutable references are an intentionally conservative invalidation
            // point for plans that may have borrowed the pending frontier.
            shadow_.bumpPendingGeneration();
        }
        PeHotStateShadow& shadow_;
        friend struct PeHotStateShadow;
    };

public:
    class Transition {
    public:
        void setRenderState(RenderStateSlot key, std::uint32_t value) noexcept {
            shadow_.LiveShadow::renderStates().set(key, value);
            shadow_.PendingDelta::renderStates().set(key, value);
            shadow_.bumpPendingGeneration();
        }
        void setTextureStageState(TextureStageIndex stage,
                                  TextureStageStateType type,
                                  std::uint32_t value) noexcept {
            shadow_.LiveShadow::textureStageStates().set(stage, type, value);
            shadow_.PendingDelta::textureStageStates().set(stage, type, value);
            shadow_.bumpPendingGeneration();
        }
        void setSamplerState(SamplerIndex sampler, SamplerStateType type,
                             std::uint32_t value) noexcept {
            shadow_.LiveShadow::samplerStates().set(sampler, type, value);
            shadow_.PendingDelta::samplerStates().set(sampler, type, value);
            shadow_.bumpPendingGeneration();
        }
        void setTransform(TransformState key,
                          const D9CMatrix& value) noexcept {
            shadow_.LiveShadow::transforms().set(key, value);
            shadow_.PendingDelta::transforms().set(key, value);
            shadow_.bumpPendingGeneration();
        }
        void setViewport(const D9CViewport& value) noexcept {
            shadow_.LiveShadow::viewportShadow_ = value;
            shadow_.PendingDelta::pendingViewport_ = true;
            shadow_.bumpPendingGeneration();
        }
        void setScissor(const D9CRect& value) noexcept {
            shadow_.LiveShadow::scissorShadow_ = value;
            shadow_.PendingDelta::pendingScissor_ = true;
            shadow_.bumpPendingGeneration();
        }
        void setMaterial(const D9CMaterial& value) noexcept {
            shadow_.LiveShadow::materialShadow_ = value;
            shadow_.PendingDelta::pendingMaterial_ = true;
            shadow_.bumpPendingGeneration();
        }
        void setClipPlane(std::uint32_t slot, const float* value) noexcept {
            if (slot >= 6u || !value) return;
            std::memcpy(shadow_.LiveShadow::clipPlaneShadow_ + slot * 4u,
                        value, sizeof(float) * 4u);
            shadow_.PendingDelta::pendingClipPlaneMask_ |= 1u << slot;
            shadow_.bumpPendingGeneration();
        }
        void setLight(std::uint32_t slot, const D9CLight& value) noexcept {
            if (slot >= D9C_DRAW_PACKET_MAX_LIGHTS) return;
            shadow_.LiveShadow::lightShadow_[slot] = value;
            shadow_.PendingDelta::pendingLightSlotMask_ |= 1u << slot;
            shadow_.bumpPendingGeneration();
        }
        void setLightEnable(std::uint32_t slot, bool enabled) noexcept {
            if (slot >= D9C_DRAW_PACKET_MAX_LIGHTS) return;
            const std::uint32_t bit = 1u << slot;
            shadow_.PendingDelta::pendingLightEnableValidMask_ |= bit;
            if (enabled) {
                shadow_.PendingDelta::pendingLightEnableMask_ |= bit;
                shadow_.LiveShadow::lightEnableShadow_ |= bit;
            } else {
                shadow_.PendingDelta::pendingLightEnableMask_ &= ~bit;
                shadow_.LiveShadow::lightEnableShadow_ &= ~bit;
            }
            shadow_.bumpPendingGeneration();
        }

        template<typename Fn>
            requires std::is_nothrow_invocable_v<Fn&&>
        void bindTexture(std::uint32_t slot, Fn&& bind) noexcept {
            if (slot >= D9C_DRAW_PACKET_MAX_TEXTURES) return;
            std::forward<Fn>(bind)();
            shadow_.PendingDelta::pendingTextureMask_ |= 1u << slot;
            shadow_.bumpPendingGeneration();
        }
        template<typename Fn>
            requires std::is_nothrow_invocable_v<Fn&&>
        void bindStream(std::uint32_t slot, Fn&& bind) noexcept {
            if (slot >= D9C_DRAW_PACKET_MAX_STREAMS) return;
            std::forward<Fn>(bind)();
            shadow_.PendingDelta::pendingStreamMask_ |= 1u << slot;
            shadow_.bumpPendingGeneration();
        }
        template<typename Fn>
            requires std::is_nothrow_invocable_v<Fn&&>
        void bindRenderTarget(std::uint32_t slot, Fn&& bind) noexcept {
            if (slot >= D9C_DRAW_PACKET_MAX_RENDER_TARGETS) return;
            std::forward<Fn>(bind)();
            shadow_.PendingDelta::pendingRtMask_ |= 1u << slot;
            shadow_.bumpPendingGeneration();
        }
        template<typename Fn>
            requires std::is_nothrow_invocable_v<Fn&&>
        void bindDepthStencil(Fn&& bind) noexcept {
            std::forward<Fn>(bind)();
            shadow_.PendingDelta::pendingDs_ = true;
            shadow_.bumpPendingGeneration();
        }
        template<typename Fn>
            requires std::is_nothrow_invocable_v<Fn&&>
        void bindVertexInput(Fn&& bind) noexcept {
            std::forward<Fn>(bind)();
            shadow_.PendingDelta::pendingFvf_ = true;
            shadow_.PendingDelta::pendingVdecl_ = true;
            shadow_.bumpPendingGeneration();
        }
        template<typename Fn>
            requires std::is_nothrow_invocable_v<Fn&&>
        void bindVertexShader(Fn&& bind) noexcept {
            std::forward<Fn>(bind)();
            shadow_.PendingDelta::pendingVs_ = true;
            shadow_.bumpPendingGeneration();
        }
        template<typename Fn>
            requires std::is_nothrow_invocable_v<Fn&&>
        void bindPixelShader(Fn&& bind) noexcept {
            std::forward<Fn>(bind)();
            shadow_.PendingDelta::pendingPs_ = true;
            shadow_.bumpPendingGeneration();
        }
        template<typename Fn>
            requires std::is_nothrow_invocable_v<Fn&&>
        void bindIndexBuffer(Fn&& bind) noexcept {
            std::forward<Fn>(bind)();
            shadow_.PendingDelta::pendingIb_ = true;
            shadow_.bumpPendingGeneration();
        }

    private:
        explicit Transition(PeHotStateShadow& shadow) noexcept : shadow_(shadow) {}
        PeHotStateShadow& shadow_;
        friend struct PeHotStateShadow;
    };

    // Immutable, ticket-scoped view of the PendingDelta frontier.  The view
    // borrows the hot shadow and is therefore only valid while its generation
    // witness still matches.  This keeps the producer from accidentally
    // reading a newer setter frontier after a plan has been prepared.
    class PendingDeltaView {
    public:
        PendingDeltaView() noexcept = default;
        bool valid() const noexcept {
            return shadow_ != nullptr && ticket_.valid() &&
                   shadow_->pendingTicketMatches(ticket_);
        }
        PendingDeltaTicket ticket() const noexcept { return ticket_; }
        std::uint32_t pendingTextureMask() const noexcept {
            return shadow_->PendingDelta::pendingTextureMask_;
        }
        std::uint32_t pendingStreamMask() const noexcept {
            return shadow_->PendingDelta::pendingStreamMask_;
        }
        bool pendingFvf() const noexcept { return shadow_->PendingDelta::pendingFvf_; }
        bool pendingVs() const noexcept { return shadow_->PendingDelta::pendingVs_; }
        bool pendingPs() const noexcept { return shadow_->PendingDelta::pendingPs_; }
        bool pendingVdecl() const noexcept {
            return shadow_->PendingDelta::pendingVdecl_;
        }
        bool pendingIb() const noexcept { return shadow_->PendingDelta::pendingIb_; }
        std::uint32_t pendingRtMask() const noexcept {
            return shadow_->PendingDelta::pendingRtMask_;
        }
        bool pendingDs() const noexcept { return shadow_->PendingDelta::pendingDs_; }
        bool pendingViewport() const noexcept {
            return shadow_->PendingDelta::pendingViewport_;
        }
        bool pendingScissor() const noexcept {
            return shadow_->PendingDelta::pendingScissor_;
        }
        bool pendingMaterial() const noexcept {
            return shadow_->PendingDelta::pendingMaterial_;
        }
        std::uint32_t pendingClipPlaneMask() const noexcept {
            return shadow_->PendingDelta::pendingClipPlaneMask_;
        }
        std::uint32_t pendingLightSlotMask() const noexcept {
            return shadow_->PendingDelta::pendingLightSlotMask_;
        }
        std::uint32_t pendingLightEnableValidMask() const noexcept {
            return shadow_->PendingDelta::pendingLightEnableValidMask_;
        }
        ConstRenderStateTableView pendingRenderStatesTyped() const noexcept {
            return shadow_->PendingDelta::renderStates();
        }
        ConstTssTableView pendingTssTyped() const noexcept {
            return shadow_->PendingDelta::textureStageStates();
        }
        ConstSamplerStateTableView pendingSamplerStatesTyped() const noexcept {
            return shadow_->PendingDelta::samplerStates();
        }
        ConstTypedTransformTableView pendingTransformsTyped() const noexcept {
            return shadow_->PendingDelta::transforms();
        }

    private:
        PendingDeltaView(const PeHotStateShadow& shadow,
                         PendingDeltaTicket ticket) noexcept
            : shadow_(&shadow), ticket_(ticket) {}
        const PeHotStateShadow* shadow_ = nullptr;
        PendingDeltaTicket ticket_{};
        friend struct PeHotStateShadow;
    };

    PendingDeltaView pendingDeltaView(
        PendingDeltaTicket ticket = {}) const noexcept {
        const auto current = pendingTicket();
        return PendingDeltaView(*this, ticket.valid() ? ticket : current);
    }

    class Snapshot {
    public:
        const D9CViewport& viewportShadow() const noexcept {
            return shadow_.LiveShadow::viewportShadow_;
        }
        const D9CRect& scissorShadow() const noexcept {
            return shadow_.LiveShadow::scissorShadow_;
        }
        const D9CMaterial& materialShadow() const noexcept {
            return shadow_.LiveShadow::materialShadow_;
        }
        const float* clipPlaneShadow() const noexcept {
            return shadow_.LiveShadow::clipPlaneShadow_;
        }
        const D9CLight* lightShadow() const noexcept {
            return shadow_.LiveShadow::lightShadow_;
        }
        std::uint32_t lightEnableShadow() const noexcept {
            return shadow_.LiveShadow::lightEnableShadow_;
        }
        std::uint32_t pendingTextureMask() const noexcept {
            return shadow_.PendingDelta::pendingTextureMask_;
        }
        std::uint32_t pendingStreamMask() const noexcept {
            return shadow_.PendingDelta::pendingStreamMask_;
        }
        bool pendingFvf() const noexcept { return shadow_.PendingDelta::pendingFvf_; }
        bool pendingVs() const noexcept { return shadow_.PendingDelta::pendingVs_; }
        bool pendingPs() const noexcept { return shadow_.PendingDelta::pendingPs_; }
        bool pendingVdecl() const noexcept {
            return shadow_.PendingDelta::pendingVdecl_;
        }
        bool pendingIb() const noexcept { return shadow_.PendingDelta::pendingIb_; }
        std::uint32_t pendingRtMask() const noexcept {
            return shadow_.PendingDelta::pendingRtMask_;
        }
        bool pendingDs() const noexcept { return shadow_.PendingDelta::pendingDs_; }
        bool pendingViewport() const noexcept {
            return shadow_.PendingDelta::pendingViewport_;
        }
        bool pendingScissor() const noexcept {
            return shadow_.PendingDelta::pendingScissor_;
        }
        bool pendingMaterial() const noexcept {
            return shadow_.PendingDelta::pendingMaterial_;
        }
        std::uint32_t pendingClipPlaneMask() const noexcept {
            return shadow_.PendingDelta::pendingClipPlaneMask_;
        }
        std::uint32_t pendingLightSlotMask() const noexcept {
            return shadow_.PendingDelta::pendingLightSlotMask_;
        }
        std::uint32_t pendingLightEnableValidMask() const noexcept {
            return shadow_.PendingDelta::pendingLightEnableValidMask_;
        }
        std::uint32_t pendingLightEnableMask() const noexcept {
            return shadow_.PendingDelta::pendingLightEnableMask_;
        }
        ConstRenderStateTableView renderStateShadowTyped() const noexcept {
            return shadow_.LiveShadow::renderStates();
        }
        ConstRenderStateTableView pendingRenderStatesTyped() const noexcept {
            return shadow_.PendingDelta::renderStates();
        }
        ConstTssTableView tssShadowTyped() const noexcept {
            return shadow_.LiveShadow::textureStageStates();
        }
        ConstTssTableView pendingTssTyped() const noexcept {
            return shadow_.PendingDelta::textureStageStates();
        }
        ConstSamplerStateTableView samplerStateShadowTyped() const noexcept {
            return shadow_.LiveShadow::samplerStates();
        }
        ConstSamplerStateTableView pendingSamplerStatesTyped() const noexcept {
            return shadow_.PendingDelta::samplerStates();
        }
        ConstTypedTransformTableView transformShadowTyped() const noexcept {
            return shadow_.LiveShadow::transforms();
        }
        ConstTypedTransformTableView pendingTransformsTyped() const noexcept {
            return shadow_.PendingDelta::transforms();
        }

    private:
        explicit Snapshot(const PeHotStateShadow& shadow) noexcept
            : shadow_(shadow) {}
        const PeHotStateShadow& shadow_;
        friend struct PeHotStateShadow;
    };

    class ConditionalPendingConsumer;

    class Consumer {
    public:
        bool acceptRenderStateBatch(
            std::span<const D9CCommandChunkWireRenderState> accepted,
            const dxmt9::d3d9::pe::AppendPlan& plan) noexcept {
            if (!valid()) return false;
            return shadow_.PendingDelta::acceptRenderStateBatch(accepted, plan);
        }
        bool acceptTextureStageStateBatch(
            std::span<const D9CDrawPacketTextureStageState> accepted,
            const dxmt9::d3d9::pe::AppendPlan& plan) noexcept {
            if (!valid()) return false;
            return shadow_.PendingDelta::acceptTextureStageStateBatch(accepted,
                                                                       plan);
        }
        bool acceptSamplerStateBatch(
            std::span<const D9CDrawPacketSamplerState> accepted,
            const dxmt9::d3d9::pe::AppendPlan& plan) noexcept {
            if (!valid()) return false;
            return shadow_.PendingDelta::acceptSamplerStateBatch(accepted,
                                                                  plan);
        }
        bool acceptTransformBatch(
            std::span<const D9CDrawPacketTransform> accepted,
            const dxmt9::d3d9::pe::AppendPlan& plan) noexcept {
            if (!valid()) return false;
            return shadow_.PendingDelta::acceptTransformBatch(accepted, plan);
        }
        void clearPendingHotState() noexcept {
            shadow_.PendingDelta::clearHotState();
        }
        void acceptAllRenderStates() noexcept {
            shadow_.PendingDelta::renderStates_.clear();
        }
        void acceptAllTextureStageStates() noexcept {
            shadow_.PendingDelta::textureStageStates_.clear();
        }
        void acceptAllSamplerStates() noexcept {
            shadow_.PendingDelta::samplerStates_.clear();
        }
        void acceptAllTransforms() noexcept {
            shadow_.PendingDelta::transforms_.clear();
        }
        void acceptTexture(std::uint32_t slot) noexcept {
            if (slot < D9C_DRAW_PACKET_MAX_TEXTURES)
                shadow_.PendingDelta::pendingTextureMask_ &= ~(1u << slot);
        }
        void acceptStream(std::uint32_t slot) noexcept {
            if (slot < D9C_DRAW_PACKET_MAX_STREAMS)
                shadow_.PendingDelta::pendingStreamMask_ &= ~(1u << slot);
        }
        void acceptVertexShader() noexcept {
            shadow_.PendingDelta::pendingVs_ = false;
        }
        void acceptPixelShader() noexcept {
            shadow_.PendingDelta::pendingPs_ = false;
        }
        void acceptVertexDeclaration() noexcept {
            shadow_.PendingDelta::pendingVdecl_ = false;
            shadow_.PendingDelta::pendingFvf_ = false;
        }
        void acceptFvf() noexcept {
            shadow_.PendingDelta::pendingFvf_ = false;
        }
        void acceptIndexBuffer() noexcept {
            shadow_.PendingDelta::pendingIb_ = false;
        }
        void acceptRenderTarget(std::uint32_t slot) noexcept {
            if (slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS)
                shadow_.PendingDelta::pendingRtMask_ &= ~(1u << slot);
        }
        void acceptDepthStencil() noexcept {
            shadow_.PendingDelta::pendingDs_ = false;
        }
        void acceptViewport() noexcept {
            shadow_.PendingDelta::pendingViewport_ = false;
        }
        void acceptScissor() noexcept {
            shadow_.PendingDelta::pendingScissor_ = false;
        }
        void acceptMaterial() noexcept {
            shadow_.PendingDelta::pendingMaterial_ = false;
        }
        void acceptClipPlane(std::uint32_t slot) noexcept {
            if (slot < 6u)
                shadow_.PendingDelta::pendingClipPlaneMask_ &= ~(1u << slot);
        }
        void acceptLight(std::uint32_t slot) noexcept {
            if (slot < D9C_DRAW_PACKET_MAX_LIGHTS)
                shadow_.PendingDelta::pendingLightSlotMask_ &= ~(1u << slot);
        }
        void acceptLightEnable(std::uint32_t slot) noexcept {
            if (slot >= D9C_DRAW_PACKET_MAX_LIGHTS) return;
            const std::uint32_t bit = 1u << slot;
            shadow_.PendingDelta::pendingLightEnableValidMask_ &= ~bit;
            shadow_.PendingDelta::pendingLightEnableMask_ &= ~bit;
        }

        bool valid() const noexcept {
            return true;
        }

    private:
        explicit Consumer(PeHotStateShadow& shadow) noexcept : shadow_(shadow) {}
        PeHotStateShadow& shadow_;
        friend class ConditionalPendingConsumer;
        friend struct PeHotStateShadow;
    };

    // Conditional settlement carries the generation witness separately from
    // the ordinary one-reference consumer.  The hot `Consumer` remains a
    // pointer-sized capability; this cold/conditional form intentionally costs
    // one additional word only at callers that need stale-ticket rejection.
    class ConditionalPendingConsumer {
    public:
        bool valid() const noexcept {
            return ticket_.valid() &&
                   consumer_.shadow_.pendingTicketMatches(ticket_);
        }
        bool acceptRenderStateBatch(
            std::span<const D9CCommandChunkWireRenderState> accepted,
            const dxmt9::d3d9::pe::AppendPlan& plan) noexcept {
            return valid() && consumer_.acceptRenderStateBatch(accepted, plan);
        }
        bool acceptTextureStageStateBatch(
            std::span<const D9CDrawPacketTextureStageState> accepted,
            const dxmt9::d3d9::pe::AppendPlan& plan) noexcept {
            return valid() &&
                   consumer_.acceptTextureStageStateBatch(accepted, plan);
        }
        bool acceptSamplerStateBatch(
            std::span<const D9CDrawPacketSamplerState> accepted,
            const dxmt9::d3d9::pe::AppendPlan& plan) noexcept {
            return valid() && consumer_.acceptSamplerStateBatch(accepted, plan);
        }
        bool acceptTransformBatch(
            std::span<const D9CDrawPacketTransform> accepted,
            const dxmt9::d3d9::pe::AppendPlan& plan) noexcept {
            return valid() && consumer_.acceptTransformBatch(accepted, plan);
        }
        void clearPendingHotState() noexcept {
            if (valid()) consumer_.clearPendingHotState();
        }
        void acceptAllRenderStates() noexcept {
            if (valid()) consumer_.acceptAllRenderStates();
        }
        void acceptAllTextureStageStates() noexcept {
            if (valid()) consumer_.acceptAllTextureStageStates();
        }
        void acceptAllSamplerStates() noexcept {
            if (valid()) consumer_.acceptAllSamplerStates();
        }
        void acceptAllTransforms() noexcept {
            if (valid()) consumer_.acceptAllTransforms();
        }
        void acceptTexture(std::uint32_t slot) noexcept {
            if (valid()) consumer_.acceptTexture(slot);
        }
        void acceptStream(std::uint32_t slot) noexcept {
            if (valid()) consumer_.acceptStream(slot);
        }
        void acceptVertexShader() noexcept {
            if (valid()) consumer_.acceptVertexShader();
        }
        void acceptPixelShader() noexcept {
            if (valid()) consumer_.acceptPixelShader();
        }
        void acceptVertexDeclaration() noexcept {
            if (valid()) consumer_.acceptVertexDeclaration();
        }
        void acceptFvf() noexcept {
            if (valid()) consumer_.acceptFvf();
        }
        void acceptIndexBuffer() noexcept {
            if (valid()) consumer_.acceptIndexBuffer();
        }
        void acceptRenderTarget(std::uint32_t slot) noexcept {
            if (valid()) consumer_.acceptRenderTarget(slot);
        }
        void acceptDepthStencil() noexcept {
            if (valid()) consumer_.acceptDepthStencil();
        }
        void acceptViewport() noexcept {
            if (valid()) consumer_.acceptViewport();
        }
        void acceptScissor() noexcept {
            if (valid()) consumer_.acceptScissor();
        }
        void acceptMaterial() noexcept {
            if (valid()) consumer_.acceptMaterial();
        }
        void acceptClipPlane(std::uint32_t slot) noexcept {
            if (valid()) consumer_.acceptClipPlane(slot);
        }
        void acceptLight(std::uint32_t slot) noexcept {
            if (valid()) consumer_.acceptLight(slot);
        }
        void acceptLightEnable(std::uint32_t slot) noexcept {
            if (valid()) consumer_.acceptLightEnable(slot);
        }

    private:
        explicit ConditionalPendingConsumer(
            PeHotStateShadow& shadow, PendingDeltaTicket ticket) noexcept
            : consumer_(shadow), ticket_(ticket) {}
        Consumer consumer_;
        PendingDeltaTicket ticket_{};
        friend struct PeHotStateShadow;
    };

    Transition transition() noexcept { return Transition(*this); }
    Snapshot snapshot() const noexcept { return Snapshot(*this); }
    Consumer consume() noexcept { return Consumer(*this); }
    ConditionalPendingConsumer consume(PendingDeltaTicket ticket) noexcept {
        return ConditionalPendingConsumer(*this, ticket);
    }

    PendingDeltaTicket pendingTicket() const noexcept {
        return PendingDeltaTicket{pendingGeneration_};
    }
    bool pendingTicketMatches(PendingDeltaTicket ticket) const noexcept {
        return ticket.valid() && ticket.generation == pendingGeneration_;
    }

#if defined(DXMT9_PE_SHADOW_TEST_SEAM)
    using Writer = Maintenance;
    Writer writer() noexcept { return Writer(*this); }
#endif

    ConstRenderStateTableView renderStateShadowTyped() const noexcept {
        return snapshot().renderStateShadowTyped();
    }
    const D9CViewport& viewportShadow() const noexcept {
        return LiveShadow::viewportShadow_;
    }
    const D9CRect& scissorShadow() const noexcept {
        return LiveShadow::scissorShadow_;
    }
    const D9CMaterial& materialShadow() const noexcept {
        return LiveShadow::materialShadow_;
    }
    const float* clipPlaneShadow() const noexcept {
        return LiveShadow::clipPlaneShadow_;
    }
    const D9CLight* lightShadow() const noexcept {
        return LiveShadow::lightShadow_;
    }
    std::uint32_t lightEnableShadow() const noexcept {
        return LiveShadow::lightEnableShadow_;
    }
    std::uint32_t pendingTextureMask() const noexcept {
        return PendingDelta::pendingTextureMask_;
    }
    std::uint32_t pendingStreamMask() const noexcept {
        return PendingDelta::pendingStreamMask_;
    }
    bool pendingFvf() const noexcept { return PendingDelta::pendingFvf_; }
    bool pendingVs() const noexcept { return PendingDelta::pendingVs_; }
    bool pendingPs() const noexcept { return PendingDelta::pendingPs_; }
    bool pendingVdecl() const noexcept { return PendingDelta::pendingVdecl_; }
    bool pendingIb() const noexcept { return PendingDelta::pendingIb_; }
    std::uint32_t pendingRtMask() const noexcept { return PendingDelta::pendingRtMask_; }
    bool pendingDs() const noexcept { return PendingDelta::pendingDs_; }
    bool pendingViewport() const noexcept { return PendingDelta::pendingViewport_; }
    bool pendingScissor() const noexcept { return PendingDelta::pendingScissor_; }
    bool pendingMaterial() const noexcept { return PendingDelta::pendingMaterial_; }
    std::uint32_t pendingClipPlaneMask() const noexcept {
        return PendingDelta::pendingClipPlaneMask_;
    }
    std::uint32_t pendingLightSlotMask() const noexcept {
        return PendingDelta::pendingLightSlotMask_;
    }
    std::uint32_t pendingLightEnableValidMask() const noexcept {
        return PendingDelta::pendingLightEnableValidMask_;
    }
    std::uint32_t pendingLightEnableMask() const noexcept {
        return PendingDelta::pendingLightEnableMask_;
    }
    ConstRenderStateTableView pendingRenderStatesTyped() const noexcept {
        return snapshot().pendingRenderStatesTyped();
    }
    ConstTssTableView tssShadowTyped() const noexcept {
        return snapshot().tssShadowTyped();
    }
    ConstTssTableView pendingTssTyped() const noexcept {
        return snapshot().pendingTssTyped();
    }
    ConstSamplerStateTableView samplerStateShadowTyped() const noexcept {
        return snapshot().samplerStateShadowTyped();
    }
    ConstSamplerStateTableView pendingSamplerStatesTyped() const noexcept {
        return snapshot().pendingSamplerStatesTyped();
    }
    ConstTypedTransformTableView transformShadowTyped() const noexcept {
        return snapshot().transformShadowTyped();
    }
    ConstTypedTransformTableView pendingTransformsTyped() const noexcept {
        return snapshot().pendingTransformsTyped();
    }

    bool hasPendingHotState() const noexcept {
        return PendingDelta::hasHotState();
    }

    bool renderStateEqualsTyped(RenderStateSlot state, std::uint32_t value) const noexcept {
        std::uint32_t shadowValue = 0;
        return snapshot().renderStateShadowTyped().get(state, shadowValue) &&
               shadowValue == value;
    }

private:
    Maintenance maintenance() noexcept { return Maintenance(*this); }
    friend class D3D9DeviceImpl;

    // Keep the two hot domains naturally aligned without folding the cold
    // StateBlockRecorded owner back into this object.
    void bumpPendingGeneration() noexcept {
        ++pendingGeneration_;
        if (pendingGeneration_ == 0u) pendingGeneration_ = 1u;
    }
    std::uint64_t pendingGeneration_ = 1u;
};

static_assert(sizeof(LiveShadow) == 22968u);
static_assert(sizeof(PendingDelta) == 21968u);
static_assert(sizeof(PeHotStateShadow) == 44944u);
static_assert(alignof(LiveShadow) == alignof(std::uint64_t));
static_assert(alignof(PendingDelta) == alignof(std::uint64_t));
static_assert(alignof(StateBlockRecorded) == alignof(std::uint64_t));
static_assert(alignof(PeHotStateShadow) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<LiveShadow>);
static_assert(std::is_trivially_copyable_v<PendingDelta>);
static_assert(std::is_trivially_copyable_v<StateBlockRecorded>);
static_assert(std::is_trivially_copyable_v<PeHotStateShadow>);
