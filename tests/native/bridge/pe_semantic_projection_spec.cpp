#include "d3d9_pe_semantic_tokens.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool value, std::string_view message) {
  if (!value) throw Failure(std::string(message));
}

using dxmt9::d3d9::pe::PeScalarSemanticTokenLedger;
using dxmt9::d3d9::pe::ScalarSemanticCategory;
using dxmt9::d3d9::pe::ScalarSemanticProjectionTuple;

static_assert(sizeof(PeScalarSemanticTokenLedger) == 8864u,
              "the optional cold observer footprint is pinned");

void exactProjectionAndRetry() {
  PeScalarSemanticTokenLedger ledger{};
  check(ledger.empty(), "fresh ledger is explicit no-token");
  check(ledger.canRecord(ScalarSemanticCategory::RenderState, 7u),
        "new render key has bounded capacity");
  check(ledger.record(ScalarSemanticCategory::RenderState, 7u),
        "render source ordinal records");
  const auto first = ledger.sourceOrdinalFor(
      ScalarSemanticCategory::RenderState, 7u);
  ScalarSemanticProjectionTuple tuple{};
  check(ledger.project(ScalarSemanticCategory::RenderState, 7u, 0u,
                       0x1234u, 41u, tuple),
        "exact projection tuple is ephemeral");
  check(tuple.key == 7u && tuple.value == 0x1234u &&
            tuple.sourceOrdinal == first && tuple.recordOrdinal == 41u,
        "projection carries category/key/value/source/record ordinals");
  ScalarSemanticProjectionTuple invalid{};
  check(!ledger.project(ScalarSemanticCategory::RenderState, 7u, 0u,
                        0x1234u, 0u, invalid),
        "zero record ordinal is rejected");
  check(ledger.canConsumeProjected(tuple),
        "projected tuple is a valid allocation-free transition witness");
  check(!ledger.consume(ScalarSemanticCategory::RenderState, 7u, 0u,
                        first + 1u),
        "stale retry witness cannot consume pending metadata");
  check(ledger.has(ScalarSemanticCategory::RenderState, 7u),
        "failed retry preserves metadata");
  check(ledger.consumeProjected(tuple),
        "accepted projected tuple consumes its exact source token");
  check(ledger.empty(), "accepted token is no longer pending");
  check(ledger.record(ScalarSemanticCategory::RenderState, 8u),
        "second projected source ordinal records");
  ScalarSemanticProjectionTuple older{};
  check(ledger.project(ScalarSemanticCategory::RenderState, 8u, 0u,
                       0x5678u, 40u, older),
        "older ordinal remains a representable tuple");
  check(!ledger.consumeProjected(older),
        "older record ordinal cannot consume a projected token");
}

void replacementAndDomainCapacity() {
  PeScalarSemanticTokenLedger ledger{};
  check(ledger.record(ScalarSemanticCategory::TextureStageState, 1u, 2u),
        "TSS key records");
  const auto first = ledger.sourceOrdinalFor(
      ScalarSemanticCategory::TextureStageState, 1u, 2u);
  check(ledger.record(ScalarSemanticCategory::TextureStageState, 1u, 2u),
        "TSS replacement records without another slot");
  check(ledger.size() == 1u && ledger.sourceOrdinalFor(
                                 ScalarSemanticCategory::TextureStageState,
                                 1u, 2u) != first,
        "replacement is O(1) and advances source ordinal");
  check(ledger.record(ScalarSemanticCategory::SamplerState, 3u, 4u),
        "sampler key records");
  check(ledger.eraseSuperseded(ScalarSemanticCategory::SamplerState, 3u, 4u),
        "ordered direct supersession removes its cold witness");
  check(!ledger.has(ScalarSemanticCategory::SamplerState, 3u, 4u),
        "superseded witness cannot reach later settlement");
  check(!ledger.canRecord(ScalarSemanticCategory::SamplerState, 20u, 0u),
        "out-of-domain sampler key fails closed");
  ledger.clear();
  check(!ledger.has(ScalarSemanticCategory::TextureStageState, 1u, 2u) &&
            ledger.sourceOrdinalFor(ScalarSemanticCategory::SamplerState,
                                    3u, 4u) == 0u,
        "clear masks stale ordinal arrays without bulk scrubbing");
}

void everyDistinctScalarSlotIsRepresentable() {
  PeScalarSemanticTokenLedger ledger{};
  std::uint64_t expectedSource = 0u;
  for (std::uint32_t state = 0u; state < 256u; ++state) {
    check(ledger.record(ScalarSemanticCategory::RenderState, state),
          "every render-state slot records");
    check(ledger.sourceOrdinalFor(ScalarSemanticCategory::RenderState,
                                  state) == ++expectedSource,
          "render-state source ordinal is exact");
  }
  for (std::uint32_t stage = 0u; stage < 8u; ++stage) {
    for (std::uint32_t type = 0u; type < 64u; ++type) {
      check(ledger.record(ScalarSemanticCategory::TextureStageState,
                          stage, type),
            "every TSS slot records");
      check(ledger.sourceOrdinalFor(
                ScalarSemanticCategory::TextureStageState, stage, type) ==
                ++expectedSource,
            "TSS source ordinal is exact");
    }
  }
  for (std::uint32_t sampler = 0u; sampler < 20u; ++sampler) {
    for (std::uint32_t type = 0u; type < 16u; ++type) {
      check(ledger.record(ScalarSemanticCategory::SamplerState,
                          sampler, type),
            "every sampler slot records");
      check(ledger.sourceOrdinalFor(ScalarSemanticCategory::SamplerState,
                                    sampler, type) == ++expectedSource,
            "sampler source ordinal is exact");
    }
  }
  check(expectedSource == PeScalarSemanticTokenLedger::capacity &&
            ledger.size() == PeScalarSemanticTokenLedger::capacity,
        "all 1,088 distinct scalar slots fit exactly");
  check(!ledger.canRecord(ScalarSemanticCategory::SamplerState, 20u, 0u),
        "the exhaustive domain remains closed");
}

void exhaustiveHeterogeneousSettlementPredicate() {
  using namespace dxmt9::d3d9::pe;
  for (unsigned kindValue = 0u;
       kindValue < static_cast<unsigned>(PeSemanticEnvelopeKind::Count);
       ++kindValue) {
    const auto kind = static_cast<PeSemanticEnvelopeKind>(kindValue);
    for (unsigned accepted = 0u; accepted < 2u; ++accepted) {
      for (unsigned source = 0u; source < 2u; ++source) {
        for (unsigned record = 0u; record < 2u; ++record) {
          for (unsigned range = 0u; range < 2u; ++range) {
            for (unsigned exact = 0u; exact < 2u; ++exact) {
              const auto action = planPeSemanticProjection({
                  .kind = kind,
                  .appendAccepted = accepted != 0u,
                  .sourceOrdinalValid = source != 0u,
                  .recordOrdinalValid = record != 0u,
                  .byteRangeValid = range != 0u,
                  .exactSemanticMatch = exact != 0u,
              });
              const auto expected = accepted == 0u
                  ? PeSemanticProjectionAction::PreserveForRetry
                  : (source && record && range && exact
                         ? PeSemanticProjectionAction::Accept
                         : PeSemanticProjectionAction::FailStop);
              check(action == expected,
                    "every typed semantic settlement row is exhaustive");
            }
          }
        }
      }
    }
  }
}

void typedEnvelopesRejectSameSizeSurrogates() {
  using namespace dxmt9::d3d9::pe;
  PeMatrixSemanticEnvelope matrix{};
  matrix.transformState = 7u;
  matrix.valueBits[0] = 0x3f800000u;
  matrix.sourceOrdinal = 1u;
  matrix.recordOrdinal = 9u;
  matrix.wireRange = {64u, 68u};
  auto otherMatrix = matrix;
  check(planTypedPeSemanticProjection(matrix, otherMatrix, true) ==
            PeSemanticProjectionAction::Accept,
        "matrix identity accepts exact state and IEEE-754 bits");
  otherMatrix.wireRange.offset++;
  check(planTypedPeSemanticProjection(matrix, otherMatrix, true) ==
            PeSemanticProjectionAction::FailStop,
        "semantic identity cannot substitute for its committed byte range");
  otherMatrix = matrix;
  otherMatrix.recordOrdinal++;
  check(planTypedPeSemanticProjection(matrix, otherMatrix, true) ==
            PeSemanticProjectionAction::FailStop,
        "semantic identity cannot substitute for its wire record ordinal");
  otherMatrix = matrix;
  otherMatrix.wireRange = {0xffffffffu, 1u};
  check(planTypedPeSemanticProjection(matrix, otherMatrix, true) ==
            PeSemanticProjectionAction::FailStop,
        "overflowing committed byte range fails closed");
  otherMatrix = matrix;
  otherMatrix.valueBits[0] ^= 1u;
  check(planTypedPeSemanticProjection(matrix, otherMatrix, true) ==
            PeSemanticProjectionAction::FailStop,
        "same-size matrix with different bits is not a semantic match");

  const std::array constantBytes = {
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
  };
  const std::array otherConstantBytes = {
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x05},
  };
  PeConstantSemanticEnvelope constant{
      .stage = PeConstantStage::Vertex,
      .kind = PeConstantKind::Float,
      .startRegister = 3u,
      .registerCount = 1u,
      .exactBytes = constantBytes,
      .sourceOrdinal = 2u,
      .recordOrdinal = 10u,
      .wireRange = {132u, 4u},
  };
  auto otherConstant = constant;
  otherConstant.exactBytes = otherConstantBytes;
  check(planTypedPeSemanticProjection(constant, otherConstant, true) ==
            PeSemanticProjectionAction::FailStop,
        "same-family same-size constant bytes remain exact values");

  PeComBindingSemanticEnvelope binding{
      .category = PeComBindingCategory::Texture,
      .slot = 4u,
      .kind = 2u,
      .generation = 11u,
      .objectId = 0x1234u,
      .sourceOrdinal = 3u,
      .recordOrdinal = 11u,
      .wireRange = {200u, 12u},
  };
  auto otherBinding = binding;
  otherBinding.generation++;
  check(planTypedPeSemanticProjection(binding, otherBinding, true) ==
            PeSemanticProjectionAction::FailStop,
        "COM binding identity includes category, slot, kind, generation and id");

  const std::array drawValue = {
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
  };
  const std::array presentValue = {
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x41},
  };
  PeHeterogeneousRecordSemanticEnvelope record{
      .category = PeHeterogeneousRecordCategory::Draw,
      .semanticKey = 4u,
      .semanticAux = 17u,
      .exactValue = drawValue,
      .sourceOrdinal = 4u,
      .recordOrdinal = 12u,
      .wireRange = {256u, 4u},
  };
  auto otherRecord = record;
  otherRecord.category = PeHeterogeneousRecordCategory::Present;
  otherRecord.exactValue = presentValue;
  check(planTypedPeSemanticProjection(record, otherRecord, true) ==
            PeSemanticProjectionAction::FailStop,
        "heterogeneous record category/key/value beats type-size surrogates");
  check(planTypedPeSemanticProjection(record, otherRecord, false) ==
            PeSemanticProjectionAction::PreserveForRetry,
        "pre-effect rejection preserves the exact heterogeneous source token");
}

void prepareExactIdentities(
    dxmt9::d3d9::pe::PeAllFamilySemanticTokenLedger& ledger,
    std::uint64_t sourceOrdinal, std::uint64_t recordOrdinal,
    dxmt9::d3d9::pe::PeSemanticByteRange range,
    std::uint32_t count, std::uint64_t objectSeed) {
  using namespace dxmt9::d3d9::pe;
  check(ledger.beginIdentityProjection(sourceOrdinal, recordOrdinal, range),
        "exact identity projection begins");
  for (std::uint32_t index = 0u; index < count; ++index) {
    check(ledger.observeIdentity({
              .sourceOrdinal = sourceOrdinal,
              .recordOrdinal = recordOrdinal,
              .recordWireRange = range,
              .identityOrdinal = index,
              .kind = index % (D9C_CHUNK_HANDLE_KIND_QUERY + 1u),
              .generation = index + 1u,
              .objectId = objectSeed + index,
          }),
          "every exact qualified identity is admitted in ordinal order");
  }
  check(ledger.finishIdentityProjection(count),
        "exact identity projection count settles");
}

void everyProductionFamilyBindsExactToken() {
  using namespace dxmt9::d3d9::pe;
  PeAllFamilySemanticTokenLedger ledger{};
  const std::array exactValue = {
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
  };
  std::uint64_t recordOrdinal = 1u;
  for (const auto& row : kPeSemanticProducerPolicyTable) {
    const auto sourceOrdinal = ledger.beginSource(row.recordType);
    check(sourceOrdinal != 0u, "every production family issues a source ordinal");
    const PeSemanticByteRange range{
        .offset = static_cast<std::uint32_t>(recordOrdinal * 8u),
        .length = static_cast<std::uint32_t>(exactValue.size()),
    };
    prepareExactIdentities(ledger, sourceOrdinal, recordOrdinal, range, 2u,
                           0x10000u + recordOrdinal * 4u);
    check(ledger.accept({
              .recordType = row.recordType,
              .sourceOrdinal = sourceOrdinal,
              .recordOrdinal = recordOrdinal,
              .wireRange = range,
              .exactValue = exactValue,
              .exactIdentityCount = 2u,
              .exactIdentitiesValid = true,
          }) == PeSemanticProjectionAction::Accept,
          "every production family accepts only its exact committed token");
    const auto& token = ledger.pending(ledger.pendingCount() - 1u);
    const auto identities = ledger.pendingExactIdentities(
        ledger.pendingCount() - 1u);
    check(token.producer == row.kind && token.category == row.category &&
              token.semanticKey == row.recordType &&
              token.recordType == row.recordType &&
              token.sourceOrdinal == sourceOrdinal &&
              token.recordOrdinal == recordOrdinal &&
              token.exactValueBytes == exactValue.size() &&
              token.exactIdentityCount == 2u &&
              peSemanticBytesEqual(
                  ledger.pendingExactValue(ledger.pendingCount() - 1u),
                  exactValue) && identities.size() == 2u &&
              identities[0].generation == 1u &&
              identities[0].objectId == 0x10000u + recordOrdinal * 4u &&
              identities[1].generation == 2u,
          "category/key/value-or-identity/source/record/range remain qualified");
    ++recordOrdinal;
  }
  check(ledger.pendingCount() == kPeSemanticProducerPolicyTable.size(),
        "all-family corpus retains one bounded token per accepted record");
  check(ledger.settleCapture(PeSemanticCaptureDisposition::Materialized) &&
            ledger.pendingCount() == 0u &&
            ledger.settledCount() == kPeSemanticProducerPolicyTable.size(),
        "capture materialization settles every accepted family exactly once");
}

void exhaustiveAllFamilyCounterexamples() {
  using namespace dxmt9::d3d9::pe;
  const std::array value = {std::byte{0x7a}, std::byte{0x11}};
  const auto recordType = kPeSemanticProducerPolicyTable.front().recordType;
  for (unsigned sourceValid = 0u; sourceValid < 2u; ++sourceValid) {
    for (unsigned recordValid = 0u; recordValid < 2u; ++recordValid) {
      for (unsigned rangeValid = 0u; rangeValid < 2u; ++rangeValid) {
        for (unsigned valueValid = 0u; valueValid < 2u; ++valueValid) {
          for (unsigned identitiesValid = 0u; identitiesValid < 2u;
               ++identitiesValid) {
            const auto facts = PeCommittedSemanticProjectionFacts{
                .recordType = recordType,
                .sourceOrdinal = sourceValid ? 1u : 0u,
                .recordOrdinal = recordValid ? 1u : 0u,
                .wireRange = rangeValid
                    ? PeSemanticByteRange{.offset = 8u, .length = 2u}
                    : PeSemanticByteRange{},
                .exactValue = valueValid ? std::span<const std::byte>(value)
                                         : std::span<const std::byte>{},
                .exactIdentityCount = 1u,
                .exactIdentitiesValid = identitiesValid != 0u,
            };
            const bool exact = sourceValid && recordValid && rangeValid &&
                               valueValid && identitiesValid;
            check(planCommittedPeSemanticProjection(facts) ==
                      (exact ? PeSemanticProjectionAction::Accept
                             : PeSemanticProjectionAction::FailStop),
                  "every missing exact-token field is an independent counterexample");
          }
        }
      }
    }
  }
  auto unknown = PeCommittedSemanticProjectionFacts{
      .recordType = 0xffffffffu,
      .sourceOrdinal = 1u,
      .recordOrdinal = 1u,
      .wireRange = {.offset = 4u, .length = 2u},
      .exactValue = value,
      .exactIdentityCount = 0u,
      .exactIdentitiesValid = true,
  };
  check(planCommittedPeSemanticProjection(unknown) ==
            PeSemanticProjectionAction::FailStop,
        "unknown same-size record families fail closed");
}

void allFamilyRetryPoisonAndDifferential() {
  using namespace dxmt9::d3d9::pe;
  const std::array value = {std::byte{0x01}, std::byte{0x02},
                            std::byte{0x03}, std::byte{0x04}};
  PeAllFamilySemanticTokenLedger legacy{};
  PeAllFamilySemanticTokenLedger direct{};
  std::uint64_t recordOrdinal = 1u;
  for (const auto& row : kPeSemanticProducerPolicyTable) {
    const auto legacySource = legacy.beginSource(row.recordType);
    const auto directSource = direct.beginSource(row.recordType);
    check(legacySource == directSource,
          "legacy/direct source ordinals are differential inputs");
    const PeCommittedSemanticProjectionFacts facts{
        .recordType = row.recordType,
        .sourceOrdinal = legacySource,
        .recordOrdinal = recordOrdinal,
        .wireRange = {.offset = static_cast<std::uint32_t>(recordOrdinal * 4u),
                      .length = static_cast<std::uint32_t>(value.size())},
        .exactValue = value,
        .exactIdentityCount =
            static_cast<std::uint32_t>(recordOrdinal % 3u),
        .exactIdentitiesValid = true,
    };
    prepareExactIdentities(legacy, legacySource, recordOrdinal,
                           facts.wireRange, facts.exactIdentityCount,
                           0x20000u + recordOrdinal * 8u);
    prepareExactIdentities(direct, directSource, recordOrdinal,
                           facts.wireRange, facts.exactIdentityCount,
                           0x20000u + recordOrdinal * 8u);
    check(legacy.accept(facts) == PeSemanticProjectionAction::Accept &&
              direct.accept(facts) == PeSemanticProjectionAction::Accept,
          "legacy/direct paths accept the same immutable all-family batch");
    const auto& a = legacy.pending(legacy.pendingCount() - 1u);
    const auto& b = direct.pending(direct.pendingCount() - 1u);
    check(a.producer == b.producer && a.recordType == b.recordType &&
              a.sourceOrdinal == b.sourceOrdinal &&
              a.recordOrdinal == b.recordOrdinal &&
              a.wireRange.offset == b.wireRange.offset &&
              a.wireRange.length == b.wireRange.length &&
              a.exactValueBytes == b.exactValueBytes &&
              a.exactIdentityCount == b.exactIdentityCount &&
              peSemanticBytesEqual(
                  legacy.pendingExactValue(legacy.pendingCount() - 1u),
                  direct.pendingExactValue(direct.pendingCount() - 1u)) &&
              std::equal(
                  legacy.pendingExactIdentities(legacy.pendingCount() - 1u)
                      .begin(),
                  legacy.pendingExactIdentities(legacy.pendingCount() - 1u)
                      .end(),
                  direct.pendingExactIdentities(direct.pendingCount() - 1u)
                      .begin()),
          "legacy/direct semantic tokens are byte-range and identity exact");
    ++recordOrdinal;
  }
  const auto retry = legacy.beginSource(
      kPeSemanticProducerPolicyTable.front().recordType);
  legacy.preserveForRetry(retry);
  check(legacy.preservedForRetryCount() == 1u &&
            legacy.beginSource(
                kPeSemanticProducerPolicyTable.front().recordType) == retry,
        "pre-effect failure preserves and reissues the exact source ordinal");
  legacy.bridgeEffectUnknown();
  check(legacy.effectUnknown() &&
            !legacy.settleCapture(PeSemanticCaptureDisposition::Rejected) &&
            legacy.pendingCount() == kPeSemanticProducerPolicyTable.size(),
        "effect-unknown bridge failure poisons without retracting tokens");
  legacy.discard();
  check(!legacy.effectUnknown() && legacy.pendingCount() == 0u,
        "explicit reset/teardown discard releases poisoned token ownership");
  check(direct.settleCapture(PeSemanticCaptureDisposition::Skipped) &&
            direct.pendingCount() == 0u,
        "capture-skipped settlement accepts the command and clears tokens");
}

void allFamilyLatestIssuanceBinding() {
  using namespace dxmt9::d3d9::pe;
  const auto recordTypeA = kPeSemanticProducerPolicyTable[0].recordType;
  const auto recordTypeB = kPeSemanticProducerPolicyTable[1].recordType;
  const std::array value = {std::byte{0x41}, std::byte{0x42}};

  // A/B/A is the ABA case: the first A remains structurally valid but is no
  // longer the most recent issuance once the second A has been opened.
  PeAllFamilySemanticTokenLedger aba{};
  const auto sourceA1 = aba.beginSource(recordTypeA);
  const auto sourceB = aba.beginSource(recordTypeB);
  const auto sourceA2 = aba.beginSource(recordTypeA);
  check(sourceA1 != 0u && sourceB != 0u && sourceA2 != 0u &&
            sourceA1 != sourceB && sourceB != sourceA2,
        "A/B/A issues distinct source ordinals");
  const PeSemanticByteRange rangeA1{.offset = 8u, .length = 2u};
  prepareExactIdentities(aba, sourceA1, 1u, rangeA1, 0u, 0u);
  check(aba.accept({
              .recordType = recordTypeA,
              .sourceOrdinal = sourceA1,
              .recordOrdinal = 1u,
              .wireRange = rangeA1,
              .exactValue = value,
              .exactIdentityCount = 0u,
              .exactIdentitiesValid = true,
          }) == PeSemanticProjectionAction::FailStop &&
            aba.pendingCount() == 0u,
        "A/B/A rejects the older A despite structurally valid facts");

  // A valid source ordinal paired with the wrong producer family must also
  // fail.  This catches a fact that was assembled from a different emitter.
  PeAllFamilySemanticTokenLedger wrongProducer{};
  const auto producerSource = wrongProducer.beginSource(recordTypeA);
  const PeSemanticByteRange producerRange{.offset = 16u, .length = 2u};
  prepareExactIdentities(wrongProducer, producerSource, 1u, producerRange, 0u,
                         0u);
  check(wrongProducer.accept({
              .recordType = recordTypeB,
              .sourceOrdinal = producerSource,
              .recordOrdinal = 1u,
              .wireRange = producerRange,
              .exactValue = value,
              .exactIdentityCount = 0u,
              .exactIdentitiesValid = true,
          }) == PeSemanticProjectionAction::FailStop,
        "wrong producer family cannot consume the latest issuance");

  // The source value alone is not enough: a fabricated or stale source must
  // match the exact latest beginSource result, not merely be nonzero.
  PeAllFamilySemanticTokenLedger wrongSource{};
  const auto issuedSource = wrongSource.beginSource(recordTypeA);
  const auto fabricatedSource = issuedSource + 1u;
  const PeSemanticByteRange sourceRange{.offset = 24u, .length = 2u};
  prepareExactIdentities(wrongSource, fabricatedSource, 1u, sourceRange, 0u,
                         0u);
  check(wrongSource.accept({
              .recordType = recordTypeA,
              .sourceOrdinal = fabricatedSource,
              .recordOrdinal = 1u,
              .wireRange = sourceRange,
              .exactValue = value,
              .exactIdentityCount = 0u,
              .exactIdentitiesValid = true,
          }) == PeSemanticProjectionAction::FailStop,
        "wrong source ordinal cannot consume the latest issuance");

  // A failed append still preserves its exact issuance for retry, even when
  // another family is opened in between.  The retry rebinds the latest
  // producer/type pair before accepting its facts.
  PeAllFamilySemanticTokenLedger retry{};
  const auto retrySource = retry.beginSource(recordTypeA);
  retry.preserveForRetry(retrySource);
  (void)retry.beginSource(recordTypeB);
  check(retry.beginSource(recordTypeA) == retrySource,
        "retry preserves the exact source across another family issuance");
  const PeSemanticByteRange retryRange{.offset = 32u, .length = 2u};
  prepareExactIdentities(retry, retrySource, 1u, retryRange, 0u, 0u);
  check(retry.accept({
              .recordType = recordTypeA,
              .sourceOrdinal = retrySource,
              .recordOrdinal = 1u,
              .wireRange = retryRange,
              .exactValue = value,
              .exactIdentityCount = 0u,
              .exactIdentitiesValid = true,
          }) == PeSemanticProjectionAction::Accept,
        "exact latest retry issuance remains accepted");
  prepareExactIdentities(retry, retrySource, 2u, retryRange, 0u, 0u);
  check(retry.accept({
              .recordType = recordTypeA,
              .sourceOrdinal = retrySource,
              .recordOrdinal = 2u,
              .wireRange = retryRange,
              .exactValue = value,
              .exactIdentityCount = 0u,
              .exactIdentitiesValid = true,
          }) == PeSemanticProjectionAction::FailStop &&
            retry.pendingCount() == 1u,
        "a consumed issuance cannot be accepted a second time");
}

void exactQualifiedIdentityTokens() {
  using namespace dxmt9::d3d9::pe;
  PeAllFamilySemanticTokenLedger ledger{};
  const PeSemanticByteRange range{.offset = 24u, .length = 8u};
  check(ledger.beginIdentityProjection(3u, 7u, range),
        "identity projection binds source/record/range first");
  check(ledger.observeIdentity({
            .sourceOrdinal = 3u,
            .recordOrdinal = 7u,
            .recordWireRange = range,
            .identityOrdinal = 0u,
            .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
            .generation = 9u,
            .objectId = 0x12345678u,
        }) && ledger.finishIdentityProjection(1u),
        "kind/generation/object identity is consumed exactly");
  check(ledger.beginIdentityProjection(3u, 8u, range),
        "second identity projection begins independently");
  check(!ledger.observeIdentity({
            .sourceOrdinal = 3u,
            .recordOrdinal = 8u,
            .recordWireRange = range,
            .identityOrdinal = 0u,
            .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
            .generation = 0u,
            .objectId = 0x12345678u,
        }) && !ledger.finishIdentityProjection(1u),
        "stale zero-generation identity is an executable counterexample");
}

}  // namespace

int main() {
  try {
    exactProjectionAndRetry();
    replacementAndDomainCapacity();
    everyDistinctScalarSlotIsRepresentable();
    exhaustiveHeterogeneousSettlementPredicate();
    typedEnvelopesRejectSameSizeSurrogates();
    everyProductionFamilyBindsExactToken();
    exhaustiveAllFamilyCounterexamples();
    allFamilyRetryPoisonAndDifferential();
    allFamilyLatestIssuanceBinding();
    exactQualifiedIdentityTokens();
    std::cout << "pe semantic projection spec: PASS\n";
  } catch (const Failure& failure) {
    std::cerr << "pe semantic projection spec: FAIL: " << failure.what()
              << '\n';
    return 1;
  }
  return 0;
}
