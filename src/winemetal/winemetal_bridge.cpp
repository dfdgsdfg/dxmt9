/* src/winemetal/winemetal_bridge.cpp — sole PE bridge between winemetal.dll and
 * winemetal.so.
 *
 * This module owns PE/unix bridge bootstrap and unixlib dispatch for both the
 * main dxmt9 C ABI bridge and the winemetal shader bridge.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <cstdarg>
#include <cstdlib>
#include <mutex>

#include "util/dynamic_symbol.hpp"
#include "util/log/log.hpp"
#include "dxmt9/wineunixlib.h"

namespace {

using WineLoadUnixLibFn = NTSTATUS (WINAPI *)(const UNICODE_STRING *name,
                                              unixlib_module_t *lib,
                                              unixlib_handle_t *handle);
using WineUnloadUnixLibFn = NTSTATUS (WINAPI *)(unixlib_module_t lib);
using WineUnixCallDispatcherVar = NTSTATUS (WINAPI *)(unixlib_handle_t handle,
                                                      unsigned int code,
                                                      void *args);
using WineInitUnixCallFn = NTSTATUS (WINAPI *)(void);
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
  const WCHAR* module_name = nullptr;
  bool allow_builtin_dispatcher_fallback = false;
  HMODULE ntdll = nullptr;
  WineLoadUnixLibFn load_unix_lib = nullptr;
  WineUnloadUnixLibFn unload_unix_lib = nullptr;
  WineUnixCallDispatcherVar dispatcher = nullptr;
  WineUnixCallDispatcherVar fallback_dispatcher = nullptr;
  WineInitUnixCallFn init_unix_call = nullptr;
  unixlib_handle_t* unixlib_handle_ptr = nullptr;
  unixlib_module_t module = 0;
  unixlib_handle_t handle = 0;
  unixlib_handle_t fallback_handle = 0;
  NTSTATUS status = DXMT9_STATUS_NOT_SUPPORTED;
};

BridgeState& winemetalUnixBridgeState() {
  static BridgeState state{
      .module_name = L"winemetal.so",
      .allow_builtin_dispatcher_fallback = true,
  };
  return state;
}

void bridgeDebugLog(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Debug, "winemetal-bridge", fmt, args);
  va_end(args);
}

void bridgeTraceLog(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Trace, "winemetal-bridge", fmt, args);
  va_end(args);
}

template <typename T>
T resolveProc(HMODULE module, const char *name) {
  return dxmt9::util::resolveModuleSymbol<T>(reinterpret_cast<void*>(module), name);
}

NTSTATUS initializeDispatcherOnlyFallback(BridgeState& state) {
  const auto dispatcher_export =
      dxmt9::util::resolveModuleSymbol<void*>(reinterpret_cast<void*>(state.ntdll), "__wine_unix_call_dispatcher");
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
      dxmt9::util::resolveModuleSymbol<NtQueryVirtualMemoryFn>(reinterpret_cast<void*>(state.ntdll),
                                                               "NtQueryVirtualMemory");
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

void initializeWinemetalUnixBridge() {
  auto& state = winemetalUnixBridgeState();
  state.ntdll = GetModuleHandleW(L"ntdll.dll");
  bridgeDebugLog("initialize(%ls): ntdll=%p", state.module_name, state.ntdll);
  if (!state.ntdll) {
    state.status = DXMT9_STATUS_DLL_NOT_FOUND;
    return;
  }

  const auto dispatcher_export =
      dxmt9::util::resolveModuleSymbol<void*>(reinterpret_cast<void*>(state.ntdll), "__wine_unix_call_dispatcher");
  bridgeDebugLog("initialize(%ls): dispatcher export=%p", state.module_name, dispatcher_export);
  if (dispatcher_export) {
    state.dispatcher = *reinterpret_cast<WineUnixCallDispatcherVar *>(dispatcher_export);
    bridgeDebugLog("initialize(%ls): dispatcher=%p", state.module_name, reinterpret_cast<void*>(state.dispatcher));
  }

  state.init_unix_call = resolveProc<WineInitUnixCallFn>(state.ntdll, "__wine_init_unix_call");
  state.unixlib_handle_ptr = dxmt9::util::resolveModuleSymbol<unixlib_handle_t*>(
      reinterpret_cast<void*>(state.ntdll), "__wine_unixlib_handle");
  state.load_unix_lib = resolveProc<WineLoadUnixLibFn>(state.ntdll, "__wine_load_unix_lib");
  state.unload_unix_lib = resolveProc<WineUnloadUnixLibFn>(state.ntdll, "__wine_unload_unix_lib");

  if (state.load_unix_lib && state.dispatcher) {
    UNICODE_STRING name{};
    name.Buffer = const_cast<WCHAR*>(state.module_name);
    name.Length = static_cast<USHORT>((wcslen(state.module_name)) * sizeof(WCHAR));
    name.MaximumLength = name.Length + sizeof(WCHAR);

    state.status = state.load_unix_lib(&name, &state.module, &state.handle);
    bridgeDebugLog("initialize(%ls): __wine_load_unix_lib status=0x%08lx module=0x%llx handle=0x%llx",
                   state.module_name,
                   static_cast<unsigned long>(state.status),
                   static_cast<unsigned long long>(state.module),
                   static_cast<unsigned long long>(state.handle));
    if (state.status == DXMT9_STATUS_SUCCESS) {
      return;
    }
  }

  state.status = initializeDispatcherOnlyFallback(state);
  bridgeDebugLog("initialize(%ls): final fallback status=0x%08lx handle=0x%llx dispatcher=%p",
                 state.module_name,
                 static_cast<unsigned long>(state.status),
                 static_cast<unsigned long long>(state.handle),
                 reinterpret_cast<void*>(state.dispatcher));
}

NTSTATUS ensureBridgeReady(BridgeState& state, void (*initializer)()) {
  std::call_once(state.initialized, initializer);
  return state.status;
}

}  // namespace

extern "C" NTSTATUS dxmt9_bridge_unix_call(unsigned int code, void *args) {
  auto& state = winemetalUnixBridgeState();
  const NTSTATUS status = ensureBridgeReady(state, initializeWinemetalUnixBridge);
  if (status != DXMT9_STATUS_SUCCESS) {
    bridgeDebugLog("dxmt9_bridge_unix_call: bridge not ready status=0x%08lx",
                   static_cast<unsigned long>(status));
    return status;
  }
  bridgeTraceLog("dxmt9_bridge_unix_call: handle=0x%llx code=%u dispatcher=%p",
                 static_cast<unsigned long long>(state.handle),
                 code,
                 reinterpret_cast<void*>(state.dispatcher));
  const NTSTATUS call_status = state.dispatcher(state.handle, code, args);
  if (call_status != DXMT9_STATUS_SUCCESS) {
    bridgeDebugLog("dxmt9_bridge_unix_call: code=%u args=%p status=0x%08lx",
                   code,
                   args,
                   static_cast<unsigned long>(call_status));
  }
  return call_status;
}

extern "C" NTSTATUS dxmt9_winemetal_unix_call(unsigned int code, void *args) {
  return dxmt9_bridge_unix_call(code, args);
}
