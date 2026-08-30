#pragma once

#include "dxmt9/device_c.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace dxmt9::d3d9::pe {

// One row per production PE command-record producer. Keep the six constant
// kinds and the two UP forms distinct: equal payload sizes are not semantic
// identity. This table is also the source for the generated TLA family table.
#define DXMT9_PE_SEMANTIC_PRODUCER_TABLE(X)                              \
  X(DrawPrimitive, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, Draw)              \
  X(DrawIndexedPrimitive, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE, Draw) \
  X(DrawPrimitiveUp, D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP, DrawUp)       \
  X(DrawIndexedPrimitiveUp, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP, DrawUp) \
  X(ApplyState, D9C_COMMAND_RECORD_APPLY_STATE, StateBlock)              \
  X(VsFloatConstant, D9C_COMMAND_RECORD_SET_VS_CONST_F, Constant)        \
  X(VsIntConstant, D9C_COMMAND_RECORD_SET_VS_CONST_I, Constant)          \
  X(VsBoolConstant, D9C_COMMAND_RECORD_SET_VS_CONST_B, Constant)         \
  X(PsFloatConstant, D9C_COMMAND_RECORD_SET_PS_CONST_F, Constant)        \
  X(PsIntConstant, D9C_COMMAND_RECORD_SET_PS_CONST_I, Constant)          \
  X(PsBoolConstant, D9C_COMMAND_RECORD_SET_PS_CONST_B, Constant)         \
  X(Clear, D9C_COMMAND_RECORD_CLEAR, Copy)                               \
  X(StretchRect, D9C_COMMAND_RECORD_STRETCH_RECT, Copy)                  \
  X(ColorFill, D9C_COMMAND_RECORD_COLOR_FILL, Copy)                      \
  X(UpdateTexture, D9C_COMMAND_RECORD_UPDATE_TEXTURE, Update)            \
  X(UpdateSurface, D9C_COMMAND_RECORD_UPDATE_SURFACE, Update)            \
  X(QueryIssue, D9C_COMMAND_RECORD_QUERY_ISSUE, Query)                   \
  X(Readback, D9C_COMMAND_RECORD_READBACK, Readback)                     \
  X(ReszDepthResolve, D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE, Copy)       \
  X(GenerateMipmaps, D9C_COMMAND_RECORD_GENERATE_MIPMAPS, Update)        \
  X(Present, D9C_COMMAND_RECORD_PRESENT, Present)

enum class PeSemanticProducerKind : std::uint8_t {
#define DXMT9_PE_SEMANTIC_PRODUCER_ENUM(Name, RecordType, Category) Name,
  DXMT9_PE_SEMANTIC_PRODUCER_TABLE(DXMT9_PE_SEMANTIC_PRODUCER_ENUM)
#undef DXMT9_PE_SEMANTIC_PRODUCER_ENUM
  Count,
};

enum class PeSemanticProducerCategory : std::uint8_t {
  Draw,
  DrawUp,
  StateBlock,
  Constant,
  Copy,
  Update,
  Query,
  Readback,
  Present,
};

struct PeSemanticProducerPolicyRow {
  PeSemanticProducerKind kind;
  PeSemanticProducerCategory category;
  std::uint32_t recordType;
};

inline constexpr auto kPeSemanticProducerPolicyTable = std::array{
#define DXMT9_PE_SEMANTIC_PRODUCER_ROW(Name, RecordType, Category) \
  PeSemanticProducerPolicyRow{PeSemanticProducerKind::Name, \
                              PeSemanticProducerCategory::Category, RecordType},
  DXMT9_PE_SEMANTIC_PRODUCER_TABLE(DXMT9_PE_SEMANTIC_PRODUCER_ROW)
#undef DXMT9_PE_SEMANTIC_PRODUCER_ROW
};
static_assert(kPeSemanticProducerPolicyTable.size() ==
              static_cast<std::size_t>(PeSemanticProducerKind::Count));
static_assert([]() constexpr {
  for (std::size_t index = 0u;
       index < kPeSemanticProducerPolicyTable.size(); ++index) {
    if (kPeSemanticProducerPolicyTable[index].kind !=
        static_cast<PeSemanticProducerKind>(index)) {
      return false;
    }
    for (std::size_t other = index + 1u;
         other < kPeSemanticProducerPolicyTable.size(); ++other) {
      if (kPeSemanticProducerPolicyTable[index].recordType ==
          kPeSemanticProducerPolicyTable[other].recordType) {
        return false;
      }
    }
  }
  return true;
}(), "semantic producer table must cover every record type exactly once");

constexpr const PeSemanticProducerPolicyRow* peSemanticProducerPolicy(
    std::uint32_t recordType) noexcept {
  for (const auto& row : kPeSemanticProducerPolicyTable) {
    if (row.recordType == recordType) return &row;
  }
  return nullptr;
}

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

enum class PeSemanticCaptureDisposition : std::uint8_t {
  Materialized,
  Rejected,
  Skipped,
};

struct PeCommittedSemanticToken {
  PeSemanticProducerKind producer = PeSemanticProducerKind::DrawPrimitive;
  PeSemanticProducerCategory category = PeSemanticProducerCategory::Draw;
  std::uint32_t semanticKey = 0u;
  std::uint32_t recordType = 0u;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;
  PeSemanticByteRange wireRange{};
  std::uint32_t exactValueOffset = 0u;
  std::uint32_t exactValueBytes = 0u;
  std::uint32_t exactIdentityOffset = 0u;
  std::uint32_t exactIdentityCount = 0u;
};

struct PeCommittedSemanticIdentityValue {
  std::uint32_t kind = 0u;
  std::uint32_t generation = 0u;
  std::uint64_t objectId = 0u;

  friend bool operator==(const PeCommittedSemanticIdentityValue&,
                         const PeCommittedSemanticIdentityValue&) = default;
};

static_assert(std::is_standard_layout_v<PeCommittedSemanticIdentityValue>);
static_assert(std::is_trivially_copyable_v<PeCommittedSemanticIdentityValue>);

struct PeCommittedSemanticProjectionFacts {
  std::uint32_t recordType = 0u;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;
  PeSemanticByteRange wireRange{};
  std::span<const std::byte> exactValue{};
  std::uint32_t exactIdentityCount = 0u;
  bool exactIdentitiesValid = false;
};

struct PeCommittedSemanticIdentityToken {
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;
  PeSemanticByteRange recordWireRange{};
  std::uint32_t identityOrdinal = 0u;
  std::uint32_t kind = 0u;
  std::uint32_t generation = 0u;
  std::uint64_t objectId = 0u;
};

constexpr PeSemanticProjectionAction planCommittedPeSemanticProjection(
    const PeCommittedSemanticProjectionFacts& facts) noexcept {
  const auto* producer = peSemanticProducerPolicy(facts.recordType);
  const bool exactValue = facts.exactValue.size() == facts.wireRange.length &&
                          !facts.exactValue.empty();
  return planPeSemanticProjection({
      .kind = PeSemanticEnvelopeKind::HeterogeneousRecord,
      .appendAccepted = true,
      .sourceOrdinalValid = facts.sourceOrdinal != 0u,
      .recordOrdinalValid = facts.recordOrdinal != 0u,
      .byteRangeValid = facts.wireRange.valid(),
      .exactSemanticMatch = producer && exactValue &&
                            facts.exactIdentitiesValid,
  });
}

// Cold, default-off production witness. Exact payload bytes and qualified
// handle identities are copied synchronously from the committed builder
// record into bounded cold arenas and survive until bridge/capture settlement.
// No pointer or observer metadata enters the D9C wire ABI.
class PeAllFamilySemanticTokenLedger final {
 public:
  static constexpr std::size_t capacity = 4096u;
  static constexpr std::size_t exactValueCapacity = 16u * 1024u * 1024u;
  // Typed production emitters name at most 45 draw handles today. Keep a
  // separately checked 64-per-record diagnostic ceiling so an accidental
  // unbounded handle producer fails closed instead of truncating identity.
  static constexpr std::size_t exactIdentityCapacity = capacity * 64u;

  PeAllFamilySemanticTokenLedger()
      : exactValues_(std::make_unique_for_overwrite<std::byte[]>(
            exactValueCapacity)),
        exactIdentities_(
            std::make_unique_for_overwrite<PeCommittedSemanticIdentityValue[]>(
                exactIdentityCapacity)) {}

  PeAllFamilySemanticTokenLedger(const PeAllFamilySemanticTokenLedger&) = delete;
  PeAllFamilySemanticTokenLedger& operator=(
      const PeAllFamilySemanticTokenLedger&) = delete;

  std::uint64_t beginSource(std::uint32_t recordType) noexcept {
    const auto* producer = peSemanticProducerPolicy(recordType);
    if (!producer) {
      return 0u;
    }
    const auto index = static_cast<std::size_t>(producer->kind);
    std::uint64_t ordinal = retryOrdinals_[index];
    retryOrdinals_[index] = 0u;
    if (ordinal == 0u) {
      if (nextSourceOrdinal_ == UINT64_MAX) return 0u;
      ordinal = ++nextSourceOrdinal_;
    }
    lastIssuedKind_ = producer->kind;
    lastIssuedSourceOrdinal_ = ordinal;
    lastIssuedRecordType_ = recordType;
    return ordinal;
  }

  void preserveForRetry(std::uint64_t sourceOrdinal) noexcept {
    if (sourceOrdinal == 0u || sourceOrdinal != lastIssuedSourceOrdinal_) {
      return;
    }
    retryOrdinals_[static_cast<std::size_t>(lastIssuedKind_)] = sourceOrdinal;
    ++preservedForRetry_;
  }

  PeSemanticProjectionAction accept(
      const PeCommittedSemanticProjectionFacts& facts) noexcept {
    const auto action = planCommittedPeSemanticProjection(facts);
    const auto* producer = peSemanticProducerPolicy(facts.recordType);
    // Structural validity is not enough here: an accepted builder record must
    // be the record for the exact issuance that opened this append.  In
    // particular, A/B/A must not let the first A's still-valid source ordinal
    // bind after the second A was issued, and a B record must not bind while A
    // is the most recent issuance even if all value/range fields are valid.
    const bool exactLatestIssuance =
        producer && lastIssuedSourceOrdinal_ != 0u &&
        facts.sourceOrdinal == lastIssuedSourceOrdinal_ &&
        facts.recordType == lastIssuedRecordType_ &&
        producer->kind == lastIssuedKind_;
    if (action != PeSemanticProjectionAction::Accept ||
        pendingCount_ == capacity ||
        !identityProjectionComplete_ ||
        !exactLatestIssuance ||
        completedIdentitySourceOrdinal_ != facts.sourceOrdinal ||
        completedIdentityRecordOrdinal_ != facts.recordOrdinal ||
        completedIdentityRange_.offset != facts.wireRange.offset ||
        completedIdentityRange_.length != facts.wireRange.length ||
        completedIdentityCount_ != facts.exactIdentityCount ||
        facts.exactValue.size() > exactValueCapacity - exactValueBytes_) {
      rollbackIdentityProjection();
      return action == PeSemanticProjectionAction::Accept
          ? PeSemanticProjectionAction::FailStop : action;
    }
    if (!producer || (lastAcceptedRecordOrdinal_ != 0u &&
                      facts.recordOrdinal <= lastAcceptedRecordOrdinal_)) {
      rollbackIdentityProjection();
      return PeSemanticProjectionAction::FailStop;
    }
    const auto valueOffset = exactValueBytes_;
    std::copy(facts.exactValue.begin(), facts.exactValue.end(),
              exactValues_.get() + valueOffset);
    exactValueBytes_ += facts.exactValue.size();
    pending_[pendingCount_++] = PeCommittedSemanticToken{
        .producer = producer->kind,
        .category = producer->category,
        .semanticKey = producer->recordType,
        .recordType = facts.recordType,
        .sourceOrdinal = facts.sourceOrdinal,
        .recordOrdinal = facts.recordOrdinal,
        .wireRange = facts.wireRange,
        .exactValueOffset = static_cast<std::uint32_t>(valueOffset),
        .exactValueBytes = static_cast<std::uint32_t>(facts.exactValue.size()),
        .exactIdentityOffset = static_cast<std::uint32_t>(
            completedIdentityOffset_),
        .exactIdentityCount = facts.exactIdentityCount,
    };
    identityProjectionStarted_ = false;
    identityProjectionComplete_ = false;
    lastAcceptedRecordOrdinal_ = facts.recordOrdinal;
    ++acceptedCount_;
    // A beginSource issuance is a one-shot witness for one committed record.
    // The next append must issue a fresh witness; otherwise a repeated accept
    // could replay the same source token without traversing appendRecord.
    lastIssuedKind_ = PeSemanticProducerKind::DrawPrimitive;
    lastIssuedSourceOrdinal_ = 0u;
    lastIssuedRecordType_ = 0u;
    return PeSemanticProjectionAction::Accept;
  }

  bool beginIdentityProjection(std::uint64_t sourceOrdinal,
                               std::uint64_t recordOrdinal,
                               PeSemanticByteRange range) noexcept {
    rollbackIdentityProjection();
    identitySourceOrdinal_ = sourceOrdinal;
    identityRecordOrdinal_ = recordOrdinal;
    identityRange_ = range;
    identityCount_ = 0u;
    identityOffset_ = exactIdentityCount_;
    identityProjectionStarted_ = true;
    identityProjectionComplete_ = false;
    identityProjectionValid_ = sourceOrdinal != 0u && recordOrdinal != 0u &&
                               range.valid();
    return identityProjectionValid_;
  }

  bool observeIdentity(
      const PeCommittedSemanticIdentityToken& token) noexcept {
    const bool valid = identityProjectionValid_ &&
        token.sourceOrdinal == identitySourceOrdinal_ &&
        token.recordOrdinal == identityRecordOrdinal_ &&
        token.recordWireRange.offset == identityRange_.offset &&
        token.recordWireRange.length == identityRange_.length &&
        token.identityOrdinal == identityCount_ &&
        token.kind <= D9C_CHUNK_HANDLE_KIND_QUERY &&
        token.generation != 0u && token.objectId != 0u &&
        exactIdentityCount_ < exactIdentityCapacity;
    identityProjectionValid_ = valid;
    if (valid) {
      exactIdentities_[exactIdentityCount_++] = {
          .kind = token.kind,
          .generation = token.generation,
          .objectId = token.objectId,
      };
      ++identityCount_;
    }
    return valid;
  }

  bool finishIdentityProjection(std::uint32_t expectedCount) noexcept {
    const bool valid = identityProjectionValid_ &&
                       identityCount_ == expectedCount;
    identityProjectionValid_ = false;
    identityProjectionComplete_ = valid;
    if (valid) {
      completedIdentitySourceOrdinal_ = identitySourceOrdinal_;
      completedIdentityRecordOrdinal_ = identityRecordOrdinal_;
      completedIdentityRange_ = identityRange_;
      completedIdentityOffset_ = identityOffset_;
      completedIdentityCount_ = identityCount_;
    } else {
      exactIdentityCount_ = identityOffset_;
      identityProjectionStarted_ = false;
    }
    return valid;
  }

  void bridgeEffectUnknown() noexcept {
    effectUnknown_ = true;
  }

  bool settleCapture(PeSemanticCaptureDisposition disposition) noexcept {
    if (effectUnknown_) return false;
    captureMaterialized_ +=
        disposition == PeSemanticCaptureDisposition::Materialized;
    captureRejected_ +=
        disposition == PeSemanticCaptureDisposition::Rejected;
    captureSkipped_ +=
        disposition == PeSemanticCaptureDisposition::Skipped;
    settledCount_ += pendingCount_;
    pendingCount_ = 0u;
    exactValueBytes_ = 0u;
    exactIdentityCount_ = 0u;
    identityProjectionStarted_ = false;
    identityProjectionComplete_ = false;
    return true;
  }

  void discard() noexcept {
    pendingCount_ = 0u;
    exactValueBytes_ = 0u;
    exactIdentityCount_ = 0u;
    retryOrdinals_ = {};
    lastIssuedSourceOrdinal_ = 0u;
    lastIssuedRecordType_ = 0u;
    identityProjectionValid_ = false;
    identityProjectionStarted_ = false;
    identityProjectionComplete_ = false;
    effectUnknown_ = false;
  }

  void clear() noexcept {
    discard();
    nextSourceOrdinal_ = 0u;
    lastAcceptedRecordOrdinal_ = 0u;
    preservedForRetry_ = 0u;
    acceptedCount_ = 0u;
    settledCount_ = 0u;
    captureMaterialized_ = 0u;
    captureRejected_ = 0u;
    captureSkipped_ = 0u;
  }

  std::size_t pendingCount() const noexcept { return pendingCount_; }
  std::uint64_t acceptedCount() const noexcept { return acceptedCount_; }
  std::uint64_t settledCount() const noexcept { return settledCount_; }
  std::uint64_t preservedForRetryCount() const noexcept {
    return preservedForRetry_;
  }
  bool effectUnknown() const noexcept { return effectUnknown_; }
  const PeCommittedSemanticToken& pending(std::size_t index) const noexcept {
    return pending_[index];
  }
  std::span<const std::byte> pendingExactValue(
      std::size_t index) const noexcept {
    const auto& token = pending_[index];
    return {exactValues_.get() + token.exactValueOffset,
            token.exactValueBytes};
  }
  std::span<const PeCommittedSemanticIdentityValue> pendingExactIdentities(
      std::size_t index) const noexcept {
    const auto& token = pending_[index];
    return {exactIdentities_.get() + token.exactIdentityOffset,
            token.exactIdentityCount};
  }

 private:
  void rollbackIdentityProjection() noexcept {
    if (identityProjectionStarted_) {
      exactIdentityCount_ = identityOffset_;
    }
    identityProjectionValid_ = false;
    identityProjectionStarted_ = false;
    identityProjectionComplete_ = false;
  }

  std::array<PeCommittedSemanticToken, capacity> pending_{};
  std::unique_ptr<std::byte[]> exactValues_{};
  std::unique_ptr<PeCommittedSemanticIdentityValue[]> exactIdentities_{};
  std::size_t pendingCount_ = 0u;
  std::size_t exactValueBytes_ = 0u;
  std::size_t exactIdentityCount_ = 0u;
  std::array<std::uint64_t,
             static_cast<std::size_t>(PeSemanticProducerKind::Count)>
      retryOrdinals_{};
  std::uint64_t nextSourceOrdinal_ = 0u;
  PeSemanticProducerKind lastIssuedKind_ =
      PeSemanticProducerKind::DrawPrimitive;
  std::uint64_t lastIssuedSourceOrdinal_ = 0u;
  std::uint32_t lastIssuedRecordType_ = 0u;
  std::uint64_t lastAcceptedRecordOrdinal_ = 0u;
  std::uint64_t preservedForRetry_ = 0u;
  std::uint64_t acceptedCount_ = 0u;
  std::uint64_t settledCount_ = 0u;
  std::uint64_t captureMaterialized_ = 0u;
  std::uint64_t captureRejected_ = 0u;
  std::uint64_t captureSkipped_ = 0u;
  bool effectUnknown_ = false;
  std::uint64_t identitySourceOrdinal_ = 0u;
  std::uint64_t identityRecordOrdinal_ = 0u;
  PeSemanticByteRange identityRange_{};
  std::size_t identityOffset_ = 0u;
  std::uint32_t identityCount_ = 0u;
  bool identityProjectionValid_ = false;
  bool identityProjectionStarted_ = false;
  bool identityProjectionComplete_ = false;
  std::uint64_t completedIdentitySourceOrdinal_ = 0u;
  std::uint64_t completedIdentityRecordOrdinal_ = 0u;
  PeSemanticByteRange completedIdentityRange_{};
  std::size_t completedIdentityOffset_ = 0u;
  std::uint32_t completedIdentityCount_ = 0u;
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
