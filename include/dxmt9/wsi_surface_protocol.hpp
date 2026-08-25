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

struct LegacyHostViewClaimRelease {
  std::size_t remainingClaims = 0u;
  bool releaseHostView = false;
  bool valid = false;
};

// Wine's legacy macdrv helper returns the already-installed WineMetalView
// without retaining it when the same client Cocoa view is rebound.  Treat the
// returned pointer as one physical host view with dxmt9-local logical claims;
// only the final claim may call macdrv_view_release_metal_view().
constexpr LegacyHostViewClaimRelease releaseLegacyHostViewClaim(
    std::size_t currentClaims) noexcept {
  if (currentClaims == 0u) {
    return {};
  }
  return {
      .remainingClaims = currentClaims - 1u,
      .releaseHostView = currentClaims == 1u,
      .valid = true,
  };
}

inline constexpr std::uint32_t kFinalWsiTeardownAttemptLimit = 8u;

constexpr bool mayRetryFinalWsiTeardown(
    std::uint32_t completedAttempts) noexcept {
  return completedAttempts < kFinalWsiTeardownAttemptLimit;
}

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
             binding.layerToken != 0u && !binding.releaseAttempted;
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
    const SurfaceBindingState& binding, bool hasAcquisitionDc) noexcept {
  return hasWineReleaseObligation(binding) && hasAcquisitionDc;
}

constexpr bool canAttemptWineRelease(
    const SurfaceBindingState& binding, bool hasAcquisitionDc,
    bool unixQuiescent) noexcept {
  return hasWineReleaseObligation(binding) && unixQuiescent &&
         hasRetainedReleaseCapability(binding, hasAcquisitionDc) &&
         !binding.releaseAttempted;
}

enum class WineReleaseDisposition : std::uint8_t {
  NothingOwned,
  MissingCapability,
  AwaitingUnixQuiescence,
  DcBalanced,
  SurfaceAttemptedAndDcBalanced,
};

// Production-bound cold release seam. The PE passes the retained acquisition
// HDC through the callbacks; native tests inject counters. A retained DC is
// balanced exactly once even when the protocol state is partial and no token
// remains. A live token is never consumed without both unix quiescence and the
// matching retained capability.
template <typename ReleaseSurface, typename ReleaseDc>
WineReleaseDisposition dischargeWineRelease(
    SurfaceBindingState& binding, bool hasAcquisitionDc,
    bool unixQuiescent, ReleaseSurface&& releaseSurface,
    ReleaseDc&& releaseDc) {
  const bool hasToken = hasWineReleaseObligation(binding);
  if (!hasAcquisitionDc) {
    return hasToken ? WineReleaseDisposition::MissingCapability
                    : WineReleaseDisposition::NothingOwned;
  }
  if (hasToken && !unixQuiescent) {
    return WineReleaseDisposition::AwaitingUnixQuiescence;
  }
  if (hasToken && !binding.releaseAttempted) {
    binding.releaseAttempted = true;
    releaseSurface();
    releaseDc();
    return WineReleaseDisposition::SurfaceAttemptedAndDcBalanced;
  }
  releaseDc();
  return WineReleaseDisposition::DcBalanced;
}

constexpr bool preserveCurrentBindingOnAdoptionFailure(
    const SurfaceBindingState& current,
    const SurfaceBindingState& candidate,
    bool adoptionSucceeded) noexcept {
  return !adoptionSucceeded && current.unixAdopted &&
         validAdoptionCandidate(current) && validAdoptionCandidate(candidate);
}

}  // namespace dxmt9::wsi
