#pragma once

#include "d3d9_pe.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

static constexpr std::uint32_t kPeRenderStateSlots = 256;
static constexpr std::uint32_t kPeTextureStageSlots = 8;
static constexpr std::uint32_t kPeTextureStageStateSlots = 64;
static constexpr std::uint32_t kPeFragmentSamplerSlots = 16;
static constexpr std::uint32_t kPeVertexTextureSamplerSlots = 4;
static constexpr std::uint32_t kPeTextureSlots =
    kPeFragmentSamplerSlots + kPeVertexTextureSamplerSlots;
static constexpr std::uint32_t kPeSamplerSlots = kPeTextureSlots;
static constexpr std::uint32_t kPeSamplerStateSlots = 64;
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
        if (state == static_cast<std::uint32_t>(D3DTS_VIEW)) {
            slot = 0;
            return true;
        }
        if (state == static_cast<std::uint32_t>(D3DTS_PROJECTION)) {
            slot = 1;
            return true;
        }
        if (state >= static_cast<std::uint32_t>(D3DTS_TEXTURE0) &&
            state <= static_cast<std::uint32_t>(D3DTS_TEXTURE7)) {
            slot = kPeTransformTextureBaseSlot +
                   (state - static_cast<std::uint32_t>(D3DTS_TEXTURE0));
            return true;
        }
        if (state >= static_cast<std::uint32_t>(D3DTS_WORLD) &&
            state < static_cast<std::uint32_t>(D3DTS_WORLD) +
                        kPeTransformWorldSlots) {
            slot = kPeTransformWorldBaseSlot +
                   (state - static_cast<std::uint32_t>(D3DTS_WORLD));
            return true;
        }
        return false;
    }
    static std::uint32_t stateForSlot(std::uint32_t slot) noexcept {
        if (slot == 0) {
            return static_cast<std::uint32_t>(D3DTS_VIEW);
        }
        if (slot == 1) {
            return static_cast<std::uint32_t>(D3DTS_PROJECTION);
        }
        if (slot < kPeTransformWorldBaseSlot) {
            return static_cast<std::uint32_t>(D3DTS_TEXTURE0) +
                   (slot - kPeTransformTextureBaseSlot);
        }
        return static_cast<std::uint32_t>(D3DTS_WORLD) +
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

inline std::uint32_t textureStageSlot(DWORD stage) noexcept {
    return std::min<std::uint32_t>(stage, kPeTextureStageSlots - 1u);
}

inline bool vertexTextureSamplerSlot(DWORD sampler, std::uint32_t& slot) noexcept {
    if (sampler < D3DVERTEXTEXTURESAMPLER0 || sampler > D3DVERTEXTEXTURESAMPLER3) {
        return false;
    }
    slot = kPeFragmentSamplerSlots +
        static_cast<std::uint32_t>(sampler - D3DVERTEXTEXTURESAMPLER0);
    return true;
}

inline bool textureBindingSlot(DWORD stage, std::uint32_t& slot) noexcept {
    if (stage < kPeFragmentSamplerSlots) {
        slot = static_cast<std::uint32_t>(stage);
        return true;
    }
    return vertexTextureSamplerSlot(stage, slot);
}

inline std::uint32_t textureStageStateSlot(
    D3DTEXTURESTAGESTATETYPE type) noexcept {
    return std::min<std::uint32_t>(
        static_cast<std::uint32_t>(type), kPeTextureStageStateSlots - 1u);
}

inline bool samplerSlot(DWORD sampler, std::uint32_t& slot) noexcept {
    if (sampler < kPeFragmentSamplerSlots) {
        slot = static_cast<std::uint32_t>(sampler);
        return true;
    }
    return vertexTextureSamplerSlot(sampler, slot);
}

inline bool samplerStateSlot(D3DSAMPLERSTATETYPE type,
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

struct ConstShadow {
    std::vector<std::uint8_t> values;
    std::uint32_t dirtyStart = 0;
    std::uint32_t dirtyEnd = 0;

    bool dirty() const {
        return dirtyEnd > dirtyStart;
    }
    void clear() {
        dirtyStart = dirtyEnd = 0;
    }
    void reset() {
        values.clear();
        clear();
    }
};

struct PeConstShadowBlock {
    ConstShadow vsConstF{};
    ConstShadow vsConstI{};
    ConstShadow vsConstB{};
    ConstShadow psConstF{};
    ConstShadow psConstI{};
    ConstShadow psConstB{};

    void reset() {
        vsConstF.reset();
        vsConstI.reset();
        vsConstB.reset();
        psConstF.reset();
        psConstI.reset();
        psConstB.reset();
    }
};

inline void touchConstShadow(ConstShadow& shadow,
                             UINT start,
                             UINT count,
                             const void* data,
                             std::size_t elemSize) {
    const std::uint64_t needed64 =
        (static_cast<std::uint64_t>(start) + count) * elemSize;
    if (needed64 > 0xffffffffull) {
        return;
    }
    const auto needed = static_cast<std::size_t>(needed64);
    if (shadow.values.size() < needed) {
        shadow.values.resize(needed);
    }
    if (count > 0 && data) {
        std::memcpy(shadow.values.data() + start * elemSize,
                    data,
                    count * elemSize);
    }
    const std::uint32_t end = start + count;
    if (!shadow.dirty()) {
        shadow.dirtyStart = start;
        shadow.dirtyEnd = end;
    } else {
        shadow.dirtyStart = std::min<std::uint32_t>(shadow.dirtyStart, start);
        shadow.dirtyEnd = std::max<std::uint32_t>(shadow.dirtyEnd, end);
    }
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
    DWORD pendingTextureMask = 0;
    DWORD pendingStreamMask = 0;
    bool pendingFvf = false;
    bool pendingVs = false;
    bool pendingPs = false;
    bool pendingVdecl = false;
    bool pendingIb = false;
    DWORD pendingRtMask = 0;
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
    DWORD pendingClipPlaneMask = 0;
    float clipPlaneShadow[6 * 4]{};
    FixedTransformTable pendingTransforms{};
    FixedTransformTable transformShadow{};
    DWORD pendingLightSlotMask = 0;
    D9CLight lightShadow[D9C_DRAW_PACKET_MAX_LIGHTS]{};
    DWORD pendingLightEnableValidMask = 0;
    DWORD pendingLightEnableMask = 0;
    DWORD lightEnableShadow = 0;
    // SetClipStatus / GetClipStatus state round-trip (gap_d3d9 B.8). dxmt9
    // does not compute per-primitive clip results on this path (neither does
    // wined3d — its set/get_clip_status are storage-free stubs); this is a
    // pure shadow so GetClipStatus returns the last SetClipStatus value.
    // D3D9 post-CreateDevice default is ClipUnion=0, ClipIntersection=all-set.
    DWORD clipStatusUnion = 0;
    DWORD clipStatusIntersection = 0xFFFFFFFFu;

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

    bool renderStateEquals(DWORD state, DWORD value) const noexcept {
        std::uint32_t shadowValue = 0;
        return renderStateShadow.get(state, shadowValue) && shadowValue == value;
    }
};
