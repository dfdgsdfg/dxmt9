#pragma once

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdint>
#include <unordered_map>

namespace dxmt9::core::metalpresent {

using u64 = std::uint64_t;

bool directLayerAttachEnabled();

CAMetalLayer* lookupLayerHandle(u64 handle);
void registerLayerHandle(u64 handle, CAMetalLayer* layer);
void unregisterLayerHandle(u64 handle);

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

const WineMacInterop& wineMacInterop();
NSView* get_nsview_for_hwnd(u64 hwnd);

class PresenterState {
 public:
  PresenterState() = default;
  ~PresenterState();

  CAMetalLayer* lookupLayer(u64 hwnd) const;
  CAMetalLayer* ensureLayer(u64 hwnd, u64 seqId);
  void traceEvent(const char* event, u64 seqId, u64 windowHandle) const;

 private:
  struct LayerRecord {
    uintptr_t layerHandle = 0;
    uintptr_t wineMetalViewHandle = 0;
    bool usesWineMetalView = false;
  };

  std::unordered_map<u64, LayerRecord> layers_;
  uintptr_t wineMetalDeviceHandle_ = 0;
};

}  // namespace dxmt9::core::metalpresent
