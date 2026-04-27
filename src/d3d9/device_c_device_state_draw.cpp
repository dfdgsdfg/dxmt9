#include "device_c_provider.hpp"
#include "util/unixcall_marshal.hpp"
// Need the full dxmt9::Device type to call markChunkResources on the
// upperDevice shared_ptr that the chunk importer's Phase 4-B path
// hands the per-chunk retention list to.
#include "dxmt9/dxmt9_device.hpp"

using namespace dxmt9::d3d9::devicec;

namespace {

uint64_t wireHandleValue(const D9CWireHandle& handle) {
  return static_cast<uint64_t>(handle.lo) | (static_cast<uint64_t>(handle.hi) << 32);
}

template <typename T>
T* wireHandlePtr(const D9CWireHandle& handle) {
  const uint64_t value = wireHandleValue(handle);
  if (!value) {
    return nullptr;
  }
  if (value <= 0xffffffffull) {
    if (auto* decoded =
            dxmt9::util::marshal::wow64::decodeHandle<T*>(static_cast<uint32_t>(value))) {
      return decoded;
    }
  }
  return reinterpret_cast<T*>(static_cast<uintptr_t>(value));
}

bool failed(int32_t hr) {
  return hr < 0;
}

// True when a draw packet carries no state delta — every state-mutating
// field is empty/zero. Used by the chunk importer to recognize runs of
// consecutive draws that all reference the same baseline state, so they
// can be coalesced into a single drawPrimitiveRun call.
bool packetHasNoStateDelta(const D9CDrawPrimitivePacket& p) {
  return p.renderStateCount == 0 && p.textureMask == 0 &&
         p.streamSourceMask == 0 && p.fvfValid == 0 &&
         p.vsValid == 0 && p.psValid == 0 &&
         p.vdeclValid == 0 && p.rtMask == 0 && p.dsValid == 0 &&
         p.viewportValid == 0 && p.scissorValid == 0 &&
         p.tssCount == 0 && p.samplerStateCount == 0 &&
         p.materialValid == 0 && p.clipPlaneMask == 0 &&
         p.transformCount == 0 && p.lightSlotMask == 0 &&
         p.lightEnableValidMask == 0;
}

// Translate an in-chunk packet into a DrawParam for a draw run. `indexed`
// distinguishes DrawPrimitive (stream-only) from DrawIndexedPrimitive.
dxmt9::core::DrawParam makeRunParam(const D9CDrawPrimitivePacket& p) {
  dxmt9::core::DrawParam dp;
  dp.indexed = false;
  dp.primitiveType = ptFromD3D(p.primitiveType);
  dp.primitiveCount = p.primitiveCount;
  dp.startVertex = p.startVertex;
  return dp;
}

dxmt9::core::DrawParam makeRunParam(const D9CDrawIndexedPrimitivePacket& p) {
  dxmt9::core::DrawParam dp;
  dp.indexed = true;
  dp.primitiveType = ptFromD3D(p.state.primitiveType);
  dp.primitiveCount = p.primitiveCount;
  dp.baseVertexIndex = p.baseVertex;
  dp.startIndex = p.startIndex;
  return dp;
}

int32_t applyDrawPacketState(D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  if (packet.renderStateCount > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }

  for (uint32_t i = 0; i < packet.renderStateCount; ++i) {
    const auto& state = packet.renderStates[i];
    if (d->stateBlockRecording) {
      d->stateBlockRenderStates.insert(state.state);
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

std::uint32_t primitiveVertexCount(std::uint32_t primitiveType, std::uint32_t primitiveCount) {
  switch (primitiveType) {
  case 1: return primitiveCount;       // D3DPT_POINTLIST
  case 2: return primitiveCount * 2u;  // D3DPT_LINELIST
  case 3: return primitiveCount + 1u;  // D3DPT_LINESTRIP
  case 4: return primitiveCount * 3u;  // D3DPT_TRIANGLELIST
  case 5: return primitiveCount + 2u;  // D3DPT_TRIANGLESTRIP
  case 6: return primitiveCount + 2u;  // D3DPT_TRIANGLEFAN
  default: return 0;
  }
}

bool checkedMul(std::uint32_t a, std::uint32_t b, std::uint32_t& out) {
  const std::uint64_t value = static_cast<std::uint64_t>(a) * b;
  if (value > 0xffffffffull) {
    return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
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
                                  packet.primitiveCount, span);
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
                                         idxTypeFromD3D(packet.indexFormat));
}

}  // namespace

extern "C" void dxmt9c_device_addref(D9CDevice* d) {
  if (d) {
    d->refs.fetch_add(1);
  }
}

extern "C" uint32_t dxmt9c_device_release(D9CDevice* d) {
  if (!d) {
    return 0;
  }
  const uint32_t refs = d->refs.fetch_sub(1) - 1;
  if (refs == 0) {
    delete d;
  }
  return refs;
}

extern "C" int32_t dxmt9c_device_get_caps(D9CDevice* d, D9CCaps* out) {
  if (!out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  fillCCaps(d->iface->GetDeviceCaps(), out);
  dxmt9DebugLog(
      "device_get_caps vs=0x%x ps=0x%x maxTex=%ux%u maxRT=%u maxLights=%u maxStreams=%u "
      "maxAniso=%u intervals=0x%x devCaps=0x%x rasterCaps=0x%x texCaps=0x%x "
      "textureOpCaps=0x%x",
      out->vertexShaderVersion, out->pixelShaderVersion, out->maxTextureWidth,
      out->maxTextureHeight, out->numSimultaneousRTs, out->maxActiveLights, out->maxStreams,
      out->maxAnisotropy, out->presentationIntervals, out->devCaps, out->rasterCaps,
      out->textureCaps, out->textureBlendCaps);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_test_cooperative_level(D9CDevice* d) {
  return d->iface->TestCooperativeLevel();
}

extern "C" int32_t dxmt9c_device_check_device_state(D9CDevice* d, uint64_t w) {
  return d->iface->CheckDeviceState({w});
}

extern "C" int32_t dxmt9c_device_reset(D9CDevice* d, const D9CPresentParams* pp) {
  if (!pp) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  return d->iface->Reset(ppFromC(*pp));
}

extern "C" int32_t dxmt9c_device_reset_ex(D9CDevice* d, const D9CPresentParams* pp,
                                          const D9CDisplayModeEx* dm) {
  if (!pp) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto params = ppFromC(*pp);
  if (dm) {
    auto dmex = dmExFromC(*dm);
    return d->iface->ResetEx(params, &dmex);
  }
  return d->iface->ResetEx(params, nullptr);
}

extern "C" int32_t dxmt9c_device_present(D9CDevice* d, const D9CRect* src, const D9CRect* dst,
                                         uint64_t destWindow, const void* dirty, uint32_t flags) {
  dxmt9DebugLog("device_present begin destWindow=%llu flags=0x%x src=%d dst=%d",
                static_cast<unsigned long long>(destWindow), flags, src ? 1 : 0, dst ? 1 : 0);
  using Rect = dxmt9::core::Rect;
  Rect* srcRect = src ? new Rect{src->left, src->top, src->right, src->bottom} : nullptr;
  Rect* dstRect = dst ? new Rect{dst->left, dst->top, dst->right, dst->bottom} : nullptr;
  const auto hr = d->iface->PresentEx(srcRect, dstRect, {destWindow}, dirty, flags);
  delete srcRect;
  delete dstRect;
  dxmt9DebugLog("device_present hr=0x%08x", static_cast<unsigned>(hr));
  return hr;
}

extern "C" int32_t dxmt9c_device_begin_scene(D9CDevice* d) {
  return d->iface->BeginScene();
}

extern "C" int32_t dxmt9c_device_end_scene(D9CDevice* d) {
  return d->iface->EndScene();
}

extern "C" int32_t dxmt9c_device_clear(D9CDevice* d, uint32_t count, const D9CRect* rects,
                                       uint32_t flags, uint32_t colorARGB, float z,
                                       uint32_t stencil) {
  dxmt9::core::ClearDesc desc;
  desc.clearColor = (flags & 1) != 0;
  desc.clearDepth = (flags & 2) != 0;
  desc.clearStencil = (flags & 4) != 0;
  desc.color.a = ((colorARGB >> 24) & 0xff) / 255.0f;
  desc.color.r = ((colorARGB >> 16) & 0xff) / 255.0f;
  desc.color.g = ((colorARGB >> 8) & 0xff) / 255.0f;
  desc.color.b = (colorARGB & 0xff) / 255.0f;
  desc.depth = z;
  desc.stencil = stencil;
  for (uint32_t i = 0; i < count; ++i) {
    desc.rects.push_back({rects[i].left, rects[i].top, rects[i].right, rects[i].bottom});
  }
  return d->iface->Clear(desc);
}

extern "C" int32_t dxmt9c_device_set_viewport(D9CDevice* d, const D9CViewport* vp) {
  if (!vp) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  return d->iface->SetViewport({vp->x, vp->y, vp->width, vp->height, vp->minZ, vp->maxZ});
}

extern "C" void dxmt9c_device_get_viewport(D9CDevice* d, D9CViewport* vp) {
  if (!d || !vp) {
    return;
  }
  const auto value = d->iface->GetViewport();
  vp->x = value.x;
  vp->y = value.y;
  vp->width = value.width;
  vp->height = value.height;
  vp->minZ = value.minZ;
  vp->maxZ = value.maxZ;
}

extern "C" int32_t dxmt9c_device_set_scissor_rect(D9CDevice* d, const D9CRect* r) {
  if (!r) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  return d->iface->SetScissorRect({r->left, r->top, r->right, r->bottom});
}

extern "C" void dxmt9c_device_get_scissor_rect(D9CDevice* d, D9CRect* r) {
  if (!d || !r) {
    return;
  }
  const auto value = d->iface->GetScissorRect();
  r->left = value.left;
  r->top = value.top;
  r->right = value.right;
  r->bottom = value.bottom;
}

extern "C" int32_t dxmt9c_device_set_transform(D9CDevice* d, uint32_t state,
                                               const D9CMatrix* m) {
  if (!m) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  dxmt9::core::Matrix4x4 mat;
  std::memcpy(mat.m.data(), m->m, 16 * sizeof(float));
  return d->iface->SetTransform(transformStateFromD3D(state), mat);
}

extern "C" int32_t dxmt9c_device_get_transform(D9CDevice* d, uint32_t state, D9CMatrix* m) {
  (void)d;
  (void)state;
  (void)m;
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_material(D9CDevice* d, const D9CMaterial* m) {
  if (!m) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  dxmt9::core::Material mat;
  std::memcpy(&mat.diffuse, &m->diffuse, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&mat.ambient, &m->ambient, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&mat.specular, &m->specular, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&mat.emissive, &m->emissive, sizeof(dxmt9::core::ColorRGBA));
  mat.power = m->power;
  return d->iface->SetMaterial(mat);
}

extern "C" int32_t dxmt9c_device_get_material(D9CDevice* d, D9CMaterial* m) {
  (void)d;
  (void)m;
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_light(D9CDevice* d, uint32_t idx, const D9CLight* l) {
  if (!l) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  dxmt9::core::Light light;
  light.type = static_cast<dxmt9::core::LightType>(l->type);
  std::memcpy(&light.diffuse, &l->diffuse, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&light.specular, &l->specular, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&light.ambient, &l->ambient, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(light.position.data(), l->position, 3 * sizeof(float));
  std::memcpy(light.direction.data(), l->direction, 3 * sizeof(float));
  light.range = l->range;
  light.falloff = l->falloff;
  light.attenuation0 = l->attenuation0;
  light.attenuation1 = l->attenuation1;
  light.attenuation2 = l->attenuation2;
  light.theta = l->theta;
  light.phi = l->phi;
  return d->iface->SetLight(idx, light);
}

extern "C" int32_t dxmt9c_device_light_enable(D9CDevice* d, uint32_t i, uint32_t en) {
  return d->iface->LightEnable(i, en != 0);
}

extern "C" int32_t dxmt9c_device_set_render_state(D9CDevice* d, uint32_t s, uint32_t v) {
  if (d && d->stateBlockRecording) {
    d->stateBlockRenderStates.insert(s);
  }
  return d->iface->SetRenderState(s, v);
}

extern "C" uint32_t dxmt9c_device_get_render_state(D9CDevice* d, uint32_t s) {
  return d->iface->GetRenderState(s);
}

extern "C" int32_t dxmt9c_device_set_texture_stage_state(D9CDevice* d, uint32_t st,
                                                         uint32_t type, uint32_t val) {
  return d->iface->SetTextureStageState(st, type, val);
}

extern "C" uint32_t dxmt9c_device_get_texture_stage_state(D9CDevice* d, uint32_t st,
                                                          uint32_t type) {
  return d->iface->GetTextureStageState(st, type);
}

extern "C" int32_t dxmt9c_device_set_sampler_state(D9CDevice* d, uint32_t s, uint32_t type,
                                                   uint32_t val) {
  return d->iface->SetSamplerState(s, type, val);
}

extern "C" uint32_t dxmt9c_device_get_sampler_state(D9CDevice* d, uint32_t s, uint32_t type) {
  return d->iface->GetSamplerState(s, type);
}

extern "C" int32_t dxmt9c_device_set_clip_plane(D9CDevice* d, uint32_t idx,
                                                const float plane[4]) {
  dxmt9::core::ClipPlane cp{plane[0], plane[1], plane[2], plane[3]};
  return d->iface->SetClipPlane(idx, cp);
}

extern "C" int32_t dxmt9c_device_get_clip_plane(D9CDevice* d, uint32_t idx, float plane[4]) {
  (void)d;
  (void)idx;
  (void)plane;
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_fvf(D9CDevice* d, uint32_t fvf) {
  return d->iface->SetFVF(fvf);
}

extern "C" uint32_t dxmt9c_device_get_fvf(D9CDevice* d) {
  (void)d;
  return 0;
}

extern "C" int32_t dxmt9c_device_set_vertex_declaration(D9CDevice* d, D9CVertexDecl* vd) {
  if (!vd) {
    return d->iface->SetVertexDeclaration({});
  }
  return d->iface->SetVertexDeclaration(vd->elements);
}

extern "C" int32_t dxmt9c_device_set_stream_source(D9CDevice* d, uint32_t stream,
                                                   D9CBuffer* buf, uint32_t off,
                                                   uint32_t stride) {
  auto buffer = buf ? buf->obj : nullptr;
  return d->iface->SetStreamSource(stream, buffer, off, stride);
}

extern "C" int32_t dxmt9c_device_set_stream_source_freq(D9CDevice* d, uint32_t stream,
                                                        uint32_t freq) {
  (void)d;
  (void)stream;
  (void)freq;
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_indices(D9CDevice* d, D9CBuffer* buf) {
  auto buffer = buf ? buf->obj : nullptr;
  return d->iface->SetIndices(buffer);
}

extern "C" int32_t dxmt9c_device_set_texture(D9CDevice* d, uint32_t stage, D9CTexture* tex) {
  auto texture = tex ? tex->obj : nullptr;
  return d->iface->SetTexture(stage, texture);
}

extern "C" int32_t dxmt9c_device_set_vertex_shader(D9CDevice* d, D9CShader* s) {
  if (!s) {
    return d->iface->SetVertexShader({});
  }
  return d->iface->SetVertexShader(s->ref);
}

extern "C" int32_t dxmt9c_device_set_pixel_shader(D9CDevice* d, D9CShader* s) {
  if (!s) {
    return d->iface->SetPixelShader({});
  }
  return d->iface->SetPixelShader(s->ref);
}

extern "C" int32_t dxmt9c_device_set_vs_const_f(D9CDevice* d, uint32_t s, const float* data,
                                                uint32_t cnt) {
  return setShaderFloatConst(d, s, data, cnt, false);
}

extern "C" int32_t dxmt9c_device_get_vs_const_f(D9CDevice* d, uint32_t s, float* data,
                                                uint32_t cnt) {
  auto& consts = d->dev().state().vsConst;
  for (uint32_t i = 0; i < cnt && (s + i) < consts.float4.size(); ++i) {
    data[i * 4 + 0] = consts.float4[s + i][0];
    data[i * 4 + 1] = consts.float4[s + i][1];
    data[i * 4 + 2] = consts.float4[s + i][2];
    data[i * 4 + 3] = consts.float4[s + i][3];
  }
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_ps_const_f(D9CDevice* d, uint32_t s, const float* data,
                                                uint32_t cnt) {
  return setShaderFloatConst(d, s, data, cnt, true);
}

extern "C" int32_t dxmt9c_device_get_ps_const_f(D9CDevice* d, uint32_t s, float* data,
                                                uint32_t cnt) {
  auto& consts = d->dev().state().psConst;
  for (uint32_t i = 0; i < cnt && (s + i) < consts.float4.size(); ++i) {
    data[i * 4 + 0] = consts.float4[s + i][0];
    data[i * 4 + 1] = consts.float4[s + i][1];
    data[i * 4 + 2] = consts.float4[s + i][2];
    data[i * 4 + 3] = consts.float4[s + i][3];
  }
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_vs_const_i(D9CDevice* d, uint32_t s, const int32_t* data,
                                                uint32_t cnt) {
  auto& consts = d->dev().mutableState().vsConst;
  for (uint32_t i = 0; i < cnt && (s + i) < consts.int4.size(); ++i) {
    consts.int4[s + i][0] = data[i * 4 + 0];
    consts.int4[s + i][1] = data[i * 4 + 1];
    consts.int4[s + i][2] = data[i * 4 + 2];
    consts.int4[s + i][3] = data[i * 4 + 3];
  }
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_ps_const_i(D9CDevice* d, uint32_t s, const int32_t* data,
                                                uint32_t cnt) {
  auto& consts = d->dev().mutableState().psConst;
  for (uint32_t i = 0; i < cnt && (s + i) < consts.int4.size(); ++i) {
    consts.int4[s + i][0] = data[i * 4 + 0];
    consts.int4[s + i][1] = data[i * 4 + 1];
    consts.int4[s + i][2] = data[i * 4 + 2];
    consts.int4[s + i][3] = data[i * 4 + 3];
  }
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_vs_const_b(D9CDevice* d, uint32_t s,
                                                const uint32_t* data, uint32_t cnt) {
  auto& consts = d->dev().mutableState().vsConst;
  for (uint32_t i = 0; i < cnt && (s + i) < consts.bools.size(); ++i) {
    consts.bools[s + i] = data[i] != 0;
  }
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_ps_const_b(D9CDevice* d, uint32_t s,
                                                const uint32_t* data, uint32_t cnt) {
  auto& consts = d->dev().mutableState().psConst;
  for (uint32_t i = 0; i < cnt && (s + i) < consts.bools.size(); ++i) {
    consts.bools[s + i] = data[i] != 0;
  }
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_render_target(D9CDevice* d, uint32_t idx,
                                                   D9CSurface* surf) {
  return d->iface->SetRenderTarget(idx, surf ? surf->obj : nullptr);
}

extern "C" D9CSurface* dxmt9c_device_get_render_target(D9CDevice* d, uint32_t idx) {
  auto swapChain = d->iface->GetSwapChain(0);
  if (!swapChain) {
    return nullptr;
  }
  auto surface = idx == 0 ? swapChain->backBuffer() : nullptr;
  if (!surface) {
    return nullptr;
  }
  return new D9CSurface{surface};
}

extern "C" int32_t dxmt9c_device_set_depth_stencil(D9CDevice* d, D9CSurface* surf) {
  return d->iface->SetDepthStencilSurface(surf ? surf->obj : nullptr);
}

extern "C" D9CSurface* dxmt9c_device_get_depth_stencil(D9CDevice* d) {
  auto swapChain = d->iface->GetSwapChain(0);
  if (!swapChain) {
    return nullptr;
  }
  auto surface = swapChain->depthStencilSurface();
  if (!surface) {
    return nullptr;
  }
  return new D9CSurface{surface};
}

extern "C" int32_t dxmt9c_device_draw_primitive(D9CDevice* d, uint32_t type,
                                                uint32_t startVertex, uint32_t count) {
  return d->iface->DrawPrimitive(ptFromD3D(type), count, startVertex);
}

extern "C" int32_t dxmt9c_device_commit_chunk(D9CDevice* d, const D9CCommandChunk* chunk) {
  if (!d || !chunk || chunk->version != D9C_COMMAND_CHUNK_VERSION) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (chunk->recordBytes != 0 && !wireHandleValue(chunk->records)) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }

  const auto* records = wireHandlePtr<const dxmt9::core::u8>(chunk->records);
  if (!records && chunk->recordBytes != 0) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }

  // Phase 4: validate the chunk's resource retention list. PE recorder
  // populates handles[] with the deduped (kind, handle) set of every
  // resource bound while building the chunk; importer can mark them all
  // against the chunk seqId in one pass instead of N×per-record marks.
  // For now this is a bounds + sanity check + optional trace; the
  // pool-side bulk-mark integration is the Phase 4-B follow-up.
  const auto* handles = chunk->handleCount != 0
                            ? wireHandlePtr<const D9CChunkHandleEntry>(chunk->handles)
                            : nullptr;
  if (chunk->handleCount != 0 && !handles) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  for (std::uint32_t i = 0; i < chunk->handleCount; ++i) {
    if (handles[i].kind > D9C_CHUNK_HANDLE_KIND_VERTEX_DECL) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
  }
  // Bulk-mark every handle in chunk.handles[] against the queue's
  // current chunk seqId in one shot. Per-record markDrawResources
  // still fires inside submit{Draw,DrawRun} for now (additive — same
  // resources get marked twice with the same seqId, which is a no-op
  // for the mark*Use lastUsedSeqId compare). Once the importer is
  // confirmed to provide complete coverage, per-record marking can be
  // skipped for chunk-mode draws.
  //
  // The wire payload from PE carries the SERVER-SIDE D9C wrapper
  // pointer (D9CTexture* / D9CBuffer* / D9CSurface*) cast to uint64,
  // not the backend's core::Handle. Decode each pointer to its
  // underlying core::*::handle() value before handing the entry list
  // to CommandQueue::markChunkResources — otherwise pool.find{Texture,
  // Surface,Buffer} on a wrapper-pointer-as-handle would never match
  // and the bulk mark would silently be a no-op.
  if (chunk->handleCount > 0) {
    std::vector<dxmt9::core::ChunkHandleEntry> coreEntries;
    coreEntries.reserve(chunk->handleCount);
    for (std::uint32_t i = 0; i < chunk->handleCount; ++i) {
      const auto kind = static_cast<dxmt9::core::ChunkHandleKind>(handles[i].kind);
      const auto wirePtr = static_cast<uintptr_t>(handles[i].handle);
      if (wirePtr == 0) continue;
      dxmt9::core::Handle resolved{};
      switch (kind) {
      case dxmt9::core::ChunkHandleKind::Texture: {
        auto* wrapper = reinterpret_cast<D9CTexture*>(wirePtr);
        if (wrapper && wrapper->obj) resolved = wrapper->obj->handle();
        break;
      }
      case dxmt9::core::ChunkHandleKind::Surface: {
        auto* wrapper = reinterpret_cast<D9CSurface*>(wirePtr);
        if (wrapper && wrapper->obj) resolved = wrapper->obj->handle();
        break;
      }
      case dxmt9::core::ChunkHandleKind::Buffer: {
        auto* wrapper = reinterpret_cast<D9CBuffer*>(wirePtr);
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
      }
    }
  }

  // Phase 14: bulk markChunkResources has already pinned every resource
  // in this chunk against its seqId. Suppress the per-draw
  // markDrawResources walk inside submit{Draw,DrawBatch,DrawRun} for
  // the duration of this record-iter block — RAII guard ensures the
  // flag is cleared even if a record returns early. The flag is only
  // armed when chunk handles were actually present + at least one
  // bulk-mark fired; chunks with no handles fall through to legacy
  // per-draw marking (defensive, since the encoder still needs the
  // resources pinned somehow).
  struct ResetSkipDrawMarkGuard {
    std::shared_ptr<dxmt9::Device> upper;
    ~ResetSkipDrawMarkGuard() {
      if (upper) upper->setSkipDrawResourceMarking(false);
    }
  } resetGuard{};
  if (chunk->handleCount > 0) {
    if (auto upper = d->dev().upperDevice()) {
      upper->setSkipDrawResourceMarking(true);
      resetGuard.upper = std::move(upper);
    }
  }

  std::uint32_t offset = 0;
  std::uint32_t recordIndex = 0;
  while (offset < chunk->recordBytes) {
    if (chunk->recordBytes - offset < sizeof(D9CCommandRecordHeader)) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }

    D9CCommandRecordHeader header{};
    std::memcpy(&header, records + offset, sizeof(header));
    if (header.size < sizeof(D9CCommandRecordHeader) ||
        header.size > chunk->recordBytes - offset) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }

    const auto* record = records + offset;
    int32_t hr = dxmt9::core::D3DERR_INVALIDCALL;
    switch (header.type) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE: {
      if (header.size != sizeof(D9CCommandRecordDrawPrimitive)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      D9CCommandRecordDrawPrimitive decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      // Try to coalesce into a draw run: scan ahead for consecutive
      // DRAW_PRIMITIVE records that also have empty state deltas, build
      // one DrawParam vector, dispatch via drawPrimitiveRun (single
      // snapshotDrawDesc + single submitDrawRun on the queue side).
      // Falls through to per-record path if the run is just length-1.
      if (packetHasNoStateDelta(decoded.packet)) {
        std::vector<dxmt9::core::DrawParam> run;
        run.push_back(makeRunParam(decoded.packet));
        std::uint32_t scanOff = offset + header.size;
        std::uint32_t scanIdx = recordIndex + 1;
        while (scanOff < chunk->recordBytes) {
          if (chunk->recordBytes - scanOff < sizeof(D9CCommandRecordHeader)) break;
          D9CCommandRecordHeader nextHdr{};
          std::memcpy(&nextHdr, records + scanOff, sizeof(nextHdr));
          if (nextHdr.type != D9C_COMMAND_RECORD_DRAW_PRIMITIVE) break;
          if (nextHdr.size != sizeof(D9CCommandRecordDrawPrimitive)) break;
          if (nextHdr.size > chunk->recordBytes - scanOff) break;
          D9CCommandRecordDrawPrimitive nextDecoded{};
          std::memcpy(&nextDecoded, records + scanOff, sizeof(nextDecoded));
          if (!packetHasNoStateDelta(nextDecoded.packet)) break;
          run.push_back(makeRunParam(nextDecoded.packet));
          scanOff += nextHdr.size;
          ++scanIdx;
        }
        if (run.size() >= 2) {
          // applyDrawPacketState would be a no-op here (delta is empty),
          // so skip directly to drawPrimitiveRun. Bypasses N-1
          // applyDrawPrimitivePacket / snapshotDrawDesc calls.
          hr = d->dev().drawPrimitiveRun(std::span<const dxmt9::core::DrawParam>(run));
          if (failed(hr)) return hr;
          offset = scanOff;
          recordIndex = scanIdx;
          continue;
        }
      }
      hr = applyDrawPrimitivePacket(d, decoded.packet);
      break;
    }
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE: {
      if (header.size != sizeof(D9CCommandRecordDrawIndexedPrimitive)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      D9CCommandRecordDrawIndexedPrimitive decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      // Same coalescing as DRAW_PRIMITIVE — separate run per indexed
      // flag because DrawParam encodes that as a static field used by
      // the encoder to dispatch drawIndexed vs draw.
      if (packetHasNoStateDelta(decoded.packet.state)) {
        std::vector<dxmt9::core::DrawParam> run;
        run.push_back(makeRunParam(decoded.packet));
        std::uint32_t scanOff = offset + header.size;
        std::uint32_t scanIdx = recordIndex + 1;
        while (scanOff < chunk->recordBytes) {
          if (chunk->recordBytes - scanOff < sizeof(D9CCommandRecordHeader)) break;
          D9CCommandRecordHeader nextHdr{};
          std::memcpy(&nextHdr, records + scanOff, sizeof(nextHdr));
          if (nextHdr.type != D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) break;
          if (nextHdr.size != sizeof(D9CCommandRecordDrawIndexedPrimitive)) break;
          if (nextHdr.size > chunk->recordBytes - scanOff) break;
          D9CCommandRecordDrawIndexedPrimitive nextDecoded{};
          std::memcpy(&nextDecoded, records + scanOff, sizeof(nextDecoded));
          if (!packetHasNoStateDelta(nextDecoded.packet.state)) break;
          run.push_back(makeRunParam(nextDecoded.packet));
          scanOff += nextHdr.size;
          ++scanIdx;
        }
        if (run.size() >= 2) {
          hr = d->dev().drawPrimitiveRun(std::span<const dxmt9::core::DrawParam>(run));
          if (failed(hr)) return hr;
          offset = scanOff;
          recordIndex = scanIdx;
          continue;
        }
      }
      hr = applyDrawIndexedPrimitivePacket(d, decoded.packet);
      break;
    }
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP: {
      if (header.size < sizeof(D9CCommandRecordDrawPrimitiveUP)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      D9CCommandRecordDrawPrimitiveUP decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      hr = applyDrawPrimitiveUPPacket(d, decoded.packet, record, header.size);
      break;
    }
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP: {
      if (header.size < sizeof(D9CCommandRecordDrawIndexedPrimitiveUP)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      D9CCommandRecordDrawIndexedPrimitiveUP decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      hr = applyDrawIndexedPrimitiveUPPacket(d, decoded.packet, record, header.size);
      break;
    }
    case D9C_COMMAND_RECORD_CLEAR: {
      if (header.size < sizeof(D9CCommandRecordClear)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      D9CCommandRecordClear cl{};
      std::memcpy(&cl, record, sizeof(cl));
      const std::uint64_t expectedRectBytes =
          static_cast<std::uint64_t>(cl.rectCount) * sizeof(D9CRect);
      if (cl.rectOffset != sizeof(cl) ||
          header.size != sizeof(cl) + expectedRectBytes) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      const auto* rects = cl.rectCount != 0
                              ? reinterpret_cast<const D9CRect*>(record + cl.rectOffset)
                              : nullptr;
      hr = dxmt9c_device_clear(d, cl.rectCount, rects, cl.flags, cl.colorARGB,
                                cl.z, cl.stencil);
      break;
    }
    case D9C_COMMAND_RECORD_PRESENT: {
      if (header.size != sizeof(D9CCommandRecordPresent)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
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
      if (header.size != sizeof(D9CCommandRecordStretchRect)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      D9CCommandRecordStretchRect sr{};
      std::memcpy(&sr, record, sizeof(sr));
      auto* srcSurf = reinterpret_cast<D9CSurface*>(static_cast<uintptr_t>(sr.srcWire));
      auto* dstSurf = reinterpret_cast<D9CSurface*>(static_cast<uintptr_t>(sr.dstWire));
      const auto* srcR = sr.hasSrcRect ? &sr.srcRect : nullptr;
      const auto* dstR = sr.hasDstRect ? &sr.dstRect : nullptr;
      hr = dxmt9c_device_stretch_rect(d, srcSurf, srcR, dstSurf, dstR, sr.filter);
      break;
    }
    case D9C_COMMAND_RECORD_COLOR_FILL: {
      if (header.size != sizeof(D9CCommandRecordColorFill)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      D9CCommandRecordColorFill cf{};
      std::memcpy(&cf, record, sizeof(cf));
      auto* surf = reinterpret_cast<D9CSurface*>(static_cast<uintptr_t>(cf.surfaceWire));
      const auto* rect = cf.hasRect ? &cf.rect : nullptr;
      hr = dxmt9c_device_color_fill(d, surf, rect, cf.colorARGB);
      break;
    }
    case D9C_COMMAND_RECORD_UPDATE_TEXTURE: {
      if (header.size != sizeof(D9CCommandRecordUpdateTexture)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      D9CCommandRecordUpdateTexture ut{};
      std::memcpy(&ut, record, sizeof(ut));
      auto* srcTex = reinterpret_cast<D9CTexture*>(static_cast<uintptr_t>(ut.srcWire));
      auto* dstTex = reinterpret_cast<D9CTexture*>(static_cast<uintptr_t>(ut.dstWire));
      hr = dxmt9c_device_update_texture(d, srcTex, dstTex);
      break;
    }
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
    case D9C_COMMAND_RECORD_SET_PS_CONST_B: {
      if (header.size < sizeof(D9CCommandRecordSetConst)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      D9CCommandRecordSetConst hdr{};
      std::memcpy(&hdr, record, sizeof(hdr));
      // Per-record-type element size; payload count is hdr.count*kElemSize
      // (F/I are vec4-quad, B is single uint32).
      std::size_t elemSize = 0;
      switch (header.type) {
      case D9C_COMMAND_RECORD_SET_VS_CONST_F:
      case D9C_COMMAND_RECORD_SET_PS_CONST_F: elemSize = sizeof(float) * 4; break;
      case D9C_COMMAND_RECORD_SET_VS_CONST_I:
      case D9C_COMMAND_RECORD_SET_PS_CONST_I: elemSize = sizeof(int32_t) * 4; break;
      case D9C_COMMAND_RECORD_SET_VS_CONST_B:
      case D9C_COMMAND_RECORD_SET_PS_CONST_B: elemSize = sizeof(uint32_t); break;
      }
      const std::uint64_t expectedPayload =
          static_cast<std::uint64_t>(hdr.count) * elemSize;
      if (header.size != sizeof(hdr) + expectedPayload) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      const auto* payload = record + sizeof(hdr);
      switch (header.type) {
      case D9C_COMMAND_RECORD_SET_VS_CONST_F:
        hr = dxmt9c_device_set_vs_const_f(d, hdr.start,
                                          reinterpret_cast<const float*>(payload),
                                          hdr.count);
        break;
      case D9C_COMMAND_RECORD_SET_PS_CONST_F:
        hr = dxmt9c_device_set_ps_const_f(d, hdr.start,
                                          reinterpret_cast<const float*>(payload),
                                          hdr.count);
        break;
      case D9C_COMMAND_RECORD_SET_VS_CONST_I:
        hr = dxmt9c_device_set_vs_const_i(d, hdr.start,
                                          reinterpret_cast<const int32_t*>(payload),
                                          hdr.count);
        break;
      case D9C_COMMAND_RECORD_SET_PS_CONST_I:
        hr = dxmt9c_device_set_ps_const_i(d, hdr.start,
                                          reinterpret_cast<const int32_t*>(payload),
                                          hdr.count);
        break;
      case D9C_COMMAND_RECORD_SET_VS_CONST_B:
        hr = dxmt9c_device_set_vs_const_b(d, hdr.start,
                                          reinterpret_cast<const uint32_t*>(payload),
                                          hdr.count);
        break;
      case D9C_COMMAND_RECORD_SET_PS_CONST_B:
        hr = dxmt9c_device_set_ps_const_b(d, hdr.start,
                                          reinterpret_cast<const uint32_t*>(payload),
                                          hdr.count);
        break;
      }
      break;
    }
    default:
      return dxmt9::core::D3DERR_INVALIDCALL;
    }

    if (failed(hr)) {
      return hr;
    }
    offset += header.size;
    ++recordIndex;
  }

  return recordIndex == chunk->recordCount ? dxmt9::core::D3D_OK
                                           : dxmt9::core::D3DERR_INVALIDCALL;
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

extern "C" int32_t dxmt9c_device_draw_indexed_primitive(D9CDevice* d, uint32_t type,
                                                        int32_t baseVertex, uint32_t minV,
                                                        uint32_t numV, uint32_t startIdx,
                                                        uint32_t count) {
  (void)minV;
  (void)numV;
  const auto& state = d->dev().state();
  return d->iface->DrawIndexedPrimitive(ptFromD3D(type), count, 0, baseVertex, startIdx,
                                        state.indexType);
}

extern "C" int32_t dxmt9c_device_draw_primitive_up(D9CDevice* d, uint32_t type,
                                                   uint32_t count, const void* data,
                                                   uint32_t stride) {
  std::uint32_t bytes = 0;
  if (!checkedMul(primitiveVertexCount(type, count), stride, bytes) ||
      (bytes != 0 && !data)) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto span = std::span<const dxmt9::core::u8>(
      reinterpret_cast<const dxmt9::core::u8*>(data), bytes);
  return d->iface->DrawPrimitiveUP(ptFromD3D(type), count, span);
}

extern "C" int32_t dxmt9c_device_draw_indexed_primitive_up(D9CDevice* d, uint32_t type,
                                                           uint32_t minV, uint32_t numV,
                                                           uint32_t count, const void* idxData,
                                                           uint32_t idxFmt,
                                                           const void* vtxData,
                                                           uint32_t stride) {
  if (minV > 0xffffffffu - numV) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  std::uint32_t vertexBytes = 0;
  const uint32_t indexSize = idxFmt == 102 ? 4 : 2;
  std::uint32_t indexBytes = 0;
  if (!checkedMul(minV + numV, stride, vertexBytes) ||
      !checkedMul(primitiveVertexCount(type, count), indexSize, indexBytes) ||
      (indexBytes != 0 && !idxData) ||
      (vertexBytes != 0 && !vtxData)) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto vertexSpan = std::span<const dxmt9::core::u8>(
      reinterpret_cast<const dxmt9::core::u8*>(vtxData), vertexBytes);
  auto indexSpan = std::span<const dxmt9::core::u8>(
      reinterpret_cast<const dxmt9::core::u8*>(idxData), indexBytes);
  return d->iface->DrawIndexedPrimitiveUP(ptFromD3D(type), count, vertexSpan, indexSpan,
                                          idxTypeFromD3D(idxFmt));
}

extern "C" int32_t dxmt9c_device_update_surface(D9CDevice* d, D9CSurface* src,
                                                const D9CRect*, D9CSurface* dst,
                                                const D9CRect*) {
  if (!src || !dst) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  return d->iface->UpdateSurface(src->obj, dst->obj);
}

extern "C" int32_t dxmt9c_device_update_texture(D9CDevice* d, D9CTexture* src,
                                                D9CTexture* dst) {
  if (!src || !dst) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  return d->iface->UpdateTexture(src->obj, dst->obj);
}

extern "C" int32_t dxmt9c_device_stretch_rect(D9CDevice* d, D9CSurface* src, const D9CRect* sr,
                                              D9CSurface* dst, const D9CRect* dr,
                                              uint32_t filter) {
  if (!src || !dst) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  constexpr uint32_t kD3DTexfNone = 0;
  constexpr uint32_t kD3DTexfPoint = 1;
  constexpr uint32_t kD3DTexfLinear = 2;
  if (filter != kD3DTexfNone && filter != kD3DTexfPoint && filter != kD3DTexfLinear) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto* srcRect =
      sr ? new dxmt9::core::Rect{sr->left, sr->top, sr->right, sr->bottom} : nullptr;
  auto* dstRect =
      dr ? new dxmt9::core::Rect{dr->left, dr->top, dr->right, dr->bottom} : nullptr;
  const auto hr = d->iface->StretchRect(src->obj, srcRect, dst->obj, dstRect,
                                        filter == kD3DTexfLinear);
  delete srcRect;
  delete dstRect;
  return hr;
}

extern "C" int32_t dxmt9c_device_color_fill(D9CDevice* d, D9CSurface* surf, const D9CRect* r,
                                            uint32_t colorARGB) {
  if (!surf) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto* rect = r ? new dxmt9::core::Rect{r->left, r->top, r->right, r->bottom} : nullptr;
  dxmt9::core::ColorRGBA rgba{
      ((colorARGB >> 16) & 0xff) / 255.0f,
      ((colorARGB >> 8) & 0xff) / 255.0f,
      (colorARGB & 0xff) / 255.0f,
      ((colorARGB >> 24) & 0xff) / 255.0f,
  };
  const auto hr = d->iface->FillSurface(surf->obj, rect, rgba);
  delete rect;
  return hr;
}

extern "C" int32_t dxmt9c_device_get_render_target_data(D9CDevice* d, D9CSurface* rt,
                                                        D9CSurface* dst) {
  if (!rt || !dst) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  return d->iface->GetRenderTargetData(rt->obj, dst->obj);
}

extern "C" int32_t dxmt9c_device_set_maximum_frame_latency(D9CDevice* d, uint32_t l) {
  return d->iface->SetMaximumFrameLatency(l);
}

extern "C" uint32_t dxmt9c_device_get_maximum_frame_latency(D9CDevice* d) {
  return d->iface->GetMaximumFrameLatency();
}

extern "C" int32_t dxmt9c_device_wait_for_vblank(D9CDevice* d, uint32_t idx) {
  return d->iface->WaitForVBlank(idx);
}

extern "C" int32_t dxmt9c_device_check_device_multisample(D9CDevice* d, uint32_t fmt,
                                                          uint32_t msType,
                                                          uint32_t windowed) {
  if (!isSupportedD3DMultisample(msType)) {
    (void)windowed;
    return dxmt9::core::D3DERR_NOTAVAILABLE;
  }
  return d->iface->CheckDeviceMultiSampleType(fmtFromD3D(fmt), msTypeFromD3D(msType));
}
