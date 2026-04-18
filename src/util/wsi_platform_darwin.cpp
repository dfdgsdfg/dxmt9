#include "wsi_platform.hpp"

namespace dxmt9::util::wsi {

PlatformKind currentPlatform() {
  return PlatformKind::Darwin;
}

const char* currentPlatformName() {
  return "darwin";
}

bool hasNativeWindowSystem() {
  return true;
}

}  // namespace dxmt9::util::wsi
