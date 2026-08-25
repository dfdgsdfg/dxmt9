#include "dxmt9/core.hpp"
#include "core_private.hpp"
#include "dxmt9/assert.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "../dxmt9/dxmt9_presenter.hpp"
#include "util/util_bmp.hpp"
#include "util/config/config.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <type_traits>

namespace dxmt9::core {

using detail::DrawParamInlineStorage;
using detail::DrawPayloadArenaStorage;
using detail::kDrawRunInlineParamCapacity;
using detail::kDrawRunInlinePayloadCapacity;

namespace {

std::optional<u32> parseEnvU32(const char* name) {
  return dxmt9::util::getenvU32(name);
}

std::string getenvString(const char* name) {
  return dxmt9::util::getenvString(name);
}

void parseFrameList(const std::string& text, std::vector<u32>& out) {
  std::size_t start = 0;
  while (start < text.size()) {
    const auto comma = text.find(',', start);
    const auto end = comma == std::string::npos ? text.size() : comma;
    if (end > start) {
      const auto token = text.substr(start, end - start);
      char* tail = nullptr;
      const unsigned long value = std::strtoul(token.c_str(), &tail, 10);
      if (tail != token.c_str() && tail && *tail == '\0' &&
          value <= std::numeric_limits<u32>::max()) {
        out.push_back(static_cast<u32>(value));
      }
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
}

bool parseFrameRange(const std::string& text, u32& start, u32& end,
                     u32& interval) {
  std::array<unsigned long, 3> values{0, 0, 1};
  std::size_t count = 0;
  std::size_t pos = 0;
  while (count < values.size() && pos <= text.size()) {
    const auto colon = text.find(':', pos);
    const auto partEnd = colon == std::string::npos ? text.size() : colon;
    if (partEnd == pos) {
      return false;
    }
    const auto token = text.substr(pos, partEnd - pos);
    char* tail = nullptr;
    values[count] = std::strtoul(token.c_str(), &tail, 10);
    if (tail == token.c_str() || !tail || *tail != '\0' ||
        values[count] > std::numeric_limits<u32>::max()) {
      return false;
    }
    ++count;
    if (colon == std::string::npos) {
      break;
    }
    pos = colon + 1;
  }
  if (count < 2 || values[1] < values[0] || values[2] == 0) {
    return false;
  }
  start = static_cast<u32>(values[0]);
  end = static_cast<u32>(values[1]);
  interval = static_cast<u32>(values[2]);
  return true;
}

bool syncPresentFlushEnabled() {
  static const bool enabled = [] {
    const auto value = getenvString("DXMT9_SYNC_PRESENT_FLUSH");
    return !value.empty() && value != "0";
  }();
  return enabled;
}

bool renderTraceEnabled() {
  static const bool enabled = [] {
    return dxmt9::util::getenvFlag("DXMT_TRACE_RENDER");
  }();
  return enabled;
}

void emitRenderTrace(const char* fmt, ...) {
  if (!renderTraceEnabled()) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Info, "dxmt9-render", fmt, args);
  va_end(args);
}

}  // namespace

// hashBytes / hashString moved to src/util/util_hash.cpp so the ELF
// winemetal_dxmt9.so unix module can link them without pulling d3d9 in.

Device::Device(AdapterInfo adapter, BackendLimits limits,
               PresentParameters params, u32 behaviorFlags,
               std::shared_ptr<dxmt9::Device> upperDevice,
               bool extendedDevice)
    : adapter_(std::move(adapter)), limits_(limits),
      caps_(makeDefaultCaps(limits_)),
      backend_(upperDevice),
      upperDevice_(std::move(upperDevice)),
      presentParameters_(normalizePresentParameters(adapter_, params)), behaviorFlags_(behaviorFlags),
      extendedDevice_(extendedDevice) {
  state_.reset();
  state_.renderStates.set(RS_Z_ENABLE,
                          presentParameters_.enableAutoDepthStencil ? 1u : 0u);
  const u32 width = std::max(1u, presentParameters_.backBufferWidth);
  const u32 height = std::max(1u, presentParameters_.backBufferHeight);
  state_.viewport = {0, 0, width, height, 0.0f, 1.0f};
  state_.scissorRect = {0, 0, static_cast<i32>(width), static_cast<i32>(height)};
  deviceLost_ = false;
  maximumFrameLatency_ = kDefaultFrameLatency;
  // Identity ramp baseline — entries[i] = i << 8 per channel. Apps that
  // never call SetGammaRamp see the identity shadow in GetGammaRamp and
  // the unix-side presenter routes through the no-op fast-path.
  for (u32 i = 0; i < kMaxGammaRampEntries; ++i) {
    const u16 v = static_cast<u16>(i << 8);
    gammaRamp_.red[i] = v;
    gammaRamp_.green[i] = v;
    gammaRamp_.blue[i] = v;
  }
  gammaRampIsIdentity_ = true;
  if (backend_) {
    backend_->setMaxFrameLatency(maximumFrameLatency_);
  }
  experimentCapture_.path = getenvString("DXMT_EXPERIMENT_CAPTURE_PATH");
  experimentCapture_.dir = getenvString("DXMT_EXPERIMENT_CAPTURE_DIR");
  experimentCapture_.frame = parseEnvU32("DXMT_CAPTURE_FRAME").value_or(0);
  parseFrameList(getenvString("DXMT_CAPTURE_FRAMES"),
                 experimentCapture_.frames);
  const auto range = getenvString("DXMT_CAPTURE_RANGE");
  if (!range.empty()) {
    if (!parseFrameRange(range, experimentCapture_.rangeStart,
                         experimentCapture_.rangeEnd,
                         experimentCapture_.rangeInterval)) {
      experimentCapture_.rangeStart = 0;
      experimentCapture_.rangeEnd = 0;
      experimentCapture_.rangeInterval = 0;
    }
  }
}

Device::~Device() {
  if (backend_) {
    upperDevice_->flush();
  }
  completeUpTo(submittedSequenceId_);
  // SeqIdSafety / drain-before-teardown: pending work is drained before the
  // default-pool resources are invalidated.
  DXMT_ASSERT(completedSequenceId_ == submittedSequenceId_);
  invalidateDefaultPoolResources();
}

HResult Device::testCooperativeLevel() const {
  return deviceLost_ ? D3DERR_DEVICELOST : D3D_OK;
}

HResult Device::checkDeviceState() const {
  if (deviceLost_) {
    return D3DERR_DEVICELOST;
  }
  if (presentOccluded_) {
    return S_PRESENT_OCCLUDED;
  }
  return D3D_OK;
}

SwapDesc Device::snapshotSwapDesc() const {
  SwapDesc desc;
  desc.window = presentParameters_.deviceWindow;
  desc.width = std::max(1u, presentParameters_.backBufferWidth);
  desc.height = std::max(1u, presentParameters_.backBufferHeight);
  desc.format = presentParameters_.backBufferFormat;
  desc.interval = presentParameters_.presentationInterval;
  desc.windowed = presentParameters_.windowed;
  desc.backBufferCount = std::max(1u, presentParameters_.backBufferCount);
  desc.displaySyncEnabled = presentParameters_.presentationInterval != PresentInterval::Immediate;
  desc.multiSampleType = presentParameters_.multiSampleType;
  desc.gammaRamp = gammaRamp_;
  desc.gammaRampIsIdentity = gammaRampIsIdentity_;
  if (!swapChains_.empty()) {
    if (auto backBuffer = swapChains_[0]->backBuffer()) {
      desc.sourceSurface = backBuffer->handle();
    }
    desc.presentId = swapChains_[0]->snapshotPresentId();
  }
  return desc;
}

HResult Device::present() {
  return presentEx();
}

HResult Device::presentEx(const Rect* sourceRect, const Rect* destRect, Handle destinationWindowOverride,
                          const void* dirtyRegion, u32 flags) {
  (void)sourceRect;
  (void)destRect;
  (void)destinationWindowOverride;
  (void)dirtyRegion;
  (void)flags;
  if (deviceLost_) {
    return D3DERR_DEVICELOST;
  }
  auto desc = snapshotSwapDesc();
  // R-BACK-2.51(g) — consume the per-present pacing flag set by the
  // chunk-replay path (see setNextPresentPacedByOrdinal() doc). Any caller
  // that did not just set it (direct COM presents, synchronous non-offload
  // chunk replay) leaves the flag false here, so this present keeps the
  // inline seqId-based boundary in CommandQueue::submitPresent.
  desc.pacedByPresentOrdinal = nextPresentPacedByOrdinal_;
  nextPresentPacedByOrdinal_ = false;
  const bool synchronizePresent = desc.displaySyncEnabled;
  submitPresentInternal(desc);
  // Immediate presents must not synchronously wait for the Metal presenter:
  // some windowed apps submit before their message pump has made a drawable
  // available, and waiting here can deadlock that first frame.
  if (backend_ && synchronizePresent && syncPresentFlushEnabled()) {
    upperDevice_->flush();
  }
  completeUpTo(submittedSequenceId_);
  ++presentCount_;
  maybeCaptureExperimentFrame();
  // SeqIdSafety: a completed present must not outrun the submitted sequence.
  DXMT_ASSERT(completedSequenceId_ == submittedSequenceId_);
  return D3D_OK;
}

HResult Device::reset(const PresentParameters& params) {
  if (const auto hr = validatePresentParameters(params, false); hr != D3D_OK) {
    return hr;
  }
  return resetValidated(params);
}

HResult Device::resetEx(const PresentParameters& params, const DisplayModeEx* fullscreenMode) {
  if (const auto hr = validatePresentParameters(params, true); hr != D3D_OK) {
    return hr;
  }
  if (const auto hr = validateFullscreenModeRelation(params, fullscreenMode); hr != D3D_OK) {
    return hr;
  }
  return resetValidated(applyFullscreenMode(params, fullscreenMode));
}

HResult Device::resetValidated(const PresentParameters& params) {
  presentParameters_ = normalizePresentParameters(adapter_, params);
  deviceLost_ = false;
  presentOccluded_ = false;
  if (backend_) {
    upperDevice_->flush();
  }
  completeUpTo(submittedSequenceId_);
  // Drain-before-teardown: Reset waits for queued work to drain before
  // invalidating default-pool resources.
  DXMT_ASSERT(completedSequenceId_ == submittedSequenceId_);
  invalidateDefaultPoolResources();
  state_.reset();
  state_.renderStates.set(RS_Z_ENABLE,
                          presentParameters_.enableAutoDepthStencil ? 1u : 0u);
  const u32 width = std::max(1u, presentParameters_.backBufferWidth);
  const u32 height = std::max(1u, presentParameters_.backBufferHeight);
  state_.viewport = {0, 0, width, height, 0.0f, 1.0f};
  state_.scissorRect = {0, 0, static_cast<i32>(width), static_cast<i32>(height)};
  for (auto& chain : swapChains_) {
    if (chain) {
      chain->resize(presentParameters_);
    }
  }
  if (!swapChains_.empty() && swapChains_.front()) {
    const auto primary = swapChains_.front();
    state_.renderTargets[0] = primary->backBuffer()
                                  ? RenderTargetAttachment{primary->backBuffer()->handle(), 0,
                                                           primary->backBuffer()->multiSampleCount()}
                                  : RenderTargetAttachment{};
    state_.depthStencil = primary->depthStencilSurface()
                              ? RenderTargetAttachment{primary->depthStencilSurface()->handle(), 0,
                                                       primary->depthStencilSurface()->multiSampleCount()}
                              : RenderTargetAttachment{};
  }
  invalidateDrawStateCache(DrawStateInvalidationReset);
  submittedSequenceId_ = 0;
  completedSequenceId_ = 0;
  presentCount_ = 0;
  experimentCapture_.captured = false;
  if (backend_) {
    backend_->setMaxFrameLatency(maximumFrameLatency_);
  }
  return D3D_OK;
}

HResult Device::setMaximumFrameLatency(u32 latency) {
  if (latency > kMaxFrameLatency) {
    return D3DERR_INVALIDCALL;
  }
  maximumFrameLatency_ = latency == 0 ? kDefaultFrameLatency : latency;
  if (backend_) {
    backend_->setMaxFrameLatency(maximumFrameLatency_);
  }
  return D3D_OK;
}

HResult Device::waitForVBlank(size_t swapChainIndex) {
  auto chain = swapChain(swapChainIndex);
  if (!chain) {
    return D3DERR_INVALIDCALL;
  }
  if (backend_) {
    auto vblankDesc = makeSwapDesc(chain->params());
    vblankDesc.presentId = chain->snapshotPresentId();
    return upperDevice_->waitForVBlank(vblankDesc);
  }
  return D3D_OK;
}

HResult Device::checkResourceResidency(std::span<void* const> resources) const {
  (void)resources;
  return S_OK;
}

DisplayModeEx Device::getDisplayModeEx(size_t swapChainIndex) const {
  DisplayModeEx mode;
  const auto chain = swapChain(swapChainIndex);
  const auto& params = chain ? chain->params() : presentParameters_;
  mode.width = std::max(1u, params.backBufferWidth);
  mode.height = std::max(1u, params.backBufferHeight);
  mode.refreshRate = 60;
  mode.format = params.backBufferFormat;
  mode.scanLineOrdering = DisplayScanLineOrdering::Progressive;
  return mode;
}

HResult Device::getGPUThreadPriority(i32* priority) const {
  if (priority) {
    *priority = 0;
  }
  return D3D_OK;
}

HResult Device::setGPUThreadPriority(i32 priority) {
  (void)priority;
  return D3D_OK;
}

HResult Device::setConvolutionMonoKernel() {
  return E_NOTIMPL;
}

HResult Device::composeRects() {
  return E_NOTIMPL;
}

void Device::setGammaRamp(const GammaRamp* ramp) noexcept {
  if (!ramp) return;
  gammaRamp_ = *ramp;
  // Identity check — D3D9's documented identity table is entries[i] = i << 8
  // per channel. Recomputed lazily so the unix presenter only checks one
  // bool to decide whether to enable the gamma-apply encoder. Worst-case
  // 768 u16 compares; an order-of-magnitude cheaper than one 1.5 KB
  // setFragmentBytes per present.
  bool identity = true;
  for (u32 i = 0; identity && i < kMaxGammaRampEntries; ++i) {
    const u16 expected = static_cast<u16>(i << 8);
    if (gammaRamp_.red[i] != expected || gammaRamp_.green[i] != expected ||
        gammaRamp_.blue[i] != expected) {
      identity = false;
    }
  }
  gammaRampIsIdentity_ = identity;
}

HResult Device::checkDeviceMultiSampleType(Format format, MultiSampleType type) const {
  if (type == MultiSampleType::None) {
    return D3D_OK;
  }

  const auto supportsCount = [this](u32 count) {
    switch (count) {
      case 2:
        return limits_.supportsSampleCount2;
      case 4:
        return limits_.supportsSampleCount4;
      case 8:
        return limits_.supportsSampleCount8;
      default:
        return false;
    }
  };

  const u32 count = dxmt9::core::sampleCount(type);
  if (!supportsCount(count)) {
    return D3DERR_NOTAVAILABLE;
  }
  if (!formatSupportsUsage(format, UsageRenderTarget, limits_) &&
      !formatSupportsUsage(format, UsageDepthStencil, limits_)) {
    return D3DERR_NOTAVAILABLE;
  }
  return D3D_OK;
}

void Device::submitPresentInternal(const SwapDesc& desc) {
  if (renderTraceEnabled()) {
    emitRenderTrace("present seq=%llu window=0x%llx size=%ux%u fmt=%u windowed=%d interval=%u",
                    static_cast<unsigned long long>(submittedSequenceId_ + 1),
                    static_cast<unsigned long long>(desc.window.value),
                    desc.width,
                    desc.height,
                    static_cast<unsigned>(desc.format),
                    desc.windowed ? 1 : 0,
                    static_cast<unsigned>(desc.interval));
  }
  upperDevice_->present(desc);
  ++submittedSequenceId_;
  // SeqIdSafety: a submission can advance the current sequence, but never
  // below the completed sequence.
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
}

}  // namespace dxmt9::core
