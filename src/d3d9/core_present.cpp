#include "core_private.hpp"
#include "dxmt9/core.hpp"
#include "util/config/config.hpp"

#include <algorithm>
#include <array>

namespace dxmt9::core {

PresentParameters normalizePresentParameters(const AdapterInfo &adapter,
                                             PresentParameters params) {
  if (dxmt9::util::getenvFlag("DXMT_FORCE_WINDOWED")) {
    params.windowed = true;
  }
  if (params.backBufferCount == 0) {
    params.backBufferCount = 1;
  }
  if (params.backBufferFormat == Format::Unknown) {
    params.backBufferFormat = adapter.displayMode.format;
  }
  if (!params.windowed) {
    if (params.backBufferWidth == 0) {
      params.backBufferWidth = adapter.displayMode.width;
    }
    if (params.backBufferHeight == 0) {
      params.backBufferHeight = adapter.displayMode.height;
    }
  } else {
    params.backBufferWidth = std::max(1u, params.backBufferWidth);
    params.backBufferHeight = std::max(1u, params.backBufferHeight);
  }
  return params;
}

constexpr u32 kD3dSwapEffectCopy = 3;
constexpr u32 kD3dSwapEffectFlipex = 5;

constexpr u32 kD3dPresentIntervalDefault = 0x00000000u;
constexpr u32 kD3dPresentIntervalOne = 0x00000001u;
constexpr u32 kD3dPresentIntervalTwo = 0x00000002u;
constexpr u32 kD3dPresentIntervalThree = 0x00000004u;
constexpr u32 kD3dPresentIntervalFour = 0x00000008u;
constexpr u32 kD3dPresentIntervalImmediate = 0x80000000u;

bool isValidPresentationInterval(PresentInterval interval) {
  switch (interval) {
  case PresentInterval::Immediate:
  case PresentInterval::Default:
  case PresentInterval::Two:
    return true;
  }
  return false;
}

bool isValidPresentationIntervalRaw(u32 interval) {
  switch (interval) {
  case kD3dPresentIntervalDefault:
  case kD3dPresentIntervalOne:
  case kD3dPresentIntervalTwo:
  case kD3dPresentIntervalThree:
  case kD3dPresentIntervalFour:
  case kD3dPresentIntervalImmediate:
    return true;
  default:
    return false;
  }
}

HResult validatePresentParameters(const PresentParameters &params,
                                  bool extended) {
  const u32 maxSwapEffect =
      extended ? kD3dSwapEffectFlipex : kD3dSwapEffectCopy;
  if (params.swapEffect == 0 || params.swapEffect > maxSwapEffect) {
    return D3DERR_INVALIDCALL;
  }

  const u32 maxBackBufferCount = extended ? 30u : 3u;
  if (params.backBufferCount > maxBackBufferCount) {
    return D3DERR_INVALIDCALL;
  }

  if (params.swapEffect == kD3dSwapEffectCopy && params.backBufferCount > 1) {
    return D3DERR_INVALIDCALL;
  }

  if (!isValidPresentationInterval(params.presentationInterval) ||
      !isValidPresentationIntervalRaw(params.presentationIntervalRaw)) {
    return D3DERR_INVALIDCALL;
  }

  return D3D_OK;
}

HResult validateFullscreenModeRelation(const PresentParameters &params,
                                       const DisplayModeEx *fullscreenMode) {
  if (!fullscreenMode) {
    return D3D_OK;
  }
  if (params.windowed) {
    return D3DERR_INVALIDCALL;
  }
  if (fullscreenMode->width != params.backBufferWidth ||
      fullscreenMode->height != params.backBufferHeight) {
    return D3DERR_INVALIDCALL;
  }
  return D3D_OK;
}

PresentParameters applyFullscreenMode(PresentParameters params,
                                      const DisplayModeEx *fullscreenMode) {
  if (!fullscreenMode) {
    return params;
  }
  params.windowed = false;
  if (fullscreenMode->width != 0) {
    params.backBufferWidth = fullscreenMode->width;
  }
  if (fullscreenMode->height != 0) {
    params.backBufferHeight = fullscreenMode->height;
  }
  if (fullscreenMode->format != Format::Unknown) {
    params.backBufferFormat = fullscreenMode->format;
  }
  return params;
}

SwapDesc makeSwapDesc(const PresentParameters &params) {
  SwapDesc desc;
  desc.window = params.deviceWindow;
  desc.width = params.backBufferWidth;
  desc.height = params.backBufferHeight;
  desc.format = params.backBufferFormat;
  desc.interval = params.presentationInterval;
  desc.windowed = params.windowed;
  desc.backBufferCount = std::max(1u, params.backBufferCount);
  desc.displaySyncEnabled =
      params.presentationInterval != PresentInterval::Immediate;
  desc.multiSampleType = params.multiSampleType;
  return desc;
}

} // namespace dxmt9::core
