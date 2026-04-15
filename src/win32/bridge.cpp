/* src/win32/bridge.cpp — PE bridge between d3d9.dll and dxmt9.so.
 *
 * This module owns the PE/unix bridge bootstrap and forwards d3d9 entrypoints
 * to the frontend wrappers and unix-side dxmt9.so via Wine unixlib dispatch. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <d3d9.h>
#include <cstdlib>
#include <mutex>

#include "d3d9/d3d9_pe.hpp"
#include "dxmt9/device_c.h"
#include "util/util_log.hpp"
#include "dxmt9/wineunixlib.h"

namespace {

using WineLoadUnixLibFn = NTSTATUS (WINAPI *)(const UNICODE_STRING *name,
                                              unixlib_module_t *lib,
                                              unixlib_handle_t *handle);
using WineUnloadUnixLibFn = NTSTATUS (WINAPI *)(unixlib_module_t lib);
using WineUnixCallDispatcherVar = NTSTATUS (WINAPI *)(unixlib_handle_t handle,
                                                      unsigned int code,
                                                      void *args);
using NtQueryVirtualMemoryFn = NTSTATUS (WINAPI *)(HANDLE process,
                                                   const void *base_address,
                                                   ULONG info_class,
                                                   void *buffer,
                                                   SIZE_T size,
                                                   SIZE_T *result_size);

constexpr ULONG kMemoryWineUnixFuncs = 1000;
constexpr ULONG kMemoryWineUnixWow64Funcs = 1001;

extern "C" IMAGE_DOS_HEADER __ImageBase;

struct BridgeState {
  std::once_flag initialized;
  HMODULE ntdll = nullptr;
  WineLoadUnixLibFn load_unix_lib = nullptr;
  WineUnloadUnixLibFn unload_unix_lib = nullptr;
  WineUnixCallDispatcherVar dispatcher = nullptr;
  WineUnixCallDispatcherVar fallback_dispatcher = nullptr;
  unixlib_module_t module = 0;
  unixlib_handle_t handle = 0;
  unixlib_handle_t fallback_handle = 0;
  NTSTATUS status = DXMT9_STATUS_NOT_SUPPORTED;
};

BridgeState& bridgeState() {
  static BridgeState state;
  return state;
}

void bridgeDebugLog(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Debug, "dxmt9-bridge", fmt, args);
  va_end(args);
}

template <typename T>
T resolveProc(HMODULE module, const char *name) {
  return reinterpret_cast<T>(GetProcAddress(module, name));
}

NTSTATUS initializeDispatcherOnlyFallback(BridgeState& state) {
  const auto dispatcher_export = GetProcAddress(state.ntdll, "__wine_unix_call_dispatcher");
  bridgeDebugLog("dispatcher-only fallback: export=%p", dispatcher_export);
  if (!dispatcher_export) {
    return DXMT9_STATUS_NOT_SUPPORTED;
  }

  state.fallback_dispatcher = *reinterpret_cast<WineUnixCallDispatcherVar *>(dispatcher_export);
  bridgeDebugLog("dispatcher-only fallback: dispatcher=%p", reinterpret_cast<void*>(state.fallback_dispatcher));
  if (!state.fallback_dispatcher) {
    return DXMT9_STATUS_NOT_SUPPORTED;
  }

  const auto nt_query_virtual_memory =
      reinterpret_cast<NtQueryVirtualMemoryFn>(GetProcAddress(state.ntdll, "NtQueryVirtualMemory"));
  if (!nt_query_virtual_memory) {
    return DXMT9_STATUS_NOT_SUPPORTED;
  }

  const NTSTATUS status = nt_query_virtual_memory(GetCurrentProcess(),
                                                  reinterpret_cast<void *>(&__ImageBase),
                                                  kMemoryWineUnixFuncs,
                                                  &state.fallback_handle,
                                                  sizeof(state.fallback_handle),
                                                  nullptr);
  bridgeDebugLog("dispatcher-only fallback: NtQueryVirtualMemory(1000) status=0x%08lx handle=0x%llx",
                 static_cast<unsigned long>(status),
                 static_cast<unsigned long long>(state.fallback_handle));
  if (status != DXMT9_STATUS_SUCCESS || !state.fallback_handle) {
    return status;
  }

  state.handle = state.fallback_handle;
  state.dispatcher = state.fallback_dispatcher;
  state.module = 0;
  return DXMT9_STATUS_SUCCESS;
}

void initializeBridge() {
  auto& state = bridgeState();
  state.ntdll = GetModuleHandleW(L"ntdll.dll");
  bridgeDebugLog("initialize: ntdll=%p", state.ntdll);
  if (!state.ntdll) {
    state.status = DXMT9_STATUS_DLL_NOT_FOUND;
    return;
  }

  const auto dispatcher_export = GetProcAddress(state.ntdll, "__wine_unix_call_dispatcher");
  bridgeDebugLog("initialize: dispatcher export=%p", dispatcher_export);
  if (dispatcher_export) {
    state.dispatcher = *reinterpret_cast<WineUnixCallDispatcherVar *>(dispatcher_export);
    bridgeDebugLog("initialize: dispatcher=%p", reinterpret_cast<void*>(state.dispatcher));
  }

#if defined(DXMT9_WINE_BUILTIN_DLL)
#if !defined(_WIN64)
  if (const auto nt_query_virtual_memory =
          reinterpret_cast<NtQueryVirtualMemoryFn>(GetProcAddress(state.ntdll, "NtQueryVirtualMemory"))) {
    unixlib_handle_t wow64_handle = 0;
    const NTSTATUS wow64_status = nt_query_virtual_memory(GetCurrentProcess(),
                                                          reinterpret_cast<void *>(&__ImageBase),
                                                          kMemoryWineUnixWow64Funcs,
                                                          &wow64_handle,
                                                          sizeof(wow64_handle),
                                                          nullptr);
    bridgeDebugLog("initialize: NtQueryVirtualMemory(1001) status=0x%08lx handle=0x%llx",
                   static_cast<unsigned long>(wow64_status),
                   static_cast<unsigned long long>(wow64_handle));
    if (wow64_status == DXMT9_STATUS_SUCCESS && wow64_handle) {
      state.handle = wow64_handle;
      state.status = DXMT9_STATUS_SUCCESS;
      state.module = 0;
      bridgeDebugLog("initialize: using wow64 handle, dispatcher=%p",
                     reinterpret_cast<void*>(state.dispatcher));
      return;
    }
  }
#endif
  state.status = initializeDispatcherOnlyFallback(state);
  bridgeDebugLog("initialize: builtin dispatcher-only status=0x%08lx handle=0x%llx dispatcher=%p",
                 static_cast<unsigned long>(state.status),
                 static_cast<unsigned long long>(state.handle),
                 reinterpret_cast<void*>(state.dispatcher));
  if (state.status == DXMT9_STATUS_SUCCESS && state.handle && state.dispatcher) {
    state.module = 0;
    return;
  }
#endif

  state.load_unix_lib = resolveProc<WineLoadUnixLibFn>(state.ntdll, "__wine_load_unix_lib");
  state.unload_unix_lib = resolveProc<WineUnloadUnixLibFn>(state.ntdll, "__wine_unload_unix_lib");

  if (state.load_unix_lib && state.dispatcher) {
    static WCHAR module_name[] = L"dxmt9.so";
    UNICODE_STRING name{};
    name.Buffer = module_name;
    name.Length = static_cast<USHORT>((wcslen(module_name)) * sizeof(WCHAR));
    name.MaximumLength = name.Length + sizeof(WCHAR);

    state.status = state.load_unix_lib(&name, &state.module, &state.handle);
    bridgeDebugLog("initialize: __wine_load_unix_lib status=0x%08lx module=0x%llx handle=0x%llx",
                   static_cast<unsigned long>(state.status),
                   static_cast<unsigned long long>(state.module),
                   static_cast<unsigned long long>(state.handle));
    if (state.status == DXMT9_STATUS_SUCCESS) {
      return;
    }
  }

  state.status = initializeDispatcherOnlyFallback(state);
  bridgeDebugLog("initialize: final fallback status=0x%08lx handle=0x%llx dispatcher=%p",
                 static_cast<unsigned long>(state.status),
                 static_cast<unsigned long long>(state.handle),
                 reinterpret_cast<void*>(state.dispatcher));
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
    bridgeDebugLog("unix_call: bridge not ready status=0x%08lx", static_cast<unsigned long>(status));
    return status;
  }
#if defined(DXMT9_WINE_BUILTIN_DLL)
  if (state.dispatcher) {
    bridgeDebugLog("unix_call: dispatcher handle=0x%llx code=%u dispatcher=%p",
                   static_cast<unsigned long long>(state.handle),
                   code,
                   reinterpret_cast<void*>(state.dispatcher));
    return state.dispatcher(state.handle, code, args);
  }
  bridgeDebugLog("unix_call: builtin path missing dispatcher handle=0x%llx code=%u",
                 static_cast<unsigned long long>(state.handle),
                 code);
  return DXMT9_STATUS_NOT_SUPPORTED;
#else
  return state.dispatcher(state.handle, code, args);
#endif
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
    bridgeDebugLog("create9: factory create failed");
    return nullptr;
  }
  bridgeDebugLog("create9: factory=%p", factory);
  auto* result = CreateFactoryImpl(factory);
  bridgeDebugLog("create9: result=%p", result);
  return result;
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
    bridgeDebugLog("create9_ex: factory create failed");
    return E_OUTOFMEMORY;
  }
  bridgeDebugLog("create9_ex: factory=%p", factory);
  *ppD3D = CreateFactoryExImpl(factory);
  bridgeDebugLog("create9_ex: result=%p", *ppD3D);
  return *ppD3D ? S_OK : E_OUTOFMEMORY;
}
