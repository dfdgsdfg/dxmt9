#include "dxmt9_presenter_macdrv.hpp"

#include "dxmt9_queue.hpp"
#include "../winemetal/Metal.hpp"
#include "util/log/log.hpp"

#include <atomic>
#include <sstream>

namespace dxmt9::presentimpl {

namespace {

// R-WMB-6.2 / test_wild.rules.md: the experiment harness needs to surface
// which WSI layer acquisition path was selected without requiring the opt-in
// DXMT_TRACE_QUEUE channel. Emit one Warn-level line per process so the
// default-Warn run_experiment.py parser can populate
// result.json:wsi.layer_acquisition without requiring DXMT_LOG_LEVEL=Info.
// The atomic guard keeps this to a single emission per process even under
// concurrent first-acquire calls.
std::atomic<bool> g_layerAcquisitionLogged{false};

void logLayerAcquisitionOnce(const char* path, u64 hwnd) {
  bool expected = false;
  if (!g_layerAcquisitionLogged.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    return;
  }
  dxmt9::util::logf(dxmt9::util::LogLevel::Warn, "dxmt9-wsi",
                    "layer_acquisition=%s hwnd=0x%llx",
                    path,
                    static_cast<unsigned long long>(hwnd));
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

  // Path 1 (primary): winemetal's CreateMetalViewFromHWND wraps
  // macdrv_functions / get_win_data / macdrv_view_create_metal_view.
  auto macdrvDevice = WMT::CreateMacdrvMetalDevice();
  if (macdrvDevice) {
    result.macdrvDeviceHandle = macdrvDevice.handle;
    traceEvent("metal-device.ok", seqId, hwnd);

    WMT::MetalLayer layer;
    auto metalView =
        WMT::CreateMetalViewFromHWND(static_cast<intptr_t>(hwnd), macdrvDevice, layer);
    if (metalView && layer) {
      traceEvent("metal-view.ok", seqId, hwnd);
      logLayerAcquisitionOnce("macdrv_functions", hwnd);
      // Retain the layer for ourselves; winemetal returns +1 on the view but
      // the layer is -[macdrv_view metalLayer] which is +0.
      NSObject_retain(layer.handle);
      result.layerHandle = layer.handle;
      result.metalViewHandle = metalView.handle;
      return result;
    }
    traceEvent("metal-view.nil", seqId, hwnd);
  } else {
    traceEvent("metal-device.nil", seqId, hwnd);
  }

  // Path 2 (legacy fallback): old macdrv with only macdrv_get_cocoa_view.
  // winemetal::AcquireLegacyHwndLayer installs a fresh CAMetalLayer on the
  // window's NSView — all ObjC lives there, not here.
  obj_handle_t legacyLayer = ::AcquireLegacyHwndLayer(static_cast<intptr_t>(hwnd));
  if (legacyLayer) {
    traceEvent("legacy-view.ok", seqId, hwnd);
    logLayerAcquisitionOnce("legacy_macdrv_get_cocoa_view", hwnd);
    // Release the macdrv device acquired above; legacy path doesn't need it.
    if (result.macdrvDeviceHandle) {
      WMT::MacdrvMetalDevice{result.macdrvDeviceHandle}.release();
      result.macdrvDeviceHandle = 0;
    }
    result.layerHandle = legacyLayer;
    return result;
  }

  traceEvent("legacy-view.nil", seqId, hwnd);
  logLayerAcquisitionOnce("fallback_nil", hwnd);
  releaseLayerAcquisition(result);
  return result;
}

void releaseLayerAcquisition(LayerAcquisition& acquisition) {
  if (acquisition.metalViewHandle) {
    WMT::MacdrvMetalView{acquisition.metalViewHandle}.release();
    acquisition.metalViewHandle = 0;
  }
  if (acquisition.layerHandle) {
    NSObject_release(acquisition.layerHandle);
    acquisition.layerHandle = 0;
  }
  if (acquisition.macdrvDeviceHandle) {
    WMT::MacdrvMetalDevice{acquisition.macdrvDeviceHandle}.release();
    acquisition.macdrvDeviceHandle = 0;
  }
}

}  // namespace dxmt9::presentimpl
