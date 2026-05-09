#pragma once

// Shared helper for building short, debuggable Metal resource / encoder
// labels. dxmt9 uses `setLabel:` on every Buffer / Texture / Pipeline /
// CommandBuffer it creates so Xcode frame captures (M1) and Instruments
// timelines (M3) show semantic names instead of opaque handles.
//
// Labels deliberately stay short (<= 96 bytes by default) — they end up
// retained by the MTL runtime per-resource and are never useful past
// that fixed window of context.

#include "../winemetal/Metal.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstddef>

namespace dxmt9::labels {

template <std::size_t Cap = 96>
WMT::String makeLabelStringFmt(const char* fmt, ...) {
  char buf[Cap];
  std::va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return WMT::String::string(buf, WMTUTF8StringEncoding);
}

}  // namespace dxmt9::labels
