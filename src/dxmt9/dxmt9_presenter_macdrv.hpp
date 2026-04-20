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
  obj_handle_t layerHandle = 0;        // CAMetalLayer retained +1
  obj_handle_t metalViewHandle = 0;    // macdrv MetalView, 0 if direct-layer path
  obj_handle_t macdrvDeviceHandle = 0; // macdrv MetalDevice; 0 unless we created one
  bool valid() const noexcept { return layerHandle != 0; }
};

// Trace an event to the queue trace log (opt-in via DXMT_QUEUE_TRACE).
void traceEvent(const char* event, u64 seqId, u64 hwnd);

// Acquire a CAMetalLayer for the given HWND. Returns a default-constructed
// LayerAcquisition on any failure (caller must check .valid()).
LayerAcquisition acquireLayerForHwnd(u64 hwnd, u64 seqId);

// Release all handles owned by the given LayerAcquisition.
void releaseLayerAcquisition(LayerAcquisition& acquisition);

}  // namespace dxmt9::presentimpl
