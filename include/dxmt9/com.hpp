#pragma once

#include "dxmt9/core.hpp"

#include <atomic>

namespace dxmt9::com {

using core::HResult;
using core::u32;

inline constexpr u32 D3D_SDK_VERSION = 32;

enum class InterfaceId {
  IUnknown,
  Direct3D9,
  Direct3DDevice9,
};

class IUnknown {
 public:
  virtual ~IUnknown() = default;
  virtual u32 AddRef() = 0;
  virtual u32 Release() = 0;
  virtual bool QueryInterface(InterfaceId iid, void** object) = 0;
};

class IDirect3DDevice9;

class IDirect3D9 : public IUnknown {
 public:
  virtual size_t GetAdapterCount() const = 0;
  virtual const core::DeviceCaps& GetDeviceCaps(size_t adapterIndex) const = 0;
  virtual core::HResult CheckDeviceFormat(size_t adapterIndex, core::Format format, u32 usage) const = 0;
  virtual core::HResult CheckDeviceMultiSampleType(size_t adapterIndex, core::Format format,
                                                   core::MultiSampleType type) const = 0;
  virtual IDirect3DDevice9* CreateDevice(size_t adapterIndex, const core::PresentParameters& params,
                                         u32 behaviorFlags = 0) = 0;
};

class IDirect3DDevice9 : public IUnknown {
 public:
  virtual core::Device& coreDevice() = 0;
  virtual const core::Device& coreDevice() const = 0;
};

IDirect3D9* Direct3DCreate9(u32 sdkVersion);

}  // namespace dxmt9::com
