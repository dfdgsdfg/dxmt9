#pragma once

#include "../../../src/winemetal/winemetal.h"

// MTLCreateSystemDefaultDevice is NS_RETURNS_RETAINED. The helper transfers
// that ownership as an opaque WMT handle; callers must release it only after
// every Pool/WMT::Reference using the device has been destroyed.
extern "C" obj_handle_t dxmt9_test_acquire_metal_device() noexcept;
extern "C" void dxmt9_test_release_metal_device(obj_handle_t device) noexcept;
