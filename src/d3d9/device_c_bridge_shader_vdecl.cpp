#include "device_c_provider_api.hpp"

extern "C" D9CShader* dxmt9c_device_create_vertex_shader(D9CDevice* arg0, const uint32_t* bytecode) {
  return dxmt9p_device_create_vertex_shader(arg0, bytecode);
}

extern "C" D9CShader* dxmt9c_device_create_pixel_shader(D9CDevice* arg0, const uint32_t* bytecode) {
  return dxmt9p_device_create_pixel_shader(arg0, bytecode);
}

extern "C" D9CVertexDecl* dxmt9c_device_create_vertex_declaration(D9CDevice* arg0, const D9CVertexElement* arg1) {
  return dxmt9p_device_create_vertex_declaration(arg0, arg1);
}

extern "C" void dxmt9c_shader_addref(D9CShader* arg0) {
  dxmt9p_shader_addref(arg0);
}

extern "C" uint32_t dxmt9c_shader_release(D9CShader* arg0) {
  return dxmt9p_shader_release(arg0);
}

extern "C" int32_t dxmt9c_shader_get_bytecode(D9CShader* arg0, void* data, uint32_t* size) {
  return dxmt9p_shader_get_bytecode(arg0, data, size);
}

extern "C" void dxmt9c_vdecl_addref(D9CVertexDecl* arg0) {
  dxmt9p_vdecl_addref(arg0);
}

extern "C" uint32_t dxmt9c_vdecl_release(D9CVertexDecl* arg0) {
  return dxmt9p_vdecl_release(arg0);
}

extern "C" int32_t dxmt9c_vdecl_get_declaration(D9CVertexDecl* arg0, D9CVertexElement* out, uint32_t* count) {
  return dxmt9p_vdecl_get_declaration(arg0, out, count);
}
