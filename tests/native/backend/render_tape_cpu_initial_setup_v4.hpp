#pragma once

#include "device_c_render_tape_capture_layout.hpp"
#include "device_c_render_tape_initial_setup_v4.hpp"
#include "device_c_render_tape_runtime_objects.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace dxmt9::test {

enum class CpuInitialSetupV4EventResult : std::uint8_t {
  NotManaged, Accepted, Rejected,
};

// These are host ownership/logical seed receipts only. They provide no GPU
// upload, initializer completion, attachment contents, or topology authority.
struct CpuInitialSetupV4Evidence {
  std::size_t owners = 0u, seeds = 0u, aliases = 0u;
  std::size_t created = 0u, prepared = 0u, activated = 0u;
  std::size_t stagedSeeds = 0u, consumedSeeds = 0u, releasedHidden = 0u;
  std::uint64_t seedBytes = 0u, allocationBytes = 0u;
  bool hostCheckpointJoined = false;
};

struct CpuInitialSetupV4ReleaseSink {
  // The owner of context must outlive this helper, including failure cleanup.
  void* context = nullptr;
  void (*release)(void*, d3d9::render_tape_runtime::OwnedObject&) noexcept = nullptr;
};

namespace cpu_initial_setup_v4_detail {

inline bool sameDefinition(const d3d9::RenderTapeObjectDefineHeader& a,
                           const d3d9::RenderTapeObjectDefineHeader& b) noexcept {
  return d3d9::renderTapeIdentityV3SameIdentity(a.identity, b.identity) &&
      a.descriptorKind == b.descriptorKind && a.descriptorBytes == b.descriptorBytes &&
      a.payloadValidity == b.payloadValidity && a.reserved0 == b.reserved0 &&
      a.immutablePayloadBytes == b.immutablePayloadBytes &&
      a.immutablePayloadDigest == b.immutablePayloadDigest &&
      a.expectedContentBytes == b.expectedContentBytes &&
      a.expectedContentCount == b.expectedContentCount && a.reserved1 == b.reserved1;
}

inline bool sameMutation(const d3d9::RenderTapeResourceMutationHeader& a,
                         const d3d9::RenderTapeResourceMutationHeader& b) noexcept {
  // The wire header has implicit padding before byteOffset: bind fields.
  return d3d9::renderTapeIdentityV3SameIdentity(a.identity, b.identity) &&
      a.kind == b.kind && a.subresource == b.subresource &&
      a.bufferDisposition == b.bufferDisposition && a.byteOffset == b.byteOffset &&
      a.byteSize == b.byteSize && a.digest == b.digest;
}

inline bool hasObject(std::span<const d3d9::render_tape_runtime::OwnedObject> objects,
                      const D9CWireObjectIdentity& identity) noexcept {
  return std::any_of(objects.begin(), objects.end(), [&](const auto& object) {
    return object.value && d3d9::renderTapeIdentityV3SameIdentity(object.identity, identity);
  });
}

} // namespace cpu_initial_setup_v4_detail

// Cold, single-owner harness ledger. Admission consumes an already authenticated
// pure setup plan; it never selects a source cut or invents an initial seed.
// Callbacks borrow synchronously. The helper does not call the queue, publish,
// issue queries, or retain mutation/blob spans. A completed EVENT's hidden owner
// and its live QueryPollSnapshot remain in the executor's separate query ledger.
class CpuInitialSetupV4 {
 public:
  using Plan = d3d9::RenderTapeInitialSetupV4Plan;
  using Object = d3d9::render_tape_runtime::OwnedObject;
  using Phase = d3d9::RenderTapeInitialSetupV4OwnerPhase;
  using Action = d3d9::RenderTapeInitialSetupV4Action;
  using EventResult = CpuInitialSetupV4EventResult;

  explicit CpuInitialSetupV4(CpuInitialSetupV4ReleaseSink release) noexcept
      : release_(release) {}
  CpuInitialSetupV4(const CpuInitialSetupV4&) = delete;
  CpuInitialSetupV4& operator=(const CpuInitialSetupV4&) = delete;
  CpuInitialSetupV4(CpuInitialSetupV4&&) = delete;
  CpuInitialSetupV4& operator=(CpuInitialSetupV4&&) = delete;
  ~CpuInitialSetupV4() { releaseHidden(); }

  bool adopt(Plan&& plan, const d3d9::RenderTapeDigest& sourceDigest,
             d3d9::RenderTapeInitialSetupV4Limits limits = {}) noexcept {
    if (admitted_ || closed_ || !release_.release || !plan.valid() ||
        plan.sourcePlanDigest() != sourceDigest ||
        plan.entries().size() > limits.maxObjects ||
        plan.aliases().size() > limits.maxObjects || plan.seeds().size() > limits.maxSeeds ||
        plan.totalSeedBytes() > limits.maxSeedBytes ||
        plan.totalAllocationBytes() > limits.maxAllocationBytes) return false;
    try {
      std::vector<Object> hidden(plan.entries().size());
      std::vector<bool> aliasesQualified(plan.aliases().size(), false);
      std::vector<d3d9::RenderTapeInitialSetupV4OwnerState> states;
      states.reserve(plan.entries().size());
      for (std::size_t index = 0u; index < plan.entries().size(); ++index) {
        const auto& entry = plan.entries()[index];
        hidden[index].identity = entry.definition.identity;
        states.push_back({.seedCount = entry.seedCount, .mode = entry.mode});
      }
      hidden_ = std::move(hidden);
      states_ = std::move(states);
      aliasesQualified_ = std::move(aliasesQualified);
      evidence_ = {.owners = plan.entries().size(), .seeds = plan.seeds().size(),
          .aliases = plan.aliases().size(), .seedBytes = plan.totalSeedBytes(),
          .allocationBytes = plan.totalAllocationBytes()};
      plan_ = std::move(plan);
      admitted_ = true;
      return true;
    } catch (...) {
      return false;
    }
  }

  // create returns one acquired wrapper reference. stage must qualify the
  // authenticated seed through the CPU mutation path and its host receipt,
  // not the provider's GPU-visible applyGeneralMutation. Failure retains every
  // acquired hidden reference for deterministic cleanup; no retry is allowed.
  template <class Create, class Stage>
  bool prepare(Create&& create, Stage&& stage) noexcept {
    if (!usable() || prepareStarted_) return false;
    prepareStarted_ = true;
    try {
      for (std::size_t index = 0u; index < plan_.entries().size(); ++index) {
        const auto& entry = plan_.entries()[index];
        auto& object = hidden_[index];
        object.value = create(entry, std::span<const Object>(hidden_));
        if (!object.value) return reject();
        ++evidence_.created;
        if (!d3d9::renderTapeInitialSetupV4Advance(states_[index], Action::Prepare))
          return reject();
        for (std::uint32_t relative = 0u; relative < entry.seedCount; ++relative) {
          const auto& seed = plan_.seeds()[entry.firstSeedIndex + relative];
          if (!stage(entry, seed, object)) return reject();
          ++evidence_.stagedSeeds;
        }
        ++evidence_.prepared;
      }
      return true;
    } catch (...) {
      return reject();
    }
  }

  // The executor supplies a live, controlled-queue quiescence check after
  // setup/checkpoint work. This ledger records that join but cannot mint the
  // queue/root-bound CPU capability used by V4 arm, nor a GPU checkpoint.
  template <class Checkpoint>
  bool joinHostCheckpoint(Checkpoint&& checkpoint) noexcept {
    if (!usable() || !prepareStarted_ || evidence_.hostCheckpointJoined ||
        evidence_.prepared != evidence_.owners ||
        evidence_.stagedSeeds != evidence_.seeds) return false;
    try {
      if (!checkpoint()) return reject();
      evidence_.hostCheckpointJoined = true;
      return true;
    } catch (...) {
      return reject();
    }
  }

  EventResult activate(std::uint32_t definitionIndex, std::uint32_t eventIndex,
      const d3d9::render_tape_runtime::Definition& definition,
      std::vector<Object>& active) noexcept {
    if (!usable()) return EventResult::Rejected;
    const auto entries = plan_.entries();
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
      return entry.definitionIndex == definitionIndex;
    });
    if (found == entries.end()) return EventResult::NotManaged;
    const auto index = static_cast<std::size_t>(found - entries.begin());
    auto next = states_[index];
    if (!evidence_.hostCheckpointJoined || found->definitionEventIndex != eventIndex ||
        !cpu_initial_setup_v4_detail::sameDefinition(found->definition, definition.fixed) ||
        !std::equal(found->descriptor.begin(), found->descriptor.end(),
                    definition.descriptor.begin(), definition.descriptor.end()) ||
        !hidden_[index].value || cpu_initial_setup_v4_detail::hasObject(active, definition.fixed.identity) ||
        !d3d9::renderTapeInitialSetupV4Advance(next, Action::Activate))
      return rejectEvent();
    try {
      // The fallible table append precedes transfer. On exception Hidden
      // still owns the acquisition and cleanup releases it exactly once.
      active.push_back(hidden_[index]);
      hidden_[index].value = nullptr;
      states_[index] = next;
      ++evidence_.activated;
      return EventResult::Accepted;
    } catch (...) {
      return rejectEvent();
    }
  }

  EventResult qualifyAlias(std::uint32_t definitionIndex, std::uint32_t eventIndex,
      const d3d9::render_tape_runtime::Definition& definition,
      std::span<const Object> active) noexcept {
    if (!usable()) return EventResult::Rejected;
    const auto aliases = plan_.aliases();
    const auto found = std::find_if(aliases.begin(), aliases.end(), [&](const auto& alias) {
      return alias.definitionIndex == definitionIndex;
    });
    if (found == aliases.end()) return EventResult::NotManaged;
    const auto index = static_cast<std::size_t>(found - aliases.begin());
    d3d9::RenderTapeSurfaceDescriptorV2 descriptor{};
    if (!evidence_.hostCheckpointJoined || aliasesQualified_[index] ||
        found->eventIndex != eventIndex || definition.fixed.expectedContentCount ||
        definition.fixed.expectedContentBytes ||
        definition.fixed.descriptorKind != static_cast<std::uint32_t>(d3d9::RenderTapeDescriptorKind::Surface) ||
        !d3d9::renderTapeIdentityV3SameIdentity(found->identity, definition.fixed.identity) ||
        found->parentOwnerIndex >= states_.size() ||
        states_[found->parentOwnerIndex].phase != Phase::Active ||
        !cpu_initial_setup_v4_detail::hasObject(active, found->parentIdentity) ||
        !d3d9::renderTapeLoadSurfaceDescriptorV2(definition.descriptor, descriptor) ||
        descriptor.subresource != found->subresource ||
        !d3d9::renderTapeSurfaceAliasMatchesTextureSubresource(
            plan_.entries()[found->parentOwnerIndex].descriptor,
            found->parentIdentity, descriptor)) return rejectEvent();
    aliasesQualified_[index] = true;
    return EventResult::Accepted;
  }

  EventResult consumeSeed(std::uint32_t mutationIndex, std::uint32_t eventIndex,
      const d3d9::RenderTapeResourceMutationHeader& mutation,
      std::span<const Object> active) noexcept {
    if (!usable()) return EventResult::Rejected;
    const auto seeds = plan_.seeds();
    const auto found = std::find_if(seeds.begin(), seeds.end(), [&](const auto& seed) {
      return seed.mutationIndex == mutationIndex;
    });
    if (found == seeds.end()) return EventResult::NotManaged;
    if (!evidence_.hostCheckpointJoined || found->eventIndex != eventIndex ||
        found->ownerIndex >= states_.size() ||
        !cpu_initial_setup_v4_detail::sameMutation(found->mutation, mutation) ||
        !cpu_initial_setup_v4_detail::hasObject(active, mutation.identity)) return rejectEvent();
    const auto& entry = plan_.entries()[found->ownerIndex];
    const auto index = static_cast<std::size_t>(found - seeds.begin());
    if (index < entry.firstSeedIndex || index - entry.firstSeedIndex >= entry.seedCount ||
        !d3d9::renderTapeInitialSetupV4Advance(states_[found->ownerIndex], Action::ConsumeSeed,
            static_cast<std::uint32_t>(index - entry.firstSeedIndex))) return rejectEvent();
    ++evidence_.consumedSeeds;
    return EventResult::Accepted;
  }

  bool intervalComplete() const noexcept {
    return usable() && evidence_.hostCheckpointJoined &&
        evidence_.activated == evidence_.owners && evidence_.consumedSeeds == evidence_.seeds &&
        std::all_of(aliasesQualified_.begin(), aliasesQualified_.end(), [](bool value) { return value; });
  }
  bool failed() const noexcept { return failed_; }
  const Plan& plan() const noexcept { return plan_; }
  const CpuInitialSetupV4Evidence& evidence() const noexcept { return evidence_; }
  std::span<const Object> hiddenObjects() const noexcept { return hidden_; }

  void releaseHidden() noexcept {
    for (std::size_t index = 0u; index < hidden_.size(); ++index) {
      auto& object = hidden_[index];
      if (!object.value) continue;
      release_.release(release_.context, object);
      object.value = nullptr;
      ++evidence_.releasedHidden;
      if (states_[index].phase == Phase::Hidden)
        (void)d3d9::renderTapeInitialSetupV4Advance(states_[index], Action::Release);
    }
    closed_ = true;
  }

 private:
  bool usable() const noexcept { return admitted_ && !failed_ && !closed_; }
  bool reject() noexcept { failed_ = true; return false; }
  EventResult rejectEvent() noexcept { failed_ = true; return EventResult::Rejected; }

  CpuInitialSetupV4ReleaseSink release_{};
  Plan plan_{};
  std::vector<Object> hidden_{};
  std::vector<d3d9::RenderTapeInitialSetupV4OwnerState> states_{};
  std::vector<bool> aliasesQualified_{};
  CpuInitialSetupV4Evidence evidence_{};
  bool admitted_ = false, prepareStarted_ = false, failed_ = false, closed_ = false;
};

} // namespace dxmt9::test
