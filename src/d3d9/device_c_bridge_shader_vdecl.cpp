#include "device_c_provider_api.hpp"
#include "device_c_replay_offload.hpp"

#define DXMT9_TERMINAL_OR_RETURN(owner)                                 \
  do {                                                                  \
    if (dxmt9::d3d9::replayTerminal(owner)) {                            \
      return dxmt9::d3d9::ReplayDrainFailure{};                         \
    }                                                                   \
  } while (false)

extern "C" D9CShader* dxmt9c_device_create_vertex_shader(D9CDevice* arg0, const uint32_t* bytecode) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_device_create_vertex_shader(arg0, bytecode);
}

extern "C" D9CShader* dxmt9c_device_create_pixel_shader(D9CDevice* arg0, const uint32_t* bytecode) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_device_create_pixel_shader(arg0, bytecode);
}

extern "C" D9CVertexDecl* dxmt9c_device_create_vertex_declaration(D9CDevice* arg0, const D9CVertexElement* arg1) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_device_create_vertex_declaration(arg0, arg1);
}

// Lifetime-only refcount calls deliberately remain reachable after terminal
// publication so fail-stop cannot prevent wrapper teardown.
extern "C" void dxmt9c_shader_addref(D9CShader* arg0) {
  dxmt9p_shader_addref(arg0);
}

extern "C" uint32_t dxmt9c_shader_release(D9CShader* arg0) {
  return dxmt9p_shader_release(arg0);
}

extern "C" int32_t dxmt9c_shader_get_wire_identity(
    D9CShader* arg0, D9CWireObjectIdentity* out) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_shader_get_wire_identity(arg0, out);
}

extern "C" int32_t dxmt9c_shader_get_bytecode(D9CShader* arg0, void* data, uint32_t* size) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_shader_get_bytecode(arg0, data, size);
}

extern "C" void dxmt9c_vdecl_addref(D9CVertexDecl* arg0) {
  dxmt9p_vdecl_addref(arg0);
}

extern "C" uint32_t dxmt9c_vdecl_release(D9CVertexDecl* arg0) {
  return dxmt9p_vdecl_release(arg0);
}

extern "C" int32_t dxmt9c_vdecl_get_wire_identity(
    D9CVertexDecl* arg0, D9CWireObjectIdentity* out) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_vdecl_get_wire_identity(arg0, out);
}

extern "C" int32_t dxmt9c_vdecl_get_declaration(D9CVertexDecl* arg0, D9CVertexElement* out, uint32_t* count) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_vdecl_get_declaration(arg0, out, count);
}

#undef DXMT9_TERMINAL_OR_RETURN
