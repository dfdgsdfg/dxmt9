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
#include <vector>

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

struct StateBlockStreamSourceValue {
    struct BufferRef {
        void* value = nullptr;
        constexpr BufferRef() noexcept = default;
        constexpr BufferRef(void* rawValue) noexcept : value(rawValue) {}
        constexpr void* raw() const noexcept { return value; }
        constexpr void*& rawRef() noexcept { return value; }
        constexpr operator void*() const noexcept { return value; }
        friend constexpr bool operator==(BufferRef a, void* b) noexcept {
            return a.value == b;
        }
    };
    BufferRef buffer{};
    std::uint32_t offset = 0u;
    std::uint32_t stride = 0u;
};

template<typename Tag>
struct StateBlockComRef {
    void* value = nullptr;
    constexpr StateBlockComRef() noexcept = default;
    template<typename P>
    constexpr StateBlockComRef(P* rawValue) noexcept
        : value(static_cast<void*>(rawValue)) {}
    static constexpr StateBlockComRef fromRaw(void* raw) noexcept {
        return StateBlockComRef{raw};
    }
    constexpr void* raw() const noexcept { return value; }
    constexpr operator void*() const noexcept { return value; }
    friend constexpr bool operator==(StateBlockComRef a,
                                     StateBlockComRef b) noexcept {
        return a.value == b.value;
    }
    friend constexpr bool operator==(StateBlockComRef a, void* b) noexcept {
        return a.value == b;
    }
};

struct StateBlockTextureTag;
struct StateBlockVertexShaderTag;
struct StateBlockPixelShaderTag;
struct StateBlockVertexDeclarationTag;
struct StateBlockIndexBufferTag;
struct StateBlockRenderTargetTag;
struct StateBlockDepthStencilTag;
using StateBlockTextureRef = StateBlockComRef<StateBlockTextureTag>;
using StateBlockVertexShaderRef = StateBlockComRef<StateBlockVertexShaderTag>;
using StateBlockPixelShaderRef = StateBlockComRef<StateBlockPixelShaderTag>;
using StateBlockVertexDeclarationRef =
    StateBlockComRef<StateBlockVertexDeclarationTag>;
using StateBlockIndexBufferRef = StateBlockComRef<StateBlockIndexBufferTag>;
using StateBlockRenderTargetRef = StateBlockComRef<StateBlockRenderTargetTag>;
using StateBlockDepthStencilRef = StateBlockComRef<StateBlockDepthStencilTag>;

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
// Key design: each space is a distinct `enum class Tag : std::uint32_t {}`
// with no enumerators ("opaque newtype" pattern) rather than a
// std::variant<...> over the six spaces. A variant would need a runtime
// discriminant, pay a branch/visit to get the value back out, and -- worse
// for this use -- would let one variable hold "a slot from some space",
// which is exactly the untyped hazard being removed. An `enum class` with a
// fixed uint32_t underlying type has the same object representation and
// value semantics as a plain uint32_t (guaranteed by the standard: no
// hidden state, trivially copyable, standard-layout), so every conversion
// to/from the underlying value below is a single `static_cast` that
// optimizes to a no-op, and the typed accessors below compile down to the
// exact same array indexing FixedStateTable/FixedStateMatrix/
// FixedTransformTable already did -- zero-overhead in the same sense the
// hot DOD tables above already are. What the enum class actually buys is
// static overload resolution: RenderStateSlot and SamplerIndex are
// unrelated types with no implicit conversion between them (and none to
// plain uint32_t either), so a call site that mixes them fails to compile
// instead of silently indexing the wrong table at runtime.
enum class RenderStateSlot : std::uint32_t {};
enum class TextureStageIndex : std::uint32_t {};
enum class TextureStageStateType : std::uint32_t {};
enum class SamplerIndex : std::uint32_t {};
enum class SamplerStateType : std::uint32_t {};
enum class TransformState : std::uint32_t {};

constexpr std::uint32_t rawSlot(RenderStateSlot key) noexcept {
    return static_cast<std::uint32_t>(key);
}
constexpr std::uint32_t rawSlot(TextureStageIndex key) noexcept {
    return static_cast<std::uint32_t>(key);
}
constexpr std::uint32_t rawSlot(TextureStageStateType key) noexcept {
    return static_cast<std::uint32_t>(key);
}
constexpr std::uint32_t rawSlot(SamplerIndex key) noexcept {
    return static_cast<std::uint32_t>(key);
}
constexpr std::uint32_t rawSlot(SamplerStateType key) noexcept {
    return static_cast<std::uint32_t>(key);
}
constexpr std::uint32_t rawSlot(TransformState key) noexcept {
    return static_cast<std::uint32_t>(key);
}

// Typed constructors. These wrap the existing untyped clamp/lookup
// functions above (textureStageSlot, textureStageStateSlot, samplerSlot,
// samplerStateSlot) rather than duplicating their logic, so the typed and
// untyped surfaces can never disagree about what a given external D3D9
// ordinal maps to.
constexpr RenderStateSlot renderStateSlotKey(std::uint32_t state) noexcept {
    return static_cast<RenderStateSlot>(state);
}
constexpr TextureStageIndex textureStageIndexKey(std::uint32_t stage) noexcept {
    return static_cast<TextureStageIndex>(textureStageSlot(stage));
}
constexpr TextureStageStateType textureStageStateTypeKey(std::uint32_t type) noexcept {
    return static_cast<TextureStageStateType>(textureStageStateSlot(type));
}
inline bool samplerIndexKey(std::uint32_t sampler, SamplerIndex& out) noexcept {
    std::uint32_t slot = 0;
    if (!samplerSlot(sampler, slot)) {
        return false;
    }
    out = static_cast<SamplerIndex>(slot);
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
    out = static_cast<SamplerStateType>(slot);
    return true;
}
constexpr TransformState transformStateKey(std::uint32_t state) noexcept {
    return static_cast<TransformState>(state);
}

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
            fn(static_cast<Key>(slot), value);
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
            fn(static_cast<RowKey>(row), static_cast<ColKey>(col), value);
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
            fn(static_cast<TransformState>(state), value);
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
            fn(static_cast<Key>(slot), value);
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
            fn(static_cast<RowKey>(row), static_cast<ColKey>(col), value);
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
            fn(static_cast<TransformState>(state), value);
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

class LiveShadow {
public:
    D9CViewport viewportShadow{};
    D9CRect scissorShadow{};
    D9CMaterial materialShadow{};
    float clipPlaneShadow[6 * 4]{};
    D9CLight lightShadow[D9C_DRAW_PACKET_MAX_LIGHTS]{};
    std::uint32_t lightEnableShadow = 0;

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
};

class PendingDelta {
public:
    // Scalar pending categories stay flat and allocation-free. Keyed tables
    // are private so production code cannot bypass their typed category APIs.
    std::uint32_t pendingTextureMask = 0;
    std::uint32_t pendingStreamMask = 0;
    bool pendingFvf = false;
    bool pendingVs = false;
    bool pendingPs = false;
    bool pendingVdecl = false;
    bool pendingIb = false;
    std::uint32_t pendingRtMask = 0;
    bool pendingDs = false;
    bool pendingViewport = false;
    bool pendingScissor = false;
    bool pendingMaterial = false;
    std::uint32_t pendingClipPlaneMask = 0;
    std::uint32_t pendingLightSlotMask = 0;
    std::uint32_t pendingLightEnableValidMask = 0;
    std::uint32_t pendingLightEnableMask = 0;

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
    bool acceptRenderStateBatch(
        std::span<const D9CCommandChunkWireRenderState> accepted,
        const dxmt9::d3d9::pe::AppendPlan& plan) noexcept {
        if (!plan.valid() || !plan.consumeRepresentedPending() ||
            !plan.recordDurable()) {
            return false;
        }
        auto table = renderStates();
        for (const auto& entry : accepted) {
            table.erase(renderStateSlotKey(entry.state));
        }
        return true;
    }
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
    bool acceptTextureStageStateBatch(
        std::span<const D9CDrawPacketTextureStageState> accepted,
        const dxmt9::d3d9::pe::AppendPlan& plan) noexcept {
        if (!plan.valid() || !plan.consumeRepresentedPending() ||
            !plan.recordDurable()) {
            return false;
        }
        auto table = textureStageStates();
        for (const auto& entry : accepted) {
            table.erase(textureStageIndexKey(entry.stage),
                        textureStageStateTypeKey(entry.type));
        }
        return true;
    }
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
    bool acceptSamplerStateBatch(
        std::span<const D9CDrawPacketSamplerState> accepted,
        const dxmt9::d3d9::pe::AppendPlan& plan) noexcept {
        if (!plan.valid() || !plan.consumeRepresentedPending() ||
            !plan.recordDurable()) {
            return false;
        }
        auto table = samplerStates();
        for (const auto& entry : accepted) {
            SamplerStateType type{};
            if (entry.sampler < kPeSamplerSlots &&
                samplerStateTypeKey(entry.type, type)) {
                table.erase(static_cast<SamplerIndex>(entry.sampler), type);
            }
        }
        return true;
    }
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
    bool acceptTransformBatch(
        std::span<const D9CDrawPacketTransform> accepted,
        const dxmt9::d3d9::pe::AppendPlan& plan) noexcept {
        if (!plan.valid() || !plan.consumeRepresentedPending() ||
            !plan.recordDurable()) {
            return false;
        }
        auto table = transforms();
        for (const auto& entry : accepted) {
            table.erase(transformStateKey(entry.state));
        }
        return true;
    }

    bool hasHotState() const noexcept {
        return !renderStates_.empty() || pendingTextureMask != 0 ||
               pendingStreamMask != 0 || pendingFvf ||
               pendingVs || pendingPs || pendingVdecl ||
               pendingIb || pendingRtMask != 0 || pendingDs ||
               pendingViewport || pendingScissor ||
               !textureStageStates_.empty() || !samplerStates_.empty() ||
               pendingMaterial || pendingClipPlaneMask != 0 ||
               !transforms_.empty() ||
               pendingLightSlotMask != 0 ||
               pendingLightEnableValidMask != 0;
    }

    void clearHotState() noexcept {
        renderStates_.clear();
        pendingTextureMask = 0;
        pendingStreamMask = 0;
        pendingFvf = false;
        pendingVs = false;
        pendingPs = false;
        pendingVdecl = false;
        pendingIb = false;
        pendingRtMask = 0;
        pendingDs = false;
        pendingViewport = false;
        pendingScissor = false;
        textureStageStates_.clear();
        samplerStates_.clear();
        pendingMaterial = false;
        pendingClipPlaneMask = 0;
        transforms_.clear();
        pendingLightSlotMask = 0;
        pendingLightEnableValidMask = 0;
        pendingLightEnableMask = 0;
    }

private:
    FixedStateTable<kPeRenderStateSlots> renderStates_{};
    FixedStateMatrix<kPeTextureStageSlots, kPeTextureStageStateSlots>
        textureStageStates_{};
    FixedStateMatrix<kPeSamplerSlots, kPeSamplerStateSlots> samplerStates_{};
    FixedTransformTable transforms_{};
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

    // Fixed, kind-qualified recorded sets. Explicit recording writes are
    // last-write-wins; MultiplyTransform never enters transforms_.
    bool vertexDeclarationRecorded = false;

    // The remaining categories are intentionally explicit rather than a
    // heterogeneous map.  Their occupancy bits are the tracked-key set;
    // values are written only by recording-phase setters and are never read
    // by ordinary getters or the producer's pending/live paths.
    using TextureState = FixedTrackedState<StateBlockTextureRef, kPeTextureSlots>;
#define DXMT9_STATEBLOCK_DECLARE_ACCESSOR(name, storage, type, slots, role) \
    using name##State = FixedTrackedState<type, slots>;                     \
    name##State& name() noexcept { return storage; }                        \
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
    void setVertexDeclarationRecorded(bool value) noexcept {
        vertexDeclarationRecorded = value;
    }

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

    template<StateBlockApplyPhysicalStore Store>
    decltype(auto) applyPhysicalStorage() const noexcept {
        return const_cast<StateBlockRecorded*>(this)
            ->template applyPhysicalStorage<Store>();
    }

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

    template<typename Fn>
    void forEachApplyPhysical(Fn&& fn) const noexcept {
        const_cast<StateBlockRecorded*>(this)->forEachApplyPhysical(
            [&]<StateBlockApplyPhysicalStore Store>(auto&& storage) {
                fn.template operator()<Store>(std::as_const(storage));
            });
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

    PeStateBlockConstRecorded constants{};

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
                fn(value.raw());
            } else {
                fn(value.buffer.raw());
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
};

#undef DXMT9_STATEBLOCK_APPLY_PHYSICAL_INVENTORY

struct PeHotStateShadow : LiveShadow, PendingDelta {
    bool hasPendingHotState() const noexcept {
        return PendingDelta::hasHotState();
    }
    void clearPendingHotState() noexcept {
        PendingDelta::clearHotState();
    }
    void clearServerShadowTables() noexcept {
        LiveShadow::clearServerTables();
    }

    // Compatibility names for existing callers. Each exposes only the typed
    // category view owned by LiveShadow or PendingDelta; the raw keyed tables
    // are private to those owners. StateBlockRecorded is intentionally a
    // separate PeRecorderState member and has no compatibility doorway here.
    RenderStateTableView renderStateShadowTyped() noexcept {
        return LiveShadow::renderStates();
    }
    RenderStateTableView pendingRenderStatesTyped() noexcept {
        return PendingDelta::renderStates();
    }
    TssTableView tssShadowTyped() noexcept { return LiveShadow::textureStageStates(); }
    TssTableView pendingTssTyped() noexcept { return PendingDelta::textureStageStates(); }
    SamplerStateTableView samplerStateShadowTyped() noexcept {
        return LiveShadow::samplerStates();
    }
    SamplerStateTableView pendingSamplerStatesTyped() noexcept {
        return PendingDelta::samplerStates();
    }
    TypedTransformTableView transformShadowTyped() noexcept {
        return LiveShadow::transforms();
    }
    TypedTransformTableView pendingTransformsTyped() noexcept {
        return PendingDelta::transforms();
    }

    // const overloads (cv-qualification-based overloading, not name
    // collision) for read paths that only see a `const PeHotStateShadow&`.
    ConstRenderStateTableView renderStateShadowTyped() const noexcept {
        return LiveShadow::renderStates();
    }
    ConstRenderStateTableView pendingRenderStatesTyped() const noexcept {
        return PendingDelta::renderStates();
    }
    ConstTssTableView tssShadowTyped() const noexcept { return LiveShadow::textureStageStates(); }
    ConstTssTableView pendingTssTyped() const noexcept { return PendingDelta::textureStageStates(); }
    ConstSamplerStateTableView samplerStateShadowTyped() const noexcept {
        return LiveShadow::samplerStates();
    }
    ConstSamplerStateTableView pendingSamplerStatesTyped() const noexcept {
        return PendingDelta::samplerStates();
    }
    ConstTypedTransformTableView transformShadowTyped() const noexcept {
        return LiveShadow::transforms();
    }
    ConstTypedTransformTableView pendingTransformsTyped() const noexcept {
        return PendingDelta::transforms();
    }

    bool renderStateEqualsTyped(RenderStateSlot state, std::uint32_t value) const noexcept {
        std::uint32_t shadowValue = 0;
        return renderStateShadowTyped().get(state, shadowValue) && shadowValue == value;
    }

private:
    // Keep the two hot domains naturally aligned without folding the cold
    // StateBlockRecorded owner back into this object.
    [[maybe_unused]] std::uint64_t reservedLayout_ = 0u;
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
