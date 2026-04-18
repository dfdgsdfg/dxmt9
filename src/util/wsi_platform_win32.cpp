#include "wsi_platform.hpp"

namespace dxmt9::util::wsi {

PlatformKind currentPlatform() {
  return PlatformKind::Win32;
}

const char* currentPlatformName() {
  return "win32";
}

bool hasNativeWindowSystem() {
  return true;
}

}  // namespace dxmt9::util::wsi
