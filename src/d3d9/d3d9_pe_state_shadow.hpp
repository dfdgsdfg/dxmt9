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
        if (!plan.valid || !plan.consumeRepresentedPending ||
            !plan.recordDurable) {
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
        if (!plan.valid || !plan.consumeRepresentedPending ||
            !plan.recordDurable) {
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
        if (!plan.valid || !plan.consumeRepresentedPending ||
            !plan.recordDurable) {
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
        if (!plan.valid || !plan.consumeRepresentedPending ||
            !plan.recordDurable) {
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
    // Fixed, kind-qualified recorded sets. Explicit recording writes are
    // last-write-wins; MultiplyTransform never enters transforms_.
    bool vertexDeclarationRecorded = false;

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
        renderStates_.clear();
        textureStageStates_.clear();
        samplerStates_.clear();
        transforms_.clear();
        vertexDeclarationRecorded = false;
    }

private:
    FixedStateTable<kPeRenderStateSlots> renderStates_{};
    FixedStateMatrix<kPeTextureStageSlots, kPeTextureStageStateSlots>
        textureStageStates_{};
    FixedStateMatrix<kPeSamplerSlots, kPeSamplerStateSlots> samplerStates_{};
    FixedTransformTable transforms_{};
};

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
static_assert(sizeof(StateBlockRecorded) == 21936u);
static_assert(sizeof(PeHotStateShadow) == 44944u);
static_assert(alignof(LiveShadow) == alignof(std::uint64_t));
static_assert(alignof(PendingDelta) == alignof(std::uint64_t));
static_assert(alignof(StateBlockRecorded) == alignof(std::uint64_t));
static_assert(alignof(PeHotStateShadow) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<LiveShadow>);
static_assert(std::is_trivially_copyable_v<PendingDelta>);
static_assert(std::is_trivially_copyable_v<StateBlockRecorded>);
static_assert(std::is_trivially_copyable_v<PeHotStateShadow>);
