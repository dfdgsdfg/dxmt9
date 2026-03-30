/* src/win32/bridge.cpp — PE bridge between d3d9.dll and dxmt9.so.
 *
 * This module owns the Win32 COM wrappers and forwards all dxmt9c_* / shader
 * calls to the unix-side dxmt9.so via Wine unixlib dispatch. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <d3d9.h>
#include <mutex>

#include "dxmt9/device_c.h"
#include "dxmt9/wineunixlib.h"

/* Forward declarations — implemented in factory.cpp / device.cpp */
IDirect3D9*   CreateFactoryImpl(D9CFactory* f);
IDirect3D9Ex* CreateFactoryExImpl(D9CFactory* f);

namespace {

using WineLoadUnixLibFn = NTSTATUS (WINAPI *)(const UNICODE_STRING *name,
                                              unixlib_module_t *lib,
                                              unixlib_handle_t *handle);
using WineUnloadUnixLibFn = NTSTATUS (WINAPI *)(unixlib_module_t lib);
using WineUnixCallDispatcherVar = NTSTATUS (WINAPI *)(unixlib_handle_t handle,
                                                      unsigned int code,
                                                      void *args);

struct BridgeState {
  std::once_flag initialized;
  HMODULE ntdll = nullptr;
  WineLoadUnixLibFn load_unix_lib = nullptr;
  WineUnloadUnixLibFn unload_unix_lib = nullptr;
  WineUnixCallDispatcherVar dispatcher = nullptr;
  unixlib_module_t module = 0;
  unixlib_handle_t handle = 0;
  NTSTATUS status = DXMT9_STATUS_NOT_SUPPORTED;
};

BridgeState& bridgeState() {
  static BridgeState state;
  return state;
}

template <typename T>
T resolveProc(HMODULE module, const char *name) {
  return reinterpret_cast<T>(GetProcAddress(module, name));
}

void initializeBridge() {
  auto& state = bridgeState();
  state.ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!state.ntdll) {
    state.status = DXMT9_STATUS_DLL_NOT_FOUND;
    return;
  }

  state.load_unix_lib = resolveProc<WineLoadUnixLibFn>(state.ntdll, "__wine_load_unix_lib");
  state.unload_unix_lib = resolveProc<WineUnloadUnixLibFn>(state.ntdll, "__wine_unload_unix_lib");

  const auto dispatcher_export = GetProcAddress(state.ntdll, "__wine_unix_call_dispatcher");
  if (dispatcher_export) {
    state.dispatcher = *reinterpret_cast<WineUnixCallDispatcherVar *>(dispatcher_export);
  }

  if (!state.load_unix_lib || !state.dispatcher) {
    state.status = DXMT9_STATUS_NOT_SUPPORTED;
    return;
  }

  static WCHAR module_name[] = L"dxmt9.so";
  UNICODE_STRING name{};
  name.Buffer = module_name;
  name.Length = static_cast<USHORT>((wcslen(module_name)) * sizeof(WCHAR));
  name.MaximumLength = name.Length + sizeof(WCHAR);

  state.status = state.load_unix_lib(&name, &state.module, &state.handle);
}

NTSTATUS ensureBridgeReady() {
  auto& state = bridgeState();
  std::call_once(state.initialized, initializeBridge);
  return state.status;
}

}  // namespace

extern "C" NTSTATUS dxmt9_bridge_unix_call(unsigned int code, void *args) {
  auto& state = bridgeState();
  const NTSTATUS status = ensureBridgeReady();
  if (status != DXMT9_STATUS_SUCCESS) {
    return status;
  }
  return state.dispatcher(state.handle, code, args);
}

extern "C" __declspec(dllexport) IDirect3D9* WINAPI dxmt9_bridge_create9(UINT sdkVersion) {
  if (sdkVersion != D3D_SDK_VERSION) {
    return nullptr;
  }
  const NTSTATUS status = ensureBridgeReady();
  if (status != DXMT9_STATUS_SUCCESS) {
    return nullptr;
  }
  D9CFactory *factory = dxmt9c_factory_create();
  if (!factory) {
    return nullptr;
  }
  return CreateFactoryImpl(factory);
}

extern "C" __declspec(dllexport) HRESULT WINAPI dxmt9_bridge_create9_ex(UINT sdkVersion,
                                                                         IDirect3D9Ex **ppD3D) {
  if (!ppD3D) {
    return E_POINTER;
  }
  *ppD3D = nullptr;
  if (sdkVersion != D3D_SDK_VERSION) {
    return D3DERR_INVALIDCALL;
  }
  const NTSTATUS status = ensureBridgeReady();
  if (status != DXMT9_STATUS_SUCCESS) {
    return static_cast<HRESULT>(status);
  }
  D9CFactory *factory = dxmt9c_factory_create();
  if (!factory) {
    return E_OUTOFMEMORY;
  }
  *ppD3D = CreateFactoryExImpl(factory);
  return *ppD3D ? S_OK : E_OUTOFMEMORY;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
  } else if (reason == DLL_PROCESS_DETACH) {
    auto& state = bridgeState();
    if (state.unload_unix_lib && state.module) {
      state.unload_unix_lib(state.module);
      state.module = 0;
      state.handle = 0;
    }
  }
  return TRUE;
}
