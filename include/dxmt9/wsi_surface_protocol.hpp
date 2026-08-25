#pragma once

#include "winemetal/winemac_surface_escape.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dxmt9::wsi {

enum class SurfaceProtocol : std::uint32_t {
  Unsupported = 0u,
  ExtEscapeV1 = 1u,
  LegacyMacdrvSymbols = 2u,
};

struct SurfaceBindingState {
  SurfaceProtocol protocol = SurfaceProtocol::Unsupported;
  std::uint64_t hwnd = 0u;
  std::uint64_t surfaceToken = 0u;
  std::uint64_t layerToken = 0u;
  // PE-only cold release capability retained from the successful GetDC.
  // The fixed-width D9CWsiSurfaceBinding wire is built field-by-field and
  // deliberately excludes this process-local handle.
  std::uintptr_t releaseHdc = 0u;
  bool unixAdopted = false;
  bool releaseAttempted = false;
};

inline constexpr std::string_view kLegacyProtocolPrefix =
    "legacy-macdrv-symbols:";

constexpr bool isLegacyProtocolDeclaration(
    std::string_view value, std::string_view runtimeId) noexcept {
  return !runtimeId.empty() && value.size() ==
      kLegacyProtocolPrefix.size() + runtimeId.size() &&
      value.starts_with(kLegacyProtocolPrefix) &&
      value.substr(kLegacyProtocolPrefix.size()) == runtimeId;
}

constexpr SurfaceProtocol selectSurfaceProtocol(
    bool escapeQuerySupported, bool legacyQualified) noexcept {
  if (escapeQuerySupported) {
    return SurfaceProtocol::ExtEscapeV1;
  }
  return legacyQualified ? SurfaceProtocol::LegacyMacdrvSymbols
                         : SurfaceProtocol::Unsupported;
}

// `cbOutput` is the size passed into ExtEscape, not a returned byte count.
// Callers value-initialize `response` before the call so a positive result
// must also have replaced both zero fields with valid tokens.
constexpr bool validGetSurfaceEscapeCall(
    int escapeResult, std::size_t cbOutput,
    const macdrv_escape_surface& response) noexcept {
  return escapeResult > 0 && cbOutput == sizeof(macdrv_escape_surface) &&
         response.surface != 0u && response.layer != 0u;
}

enum class QuiescenceDisposition : std::uint8_t {
  Complete,
  ActiveArena,
  AlreadyActive,
  QueueStopped,
};

constexpr bool quiescenceComplete(QuiescenceDisposition value) noexcept {
  return value == QuiescenceDisposition::Complete;
}

constexpr bool presenterReplacementMayCommit(
    bool candidateValid, bool candidateRegistered,
    QuiescenceDisposition quiescence) noexcept {
  return candidateValid && candidateRegistered &&
         quiescenceComplete(quiescence);
}

constexpr bool validAdoptionCandidate(
    const SurfaceBindingState& binding) noexcept {
  switch (binding.protocol) {
    case SurfaceProtocol::ExtEscapeV1:
      return binding.hwnd != 0u && binding.surfaceToken != 0u &&
             binding.layerToken != 0u && binding.releaseHdc != 0u &&
             !binding.releaseAttempted;
    case SurfaceProtocol::LegacyMacdrvSymbols:
      return binding.hwnd != 0u && binding.surfaceToken == 0u &&
             binding.layerToken == 0u && !binding.releaseAttempted;
    case SurfaceProtocol::Unsupported:
      return false;
  }
  return false;
}

constexpr bool validPresenterRestoreBinding(
    SurfaceProtocol protocol, std::uint64_t hwnd,
    std::uint64_t layerToken) noexcept {
  if (hwnd == 0u) {
    return false;
  }
  return protocol == SurfaceProtocol::ExtEscapeV1
      ? layerToken != 0u
      : protocol == SurfaceProtocol::LegacyMacdrvSymbols && layerToken == 0u;
}

constexpr bool hasWineReleaseObligation(
    const SurfaceBindingState& binding) noexcept {
  return binding.protocol == SurfaceProtocol::ExtEscapeV1 &&
         binding.surfaceToken != 0u;
}

constexpr bool hasRetainedReleaseCapability(
    const SurfaceBindingState& binding) noexcept {
  return hasWineReleaseObligation(binding) && binding.releaseHdc != 0u;
}

constexpr bool canAttemptWineRelease(
    const SurfaceBindingState& binding, bool unixQuiescent) noexcept {
  return hasWineReleaseObligation(binding) && unixQuiescent &&
         hasRetainedReleaseCapability(binding) && !binding.releaseAttempted;
}

constexpr bool preserveCurrentBindingOnAdoptionFailure(
    const SurfaceBindingState& current,
    const SurfaceBindingState& candidate,
    bool adoptionSucceeded) noexcept {
  return !adoptionSucceeded && current.unixAdopted &&
         validAdoptionCandidate(current) && validAdoptionCandidate(candidate);
}

}  // namespace dxmt9::wsi
