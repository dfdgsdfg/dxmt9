#include "dxmt9_presenter_macdrv.hpp"

#include "dxmt9_queue.hpp"
#include "dxmt9/wsi_surface_protocol.hpp"
#include "../winemetal/Metal.hpp"

#include <mutex>
#include <sstream>
#include <unordered_map>

namespace dxmt9::presentimpl {

namespace {

class LegacyHostViewClaimRegistry {
 public:
  bool retain(obj_handle_t view) noexcept {
    if (!view) {
      return false;
    }
    try {
      std::lock_guard lock(mutex_);
      ++claims_[view];
      return true;
    } catch (...) {
      return false;
    }
  }

  bool release(obj_handle_t view) noexcept {
    if (!view) {
      return false;
    }
    try {
      std::lock_guard lock(mutex_);
      const auto it = claims_.find(view);
      if (it == claims_.end()) {
        // Unknown ownership is never permission to destroy a Wine host view.
        return false;
      }
      const auto transition =
          wsi::releaseLegacyHostViewClaim(it->second);
      if (!transition.valid) {
        return false;
      }
      if (transition.remainingClaims == 0u) {
        claims_.erase(it);
      } else {
        it->second = transition.remainingClaims;
      }
      return transition.releaseHostView;
    } catch (...) {
      // A cold teardown synchronization failure leaks the claim rather than
      // risking a release through an aliased/stale Wine pointer.
      return false;
    }
  }

 private:
  std::mutex mutex_;
  std::unordered_map<obj_handle_t, std::size_t> claims_;
};

LegacyHostViewClaimRegistry& legacyHostViewClaims() {
  static LegacyHostViewClaimRegistry registry;
  return registry;
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
      if (!legacyHostViewClaims().retain(result.metalViewHandle)) {
        // Do not release an unregistered handle here: Wine may have returned
        // the existing view without a retain, so destroying it would poison
        // the still-current Presenter.  Balance only the resources that this
        // acquisition independently retained/created and fail closed.
        result.metalViewHandle = 0;
        NSObject_release(result.layerHandle);
        result.layerHandle = 0;
        result.ownsLayer = false;
        WMT::MacdrvMetalDevice{result.macdrvDeviceHandle}.release();
        result.macdrvDeviceHandle = 0;
        traceEvent("metal-view.claim-failed", seqId, hwnd);
        return {};
      }
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
    if (legacyHostViewClaims().release(acquisition.metalViewHandle)) {
      WMT::MacdrvMetalView{acquisition.metalViewHandle}.release();
    }
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
