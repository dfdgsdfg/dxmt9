#include "wsi_platform.hpp"

namespace dxmt9::util::wsi {

PlatformKind currentPlatform() {
  return PlatformKind::Headless;
}

const char* currentPlatformName() {
  return "headless";
}

bool hasNativeWindowSystem() {
  return false;
}

}  // namespace dxmt9::util::wsi
