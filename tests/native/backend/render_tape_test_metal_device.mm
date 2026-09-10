#import <Metal/Metal.h>

#include "render_tape_test_metal_device.hpp"

#include <cstdint>

extern "C" obj_handle_t dxmt9_test_acquire_metal_device() noexcept {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  return device ? static_cast<obj_handle_t>(reinterpret_cast<uintptr_t>(device))
                : NULL_OBJECT_HANDLE;
}

extern "C" void dxmt9_test_release_metal_device(
    obj_handle_t device) noexcept {
  if (device != NULL_OBJECT_HANDLE) {
    [(id<MTLDevice>)(uintptr_t)device release];
  }
}
