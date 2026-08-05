#include "core_private.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "util/config/config.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dxmt9::core {

namespace {

std::optional<u32> parseEnvU32Auto(const char *name) {
  return dxmt9::util::getenvU32Auto(name);
}

std::string getenvString(const char *name) {
  return dxmt9::util::getenvString(name);
}

struct NullBackendDevice final : BackendDevice {
  BufferHandle createBuffer(const BufferDesc &) override {
    return Handle{++next_};
  }

  TextureHandle createTexture(const TextureDesc &) override {
    return Handle{++next_};
  }

  void destroyBuffer(BufferHandle) override {}
  void destroyTexture(TextureHandle) override {}
  void submitClear(const ClearDesc &) override {}
  void present(const SwapDesc &) override {}

private:
  u64 next_ = 1000;
};

} // namespace

namespace {

// Test-only wrapper: builds a dxmt9::Device around an externally-provided
// BackendDevice. Returns a null WMT::Device — tests don't exercise the
// Metal surface, only the core object graph.
class StubDxmt9Device final : public dxmt9::Device {
public:
  StubDxmt9Device(BackendLimits limits, std::shared_ptr<BackendDevice> backend)
      : limits_(limits),
        queue_(WMT::Device{NULL_OBJECT_HANDLE}, limits_, false),
        backend_(std::move(backend)) {}
  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  dxmt9::CommandQueue &queue() override { return queue_; }
  const BackendLimits &limits() const override { return limits_; }
  std::shared_ptr<BackendDevice> backend() override { return backend_; }

  // Tests drive the backend's trigger* helpers directly, expecting the
  // observer wired by Factory::createDevice to fire. Forward through so the
  // mock backend stays the source of truth on the stub path.
  void
  setDeviceLostObserver(BackendDevice::DeviceLostObserver observer) override {
    if (backend_)
      backend_->setDeviceLostObserver(std::move(observer));
  }
  void setPresentationStatusObserver(
      BackendDevice::PresentationStatusObserver observer) override {
    if (backend_)
      backend_->setPresentationStatusObserver(std::move(observer));
  }
  void setMaxFrameLatency(std::uint32_t latency) override {
    if (backend_)
      backend_->setMaxFrameLatency(latency);
  }

  // Resource-ops + submit forwarding. Tests use mock backends as the source
  // of truth, while production submission stays on dxmt9::Device.
  BufferHandle createBuffer(const BufferDesc &desc) override {
    return backend_ ? backend_->createBuffer(desc) : BufferHandle{};
  }
  TextureHandle createTexture(const TextureDesc &desc) override {
    return backend_ ? backend_->createTexture(desc) : TextureHandle{};
  }
  SurfaceHandle createSurface(const SurfaceDesc &desc) override {
    return backend_ ? backend_->createSurface(desc) : SurfaceHandle{};
  }
  SurfaceHandle createSurfaceForTexture(TextureHandle handle,
                                        std::uint32_t level,
                                        const SurfaceDesc &desc) override {
    return backend_ ? backend_->createSurfaceForTexture(handle, level, desc)
                    : SurfaceHandle{};
  }
  void destroyBuffer(BufferHandle handle) override {
    if (backend_)
      backend_->destroyBuffer(handle);
  }
  void destroyTexture(TextureHandle handle) override {
    if (backend_)
      backend_->destroyTexture(handle);
  }
  void destroySurface(SurfaceHandle handle) override {
    if (backend_)
      backend_->destroySurface(handle);
  }
  void *mapBuffer(BufferHandle handle, std::uint32_t flags) override {
    return backend_ ? backend_->mapBuffer(handle, flags) : nullptr;
  }
  void unmapBuffer(BufferHandle handle) override {
    if (backend_)
      backend_->unmapBuffer(handle);
  }
  void uploadBufferData(BufferHandle handle,
                        std::span<const std::uint8_t> bytes) override {
    if (backend_)
      backend_->uploadBufferData(handle, bytes);
  }
  void uploadTextureLevel(TextureHandle handle, std::uint32_t level,
                          std::uint32_t w, std::uint32_t h,
                          std::uint32_t depth, std::uint32_t pitch,
                          std::uint32_t slicePitch,
                          std::span<const std::uint8_t> bytes) override {
    if (backend_)
      backend_->uploadTextureLevel(handle, level, w, h, depth, pitch,
                                   slicePitch, bytes);
  }
  void submitDrawRun(CanonicalDrawState state,
                     const DrawUniformPayload &uniforms,
                     std::span<const DrawParam> draws,
                     std::span<const DrawParamPayloadView> payloads) override {
    if (!backend_) {
      return;
    }
    backend_->submitDrawRun(std::move(state), uniforms, draws, payloads);
  }
  void submitClear(const ClearDesc &desc) override {
    if (backend_)
      backend_->submitClear(desc);
  }
  void submitSurfaceCopy(const SurfaceCopyDesc &desc) override {
    if (backend_)
      backend_->submitSurfaceCopy(desc);
  }
  void submitStretchRect(const StretchRectDesc &desc) override {
    if (backend_)
      backend_->submitStretchRect(desc);
  }
  void submitReadback(const ReadbackDesc &desc) override {
    if (backend_)
      backend_->submitReadback(desc);
  }
  void submitColorFill(const ColorFillDesc &desc) override {
    if (backend_)
      backend_->submitColorFill(desc);
  }
  void submitDepthResolve(const DepthResolveDesc &desc) override {
    if (backend_)
      backend_->submitDepthResolve(desc);
  }
  void present(const SwapDesc &desc) override {
    if (backend_)
      backend_->present(desc);
  }
  void flush() override {
    if (backend_)
      backend_->flush();
  }
  HResult waitForVBlank(const SwapDesc &desc) override {
    return backend_ ? backend_->waitForVBlank(desc) : HResult{0};
  }
  bool readbackSurface(const ReadbackDesc &desc,
                       ReadbackPixels &pixels) override {
    return backend_ && backend_->readbackSurface(desc, pixels);
  }

private:
  BackendLimits limits_{};
  dxmt9::CommandQueue queue_;
  std::shared_ptr<BackendDevice> backend_;
};

} // namespace

// Exposed for com.cpp's backward-compat Direct3DCreate9/Ex overloads.
std::shared_ptr<dxmt9::Device>
makeStubDxmt9Device(BackendLimits limits,
                    std::shared_ptr<BackendDevice> backend) {
  if (!backend) {
    backend = std::make_shared<NullBackendDevice>();
  }
  return std::make_shared<StubDxmt9Device>(limits, std::move(backend));
}

namespace {

// Test convenience: build a real dxmt9::Device by selecting the first WMT
// device. Falls back to a NullBackendDevice wrapper if no WMT devices are
// available. Used by Factory(BackendLimits) for tests that exercise the
// real Metal backend without going through dxmt9c_factory_create().
std::shared_ptr<dxmt9::Device> bootstrapDeviceForTests(BackendLimits limits) {
  auto wmtDevices = WMT::CopyAllDevices();
  if (wmtDevices && wmtDevices.count() > 0) {
    dxmt9::DEVICE_DESC desc{};
    desc.device = WMT::Device{wmtDevices.object(0)};
    desc.limits = limits;
    if (auto upper = dxmt9::CreateDXMT9Device(desc)) {
      return std::shared_ptr<dxmt9::Device>(std::move(upper));
    }
  }
  return makeStubDxmt9Device(limits, std::make_shared<NullBackendDevice>());
}

} // namespace

Factory::Factory(std::shared_ptr<dxmt9::Device> device)
    : device_(std::move(device)),
      limits_(device_ ? device_->limits() : BackendLimits{}) {
  AdapterInfo adapter;
  adapter.ordinal = 0;
  adapter.name = getenvString("DXMT_ADAPTER_NAME");
  if (adapter.name.empty()) {
    adapter.name = "NVIDIA GeForce 6800";
  }
  adapter.registryId = 1;
  adapter.displayId = 1;
  adapter.displayMode = {1920, 1080, 60, Format::A8R8G8B8};
  adapters_.push_back(std::move(adapter));
  adapterCaps_.push_back(makeDefaultCaps(limits_));
}

Factory::Factory(BackendLimits limits, std::shared_ptr<BackendDevice> backend)
    : Factory(makeStubDxmt9Device(
          limits, backend ? std::move(backend)
                          : std::make_shared<NullBackendDevice>())) {}

Factory::Factory(BackendLimits limits)
    : Factory(bootstrapDeviceForTests(limits)) {}

const AdapterInfo &Factory::adapter(size_t index) const {
  if (index >= adapters_.size()) {
    throw std::out_of_range("adapter index out of range");
  }
  return adapters_[index];
}

const DeviceCaps &Factory::caps(size_t index) const {
  if (index >= adapterCaps_.size()) {
    throw std::out_of_range("caps index out of range");
  }
  return adapterCaps_[index];
}

namespace {

// FNV-1a 64-bit hash. Deterministic, byte-stable, no dependency on
// time / RNG / hardware state. Used to derive D3DADAPTER_IDENTIFIER9
// ::DeviceIdentifier from (vendorId, deviceId, description) per the
// gap.md §C.9 fix (R-CAPS-DEVICE-IDENTIFIER). Two consecutive calls
// produce byte-equal output; two distinct adapters produce different
// output with FNV-quality collision resistance (sufficient for an
// installation fingerprint — these are not security tokens).
constexpr u64 kFnvOffsetBasis = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;

u64 fnv1a64(u64 seed, const void *data, std::size_t bytes) {
  const auto *p = static_cast<const u8 *>(data);
  for (std::size_t i = 0; i < bytes; ++i) {
    seed ^= static_cast<u64>(p[i]);
    seed *= kFnvPrime;
  }
  return seed;
}

// Pack two FNV-1a 64-bit digests into the 16-byte GUID layout. The
// two hash inputs are kept disjoint (different seeds + length tag) so
// the high and low halves of the GUID do not collide for adapters
// that differ only in description length.
std::array<u8, 16> makeAdapterDeviceIdentifier(u32 vendorId, u32 deviceId,
                                               const std::string &description) {
  u64 lo = kFnvOffsetBasis;
  lo = fnv1a64(lo, &vendorId, sizeof(vendorId));
  lo = fnv1a64(lo, &deviceId, sizeof(deviceId));
  lo = fnv1a64(lo, description.data(), description.size());

  // Independent digest seeded with a different basis + length tag so
  // the upper 8 bytes are not a trivial transform of the lower 8.
  constexpr u64 kSecondaryBasis = 0x9e3779b97f4a7c15ull;
  const u64 descLen = static_cast<u64>(description.size());
  u64 hi = kSecondaryBasis;
  hi = fnv1a64(hi, &descLen, sizeof(descLen));
  hi = fnv1a64(hi, &deviceId, sizeof(deviceId));
  hi = fnv1a64(hi, &vendorId, sizeof(vendorId));
  hi = fnv1a64(hi, description.data(), description.size());

  std::array<u8, 16> bytes{};
  for (std::size_t i = 0; i < 8; ++i) {
    bytes[i] = static_cast<u8>((lo >> (i * 8)) & 0xffu);
    bytes[i + 8] = static_cast<u8>((hi >> (i * 8)) & 0xffu);
  }
  return bytes;
}

} // namespace

AdapterIdentifier Factory::getAdapterIdentifier(size_t index) const {
  const auto &info = adapter(index);
  AdapterIdentifier identifier;
  identifier.description = info.name;
  identifier.deviceName = "\\\\.\\DISPLAY1";
  identifier.driver = getenvString("DXMT_ADAPTER_DRIVER");
  if (identifier.driver.empty()) {
    identifier.driver = "nvd3dum.dll";
  }
  identifier.driverVersion = info.registryId;
  identifier.vendorId =
      parseEnvU32Auto("DXMT_ADAPTER_VENDOR_ID").value_or(0x10deu);
  identifier.deviceId =
      parseEnvU32Auto("DXMT_ADAPTER_DEVICE_ID").value_or(0x0041u);
  identifier.subSysId = 0;
  identifier.revision = 0;
  identifier.monitor = info.displayId;
  // gap.md §C.9: derive a byte-stable per-adapter GUID so apps that
  // use D3DADAPTER_IDENTIFIER9::DeviceIdentifier as an installation
  // fingerprint do not see the all-zero GUID and refuse to launch.
  identifier.deviceIdentifier = makeAdapterDeviceIdentifier(
      identifier.vendorId, identifier.deviceId, identifier.description);
  // Apple Silicon GPUs are not WHQL-certified; D3D9 spec permits 0.
  identifier.whqlLevel = 0;
  return identifier;
}

std::vector<DisplayMode> Factory::enumAdapterModes(size_t index,
                                                   Format format) const {
  if (index >= adapters_.size()) {
    return {};
  }
  return makeAdapterModes(format, limits_);
}

DisplayMode Factory::getAdapterDisplayMode(size_t index) const {
  return adapter(index).displayMode;
}

u32 Factory::getAdapterMonitor(size_t index) const {
  return adapter(index).displayId;
}

HRESULT Factory::checkDeviceType(size_t adapterIndex, DeviceType deviceType,
                                 Format adapterFormat, Format backBufferFormat,
                                 bool windowed) const {
  if (adapterIndex >= adapters_.size()) {
    return D3DERR_INVALIDCALL;
  }
  if (deviceType != DeviceType::Hal) {
    return D3DERR_NOTAVAILABLE;
  }
  if (windowed) {
    if (!isDisplayModeFormat(adapterFormat) ||
        !isDisplayModeFormat(backBufferFormat)) {
      return D3DERR_NOTAVAILABLE;
    }
  } else {
    if (!isDisplayModeFormat(adapterFormat) ||
        !isDisplayModeFormat(backBufferFormat)) {
      return D3DERR_NOTAVAILABLE;
    }
    if (adapterFormat != backBufferFormat) {
      return D3DERR_NOTAVAILABLE;
    }
    if (enumAdapterModes(adapterIndex, backBufferFormat).empty()) {
      return D3DERR_NOTAVAILABLE;
    }
  }
  if (!formatSupportsUsage(adapterFormat, UsageRenderTarget, limits_) ||
      !formatSupportsUsage(backBufferFormat, UsageRenderTarget, limits_)) {
    return D3DERR_NOTAVAILABLE;
  }
  return D3D_OK;
}

HRESULT Factory::checkDeviceFormat(size_t adapterIndex, Format format,
                                   u32 usage) const {
  if (adapterIndex >= adapters_.size()) {
    return D3DERR_INVALIDCALL;
  }
  return formatSupportsUsage(format, usage, limits_) ? D3D_OK
                                                     : D3DERR_NOTAVAILABLE;
}

HRESULT Factory::checkDeviceMultiSampleType(size_t adapterIndex, Format format,
                                            MultiSampleType type) const {
  if (adapterIndex >= adapters_.size()) {
    return D3DERR_INVALIDCALL;
  }

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

std::shared_ptr<Device> Factory::createDevice(size_t adapterIndex,
                                              const PresentParameters &params,
                                              u32 behaviorFlags) {
  if (validatePresentParameters(params, false) != D3D_OK) {
    return {};
  }
  return createDeviceValidated(adapterIndex, params, behaviorFlags, false);
}

std::shared_ptr<Device>
Factory::createDeviceEx(size_t adapterIndex, const PresentParameters &params,
                        const DisplayModeEx *fullscreenMode,
                        u32 behaviorFlags) {
  if (const auto hr = validatePresentParameters(params, true); hr != D3D_OK) {
    return {};
  }
  if (const auto hr = validateFullscreenModeRelation(params, fullscreenMode);
      hr != D3D_OK) {
    return {};
  }
  return createDeviceValidated(adapterIndex,
                               applyFullscreenMode(params, fullscreenMode),
                               behaviorFlags, true);
}

std::shared_ptr<Device>
Factory::createDeviceValidated(size_t adapterIndex,
                               const PresentParameters &params,
                               u32 behaviorFlags, bool extendedDevice) {
  if (adapterIndex >= adapters_.size()) {
    return {};
  }
  const auto &adapterInfo = adapters_[adapterIndex];
  const auto normalized = normalizePresentParameters(adapterInfo, params);
  const auto fullscreenAdapterFormat = normalized.windowed
                                           ? adapterInfo.displayMode.format
                                           : normalized.backBufferFormat;
  if (checkDeviceType(adapterIndex, DeviceType::Hal, fullscreenAdapterFormat,
                      normalized.backBufferFormat,
                      normalized.windowed) != D3D_OK) {
    return {};
  }
  if (!normalized.windowed) {
    const auto modes =
        enumAdapterModes(adapterIndex, normalized.backBufferFormat);
    const auto match =
        std::find_if(modes.begin(), modes.end(), [&](const DisplayMode &mode) {
          return mode.width == normalized.backBufferWidth &&
                 mode.height == normalized.backBufferHeight;
        });
    if (match == modes.end()) {
      return {};
    }
  }
  auto device = std::shared_ptr<Device>(new Device(adapterInfo, limits_,
                                                   normalized, behaviorFlags,
                                                   device_, extendedDevice));
  device->initializeDefaultSwapChain();
  if (device_) {
    std::weak_ptr<Device> weak = device;
    device_->setDeviceLostObserver([weak](bool lost) {
      if (auto locked = weak.lock()) {
        locked->setDeviceLost(lost);
      }
    });
    device_->setPresentationStatusObserver([weak](bool occluded) {
      if (auto locked = weak.lock()) {
        locked->setPresentOccluded(occluded);
      }
    });
    device_->setMaxFrameLatency(device->maximumFrameLatency());
  }
  return device;
}

} // namespace dxmt9::core
