#include "../winemetal_thunks.hpp"
#include "../winemetal_unix_bridge.hpp"

#if defined(WINE_UNIX_LIB)
#include "../wineunixlib.h"
#endif

#if defined(WINE_UNIX_LIB)

#define DXMT9_BRIDGE_UNIX_ENTRY(native, wow64) extern NTSTATUS native(void *opaque); extern NTSTATUS wow64(void *opaque);
#include "dxmt9_wine_unix_entries.generated.h"
#undef DXMT9_BRIDGE_UNIX_ENTRY

namespace {

NTSTATUS compileShaderCall(void* args) {
  return dxmt9_winemetal_bridge_compile_shader_params(
      static_cast<Dxmt9WinemetalCompileShaderParams*>(args));
}

NTSTATUS shaderSourceSizeCall(void* args) {
  return dxmt9_winemetal_bridge_shader_source_size_params(
      static_cast<Dxmt9WinemetalShaderSourceSizeParams*>(args));
}

NTSTATUS shaderSourceCopyCall(void* args) {
  return dxmt9_winemetal_bridge_shader_source_copy_params(
      static_cast<Dxmt9WinemetalShaderSourceCopyParams*>(args));
}

NTSTATUS destroyShaderCall(void* args) {
  return dxmt9_winemetal_bridge_destroy_shader_params(
      static_cast<Dxmt9WinemetalDestroyShaderParams*>(args));
}

}  // namespace

extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_funcs[] = {
#define DXMT9_BRIDGE_UNIX_ENTRY(native, wow64) native,
#include "dxmt9_wine_unix_entries.generated.h"
#undef DXMT9_BRIDGE_UNIX_ENTRY
    compileShaderCall,
    shaderSourceSizeCall,
    shaderSourceCopyCall,
    destroyShaderCall,
};

extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {
#define DXMT9_BRIDGE_UNIX_ENTRY(native, wow64) wow64,
#include "dxmt9_wine_unix_entries.generated.h"
#undef DXMT9_BRIDGE_UNIX_ENTRY
    compileShaderCall,
    shaderSourceSizeCall,
    shaderSourceCopyCall,
    destroyShaderCall,
};

#endif
