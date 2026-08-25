// Unix-side handler for the reserved DXMT9_WINEMETAL_CALL_ABI_HASH slot.
//
// Returns the codegen-time kBridgeAbiHash baked into winemetal_dxmt9.so so the
// PE-side winemetal_dxmt9.dll can compare it against its own kBridgeAbiHash at
// DllMain and refuse to load on mismatch (see src/winemetal/main.c).
//
// Lives in its own TU rather than alongside the dxmt9c_* dispatch thunks
// because the slot is not part of the generated device_c bridge — it is
// reserved at a fixed positional index (slot 4) that does not drift when
// dxmt9c_* prototypes are added or removed. The handler is wired into
// __wine_unix_call_funcs[] / __wine_unix_call_wow64_funcs[] from
// src/winemetal/unix/winemetal_unix.cpp.

#if defined(WINE_UNIX_LIB)

#include "../wineunixlib.h"
#include "../winemetal_thunks.hpp"
#include "dxmt9_bridge_ops.generated.h"

namespace {

NTSTATUS fillAbiHashParams(void* opaque) {
  if (!opaque) {
    return DXMT9_STATUS_INVALID_PARAMETER;
  }
  auto* params = static_cast<Dxmt9WinemetalAbiHashParams*>(opaque);
  params->hash = dxmt9::bridge::kBridgeAbiHash;
  return DXMT9_STATUS_SUCCESS;
}

}  // namespace

extern "C" NTSTATUS dxmt9_winemetal_abi_hash_unix_call(void* args) {
  return fillAbiHashParams(args);
}

#endif  // WINE_UNIX_LIB
