#include "wsi_platform.hpp"

namespace dxmt9::util::wsi {

WindowInfo defaultWindowInfo() {
  return WindowInfo{
      .headless = true,
  };
}

}  // namespace dxmt9::util::wsi
