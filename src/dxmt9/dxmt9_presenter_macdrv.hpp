#pragma once

// macdrv-interop shim for Presenter. Pure C interface so Presenter itself can
// stay in a .cpp file; all ObjC lives in dxmt9_presenter_macdrv.mm.

#include "../winemetal/winemetal.h"

#include <cstdint>

namespace dxmt9::presentimpl {

using u64 = std::uint64_t;

// Per-hwnd resources acquired from macdrv (or the direct-layer fallback).
// Owned by exactly one Presenter; released via releaseLayerAcquisition().
struct LayerAcquisition {
  obj_handle_t layerHandle = 0;        // retained +1 only when ownsLayer is true
  obj_handle_t metalViewHandle = 0;    // macdrv MetalView, 0 if direct-layer path
  obj_handle_t macdrvDeviceHandle = 0; // macdrv MetalDevice; 0 unless we created one
  bool ownsLayer = false;
  bool valid() const noexcept { return layerHandle != 0; }
};

// Trace an event to the queue trace log (opt-in via DXMT_QUEUE_TRACE).
void traceEvent(const char* event, u64 seqId, u64 hwnd);

// Adopt a Wine-pinned borrowed CAMetalLayer token from the ExtEscape path.
LayerAcquisition borrowLayerForHwnd(u64 hwnd, u64 layerToken, u64 seqId);

// Exact-qualified legacy aggregate-table path. The PE protocol selector must
// qualify the runtime before calling this function.
LayerAcquisition acquireLegacyLayerForHwnd(u64 hwnd, u64 seqId);

// Release all handles owned by the given LayerAcquisition.
void releaseLayerAcquisition(LayerAcquisition& acquisition);

}  // namespace dxmt9::presentimpl
