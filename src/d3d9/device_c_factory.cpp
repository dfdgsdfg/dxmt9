#include "device_c_provider.hpp"

#include "dxmt9/dxmt9_device.hpp"
#include "../winemetal/Metal.hpp"

using namespace dxmt9::d3d9::devicec;

namespace {

uint32_t usageFromResourceType(uint32_t resourceType) {
  using namespace dxmt9::core;
  switch (resourceType) {
    case 1:  // D3DRTYPE_SURFACE
    case 2:  // D3DRTYPE_VOLUME
    case 3:  // D3DRTYPE_TEXTURE
    case 4:  // D3DRTYPE_VOLUMETEXTURE
    case 5:  // D3DRTYPE_CUBETEXTURE
      return UsageTexture;
    case 6:  // D3DRTYPE_VERTEXBUFFER
      return UsageVertexBuffer;
    case 7:  // D3DRTYPE_INDEXBUFFER
      return UsageIndexBuffer;
    default:
      return 0;
  }
}

}  // namespace

extern "C" D9CFactory* dxmt9c_factory_create(void) {
  dxmt9DebugLog("factory_create begin");

  // Top-down creation: pick a WMT Metal device, wrap in the upper dxmt9::Device,
  // then hand ownership to the COM factory. Matches dxmt's D3D11CoreCreateDevice
  // / CreateDXMTDevice flow (dxmt/src/d3d11/d3d11.cpp).
  auto wmtDevices = WMT::CopyAllDevices();
  if (!wmtDevices || wmtDevices.count() == 0) {
    dxmt9DebugLog("factory_create: no WMT devices");
    return nullptr;
  }
  dxmt9::DEVICE_DESC desc{};
  desc.device = WMT::Device{wmtDevices.object(0)};
  auto device = dxmt9::CreateDXMT9Device(desc);
  if (!device) {
    dxmt9DebugLog("factory_create: CreateDXMT9Device failed");
    return nullptr;
  }

  auto* ex = dxmt9::com::Direct3DCreate9Ex(dxmt9::com::D3D_SDK_VERSION, std::move(device));
  if (!ex) {
    dxmt9DebugLog("factory_create failed");
    return nullptr;
  }
  dxmt9DebugLog("factory_create ok iface=%p", static_cast<void*>(ex));
  return new D9CFactory(ex);
}

extern "C" void dxmt9c_factory_addref(D9CFactory* f) {
  if (f) {
    f->refs.fetch_add(1);
  }
}

extern "C" uint32_t dxmt9c_factory_release(D9CFactory* f) {
  if (!f) {
    return 0;
  }
  const uint32_t refs = f->refs.fetch_sub(1) - 1;
  if (refs == 0) {
    delete f;
  }
  return refs;
}

extern "C" uint32_t dxmt9c_factory_adapter_count(D9CFactory* f) {
  return static_cast<uint32_t>(f->iface->GetAdapterCount());
}

extern "C" int32_t dxmt9c_factory_get_adapter_identifier(D9CFactory* f,
                                                         uint32_t adapter,
                                                         D9CAdapterIdentifier* out) {
  if (!out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (adapter >= f->iface->GetAdapterCount()) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto id = f->iface->GetAdapterIdentifier(adapter);
  std::memset(out, 0, sizeof(*out));
  std::strncpy(out->driver, id.driver.c_str(), sizeof(out->driver) - 1);
  std::strncpy(out->description, id.description.c_str(), sizeof(out->description) - 1);
  std::strncpy(out->deviceName, id.deviceName.c_str(), sizeof(out->deviceName) - 1);
  out->driverVersion = id.driverVersion;
  out->vendorId = id.vendorId;
  out->deviceId = id.deviceId;
  out->subSysId = id.subSysId;
  out->revision = id.revision;
  return dxmt9::core::D3D_OK;
}

extern "C" uint32_t dxmt9c_factory_get_adapter_mode_count(D9CFactory* f,
                                                          uint32_t adapter,
                                                          uint32_t d3dFmt) {
  if (adapter >= f->iface->GetAdapterCount()) {
    return 0;
  }
  auto modes = f->iface->EnumAdapterModes(adapter, fmtFromD3D(d3dFmt));
  return static_cast<uint32_t>(modes.size());
}

extern "C" int32_t dxmt9c_factory_enum_adapter_modes(D9CFactory* f, uint32_t adapter,
                                                     uint32_t d3dFmt, uint32_t modeIdx,
                                                     uint32_t* outW, uint32_t* outH,
                                                     uint32_t* outRefresh, uint32_t* outFmt) {
  if (adapter >= f->iface->GetAdapterCount()) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto modes = f->iface->EnumAdapterModes(adapter, fmtFromD3D(d3dFmt));
  if (modeIdx >= modes.size()) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto& mode = modes[modeIdx];
  if (outW) {
    *outW = mode.width;
  }
  if (outH) {
    *outH = mode.height;
  }
  if (outRefresh) {
    *outRefresh = mode.refreshRate;
  }
  if (outFmt) {
    *outFmt = fmtToD3D(mode.format);
  }
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_factory_get_adapter_display_mode(D9CFactory* f, uint32_t adapter,
                                                           uint32_t* outW, uint32_t* outH,
                                                           uint32_t* outRefresh, uint32_t* outFmt) {
  if (adapter >= f->iface->GetAdapterCount()) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  const auto mode = f->iface->GetAdapterDisplayMode(adapter);
  if (outW) {
    *outW = mode.width;
  }
  if (outH) {
    *outH = mode.height;
  }
  if (outRefresh) {
    *outRefresh = mode.refreshRate;
  }
  if (outFmt) {
    *outFmt = fmtToD3D(mode.format);
  }
  return dxmt9::core::D3D_OK;
}

extern "C" uint64_t dxmt9c_factory_get_adapter_monitor(D9CFactory* f, uint32_t adapter) {
  if (adapter >= f->iface->GetAdapterCount()) {
    return 0;
  }
  return static_cast<uint64_t>(f->iface->GetAdapterMonitor(adapter));
}

extern "C" int32_t dxmt9c_factory_check_device_type(D9CFactory* f, uint32_t adapter,
                                                    uint32_t devType, uint32_t adapterFmt,
                                                    uint32_t backFmt, uint32_t windowed) {
  return f->iface->CheckDeviceType(adapter, static_cast<dxmt9::core::DeviceType>(devType),
                                   fmtFromD3D(adapterFmt), fmtFromD3D(backFmt), windowed != 0);
}

extern "C" int32_t dxmt9c_factory_check_device_format(D9CFactory* f, uint32_t adapter,
                                                      uint32_t d3dFmt, uint32_t usage) {
  return f->iface->CheckDeviceFormat(adapter, fmtFromD3D(d3dFmt), usageFromD3D(usage));
}

extern "C" int32_t dxmt9c_factory_check_device_format2(D9CFactory* f, uint32_t adapter,
                                                       uint32_t d3dFmt, uint32_t usage,
                                                       uint32_t resourceType) {
  return f->iface->CheckDeviceFormat(adapter, fmtFromD3D(d3dFmt),
                                     usageFromD3D(usage) | usageFromResourceType(resourceType));
}

extern "C" int32_t dxmt9c_factory_check_device_multisample(D9CFactory* f, uint32_t adapter,
                                                           uint32_t d3dFmt, uint32_t msType,
                                                           uint32_t windowed) {
  if (!isSupportedD3DMultisample(msType)) {
    (void)windowed;
    return dxmt9::core::D3DERR_NOTAVAILABLE;
  }
  return f->iface->CheckDeviceMultiSampleType(adapter, fmtFromD3D(d3dFmt),
                                              msTypeFromD3D(msType));
}

extern "C" int32_t dxmt9c_factory_get_caps(D9CFactory* f, uint32_t adapter, D9CCaps* out) {
  if (!out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (adapter >= f->iface->GetAdapterCount()) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  fillCCaps(f->iface->GetDeviceCaps(adapter), out);
  out->adapterOrdinal = adapter;
  dxmt9DebugLog(
      "factory_get_caps adapter=%u vs=0x%x ps=0x%x maxTex=%ux%u maxRT=%u maxLights=%u "
      "maxStreams=%u maxAniso=%u intervals=0x%x devCaps=0x%x rasterCaps=0x%x texCaps=0x%x "
      "textureOpCaps=0x%x",
      adapter, out->vertexShaderVersion, out->pixelShaderVersion, out->maxTextureWidth,
      out->maxTextureHeight, out->numSimultaneousRTs, out->maxActiveLights, out->maxStreams,
      out->maxAnisotropy, out->presentationIntervals, out->devCaps, out->rasterCaps,
      out->textureCaps, out->textureBlendCaps);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_factory_get_adapter_luid(D9CFactory* f, uint32_t adapter,
                                                   uint32_t* lowPart, int32_t* highPart) {
  dxmt9::core::Luid luid{};
  if (!f->iface->GetAdapterLUID(adapter, &luid)) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (lowPart) {
    *lowPart = luid.lowPart;
  }
  if (highPart) {
    *highPart = luid.highPart;
  }
  return dxmt9::core::D3D_OK;
}

extern "C" D9CDevice* dxmt9c_factory_create_device(D9CFactory* f, uint32_t adapter,
                                                   const D9CPresentParams* pp,
                                                   uint32_t behaviorFlags,
                                                   const D9CDisplayModeEx* fullscreen) {
  D9CDevice* device = nullptr;
  const int32_t hr = dxmt9c_factory_create_device2(f, adapter, pp, behaviorFlags, fullscreen, &device);
  return hr == dxmt9::core::D3D_OK ? device : nullptr;
}

extern "C" int32_t dxmt9c_factory_create_device2(D9CFactory* f, uint32_t adapter,
                                                 const D9CPresentParams* pp,
                                                 uint32_t behaviorFlags,
                                                 const D9CDisplayModeEx* fullscreen,
                                                 D9CDevice** outDevice) {
  if (outDevice) {
    *outDevice = nullptr;
  }
  if (!f || !f->iface || !pp || !outDevice) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  dxmt9DebugLog(
      "factory_create_device begin adapter=%u windowed=%u size=%ux%u fmt=%u hwnd=%llu "
      "behavior=0x%x fullscreen=%d",
      adapter, pp->windowed, pp->backBufferWidth, pp->backBufferHeight, pp->backBufferFormat,
      static_cast<unsigned long long>(pp->deviceWindow), behaviorFlags, fullscreen ? 1 : 0);
  auto params = ppFromC(*pp);
  dxmt9::com::IDirect3DDevice9Ex* dev = nullptr;

  if (fullscreen) {
    auto dmex = dmExFromC(*fullscreen);
    dev = f->iface->CreateDeviceEx(adapter, params, &dmex, behaviorFlags);
  } else {
    dev = f->iface->CreateDeviceEx(adapter, params, nullptr, behaviorFlags);
  }
  if (!dev) {
    dxmt9DebugLog("factory_create_device failed");
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  dxmt9DebugLog("factory_create_device ok iface=%p", static_cast<void*>(dev));
  *outDevice = new D9CDevice(dev);
  return dxmt9::core::D3D_OK;
}
