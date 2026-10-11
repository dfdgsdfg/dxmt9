#pragma once

#include "device_c_render_tape_action_witness.hpp"
#include "../dxmt9/dxmt9_render_tape_action_profile.hpp"
#include "dxmt9/device_c.h"
#include "dxmt9/query_poll_snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

class RenderTapeRuntimeIdentityV4;
struct RenderTapeControlledCpuV4Authority;

enum class RenderTapeRuntimeBindingV4Domain : std::uint8_t { Production, ControlledCpu };

// Borrowed wrapper plus an owned receipt of the one real setup END. Neither
// captured cycle labels nor captured completion watermarks enter this API.
struct RenderTapeReplayQueryBaselineV4 {
  D9CQuery* query = nullptr;
  core::QueryPollSnapshot snapshot{};
};

// Replay-only cold arm. Validates every live receipt, initializes local query
// history through short endpoint admission, then atomically exposes the root.
// The public capture C ABI continues to use an unseeded production arm.
std::int32_t beginRenderTapeIdentityCaptureV4ForReplay(D9CDevice* device,
    std::uint64_t token, std::uint64_t epoch,
    std::span<const RenderTapeReplayQueryBaselineV4> queries,
    D9CRenderTapeIdentityCaptureV4ArmResult* out) noexcept;

// The queue reducer changes a binary policy only when the bit changes. Bind
// its monotonic revision history, including pre-arm changes and ABA, instead
// of interpreting two equal endpoint booleans as an unchanged interval.
inline constexpr bool renderTapeRuntimeBindingV4ActionProfileCoherent(
    const RenderTapeActionProfileSnapshot& snapshot) noexcept {
  return snapshot.valid && snapshot.version == kRenderTapeActionProfileVersion &&
      snapshot.constructionProfile <= kRenderTapeActionProfileExact &&
      snapshot.currentProfile <= kRenderTapeActionProfileExact &&
      snapshot.currentProfile ==
          (snapshot.constructionProfile ^ static_cast<std::uint32_t>(snapshot.revision & 1u));
}

inline constexpr bool renderTapeRuntimeBindingV4BuildActionWitness(
    const RenderTapeActionProfileSnapshot& arm,
    const RenderTapeActionProfileSnapshot& freeze,
    std::uint64_t token, std::uint64_t epoch, std::uint64_t queue,
    RenderTapeActionWitnessV1& out) noexcept {
  out = {};
  if (!renderTapeRuntimeBindingV4ActionProfileCoherent(arm) ||
      !renderTapeRuntimeBindingV4ActionProfileCoherent(freeze) ||
      arm.constructionProfile != freeze.constructionProfile ||
      freeze.revision < arm.revision || !token || !epoch || !queue)
    return false;
  const RenderTapeActionWitnessV1 candidate{
      .evidence = static_cast<std::uint32_t>(RenderTapeActionWitnessEvidenceV1::QueueObserved),
      .constructionProfile = arm.constructionProfile,
      .armProfile = arm.currentProfile,
      .freezeProfile = freeze.currentProfile,
      .captureToken = token,
      .captureEpoch = epoch,
      .queue = queue,
      .armRevision = arm.revision,
      .freezeRevision = freeze.revision,
  };
  if (!renderTapeActionWitnessV1MatchesBinding(candidate, token, epoch, queue))
    return false;
  out = candidate;
  return true;
}

// Device-owned cold binding state. The receipt and transfer are frozen as one
// value, so a size query and its later copy cannot describe different captures.
// This state never retains a queue, endpoint, or a lease of its own root.
struct RenderTapeRuntimeBindingV4State {
  RenderTapeRuntimeBindingV4Domain domain = RenderTapeRuntimeBindingV4Domain::Production;
  std::weak_ptr<RenderTapeControlledCpuV4Authority> controlledCpuAuthority{};
  bool controlledCpuFinalized = false;
  std::uint64_t captureToken = 0u;
  std::uint64_t captureEpoch = 0u;
  std::uint64_t queue = 0u;
  RenderTapeActionProfileSnapshot armActionProfile{};
  // Sticky even for an invalid freeze observation: a retry cannot resample
  // policy after endpoint retirement and manufacture a better witness.
  bool actionWitnessFrozen = false;
  std::uint64_t frozenPresentOrdinal = 0u;
  RenderTapeActionWitnessV1 actionWitness{};
  // Original production Normal arm only; replay initialization grants none.
  std::vector<std::byte> attachmentHistory{};
  std::optional<D9CRenderTapeIdentityCaptureV4Result> receipt{};
  std::vector<std::byte> transfer{};
};

// Replay/query observers acquire one old-arm lifetime without racing the
// atomic revocation/replacement of the device's retained root.
std::shared_ptr<RenderTapeRuntimeIdentityV4>
acquireRenderTapeRuntimeIdentityV4(D9CDevice* device) noexcept;

void detachRenderTapeRuntimeIdentityV4(D9CDevice* device) noexcept;

} // namespace dxmt9::d3d9
