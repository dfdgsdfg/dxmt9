#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dxmt9::d3d9::pe {

// Mirrors the three scalar PendingDelta tables. It stores only occupancy and
// source ordinals; key/index and value remain in PendingDelta. Matrices, COM
// bindings, constants, and heterogeneous records use the explicit no-token
// path.
enum class ScalarSemanticCategory : std::uint8_t {
  RenderState,
  TextureStageState,
  SamplerState,
};

constexpr std::size_t kPeScalarSemanticTokenCapacity =
    256u + (8u * 64u) + (20u * 16u);

struct ScalarSemanticProjectionTuple {
  ScalarSemanticCategory category = ScalarSemanticCategory::RenderState;
  std::uint32_t key = 0u;
  std::uint32_t index = 0u;
  std::uint32_t value = 0u;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;
};

struct PeScalarSemanticTokenLedger {
  static constexpr std::size_t capacity = kPeScalarSemanticTokenCapacity;

  constexpr std::size_t size() const noexcept { return count_; }
  constexpr bool empty() const noexcept { return count_ == 0u; }
  constexpr std::uint64_t sourceOrdinal() const noexcept { return sourceOrdinal_; }

  constexpr void clear() noexcept {
    renderMask_ = {};
    tssMask_ = {};
    samplerMask_ = {};
    // Ordinal arrays are intentionally not scrubbed: all lookups are masked,
    // and this owner is chunk-local. Resetting the masks/count makes every
    // stale ordinal unreachable while avoiding an 8.7 KiB reset on the hot
    // recorder boundary.
    count_ = 0u;
    sourceOrdinal_ = 0u;
    lastAcceptedRecordOrdinal_ = 0u;
  }

  constexpr bool canRecord(ScalarSemanticCategory category,
                           std::uint32_t key,
                           std::uint32_t index = 0u) const noexcept {
    bool present = false;
    if (!valid(category, key, index, &present)) return false;
    return present || count_ < capacity;
  }

  // Called after the shared typed LiveShadow/PendingDelta transition. No
  // key/value copy is made: PendingDelta remains the sole scalar value owner.
  constexpr bool record(ScalarSemanticCategory category,
                        std::uint32_t key,
                        std::uint32_t index = 0u) noexcept {
    bool present = false;
    if (!valid(category, key, index, &present) ||
        (!present && count_ == capacity)) return false;
    set(category, key, index, ++sourceOrdinal_, present);
    return true;
  }

  constexpr bool has(ScalarSemanticCategory category, std::uint32_t key,
                     std::uint32_t index = 0u) const noexcept {
    bool present = false;
    return valid(category, key, index, &present) && present;
  }

  constexpr std::uint64_t sourceOrdinalFor(
      ScalarSemanticCategory category, std::uint32_t key,
      std::uint32_t index = 0u) const noexcept {
    bool present = false;
    if (!valid(category, key, index, &present) || !present) return 0u;
    return get(category, key, index);
  }

  // Ephemeral exact tuple: value comes from the corresponding PendingDelta
  // slot; this owner retains no duplicate scalar value.
  constexpr bool project(ScalarSemanticCategory category,
                         std::uint32_t key, std::uint32_t index,
                         std::uint32_t value, std::uint64_t recordOrdinal,
                         ScalarSemanticProjectionTuple& out) const noexcept {
    const auto ordinal = sourceOrdinalFor(category, key, index);
    if (ordinal == 0u || recordOrdinal == 0u) return false;
    out = ScalarSemanticProjectionTuple{
        category, key, index, value, ordinal, recordOrdinal};
    return true;
  }

  // The accepted tuple is the production semantic witness. Record ordinals
  // are PE-local and monotone for active builder records; equal ordinals are
  // valid when one record consumes multiple scalar sections. No wire bytes or
  // value hash is stored here: PendingDelta remains the value owner.
  constexpr bool canConsumeProjected(
      const ScalarSemanticProjectionTuple& tuple) const noexcept {
    return tuple.recordOrdinal != 0u &&
           (lastAcceptedRecordOrdinal_ == 0u ||
            tuple.recordOrdinal >= lastAcceptedRecordOrdinal_) &&
           canConsume(tuple.category, tuple.key, tuple.index,
                      tuple.sourceOrdinal);
  }

  constexpr bool consumeProjected(
      const ScalarSemanticProjectionTuple& tuple) noexcept {
    if (!canConsumeProjected(tuple) ||
        !consume(tuple.category, tuple.key, tuple.index,
                 tuple.sourceOrdinal)) {
      return false;
    }
    lastAcceptedRecordOrdinal_ = tuple.recordOrdinal;
    return true;
  }

  // Caller validates/projects every tuple first; this second pass atomically
  // clears exact pending metadata. Each operation is O(1).
  constexpr bool consume(ScalarSemanticCategory category,
                         std::uint32_t key, std::uint32_t index,
                         std::uint64_t sourceOrdinal) noexcept {
    bool present = false;
    if (!valid(category, key, index, &present) || !present ||
        get(category, key, index) != sourceOrdinal) return false;
    clearSlot(category, key, index);
    --count_;
    return true;
  }

  constexpr bool canConsume(ScalarSemanticCategory category,
                            std::uint32_t key, std::uint32_t index,
                            std::uint64_t sourceOrdinal) const noexcept {
    bool present = false;
    return valid(category, key, index, &present) && present &&
           get(category, key, index) == sourceOrdinal;
  }

  // Ordered direct state-block Apply supersedes an older pending delta without
  // projecting it into a chunk record. Keep this observer-only erase distinct
  // from accepted projection consumption.
  constexpr bool eraseSuperseded(ScalarSemanticCategory category,
                                 std::uint32_t key,
                                 std::uint32_t index = 0u) noexcept {
    bool present = false;
    if (!valid(category, key, index, &present) || !present) return false;
    clearSlot(category, key, index);
    --count_;
    return true;
  }

 private:
  constexpr bool valid(ScalarSemanticCategory category,
                       std::uint32_t key, std::uint32_t index,
                       bool* present) const noexcept {
    switch (category) {
    case ScalarSemanticCategory::RenderState:
      if (key >= 256u || index != 0u) return false;
      *present = (renderMask_[key >> 6u] & (1ull << (key & 63u))) != 0u;
      return true;
    case ScalarSemanticCategory::TextureStageState:
      if (key >= 8u || index >= 64u) return false;
      *present = (tssMask_[key] & (1ull << index)) != 0u;
      return true;
    case ScalarSemanticCategory::SamplerState:
      if (key >= 20u || index >= 16u) return false;
      *present = (samplerMask_[key] & static_cast<std::uint16_t>(1u << index)) != 0u;
      return true;
    }
    return false;
  }

  constexpr std::uint64_t get(ScalarSemanticCategory category,
                              std::uint32_t key,
                              std::uint32_t index) const noexcept {
    switch (category) {
    case ScalarSemanticCategory::RenderState: return renderOrdinals_[key];
    case ScalarSemanticCategory::TextureStageState:
      return tssOrdinals_[key * 64u + index];
    case ScalarSemanticCategory::SamplerState:
      return samplerOrdinals_[key * 16u + index];
    }
    return 0u;
  }

  constexpr void set(ScalarSemanticCategory category, std::uint32_t key,
                     std::uint32_t index, std::uint64_t ordinal,
                     bool present) noexcept {
    switch (category) {
    case ScalarSemanticCategory::RenderState:
      renderOrdinals_[key] = ordinal;
      renderMask_[key >> 6u] |= 1ull << (key & 63u);
      break;
    case ScalarSemanticCategory::TextureStageState:
      tssOrdinals_[key * 64u + index] = ordinal;
      tssMask_[key] |= 1ull << index;
      break;
    case ScalarSemanticCategory::SamplerState:
      samplerOrdinals_[key * 16u + index] = ordinal;
      samplerMask_[key] |= static_cast<std::uint16_t>(1u << index);
      break;
    }
    if (!present) ++count_;
  }

  constexpr void clearSlot(ScalarSemanticCategory category,
                           std::uint32_t key, std::uint32_t index) noexcept {
    switch (category) {
    case ScalarSemanticCategory::RenderState:
      renderMask_[key >> 6u] &= ~(1ull << (key & 63u));
      renderOrdinals_[key] = 0u;
      break;
    case ScalarSemanticCategory::TextureStageState:
      tssMask_[key] &= ~(1ull << index);
      tssOrdinals_[key * 64u + index] = 0u;
      break;
    case ScalarSemanticCategory::SamplerState:
      samplerMask_[key] &= static_cast<std::uint16_t>(~(1u << index));
      samplerOrdinals_[key * 16u + index] = 0u;
      break;
    }
  }

  std::array<std::uint64_t, 4u> renderMask_{};
  std::array<std::uint64_t, 8u> tssMask_{};
  std::array<std::uint16_t, 20u> samplerMask_{};
  std::array<std::uint64_t, 256u> renderOrdinals_{};
  std::array<std::uint64_t, 8u * 64u> tssOrdinals_{};
  std::array<std::uint64_t, 20u * 16u> samplerOrdinals_{};
  std::size_t count_ = 0u;
  std::uint64_t sourceOrdinal_ = 0u;
  std::uint64_t lastAcceptedRecordOrdinal_ = 0u;
};

static_assert(std::is_standard_layout_v<ScalarSemanticProjectionTuple>);
static_assert(std::is_trivially_copyable_v<ScalarSemanticProjectionTuple>);
static_assert(std::is_standard_layout_v<PeScalarSemanticTokenLedger>);
static_assert(std::is_trivially_copyable_v<PeScalarSemanticTokenLedger>);
static_assert(sizeof(PeScalarSemanticTokenLedger) == 8864u,
              "cold scalar observer footprint changed");

}  // namespace dxmt9::d3d9::pe
