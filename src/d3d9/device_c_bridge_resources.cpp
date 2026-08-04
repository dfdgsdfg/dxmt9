#include "device_c_provider_api.hpp"
#include "device_c_replay_offload.hpp"

#define DXMT9_DRAIN_OR_RETURN(...)                                      \
  do {                                                                  \
    if (!dxmt9::d3d9::drainDeferredReplay(__VA_ARGS__)) {               \
      return dxmt9::d3d9::ReplayDrainFailure{};                         \
    }                                                                   \
  } while (false)
#define DXMT9_TERMINAL_OR_RETURN(owner)                                 \
  do {                                                                  \
    if (dxmt9::d3d9::replayTerminal(owner)) {                            \
      return dxmt9::d3d9::ReplayDrainFailure{};                         \
    }                                                                   \
  } while (false)

// Drain-fence rule for this file (R-BACK-2.51(d)): fence every entry point that
// READS OR PUBLISHES BACKEND RESOURCE CONTENTS, because the offload worker may
// hold queued chunks that write or read the same resource, and a direct call
// that skips the fence observes them out of program order.
//
// Fence:      lock/unlock (both halves -- unlock is where a write publishes),
//             mip generation, direct sampling, palette upload, LOD clamp
//             (it notifies the owner), and resource creation.
// Do not drain: wire-identity and desc/level/container getters. They read
//             wrapper-cached metadata the replay never touches, but still
//             acquire-check terminal state. Addref/release alone remain
//             reachable after fail-stop so wrapper teardown cannot leak.
//
// The fence used to cover create_* and buffer_lock only, which reads as a list
// rather than a rule; the lock/unlock pairs below were unfenced with no
// exemption note (contrast the measured one on begin/end_scene in
// device_c_bridge_device_state_draw.cpp).

extern "C" D9CTexture* dxmt9c_device_create_texture(D9CDevice* arg0, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_create_texture");
  return dxmt9p_device_create_texture(arg0, w, h, levels, usage, fmt, pool);
}

extern "C" D9CTexture* dxmt9c_device_create_texture_shared(D9CDevice* arg0, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool, uint64_t* sharedHandle) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_create_texture_shared");
  return dxmt9p_device_create_texture_shared(arg0, w, h, levels, usage, fmt, pool, sharedHandle);
}

extern "C" D9CTexture* dxmt9c_device_create_cube_texture(D9CDevice* arg0, uint32_t size, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_create_cube_texture");
  return dxmt9p_device_create_cube_texture(arg0, size, levels, usage, fmt, pool);
}

extern "C" D9CTexture* dxmt9c_device_create_cube_texture_shared(D9CDevice* arg0, uint32_t size, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool, uint64_t* sharedHandle) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_create_cube_texture_shared");
  return dxmt9p_device_create_cube_texture_shared(arg0, size, levels, usage, fmt, pool, sharedHandle);
}

extern "C" D9CTexture* dxmt9c_device_create_volume_texture(D9CDevice* arg0, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_create_volume_texture");
  return dxmt9p_device_create_volume_texture(arg0, w, h, d, levels, usage, fmt, pool);
}

extern "C" D9CTexture* dxmt9c_device_create_volume_texture_shared(D9CDevice* arg0, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool, uint64_t* sharedHandle) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_create_volume_texture_shared");
  return dxmt9p_device_create_volume_texture_shared(arg0, w, h, d, levels, usage, fmt, pool, sharedHandle);
}

extern "C" D9CBuffer* dxmt9c_device_create_vertex_buffer(D9CDevice* arg0, uint32_t length, uint32_t usage, uint32_t fvf, uint32_t pool) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_create_vertex_buffer");
  return dxmt9p_device_create_vertex_buffer(arg0, length, usage, fvf, pool);
}

extern "C" D9CBuffer* dxmt9c_device_create_vertex_buffer_shared(D9CDevice* arg0, uint32_t length, uint32_t usage, uint32_t fvf, uint32_t pool, uint64_t* sharedHandle) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_create_vertex_buffer_shared");
  return dxmt9p_device_create_vertex_buffer_shared(arg0, length, usage, fvf, pool, sharedHandle);
}

extern "C" D9CBuffer* dxmt9c_device_create_index_buffer(D9CDevice* arg0, uint32_t length, uint32_t usage, uint32_t fmt, uint32_t pool) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_create_index_buffer");
  return dxmt9p_device_create_index_buffer(arg0, length, usage, fmt, pool);
}

extern "C" D9CBuffer* dxmt9c_device_create_index_buffer_shared(D9CDevice* arg0, uint32_t length, uint32_t usage, uint32_t fmt, uint32_t pool, uint64_t* sharedHandle) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_create_index_buffer_shared");
  return dxmt9p_device_create_index_buffer_shared(arg0, length, usage, fmt, pool, sharedHandle);
}

extern "C" D9CSurface* dxmt9c_device_create_render_target(D9CDevice* arg0, uint32_t w, uint32_t h, uint32_t fmt, uint32_t msType, uint32_t msQuality, uint32_t lockable, uint64_t* sharedHandle) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_create_render_target");
  return dxmt9p_device_create_render_target(arg0, w, h, fmt, msType, msQuality, lockable, sharedHandle);
}

extern "C" D9CSurface* dxmt9c_device_create_depth_stencil(D9CDevice* arg0, uint32_t w, uint32_t h, uint32_t fmt, uint32_t msType, uint32_t msQuality, uint32_t discard, uint64_t* sharedHandle) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_create_depth_stencil");
  return dxmt9p_device_create_depth_stencil(arg0, w, h, fmt, msType, msQuality, discard, sharedHandle);
}

extern "C" D9CSurface* dxmt9c_device_create_offscreen_surface(D9CDevice* arg0, uint32_t w, uint32_t h, uint32_t fmt, uint32_t pool, uint64_t* sharedHandle) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_create_offscreen_surface");
  return dxmt9p_device_create_offscreen_surface(arg0, w, h, fmt, pool, sharedHandle);
}

extern "C" void dxmt9c_texture_addref(D9CTexture* arg0) {
  dxmt9p_texture_addref(arg0);
}

extern "C" uint32_t dxmt9c_texture_release(D9CTexture* arg0) {
  return dxmt9p_texture_release(arg0);
}

extern "C" int32_t dxmt9c_texture_get_wire_identity(
    D9CTexture* arg0, D9CWireObjectIdentity* out) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_texture_get_wire_identity(arg0, out);
}

extern "C" int32_t dxmt9c_texture_lock_rect(D9CTexture* arg0, uint32_t level, D9CLockedRect* out, const D9CRect* arg3, uint32_t flags) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_texture_lock_rect");
  return dxmt9p_texture_lock_rect(arg0, level, out, arg3, flags);
}

extern "C" int32_t dxmt9c_texture_unlock_rect(D9CTexture* arg0, uint32_t level) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_texture_unlock_rect");
  return dxmt9p_texture_unlock_rect(arg0, level);
}

extern "C" D9CSurface* dxmt9c_texture_get_surface_level(D9CTexture* arg0, uint32_t level) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_texture_get_surface_level(arg0, level);
}

extern "C" uint32_t dxmt9c_texture_get_level_count(D9CTexture* arg0) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_texture_get_level_count(arg0);
}

extern "C" int32_t dxmt9c_texture_get_level_desc(D9CTexture* arg0, uint32_t level, D9CSurfaceDesc* arg2) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_texture_get_level_desc(arg0, level, arg2);
}

extern "C" int32_t dxmt9c_texture_generate_mip_sublevels(D9CTexture* arg0) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_texture_generate_mip_sublevels");
  return dxmt9p_texture_generate_mip_sublevels(arg0);
}

extern "C" uint32_t dxmt9c_texture_set_lod(D9CTexture* arg0, uint32_t lod) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_texture_set_lod");
  return dxmt9p_texture_set_lod(arg0, lod);
}

extern "C" int32_t dxmt9c_texture_sample_2d(D9CTexture* arg0, uint32_t level,
                                             float u, float v,
                                             float* outRgba4) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_texture_sample_2d");
  return dxmt9p_texture_sample_2d(arg0, level, u, v, outRgba4);
}

extern "C" int32_t dxmt9c_texture_set_palette(D9CTexture* arg0,
                                               const uint32_t* argbEntries,
                                               uint32_t entryCount) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_texture_set_palette");
  return dxmt9p_texture_set_palette(arg0, argbEntries, entryCount);
}

extern "C" void dxmt9c_buffer_addref(D9CBuffer* arg0) {
  dxmt9p_buffer_addref(arg0);
}

extern "C" uint32_t dxmt9c_buffer_release(D9CBuffer* arg0) {
  return dxmt9p_buffer_release(arg0);
}

extern "C" int32_t dxmt9c_buffer_get_wire_identity(
    D9CBuffer* arg0, D9CWireObjectIdentity* out) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_buffer_get_wire_identity(arg0, out);
}

extern "C" int32_t dxmt9c_buffer_lock(D9CBuffer* arg0, uint32_t offset, uint32_t size, void** data, uint32_t flags) {
  if (!dxmt9::d3d9::drainDeferredReplayForBufferLock(arg0, flags)) {
    if (data) *data = nullptr;
    return dxmt9::core::D3DERR_DEVICELOST;
  }
  return dxmt9p_buffer_lock(arg0, offset, size, data, flags);
}

extern "C" int32_t dxmt9c_buffer_unlock(D9CBuffer* arg0) {
  if (!dxmt9::d3d9::drainDeferredReplayForBufferUnlock(arg0)) {
    return dxmt9::core::D3DERR_DEVICELOST;
  }
  return dxmt9p_buffer_unlock(arg0);
}

extern "C" int32_t dxmt9c_buffer_get_desc(D9CBuffer* arg0, D9CBufferDesc* arg1) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_buffer_get_desc(arg0, arg1);
}

extern "C" void dxmt9c_surface_addref(D9CSurface* arg0) {
  dxmt9p_surface_addref(arg0);
}

extern "C" uint32_t dxmt9c_surface_release(D9CSurface* arg0) {
  return dxmt9p_surface_release(arg0);
}

extern "C" int32_t dxmt9c_surface_get_wire_identity(
    D9CSurface* arg0, D9CWireObjectIdentity* out) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_surface_get_wire_identity(arg0, out);
}

extern "C" int32_t dxmt9c_surface_lock_rect(D9CSurface* arg0, D9CLockedRect* arg1, const D9CRect* arg2, uint32_t flags) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_surface_lock_rect");
  return dxmt9p_surface_lock_rect(arg0, arg1, arg2, flags);
}

extern "C" int32_t dxmt9c_surface_unlock_rect(D9CSurface* arg0) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_surface_unlock_rect");
  return dxmt9p_surface_unlock_rect(arg0);
}

extern "C" int32_t dxmt9c_surface_get_desc(D9CSurface* arg0, D9CSurfaceDesc* arg1) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_surface_get_desc(arg0, arg1);
}

extern "C" D9CTexture* dxmt9c_surface_get_container_texture(D9CSurface* arg0) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_surface_get_container_texture(arg0);
}

#undef DXMT9_TERMINAL_OR_RETURN
#undef DXMT9_DRAIN_OR_RETURN
