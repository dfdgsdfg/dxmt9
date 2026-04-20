#include "dxmt9_presenter_support.hpp"

#include "dxmt9_queue.hpp"
#include "../winemetal/Metal.hpp"
#include "util/config/config.hpp"
#include "util/dynamic_symbol.hpp"

#include <cstdlib>
#include <sstream>
#include <mutex>

namespace dxmt9::core::metalpresent {

bool directLayerAttachEnabled() {
  static const bool enabled = dxmt9::util::getenvFlag("DXMT_DIRECT_LAYER_ATTACH");
  return enabled;
}

static std::mutex gLayerRegistryMutex;
static std::unordered_map<u64, uintptr_t> gLayerRegistry;

static uintptr_t toLayerHandle(CAMetalLayer* layer) {
  return reinterpret_cast<uintptr_t>(layer);
}

static CAMetalLayer* asCAMetalLayer(uintptr_t handle) {
  return reinterpret_cast<CAMetalLayer*>(handle);
}

static uintptr_t toOpaqueHandle(void* value) {
  return reinterpret_cast<uintptr_t>(value);
}

static void* asOpaquePointer(uintptr_t handle) {
  return reinterpret_cast<void*>(handle);
}

CAMetalLayer* lookupLayerHandle(u64 handle) {
  std::lock_guard lock(gLayerRegistryMutex);
  if (auto it = gLayerRegistry.find(handle); it != gLayerRegistry.end()) {
    return asCAMetalLayer(it->second);
  }
  return nullptr;
}

void registerLayerHandle(u64 handle, CAMetalLayer* layer) {
  std::lock_guard lock(gLayerRegistryMutex);
  gLayerRegistry[handle] = toLayerHandle(layer);
}

void unregisterLayerHandle(u64 handle) {
  std::lock_guard lock(gLayerRegistryMutex);
  gLayerRegistry.erase(handle);
}

static WineMacInterop resolveWineMacInterop() {
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

NSView* get_nsview_for_hwnd(u64 hwnd) {
  const auto& interop = wineMacInterop();
  if (!interop.getCocoaView) {
    return nil;
  }
  return interop.getCocoaView(reinterpret_cast<void*>(static_cast<uintptr_t>(hwnd)));
}

PresenterState::~PresenterState() {
  for (auto& [hwnd, record] : layers_) {
    unregisterLayerHandle(hwnd);
    if (record.wineMetalViewHandle) {
      WMT::MacdrvMetalView{static_cast<obj_handle_t>(record.wineMetalViewHandle)}.release();
    }
    if (record.layerHandle) {
      [asCAMetalLayer(record.layerHandle) release];
    }
  }
  layers_.clear();
  if (wineMetalDeviceHandle_) {
    WMT::MacdrvMetalDevice{static_cast<obj_handle_t>(wineMetalDeviceHandle_)}.release();
  }
  wineMetalDeviceHandle_ = 0;
}

CAMetalLayer* PresenterState::lookupLayer(u64 hwnd) const {
  return lookupLayerHandle(hwnd);
}

void PresenterState::traceEvent(const char* event, u64 seqId, u64 windowHandle) const {
  if (!metalqueue::queueTraceEnabled()) {
    return;
  }
  const u64 threshold = metalqueue::queueTraceFromSeq();
  if (threshold != 0 && seqId != 0 && seqId < threshold) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-present] " << event
      << " seq=" << static_cast<unsigned long long>(seqId)
      << " hwnd=" << static_cast<unsigned long long>(windowHandle);
  metalqueue::emitQueueTraceLine(out.str());
}

CAMetalLayer* PresenterState::ensureLayer(u64 hwnd, u64 seqId) {
  if (!hwnd) {
    return nullptr;
  }
  traceEvent("layer.ensure.begin", seqId, hwnd);
  if (auto* layer = lookupLayerHandle(hwnd)) {
    traceEvent("layer.ensure.cached", seqId, hwnd);
    return layer;
  }

  LayerRecord record;
  const auto& interop = wineMacInterop();

  if (interop.getCocoaView) {
    __block CAMetalLayer* legacyLayer = nil;
    dispatch_sync(dispatch_get_main_queue(), ^{
      @autoreleasepool {
        NSView* view = get_nsview_for_hwnd(hwnd);
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
      return nullptr;
    }
    record.layerHandle = toLayerHandle([legacyLayer retain]);
  } else if (interop.getCocoaWindow) {
    traceEvent("table-path.begin", seqId, hwnd);
    if (!wineMetalDeviceHandle_) {
      traceEvent("metal-device.begin", seqId, hwnd);
      auto macdrvDevice = WMT::CreateMacdrvMetalDevice();
      wineMetalDeviceHandle_ = static_cast<uintptr_t>(macdrvDevice.handle);
      if (!wineMetalDeviceHandle_) {
        traceEvent("metal-device.nil", seqId, hwnd);
        return nullptr;
      }
      traceEvent("metal-device.ok", seqId, hwnd);
    } else {
      traceEvent("metal-device.cached", seqId, hwnd);
    }

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
          if (metalqueue::queueTraceEnabled()) {
            std::ostringstream out;
            out << "[dxmt9-present] cocoa-object.class"
                << " seq=" << static_cast<unsigned long long>(seqId)
                << " hwnd=" << static_cast<unsigned long long>(hwnd)
                << " mode=" << queryMode
                << " class=" << (object ? [NSStringFromClass([object class]) UTF8String] : "nil");
            metalqueue::emitQueueTraceLine(out.str());
          }
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
      return nullptr;
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
        record.layerHandle = toLayerHandle([directLayer retain]);
        CAMetalLayer* layer = asCAMetalLayer(record.layerHandle);
        registerLayerHandle(hwnd, layer);
        layers_[hwnd] = std::move(record);
        return layer;
      }
      traceEvent("direct-layer.nil", seqId, hwnd);
    }

    traceEvent("metal-view.begin", seqId, hwnd);
    WMT::MetalLayer wrappedLayer;
    auto metalView = WMT::CreateMetalViewFromCocoaView(
        static_cast<obj_handle_t>(toOpaqueHandle(cocoaView)),
        WMT::MacdrvMetalDevice{static_cast<obj_handle_t>(wineMetalDeviceHandle_)},
        wrappedLayer
    );
    if (!metalView) {
      traceEvent("metal-view.nil", seqId, hwnd);
      return nullptr;
    }
    traceEvent("metal-view.ok", seqId, hwnd);

    traceEvent("metal-layer.begin", seqId, hwnd);
    auto* layer = asCAMetalLayer(static_cast<uintptr_t>(wrappedLayer.handle));
    if (!layer) {
      if (metalView) {
        metalView.release();
      }
      traceEvent("metal-layer.nil", seqId, hwnd);
      return nullptr;
    }
    traceEvent("metal-layer.ok", seqId, hwnd);

    if (metalqueue::queueTraceEnabled()) {
      dispatch_sync(dispatch_get_main_queue(), ^{
        @autoreleasepool {
          NSView* parentView = static_cast<NSView*>(cocoaView);
          id metalViewObject = static_cast<id>(asOpaquePointer(static_cast<uintptr_t>(metalView.handle)));
          std::ostringstream out;
          out << "[dxmt9-present] metal-view.info"
              << " seq=" << static_cast<unsigned long long>(seqId)
              << " hwnd=" << static_cast<unsigned long long>(hwnd)
              << " parentClass=" << (parentView ? [NSStringFromClass([parentView class]) UTF8String] : "nil")
              << " metalClass=" << (metalViewObject ? [NSStringFromClass([metalViewObject class]) UTF8String] : "nil");
          if (parentView) {
            const NSRect bounds = parentView.bounds;
            out << " parentBounds=" << bounds.origin.x << "," << bounds.origin.y << " "
                << bounds.size.width << "x" << bounds.size.height;
          }
          if ([metalViewObject isKindOfClass:[NSView class]]) {
            NSView* metalSubview = static_cast<NSView*>(metalViewObject);
            if ([metalSubview respondsToSelector:@selector(setOpaque:)]) {
              [reinterpret_cast<id>(metalSubview) setOpaque:YES];
            }
            const NSRect frame = metalSubview.frame;
            const NSRect bounds = metalSubview.bounds;
            out << " metalFrame=" << frame.origin.x << "," << frame.origin.y << " "
                << frame.size.width << "x" << frame.size.height
                << " metalBounds=" << bounds.origin.x << "," << bounds.origin.y << " "
                << bounds.size.width << "x" << bounds.size.height
                << " hidden=" << ([metalSubview isHidden] ? 1 : 0)
                << " superClass="
                << (metalSubview.superview ? [NSStringFromClass([metalSubview.superview class]) UTF8String] : "nil");
            if (parentView) {
              const auto* subviews = parentView.subviews;
              out << " subviews=" << subviews.count;
              for (NSUInteger i = 0; i < subviews.count; ++i) {
                NSView* sibling = subviews[i];
                if (sibling == metalSubview) {
                  out << " metalIndex=" << i;
                }
              }
            }
          }
          const CGRect layerFrame = layer.frame;
          out << " layerFrame=" << layerFrame.origin.x << "," << layerFrame.origin.y << " "
              << layerFrame.size.width << "x" << layerFrame.size.height
              << " layerOpaque=" << ([layer isOpaque] ? 1 : 0);
          metalqueue::emitQueueTraceLine(out.str());
        }
      });
    }

    record.layerHandle = toLayerHandle([layer retain]);
    record.wineMetalViewHandle = static_cast<uintptr_t>(metalView.handle);
    record.usesWineMetalView = true;
  } else {
    traceEvent("interop.unavailable", seqId, hwnd);
    return nullptr;
  }

  CAMetalLayer* layer = asCAMetalLayer(record.layerHandle);
  registerLayerHandle(hwnd, layer);
  layers_[hwnd] = std::move(record);
  return layer;
}

}  // namespace dxmt9::core::metalpresent
