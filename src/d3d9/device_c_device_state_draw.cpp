#include "device_c_provider.hpp"

using namespace dxmt9::d3d9::devicec;

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
  const size_t bytes = stride * (count + 2) * 3;
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
  const size_t vertexBytes = stride * (minV + numV);
  const uint32_t indexSize = idxFmt == 102 ? 4 : 2;
  const size_t indexBytes = indexSize * count * 3;
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
  auto* srcRect =
      sr ? new dxmt9::core::Rect{sr->left, sr->top, sr->right, sr->bottom} : nullptr;
  auto* dstRect =
      dr ? new dxmt9::core::Rect{dr->left, dr->top, dr->right, dr->bottom} : nullptr;
  const auto hr = d->iface->StretchRect(src->obj, srcRect, dst->obj, dstRect, filter != 1);
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
