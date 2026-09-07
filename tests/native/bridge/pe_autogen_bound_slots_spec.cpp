#include "d3d9_pe_autogen_mipmap.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

using dxmt9::d3d9::pe::AutogenMipmapEvent;
using dxmt9::d3d9::pe::AutogenMipmapState;
using dxmt9::d3d9::pe::BoundAutogenTextureSlots;
using dxmt9::d3d9::pe::transitionAutogenMipmap;

constexpr std::size_t kSlots = 3;
constexpr std::size_t kResources = 4;

enum class Resource : std::uint8_t {
  Null,
  NonAutogen,
  AutogenA,
  AutogenB,
};

constexpr std::array<Resource, kResources> kResourceValues = {
    Resource::Null,
    Resource::NonAutogen,
    Resource::AutogenA,
    Resource::AutogenB,
};

[[nodiscard]] constexpr bool isAutogen(Resource resource) {
  return resource == Resource::AutogenA || resource == Resource::AutogenB;
}

[[nodiscard]] constexpr std::size_t resourceIndex(Resource resource) {
  return static_cast<std::size_t>(resource);
}

[[nodiscard]] constexpr Resource resourceFrom(std::uint32_t value) {
  return kResourceValues[value % kResources];
}

[[nodiscard]] constexpr std::uint32_t bit(std::size_t slot) {
  return std::uint32_t{1u} << slot;
}

[[nodiscard]] std::uint32_t denseMask(
    const std::array<bool, BoundAutogenTextureSlots::kSlotCount>& enabled) {
  std::uint32_t mask = 0;
  for (std::size_t slot = 0;
       slot < BoundAutogenTextureSlots::kSlotCount; ++slot) {
    if (enabled[slot]) mask |= bit(slot);
  }
  return mask;
}

struct Model {
  BoundAutogenTextureSlots actual;
  std::array<bool, BoundAutogenTextureSlots::kSlotCount> expected{};
  std::array<Resource, kSlots> live{};
  std::array<Resource, kSlots> snapshot{};
  std::array<bool, kSlots> recorded{};
  std::array<AutogenMipmapState, kResources> state{};
  bool recording = false;
  bool terminal = false;

  Model() {
    live.fill(Resource::Null);
    snapshot.fill(Resource::Null);
    state[resourceIndex(Resource::Null)] = {false, false};
    state[resourceIndex(Resource::NonAutogen)] = {false, false};
    state[resourceIndex(Resource::AutogenA)] = {true, false};
    state[resourceIndex(Resource::AutogenB)] = {true, false};
  }

  void setTexture(std::uint32_t slot, Resource resource, bool valid) {
    if (!valid || slot >= kSlots) return;
    if (recording) {
      snapshot[slot] = resource;
      recorded[slot] = true;
      return;
    }
    live[slot] = resource;
    actual.publish(slot, isAutogen(resource));
    expected[slot] = isAutogen(resource);
  }

  void beginRecording() {
    snapshot = live;
    recorded.fill(false);
    recording = true;
  }

  void apply(int failAtSlot) {
    // Apply visits every recorded row even after one callback reports a
    // validation failure. Each row takes/replaces first; the failed row keeps
    // that attempted Live binding but does not republish eligibility. The
    // terminal poison is published after the category traversal completes.
    bool validationFailed = false;
    for (std::size_t slot = 0; slot < kSlots; ++slot) {
      if (!recorded[slot]) continue;
      actual.publish(slot, false);
      expected[slot] = false;
      live[slot] = snapshot[slot];
      if (static_cast<int>(slot) == failAtSlot) {
        validationFailed = true;
        continue;
      }
      actual.publish(slot, isAutogen(live[slot]));
      expected[slot] = isAutogen(live[slot]);
    }
    if (validationFailed) terminal = true;
    recording = false;
  }

  void reset() {
    actual.clear();
    expected.fill(false);
    live.fill(Resource::Null);
    snapshot.fill(Resource::Null);
    recorded.fill(false);
    recording = false;
    terminal = false;
  }

  void event(Resource resource, AutogenMipmapEvent event) {
    const auto index = resourceIndex(resource);
    state[index] = transitionAutogenMipmap(state[index], event);
  }
};

struct DrawObservation {
  std::array<Resource, kSlots> resources{};
  std::array<std::uint8_t, kSlots> sourceSlots{};
  std::size_t count = 0;
  std::size_t rttiCalls = 0;
  bool arraysTouched = false;
  bool failed = false;
  bool terminal = false;
  std::uint32_t touchedMask = 0;
};

struct Candidate {
  Resource resource = Resource::Null;
  std::uint8_t sourceSlot = 0;
};

// This is deliberately separate from the production-shaped path below: the
// oracle scans Live slots directly and clears dirty directly.  The path under
// test uses the real mask helper and transition function.
DrawObservation drawDenseReference(Model& model, int failAt,
                                   bool terminalOnFailure) {
  DrawObservation observation;
  observation.terminal = model.terminal;
  if (model.terminal) return observation;
  observation.arraysTouched = true;

  std::array<Candidate, kSlots> candidates{};
  std::size_t candidateCount = 0;
  std::array<bool, kResources> seen{};
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    const auto resource = model.live[slot];
    const auto index = resourceIndex(resource);
    if (resource == Resource::Null) continue;
    ++observation.rttiCalls;
    if (!isAutogen(resource) || !model.state[index].generationRequired())
      continue;
    if (seen[index]) continue;
    seen[index] = true;
    candidates[candidateCount++] = {
        resource, static_cast<std::uint8_t>(slot)};
    observation.touchedMask |= bit(slot);
  }
  // Gathering/deduplication is complete before any append or Clean mutation.
  for (std::size_t candidate = 0; candidate < candidateCount; ++candidate) {
    const auto resource = candidates[candidate].resource;
    const auto sourceSlot = candidates[candidate].sourceSlot;
    if (static_cast<int>(sourceSlot) == failAt) {
      observation.failed = true;
      if (terminalOnFailure) {
        model.terminal = true;
        observation.terminal = true;
      }
      return observation;
    }
    observation.resources[observation.count] = resource;
    observation.sourceSlots[observation.count++] = sourceSlot;
    // The dense oracle intentionally spells out the semantic result instead
    // of calling transitionAutogenMipmap.
    model.state[resourceIndex(resource)].dirty = false;
  }
  observation.terminal = model.terminal;
  return observation;
}

DrawObservation drawWithProductionPrimitives(Model& model, int failAt,
                                             bool terminalOnFailure) {
  DrawObservation observation;
  observation.terminal = model.terminal;
  if (model.terminal) return observation;
  const auto mask = model.actual.mask();
  if (mask == 0) return observation;
  observation.arraysTouched = true;

  std::array<Candidate, kSlots> candidates{};
  std::size_t candidateCount = 0;
  std::array<bool, kResources> seen{};
  for (std::size_t slot = 0; slot < kSlots; ++slot) {
    if ((mask & bit(slot)) == 0) continue;
    const auto resource = model.live[slot];
    const auto index = resourceIndex(resource);
    if (resource == Resource::Null) continue;
    ++observation.rttiCalls;
    if (!isAutogen(resource) || !model.state[index].generationRequired())
      continue;
    if (seen[index]) continue;
    seen[index] = true;
    candidates[candidateCount++] = {
        resource, static_cast<std::uint8_t>(slot)};
    observation.touchedMask |= bit(slot);
  }
  // Match the producer boundary: all candidates are gathered and deduped
  // before the first append can settle any resource.
  for (std::size_t candidate = 0; candidate < candidateCount; ++candidate) {
    const auto resource = candidates[candidate].resource;
    const auto sourceSlot = candidates[candidate].sourceSlot;
    if (static_cast<int>(sourceSlot) == failAt) {
      observation.failed = true;
      if (terminalOnFailure) {
        model.terminal = true;
        observation.terminal = true;
      }
      return observation;
    }
    observation.resources[observation.count] = resource;
    observation.sourceSlots[observation.count++] = sourceSlot;
    model.state[resourceIndex(resource)] = transitionAutogenMipmap(
        model.state[resourceIndex(resource)],
        AutogenMipmapEvent::GenerationSucceeded);
  }
  observation.terminal = model.terminal;
  return observation;
}

[[nodiscard]] bool sameObservation(const DrawObservation& lhs,
                                   const DrawObservation& rhs) {
  if (lhs.count != rhs.count || lhs.failed != rhs.failed ||
      lhs.terminal != rhs.terminal ||
      lhs.touchedMask != rhs.touchedMask)
    return false;
  for (std::size_t i = 0; i < lhs.count; ++i) {
    if (lhs.resources[i] != rhs.resources[i] ||
        lhs.sourceSlots[i] != rhs.sourceSlots[i])
      return false;
  }
  return true;
}

[[nodiscard]] bool sameModelSemantics(const Model& lhs, const Model& rhs) {
  if (lhs.live != rhs.live || lhs.snapshot != rhs.snapshot ||
      lhs.recorded != rhs.recorded || lhs.recording != rhs.recording ||
      lhs.terminal != rhs.terminal)
    return false;
  for (std::size_t slot = 0;
       slot < BoundAutogenTextureSlots::kSlotCount; ++slot) {
    if (lhs.expected[slot] != rhs.expected[slot]) return false;
  }
  for (std::size_t resource = 0; resource < kResources; ++resource) {
    if (lhs.state[resource].enabled != rhs.state[resource].enabled ||
        lhs.state[resource].dirty != rhs.state[resource].dirty)
      return false;
  }
  if (lhs.actual.mask() != rhs.actual.mask()) return false;
  return true;
}

bool require(bool condition, const char* expression, const char* context) {
  if (condition) return true;
  std::fprintf(stderr, "pe-autogen-bound-slots: %s (%s)\n", expression,
               context);
  return false;
}

#define CHECK(condition, context) \
  do {                               \
    if (!require((condition), #condition, (context))) return false; \
  } while (false)

Model makeBase(std::uint32_t liveCode, std::uint32_t snapshotCode,
               std::uint32_t dirtyBits) {
  Model model;
  for (std::size_t resource = resourceIndex(Resource::AutogenA);
       resource <= resourceIndex(Resource::AutogenB); ++resource) {
    if ((dirtyBits & bit(resource - resourceIndex(Resource::AutogenA))) != 0)
      model.state[resource] = transitionAutogenMipmap(
          model.state[resource], AutogenMipmapEvent::RenderTargetWrite);
  }
  for (std::size_t slot = 0; slot < kSlots; ++slot)
    model.setTexture(slot, resourceFrom(liveCode >> (2 * slot)), true);
  model.beginRecording();
  for (std::size_t slot = 0; slot < kSlots; ++slot)
    model.setTexture(slot, resourceFrom(snapshotCode >> (2 * slot)), true);
  return model;
}

Model makeLive(std::uint32_t liveCode, std::uint32_t dirtyBits) {
  Model model;
  for (std::size_t resource = resourceIndex(Resource::AutogenA);
       resource <= resourceIndex(Resource::AutogenB); ++resource) {
    if ((dirtyBits & bit(resource - resourceIndex(Resource::AutogenA))) != 0)
      model.state[resource] = transitionAutogenMipmap(
          model.state[resource], AutogenMipmapEvent::RenderTargetWrite);
  }
  for (std::size_t slot = 0; slot < kSlots; ++slot)
    model.setTexture(slot, resourceFrom(liveCode >> (2 * slot)), true);
  return model;
}

bool testTransitionTruthTable(std::size_t& cases) {
  constexpr std::array<AutogenMipmapEvent, 3> events = {
      AutogenMipmapEvent::RenderTargetWrite,
      AutogenMipmapEvent::GenerationSucceeded,
      AutogenMipmapEvent::GenerationFailed,
  };
  for (bool enabled : {false, true}) {
    for (bool dirty : {false, true}) {
      for (const auto event : events) {
        const auto before = AutogenMipmapState{enabled, dirty};
        const auto after = transitionAutogenMipmap(before, event);
        bool expectedDirty = dirty;
        if (enabled) {
          expectedDirty = event != AutogenMipmapEvent::GenerationSucceeded;
          if (event == AutogenMipmapEvent::RenderTargetWrite)
            expectedDirty = true;
        }
        CHECK(after.enabled == enabled && after.dirty == expectedDirty,
              "transition truth table");
        ++cases;
      }
    }
  }
  return true;
}

bool testBoundMaskLaws(std::size_t& cases) {
  for (std::uint32_t seed = 0; seed < (1u << kSlots); ++seed) {
    BoundAutogenTextureSlots actual;
    std::array<bool, BoundAutogenTextureSlots::kSlotCount> expected{};
    for (std::size_t slot = 0; slot < kSlots; ++slot) {
      const bool enabled = (seed & bit(slot)) != 0;
      actual.publish(slot, enabled);
      expected[slot] = enabled;
      CHECK(actual.mask() == denseMask(expected), "small dense mask law");
      ++cases;
    }
    for (std::size_t slot = 0; slot < kSlots; ++slot) {
      actual.publish(slot, false);
      expected[slot] = false;
      CHECK(actual.mask() == denseMask(expected), "small clear law");
      ++cases;
    }
  }

  BoundAutogenTextureSlots all;
  for (std::size_t slot = 0;
       slot < BoundAutogenTextureSlots::kSlotCount; ++slot) {
    all.publish(static_cast<std::uint32_t>(slot), true);
    CHECK(all.mask() == bit(slot), "all-position set law");
    all.publish(static_cast<std::uint32_t>(slot), false);
    CHECK(all.mask() == 0, "all-position clear law");
    ++cases;
  }
  for (std::size_t slot = 0;
       slot < BoundAutogenTextureSlots::kSlotCount; ++slot)
    all.publish(static_cast<std::uint32_t>(slot), true);
  CHECK(all.mask() == ((std::uint32_t{1u} << 20) - 1u),
        "twenty-position low mask");
  ++cases;
  const auto beforeInvalid = all.mask();
  all.publish(BoundAutogenTextureSlots::kSlotCount, false);
  all.publish(std::numeric_limits<std::uint32_t>::max(), true);
  CHECK(all.mask() == beforeInvalid, "invalid slots are no-op");
  ++cases;
  CHECK((all.mask() & ~((std::uint32_t{1u} << 20) - 1u)) == 0,
        "no high bits");
  ++cases;
  all.clear();
  CHECK(all.mask() == 0, "clear law");
  ++cases;
  return true;
}

bool compareDraw(Model model, int failAt, bool terminalOnFailure,
                 const char* context) {
  CHECK(model.actual.mask() == denseMask(model.expected),
        "production/reference masks agree");
  auto actual = model;
  auto reference = model;
  const auto actualResult =
      drawWithProductionPrimitives(actual, failAt, terminalOnFailure);
  const auto referenceResult =
      drawDenseReference(reference, failAt, terminalOnFailure);
  const auto expectedMask = denseMask(model.expected);
  if (model.terminal || expectedMask == 0) {
    CHECK(!actualResult.arraysTouched && actualResult.rttiCalls == 0,
          "terminal/empty draw returns before arrays and RTTI");
  } else {
    CHECK(actualResult.arraysTouched, "nonempty draw touches arrays");
    std::size_t expectedRttiCalls = 0;
    for (std::size_t slot = 0; slot < kSlots; ++slot) {
      if ((expectedMask & bit(slot)) != 0 &&
          model.live[slot] != Resource::Null)
        ++expectedRttiCalls;
    }
    CHECK(actualResult.rttiCalls == expectedRttiCalls,
          "nonempty draw RTTI checks every bound slot");
  }
  CHECK(sameObservation(actualResult, referenceResult), context);
  CHECK(sameModelSemantics(actual, reference), context);
  CHECK(actual.actual.mask() == denseMask(actual.expected), context);
  return true;
}

bool testStateSpace(std::size_t& cases) {
  constexpr std::uint32_t kBindingValues = 1u << (2 * kSlots);
  for (std::uint32_t liveCode = 0; liveCode < kBindingValues; ++liveCode) {
    for (std::uint32_t snapshotCode = 0; snapshotCode < kBindingValues;
         ++snapshotCode) {
      for (std::uint32_t dirtyBits = 0; dirtyBits < (1u << 2);
           ++dirtyBits) {
        const auto base = makeBase(liveCode, snapshotCode, dirtyBits);
        CHECK(base.actual.mask() == denseMask(base.expected),
              "base live publication");
        ++cases;

        // Recording writes are snapshot-only and cannot alter live eligibility.
        for (std::size_t slot = 0; slot < kSlots; ++slot) {
          const auto failed = base;
          auto attempted = failed;
          attempted.setTexture(static_cast<std::uint32_t>(slot),
                               resourceFrom((snapshotCode + slot + 1) & 3u),
                               false);
          CHECK(sameModelSemantics(attempted, failed),
                "failed setter preserves live and snapshot");
          ++cases;
        }

        // The same failed-validation guarantee applies to ordinary Live
        // binding (and to an out-of-range slot), not only to a recording.
        auto liveFailed = makeLive(liveCode, dirtyBits);
        const auto liveBeforeFailure = liveFailed;
        for (std::size_t slot = 0; slot < kSlots; ++slot) {
          liveFailed.setTexture(static_cast<std::uint32_t>(slot),
                                Resource::AutogenB, false);
          CHECK(sameModelSemantics(liveFailed, liveBeforeFailure),
                "failed live setter preserves ownership");
          ++cases;
        }
        liveFailed.setTexture(static_cast<std::uint32_t>(kSlots),
                              Resource::AutogenA, true);
        CHECK(sameModelSemantics(liveFailed, liveBeforeFailure),
              "out-of-range binding preserves ownership");
        ++cases;

        auto applied = base;
        applied.apply(-1);
        CHECK(applied.actual.mask() == denseMask(applied.expected),
              "successful apply publication");
        for (std::size_t slot = 0; slot < kSlots; ++slot)
          CHECK(applied.live[slot] ==
                    resourceFrom(snapshotCode >> (2 * slot)),
                "successful apply binding");
        ++cases;

        for (std::size_t failSlot = 0; failSlot < kSlots; ++failSlot) {
          auto rejected = base;
          const auto recordedSnapshot = rejected.snapshot;
          rejected.apply(static_cast<int>(failSlot));
          CHECK(rejected.terminal && !rejected.recording,
                "failed apply terminal policy");
          CHECK(rejected.actual.mask() == denseMask(rejected.expected),
                "failed apply eligibility publication");
          for (std::size_t slot = 0; slot < kSlots; ++slot) {
            CHECK(rejected.live[slot] == recordedSnapshot[slot],
                  "apply visits suffix after callback failure");
            if (slot == failSlot) {
              CHECK(!rejected.expected[slot],
                    "failed apply suppresses failing eligibility");
            } else {
              CHECK(rejected.expected[slot] ==
                        isAutogen(recordedSnapshot[slot]),
                    "successful apply republishes visited row");
            }
          }
          ++cases;
        }

        auto reset = base;
        reset.reset();
        CHECK(reset.actual.mask() == 0 && denseMask(reset.expected) == 0,
              "reset clears eligibility before release");
        ++cases;
      }
    }
  }
  return true;
}

bool testDrawSpace(std::size_t& cases) {
  constexpr std::uint32_t kBindingValues = 1u << (2 * kSlots);
  constexpr std::array<int, 7> failureModes = {-1, 0, 1, 2, 0, 1, 2};
  constexpr std::array<bool, 7> terminalModes = {
      false, false, false, false, true, true, true};
  for (std::uint32_t liveCode = 0; liveCode < kBindingValues; ++liveCode) {
    for (std::uint32_t dirtyBits = 0; dirtyBits < (1u << 2); ++dirtyBits) {
      for (std::size_t mode = 0; mode < failureModes.size(); ++mode) {
        CHECK(compareDraw(makeLive(liveCode, dirtyBits), failureModes[mode],
                          terminalModes[mode], "draw dense refinement"),
              "draw dense refinement");
        ++cases;
      }
    }
  }

  // A failed append accepts and cleans the prefix only.  The retry then
  // accepts the still-dirty suffix; the terminal variant records poison.
  Model retry = makeLive(0, 0);
  retry.setTexture(0, Resource::AutogenA, true);
  retry.setTexture(1, Resource::AutogenB, true);
  retry.setTexture(2, Resource::AutogenA, true);
  retry.event(Resource::AutogenA, AutogenMipmapEvent::RenderTargetWrite);
  retry.event(Resource::AutogenB, AutogenMipmapEvent::RenderTargetWrite);
  auto firstActual = retry;
  auto firstReference = retry;
  const auto failedActual = drawWithProductionPrimitives(firstActual, 1, false);
  const auto failedReference = drawDenseReference(firstReference, 1, false);
  CHECK(sameObservation(failedActual, failedReference),
        "append failure prefix observation");
  CHECK(firstActual.state[resourceIndex(Resource::AutogenA)].dirty == false &&
            firstActual.state[resourceIndex(Resource::AutogenB)].dirty,
        "append failure prefix clean ordering");
  CHECK(sameModelSemantics(firstActual, firstReference),
        "append failure prefix state");
  ++cases;
  auto retryActual = firstActual;
  auto retryReference = firstReference;
  const auto retriedActual =
      drawWithProductionPrimitives(retryActual, -1, false);
  const auto retriedReference =
      drawDenseReference(retryReference, -1, false);
  CHECK(sameObservation(retriedActual, retriedReference),
        "append retry observation");
  CHECK(!retryActual.state[resourceIndex(Resource::AutogenB)].dirty,
        "append retry settles suffix");
  CHECK(sameModelSemantics(retryActual, retryReference),
        "append retry state");
  ++cases;

  auto terminalActual = retry;
  auto terminalReference = retry;
  const auto terminalResult =
      drawWithProductionPrimitives(terminalActual, 1, true);
  const auto terminalOracle =
      drawDenseReference(terminalReference, 1, true);
  CHECK(terminalResult.failed && terminalActual.terminal &&
            sameObservation(terminalResult, terminalOracle) &&
            sameModelSemantics(terminalActual, terminalReference),
        "post-accept terminal failure");
  ++cases;
  return true;
}

bool testExplicitMutants(std::size_t& cases, std::size_t& mutants) {
  // Omitted-republication mutant: a null Live slot receives a recorded dirty
  // autogen texture, and successful Apply must publish its immutable bit.
  Model stale = makeLive(0, 0);
  stale.beginRecording();
  stale.setTexture(0, Resource::AutogenA, true);
  stale.event(Resource::AutogenA,
              AutogenMipmapEvent::RenderTargetWrite);
  stale.apply(-1);
  CHECK((stale.actual.mask() & bit(0)) != 0 && stale.expected[0],
        "successful apply republishes autogen bit");
  ++cases;

  auto staleMutant = stale;
  staleMutant.actual.publish(0, false);
  auto staleReference = stale;
  const auto mutantResult =
      drawWithProductionPrimitives(staleMutant, -1, false);
  const auto referenceResult =
      drawDenseReference(staleReference, -1, false);
  CHECK(mutantResult.count != referenceResult.count &&
            referenceResult.count == 1 &&
            referenceResult.resources[0] == Resource::AutogenA &&
            referenceResult.sourceSlots[0] == 0,
        "omitted Apply republication mutant has counterexample");
  ++mutants;

  // Dirty-at-bind mutant: eligibility is immutable metadata, so a later RT
  // write must become visible without rebinding.
  Model dirtyAfterBind;
  dirtyAfterBind.setTexture(0, Resource::AutogenA, true);
  dirtyAfterBind.event(Resource::AutogenA,
                       AutogenMipmapEvent::RenderTargetWrite);
  CHECK((dirtyAfterBind.actual.mask() & bit(0)) != 0,
        "dirty after bind retains metadata bit");
  CHECK(compareDraw(dirtyAfterBind, -1, false,
                    "dirty-at-bind current dirty read"),
        "dirty-at-bind current dirty read");
  ++cases;
  auto dirtyMutant = dirtyAfterBind;
  dirtyMutant.actual.publish(0, false);
  auto dirtyReference = dirtyAfterBind;
  const auto dirtyMutantResult =
      drawWithProductionPrimitives(dirtyMutant, -1, false);
  const auto dirtyReferenceResult =
      drawDenseReference(dirtyReference, -1, false);
  CHECK(dirtyMutantResult.count != dirtyReferenceResult.count,
        "dirty-at-bind mutant has counterexample");
  ++mutants;

  // The inverse transition (dirty at bind, then settled) must not leave a
  // cached dirty bit behind.
  Model cleanAfterBind;
  cleanAfterBind.event(Resource::AutogenA,
                       AutogenMipmapEvent::RenderTargetWrite);
  cleanAfterBind.setTexture(0, Resource::AutogenA, true);
  cleanAfterBind.event(Resource::AutogenA,
                       AutogenMipmapEvent::GenerationSucceeded);
  CHECK(compareDraw(cleanAfterBind, -1, false,
                    "clean-after-bind current dirty read"),
        "clean-after-bind current dirty read");
  ++cases;

  Model poisoned = makeLive(2u, 1u);
  poisoned.terminal = true;
  CHECK(compareDraw(poisoned, -1, false, "terminal prevents draw"),
        "terminal prevents draw");
  ++cases;
  return true;
}

} // namespace

int main() {
  std::size_t transitionCases = 0;
  std::size_t maskCases = 0;
  std::size_t stateCases = 0;
  std::size_t drawCases = 0;
  std::size_t explicitCases = 0;
  std::size_t mutantCases = 0;
  if (!testTransitionTruthTable(transitionCases) ||
      !testBoundMaskLaws(maskCases) ||
      !testStateSpace(stateCases) || !testDrawSpace(drawCases) ||
      !testExplicitMutants(explicitCases, mutantCases))
    return 1;
  std::printf(
      "pe-autogen-bound-slots: ok; transitions=%zu mask=%zu state=%zu "
      "draw=%zu explicit=%zu mutant-counterexamples=%zu (3 slots, 4 resources, "
      "64x64x4 state enumeration, 7 draw modes)\n",
      transitionCases, maskCases, stateCases, drawCases, explicitCases,
      mutantCases);
  return 0;
}
