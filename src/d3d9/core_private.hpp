#pragma once

#include "dxmt9/core.hpp"

#include <vector>

namespace dxmt9::core {

bool isDisplayModeFormat(Format format);
std::vector<DisplayMode> makeAdapterModes(Format format,
                                          const BackendLimits &limits);
PresentParameters normalizePresentParameters(const AdapterInfo &adapter,
                                             PresentParameters params);
HResult validatePresentParameters(const PresentParameters &params,
                                  bool extended);
HResult validateFullscreenModeRelation(const PresentParameters &params,
                                       const DisplayModeEx *fullscreenMode);
PresentParameters applyFullscreenMode(PresentParameters params,
                                      const DisplayModeEx *fullscreenMode);
SwapDesc makeSwapDesc(const PresentParameters &params);

} // namespace dxmt9::core
