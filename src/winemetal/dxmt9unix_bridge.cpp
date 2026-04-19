#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

#include <mutex>

#include "dxmt9/wineunixlib.h"
#include "util/dynamic_symbol.hpp"

namespace {

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

extern "C" IMAGE_DOS_HEADER __ImageBase;

struct ProviderBridgeState {
  std::once_flag initialized;
  HMODULE ntdll = nullptr;
  WineUnixCallDispatcherVar dispatcher = nullptr;
  unixlib_handle_t handle = 0;
  NTSTATUS status = DXMT9_STATUS_NOT_SUPPORTED;
};

ProviderBridgeState& providerBridgeState() {
  static ProviderBridgeState state;
  return state;
}

template <typename T>
T resolveProc(HMODULE module, const char *name) {
  return dxmt9::util::resolveModuleSymbol<T>(reinterpret_cast<void*>(module), name);
}

void initializeProviderBridge() {
  auto& state = providerBridgeState();
  state.ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!state.ntdll) {
    state.status = DXMT9_STATUS_DLL_NOT_FOUND;
    return;
  }

  const auto dispatcher_export =
      dxmt9::util::resolveModuleSymbol<void*>(reinterpret_cast<void*>(state.ntdll),
                                             "__wine_unix_call_dispatcher");
  if (!dispatcher_export) {
    state.status = DXMT9_STATUS_NOT_SUPPORTED;
    return;
  }
  state.dispatcher = *reinterpret_cast<WineUnixCallDispatcherVar *>(dispatcher_export);
  if (!state.dispatcher) {
    state.status = DXMT9_STATUS_NOT_SUPPORTED;
    return;
  }

  const auto nt_query_virtual_memory =
      resolveProc<NtQueryVirtualMemoryFn>(state.ntdll, "NtQueryVirtualMemory");
  if (!nt_query_virtual_memory) {
    state.status = DXMT9_STATUS_NOT_SUPPORTED;
    return;
  }

  state.status = nt_query_virtual_memory(GetCurrentProcess(),
                                         reinterpret_cast<void *>(&__ImageBase),
                                         kMemoryWineUnixFuncs,
                                         &state.handle,
                                         sizeof(state.handle),
                                         nullptr);
  if (state.status != DXMT9_STATUS_SUCCESS || !state.handle) {
    if (state.status == DXMT9_STATUS_SUCCESS) {
      state.status = DXMT9_STATUS_NOT_SUPPORTED;
    }
    return;
  }
}

}  // namespace

extern "C" NTSTATUS WINAPI dxmt9unix_bridge_unix_call(unsigned int code, void* args) {
  auto& state = providerBridgeState();
  std::call_once(state.initialized, initializeProviderBridge);
  if (state.status != DXMT9_STATUS_SUCCESS) {
    return state.status;
  }
  return state.dispatcher(state.handle, code, args);
}
