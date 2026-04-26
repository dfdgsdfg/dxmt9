#include "device_c_provider_api.hpp"

extern "C" void dxmt9c_device_addref(D9CDevice* arg0) {
  dxmt9p_device_addref(arg0);
}

extern "C" uint32_t dxmt9c_device_release(D9CDevice* arg0) {
  return dxmt9p_device_release(arg0);
}

extern "C" int32_t dxmt9c_device_get_caps(D9CDevice* arg0, D9CCaps* out) {
  return dxmt9p_device_get_caps(arg0, out);
}

extern "C" int32_t dxmt9c_device_test_cooperative_level(D9CDevice* arg0) {
  return dxmt9p_device_test_cooperative_level(arg0);
}

extern "C" int32_t dxmt9c_device_check_device_state(D9CDevice* arg0, uint64_t destWindow) {
  return dxmt9p_device_check_device_state(arg0, destWindow);
}

extern "C" int32_t dxmt9c_device_reset(D9CDevice* arg0, const D9CPresentParams* arg1) {
  return dxmt9p_device_reset(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_reset_ex(D9CDevice* arg0, const D9CPresentParams* arg1, const D9CDisplayModeEx* arg2) {
  return dxmt9p_device_reset_ex(arg0, arg1, arg2);
}

extern "C" int32_t dxmt9c_device_present(D9CDevice* arg0, const D9CRect* src, const D9CRect* dst, uint64_t destWindowOverride, const void* dirtyRegion, uint32_t flags) {
  return dxmt9p_device_present(arg0, src, dst, destWindowOverride, dirtyRegion, flags);
}

extern "C" int32_t dxmt9c_device_begin_scene(D9CDevice* arg0) {
  return dxmt9p_device_begin_scene(arg0);
}

extern "C" int32_t dxmt9c_device_end_scene(D9CDevice* arg0) {
  return dxmt9p_device_end_scene(arg0);
}

extern "C" int32_t dxmt9c_device_clear(D9CDevice* arg0, uint32_t count, const D9CRect* rects, uint32_t flags, uint32_t colorARGB, float z, uint32_t stencil) {
  return dxmt9p_device_clear(arg0, count, rects, flags, colorARGB, z, stencil);
}

extern "C" int32_t dxmt9c_device_set_viewport(D9CDevice* arg0, const D9CViewport* arg1) {
  return dxmt9p_device_set_viewport(arg0, arg1);
}

extern "C" void dxmt9c_device_get_viewport(D9CDevice* arg0, D9CViewport* arg1) {
  dxmt9p_device_get_viewport(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_set_scissor_rect(D9CDevice* arg0, const D9CRect* arg1) {
  return dxmt9p_device_set_scissor_rect(arg0, arg1);
}

extern "C" void dxmt9c_device_get_scissor_rect(D9CDevice* arg0, D9CRect* arg1) {
  dxmt9p_device_get_scissor_rect(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_set_transform(D9CDevice* arg0, uint32_t state, const D9CMatrix* arg2) {
  return dxmt9p_device_set_transform(arg0, state, arg2);
}

extern "C" int32_t dxmt9c_device_get_transform(D9CDevice* arg0, uint32_t state, D9CMatrix* arg2) {
  return dxmt9p_device_get_transform(arg0, state, arg2);
}

extern "C" int32_t dxmt9c_device_set_material(D9CDevice* arg0, const D9CMaterial* arg1) {
  return dxmt9p_device_set_material(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_get_material(D9CDevice* arg0, D9CMaterial* arg1) {
  return dxmt9p_device_get_material(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_set_light(D9CDevice* arg0, uint32_t index, const D9CLight* arg2) {
  return dxmt9p_device_set_light(arg0, index, arg2);
}

extern "C" int32_t dxmt9c_device_light_enable(D9CDevice* arg0, uint32_t index, uint32_t enable) {
  return dxmt9p_device_light_enable(arg0, index, enable);
}

extern "C" int32_t dxmt9c_device_set_render_state(D9CDevice* arg0, uint32_t state, uint32_t value) {
  return dxmt9p_device_set_render_state(arg0, state, value);
}

extern "C" uint32_t dxmt9c_device_get_render_state(D9CDevice* arg0, uint32_t state) {
  return dxmt9p_device_get_render_state(arg0, state);
}

extern "C" int32_t dxmt9c_device_set_texture_stage_state(D9CDevice* arg0, uint32_t stage, uint32_t type, uint32_t value) {
  return dxmt9p_device_set_texture_stage_state(arg0, stage, type, value);
}

extern "C" uint32_t dxmt9c_device_get_texture_stage_state(D9CDevice* arg0, uint32_t stage, uint32_t type) {
  return dxmt9p_device_get_texture_stage_state(arg0, stage, type);
}

extern "C" int32_t dxmt9c_device_set_sampler_state(D9CDevice* arg0, uint32_t sampler, uint32_t type, uint32_t value) {
  return dxmt9p_device_set_sampler_state(arg0, sampler, type, value);
}

extern "C" uint32_t dxmt9c_device_get_sampler_state(D9CDevice* arg0, uint32_t sampler, uint32_t type) {
  return dxmt9p_device_get_sampler_state(arg0, sampler, type);
}

extern "C" int32_t dxmt9c_device_set_clip_plane(D9CDevice* arg0, uint32_t index, const float plane[4]) {
  return dxmt9p_device_set_clip_plane(arg0, index, plane);
}

extern "C" int32_t dxmt9c_device_get_clip_plane(D9CDevice* arg0, uint32_t index, float plane[4]) {
  return dxmt9p_device_get_clip_plane(arg0, index, plane);
}

extern "C" int32_t dxmt9c_device_set_fvf(D9CDevice* arg0, uint32_t fvf) {
  return dxmt9p_device_set_fvf(arg0, fvf);
}

extern "C" uint32_t dxmt9c_device_get_fvf(D9CDevice* arg0) {
  return dxmt9p_device_get_fvf(arg0);
}

extern "C" int32_t dxmt9c_device_set_vertex_declaration(D9CDevice* arg0, D9CVertexDecl* arg1) {
  return dxmt9p_device_set_vertex_declaration(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_set_stream_source(D9CDevice* arg0, uint32_t stream, D9CBuffer* arg2, uint32_t offset, uint32_t stride) {
  return dxmt9p_device_set_stream_source(arg0, stream, arg2, offset, stride);
}

extern "C" int32_t dxmt9c_device_set_stream_source_freq(D9CDevice* arg0, uint32_t stream, uint32_t freq) {
  return dxmt9p_device_set_stream_source_freq(arg0, stream, freq);
}

extern "C" int32_t dxmt9c_device_set_indices(D9CDevice* arg0, D9CBuffer* arg1) {
  return dxmt9p_device_set_indices(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_set_texture(D9CDevice* arg0, uint32_t stage, D9CTexture* arg2) {
  return dxmt9p_device_set_texture(arg0, stage, arg2);
}

extern "C" int32_t dxmt9c_device_set_vertex_shader(D9CDevice* arg0, D9CShader* arg1) {
  return dxmt9p_device_set_vertex_shader(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_set_pixel_shader(D9CDevice* arg0, D9CShader* arg1) {
  return dxmt9p_device_set_pixel_shader(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_set_vs_const_f(D9CDevice* arg0, uint32_t start, const float* data, uint32_t count) {
  return dxmt9p_device_set_vs_const_f(arg0, start, data, count);
}

extern "C" int32_t dxmt9c_device_get_vs_const_f(D9CDevice* arg0, uint32_t start, float* data, uint32_t count) {
  return dxmt9p_device_get_vs_const_f(arg0, start, data, count);
}

extern "C" int32_t dxmt9c_device_set_ps_const_f(D9CDevice* arg0, uint32_t start, const float* data, uint32_t count) {
  return dxmt9p_device_set_ps_const_f(arg0, start, data, count);
}

extern "C" int32_t dxmt9c_device_get_ps_const_f(D9CDevice* arg0, uint32_t start, float* data, uint32_t count) {
  return dxmt9p_device_get_ps_const_f(arg0, start, data, count);
}

extern "C" int32_t dxmt9c_device_set_vs_const_i(D9CDevice* arg0, uint32_t start, const int32_t* data, uint32_t count) {
  return dxmt9p_device_set_vs_const_i(arg0, start, data, count);
}

extern "C" int32_t dxmt9c_device_set_ps_const_i(D9CDevice* arg0, uint32_t start, const int32_t* data, uint32_t count) {
  return dxmt9p_device_set_ps_const_i(arg0, start, data, count);
}

extern "C" int32_t dxmt9c_device_set_vs_const_b(D9CDevice* arg0, uint32_t start, const uint32_t* data, uint32_t count) {
  return dxmt9p_device_set_vs_const_b(arg0, start, data, count);
}

extern "C" int32_t dxmt9c_device_set_ps_const_b(D9CDevice* arg0, uint32_t start, const uint32_t* data, uint32_t count) {
  return dxmt9p_device_set_ps_const_b(arg0, start, data, count);
}

extern "C" int32_t dxmt9c_device_set_render_target(D9CDevice* arg0, uint32_t index, D9CSurface* arg2) {
  return dxmt9p_device_set_render_target(arg0, index, arg2);
}

extern "C" D9CSurface* dxmt9c_device_get_render_target(D9CDevice* arg0, uint32_t index) {
  return dxmt9p_device_get_render_target(arg0, index);
}

extern "C" int32_t dxmt9c_device_set_depth_stencil(D9CDevice* arg0, D9CSurface* arg1) {
  return dxmt9p_device_set_depth_stencil(arg0, arg1);
}

extern "C" D9CSurface* dxmt9c_device_get_depth_stencil(D9CDevice* arg0) {
  return dxmt9p_device_get_depth_stencil(arg0);
}

extern "C" int32_t dxmt9c_device_draw_primitive(D9CDevice* arg0, uint32_t type, uint32_t startVertex, uint32_t count) {
  return dxmt9p_device_draw_primitive(arg0, type, startVertex, count);
}

extern "C" int32_t dxmt9c_device_draw_primitive_packet(D9CDevice* arg0, const D9CDrawPrimitivePacket* packet) {
  return dxmt9p_device_draw_primitive_packet(arg0, packet);
}

extern "C" int32_t dxmt9c_device_draw_primitive_chunk(D9CDevice* arg0, const D9CDrawPrimitivePacket* packets, uint32_t packetCount) {
  return dxmt9p_device_draw_primitive_chunk(arg0, packets, packetCount);
}

extern "C" int32_t dxmt9c_device_draw_indexed_primitive(D9CDevice* arg0, uint32_t type, int32_t baseVertex, uint32_t minVertex, uint32_t numVertices, uint32_t startIndex, uint32_t count) {
  return dxmt9p_device_draw_indexed_primitive(arg0, type, baseVertex, minVertex, numVertices, startIndex, count);
}

extern "C" int32_t dxmt9c_device_draw_primitive_up(D9CDevice* arg0, uint32_t type, uint32_t count, const void* data, uint32_t stride) {
  return dxmt9p_device_draw_primitive_up(arg0, type, count, data, stride);
}

extern "C" int32_t dxmt9c_device_draw_indexed_primitive_up(D9CDevice* arg0, uint32_t type, uint32_t minVertex, uint32_t numVertices, uint32_t count, const void* indexData, uint32_t indexFmt, const void* vertexData, uint32_t stride) {
  return dxmt9p_device_draw_indexed_primitive_up(arg0, type, minVertex, numVertices, count, indexData, indexFmt, vertexData, stride);
}

extern "C" int32_t dxmt9c_device_update_surface(D9CDevice* arg0, D9CSurface* src, const D9CRect* srcRect, D9CSurface* dst, const D9CRect* dstPt) {
  return dxmt9p_device_update_surface(arg0, src, srcRect, dst, dstPt);
}

extern "C" int32_t dxmt9c_device_update_texture(D9CDevice* arg0, D9CTexture* src, D9CTexture* dst) {
  return dxmt9p_device_update_texture(arg0, src, dst);
}

extern "C" int32_t dxmt9c_device_stretch_rect(D9CDevice* arg0, D9CSurface* src, const D9CRect* srcRect, D9CSurface* dst, const D9CRect* dstRect, uint32_t filter) {
  return dxmt9p_device_stretch_rect(arg0, src, srcRect, dst, dstRect, filter);
}

extern "C" int32_t dxmt9c_device_color_fill(D9CDevice* arg0, D9CSurface* arg1, const D9CRect* arg2, uint32_t colorARGB) {
  return dxmt9p_device_color_fill(arg0, arg1, arg2, colorARGB);
}

extern "C" int32_t dxmt9c_device_get_render_target_data(D9CDevice* arg0, D9CSurface* rt, D9CSurface* dst) {
  return dxmt9p_device_get_render_target_data(arg0, rt, dst);
}

extern "C" int32_t dxmt9c_device_set_maximum_frame_latency(D9CDevice* arg0, uint32_t arg1) {
  return dxmt9p_device_set_maximum_frame_latency(arg0, arg1);
}

extern "C" uint32_t dxmt9c_device_get_maximum_frame_latency(D9CDevice* arg0) {
  return dxmt9p_device_get_maximum_frame_latency(arg0);
}

extern "C" int32_t dxmt9c_device_wait_for_vblank(D9CDevice* arg0, uint32_t swapChainIndex) {
  return dxmt9p_device_wait_for_vblank(arg0, swapChainIndex);
}

extern "C" int32_t dxmt9c_device_check_device_multisample(D9CDevice* arg0, uint32_t fmt, uint32_t msType, uint32_t windowed) {
  return dxmt9p_device_check_device_multisample(arg0, fmt, msType, windowed);
}
