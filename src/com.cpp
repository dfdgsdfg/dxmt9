#include "dxmt9/com.hpp"

namespace dxmt9::com {

namespace {

class Direct3DDevice9Impl final : public IDirect3DDevice9 {
 public:
  explicit Direct3DDevice9Impl(std::shared_ptr<core::Device> device) : device_(std::move(device)) {}

  u32 AddRef() override { return ++refCount_; }

  u32 Release() override {
    const u32 next = --refCount_;
    if (next == 0) {
      delete this;
    }
    return next;
  }

  bool QueryInterface(InterfaceId iid, void** object) override {
    if (!object) {
      return false;
    }
    switch (iid) {
      case InterfaceId::IUnknown:
      case InterfaceId::Direct3DDevice9:
        *object = static_cast<IDirect3DDevice9*>(this);
        AddRef();
        return true;
      default:
        *object = nullptr;
        return false;
    }
  }

  core::Device& coreDevice() override { return *device_; }
  const core::Device& coreDevice() const override { return *device_; }

 private:
  std::atomic<u32> refCount_{1};
  std::shared_ptr<core::Device> device_;
};

class Direct3D9Impl final : public IDirect3D9 {
 public:
  explicit Direct3D9Impl(core::BackendLimits limits = {}, std::shared_ptr<core::BackendDevice> backend = {})
      : factory_(limits, std::move(backend)) {}

  u32 AddRef() override { return ++refCount_; }

  u32 Release() override {
    const u32 next = --refCount_;
    if (next == 0) {
      delete this;
    }
    return next;
  }

  bool QueryInterface(InterfaceId iid, void** object) override {
    if (!object) {
      return false;
    }
    switch (iid) {
      case InterfaceId::IUnknown:
      case InterfaceId::Direct3D9:
        *object = static_cast<IDirect3D9*>(this);
        AddRef();
        return true;
      default:
        *object = nullptr;
        return false;
    }
  }

  size_t GetAdapterCount() const override { return factory_.adapterCount(); }

  const core::DeviceCaps& GetDeviceCaps(size_t adapterIndex) const override { return factory_.caps(adapterIndex); }

  core::HResult CheckDeviceFormat(size_t adapterIndex, core::Format format, u32 usage) const override {
    return factory_.checkDeviceFormat(adapterIndex, format, usage);
  }

  core::HResult CheckDeviceMultiSampleType(size_t adapterIndex, core::Format format,
                                           core::MultiSampleType type) const override {
    return factory_.checkDeviceMultiSampleType(adapterIndex, format, type);
  }

  IDirect3DDevice9* CreateDevice(size_t adapterIndex, const core::PresentParameters& params,
                                 u32 behaviorFlags = 0) override {
    auto device = factory_.createDevice(adapterIndex, params, behaviorFlags);
    if (!device) {
      return nullptr;
    }
    return new Direct3DDevice9Impl(std::move(device));
  }

 private:
  std::atomic<u32> refCount_{1};
  core::Factory factory_;
};

}  // namespace

IDirect3D9* Direct3DCreate9(u32 sdkVersion) {
  if (sdkVersion != D3D_SDK_VERSION) {
    return nullptr;
  }
  return new Direct3D9Impl();
}

}  // namespace dxmt9::com
