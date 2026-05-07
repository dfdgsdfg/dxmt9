#if defined(WINE_UNIX_LIB)
#include "../wineunixlib.h"
#include "../../dxmt9/dxmt9_provider_shader_handlers.hpp"
#include "../winemetal_thunks.hpp"

// Reserved ABI-hash handler — slot 4 of the unified dispatch table. Returns
// the codegen-time dxmt9::bridge::kBridgeAbiHash so the PE-side DllMain in
// src/winemetal/main.c can detect a winemetal.dll/winemetal.so version
// skew before any bridge call is dispatched.
extern "C" NTSTATUS dxmt9_winemetal_abi_hash_unix_call(void*);

// Forward-declarations for every thunk_<dxmt9c_*> + thunk_wow64_<dxmt9c_*>
// produced by scripts/gen_wine_bridge.py. The macro expansion below pulls
// in the same entries.h that the generated dispatch.cpp implements.
#define DXMT9_BRIDGE_UNIX_ENTRY(thunk, wow64) \
  extern "C" NTSTATUS thunk(void*); \
  extern "C" NTSTATUS wow64(void*);
#include "dxmt9_wine_unix_entries.generated.h"
#undef DXMT9_BRIDGE_UNIX_ENTRY

// Unified __wine_unix_call dispatch table for winemetal.so.
//
// Slots 0..DXMT9_WINEMETAL_CALL_SHADER_COUNT-1 are owned by the shader
// service handlers (compile / source-size / source-copy / destroy).
// Slot DXMT9_WINEMETAL_CALL_ABI_HASH (= 4) is the reserved ABI-hash
// handshake handler — fixed positional index that does NOT drift when
// dxmt9c_* prototypes are added or removed.
// Slots from DXMT9_WINEMETAL_BRIDGE_OP_BASE onward are populated by the
// generated device_c bridge entries (one slot per dxmt9c_* function in
// include/dxmt9/device_c.h, ordered by appearance — matches the
// BridgeOpcode enum the PE-side client emits).
//
// PE→ELF flow:
//   1. d3d9.dll exports Direct3DCreate9 → calls dxmt9c_factory_create()
//   2. Generated client (dxmt9_device_c_bridge_exports.generated.cpp,
//      lives in dxmt9_d3d9_forwarder.a inside d3d9.dll) packs Args_*
//      and calls dxmt9_winemetal_unix_call(BridgeOpcode::xxx, &args).
//   3. winemetal.dll's thunk forwards via __wine_unix_call to ntdll.
//   4. ntdll routes to slot BridgeOpcode::xxx in this table.
//   5. Generated server thunk (dxmt9_wine_unix_dispatch.generated.cpp)
//      decodes Args_* and calls the native dxmt9c_* implementation
//      (which in turn forwards to dxmt9p_* in src/d3d9/device_c_*.cpp).
// Phase 35: positional initializer (not C99 designators) — clang in C++
// mode rejects mixed designated/positional initializer lists. Slot
// ordering is enforced by static_asserts below: the first 4 entries
// MUST occupy DXMT9_WINEMETAL_CALL_{COMPILE_SHADER, SHADER_SOURCE_SIZE,
// SHADER_SOURCE_COPY, DESTROY_SHADER} = 0..3, slot 4 is the ABI-hash
// reservation, and bridge entries start at
// DXMT9_WINEMETAL_BRIDGE_OP_BASE = RESERVED_COUNT = 5.
static_assert(DXMT9_WINEMETAL_CALL_COMPILE_SHADER == 0,
              "shader call opcodes must occupy slots 0..3 in order");
static_assert(DXMT9_WINEMETAL_CALL_SHADER_SOURCE_SIZE == 1,
              "shader call opcodes must occupy slots 0..3 in order");
static_assert(DXMT9_WINEMETAL_CALL_SHADER_SOURCE_COPY == 2,
              "shader call opcodes must occupy slots 0..3 in order");
static_assert(DXMT9_WINEMETAL_CALL_DESTROY_SHADER == 3,
              "shader call opcodes must occupy slots 0..3 in order");
static_assert(DXMT9_WINEMETAL_CALL_ABI_HASH == 4,
              "ABI-hash handshake slot is reserved at fixed index 4");
static_assert(DXMT9_WINEMETAL_BRIDGE_OP_BASE == 5,
              "bridge entry table starts at slot 5 (after shader + abi-hash)");

extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_funcs[] = {
    &dxmt9_winemetal_compile_shader_unix_call,      // slot 0
    &dxmt9_winemetal_shader_source_size_unix_call,  // slot 1
    &dxmt9_winemetal_shader_source_copy_unix_call,  // slot 2
    &dxmt9_winemetal_destroy_shader_unix_call,      // slot 3
    &dxmt9_winemetal_abi_hash_unix_call,            // slot 4
#define DXMT9_BRIDGE_UNIX_ENTRY(thunk, wow64) &thunk,
#include "dxmt9_wine_unix_entries.generated.h"
#undef DXMT9_BRIDGE_UNIX_ENTRY
};

extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {
    &dxmt9_winemetal_compile_shader_unix_call,      // slot 0
    &dxmt9_winemetal_shader_source_size_unix_call,  // slot 1
    &dxmt9_winemetal_shader_source_copy_unix_call,  // slot 2
    &dxmt9_winemetal_destroy_shader_unix_call,      // slot 3
    &dxmt9_winemetal_abi_hash_unix_call,            // slot 4
#define DXMT9_BRIDGE_UNIX_ENTRY(thunk, wow64) &wow64,
#include "dxmt9_wine_unix_entries.generated.h"
#undef DXMT9_BRIDGE_UNIX_ENTRY
};

#endif
