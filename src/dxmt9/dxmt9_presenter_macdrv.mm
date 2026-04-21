#include "dxmt9_presenter_macdrv.hpp"

#include "dxmt9_queue.hpp"
#include "../winemetal/Metal.hpp"
#include "util/dynamic_symbol.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <sstream>

namespace dxmt9::presentimpl {

namespace {

// Legacy fallback only — used when the running macdrv predates the
// `macdrv_functions` table and the individual get_win_data/... symbols
// that WMT::CreateMetalViewFromHWND needs. Looks up the view directly
// and installs a fresh CAMetalLayer on it.
using GetCocoaViewFn = NSView* (*)(void*);

GetCocoaViewFn legacyGetCocoaView() {
  static const auto fn =
      dxmt9::util::resolveDefaultSymbol<GetCocoaViewFn>("macdrv_get_cocoa_view");
  return fn;
}

NSView* getLegacyNSViewForHwnd(u64 hwnd) {
  auto fn = legacyGetCocoaView();
  if (!fn) {
    return nil;
  }
  return fn(reinterpret_cast<void*>(static_cast<uintptr_t>(hwnd)));
}

CAMetalLayer* asCAMetalLayer(uintptr_t handle) {
  return reinterpret_cast<CAMetalLayer*>(handle);
}

CAMetalLayer* installFreshLayerOnView(NSView* view) {
  __block CAMetalLayer* result = nil;
  dispatch_sync(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      if (!view) {
        return;
      }
      CAMetalLayer* newLayer = [CAMetalLayer layer];
      view.wantsLayer = YES;
      view.layer = newLayer;
      result = [newLayer retain];
    }
  });
  return result;
}

}  // namespace

void traceEvent(const char* event, u64 seqId, u64 hwnd) {
  using namespace dxmt9::core::metalqueue;
  if (!queueTraceEnabled()) {
    return;
  }
  const u64 threshold = queueTraceFromSeq();
  if (threshold != 0 && seqId != 0 && seqId < threshold) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-present] " << event
      << " seq=" << static_cast<unsigned long long>(seqId)
      << " hwnd=" << static_cast<unsigned long long>(hwnd);
  emitQueueTraceLine(out.str());
}

LayerAcquisition acquireLayerForHwnd(u64 hwnd, u64 seqId) {
  LayerAcquisition result{};
  if (!hwnd) {
    return result;
  }
  traceEvent("layer.ensure.begin", seqId, hwnd);

  // Path 1: legacy macdrv — predates `macdrv_functions` / get_win_data. The
  // `macdrv_get_cocoa_view` export returns the NSView directly; we install
  // a fresh CAMetalLayer on it.
  if (auto* view = getLegacyNSViewForHwnd(hwnd)) {
    traceEvent("legacy-view.ok", seqId, hwnd);
    CAMetalLayer* layer = installFreshLayerOnView(view);
    if (!layer) {
      traceEvent("legacy-view.nil", seqId, hwnd);
      return result;
    }
    result.layerHandle = static_cast<obj_handle_t>(reinterpret_cast<uintptr_t>(layer));
    return result;
  }

  // Path 2 (primary): winemetal's CreateMetalViewFromHWND encapsulates the
  // full macdrv_functions lookup + get_win_data + macdrv_view_create_metal_view
  // behind the thunk boundary. No direct macdrv symbol resolution here.
  auto macdrvDevice = WMT::CreateMacdrvMetalDevice();
  if (!macdrvDevice) {
    traceEvent("metal-device.nil", seqId, hwnd);
    return result;
  }
  result.macdrvDeviceHandle = macdrvDevice.handle;
  traceEvent("metal-device.ok", seqId, hwnd);

  WMT::MetalLayer layer;
  auto metalView = WMT::CreateMetalViewFromHWND(static_cast<intptr_t>(hwnd), macdrvDevice, layer);
  if (!metalView || !layer) {
    traceEvent("metal-view.nil", seqId, hwnd);
    releaseLayerAcquisition(result);
    return result;
  }
  traceEvent("metal-view.ok", seqId, hwnd);

  // Retain the layer for ourselves; winemetal returns +1 on the view but the
  // layer comes from -[macdrv_view metalLayer] which is +0.
  result.layerHandle =
      static_cast<obj_handle_t>(reinterpret_cast<uintptr_t>([asCAMetalLayer(layer.handle) retain]));
  result.metalViewHandle = metalView.handle;
  return result;
}

void releaseLayerAcquisition(LayerAcquisition& acquisition) {
  if (acquisition.metalViewHandle) {
    WMT::MacdrvMetalView{acquisition.metalViewHandle}.release();
    acquisition.metalViewHandle = 0;
  }
  if (acquisition.layerHandle) {
    [asCAMetalLayer(acquisition.layerHandle) release];
    acquisition.layerHandle = 0;
  }
  if (acquisition.macdrvDeviceHandle) {
    WMT::MacdrvMetalDevice{acquisition.macdrvDeviceHandle}.release();
    acquisition.macdrvDeviceHandle = 0;
  }
}

}  // namespace dxmt9::presentimpl
