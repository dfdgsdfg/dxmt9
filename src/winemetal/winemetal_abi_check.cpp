// PE-side ABI handshake — implementation.
//
// At DLL_PROCESS_ATTACH the DllMain in src/winemetal/main.c calls
// dxmt9_winemetal_check_abi_handshake() exactly once. We issue a single
// unix-call into the reserved DXMT9_WINEMETAL_CALL_ABI_HASH slot in
// winemetal.so and verify the returned hash matches the codegen-time
// constant baked into THIS TU. Any mismatch indicates a winemetal.dll /
// winemetal.so version skew — the slot ordering or argument layout of the
// generated bridge ops cannot be trusted, so we refuse to load the DLL
// rather than silently dispatch into a misaligned table.
//
// Static-asserted invariants:
//   * kBridgeAbiHash != 0 (codegen-emitted assertion in
//     dxmt9_bridge_ops.generated.h).
//   * sizeof(Dxmt9WinemetalAbiHashParams) == sizeof(uint64_t) — the
//     params struct is single-field POD shared by both sides.

#include "winemetal_abi_check.hpp"

#include "dxmt9/wineunixlib.h"
#include "dxmt9_bridge_ops.generated.h"
#include "util/log/log.hpp"
#include "winemetal_thunks.hpp"

#include <cstdint>

extern "C" NTSTATUS dxmt9_winemetal_unix_call(unsigned int code, void* args);

namespace {

static_assert(sizeof(Dxmt9WinemetalAbiHashParams) == sizeof(std::uint64_t),
              "abi-hash params struct must be a single uint64_t for "
              "pointer-free PE/unix marshalling");

}  // namespace

extern "C" int dxmt9_winemetal_check_abi_handshake(void) {
  Dxmt9WinemetalAbiHashParams params{};
  const NTSTATUS status =
      dxmt9_winemetal_unix_call(DXMT9_WINEMETAL_CALL_ABI_HASH, &params);
  if (status != DXMT9_STATUS_SUCCESS) {
    dxmt9::util::logf(dxmt9::util::LogLevel::Error, "winemetal-abi",
                      "abi-hash unix-call failed status=0x%08lx; refusing to "
                      "attach winemetal.dll",
                      static_cast<unsigned long>(status));
    return 0;
  }
  if (params.hash != dxmt9::bridge::kBridgeAbiHash) {
    dxmt9::util::logf(
        dxmt9::util::LogLevel::Error, "winemetal-abi",
        "winemetal.dll/winemetal.so ABI hash mismatch: "
        "PE=0x%016llx unix=0x%016llx — refusing to attach. Rebuild both "
        "sides from the same device_c.h schema.",
        static_cast<unsigned long long>(dxmt9::bridge::kBridgeAbiHash),
        static_cast<unsigned long long>(params.hash));
    return 0;
  }
  dxmt9::util::logf(dxmt9::util::LogLevel::Info, "winemetal-abi",
                    "abi-hash handshake OK (0x%016llx)",
                    static_cast<unsigned long long>(params.hash));
  return 1;
}
