#pragma once

#include "device_c_render_tape.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

enum class RenderTapeStateFoldStatus : std::uint8_t {
  Valid,
  InvalidSource,
  InvalidSelection,
  MissingBootstrap,
  IncompleteCoverage,
  InvalidStateRecord,
  UnsupportedOrderedInput,
  MissingLiveIdentity,
  OutputBuildFailed,
  AllocationFailed,
};

// Cold, value-owned result of folding the production BootstrapState and the
// canonical state records through one selected Draw. The output chunk contains
// exactly one APPLY_STATE|FULL_SNAPSHOT record and can be embedded directly in
// another Render Tape BootstrapState without inventing another state schema.
struct RenderTapeStateFoldResult {
  RenderTapeStateFoldStatus status = RenderTapeStateFoldStatus::InvalidSource;
  RenderTapeValidationResult sourceValidation{};
  std::uint32_t failedEventIndex = 0xffffffffu;
  std::uint32_t failedRecordIndex = 0xffffffffu;
  std::uint16_t failedSectionKind = 0u;
  std::uint64_t coverageMask = 0u;
  bool selectedRecordWasFullSnapshot = false;
  std::vector<std::byte> bootstrapChunk{};
  std::vector<std::byte> gammaRamp{};
  std::vector<D9CWireObjectIdentity> referencedIdentities{};

  bool valid() const noexcept {
    return status == RenderTapeStateFoldStatus::Valid;
  }
};

// Validates the source tape before folding. State is seeded from the typed
// baseline plus the source BootstrapState overlays, then updated in exact
// event/record order through the selected record (which must be a Draw).
// Object handles are retained only as generation-qualified values and must be
// live at the selected source point. No replay/provider/writer callback runs.
RenderTapeStateFoldResult foldRenderTapeStateForDraw(
    std::span<const std::byte> source,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    std::uint64_t commandEventOrdinal,
    std::uint32_t recordIndex) noexcept;

const char* renderTapeStateFoldStatusName(
    RenderTapeStateFoldStatus status) noexcept;

} // namespace dxmt9::d3d9
