#include "device_c_provider.hpp"
#include "util/unixcall_marshal.hpp"

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
      hr = applyDrawPrimitivePacket(d, decoded.packet);
      break;
    }
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE: {
      if (header.size != sizeof(D9CCommandRecordDrawIndexedPrimitive)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      D9CCommandRecordDrawIndexedPrimitive decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
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
