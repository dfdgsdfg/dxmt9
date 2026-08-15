#include "device_c_provider_api.hpp"
#include "device_c_replay_offload.hpp"

#define DXMT9_DRAIN_OR_RETURN(...)                                      \
  do {                                                                  \
    if (!dxmt9::d3d9::drainDeferredReplay(__VA_ARGS__)) {               \
      return dxmt9::d3d9::ReplayDrainFailure{};                         \
    }                                                                   \
  } while (false)
#define DXMT9_DRAIN_OR_RETURN_VOID(...)                                 \
  do {                                                                  \
    if (!dxmt9::d3d9::drainDeferredReplay(__VA_ARGS__)) {               \
      return;                                                           \
    }                                                                   \
  } while (false)
#define DXMT9_TERMINAL_OR_RETURN(device)                                 \
  do {                                                                  \
    if (dxmt9::d3d9::replayTerminal(device)) {                           \
      return dxmt9::d3d9::ReplayDrainFailure{};                         \
    }                                                                   \
  } while (false)

extern "C" void dxmt9c_device_addref(D9CDevice* arg0) {
  (void)dxmt9::d3d9::drainDeferredReplay(arg0, "dxmt9c_device_addref");
  dxmt9p_device_addref(arg0);
}

extern "C" uint32_t dxmt9c_device_release(D9CDevice* arg0) {
  // Teardown must remain reachable after fail-stop; it observes the terminal
  // result but still enters the provider solely to release device ownership.
  (void)dxmt9::d3d9::drainDeferredReplay(arg0, "dxmt9c_device_release");
  return dxmt9p_device_release(arg0);
}

extern "C" int32_t dxmt9c_device_negotiate_command_chunk(
    D9CDevice* arg0, D9CCommandChunkNegotiation* arg1) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_negotiate_command_chunk");
  return dxmt9p_device_negotiate_command_chunk(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_get_caps(D9CDevice* arg0, D9CCaps* out) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_get_caps");
  return dxmt9p_device_get_caps(arg0, out);
}

extern "C" int32_t dxmt9c_device_test_cooperative_level(D9CDevice* arg0) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_test_cooperative_level");
  return dxmt9p_device_test_cooperative_level(arg0);
}

extern "C" int32_t dxmt9c_device_check_device_state(D9CDevice* arg0, uint64_t destWindow) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_check_device_state");
  return dxmt9p_device_check_device_state(arg0, destWindow);
}

extern "C" int32_t dxmt9c_device_reset(D9CDevice* arg0, const D9CPresentParams* arg1) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_reset");
  return dxmt9p_device_reset(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_reset_ex(D9CDevice* arg0, const D9CPresentParams* arg1, const D9CDisplayModeEx* arg2) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_reset_ex");
  return dxmt9p_device_reset_ex(arg0, arg1, arg2);
}

extern "C" int32_t dxmt9c_device_present(D9CDevice* arg0, const D9CRect* src, const D9CRect* dst, uint64_t destWindowOverride, const void* dirtyRegion, uint32_t flags) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_present");
  return dxmt9p_device_present(arg0, src, dst, destWindowOverride, dirtyRegion, flags);
}

// Scene markers are deliberately NOT drain-fenced: begin/endScene are pure
// scene-flag toggles (core_state.cpp Device::beginScene/endScene) with no
// replay-dependent reads, and they arrive once per frame — fencing them
// forces a full pipeline drain every frame, which defeats the offload's
// producer/worker overlap (measured 12 fps vs 16 fps baseline).
extern "C" int32_t dxmt9c_device_begin_scene(D9CDevice* arg0) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_device_begin_scene(arg0);
}

extern "C" int32_t dxmt9c_device_end_scene(D9CDevice* arg0) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_device_end_scene(arg0);
}

extern "C" int32_t dxmt9c_device_clear(D9CDevice* arg0, uint32_t count, const D9CRect* rects, uint32_t flags, uint32_t colorARGB, float z, uint32_t stencil) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_clear");
  return dxmt9p_device_clear(arg0, count, rects, flags, colorARGB, z, stencil);
}

extern "C" int32_t dxmt9c_device_set_viewport(D9CDevice* arg0, const D9CViewport* arg1) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_viewport");
  return dxmt9p_device_set_viewport(arg0, arg1);
}

extern "C" void dxmt9c_device_get_viewport(D9CDevice* arg0, D9CViewport* arg1) {
  DXMT9_DRAIN_OR_RETURN_VOID(arg0, "dxmt9c_device_get_viewport");
  dxmt9p_device_get_viewport(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_set_scissor_rect(D9CDevice* arg0, const D9CRect* arg1) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_scissor_rect");
  return dxmt9p_device_set_scissor_rect(arg0, arg1);
}

extern "C" void dxmt9c_device_get_scissor_rect(D9CDevice* arg0, D9CRect* arg1) {
  DXMT9_DRAIN_OR_RETURN_VOID(arg0, "dxmt9c_device_get_scissor_rect");
  dxmt9p_device_get_scissor_rect(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_set_transform(D9CDevice* arg0, uint32_t state, const D9CMatrix* arg2) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_transform");
  return dxmt9p_device_set_transform(arg0, state, arg2);
}

extern "C" int32_t dxmt9c_device_get_transform(D9CDevice* arg0, uint32_t state, D9CMatrix* arg2) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_get_transform");
  return dxmt9p_device_get_transform(arg0, state, arg2);
}

extern "C" int32_t dxmt9c_device_set_material(D9CDevice* arg0, const D9CMaterial* arg1) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_material");
  return dxmt9p_device_set_material(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_get_material(D9CDevice* arg0, D9CMaterial* arg1) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_get_material");
  return dxmt9p_device_get_material(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_set_light(D9CDevice* arg0, uint32_t index, const D9CLight* arg2) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_light");
  return dxmt9p_device_set_light(arg0, index, arg2);
}

extern "C" int32_t dxmt9c_device_light_enable(D9CDevice* arg0, uint32_t index, uint32_t enable) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_light_enable");
  return dxmt9p_device_light_enable(arg0, index, enable);
}

extern "C" int32_t dxmt9c_device_set_render_state(D9CDevice* arg0, uint32_t state, uint32_t value) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_render_state");
  return dxmt9p_device_set_render_state(arg0, state, value);
}

extern "C" uint32_t dxmt9c_device_get_render_state(D9CDevice* arg0, uint32_t state) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_get_render_state");
  return dxmt9p_device_get_render_state(arg0, state);
}

extern "C" int32_t dxmt9c_device_set_texture_stage_state(D9CDevice* arg0, uint32_t stage, uint32_t type, uint32_t value) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_texture_stage_state");
  return dxmt9p_device_set_texture_stage_state(arg0, stage, type, value);
}

extern "C" uint32_t dxmt9c_device_get_texture_stage_state(D9CDevice* arg0, uint32_t stage, uint32_t type) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_get_texture_stage_state");
  return dxmt9p_device_get_texture_stage_state(arg0, stage, type);
}

extern "C" int32_t dxmt9c_device_set_sampler_state(D9CDevice* arg0, uint32_t sampler, uint32_t type, uint32_t value) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_sampler_state");
  return dxmt9p_device_set_sampler_state(arg0, sampler, type, value);
}

extern "C" uint32_t dxmt9c_device_get_sampler_state(D9CDevice* arg0, uint32_t sampler, uint32_t type) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_get_sampler_state");
  return dxmt9p_device_get_sampler_state(arg0, sampler, type);
}

extern "C" int32_t dxmt9c_device_set_clip_plane(D9CDevice* arg0, uint32_t index, const float plane[4]) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_clip_plane");
  return dxmt9p_device_set_clip_plane(arg0, index, plane);
}

extern "C" int32_t dxmt9c_device_get_clip_plane(D9CDevice* arg0, uint32_t index, float plane[4]) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_get_clip_plane");
  return dxmt9p_device_get_clip_plane(arg0, index, plane);
}

extern "C" int32_t dxmt9c_device_set_fvf(D9CDevice* arg0, uint32_t fvf) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_fvf");
  return dxmt9p_device_set_fvf(arg0, fvf);
}

extern "C" uint32_t dxmt9c_device_get_fvf(D9CDevice* arg0) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_get_fvf");
  return dxmt9p_device_get_fvf(arg0);
}

extern "C" int32_t dxmt9c_device_set_vertex_declaration(D9CDevice* arg0, D9CVertexDecl* arg1) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_vertex_declaration");
  return dxmt9p_device_set_vertex_declaration(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_set_stream_source(D9CDevice* arg0, uint32_t stream, D9CBuffer* arg2, uint32_t offset, uint32_t stride) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_stream_source");
  return dxmt9p_device_set_stream_source(arg0, stream, arg2, offset, stride);
}

extern "C" int32_t dxmt9c_device_set_stream_source_freq(D9CDevice* arg0, uint32_t stream, uint32_t freq) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_stream_source_freq");
  return dxmt9p_device_set_stream_source_freq(arg0, stream, freq);
}

extern "C" int32_t dxmt9c_device_set_indices(D9CDevice* arg0, D9CBuffer* arg1) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_indices");
  return dxmt9p_device_set_indices(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_set_texture(D9CDevice* arg0, uint32_t stage, D9CTexture* arg2) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_texture");
  return dxmt9p_device_set_texture(arg0, stage, arg2);
}

extern "C" int32_t dxmt9c_device_set_vertex_shader(D9CDevice* arg0, D9CShader* arg1) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_vertex_shader");
  return dxmt9p_device_set_vertex_shader(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_set_pixel_shader(D9CDevice* arg0, D9CShader* arg1) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_pixel_shader");
  return dxmt9p_device_set_pixel_shader(arg0, arg1);
}

extern "C" int32_t dxmt9c_device_set_vs_const_f(D9CDevice* arg0, uint32_t start, const float* data, uint32_t count) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_vs_const_f");
  return dxmt9p_device_set_vs_const_f(arg0, start, data, count);
}

extern "C" int32_t dxmt9c_device_get_vs_const_f(D9CDevice* arg0, uint32_t start, float* data, uint32_t count) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_get_vs_const_f");
  return dxmt9p_device_get_vs_const_f(arg0, start, data, count);
}

extern "C" int32_t dxmt9c_device_set_ps_const_f(D9CDevice* arg0, uint32_t start, const float* data, uint32_t count) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_ps_const_f");
  return dxmt9p_device_set_ps_const_f(arg0, start, data, count);
}

extern "C" int32_t dxmt9c_device_get_ps_const_f(D9CDevice* arg0, uint32_t start, float* data, uint32_t count) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_get_ps_const_f");
  return dxmt9p_device_get_ps_const_f(arg0, start, data, count);
}

extern "C" int32_t dxmt9c_device_set_vs_const_i(D9CDevice* arg0, uint32_t start, const int32_t* data, uint32_t count) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_vs_const_i");
  return dxmt9p_device_set_vs_const_i(arg0, start, data, count);
}

extern "C" int32_t dxmt9c_device_set_ps_const_i(D9CDevice* arg0, uint32_t start, const int32_t* data, uint32_t count) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_ps_const_i");
  return dxmt9p_device_set_ps_const_i(arg0, start, data, count);
}

extern "C" int32_t dxmt9c_device_set_vs_const_b(D9CDevice* arg0, uint32_t start, const uint32_t* data, uint32_t count) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_vs_const_b");
  return dxmt9p_device_set_vs_const_b(arg0, start, data, count);
}

extern "C" int32_t dxmt9c_device_set_ps_const_b(D9CDevice* arg0, uint32_t start, const uint32_t* data, uint32_t count) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_ps_const_b");
  return dxmt9p_device_set_ps_const_b(arg0, start, data, count);
}

extern "C" int32_t dxmt9c_device_set_render_target(D9CDevice* arg0, uint32_t index, D9CSurface* arg2) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_render_target");
  return dxmt9p_device_set_render_target(arg0, index, arg2);
}

extern "C" D9CSurface* dxmt9c_device_get_render_target(D9CDevice* arg0, uint32_t index) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_get_render_target");
  return dxmt9p_device_get_render_target(arg0, index);
}

extern "C" int32_t dxmt9c_device_set_depth_stencil(D9CDevice* arg0, D9CSurface* arg1) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_depth_stencil");
  return dxmt9p_device_set_depth_stencil(arg0, arg1);
}

extern "C" D9CSurface* dxmt9c_device_get_depth_stencil(D9CDevice* arg0) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_get_depth_stencil");
  return dxmt9p_device_get_depth_stencil(arg0);
}

extern "C" int32_t dxmt9c_device_draw_primitive(D9CDevice* arg0, uint32_t type, uint32_t startVertex, uint32_t count) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_draw_primitive");
  return dxmt9p_device_draw_primitive(arg0, type, startVertex, count);
}

extern "C" int32_t dxmt9c_device_commit_chunk(D9CDevice* arg0, const D9CCommandChunk* chunk) {
  return dxmt9p_device_commit_chunk(arg0, chunk);
}

extern "C" int32_t dxmt9c_device_reserve_render_tape_present_capture(
    D9CDevice* arg0) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_reserve_render_tape_present_capture");
  return dxmt9p_device_reserve_render_tape_present_capture(arg0);
}

extern "C" int32_t dxmt9c_device_finish_render_tape_present_capture(
    D9CDevice* arg0, D9CRenderTapePresentCaptureResult* out) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_finish_render_tape_present_capture");
  return dxmt9p_device_finish_render_tape_present_capture(arg0, out);
}

extern "C" void dxmt9c_device_cancel_render_tape_present_capture(
    D9CDevice* arg0) {
  if (arg0) dxmt9p_device_cancel_render_tape_present_capture(arg0);
}

extern "C" int32_t dxmt9c_device_capture_render_tape_d24x8_snapshot(
    D9CDevice* arg0, const D9CRenderTapeD24X8SnapshotRequest* request,
    D9CRenderTapeD24X8SnapshotResult* out, void* bytes, uint64_t capacity) {
  DXMT9_DRAIN_OR_RETURN(
      arg0, "dxmt9c_device_capture_render_tape_d24x8_snapshot");
  return dxmt9p_device_capture_render_tape_d24x8_snapshot(
      arg0, request, out, bytes, capacity);
}

extern "C" int32_t dxmt9c_device_capture_render_tape_color_snapshot(
    D9CDevice* arg0, const D9CRenderTapeColorSnapshotRequest* request,
    D9CRenderTapeColorSnapshotResult* out, void* bytes, uint64_t capacity) {
  DXMT9_DRAIN_OR_RETURN(
      arg0, "dxmt9c_device_capture_render_tape_color_snapshot");
  return dxmt9p_device_capture_render_tape_color_snapshot(
      arg0, request, out, bytes, capacity);
}

extern "C" int32_t dxmt9c_device_draw_indexed_primitive(D9CDevice* arg0, uint32_t type, int32_t baseVertex, uint32_t minVertex, uint32_t numVertices, uint32_t startIndex, uint32_t count) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_draw_indexed_primitive");
  return dxmt9p_device_draw_indexed_primitive(arg0, type, baseVertex, minVertex, numVertices, startIndex, count);
}

extern "C" int32_t dxmt9c_device_draw_primitive_up(D9CDevice* arg0, uint32_t type, uint32_t count, const void* data, uint32_t stride) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_draw_primitive_up");
  return dxmt9p_device_draw_primitive_up(arg0, type, count, data, stride);
}

extern "C" int32_t dxmt9c_device_draw_indexed_primitive_up(D9CDevice* arg0, uint32_t type, uint32_t minVertex, uint32_t numVertices, uint32_t count, const void* indexData, uint32_t indexFmt, const void* vertexData, uint32_t stride) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_draw_indexed_primitive_up");
  return dxmt9p_device_draw_indexed_primitive_up(arg0, type, minVertex, numVertices, count, indexData, indexFmt, vertexData, stride);
}

extern "C" int32_t dxmt9c_device_update_surface(D9CDevice* arg0, D9CSurface* src, const D9CRect* srcRect, D9CSurface* dst, const D9CRect* dstPt) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_update_surface");
  return dxmt9p_device_update_surface(arg0, src, srcRect, dst, dstPt);
}

extern "C" int32_t dxmt9c_device_update_texture(D9CDevice* arg0, D9CTexture* src, D9CTexture* dst) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_update_texture");
  return dxmt9p_device_update_texture(arg0, src, dst);
}

extern "C" int32_t dxmt9c_device_stretch_rect(D9CDevice* arg0, D9CSurface* src, const D9CRect* srcRect, D9CSurface* dst, const D9CRect* dstRect, uint32_t filter) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_stretch_rect");
  return dxmt9p_device_stretch_rect(arg0, src, srcRect, dst, dstRect, filter);
}

extern "C" int32_t dxmt9c_device_color_fill(D9CDevice* arg0, D9CSurface* arg1, const D9CRect* arg2, uint32_t colorARGB) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_color_fill");
  return dxmt9p_device_color_fill(arg0, arg1, arg2, colorARGB);
}

extern "C" int32_t dxmt9c_device_get_render_target_data(D9CDevice* arg0, D9CSurface* rt, D9CSurface* dst) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_get_render_target_data");
  return dxmt9p_device_get_render_target_data(arg0, rt, dst);
}

extern "C" int32_t dxmt9c_device_set_maximum_frame_latency(D9CDevice* arg0, uint32_t arg1) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_set_maximum_frame_latency");
  return dxmt9p_device_set_maximum_frame_latency(arg0, arg1);
}

extern "C" uint32_t dxmt9c_device_get_maximum_frame_latency(D9CDevice* arg0) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_get_maximum_frame_latency");
  return dxmt9p_device_get_maximum_frame_latency(arg0);
}

extern "C" int32_t dxmt9c_device_wait_for_vblank(D9CDevice* arg0, uint32_t swapChainIndex) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_wait_for_vblank");
  return dxmt9p_device_wait_for_vblank(arg0, swapChainIndex);
}

extern "C" int32_t dxmt9c_device_check_device_multisample(D9CDevice* arg0, uint32_t fmt, uint32_t msType, uint32_t windowed) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_check_device_multisample");
  return dxmt9p_device_check_device_multisample(arg0, fmt, msType, windowed);
}

#undef DXMT9_DRAIN_OR_RETURN_VOID
#undef DXMT9_DRAIN_OR_RETURN
