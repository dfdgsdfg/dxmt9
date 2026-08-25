#include "dxmt9_presenter_macdrv.hpp"

#include "dxmt9_queue.hpp"
#include "../winemetal/Metal.hpp"

#include <sstream>

namespace dxmt9::presentimpl {

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

LayerAcquisition borrowLayerForHwnd(u64 hwnd, u64 layerToken, u64 seqId) {
  LayerAcquisition result{};
  if (!hwnd || !layerToken) {
    return result;
  }
  traceEvent("layer.ensure.begin", seqId, hwnd);
  result.layerHandle = static_cast<obj_handle_t>(layerToken);
  traceEvent("extescape-layer.ok", seqId, hwnd);
  return result;
}

LayerAcquisition acquireLegacyLayerForHwnd(u64 hwnd, u64 seqId) {
  LayerAcquisition result{};
  if (!hwnd) {
    return result;
  }
  traceEvent("legacy-layer.ensure.begin", seqId, hwnd);

  // This function is reachable only after PE selected an exact manifest entry
  // qualified as legacy-macdrv-symbols:<runtime-id>. The unix helpers resolve
  // the aggregate macdrv_functions table only; direct per-symbol dlsym is not
  // supported.
  auto macdrvDevice = WMT::CreateMacdrvMetalDevice();
  if (macdrvDevice) {
    result.macdrvDeviceHandle = macdrvDevice.handle;
    traceEvent("metal-device.ok", seqId, hwnd);

    WMT::MetalLayer layer;
    auto metalView =
        WMT::CreateMetalViewFromHWND(static_cast<intptr_t>(hwnd), macdrvDevice, layer);
    if (metalView && layer) {
      traceEvent("metal-view.ok", seqId, hwnd);
      // Retain the layer for ourselves; winemetal returns +1 on the view but
      // the layer is -[macdrv_view metalLayer] which is +0.
      NSObject_retain(layer.handle);
      result.layerHandle = layer.handle;
      result.metalViewHandle = metalView.handle;
      result.ownsLayer = true;
      return result;
    }
    traceEvent("metal-view.nil", seqId, hwnd);
  } else {
    traceEvent("metal-device.nil", seqId, hwnd);
  }

  traceEvent("legacy-view.nil", seqId, hwnd);
  releaseLayerAcquisition(result);
  return result;
}

void releaseLayerAcquisition(LayerAcquisition& acquisition) {
  if (acquisition.metalViewHandle) {
    WMT::MacdrvMetalView{acquisition.metalViewHandle}.release();
    acquisition.metalViewHandle = 0;
  }
  if (acquisition.layerHandle && acquisition.ownsLayer) {
    NSObject_release(acquisition.layerHandle);
  }
  acquisition.layerHandle = 0;
  acquisition.ownsLayer = false;
  if (acquisition.macdrvDeviceHandle) {
    WMT::MacdrvMetalDevice{acquisition.macdrvDeviceHandle}.release();
    acquisition.macdrvDeviceHandle = 0;
  }
}

}  // namespace dxmt9::presentimpl
