#if defined(WINE_UNIX_LIB)
#include "../wineunixlib.h"
#include "../../dxmt9/dxmt9_provider_shader_handlers.hpp"
#include "../winemetal_thunks.hpp"
#include "dxmt9_bridge_ops.generated.h"

// Reserved ABI-hash handler — slot 4 of the unified dispatch table. Returns
// the codegen-time dxmt9::bridge::kBridgeAbiHash so the PE-side DllMain in
// src/winemetal/main.c can detect a winemetal_dxmt9.dll/winemetal_dxmt9.so version
// skew before any bridge call is dispatched.
extern "C" NTSTATUS dxmt9_winemetal_abi_hash_unix_call(void*);

// Forward-declarations for every thunk_<dxmt9c_*> + thunk_wow64_<dxmt9c_*>
// produced by scripts/codegen/gen_wine_bridge.py. The macro expansion below pulls
// in the same entries.h that the generated dispatch.cpp implements.
#define DXMT9_BRIDGE_UNIX_ENTRY(thunk, wow64) \
  extern "C" NTSTATUS thunk(void*); \
  extern "C" NTSTATUS wow64(void*);
#include "dxmt9_wine_unix_entries.generated.h"
#undef DXMT9_BRIDGE_UNIX_ENTRY

// Unified __wine_unix_call dispatch table for winemetal_dxmt9.so.
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
//   3. winemetal_dxmt9.dll's thunk forwards via __wine_unix_call to ntdll.
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

// Bridge defense-in-depth: Wine indexes __wine_unix_call_funcs[] by the
// caller-supplied opcode without checking the index against the array
// size. The PE side's codegen is supposed to emit only valid opcodes,
// and the kBridgeAbiHash handshake in winemetal_abi_check.cpp catches
// codegen drift before any other call lands. A bogus opcode arriving
// past those defenses (e.g., a future bug in PE-side dispatch) would
// reach an uninitialized slot. A runtime bounds check is not feasible
// here — Wine performs the indexing into __wine_unix_call_funcs[] in
// ntdll before any code in this translation unit runs. The build-time
// guard is two-layered: the dense slot-index static_asserts above pin
// the reserved-region layout, and the codegen sentinel
// `BridgeOpcode::dxmt9c_bridge_op_count` plus the static_assert just
// below the array definition pin the total array length against the
// generated opcode set, so adding/removing a dxmt9c_* op without
// updating winemetal_unix.cpp is rejected at build time.
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

// Codegen-sentinel guard: the array above must have exactly
// (DXMT9_WINEMETAL_BRIDGE_OP_BASE + kBridgeOpcodeCount) entries — i.e.
// 5 reserved slots (4 shader handlers + 1 ABI-hash) plus one slot per
// generated dxmt9c_* bridge op. The sentinel
// `BridgeOpcode::dxmt9c_bridge_op_count` is emitted by
// scripts/codegen/gen_wine_bridge.py as the last enumerator of
// `BridgeOpcode`, so its underlying value is exactly the expected
// array length (DXMT9_WINEMETAL_BRIDGE_OP_BASE + N).
static_assert(
    sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0])
        == static_cast<unsigned>(dxmt9::bridge::BridgeOpcode::dxmt9c_bridge_op_count),
    "winemetal unix dispatch array size mismatch — codegen and unix.cpp out of sync");

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

static_assert(
    sizeof(__wine_unix_call_wow64_funcs) / sizeof(__wine_unix_call_wow64_funcs[0])
        == static_cast<unsigned>(dxmt9::bridge::BridgeOpcode::dxmt9c_bridge_op_count),
    "winemetal unix wow64 dispatch array size mismatch — codegen and unix.cpp out of sync");

#endif
