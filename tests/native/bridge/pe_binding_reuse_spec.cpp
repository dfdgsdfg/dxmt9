// Production decision helpers for retained Live bindings and inline-constant
// cadence accounting.  This is a bounded value-level proof, not a COM/Wine
// end-to-end test.

#include "d3d9_pe_append_preparation.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace pe = dxmt9::d3d9::pe;

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw Failure(std::string(message));
}

pe::PeWireObjectRef validWire(std::uint32_t kind, void* raw,
                              std::uint32_t generation = 1u,
                              std::uint64_t objectId = 7u) noexcept {
  return pe::PeWireObjectRef{
      .identity = {.kind = kind, .generation = generation,
                   .objectId = objectId},
      .object = raw,
  };
}

// Independent value-level oracle for the setter's externally relevant
// decisions. This deliberately models slot multiplicity and dirty/record
// effects; it is not a COM implementation and performs no real AddRef or
// Release calls.
struct AlwaysValidateReference {
  void* owner = nullptr;
  void* a = nullptr;
  void* b = nullptr;
  void* rawA = nullptr;
  void* rawB = nullptr;
  void* bound = nullptr;
  std::array<unsigned, 2u> retainedMultiplicity{1u, 0u};
  unsigned dirtyEffects = 0u;
  unsigned recordEffects = 0u;
};

struct ReferenceStep {
  void* candidate = nullptr;
  bool valid = true;
  bool recording = false;
  bool writeAllowed = true;
  std::string_view label;
};

struct ReferenceResult {
  bool shortcut = false;
  bool hrSuccess = false;
};

ReferenceResult alwaysValidate(AlwaysValidateReference& state,
                              const ReferenceStep& step) noexcept {
  if (!step.writeAllowed || !step.valid) return {};
  if (step.recording) {
    ++state.dirtyEffects;
    ++state.recordEffects;
    return {.shortcut = false, .hrSuccess = true};
  }
  if (step.candidate == state.bound) {
    return {.shortcut = step.candidate != nullptr, .hrSuccess = true};
  }
  if (state.bound == state.a) {
    --state.retainedMultiplicity[0];
  } else if (state.bound == state.b) {
    --state.retainedMultiplicity[1];
  }
  state.bound = step.candidate;
  if (state.bound == state.a) {
    ++state.retainedMultiplicity[0];
  } else if (state.bound == state.b) {
    ++state.retainedMultiplicity[1];
  }
  ++state.dirtyEffects;
  return {.shortcut = false, .hrSuccess = true};
}

pe::PeSemanticRetainedBinding retainedProof(
    const AlwaysValidateReference& state) noexcept {
  if (state.bound == state.a) {
    return {.ownerDevice = state.owner,
            .publicIdentity = state.a,
            .wire = validWire(D9C_CHUNK_HANDLE_KIND_SHADER, state.rawA)};
  }
  if (state.bound == state.b) {
    return {.ownerDevice = state.owner,
            .publicIdentity = state.b,
            .wire = validWire(D9C_CHUNK_HANDLE_KIND_SHADER, state.rawB, 2u,
                              8u)};
  }
  return {.ownerDevice = state.owner};
}

void retainedBindingMatchesIndependentReference() {
  int owner = 0;
  int a = 0;
  int b = 0;
  int rawA = 0;
  int rawB = 0;
  AlwaysValidateReference reference{
      .owner = &owner, .a = &a, .b = &b, .rawA = &rawA, .rawB = &rawB,
      .bound = &a};
  constexpr auto kind = D9C_CHUNK_HANDLE_KIND_SHADER;
  const std::array steps{
      ReferenceStep{&a, true, false, true, "A"},
      ReferenceStep{&a, true, false, true, "A again"},
      ReferenceStep{&b, true, false, true, "B replacement"},
      ReferenceStep{&a, true, false, true, "A replacement"},
      ReferenceStep{nullptr, true, true, true, "Recording null"},
      ReferenceStep{nullptr, true, false, true, "Live null"},
      ReferenceStep{&b, false, false, true, "foreign"},
      ReferenceStep{&a, true, false, false, "write denied"},
  };
  for (const auto& step : steps) {
    const auto retained = retainedProof(reference);
    const bool actual = pe::peSemanticCanReuseRetainedBinding(
        step.writeAllowed, step.recording, step.candidate, retained,
        &owner, kind);
    const auto expected = alwaysValidate(reference, step);
    check(actual == expected.shortcut,
          std::string("shortcut differs from always-validate reference: ") +
              std::string(step.label));
    check(expected.hrSuccess == (step.writeAllowed && step.valid),
          "reference preserves HRESULT success/failure disposition");
    check(reference.retainedMultiplicity[0] +
              reference.retainedMultiplicity[1] <= 1u,
          "reference preserves one strong retained slot multiplicity");
  }
  check(reference.retainedMultiplicity == std::array<unsigned, 2u>{0u, 0u},
        "null unbind releases the retained slot in the reference");
  check(reference.dirtyEffects == 4u && reference.recordEffects == 1u,
        "reference preserves dirty and Recording effects");
}

void retainedBindingDecisionTruthTable() {
  int ownerA = 0;
  int ownerB = 0;
  int publicA = 0;
  int publicB = 0;
  int rawA = 0;
  int rawB = 0;
  const auto expectedKind = D9C_CHUNK_HANDLE_KIND_SHADER;
  pe::PeSemanticRetainedBinding retained{
      .ownerDevice = &ownerA,
      .publicIdentity = &publicA,
      .wire = validWire(expectedKind, &rawA),
  };

  check(pe::peSemanticCanReuseRetainedBinding(
            true, false, &publicA, retained, &ownerA, expectedKind),
        "same retained Live binding is reusable");
  check(!pe::peSemanticCanReuseRetainedBinding(
            true, false, nullptr, retained, &ownerA, expectedKind),
        "null binding remains on the full validation path");
  check(!pe::peSemanticCanReuseRetainedBinding(
            true, false, &publicB, retained, &ownerA, expectedKind),
        "replacement or foreign public identity is not reused");
  check(!pe::peSemanticCanReuseRetainedBinding(
            true, false, &publicA, retained, &ownerB, expectedKind),
        "foreign owner is not reused");
  check(!pe::peSemanticCanReuseRetainedBinding(
            true, false, &publicA, retained, &ownerA,
            D9C_CHUNK_HANDLE_KIND_BUFFER),
        "wrong wire kind is not reused");
  check(!pe::peSemanticCanReuseRetainedBinding(
            true, true, &publicA, retained, &ownerA, expectedKind),
        "StateBlock Recording excludes the Live shortcut");
  check(!pe::peSemanticCanReuseRetainedBinding(
            false, false, &publicA, retained, &ownerA, expectedKind),
        "Reset/failure write denial excludes the shortcut");

  retained.wire.identity.generation = 0u;
  check(!pe::peSemanticCanReuseRetainedBinding(
            true, false, &publicA, retained, &ownerA, expectedKind),
        "zero generation shape fails closed");
  retained.wire = validWire(expectedKind, &rawA);
  retained.wire.object = nullptr;
  check(!pe::peSemanticCanReuseRetainedBinding(
            true, false, &publicA, retained, &ownerA, expectedKind),
        "malformed retained wire fails closed");

  // A/B/A replacement keeps the exact multiplicity of the strong slot
  // external to this pure decision: repeated hits never acquire another ref.
  retained.wire = validWire(expectedKind, &rawA);
  for (unsigned repeat = 0u; repeat != 3u; ++repeat) {
    check(pe::peSemanticCanReuseRetainedBinding(
              true, false, &publicA, retained, &ownerA, expectedKind),
          "retained A remains reusable without another retain");
  }
  retained.publicIdentity = &publicB;
  retained.wire = validWire(expectedKind, &rawB, 2u, 8u);
  check(pe::peSemanticCanReuseRetainedBinding(
            true, false, &publicB, retained, &ownerA, expectedKind),
        "replacement B is reusable only after its own qualification");
  check(!pe::peSemanticCanReuseRetainedBinding(
            true, false, &publicA, retained, &ownerA, expectedKind),
        "old A cannot reuse a replaced slot");
  retained.publicIdentity = &publicA;
  retained.wire = validWire(expectedKind, &rawA, 3u, 9u);
  check(pe::peSemanticCanReuseRetainedBinding(
            true, false, &publicA, retained, &ownerA, expectedKind),
        "A/B/A rebind requires the current retained proof");
}

void inlineConstantCadenceDecisionTruthTable() {
  constexpr std::size_t stableCadenceBytes = 64u;
  unsigned queries = 0u;
  const auto query = [&]() noexcept -> std::size_t {
    ++queries;
    return 36u;
  };
  check(pe::peSemanticConstantPayloadBytes(false, query) == 0u,
        "inline constants OFF contribute zero cadence payload");
  check(queries == 0u, "inline constants OFF issue no six-range query");
  check(stableCadenceBytes +
            pe::peSemanticConstantPayloadBytes(false, query) ==
            stableCadenceBytes,
        "OFF cadence bytes remain the historical fixed value");
  const std::size_t enabledPayload =
      pe::peSemanticConstantPayloadBytes(true, query);
  check(enabledPayload == 36u,
        "inline constants ON retain exact payload accounting");
  check(queries == 1u, "one enabled cadence query reads the exact payload");
  check(stableCadenceBytes + enabledPayload == 100u,
        "ON cadence includes the exact queried payload");
}

}  // namespace

int main() {
  try {
    retainedBindingMatchesIndependentReference();
    retainedBindingDecisionTruthTable();
    inlineConstantCadenceDecisionTruthTable();
  } catch (const Failure& failure) {
    std::cerr << failure.what() << '\n';
    return 1;
  }
  return 0;
}
