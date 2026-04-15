#import <dlfcn.h>

#include "dxmt9_presenter_support.hpp"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace dxmt9::core::metalpresent {

bool directLayerAttachEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT_DIRECT_LAYER_ATTACH");
    return env != nullptr && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

static std::mutex gLayerRegistryMutex;
static std::unordered_map<u64, CAMetalLayer*> gLayerRegistry;

CAMetalLayer* lookupLayerHandle(u64 handle) {
  std::lock_guard lock(gLayerRegistryMutex);
  if (auto it = gLayerRegistry.find(handle); it != gLayerRegistry.end()) {
    return it->second;
  }
  return nullptr;
}

void registerLayerHandle(u64 handle, CAMetalLayer* layer) {
  std::lock_guard lock(gLayerRegistryMutex);
  gLayerRegistry[handle] = layer;
}

void unregisterLayerHandle(u64 handle) {
  std::lock_guard lock(gLayerRegistryMutex);
  gLayerRegistry.erase(handle);
}

static WineMacInterop resolveWineMacInterop() {
  WineMacInterop interop;
  interop.getCocoaView =
      reinterpret_cast<WineMacInterop::GetCocoaViewFn>(dlsym(RTLD_DEFAULT, "macdrv_get_cocoa_view"));
  if (interop.getCocoaView) {
    return interop;
  }

  auto* functions = reinterpret_cast<void* const*>(dlsym(RTLD_DEFAULT, "macdrv_functions"));
  if (!functions) {
    functions = reinterpret_cast<void* const*>(dlsym(RTLD_DEFAULT, "_macdrv_functions"));
  }
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

}  // namespace dxmt9::core::metalpresent
