#pragma once

// Cold general-frame completion policy used by the CPU experiment adapter.
// The predicate only selects the point at which an already-submitted
// no-GPU source is acknowledged; it does not observe Metal, a queue
// completion waterline, or resource-pool ownership.

#include "../../../src/d3d9/device_c_render_tape.hpp"

namespace dxmt9::test {

// In the immediate lane, the existing post-command-chunk path completes the
// source, so this pre-event predicate is always false.  BeforeNextWork moves
// that acknowledgement to the boundary immediately before the next piece of
// frame work (or before PresentComplete closes the frame).  ObjectDestroy is
// deliberately excluded: wrapper destruction may occur while a source is
// still pending and must not become an implicit completion operation.
//
// This is a CPU scheduling predicate only.  It is not a GPU-lifetime proof,
// and BeforeNextWork must not delay completion across the mutating work it
// is intended to precede.
inline constexpr bool releaseBeforeCpuTapeWork(
    d3d9::RenderTapeEventType event, bool beforeNextWork) noexcept {
  switch (event) {
  case d3d9::RenderTapeEventType::BootstrapState:
  case d3d9::RenderTapeEventType::ObjectDefine:
  case d3d9::RenderTapeEventType::ResourceMutation:
  case d3d9::RenderTapeEventType::CommandChunk:
  case d3d9::RenderTapeEventType::OrderedControl:
  case d3d9::RenderTapeEventType::PresentComplete:
    return beforeNextWork;
  case d3d9::RenderTapeEventType::ObjectDestroy:
    return false;
  }
#if defined(__clang__) || defined(__GNUC__)
  __builtin_unreachable();
#endif
}

}  // namespace dxmt9::test
