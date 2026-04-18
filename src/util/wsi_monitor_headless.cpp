#include "wsi_platform.hpp"

namespace dxmt9::util::wsi {

MonitorInfo defaultMonitorInfo() {
  return MonitorInfo{
      .headless = true,
  };
}

}  // namespace dxmt9::util::wsi
