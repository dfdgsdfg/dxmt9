#pragma once

// PE-side ABI-hash handshake. Called once from DllMain (DLL_PROCESS_ATTACH)
// after __wine_init_unix_call has resolved the unixlib handle. Issues the
// reserved DXMT9_WINEMETAL_CALL_ABI_HASH unix-call into winemetal_dxmt9.so and
// compares the returned hash against the codegen-time
// dxmt9::bridge::kBridgeAbiHash baked into this winemetal_dxmt9.dll TU.
//
// Exposed as a C entry point so the C-language main.c DllMain can call it
// directly without including the generated C++ header.
//
// Returns non-zero on success (matching hashes), zero on mismatch.
// Mismatch is also logged via dxmt9::util::log at Error level so the
// process console / wine debug stream surfaces the schema skew before the
// caller refuses to attach the DLL.

#ifdef __cplusplus
extern "C" {
#endif

int dxmt9_winemetal_check_abi_handshake(void);

#ifdef __cplusplus
}  // extern "C"
#endif
