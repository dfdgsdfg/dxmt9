#pragma once

#include "device_c_render_tape_source_plan_v4.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

namespace render_tape_runtime { struct GeneralPreflightPlan; }

enum class RenderTapeInitialSetupV4Status : std::uint8_t {
  Valid,
  InvalidBinding,
  InvalidBoundary,
  UnsupportedClosure,
  IncompleteClosure,
  InvalidAlias,
  CapacityExceeded,
  AllocationFailed,
};

struct RenderTapeInitialSetupV4Limits {
  // Related alias definitions are independently bounded by maxObjects too.
  std::uint32_t maxObjects = 1024u;
  std::uint32_t maxSeeds = 8192u;
  std::uint64_t maxSeedBytes = 512u * 1024u * 1024u;
  // Descriptor-only physical allocation is not promised initial content.
  std::uint64_t maxAllocationBytes = 64u * 1024u * 1024u;
};

enum class RenderTapeInitialSetupV4Family : std::uint8_t {
  Texture2D, TextureCube, TextureVolume, VertexBuffer, IndexBuffer,
};
enum class RenderTapeInitialSetupV4Mode : std::uint8_t {
  Seeded, AllocationOnly,
};

struct RenderTapeInitialSetupV4Entry {
  RenderTapeObjectDefineHeader definition{};
  std::vector<std::byte> descriptor{};
  RenderTapeInitialSetupV4Family family = RenderTapeInitialSetupV4Family::Texture2D;
  std::uint32_t definitionIndex = 0u, definitionEventIndex = 0u;
  // AllocationOnly has count zero and firstSeedIndex == NoIndex.
  std::uint32_t firstSeedIndex = 0u, seedCount = 0u;
  std::uint64_t totalBytes = 0u;
  RenderTapeInitialSetupV4Mode mode = RenderTapeInitialSetupV4Mode::Seeded;
  std::uint64_t allocationBytes = 0u;
};

struct RenderTapeInitialSetupV4Seed {
  RenderTapeResourceMutationHeader mutation{};
  std::uint32_t mutationIndex = 0u, eventIndex = 0u, ownerIndex = 0u;
};

struct RenderTapeInitialSetupV4Alias {
  D9CWireObjectIdentity identity{}, parentIdentity{};
  std::uint32_t parentOwnerIndex = 0u, definitionIndex = 0u, eventIndex = 0u;
  std::uint32_t subresource = 0u;
};

// All authenticated metadata is copied; no blob payload, tape span, wrapper,
// queue, or device pointer is retained. A moved-from plan loses authority.
class RenderTapeInitialSetupV4Plan {
public:
  RenderTapeInitialSetupV4Plan() = default;
  RenderTapeInitialSetupV4Plan(RenderTapeInitialSetupV4Plan&&) noexcept;
  RenderTapeInitialSetupV4Plan& operator=(RenderTapeInitialSetupV4Plan&&) noexcept;
  RenderTapeInitialSetupV4Plan(const RenderTapeInitialSetupV4Plan&) = delete;
  RenderTapeInitialSetupV4Plan& operator=(const RenderTapeInitialSetupV4Plan&) = delete;

  bool valid() const noexcept { return valid_; }
  std::uint32_t firstIntervalEvent() const noexcept { return firstIntervalEvent_; }
  std::uint64_t totalSeedBytes() const noexcept { return totalSeedBytes_; }
  std::uint64_t totalAllocationBytes() const noexcept { return totalAllocationBytes_; }
  const RenderTapeDigest& sourcePlanDigest() const noexcept { return digest_; }
  std::span<const RenderTapeInitialSetupV4Entry> entries() const noexcept { return entries_; }
  std::span<const RenderTapeInitialSetupV4Seed> seeds() const noexcept { return seeds_; }
  std::span<const RenderTapeInitialSetupV4Alias> aliases() const noexcept { return aliases_; }

private:
  friend RenderTapeInitialSetupV4Status buildRenderTapeInitialSetupV4(
      const CaptureSourceReplayPlanV4&, const render_tape_runtime::GeneralPreflightPlan&,
      std::span<const std::byte>, std::uint32_t, RenderTapeInitialSetupV4Plan&,
      RenderTapeInitialSetupV4Limits) noexcept;
  std::vector<RenderTapeInitialSetupV4Entry> entries_{};
  std::vector<RenderTapeInitialSetupV4Seed> seeds_{};
  std::vector<RenderTapeInitialSetupV4Alias> aliases_{};
  RenderTapeDigest digest_{};
  std::uint64_t totalSeedBytes_ = 0u, totalAllocationBytes_ = 0u;
  std::uint32_t firstIntervalEvent_ = kRenderTapeIdentityV4NoIndex;
  bool valid_ = false;
};

// Shared with execution admission. This is precisely its existing setup-only
// prefix grammar, not a caller-selected source cut. Failure leaves out intact.
bool renderTapeInitialSetupV4SnapshotPrefix(
    const render_tape_runtime::GeneralPreflightPlan&, std::uint32_t& out) noexcept;

// Authenticates the whole supplied General plan against the immutable V4
// owner before selecting any seed. Late promised closures must be complete,
// contiguous, canonical initial Upload blocks; subsequent writes are excluded.
// AllocationOnly admits only canonical late NullRtDescriptorOnly textures with
// zero promises, not ProducedByCapturedPass or independently prepared aliases.
// Admission is transactional: every failure leaves the destination unchanged.
RenderTapeInitialSetupV4Status buildRenderTapeInitialSetupV4(
    const CaptureSourceReplayPlanV4&, const render_tape_runtime::GeneralPreflightPlan&,
    std::span<const std::byte> exactTape, std::uint32_t firstIntervalEvent,
    RenderTapeInitialSetupV4Plan&, RenderTapeInitialSetupV4Limits = {}) noexcept;

enum class RenderTapeInitialSetupV4OwnerPhase : std::uint8_t {
  Unprepared, Hidden, Active, Released,
};
enum class RenderTapeInitialSetupV4Action : std::uint8_t {
  Prepare, Activate, ConsumeSeed, Release,
};
struct RenderTapeInitialSetupV4OwnerState {
  RenderTapeInitialSetupV4OwnerPhase phase = RenderTapeInitialSetupV4OwnerPhase::Unprepared;
  std::uint32_t seedCount = 0u, consumedSeeds = 0u;
  RenderTapeInitialSetupV4Mode mode = RenderTapeInitialSetupV4Mode::Seeded;
};

// Ownership only: Hidden is not GPU completion authority. The provider's real
// checked setup checkpoint must finish before arm/activation. Release permits
// partially prepared owners, but duplicate transitions never count twice.
// Zero seeds are legal only for AllocationOnly, which grants no content action.
constexpr bool renderTapeInitialSetupV4Advance(RenderTapeInitialSetupV4OwnerState& state,
    RenderTapeInitialSetupV4Action action,
    std::uint32_t relativeSeedIndex = kRenderTapeIdentityV4NoIndex) noexcept {
  using Phase = RenderTapeInitialSetupV4OwnerPhase;
  using Action = RenderTapeInitialSetupV4Action;
  using Mode = RenderTapeInitialSetupV4Mode;
  if (state.mode == Mode::Seeded) {
    if (!state.seedCount || state.consumedSeeds > state.seedCount) return false;
  } else if (state.mode == Mode::AllocationOnly) {
    if (state.seedCount || state.consumedSeeds) return false;
  } else return false;
  auto next = state;
  switch (action) {
  case Action::Prepare:
    if (state.phase != Phase::Unprepared || state.consumedSeeds ||
        relativeSeedIndex != kRenderTapeIdentityV4NoIndex) return false;
    next.phase = Phase::Hidden;
    break;
  case Action::Activate:
    if (state.phase != Phase::Hidden || state.consumedSeeds ||
        relativeSeedIndex != kRenderTapeIdentityV4NoIndex) return false;
    next.phase = Phase::Active;
    break;
  case Action::ConsumeSeed:
    if (state.mode != Mode::Seeded || state.phase != Phase::Active ||
        relativeSeedIndex != state.consumedSeeds ||
        state.consumedSeeds == state.seedCount) return false;
    ++next.consumedSeeds;
    break;
  case Action::Release:
    if ((state.phase != Phase::Hidden && state.phase != Phase::Active) ||
        relativeSeedIndex != kRenderTapeIdentityV4NoIndex) return false;
    next.phase = Phase::Released;
    break;
  default: return false;
  }
  state = next;
  return true;
}

} // namespace dxmt9::d3d9
