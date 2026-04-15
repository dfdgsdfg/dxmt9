#pragma once

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdint>

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

}  // namespace dxmt9::core::metalpresent
