// Batched-chunk C ABI: dxmt9c_device_commit_chunk and the packet->state
// replay machinery it invokes. Per-call dxmt9c_* setters live in
// device_c_draw.cpp (interactive entry points) and device_c_state.cpp
// (state setters); both are referenced via extern "C" forward decls.

#include "device_c_provider.hpp"
#include "device_c_record_utils.hpp"
#include "util/unixcall_marshal.hpp"

#include "../dxmt9/dxmt9_perf_counters.hpp"
// Need the full dxmt9::Device type to call markChunkResources on the
// upperDevice shared_ptr that the chunk importer's Phase 4-B path
// hands the per-chunk retention list to.
#include "dxmt9/dxmt9_device.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

using namespace dxmt9::d3d9::devicec;

// Forward declarations for state-setter ABI entry points implemented in
// device_c_state.cpp. The packet-replay path (applyDrawPacketStateViaIface)
// and the chunk importer (dxmt9c_device_commit_chunk) call these to dispatch
// per-record state deltas. The provider macro renames apply uniformly via
// device_c_provider.hpp, so the linker resolves to the same symbols.
extern "C" int32_t dxmt9c_device_set_viewport(D9CDevice* d, const D9CViewport* vp);
extern "C" int32_t dxmt9c_device_set_scissor_rect(D9CDevice* d, const D9CRect* r);
extern "C" int32_t dxmt9c_device_set_transform(D9CDevice* d, uint32_t state,
                                               const D9CMatrix* m);
extern "C" int32_t dxmt9c_device_set_material(D9CDevice* d, const D9CMaterial* m);
extern "C" int32_t dxmt9c_device_set_light(D9CDevice* d, uint32_t idx, const D9CLight* l);
extern "C" int32_t dxmt9c_device_light_enable(D9CDevice* d, uint32_t i, uint32_t en);
extern "C" int32_t dxmt9c_device_set_texture_stage_state(D9CDevice* d, uint32_t st,
                                                         uint32_t type, uint32_t val);
extern "C" int32_t dxmt9c_device_set_sampler_state(D9CDevice* d, uint32_t s, uint32_t type,
                                                   uint32_t val);
extern "C" int32_t dxmt9c_device_set_clip_plane(D9CDevice* d, uint32_t idx,
                                                const float plane[4]);
extern "C" int32_t dxmt9c_device_set_vs_const_f(D9CDevice* d, uint32_t s, const float* data,
                                                uint32_t cnt);
extern "C" int32_t dxmt9c_device_set_ps_const_f(D9CDevice* d, uint32_t s, const float* data,
                                                uint32_t cnt);
extern "C" int32_t dxmt9c_device_set_vs_const_i(D9CDevice* d, uint32_t s, const int32_t* data,
                                                uint32_t cnt);
extern "C" int32_t dxmt9c_device_set_ps_const_i(D9CDevice* d, uint32_t s, const int32_t* data,
                                                uint32_t cnt);
extern "C" int32_t dxmt9c_device_set_vs_const_b(D9CDevice* d, uint32_t s,
                                                const uint32_t* data, uint32_t cnt);
extern "C" int32_t dxmt9c_device_set_ps_const_b(D9CDevice* d, uint32_t s,
                                                const uint32_t* data, uint32_t cnt);

// Forward declaration for the query-issue ABI entry implemented in
// device_c_swapchain_query_stateblock.cpp; chunk replay routes
// D9C_COMMAND_RECORD_QUERY_ISSUE through it.
extern "C" int32_t dxmt9c_query_issue(D9CQuery* q, uint32_t flags);

// Forward declarations for the per-call interactive C ABI entries that
// remain in device_c_draw.cpp. The chunk dispatcher routes the matching
// D9C_COMMAND_RECORD_* and the per-packet shader/render-target/depth/
// vertex-decl/index-buffer deltas through these so behavior matches the
// per-call path bit-for-bit.
extern "C" int32_t dxmt9c_device_present(D9CDevice* d, const D9CRect* src, const D9CRect* dst,
                                         uint64_t destWindow, const void* dirty, uint32_t flags);
extern "C" int32_t dxmt9c_device_clear(D9CDevice* d, uint32_t count, const D9CRect* rects,
                                       uint32_t flags, uint32_t colorARGB, float z,
                                       uint32_t stencil);
extern "C" int32_t dxmt9c_device_set_vertex_declaration(D9CDevice* d, D9CVertexDecl* vd);
extern "C" int32_t dxmt9c_device_set_indices(D9CDevice* d, D9CBuffer* buf);
extern "C" int32_t dxmt9c_device_set_vertex_shader(D9CDevice* d, D9CShader* s);
extern "C" int32_t dxmt9c_device_set_pixel_shader(D9CDevice* d, D9CShader* s);
extern "C" int32_t dxmt9c_device_set_render_target(D9CDevice* d, uint32_t idx,
                                                   D9CSurface* surf);
extern "C" int32_t dxmt9c_device_set_depth_stencil(D9CDevice* d, D9CSurface* surf);
extern "C" int32_t dxmt9c_device_update_surface(D9CDevice* d, D9CSurface* src,
                                                const D9CRect*, D9CSurface* dst,
                                                const D9CRect*);
extern "C" int32_t dxmt9c_device_update_texture(D9CDevice* d, D9CTexture* src,
                                                D9CTexture* dst);
extern "C" int32_t dxmt9c_device_stretch_rect(D9CDevice* d, D9CSurface* src, const D9CRect* sr,
                                              D9CSurface* dst, const D9CRect* dr,
                                              uint32_t filter);
extern "C" int32_t dxmt9c_device_color_fill(D9CDevice* d, D9CSurface* surf, const D9CRect* r,
                                            uint32_t colorARGB);
extern "C" int32_t dxmt9c_device_get_render_target_data(D9CDevice* d, D9CSurface* rt,
                                                        D9CSurface* dst);

namespace {

// C1 helper: route per-record dirty marking through the dxmt9
// CommandQueue's pendingDirty_ accumulator. Returns nullptr on test /
// stub paths where no upperDevice exists; callers no-op in that case.
dxmt9::CommandQueue* findDirtyQueue(D9CDevice* d) {
  if (!d) return nullptr;
  auto upper = d->dev().upperDevice();
  if (!upper) return nullptr;
  return &upper->queue();
}

// C1 helper: D3D9 render-state IDs that affect each FFP PS uniform
// sub-block. Centralized so commit_chunk's APPLY_STATE handler and
// the per-call SetRenderState dispatcher (if it ever wires direct
// dirty-marking) stay in sync.
bool isFogRenderState(uint32_t state) {
  return state == dxmt9::core::RS_FOG_ENABLE ||
         state == dxmt9::core::RS_FOG_COLOR ||
         state == dxmt9::core::RS_FOG_TABLE_MODE ||
         state == dxmt9::core::RS_FOG_START ||
         state == dxmt9::core::RS_FOG_END ||
         state == dxmt9::core::RS_FOG_DENSITY ||
         state == dxmt9::core::RS_FOG_FROM_VERTEX;
}

bool isAlphaRenderState(uint32_t state) {
  return state == dxmt9::core::RS_ALPHA_TEST_ENABLE ||
         state == dxmt9::core::RS_ALPHA_FUNC ||
         state == dxmt9::core::RS_ALPHA_REF;
}

// C1 helper: scan a draw-packet state delta and OR matching dirty bits
// onto the queue's pendingDirty_. Used by APPLY_STATE record
// dispatch AND by every DRAW_* record (which folds a state delta in
// front of its draw call via applyDrawPacketState). Safe to call when
// q is nullptr (no-op for stub / test paths).
void markDirtyFromDrawPacketState(dxmt9::CommandQueue* q,
                                  const D9CDrawPrimitivePacket& packet) {
  if (!q) return;
  for (uint32_t i = 0; i < packet.renderStateCount; ++i) {
    const auto& entry = packet.renderStates[i];
    if (isFogRenderState(entry.state)) q->applyDirtyRenderStateFog();
    if (isAlphaRenderState(entry.state)) q->applyDirtyRenderStateAlpha();
    if (entry.state == dxmt9::core::RS_TEXTURE_FACTOR) {
      q->applyDirtyRenderStateTexFactor();
    }
  }
  for (uint32_t i = 0; i < packet.tssCount; ++i) {
    if (packet.tss[i].type == dxmt9::core::TSS_CONSTANT) {
      q->applyDirtyTextureStageConstant();
    }
  }
  if (packet.transformCount > 0) q->applyDirtyTransformChange();
  // Light slot/enable + material deltas share the FFP VS uniform
  // block with transforms (design.md §10 — lights[8] sits next to
  // worldViewMatrix); folded into the transforms-bit so the FFP VS
  // uniform block re-uploads.
  if (packet.lightSlotMask != 0 ||
      packet.lightEnableValidMask != 0 ||
      packet.materialValid) {
    q->applyDirtyTransformChange();
  }
  if (packet.clipPlaneMask != 0) q->applyDirtyClipPlaneChange();
  if (packet.viewportValid) q->applyDirtyViewportChange();
}

uint64_t wireHandleValue(const D9CWireHandle& handle) {
  return static_cast<uint64_t>(handle.lo) | (static_cast<uint64_t>(handle.hi) << 32);
}

template <typename T>
T* wireValuePtr(uint64_t value) {
  if (!value) {
    return nullptr;
  }
  if (value <= 0xffffffffull) {
    if (auto* decoded =
            dxmt9::util::marshal::wow64::decodeHandle<T*>(static_cast<uint32_t>(value))) {
      return decoded;
    }
    if (isWow64NativePointerAllowed(value)) {
      return reinterpret_cast<T*>(static_cast<uintptr_t>(value));
    }
    if (requiresWow64PointerShadow()) {
      return nullptr;
    }
  }
  return reinterpret_cast<T*>(static_cast<uintptr_t>(value));
}

template <typename T>
T* wireHandlePtr(const D9CWireHandle& handle) {
  return wireValuePtr<T>(wireHandleValue(handle));
}

bool failed(int32_t hr) {
  return hr < 0;
}

void recordStateBlockRenderState(D9CDevice* d, uint32_t state, uint32_t value) {
  d->stateBlockRenderStates.insert(state);
  d->stateBlockRenderStateValues[state] = value;
}

dxmt9::core::Matrix4x4 matrixFromC(const D9CMatrix& m) {
  dxmt9::core::Matrix4x4 matrix;
  std::memcpy(matrix.m.data(), m.m, 16 * sizeof(float));
  return matrix;
}

dxmt9::core::Viewport viewportFromC(const D9CViewport& vp) {
  return {vp.x, vp.y, vp.width, vp.height, vp.minZ, vp.maxZ};
}

bool viewportValid(const dxmt9::core::Viewport& viewport) {
  return viewport.width != 0 && viewport.height != 0 &&
         std::isfinite(viewport.minZ) && std::isfinite(viewport.maxZ) &&
         viewport.minZ >= 0.0f && viewport.maxZ <= 1.0f &&
         viewport.minZ <= viewport.maxZ;
}

dxmt9::core::Rect rectFromC(const D9CRect& r) {
  return {r.left, r.top, r.right, r.bottom};
}

dxmt9::core::Material materialFromC(const D9CMaterial& m) {
  dxmt9::core::Material material;
  std::memcpy(&material.diffuse, &m.diffuse, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&material.ambient, &m.ambient, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&material.specular, &m.specular, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&material.emissive, &m.emissive, sizeof(dxmt9::core::ColorRGBA));
  material.power = m.power;
  return material;
}

dxmt9::core::Light lightFromC(const D9CLight& l) {
  dxmt9::core::Light light;
  light.type = static_cast<dxmt9::core::LightType>(l.type);
  std::memcpy(&light.diffuse, &l.diffuse, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&light.specular, &l.specular, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&light.ambient, &l.ambient, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(light.position.data(), l.position, 3 * sizeof(float));
  std::memcpy(light.direction.data(), l.direction, 3 * sizeof(float));
  light.range = l.range;
  light.falloff = l.falloff;
  light.attenuation0 = l.attenuation0;
  light.attenuation1 = l.attenuation1;
  light.attenuation2 = l.attenuation2;
  light.theta = l.theta;
  light.phi = l.phi;
  return light;
}

dxmt9::core::RenderTargetAttachment attachmentFromSurface(
    const std::shared_ptr<dxmt9::core::Surface>& surface) {
  return surface ? dxmt9::core::RenderTargetAttachment{
                       surface->handle(), surface->level(), surface->multiSampleCount()}
                 : dxmt9::core::RenderTargetAttachment{};
}

int32_t validateDrawPacketStateDelta(const D9CDrawPrimitivePacket& packet) {
  if (packet.renderStateCount > D9C_DRAW_PACKET_MAX_RENDER_STATES ||
      packet.tssCount > D9C_DRAW_PACKET_MAX_TSS ||
      packet.samplerStateCount > D9C_DRAW_PACKET_MAX_SAMPLER ||
      packet.transformCount > D9C_DRAW_PACKET_MAX_TRANSFORMS) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  for (uint32_t i = 0; i < packet.samplerStateCount; ++i) {
    if (packet.samplerStates[i].sampler >= dxmt9::core::kMaxSamplers) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
  }
  if (packet.viewportValid && !viewportValid(viewportFromC(packet.viewport))) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  return dxmt9::core::D3D_OK;
}

int32_t commitChunkFail(const char* reason,
                        std::uint32_t index = 0xffffffffu,
                        std::uint32_t type = 0,
                        int32_t hr = dxmt9::core::D3DERR_INVALIDCALL) {
  dxmt9DebugLog("commit_chunk fail reason=%s index=%u type=%u hr=0x%08x",
                reason, index, type, static_cast<std::uint32_t>(hr));
  // R-BACK-2.10: every commit_chunk reject path funnels through here.
  dxmt9::perf::countChunkReject();
  return hr;
}

int32_t applyDrawPacketStateViaIface(D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  if (packet.renderStateCount > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }

  for (uint32_t i = 0; i < packet.renderStateCount; ++i) {
    const auto& state = packet.renderStates[i];
    if (d->stateBlockRecording) {
      recordStateBlockRenderState(d, state.state, state.value);
      continue;
    }
    const int32_t hr = d->iface->SetRenderState(state.state, state.value);
    if (failed(hr)) {
      return hr;
    }
  }

  for (uint32_t stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
    if ((packet.textureMask & (1u << stage)) == 0) {
      continue;
    }
    auto texture = wireHandlePtr<D9CTexture>(packet.textures[stage]);
    const int32_t hr = d->iface->SetTexture(stage, texture ? texture->obj : nullptr);
    if (failed(hr)) {
      return hr;
    }
  }

  for (uint32_t stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
    if ((packet.streamSourceMask & (1u << stream)) == 0) {
      continue;
    }
    const auto& source = packet.streamSources[stream];
    auto buffer = wireHandlePtr<D9CBuffer>(source.buffer);
    const int32_t hr = d->iface->SetStreamSource(stream, buffer ? buffer->obj : nullptr,
                                                 source.offset, source.stride);
    if (failed(hr)) {
      return hr;
    }
  }

  if (packet.fvfValid) {
    const int32_t hr = d->iface->SetFVF(packet.fvf);
    if (failed(hr)) {
      return hr;
    }
  }

  // Phase 12: shader-handle delta dispatch. Wire handles are server-side
  // D9CShader* casts (rawVS/rawPS); decode + call dxmt9c_device_set_*_shader.
  if (packet.vsValid) {
    auto* vs = wireHandlePtr<D9CShader>(packet.vsHandle);
    const int32_t hr = dxmt9c_device_set_vertex_shader(d, vs);
    if (failed(hr)) return hr;
  }
  if (packet.psValid) {
    auto* ps = wireHandlePtr<D9CShader>(packet.psHandle);
    const int32_t hr = dxmt9c_device_set_pixel_shader(d, ps);
    if (failed(hr)) return hr;
  }

  // Phase 12: vertex-decl handle delta.
  if (packet.vdeclValid) {
    auto* vd = wireHandlePtr<D9CVertexDecl>(packet.vdeclHandle);
    const int32_t hr = dxmt9c_device_set_vertex_declaration(d, vd);
    if (failed(hr)) return hr;
  }

  // Phase 12: render-target deltas. One dispatch per set bit in rtMask.
  for (uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
    if ((packet.rtMask & (1u << slot)) == 0) continue;
    auto* rt = wireHandlePtr<D9CSurface>(packet.rtHandles[slot]);
    const int32_t hr = dxmt9c_device_set_render_target(d, slot, rt);
    if (failed(hr)) return hr;
  }

  // Phase 12: depth-stencil delta.
  if (packet.dsValid) {
    auto* ds = wireHandlePtr<D9CSurface>(packet.dsHandle);
    const int32_t hr = dxmt9c_device_set_depth_stencil(d, ds);
    if (failed(hr)) return hr;
  }

  // Phase 12: viewport / scissor deltas.
  if (packet.viewportValid) {
    const int32_t hr = dxmt9c_device_set_viewport(d, &packet.viewport);
    if (failed(hr)) return hr;
  }
  if (packet.scissorValid) {
    const int32_t hr = dxmt9c_device_set_scissor_rect(d, &packet.scissor);
    if (failed(hr)) return hr;
  }

  // Phase 12: TSS / SamplerState delta dispatch.
  if (packet.tssCount > D9C_DRAW_PACKET_MAX_TSS ||
      packet.samplerStateCount > D9C_DRAW_PACKET_MAX_SAMPLER) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  for (uint32_t i = 0; i < packet.tssCount; ++i) {
    const auto& e = packet.tss[i];
    const int32_t hr = dxmt9c_device_set_texture_stage_state(d, e.stage, e.type, e.value);
    if (failed(hr)) return hr;
  }
  for (uint32_t i = 0; i < packet.samplerStateCount; ++i) {
    const auto& e = packet.samplerStates[i];
    const int32_t hr = dxmt9c_device_set_sampler_state(d, e.sampler, e.type, e.value);
    if (failed(hr)) return hr;
  }

  // Phase 12: material delta.
  if (packet.materialValid) {
    const int32_t hr = dxmt9c_device_set_material(d, &packet.material);
    if (failed(hr)) return hr;
  }

  // Phase 12: clip-plane delta — one dispatch per set bit in
  // clipPlaneMask. Each plane is 4 floats stored contiguously at
  // clipPlanes[i*4..i*4+3].
  for (uint32_t i = 0; i < 6; ++i) {
    if ((packet.clipPlaneMask & (1u << i)) == 0) continue;
    const int32_t hr = dxmt9c_device_set_clip_plane(d, i, &packet.clipPlanes[i * 4]);
    if (failed(hr)) return hr;
  }

  // Phase 12: Transform delta — packet.transforms[0..transformCount).
  if (packet.transformCount > D9C_DRAW_PACKET_MAX_TRANSFORMS) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  for (uint32_t i = 0; i < packet.transformCount; ++i) {
    const auto& e = packet.transforms[i];
    const int32_t hr = dxmt9c_device_set_transform(d, e.state, &e.matrix);
    if (failed(hr)) return hr;
  }

  // Phase 12: Light delta — one dispatch per set bit in lightSlotMask.
  for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
    if ((packet.lightSlotMask & (1u << i)) == 0) continue;
    const int32_t hr = dxmt9c_device_set_light(d, i, &packet.lights[i]);
    if (failed(hr)) return hr;
  }

  // Phase 12: LightEnable delta. ValidMask says which slots have a fresh
  // value; lightEnableMask carries the new enabled bit per slot.
  for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
    if ((packet.lightEnableValidMask & (1u << i)) == 0) continue;
    const uint32_t enabled = (packet.lightEnableMask & (1u << i)) ? 1u : 0u;
    const int32_t hr = dxmt9c_device_light_enable(d, i, enabled);
    if (failed(hr)) return hr;
  }

  return dxmt9::core::D3D_OK;
}

// Normal chunk replay path: apply the packet's flat delta directly to the
// core DeviceState and invalidate derived draw-state caches once. The iface
// replay above remains only for state-block recording semantics.
int32_t applyDrawPacketStateDirect(D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  const int32_t validationHr = validateDrawPacketStateDelta(packet);
  if (failed(validationHr)) {
    return validationHr;
  }

  auto& state = d->dev().mutableState();

  for (uint32_t i = 0; i < packet.renderStateCount; ++i) {
    const auto& entry = packet.renderStates[i];
    state.renderStates.set(entry.state, entry.value);
    if (entry.state == dxmt9::core::RS_SCISSOR_TEST_ENABLE) {
      state.scissorEnabled = entry.value != 0;
    }
  }

  for (uint32_t stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
    if ((packet.textureMask & (1u << stage)) == 0) {
      continue;
    }
    auto* texture = wireHandlePtr<D9CTexture>(packet.textures[stage]);
    state.textures[stage] = texture ? texture->obj : nullptr;
  }

  for (uint32_t stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
    if ((packet.streamSourceMask & (1u << stream)) == 0) {
      continue;
    }
    const auto& source = packet.streamSources[stream];
    auto* buffer = wireHandlePtr<D9CBuffer>(source.buffer);
    state.streamBuffers[stream] = buffer ? buffer->obj : nullptr;
    state.streamOffsets[stream] = source.offset;
    state.streamStrides[stream] = source.stride;
  }

  if (packet.fvfValid) {
    state.fvf = packet.fvf;
    state.vertexDecl.fvf = packet.fvf;
    state.vertexDecl.elements.clear();
  }

  if (packet.vsValid) {
    auto* vs = wireHandlePtr<D9CShader>(packet.vsHandle);
    state.vertexShader = vs ? vs->ref : dxmt9::core::ShaderRef{};
  }
  if (packet.psValid) {
    auto* ps = wireHandlePtr<D9CShader>(packet.psHandle);
    state.pixelShader = ps ? ps->ref : dxmt9::core::ShaderRef{};
  }

  if (packet.vdeclValid) {
    auto* vd = wireHandlePtr<D9CVertexDecl>(packet.vdeclHandle);
    if (vd) {
      state.vertexDecl.elements = vd->elements;
    } else {
      state.vertexDecl.elements.clear();
    }
    state.vertexDecl.fvf = state.fvf;
  }

  for (uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
    if ((packet.rtMask & (1u << slot)) == 0) {
      continue;
    }
    auto* rt = wireHandlePtr<D9CSurface>(packet.rtHandles[slot]);
    const auto surface = rt ? rt->obj : nullptr;
    state.renderTargets[slot] = attachmentFromSurface(surface);
    if (slot == 0 && surface) {
      const auto& desc = surface->desc();
      state.viewport = {0, 0, std::max(1u, desc.width), std::max(1u, desc.height),
                        0.0f, 1.0f};
    }
  }

  if (packet.dsValid) {
    auto* ds = wireHandlePtr<D9CSurface>(packet.dsHandle);
    state.depthStencil = attachmentFromSurface(ds ? ds->obj : nullptr);
  }

  if (packet.viewportValid) {
    state.viewport = viewportFromC(packet.viewport);
  }
  if (packet.scissorValid) {
    state.scissorRect = rectFromC(packet.scissor);
  }

  for (uint32_t i = 0; i < packet.tssCount; ++i) {
    const auto& e = packet.tss[i];
    const uint32_t stage = std::min(e.stage, dxmt9::core::kMaxTextureStages - 1);
    const uint32_t key = std::min(e.type, dxmt9::core::kMaxTextureStageStates - 1);
    state.textureStageStates[stage].set(key, e.value);
  }
  for (uint32_t i = 0; i < packet.samplerStateCount; ++i) {
    const auto& e = packet.samplerStates[i];
    state.samplerStates[e.sampler].set(e.type, e.value);
  }

  if (packet.materialValid) {
    state.material = materialFromC(packet.material);
  }

  for (uint32_t i = 0; i < dxmt9::core::kMaxClipPlanes; ++i) {
    if ((packet.clipPlaneMask & (1u << i)) == 0) {
      continue;
    }
    state.clipPlanes[i] = {packet.clipPlanes[i * 4 + 0],
                           packet.clipPlanes[i * 4 + 1],
                           packet.clipPlanes[i * 4 + 2],
                           packet.clipPlanes[i * 4 + 3]};
  }

  for (uint32_t i = 0; i < packet.transformCount; ++i) {
    const auto& e = packet.transforms[i];
    state.transforms.set(transformStateFromD3D(e.state), matrixFromC(e.matrix));
  }

  for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
    if ((packet.lightSlotMask & (1u << i)) == 0) {
      continue;
    }
    state.lights[i] = lightFromC(packet.lights[i]);
  }
  for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
    if ((packet.lightEnableValidMask & (1u << i)) == 0) {
      continue;
    }
    const bool enabled = (packet.lightEnableMask & (1u << i)) != 0;
    state.lightEnabled[i] = enabled;
    state.lights[i].enabled = enabled;
  }

  return dxmt9::core::D3D_OK;
}

int32_t applyDrawPacketState(D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  if (d->stateBlockRecording) {
    return applyDrawPacketStateViaIface(d, packet);
  }
  return applyDrawPacketStateDirect(d, packet);
}

bool recordRangeValid(std::uint32_t recordSize, std::uint32_t offset, std::uint32_t bytes) {
  return offset <= recordSize && bytes <= recordSize - offset;
}

// Direct core::Device dispatch — bypasses the COM iface (Direct3DDevice9Impl)
// hop. The COM Draw* methods are 1-line forwarders to core::Device, so
// dispatching one record at a time through them costs an extra virtual call
// and an AddRef/Release-bearing path with no behavioral effect. The chunk
// importer is hot — every D9CCommandRecord_DRAW_* takes this path — so
// removing that hop is meaningful per-draw cost relief.
int32_t applyDrawPrimitivePacket(D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  const int32_t stateHr = applyDrawPacketState(d, packet);
  if (failed(stateHr)) {
    return stateHr;
  }
  return d->dev().drawPrimitive(ptFromD3D(packet.primitiveType),
                                packet.primitiveCount,
                                packet.startVertex);
}

int32_t applyDrawIndexedPrimitivePacket(D9CDevice* d,
                                        const D9CDrawIndexedPrimitivePacket& packet) {
  const int32_t stateHr = applyDrawPacketState(d, packet.state);
  if (failed(stateHr)) {
    return stateHr;
  }
  // Phase 12: index buffer delta — applied AFTER applyDrawPacketState so
  // it overrides any prior IB state, BEFORE the actual indexed draw.
  if (packet.ibValid) {
    auto* ib = wireHandlePtr<D9CBuffer>(packet.ibHandle);
    const int32_t hr = dxmt9c_device_set_indices(d, ib);
    if (failed(hr)) return hr;
  }
  const auto& state = d->dev().state();
  return d->dev().drawIndexedPrimitive(ptFromD3D(packet.state.primitiveType),
                                       packet.primitiveCount, 0, packet.baseVertex,
                                       packet.startIndex, state.indexType);
}

int32_t applyDrawPrimitiveUPPacket(D9CDevice* d,
                                   const D9CDrawPrimitiveUPPacket& packet,
                                   const dxmt9::core::u8* record,
                                   std::uint32_t recordSize) {
  const int32_t stateHr = applyDrawPacketState(d, packet.state);
  if (failed(stateHr)) {
    return stateHr;
  }
  if (!recordRangeValid(recordSize, packet.vertexDataOffset, packet.vertexDataSize)) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto span = std::span<const dxmt9::core::u8>(record + packet.vertexDataOffset,
                                               packet.vertexDataSize);
  return d->dev().drawPrimitiveUP(ptFromD3D(packet.state.primitiveType),
                                  packet.primitiveCount, span, packet.stride);
}

int32_t applyDrawIndexedPrimitiveUPPacket(D9CDevice* d,
                                          const D9CDrawIndexedPrimitiveUPPacket& packet,
                                          const dxmt9::core::u8* record,
                                          std::uint32_t recordSize) {
  const int32_t stateHr = applyDrawPacketState(d, packet.state);
  if (failed(stateHr)) {
    return stateHr;
  }
  if (!recordRangeValid(recordSize, packet.indexDataOffset, packet.indexDataSize) ||
      !recordRangeValid(recordSize, packet.vertexDataOffset, packet.vertexDataSize)) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto vertexSpan = std::span<const dxmt9::core::u8>(record + packet.vertexDataOffset,
                                                     packet.vertexDataSize);
  auto indexSpan = std::span<const dxmt9::core::u8>(record + packet.indexDataOffset,
                                                    packet.indexDataSize);
  return d->dev().drawIndexedPrimitiveUP(ptFromD3D(packet.state.primitiveType),
                                         packet.primitiveCount, vertexSpan, indexSpan,
                                         idxTypeFromD3D(packet.indexFormat), packet.stride);
}

}  // namespace

extern "C" int32_t dxmt9c_device_commit_chunk(D9CDevice* d, const D9CCommandChunk* chunk) {
  // V1 boundary B2 — wall-clock latency of one bridge crossing. Sampled
  // at the d3d9-side bridge entry (this function) so the measurement
  // reflects WINE_UNIX_CALL marshalling + importer validation + seqId
  // assignment in isolation from encode and GPU work. Recorded only at
  // the success exit (mirrors countChunkAdmit) so a reject path with a
  // fast-rejected malformed payload does not skew the success-path
  // distribution that the bridge_empty probe regression-gates on.
  const auto bridgeCommitStart = std::chrono::steady_clock::now();
  if (!d || !chunk || chunk->version != D9C_COMMAND_CHUNK_VERSION) {
    return commitChunkFail("bad-header");
  }
  const auto* records = chunk->recordBytes != 0
                            ? wireHandlePtr<const dxmt9::core::u8>(chunk->records)
                            : nullptr;
  if (!records && chunk->recordBytes != 0) {
    return commitChunkFail("missing-records");
  }

  ImportedWireChunkView importedChunk{};
  const auto wireBlob = makeImportedWireChunkBlobView(records, chunk->recordBytes);
  if (!wireBlob.valid()) {
    return commitChunkFail("bad-wire-blob", 0xffffffffu,
                           static_cast<std::uint32_t>(wireBlob.status));
  }
  importedChunk = wireBlob.chunk;
  if ((chunk->recordCount != 0 && importedChunk.recordCount != chunk->recordCount) ||
      (chunk->handleCount != 0 && importedChunk.handleCount != chunk->handleCount)) {
    return commitChunkFail("count-mismatch", importedChunk.recordCount,
                           importedChunk.handleCount);
  }

  const auto validation = validateImportedWireChunk(importedChunk);
  if (!validation.valid()) {
    return commitChunkFail("validation", validation.failedRecordIndex,
                           static_cast<std::uint32_t>(validation.status));
  }

  // Phase 4 / 18: validate and retain only handles selected by record
  // handle ranges.
  //
  // The wire payload from PE carries the SERVER-SIDE D9C wrapper
  // pointer (D9CTexture* / D9CBuffer* / D9CSurface*) cast to uint64,
  // not the backend's core::Handle. Decode each pointer to its
  // underlying core::*::handle() value before handing the entry list
  // to CommandQueue::markChunkResources — otherwise pool.find{Texture,
  // Surface,Buffer} on a wrapper-pointer-as-handle would never match
  // and the bulk mark would silently be a no-op.
  bool didBulkMarkResources = false;
  if (importedChunk.handleCount > 0) {
    ImportedChunkHandleSet retainedWireHandles;
    if (!collectImportedWireChunkHandles(importedChunk, retainedWireHandles)) {
      return commitChunkFail("collect-handles");
    }
    const auto retainedEntries = makeImportedChunkHandleEntries(retainedWireHandles);

    std::vector<dxmt9::core::ChunkHandleEntry> coreEntries;
    coreEntries.reserve(retainedEntries.size());
    for (const auto& handle : retainedEntries) {
      const auto kind = static_cast<dxmt9::core::ChunkHandleKind>(handle.kind);
      const auto wirePtr = handle.handle;
      if (wirePtr == 0) continue;
      dxmt9::core::Handle resolved{};
      switch (kind) {
      case dxmt9::core::ChunkHandleKind::Texture: {
        auto* wrapper = wireValuePtr<D9CTexture>(wirePtr);
        if (wrapper && wrapper->obj) resolved = wrapper->obj->handle();
        break;
      }
      case dxmt9::core::ChunkHandleKind::Surface: {
        auto* wrapper = wireValuePtr<D9CSurface>(wirePtr);
        if (wrapper && wrapper->obj) resolved = wrapper->obj->handle();
        break;
      }
      case dxmt9::core::ChunkHandleKind::Buffer: {
        auto* wrapper = wireValuePtr<D9CBuffer>(wirePtr);
        if (wrapper && wrapper->obj) resolved = wrapper->obj->handle();
        break;
      }
      case dxmt9::core::ChunkHandleKind::Shader:
      case dxmt9::core::ChunkHandleKind::VertexDecl:
        // No pool retention table for shaders / vertex decls — skip.
        break;
      }
      if (resolved.value == 0) continue;
      coreEntries.push_back(dxmt9::core::ChunkHandleEntry{
          .kind = kind,
          .handle = resolved,
      });
    }
    if (!coreEntries.empty()) {
      if (auto upper = d->dev().upperDevice()) {
        upper->markChunkResources(coreEntries);
        didBulkMarkResources = true;
      }
    }
  }

  // Phase 14: bulk markChunkResources has already pinned every resource
  // in this chunk against its seqId. Suppress the submitDrawRun
  // markDrawResources walk for the duration of this record-iter block;
  // the RAII guard clears the flag even if a record returns early.
  // Chunks with no retained pool resources keep the normal run-level
  // hot-state marking path.
  struct ResetSkipDrawMarkGuard {
    std::shared_ptr<dxmt9::Device> upper;
    ~ResetSkipDrawMarkGuard() {
      if (upper) upper->setSkipDrawResourceMarking(false);
    }
  } resetGuard{};
  if (didBulkMarkResources) {
    if (auto upper = d->dev().upperDevice()) {
      upper->setSkipDrawResourceMarking(true);
      resetGuard.upper = std::move(upper);
    }
  }

  std::uint32_t recordIndex = 0;
  while (auto recordView = nextImportedRecord(importedChunk, recordIndex)) {
    if (!recordView->valid()) {
      return commitChunkFail("record-view", recordIndex);
    }

    const auto header = recordView->header;
    const auto* record = recordView->record;
    int32_t hr = dxmt9::core::D3DERR_INVALIDCALL;
    switch (header.type) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE: {
      D9CCommandRecordDrawPrimitive decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      // C1: every DRAW_* packet folds a state delta in front of its
      // draw via applyDrawPacketState; mark the matching dirty bits so
      // C2 sees the same categories the canonical state changed.
      markDirtyFromDrawPacketState(findDirtyQueue(d), decoded.packet);
      // Try to coalesce into a draw run: scan ahead for consecutive
      // DRAW_PRIMITIVE records that also have empty state deltas, append
      // scanned DrawParam records into a flat span, then submit once (single
      // canonical hot-state build + single queue submission).
      // Falls through to per-record path if the run is just length-1.
      const auto scan = scanImportedDrawRun(importedChunk, *recordView);
      if (scan.replayAsRun()) {
        std::vector<dxmt9::core::DrawParam> runParams;
        runParams.reserve(scan.recordCount);
        runParams.push_back(makeRunParam(decoded.packet));

        std::uint32_t runIndex = recordView->nextIndex();
        while (runIndex < scan.endIndex) {
          const auto nextRecord = nextImportedRecord(importedChunk, runIndex);
          if (!nextRecord || !nextRecord->valid()) {
            return commitChunkFail("draw-run-record", runIndex, header.type);
          }
          D9CCommandRecordDrawPrimitive nextDecoded{};
          std::memcpy(&nextDecoded, nextRecord->record, sizeof(nextDecoded));
          runParams.push_back(makeRunParam(nextDecoded.packet));
          runIndex = nextRecord->nextIndex();
        }
        if (runParams.size() != scan.recordCount) {
          return commitChunkFail("draw-run-count", recordIndex, header.type);
        }

        // applyDrawPacketState would be a no-op here (delta is empty),
        // so skip directly to drawPrimitiveRun. Bypasses N-1
        // applyDrawPrimitivePacket / canonical-state builds.
        hr = d->dev().drawPrimitiveRun(runParams);
        if (failed(hr)) return commitChunkFail("draw-run", recordIndex, header.type, hr);
        recordIndex = scan.endIndex;
        continue;
      }
      hr = applyDrawPrimitivePacket(d, decoded.packet);
      break;
    }
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE: {
      D9CCommandRecordDrawIndexedPrimitive decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      markDirtyFromDrawPacketState(findDirtyQueue(d), decoded.packet.state);
      // Same coalescing as DRAW_PRIMITIVE — separate run per indexed
      // flag because DrawParam encodes that as a static field used by
      // the encoder to dispatch drawIndexed vs draw.
      const auto scan = scanImportedDrawRun(importedChunk, *recordView);
      if (scan.replayAsRun()) {
        std::vector<dxmt9::core::DrawParam> runParams;
        runParams.reserve(scan.recordCount);
        runParams.push_back(makeRunParam(decoded.packet));

        std::uint32_t runIndex = recordView->nextIndex();
        while (runIndex < scan.endIndex) {
          const auto nextRecord = nextImportedRecord(importedChunk, runIndex);
          if (!nextRecord || !nextRecord->valid()) {
            return commitChunkFail("indexed-draw-run-record", runIndex, header.type);
          }
          D9CCommandRecordDrawIndexedPrimitive nextDecoded{};
          std::memcpy(&nextDecoded, nextRecord->record, sizeof(nextDecoded));
          runParams.push_back(makeRunParam(nextDecoded.packet));
          runIndex = nextRecord->nextIndex();
        }
        if (runParams.size() != scan.recordCount) {
          return commitChunkFail("indexed-draw-run-count", recordIndex, header.type);
        }

        hr = d->dev().drawPrimitiveRun(runParams);
        if (failed(hr)) return commitChunkFail("indexed-draw-run", recordIndex, header.type, hr);
        recordIndex = scan.endIndex;
        continue;
      }
      hr = applyDrawIndexedPrimitivePacket(d, decoded.packet);
      break;
    }
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP: {
      D9CCommandRecordDrawPrimitiveUP decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      markDirtyFromDrawPacketState(findDirtyQueue(d), decoded.packet.state);
      hr = applyDrawPrimitiveUPPacket(d, decoded.packet, record, header.size);
      break;
    }
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP: {
      D9CCommandRecordDrawIndexedPrimitiveUP decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      markDirtyFromDrawPacketState(findDirtyQueue(d), decoded.packet.state);
      hr = applyDrawIndexedPrimitiveUPPacket(d, decoded.packet, record, header.size);
      break;
    }
    case D9C_COMMAND_RECORD_CLEAR: {
      D9CCommandRecordClear cl{};
      std::memcpy(&cl, record, sizeof(cl));
      const auto* rects = cl.rectCount != 0
                              ? reinterpret_cast<const D9CRect*>(record + cl.rectOffset)
                              : nullptr;
      hr = dxmt9c_device_clear(d, cl.rectCount, rects, cl.flags, cl.colorARGB,
                                cl.z, cl.stencil);
      break;
    }
    case D9C_COMMAND_RECORD_PRESENT: {
      D9CCommandRecordPresent pr{};
      std::memcpy(&pr, record, sizeof(pr));
      const auto* srcRect = pr.hasSrc ? &pr.src : nullptr;
      const auto* dstRect = pr.hasDst ? &pr.dst : nullptr;
      // dirty-region payload was dropped at chunk-record time (PE
      // doesn't ship it); pass nullptr.
      hr = dxmt9c_device_present(d, srcRect, dstRect, pr.hwnd,
                                  /*dirtyRegion=*/nullptr, pr.flags);
      break;
    }
    case D9C_COMMAND_RECORD_STRETCH_RECT: {
      D9CCommandRecordStretchRect sr{};
      std::memcpy(&sr, record, sizeof(sr));
      auto* srcSurf = wireValuePtr<D9CSurface>(sr.srcWire);
      auto* dstSurf = wireValuePtr<D9CSurface>(sr.dstWire);
      const auto* srcR = sr.hasSrcRect ? &sr.srcRect : nullptr;
      const auto* dstR = sr.hasDstRect ? &sr.dstRect : nullptr;
      hr = dxmt9c_device_stretch_rect(d, srcSurf, srcR, dstSurf, dstR, sr.filter);
      break;
    }
    case D9C_COMMAND_RECORD_COLOR_FILL: {
      D9CCommandRecordColorFill cf{};
      std::memcpy(&cf, record, sizeof(cf));
      auto* surf = wireValuePtr<D9CSurface>(cf.surfaceWire);
      const auto* rect = cf.hasRect ? &cf.rect : nullptr;
      hr = dxmt9c_device_color_fill(d, surf, rect, cf.colorARGB);
      break;
    }
    case D9C_COMMAND_RECORD_UPDATE_TEXTURE: {
      D9CCommandRecordUpdateTexture ut{};
      std::memcpy(&ut, record, sizeof(ut));
      auto* srcTex = wireValuePtr<D9CTexture>(ut.srcWire);
      auto* dstTex = wireValuePtr<D9CTexture>(ut.dstWire);
      hr = dxmt9c_device_update_texture(d, srcTex, dstTex);
      break;
    }
    case D9C_COMMAND_RECORD_UPDATE_SURFACE: {
      D9CCommandRecordUpdateSurface us{};
      std::memcpy(&us, record, sizeof(us));
      auto* srcSurf = wireValuePtr<D9CSurface>(us.srcWire);
      auto* dstSurf = wireValuePtr<D9CSurface>(us.dstWire);
      const auto* srcRect = us.hasSrcRect ? &us.srcRect : nullptr;
      const auto* dstPoint = us.hasDstPoint ? &us.dstPoint : nullptr;
      hr = dxmt9c_device_update_surface(d, srcSurf, srcRect, dstSurf, dstPoint);
      break;
    }
    case D9C_COMMAND_RECORD_QUERY_ISSUE: {
      D9CCommandRecordQueryIssue qi{};
      std::memcpy(&qi, record, sizeof(qi));
      auto* query = wireValuePtr<D9CQuery>(qi.queryWire);
      hr = dxmt9c_query_issue(query, qi.flags);
      break;
    }
    case D9C_COMMAND_RECORD_READBACK: {
      D9CCommandRecordReadback rb{};
      std::memcpy(&rb, record, sizeof(rb));
      auto* srcSurf = wireValuePtr<D9CSurface>(rb.srcWire);
      auto* dstSurf = wireValuePtr<D9CSurface>(rb.dstWire);
      // Routes to the same backend path as the legacy bridge call —
      // encodes the copy + waits for GPU completion + writes pixels
      // into dst. HRESULT propagates back through commit_chunk's
      // per-record short-circuit to the PE caller.
      hr = dxmt9c_device_get_render_target_data(d, srcSurf, dstSurf);
      break;
    }
    case D9C_COMMAND_RECORD_APPLY_STATE: {
      D9CCommandRecordApplyState as{};
      std::memcpy(&as, record, sizeof(as));
      // Apply the state delta only; draw fields in the packet
      // (primitiveType / primitiveCount / startVertex) are unused.
      hr = applyDrawPacketState(d, as.packet);
      // C1 dirty tracking: APPLY_STATE bundles RS / transform / clip /
      // viewport / light / material deltas. False-dirty is safe per
      // task spec; the alternative (missing-dirty) is a correctness bug.
      markDirtyFromDrawPacketState(findDirtyQueue(d), as.packet);
      break;
    }
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
    case D9C_COMMAND_RECORD_SET_PS_CONST_B: {
      D9CCommandRecordSetConst hdr{};
      std::memcpy(&hdr, record, sizeof(hdr));
      const auto* payload = record + sizeof(hdr);
      auto* q = findDirtyQueue(d);
      switch (header.type) {
      case D9C_COMMAND_RECORD_SET_VS_CONST_F:
        hr = dxmt9c_device_set_vs_const_f(d, hdr.start,
                                          reinterpret_cast<const float*>(payload),
                                          hdr.count);
        if (q) q->applyDirtyConstantSetVsF(hdr.start, hdr.count);
        break;
      case D9C_COMMAND_RECORD_SET_PS_CONST_F:
        hr = dxmt9c_device_set_ps_const_f(d, hdr.start,
                                          reinterpret_cast<const float*>(payload),
                                          hdr.count);
        if (q) q->applyDirtyConstantSetPsF(hdr.start, hdr.count);
        break;
      case D9C_COMMAND_RECORD_SET_VS_CONST_I:
        hr = dxmt9c_device_set_vs_const_i(d, hdr.start,
                                          reinterpret_cast<const int32_t*>(payload),
                                          hdr.count);
        if (q) q->applyDirtyConstantSetVsI(hdr.start, hdr.count);
        break;
      case D9C_COMMAND_RECORD_SET_PS_CONST_I:
        hr = dxmt9c_device_set_ps_const_i(d, hdr.start,
                                          reinterpret_cast<const int32_t*>(payload),
                                          hdr.count);
        if (q) q->applyDirtyConstantSetPsI(hdr.start, hdr.count);
        break;
      case D9C_COMMAND_RECORD_SET_VS_CONST_B:
        hr = dxmt9c_device_set_vs_const_b(d, hdr.start,
                                          reinterpret_cast<const uint32_t*>(payload),
                                          hdr.count);
        if (q) q->applyDirtyConstantSetVsB(hdr.start, hdr.count);
        break;
      case D9C_COMMAND_RECORD_SET_PS_CONST_B:
        hr = dxmt9c_device_set_ps_const_b(d, hdr.start,
                                          reinterpret_cast<const uint32_t*>(payload),
                                          hdr.count);
        if (q) q->applyDirtyConstantSetPsB(hdr.start, hdr.count);
        break;
      }
      break;
    }
    default:
      return commitChunkFail("unknown-record", recordIndex, header.type);
    }

    if (failed(hr)) {
      return commitChunkFail("record-replay", recordIndex, header.type, hr);
    }
    recordIndex = recordView->nextIndex();
  }

  if (recordIndex != importedChunk.recordCount) {
    return commitChunkFail("truncated-records", recordIndex, importedChunk.recordCount);
  }
  // R-BACK-2.10: chunk fully validated + replayed. Bumping admit at the
  // single success point keeps reject + admit symmetric.
  dxmt9::perf::countChunkAdmit();
  // V1 boundary B2 — record bridge crossing latency at the same single
  // success exit so the percentile ring is populated only by complete
  // commits. enabled() is checked inside the helper.
  const auto bridgeCommitDelta =
      std::chrono::steady_clock::now() - bridgeCommitStart;
  dxmt9::perf::countBridgeCommitLatencyNs(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(bridgeCommitDelta)
          .count()));
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_draw_primitive_packet(D9CDevice* d,
                                                       const D9CDrawPrimitivePacket* packet) {
  if (!d || !packet) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  return applyDrawPrimitivePacket(d, *packet);
}

extern "C" int32_t dxmt9c_device_draw_primitive_chunk(D9CDevice* d,
                                                      const D9CDrawPrimitivePacket* packets,
                                                      uint32_t packetCount) {
  if (!d || (!packets && packetCount != 0)) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  for (uint32_t i = 0; i < packetCount; ++i) {
    const int32_t hr = applyDrawPrimitivePacket(d, packets[i]);
    if (failed(hr)) {
      return hr;
    }
  }
  return dxmt9::core::D3D_OK;
}
