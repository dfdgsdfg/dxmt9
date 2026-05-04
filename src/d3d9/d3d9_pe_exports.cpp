/* src/d3d9/d3d9_pe_exports.cpp — frontend-owned PE export shims.
 *
 * Object creation belongs to the D3D9 frontend layer. The win32 bridge stays
 * focused on PE/unix bootstrap and unixlib transport. */

#include "d3d9_pe_exports.hpp"

#include "d3d9_pe.hpp"
#include "dxmt9/device_c.h"
#include "util/log/log.hpp"

#include <atomic>
#include <cstdarg>

namespace {

std::atomic<int> g_d3dperf_event_level{0};

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

extern "C" IDirect3D9* WINAPI dxmt9_pe_create9_on12(UINT sdkVersion,
                                                     void* d3d9On12Args,
                                                     UINT d3d9On12ArgsCount) {
  (void)d3d9On12Args;
  (void)d3d9On12ArgsCount;

  IDirect3D9Ex* d3d9 = nullptr;
  const HRESULT hr = dxmt9_pe_create9_ex(sdkVersion, &d3d9);
  if (FAILED(hr)) {
    dxmt9PeDebugLog("create9_on12: create9_ex failed hr=0x%08x", static_cast<unsigned>(hr));
    return nullptr;
  }

  return static_cast<IDirect3D9*>(d3d9);
}

extern "C" int WINAPI dxmt9_pe_perf_begin_event(D3DCOLOR, const WCHAR*) {
  return g_d3dperf_event_level.fetch_add(1, std::memory_order_relaxed);
}

extern "C" int WINAPI dxmt9_pe_perf_end_event(void) {
  return g_d3dperf_event_level.fetch_sub(1, std::memory_order_relaxed) - 1;
}

extern "C" DWORD WINAPI dxmt9_pe_perf_get_status(void) {
  return 0;
}

extern "C" BOOL WINAPI dxmt9_pe_perf_query_repeat_frame(void) {
  return FALSE;
}

extern "C" void WINAPI dxmt9_pe_perf_set_marker(D3DCOLOR, const WCHAR*) {
}

extern "C" void WINAPI dxmt9_pe_perf_set_options(DWORD) {
}

extern "C" void WINAPI dxmt9_pe_perf_set_region(D3DCOLOR, const WCHAR*) {
}

extern "C" void WINAPI dxmt9_pe_debug_set_mute(void) {
}
