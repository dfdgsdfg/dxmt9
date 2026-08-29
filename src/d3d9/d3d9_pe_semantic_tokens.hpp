#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace dxmt9::d3d9::pe {

enum class PeSemanticEnvelopeKind : std::uint8_t {
  Scalar,
  Matrix,
  Constant,
  ComBinding,
  HeterogeneousRecord,
  Count,
};

enum class PeSemanticProjectionAction : std::uint8_t {
  Accept,
  PreserveForRetry,
  FailStop,
};

struct PeSemanticProjectionFacts {
  PeSemanticEnvelopeKind kind = PeSemanticEnvelopeKind::Scalar;
  bool appendAccepted = false;
  bool sourceOrdinalValid = false;
  bool recordOrdinalValid = false;
  bool byteRangeValid = false;
  bool exactSemanticMatch = false;
};

struct PeSemanticProjectionPolicyRow {
  PeSemanticEnvelopeKind kind;
  bool exactSemanticIdentityRequired;
  bool exactByteRangeRequired;
};

inline constexpr auto kPeSemanticProjectionPolicyTable = std::array{
    PeSemanticProjectionPolicyRow{PeSemanticEnvelopeKind::Scalar, true, true},
    PeSemanticProjectionPolicyRow{PeSemanticEnvelopeKind::Matrix, true, true},
    PeSemanticProjectionPolicyRow{PeSemanticEnvelopeKind::Constant, true, true},
    PeSemanticProjectionPolicyRow{PeSemanticEnvelopeKind::ComBinding, true, true},
    PeSemanticProjectionPolicyRow{
        PeSemanticEnvelopeKind::HeterogeneousRecord, true, true},
};
static_assert(kPeSemanticProjectionPolicyTable.size() ==
              static_cast<std::size_t>(PeSemanticEnvelopeKind::Count));
static_assert([]() constexpr {
  for (std::size_t index = 0u;
       index < kPeSemanticProjectionPolicyTable.size(); ++index) {
    if (kPeSemanticProjectionPolicyTable[index].kind !=
        static_cast<PeSemanticEnvelopeKind>(index)) {
      return false;
    }
  }
  return true;
}(), "semantic projection policy must cover every kind exactly once");

// Shared production settlement predicate.  Unaccepted appends preserve their
// source token for retry; once a record is accepted, any missing ordinal,
// byte-range, or exact typed identity is effect-unknown and therefore
// fail-stop.  No type/sizeof surrogate participates in this decision.
constexpr PeSemanticProjectionAction planPeSemanticProjection(
    PeSemanticProjectionFacts facts) noexcept {
  if (!facts.appendAccepted) {
    return PeSemanticProjectionAction::PreserveForRetry;
  }
  for (const auto& row : kPeSemanticProjectionPolicyTable) {
    if (row.kind != facts.kind) continue;
    const bool valid = facts.sourceOrdinalValid &&
                       facts.recordOrdinalValid &&
                       (!row.exactByteRangeRequired || facts.byteRangeValid) &&
                       (!row.exactSemanticIdentityRequired ||
                        facts.exactSemanticMatch);
    return valid ? PeSemanticProjectionAction::Accept
                 : PeSemanticProjectionAction::FailStop;
  }
  return PeSemanticProjectionAction::FailStop;
}

struct PeSemanticByteRange {
  std::uint32_t offset = 0u;
  std::uint32_t length = 0u;

  constexpr bool valid() const noexcept {
    return length != 0u && offset <= 0xffffffffu - length;
  }
};

enum class PeConstantStage : std::uint8_t { Vertex, Pixel };
enum class PeConstantKind : std::uint8_t { Float, Int, Bool };

struct PeConstantSemanticEnvelope {
  PeConstantStage stage = PeConstantStage::Vertex;
  PeConstantKind kind = PeConstantKind::Float;
  std::uint32_t startRegister = 0u;
  std::uint32_t registerCount = 0u;
  std::span<const std::byte> exactBytes{};
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;
  PeSemanticByteRange wireRange{};
};

struct PeMatrixSemanticEnvelope {
  std::uint32_t transformState = 0u;
  // Bit identity, not floating-point equality: +0/-0 and NaN payloads are
  // distinct D3D9 state values and must project to the exact wire matrix.
  std::array<std::uint32_t, 16u> valueBits{};
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;
  PeSemanticByteRange wireRange{};
};

enum class PeComBindingCategory : std::uint8_t {
  Texture,
  Stream,
  VertexShader,
  PixelShader,
  Declaration,
  IndexBuffer,
  RenderTarget,
  DepthStencil,
};

struct PeComBindingSemanticEnvelope {
  PeComBindingCategory category = PeComBindingCategory::Texture;
  std::uint32_t slot = 0u;
  std::uint32_t kind = 0u;
  std::uint32_t generation = 0u;
  std::uint64_t objectId = 0u;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;
  PeSemanticByteRange wireRange{};
};

enum class PeHeterogeneousRecordCategory : std::uint8_t {
  Draw,
  DrawUp,
  StateBlock,
  Query,
  Readback,
  Update,
  Copy,
  Present,
  CaptureSettlement,
};

struct PeHeterogeneousRecordSemanticEnvelope {
  PeHeterogeneousRecordCategory category =
      PeHeterogeneousRecordCategory::Draw;
  std::uint64_t semanticKey = 0u;
  std::uint64_t semanticAux = 0u;
  std::span<const std::byte> exactValue{};
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;
  PeSemanticByteRange wireRange{};
};

inline bool peSemanticBytesEqual(std::span<const std::byte> a,
                                 std::span<const std::byte> b) noexcept {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

constexpr bool peSemanticIdentityEqual(
    const PeMatrixSemanticEnvelope& a,
    const PeMatrixSemanticEnvelope& b) noexcept {
  return a.transformState == b.transformState &&
         a.valueBits == b.valueBits;
}

inline bool peSemanticIdentityEqual(
    const PeConstantSemanticEnvelope& a,
    const PeConstantSemanticEnvelope& b) noexcept {
  return a.stage == b.stage && a.kind == b.kind &&
         a.startRegister == b.startRegister &&
         a.registerCount == b.registerCount &&
         peSemanticBytesEqual(a.exactBytes, b.exactBytes);
}

constexpr bool peSemanticIdentityEqual(
    const PeComBindingSemanticEnvelope& a,
    const PeComBindingSemanticEnvelope& b) noexcept {
  return a.category == b.category && a.slot == b.slot &&
         a.kind == b.kind && a.generation == b.generation &&
         a.objectId == b.objectId;
}

inline bool peSemanticIdentityEqual(
    const PeHeterogeneousRecordSemanticEnvelope& a,
    const PeHeterogeneousRecordSemanticEnvelope& b) noexcept {
  return a.category == b.category && a.semanticKey == b.semanticKey &&
         a.semanticAux == b.semanticAux &&
         peSemanticBytesEqual(a.exactValue, b.exactValue);
}

template <typename Envelope>
PeSemanticProjectionAction planTypedPeSemanticProjection(
    const Envelope& source, const Envelope& wire,
    bool appendAccepted) noexcept {
  PeSemanticEnvelopeKind kind = PeSemanticEnvelopeKind::HeterogeneousRecord;
  if constexpr (std::is_same_v<Envelope, PeMatrixSemanticEnvelope>) {
    kind = PeSemanticEnvelopeKind::Matrix;
  } else if constexpr (std::is_same_v<Envelope,
                                      PeConstantSemanticEnvelope>) {
    kind = PeSemanticEnvelopeKind::Constant;
  } else if constexpr (std::is_same_v<Envelope,
                                      PeComBindingSemanticEnvelope>) {
    kind = PeSemanticEnvelopeKind::ComBinding;
  }
  return planPeSemanticProjection({
      .kind = kind,
      .appendAccepted = appendAccepted,
      .sourceOrdinalValid = source.sourceOrdinal != 0u &&
                            source.sourceOrdinal == wire.sourceOrdinal,
      .recordOrdinalValid = source.recordOrdinal != 0u &&
                            source.recordOrdinal == wire.recordOrdinal,
      .byteRangeValid = source.wireRange.valid() && wire.wireRange.valid() &&
                        source.wireRange.offset == wire.wireRange.offset &&
                        source.wireRange.length == wire.wireRange.length,
      .exactSemanticMatch = peSemanticIdentityEqual(source, wire),
  });
}

// Mirrors the three scalar PendingDelta tables. It stores only occupancy and
// source ordinals; key/index and value remain in PendingDelta. Matrices, COM
// bindings, constants, and heterogeneous records use the distinct exact
// envelope types above and never reuse this scalar occupancy layout.
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
    const bool exact =
        (lastAcceptedRecordOrdinal_ == 0u ||
         tuple.recordOrdinal >= lastAcceptedRecordOrdinal_) &&
        canConsume(tuple.category, tuple.key, tuple.index,
                   tuple.sourceOrdinal);
    return planPeSemanticProjection({
        .kind = PeSemanticEnvelopeKind::Scalar,
        .appendAccepted = true,
        .sourceOrdinalValid = tuple.sourceOrdinal != 0u,
        .recordOrdinalValid = tuple.recordOrdinal != 0u,
        .byteRangeValid = true,
        .exactSemanticMatch = exact,
    }) == PeSemanticProjectionAction::Accept;
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
