// Production decision helpers for retained Live bindings and inline-constant
// cadence accounting.  This is a bounded value-level proof, not a COM/Wine
// end-to-end test.

#include "d3d9_pe_append_preparation.hpp"

#include <array>
#include <cstdint>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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

constexpr std::size_t kStreamSlotCount = 17u;
constexpr std::size_t kLiveStreamCount = 16u;

struct StreamReuseState {
  void* owner = nullptr;
  std::array<void*, 2u> buffers{};
  std::array<void*, 2u> rawBuffers{};
  std::array<void*, kStreamSlotCount> bound{};
  std::array<std::uint32_t, kStreamSlotCount> offsets{};
  std::array<std::uint32_t, kStreamSlotCount> strides{};
  // This tracks Live slot multiplicity only; Recording owner references are
  // deliberately outside this bounded value model.
  std::array<unsigned, 2u> retainedRefs{};
  unsigned dirtyEffects = 0u;
  unsigned recordingEffects = 0u;
};

struct StreamReuseStep {
  std::size_t slot = 0u;
  void* candidate = nullptr;
  std::uint32_t offset = 0u;
  std::uint32_t stride = 0u;
  bool candidateValid = true;
  bool malformedRetainedWire = false;
  bool recording = false;
  bool writeAllowed = true;
  bool nullZero = false;
  std::string_view label;
};

enum class StreamReuseResult { Ok, InvalidCall, DeviceLost };

pe::PeSemanticRetainedBinding streamRetainedProof(
    const StreamReuseState& state, std::size_t slot,
    bool malformedWire) noexcept {
  pe::PeSemanticRetainedBinding retained{.ownerDevice = state.owner,
                                         .publicIdentity = state.bound[slot]};
  for (std::size_t index = 0u; index != state.buffers.size(); ++index) {
    if (state.bound[slot] != state.buffers[index]) continue;
    retained.wire = validWire(D9C_CHUNK_HANDLE_KIND_BUFFER,
                               state.rawBuffers[index], 1u,
                               0x100u + index);
    break;
  }
  if (malformedWire) {
    retained.wire.identity.generation = 0u;
  }
  return retained;
}

void releaseRetained(StreamReuseState& state, void* prior) noexcept {
  if (prior == nullptr) return;
  for (std::size_t index = 0u; index != state.buffers.size(); ++index) {
    if (prior == state.buffers[index] && state.retainedRefs[index] != 0u) {
      --state.retainedRefs[index];
    }
  }
}

std::size_t bufferIndex(const StreamReuseState& state,
                        void* candidate) noexcept {
  for (std::size_t index = 0u; index != state.buffers.size(); ++index) {
    if (candidate == state.buffers[index]) return index;
  }
  return state.buffers.size();
}

StreamReuseResult applyAlwaysValidate(StreamReuseState& state,
                                      const StreamReuseStep& step) noexcept {
  if (!step.writeAllowed) return StreamReuseResult::DeviceLost;
  if (step.slot >= kLiveStreamCount) return StreamReuseResult::InvalidCall;
  // Full validation precedes the Recording branch. An invalid/foreign
  // candidate therefore has no dirty or Recording effect.
  if (!step.candidateValid) return StreamReuseResult::InvalidCall;
  if (step.recording) {
    ++state.dirtyEffects;
    ++state.recordingEffects;
    return StreamReuseResult::Ok;
  }

  void* const prior = state.bound[step.slot];
  const bool sameTuple = state.offsets[step.slot] == step.offset &&
                         state.strides[step.slot] == step.stride;
  if (step.candidate == prior && sameTuple) {
    return StreamReuseResult::Ok;
  }
  if (step.candidate == nullptr && prior == nullptr && step.nullZero) {
    return StreamReuseResult::Ok;
  }

  if (step.candidate == nullptr) {
    releaseRetained(state, prior);
    state.bound[step.slot] = nullptr;
    if (!step.nullZero) {
      state.offsets[step.slot] = step.offset;
      state.strides[step.slot] = step.stride;
    }
    ++state.dirtyEffects;
    return StreamReuseResult::Ok;
  }

  const std::size_t candidateIndex = bufferIndex(state, step.candidate);
  if (candidateIndex == state.buffers.size()) {
    return StreamReuseResult::InvalidCall;
  }
  if (step.candidate == prior) {
    state.offsets[step.slot] = step.offset;
    state.strides[step.slot] = step.stride;
    ++state.dirtyEffects;
    return StreamReuseResult::Ok;
  }
  releaseRetained(state, prior);
  state.bound[step.slot] = step.candidate;
  state.offsets[step.slot] = step.offset;
  state.strides[step.slot] = step.stride;
  ++state.retainedRefs[candidateIndex];
  ++state.dirtyEffects;
  return StreamReuseResult::Ok;
}

StreamReuseResult applyOptimized(StreamReuseState& state,
                                 const StreamReuseStep& step,
                                 bool* reused) noexcept {
  *reused = false;
  if (!step.writeAllowed) return StreamReuseResult::DeviceLost;
  if (step.slot >= kLiveStreamCount) return StreamReuseResult::InvalidCall;

  const auto retained =
      streamRetainedProof(state, step.slot, step.malformedRetainedWire);
  const bool guard = pe::peSemanticCanReuseRetainedBinding(
      step.writeAllowed, step.recording, step.candidate, retained,
      state.owner, D9C_CHUNK_HANDLE_KIND_BUFFER);
  if (!guard) {
    return applyAlwaysValidate(state, step);
  }

  *reused = true;
  // A retained stream hit has already passed Live/write/Recording checks.
  // Exact tuple is a no-op; a tuple-only change updates the cached wire's
  // metadata through bindStream without changing COM multiplicity.
  if (state.offsets[step.slot] == step.offset &&
      state.strides[step.slot] == step.stride) {
    return StreamReuseResult::Ok;
  }
  state.offsets[step.slot] = step.offset;
  state.strides[step.slot] = step.stride;
  ++state.dirtyEffects;
  return StreamReuseResult::Ok;
}

void checkStreamStatesEqual(const StreamReuseState& always,
                            const StreamReuseState& optimized,
                            std::string_view label) {
  check(always.bound == optimized.bound,
        std::string("stream bound differs: ") + std::string(label));
  check(always.offsets == optimized.offsets &&
            always.strides == optimized.strides,
        std::string("stream tuple differs: ") + std::string(label));
  check(always.retainedRefs == optimized.retainedRefs,
        std::string("stream retained multiplicity differs: ") +
            std::string(label));
  check(always.dirtyEffects == optimized.dirtyEffects &&
            always.recordingEffects == optimized.recordingEffects,
        std::string("stream effects differ: ") + std::string(label));
}

void runStreamDifferential(StreamReuseState& always,
                           StreamReuseState& optimized,
                           const StreamReuseStep& step) {
  const void* boundBefore =
      step.slot < kLiveStreamCount ? optimized.bound[step.slot] : nullptr;
  bool reused = false;
  const auto optimizedResult = applyOptimized(optimized, step, &reused);
  const auto alwaysResult = applyAlwaysValidate(always, step);
  check(optimizedResult == alwaysResult,
        std::string("stream result differs: ") + std::string(step.label));
  checkStreamStatesEqual(always, optimized, step.label);
  const bool expectedReuse =
      step.writeAllowed && step.slot < kLiveStreamCount &&
      step.candidateValid && !step.recording && step.candidate != nullptr &&
      step.candidate == boundBefore &&
      !step.malformedRetainedWire;
  check(reused == expectedReuse,
        std::string("stream retained-guard decision differs: ") +
            std::string(step.label));
}

StreamReuseState emptyStreamState(void* owner, void* bufferA, void* bufferB,
                                  void* rawA, void* rawB) noexcept {
  return StreamReuseState{
      .owner = owner,
      .buffers = {bufferA, bufferB},
      .rawBuffers = {rawA, rawB},
  };
}

void retainedStreamBindingTupleDecisionTruthTable() {
  int owner = 0;
  int bufferA = 0;
  int bufferB = 0;
  int foreign = 0;
  int rawA = 0;
  int rawB = 0;
  const auto initial = emptyStreamState(&owner, &bufferA, &bufferB, &rawA,
                                        &rawB);
  StreamReuseState always = initial;
  StreamReuseState optimized = initial;
  const std::array steps{
      StreamReuseStep{0u, &bufferA, 4u, 12u, true, false, false, true,
                      false, "slot 0 initial A"},
      StreamReuseStep{1u, &bufferB, 8u, 16u, true, false, false, true,
                      false, "slot 1 initial B"},
      StreamReuseStep{0u, &bufferA, 4u, 12u, true, false, false, true,
                      false, "slot 0 exact A"},
      StreamReuseStep{0u, &bufferA, 8u, 12u, true, false, false, true,
                      false, "slot 0 changed offset"},
      StreamReuseStep{0u, &bufferA, 8u, 16u, true, false, false, true,
                      false, "slot 0 changed stride"},
      StreamReuseStep{0u, &bufferB, 8u, 16u, true, false, false, true,
                      false, "slot 0 replacement B"},
      StreamReuseStep{0u, &bufferA, 8u, 16u, true, false, false, true,
                      false, "slot 0 A/B/A"},
      StreamReuseStep{0u, nullptr, 0u, 0u, true, false, false, true, true,
                      "slot 0 null zero preserves tuple"},
      StreamReuseStep{0u, nullptr, 0u, 0u, true, false, false, true, true,
                      "slot 0 repeated null zero no-op"},
      StreamReuseStep{0u, nullptr, 3u, 5u, true, false, false, true, false,
                      "slot 0 null nonzero stores tuple"},
      StreamReuseStep{0u, nullptr, 3u, 5u, true, false, false, true, false,
                      "slot 0 repeated null nonzero no-op"},
      StreamReuseStep{0u, &bufferA, 3u, 5u, true, false, false, true, false,
                      "slot 0 rebind after null"},
      StreamReuseStep{0u, &bufferA, 3u, 5u, true, false, true, true, false,
                      "recording exact A does not touch Live"},
      StreamReuseStep{0u, &foreign, 3u, 5u, false, false, true, true, false,
                      "invalid candidate rejected before Recording"},
      StreamReuseStep{0u, &foreign, 3u, 5u, false, false, false, true, false,
                      "foreign replacement falls through"},
      StreamReuseStep{0u, &bufferA, 3u, 5u, true, true, false, true, false,
                      "malformed retained wire falls through to validation"},
      StreamReuseStep{0u, &bufferA, 3u, 5u, true, false, false, false, false,
                      "write denied"},
      StreamReuseStep{16u, &bufferA, 3u, 5u, true, false, false, true, false,
                      "slot 16 rejected"},
  };
  for (const auto& step : steps) {
    runStreamDifferential(always, optimized, step);
    if (step.nullZero && step.slot == 0u) {
      check(always.bound[0] == nullptr && always.offsets[0] == 8u &&
                always.strides[0] == 16u,
            "null (0,0) detaches without losing the cached tuple");
    }
  }

  check(always.bound[0] == &bufferA && always.bound[1] == &bufferB,
        "A/B/A keeps both Live stream bindings");
  check(always.retainedRefs == std::array<unsigned, 2u>{1u, 1u},
        "two stream slots retain exactly one reference per live buffer");
  check(always.recordingEffects == 1u,
        "valid Recording update has no Live reference effect");

  // One-step matrix over reachable Live bindings. The optimized transition
  // always uses the production guard, while the reference always validates
  // the public candidate first; their complete state must agree on every row.
  const std::array<void*, 3u> bindings{
      nullptr, static_cast<void*>(&bufferA), static_cast<void*>(&bufferB)};
  const std::array tuples{
      std::pair<std::uint32_t, std::uint32_t>{0u, 0u},
      std::pair<std::uint32_t, std::uint32_t>{4u, 12u},
      std::pair<std::uint32_t, std::uint32_t>{8u, 16u},
  };
  const std::array<void*, 4u> candidates{
      static_cast<void*>(&bufferA), static_cast<void*>(&bufferB), nullptr,
      static_cast<void*>(&foreign)};
  for (void* binding : bindings) {
    for (const auto [oldOffset, oldStride] : tuples) {
      for (void* candidate : candidates) {
        for (const auto [offset, stride] : tuples) {
          for (bool recording : {false, true}) {
            for (bool writeAllowed : {false, true}) {
              StreamReuseState matrixAlways =
                  emptyStreamState(&owner, &bufferA, &bufferB, &rawA, &rawB);
              matrixAlways.bound[0] = binding;
              matrixAlways.offsets[0] = oldOffset;
              matrixAlways.strides[0] = oldStride;
              const auto oldIndex = bufferIndex(matrixAlways, binding);
              if (oldIndex != matrixAlways.buffers.size()) {
                matrixAlways.retainedRefs[oldIndex] = 1u;
              }
              StreamReuseState matrixOptimized = matrixAlways;
              const StreamReuseStep step{
                  0u, candidate, offset, stride, candidate != &foreign,
                  binding != nullptr && candidate == binding &&
                      oldIndex != matrixAlways.buffers.size() &&
                      offset == oldOffset && stride == oldStride &&
                      oldOffset == 4u,
                  recording, writeAllowed, candidate == nullptr && offset == 0u &&
                      stride == 0u, "one-step stream matrix"};
              runStreamDifferential(matrixAlways, matrixOptimized, step);
            }
          }
        }
      }
    }
  }
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
    retainedStreamBindingTupleDecisionTruthTable();
    inlineConstantCadenceDecisionTruthTable();
  } catch (const Failure& failure) {
    std::cerr << failure.what() << '\n';
    return 1;
  }
  return 0;
}
