/* src/d3d9/d3d9_pe_exports.cpp — frontend-owned PE export shims.
 *
 * Object creation belongs to the D3D9 frontend layer. The win32 bridge stays
 * focused on PE/unix bootstrap and unixlib transport. */

#include "d3d9_pe_exports.hpp"

#include "d3d9_pe.hpp"
#include "dxmt9/device_c.h"
#include "util/log/log.hpp"

#include <cstdarg>

namespace {

void dxmt9PeDebugLog(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Debug, "dxmt9-pe", fmt, args);
  va_end(args);
}

}  // namespace

extern "C" IDirect3D9* WINAPI dxmt9_pe_create9(UINT sdkVersion) {
  if (sdkVersion != D3D_SDK_VERSION) {
    return nullptr;
  }

  D9CFactory* factory = dxmt9c_factory_create();
  if (!factory) {
    dxmt9PeDebugLog("create9: factory create failed");
    return nullptr;
  }

  dxmt9PeDebugLog("create9: factory=%p", factory);
  auto* result = CreateFactoryImpl(factory);
  dxmt9PeDebugLog("create9: result=%p", result);
  return result;
}

extern "C" HRESULT WINAPI dxmt9_pe_create9_ex(UINT sdkVersion,
                                              IDirect3D9Ex** ppD3D) {
  if (!ppD3D) {
    return E_POINTER;
  }
  *ppD3D = nullptr;
  if (sdkVersion != D3D_SDK_VERSION) {
    return D3DERR_INVALIDCALL;
  }

  D9CFactory* factory = dxmt9c_factory_create();
  if (!factory) {
    dxmt9PeDebugLog("create9_ex: factory create failed");
    return E_OUTOFMEMORY;
  }

  dxmt9PeDebugLog("create9_ex: factory=%p", factory);
  *ppD3D = CreateFactoryExImpl(factory);
  dxmt9PeDebugLog("create9_ex: result=%p", *ppD3D);
  return *ppD3D ? S_OK : E_OUTOFMEMORY;
}
