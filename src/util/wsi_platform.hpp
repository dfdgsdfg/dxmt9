#pragma once

namespace dxmt9::util::wsi {

enum class PlatformKind {
  Headless,
  Darwin,
  Win32,
};

struct MonitorInfo {
  bool headless = true;
};

struct WindowInfo {
  bool headless = true;
};

PlatformKind currentPlatform();
const char* currentPlatformName();
bool hasNativeWindowSystem();

MonitorInfo defaultMonitorInfo();
WindowInfo defaultWindowInfo();

}  // namespace dxmt9::util::wsi
