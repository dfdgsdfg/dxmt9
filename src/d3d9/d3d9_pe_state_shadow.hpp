#pragma once

#include "d3d9_pe_const_shadow.hpp"
#include "dxmt9/device_c.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
struct FixedStateTable {
    std::array<std::uint32_t, Slots> values{};
    std::array<std::uint64_t, (Slots + 63u) / 64u> occupied{};
    std::uint32_t count = 0;

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

inline std::uint32_t textureStageSlot(std::uint32_t stage) noexcept {
    return std::min<std::uint32_t>(stage, kPeTextureStageSlots - 1u);
}

inline bool vertexTextureSamplerSlot(std::uint32_t sampler, std::uint32_t& slot) noexcept {
    if (sampler < kD3dVertexTextureSampler0 || sampler > kD3dVertexTextureSampler3) {
        return false;
    }
    slot = kPeFragmentSamplerSlots +
        static_cast<std::uint32_t>(sampler - kD3dVertexTextureSampler0);
    return true;
}

inline bool textureBindingSlot(std::uint32_t stage, std::uint32_t& slot) noexcept {
    if (stage < kPeFragmentSamplerSlots) {
        slot = static_cast<std::uint32_t>(stage);
        return true;
    }
    return vertexTextureSamplerSlot(stage, slot);
}

inline std::uint32_t textureStageStateSlot(
    std::uint32_t type) noexcept {
    return std::min<std::uint32_t>(
        static_cast<std::uint32_t>(type), kPeTextureStageStateSlots - 1u);
}

inline bool samplerSlot(std::uint32_t sampler, std::uint32_t& slot) noexcept {
    if (sampler < kPeFragmentSamplerSlots) {
        slot = static_cast<std::uint32_t>(sampler);
        return true;
    }
    return vertexTextureSamplerSlot(sampler, slot);
}

inline bool samplerStateSlot(std::uint32_t type,
                             std::uint32_t& slot) noexcept {
    slot = static_cast<std::uint32_t>(type);
    return slot < kPeSamplerStateSlots;
}

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

struct PeHotStateShadow {
    FixedStateTable<kPeRenderStateSlots> renderStateShadow{};
    FixedStateTable<kPeRenderStateSlots> pendingRenderStates{};
    FixedStateTable<kPeRenderStateSlots> stateBlockRenderStateRestore{};
    FixedTransformTable stateBlockTransformRestore{};
    // PE-shadow stateblock support. Captures the NEW transform value that
    // SetTransform writes during BeginStateBlock/EndStateBlock recording.
    // EndStateBlock hands this to the newly-created D3D9StateBlockImpl
    // before the *Restore loop reverts the device shadow to pre-Begin
    // values; on Apply the stateblock replays these values via the existing
    // IDirect3DDevice9::SetTransform path. MultiplyTransform intentionally
    // bypasses this table to match wined3d's "MultiplyTransform during
    // recording is not captured" quirk (see Wine d3d9 tests).
    FixedTransformTable stateBlockTransformRecorded{};
    // True if SetVertexDeclaration was called between Begin/End. Same role
    // as stateBlockTransformRecorded but for the singleton vdecl slot.
    bool stateBlockVdeclRecorded = false;
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
    D9CViewport viewportShadow{};
    D9CRect scissorShadow{};
    FixedStateMatrix<kPeTextureStageSlots, kPeTextureStageStateSlots>
        pendingTss{};
    FixedStateMatrix<kPeSamplerSlots, kPeSamplerStateSlots>
        pendingSamplerStates{};
    FixedStateMatrix<kPeTextureStageSlots, kPeTextureStageStateSlots>
        tssShadow{};
    FixedStateMatrix<kPeSamplerSlots, kPeSamplerStateSlots>
        samplerStateShadow{};
    bool pendingMaterial = false;
    D9CMaterial materialShadow{};
    std::uint32_t pendingClipPlaneMask = 0;
    float clipPlaneShadow[6 * 4]{};
    FixedTransformTable pendingTransforms{};
    FixedTransformTable transformShadow{};
    std::uint32_t pendingLightSlotMask = 0;
    D9CLight lightShadow[D9C_DRAW_PACKET_MAX_LIGHTS]{};
    std::uint32_t pendingLightEnableValidMask = 0;
    std::uint32_t pendingLightEnableMask = 0;
    std::uint32_t lightEnableShadow = 0;

    bool hasPendingHotState() const noexcept {
        return !pendingRenderStates.empty() || pendingTextureMask != 0 ||
               pendingStreamMask != 0 || pendingFvf ||
               pendingVs || pendingPs || pendingVdecl ||
               pendingIb || pendingRtMask != 0 || pendingDs ||
               pendingViewport || pendingScissor ||
               !pendingTss.empty() || !pendingSamplerStates.empty() ||
               pendingMaterial || pendingClipPlaneMask != 0 ||
               !pendingTransforms.empty() ||
               pendingLightSlotMask != 0 ||
               pendingLightEnableValidMask != 0;
    }

    void clearPendingHotState() noexcept {
        pendingRenderStates.clear();
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
        pendingTss.clear();
        pendingSamplerStates.clear();
        pendingMaterial = false;
        pendingClipPlaneMask = 0;
        pendingTransforms.clear();
        pendingLightSlotMask = 0;
        pendingLightEnableValidMask = 0;
        pendingLightEnableMask = 0;
    }

    void clearServerShadowTables() noexcept {
        renderStateShadow.clear();
        tssShadow.clear();
        samplerStateShadow.clear();
        transformShadow.clear();
    }

    bool renderStateEquals(std::uint32_t state, std::uint32_t value) const noexcept {
        std::uint32_t shadowValue = 0;
        return renderStateShadow.get(state, shadowValue) && shadowValue == value;
    }
};
