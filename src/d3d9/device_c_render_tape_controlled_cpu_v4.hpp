#pragma once

#include "device_c_render_tape_action_witness.hpp"
#include "device_c_render_tape_identity_v4.hpp"
#include "device_c_render_tape_semantic_mapping_v4.hpp"
#include "dxmt9/device_c.h"

#include <memory>
#include <span>

namespace dxmt9 {
class Device;
struct CommandQueueArenaLeaseTestAccess;
}

namespace dxmt9::d3d9 {

class RenderTapeExecutionV4;
class RenderTapeRuntimeIdentityV4;
struct RenderTapeReplayQueryBaselineV4;
struct RenderTapeControlledCpuV4Authority;

enum class RenderTapeControlledCpuV4Boundary : std::uint8_t { Mint, Replay, Finish };
enum class RenderTapeControlledCpuV4Phase : std::uint8_t { Available, Bound, Finalized, Revoked };
enum class RenderTapeControlledCpuV4Action : std::uint8_t { Bind, Finish, Revoke };

struct RenderTapeControlledCpuV4Facts {
  bool fixtureAlive = false;
  bool isolated = false;
  bool exactDevice = false;
  bool exactRoot = false;
  bool provenanceComplete = false;
};

// Shared finite-state policy: authority never returns from Finalized/Revoked
// to Available, and finish cannot substitute topology for provenance.
inline constexpr bool renderTapeControlledCpuV4Advance(RenderTapeControlledCpuV4Phase& phase,
    RenderTapeControlledCpuV4Action action, RenderTapeControlledCpuV4Facts facts) noexcept {
  using Phase = RenderTapeControlledCpuV4Phase;
  using Action = RenderTapeControlledCpuV4Action;
  if (action == Action::Revoke && phase != Phase::Revoked) {
    phase = Phase::Revoked;
    return true;
  }
  if (!facts.fixtureAlive || !facts.isolated || !facts.exactDevice || !facts.exactRoot)
    return false;
  if (action == Action::Bind && phase == Phase::Available) phase = Phase::Bound;
  else if (action == Action::Finish && phase == Phase::Bound && facts.provenanceComplete)
    phase = Phase::Finalized;
  else return false;
  return true;
}

// A consumer may retain this weak authority plus its independent backend pin.
// Each queue operation must use visitBackend, not a previously successful
// validation. Call it outside the queue mutex: the isolation probe may take
// that mutex. Authority/binding locks are released before the visitor, while
// a live fixture guard remains retained; visitors may therefore wait without
// excluding replay. It grants no capture arm, finalization, or GPU evidence.
class RenderTapeControlledCpuV4Lease {
public:
  using Visitor = bool (*)(void*, D9CDevice*, dxmt9::Device&) noexcept;
  bool visitBackend(RenderTapeControlledCpuV4Boundary, void*, Visitor) const noexcept;

private:
  friend class RenderTapeControlledCpuV4Capability;
  friend class RenderTapeExecutionV4;
  std::weak_ptr<RenderTapeControlledCpuV4Authority> authority_{};
  std::shared_ptr<dxmt9::Device> backend_{};
};

// Cold, move-only authority minted only by the inert queue fixture. The
// fixture exclusively owns wrapper/reset lifetime and must join all consumers
// before revoke, reset, or wrapper destruction. Its weak lifetime token is
// checked before any borrowed device/probe-context dereference. The backend
// pin alone never keeps that wrapper authority alive.
class RenderTapeControlledCpuV4Capability {
public:
  RenderTapeControlledCpuV4Capability() = default;
  ~RenderTapeControlledCpuV4Capability();
  RenderTapeControlledCpuV4Capability(RenderTapeControlledCpuV4Capability&&) noexcept;
  RenderTapeControlledCpuV4Capability& operator=(RenderTapeControlledCpuV4Capability&&) noexcept;
  RenderTapeControlledCpuV4Capability(const RenderTapeControlledCpuV4Capability&) = delete;
  RenderTapeControlledCpuV4Capability& operator=(const RenderTapeControlledCpuV4Capability&) = delete;
  RenderTapeControlledCpuV4Lease lease() const noexcept;
  void revoke() noexcept;

private:
  friend struct dxmt9::CommandQueueArenaLeaseTestAccess;
  friend class RenderTapeExecutionV4;
  using IsolationProbe = bool (*)(void*, D9CDevice*, const dxmt9::Device&,
      RenderTapeControlledCpuV4Boundary) noexcept;
  static RenderTapeControlledCpuV4Capability mint(D9CDevice*, std::shared_ptr<dxmt9::Device>,
      std::weak_ptr<void> fixtureLifetime, void* probeContext, IsolationProbe) noexcept;
  std::int32_t beginCapture(D9CDevice*, std::uint64_t token, std::uint64_t epoch,
      std::span<const RenderTapeReplayQueryBaselineV4>,
      D9CRenderTapeIdentityCaptureV4ArmResult*) const noexcept;
  bool finishCapture(D9CDevice*, RenderTapeIdentityV4CaptureKey, std::uint64_t queue,
      std::uint64_t presentOrdinal, RenderTapeIdentityV4View&,
      RenderTapeActionWitnessV1&) const noexcept;
  std::shared_ptr<RenderTapeControlledCpuV4Authority> authority_{};
};

// This is controlled queue/provenance evidence, not the production completion
// domain. No topology, GPU completion, occlusion number, pixel or timing claim
// can be represented by a successful controlled CPU finalization.
struct RenderTapeControlledCpuV4Evidence {
  enum class Domain : std::uint8_t { None, ControlledCpu };
  Domain domain = Domain::None;
  bool controlledCpuComplete = false;
  bool canonicalTraversalComplete = false;
  bool rawAckCoverageComplete = false;
  bool semanticMappingComplete = false;
  RenderTapeDigest planDigest{};
  std::size_t sourceObligationsRegistered = 0u;
  std::size_t sourceObligationsQualified = 0u;
  std::shared_ptr<const RenderTapeSemanticMappingV4> semanticMapping{};
  RenderTapeIdentityV4View actualIdentity{};
  RenderTapeActionWitnessV1 actionWitness{};
};

} // namespace dxmt9::d3d9
