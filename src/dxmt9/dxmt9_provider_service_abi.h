#pragma once

#include "dxmt9/winemetal.h"

// Forward-decl the unix-call param structs so the ABI surface doesn't drag
// in winemetal_thunks.hpp transitively.
struct Dxmt9WinemetalCompileShaderParams;
struct Dxmt9WinemetalShaderSourceCopyParams;
struct Dxmt9WinemetalDestroyShaderParams;

#ifdef __cplusplus
extern "C" {
#endif

dxmt9_u64 dxmt9_provider_service_compile_shader(const WinemetalShaderCompileRequest* request);
dxmt9_u64 dxmt9_provider_service_shader_source_size(dxmt9_u64 shaderHandle);
dxmt9_u64 dxmt9_provider_service_shader_source_copy(dxmt9_u64 shaderHandle,
                                                    char* buffer,
                                                    dxmt9_u64 bufferCapacity);
void dxmt9_provider_service_destroy_shader(dxmt9_u64 shaderHandle);

// Marshaled variants — consumed by the ELF __wine_unix_call handlers in
// dxmt9_provider_shader_handlers.cpp. They decode the per-op param struct
// (pointers + ints packed for cross-bitness PE→ELF transit) and dispatch
// to the typed entry points above.
dxmt9_u64 dxmt9_provider_service_compile_shader_marshaled(
    const Dxmt9WinemetalCompileShaderParams* params);
dxmt9_u64 dxmt9_provider_service_shader_source_copy_marshaled(
    const Dxmt9WinemetalShaderSourceCopyParams* params);
void dxmt9_provider_service_destroy_shader_marshaled(
    const Dxmt9WinemetalDestroyShaderParams* params);

#ifdef __cplusplus
}
#endif
