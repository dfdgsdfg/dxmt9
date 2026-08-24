#include "d3d9_pe_semantic_tokens.hpp"

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

}  // namespace

int main() {
  try {
    exactProjectionAndRetry();
    replacementAndDomainCapacity();
    everyDistinctScalarSlotIsRepresentable();
    std::cout << "pe semantic projection spec: PASS\n";
  } catch (const Failure& failure) {
    std::cerr << "pe semantic projection spec: FAIL: " << failure.what()
              << '\n';
    return 1;
  }
  return 0;
}
