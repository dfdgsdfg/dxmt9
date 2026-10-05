#pragma once

#include "render_tape_cpu_general_grammar.hpp"

#include <cstddef>
#include <cstdint>

namespace dxmt9::test {

enum class ControlledCpuV4InputProfile : std::uint8_t {
  Raw256,
};

// This prerequisite is deliberately ungranted. Total publication demand is
// not simultaneous ring occupancy: the executor must separately establish
// genuine before-record consumer progress and live source-capacity authority.
enum class ControlledCpuV4SourceProgressRequirement : std::uint8_t {
  RequiredUnproven,
};

class ControlledCpuV4InputAdmission;
inline ControlledCpuV4InputAdmission admitControlledCpuV4Input(
    const d3d9::render_tape_runtime::GeneralPreflightPlan&,
    ControlledCpuV4InputProfile = ControlledCpuV4InputProfile::Raw256) noexcept;

// Owned INPUT evidence only, not an executable token or source-capacity proof.
// The immutable plan must already come from authenticated general preflight;
// neither this result nor the shared narrowing grammar authenticates a tape or
// revalidates command bodies. Complete gamma bytes retain that prerequisite.
class ControlledCpuV4InputAdmission {
public:
  using Counts = d3d9::render_tape_runtime::GeneralCpuAdmissionCounts;
  using Caps = d3d9::render_tape_runtime::GeneralCpuAdmissionCaps;
  using Reason = d3d9::render_tape_runtime::GeneralCpuAdmissionReason;

  constexpr bool accepted() const noexcept { return input_.accepted(); }
  constexpr Reason reason() const noexcept { return input_.reason; }
  constexpr const Counts& counts() const noexcept { return input_.counts; }
  constexpr const Caps& inputCaps() const noexcept { return input_.inputCaps; }
  constexpr std::uint32_t eventIndex() const noexcept { return input_.eventIndex; }
  constexpr std::uint32_t chunkIndex() const noexcept { return input_.chunkIndex; }
  constexpr std::uint32_t recordIndex() const noexcept { return input_.recordIndex; }
  constexpr ControlledCpuV4InputProfile profile() const noexcept { return profile_; }
  constexpr std::size_t bootstrapRecordsPerChunkLimit() const noexcept { return 64u; }
  constexpr ControlledCpuV4SourceProgressRequirement sourceProgressRequirement() const noexcept {
    return ControlledCpuV4SourceProgressRequirement::RequiredUnproven;
  }

private:
  friend ControlledCpuV4InputAdmission admitControlledCpuV4Input(
      const d3d9::render_tape_runtime::GeneralPreflightPlan&,
      ControlledCpuV4InputProfile) noexcept;

  constexpr ControlledCpuV4InputAdmission(ControlledCpuV4InputProfile profile,
      d3d9::render_tape_runtime::GeneralCpuAdmissionResult input) noexcept
      : profile_(profile), input_(input) {}

  ControlledCpuV4InputProfile profile_;
  d3d9::render_tape_runtime::GeneralCpuAdmissionResult input_;
};

inline ControlledCpuV4InputAdmission admitControlledCpuV4Input(
    const d3d9::render_tape_runtime::GeneralPreflightPlan& plan,
    ControlledCpuV4InputProfile profile) noexcept {
  namespace runtime = d3d9::render_tape_runtime;
  if (profile != ControlledCpuV4InputProfile::Raw256)
    return {profile, {.reason = runtime::GeneralCpuAdmissionReason::InvalidBudget}};
  auto caps = runtime::generalCpuExtendedInputCaps();
  caps.maxRecordsPerChunk = 256u;
  auto input = runtime::detail::admitGeneralCpuPlanWithPolicy(
      plan, caps, runtime::detail::GeneralCpuInputValidationPolicy::ControlledV4Raw256);
  input.inputCaps = caps;
  return {profile, input};
}

} // namespace dxmt9::test
