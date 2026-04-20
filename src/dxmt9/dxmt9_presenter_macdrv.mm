#include "dxmt9_presenter_macdrv.hpp"

#include "dxmt9_queue.hpp"
#include "../winemetal/Metal.hpp"
#include "util/config/config.hpp"
#include "util/dynamic_symbol.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <sstream>

namespace dxmt9::presentimpl {

namespace {

struct WineMacInterop {
  using GetCocoaViewFn = NSView* (*)(void*);
  using GetCocoaWindowFn = void* (*)(void*, int);
  using CreateMetalDeviceFn = void* (*)();
  using ReleaseMetalDeviceFn = void (*)(void*);
  using CreateMetalViewFn = void* (*)(void*, void*);
  using GetMetalLayerFn = void* (*)(void*);
  using ReleaseMetalViewFn = void (*)(void*);

  GetCocoaViewFn getCocoaView = nullptr;
  GetCocoaWindowFn getCocoaWindow = nullptr;
  CreateMetalDeviceFn createMetalDevice = nullptr;
  ReleaseMetalDeviceFn releaseMetalDevice = nullptr;
  CreateMetalViewFn createMetalView = nullptr;
  GetMetalLayerFn getMetalLayer = nullptr;
  ReleaseMetalViewFn releaseMetalView = nullptr;
};

WineMacInterop resolveWineMacInterop() {
  WineMacInterop interop;
  interop.getCocoaView =
      dxmt9::util::resolveDefaultSymbol<WineMacInterop::GetCocoaViewFn>("macdrv_get_cocoa_view");
  if (interop.getCocoaView) {
    return interop;
  }

  auto* functions = dxmt9::util::resolveDefaultSymbol<void* const*>("macdrv_functions", "_macdrv_functions");
  if (!functions) {
    return interop;
  }

  interop.getCocoaWindow = reinterpret_cast<WineMacInterop::GetCocoaWindowFn>(functions[3]);
  interop.createMetalDevice = reinterpret_cast<WineMacInterop::CreateMetalDeviceFn>(functions[4]);
  interop.releaseMetalDevice = reinterpret_cast<WineMacInterop::ReleaseMetalDeviceFn>(functions[5]);
  interop.createMetalView = reinterpret_cast<WineMacInterop::CreateMetalViewFn>(functions[6]);
  interop.getMetalLayer = reinterpret_cast<WineMacInterop::GetMetalLayerFn>(functions[7]);
  interop.releaseMetalView = reinterpret_cast<WineMacInterop::ReleaseMetalViewFn>(functions[8]);
  return interop;
}

const WineMacInterop& wineMacInterop() {
  static const WineMacInterop interop = resolveWineMacInterop();
  return interop;
}

bool directLayerAttachEnabled() {
  static const bool enabled = dxmt9::util::getenvFlag("DXMT_DIRECT_LAYER_ATTACH");
  return enabled;
}

NSView* getNSViewForHwnd(u64 hwnd) {
  const auto& interop = wineMacInterop();
  if (!interop.getCocoaView) {
    return nil;
  }
  return interop.getCocoaView(reinterpret_cast<void*>(static_cast<uintptr_t>(hwnd)));
}

uintptr_t toOpaqueHandle(void* value) {
  return reinterpret_cast<uintptr_t>(value);
}

CAMetalLayer* asCAMetalLayer(uintptr_t handle) {
  return reinterpret_cast<CAMetalLayer*>(handle);
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

  const auto& interop = wineMacInterop();

  // Legacy path: old macdrv exports the view directly and we install a fresh
  // CAMetalLayer on it.
  if (interop.getCocoaView) {
    __block CAMetalLayer* legacyLayer = nil;
    dispatch_sync(dispatch_get_main_queue(), ^{
      @autoreleasepool {
        NSView* view = getNSViewForHwnd(hwnd);
        if (!view) {
          return;
        }
        CAMetalLayer* newLayer = [CAMetalLayer layer];
        view.wantsLayer = YES;
        view.layer = newLayer;
        legacyLayer = newLayer;
      }
    });
    if (!legacyLayer) {
      traceEvent("legacy-view.nil", seqId, hwnd);
      return result;
    }
    result.layerHandle = static_cast<obj_handle_t>(reinterpret_cast<uintptr_t>([legacyLayer retain]));
    return result;
  }

  if (!interop.getCocoaWindow) {
    traceEvent("interop.unavailable", seqId, hwnd);
    return result;
  }

  traceEvent("table-path.begin", seqId, hwnd);
  traceEvent("metal-device.begin", seqId, hwnd);
  auto macdrvDevice = WMT::CreateMacdrvMetalDevice();
  if (!macdrvDevice) {
    traceEvent("metal-device.nil", seqId, hwnd);
    return result;
  }
  result.macdrvDeviceHandle = macdrvDevice.handle;
  traceEvent("metal-device.ok", seqId, hwnd);

  __block void* cocoaView = nullptr;
  __block bool haveWindowObject = false;
  for (int queryMode : {0, 1}) {
    traceEvent(queryMode == 0 ? "cocoa-object.begin.0" : "cocoa-object.begin.1", seqId, hwnd);
    void* cocoaObject = interop.getCocoaWindow(reinterpret_cast<void*>(static_cast<uintptr_t>(hwnd)), queryMode);
    if (!cocoaObject) {
      traceEvent(queryMode == 0 ? "cocoa-object.nil.0" : "cocoa-object.nil.1", seqId, hwnd);
      continue;
    }
    traceEvent(queryMode == 0 ? "cocoa-object.ok.0" : "cocoa-object.ok.1", seqId, hwnd);
    traceEvent("content-view.begin", seqId, hwnd);
    dispatch_sync(dispatch_get_main_queue(), ^{
      @autoreleasepool {
        id object = static_cast<id>(cocoaObject);
        if ([object isKindOfClass:[NSView class]]) {
          cocoaView = object;
          return;
        }
        if ([object isKindOfClass:[NSWindow class]]) {
          haveWindowObject = true;
          cocoaView = [static_cast<NSWindow*>(object) contentView];
        }
      }
    });
    if (cocoaView) {
      break;
    }
  }
  if (!cocoaView) {
    traceEvent(haveWindowObject ? "content-view.nil" : "cocoa-object.nil", seqId, hwnd);
    releaseLayerAcquisition(result);
    return result;
  }
  traceEvent("content-view.ok", seqId, hwnd);

  if (directLayerAttachEnabled()) {
    __block CAMetalLayer* directLayer = nil;
    traceEvent("direct-layer.begin", seqId, hwnd);
    dispatch_sync(dispatch_get_main_queue(), ^{
      @autoreleasepool {
        NSView* view = static_cast<NSView*>(cocoaView);
        if (!view) {
          return;
        }
        CAMetalLayer* newLayer = [CAMetalLayer layer];
        view.wantsLayer = YES;
        view.layer = newLayer;
        newLayer.opaque = YES;
        newLayer.frame = view.bounds;
        if (view.window.screen) {
          newLayer.contentsScale = view.window.screen.backingScaleFactor;
        } else if (NSScreen.mainScreen) {
          newLayer.contentsScale = NSScreen.mainScreen.backingScaleFactor;
        }
        directLayer = newLayer;
      }
    });
    if (directLayer) {
      traceEvent("direct-layer.ok", seqId, hwnd);
      result.layerHandle = static_cast<obj_handle_t>(reinterpret_cast<uintptr_t>([directLayer retain]));
      // macdrvDevice we created is not needed on this path.
      if (result.macdrvDeviceHandle) {
        WMT::MacdrvMetalDevice{result.macdrvDeviceHandle}.release();
        result.macdrvDeviceHandle = 0;
      }
      return result;
    }
    traceEvent("direct-layer.nil", seqId, hwnd);
  }

  traceEvent("metal-view.begin", seqId, hwnd);
  WMT::MetalLayer wrappedLayer;
  auto metalView = WMT::CreateMetalViewFromCocoaView(
      static_cast<obj_handle_t>(toOpaqueHandle(cocoaView)),
      WMT::MacdrvMetalDevice{result.macdrvDeviceHandle},
      wrappedLayer);
  if (!metalView) {
    traceEvent("metal-view.nil", seqId, hwnd);
    releaseLayerAcquisition(result);
    return result;
  }
  traceEvent("metal-view.ok", seqId, hwnd);

  if (!wrappedLayer) {
    traceEvent("metal-layer.nil", seqId, hwnd);
    metalView.release();
    releaseLayerAcquisition(result);
    return result;
  }
  traceEvent("metal-layer.ok", seqId, hwnd);

  result.layerHandle =
      static_cast<obj_handle_t>(reinterpret_cast<uintptr_t>([asCAMetalLayer(wrappedLayer.handle) retain]));
  result.metalViewHandle = static_cast<obj_handle_t>(metalView.handle);
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
