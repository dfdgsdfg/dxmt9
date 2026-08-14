#pragma once

#include "device_c_chunk_validate.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace dxmt9::d3d9 {

enum class RenderTapeFirstAccessClass : std::uint32_t {
  BindingOnly = 0u,
  FullClearWrite,
  PartialClearWrite,
  DrawWriteUnknownCoverage,
  ShaderReadCandidate,
  CopySource,
  CopyDestinationFull,
  CopyDestinationPartial,
  PresentRead,
  Unknown,
};

enum class RenderTapeFirstAccessStatus : std::uint32_t {
  Idle = 0u,
  Observing,
  Terminal,
  Malformed,
  Complete,
};

inline constexpr std::uint32_t kRenderTapeFirstAccessSentinel =
    std::numeric_limits<std::uint32_t>::max();
inline constexpr std::size_t kRenderTapeFirstAccessRenderTargetSlots = 4u;

struct RenderTapeFirstAccessBinding {
  bool valid = false;
  D9CWireObjectIdentity identity{};
  std::uint32_t handleIndex = kRenderTapeFirstAccessSentinel;
  std::uint32_t sectionKind = kRenderTapeFirstAccessSentinel;
  std::uint32_t bindingSlot = kRenderTapeFirstAccessSentinel;
};

struct RenderTapeFirstAccessLedger {
  bool armed = false;
  bool terminal = false;
  D9CWireObjectIdentity originIdentity{};
  D9CWireObjectIdentity resolvedIdentity{};
  std::array<RenderTapeFirstAccessBinding,
             kRenderTapeFirstAccessRenderTargetSlots>
      renderTargets{};
  RenderTapeFirstAccessBinding depthStencil{};
};

struct RenderTapeFirstAccessObservation {
  RenderTapeFirstAccessStatus status = RenderTapeFirstAccessStatus::Idle;
  RenderTapeFirstAccessClass classification = RenderTapeFirstAccessClass::Unknown;
  D9CWireObjectIdentity originIdentity{};
  D9CWireObjectIdentity resolvedIdentity{};
  D9CWireObjectIdentity observedIdentity{};
  std::uint32_t recordIndex = kRenderTapeFirstAccessSentinel;
  std::uint32_t recordType = 0u;
  std::uint32_t handleIndex = kRenderTapeFirstAccessSentinel;
  std::uint32_t sectionKind = kRenderTapeFirstAccessSentinel;
  std::uint32_t bindingSlot = kRenderTapeFirstAccessSentinel;
  bool aliasOrigin = false;
};

void renderTapeFirstAccessArm(
    RenderTapeFirstAccessLedger& ledger,
    const D9CWireObjectIdentity& originIdentity,
    const D9CWireObjectIdentity& resolvedIdentity) noexcept;

RenderTapeFirstAccessObservation renderTapeFirstAccessObserve(
    RenderTapeFirstAccessLedger& ledger,
    const ImportedChunkView& chunk) noexcept;

const char* renderTapeFirstAccessClassName(
    RenderTapeFirstAccessClass classification) noexcept;
const char* renderTapeFirstAccessStatusName(
    RenderTapeFirstAccessStatus status) noexcept;

} // namespace dxmt9::d3d9
