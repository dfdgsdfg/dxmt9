#include "../winemetal_thunks.hpp"
#include "../winemetal_shader_bridge.hpp"

#if defined(WINE_UNIX_LIB)
#include "../wineunixlib.h"
#endif

#if defined(WINE_UNIX_LIB)

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

extern "C" DECLSPEC_EXPORT NTSTATUS __wine_unix_lib_init(void) {
  return DXMT9_STATUS_SUCCESS;
}

extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_funcs[] = {
    compileShaderCall,
    shaderSourceSizeCall,
    shaderSourceCopyCall,
    destroyShaderCall,
};

extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {
    compileShaderCall,
    shaderSourceSizeCall,
    shaderSourceCopyCall,
    destroyShaderCall,
};

#endif
