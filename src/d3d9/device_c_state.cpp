#include "device_c_provider.hpp"

#include <cstdint>
#include <cstring>

using namespace dxmt9::d3d9::devicec;

namespace {

void clearBoundRenderTargetCache(D9CDevice* d) {
  d->renderTargets.fill(nullptr);
  d->renderTargetExplicit.fill(false);
}

template <typename Constants>
std::size_t clampedIntConstantCount(const Constants& constants, uint32_t start,
                                    uint32_t count) noexcept {
  const auto capacity = constants.int4.size();
  if (start >= capacity) {
    return 0u;
  }
  return std::min<std::size_t>(count, capacity - start);
}

template <typename Constants>
std::size_t clampedBoolConstantCount(const Constants& constants, uint32_t start,
                                     uint32_t count) noexcept {
  const auto capacity = constants.bools.size();
  if (start >= capacity) {
    return 0u;
  }
  return std::min<std::size_t>(count, capacity - start);
}

template <typename Constants>
bool intConstantsMatch(const Constants& constants, uint32_t start,
                       const int32_t* data, std::size_t count) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    if (std::memcmp(constants.int4[start + i].data(), data + i * 4u,
                    sizeof(int32_t) * 4u) != 0) {
      return false;
    }
  }
  return true;
}

template <typename Constants>
bool boolConstantsMatch(const Constants& constants, uint32_t start,
                        const uint32_t* data, std::size_t count) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    if (constants.bools[start + i] != (data[i] != 0)) {
      return false;
    }
  }
  return true;
}

template <typename Constants>
void writeIntConstants(Constants& constants, uint32_t start, const int32_t* data,
                       std::size_t count) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    std::memcpy(constants.int4[start + i].data(), data + i * 4u,
                sizeof(int32_t) * 4u);
  }
}

template <typename Constants>
void writeBoolConstants(Constants& constants, uint32_t start, const uint32_t* data,
                        std::size_t count) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    constants.bools[start + i] = data[i] != 0;
  }
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
  const int32_t hr = d->iface->Reset(ppFromC(*pp));
  if (hr == dxmt9::core::D3D_OK) {
    clearBoundRenderTargetCache(d);
  }
  return hr;
}

extern "C" int32_t dxmt9c_device_reset_ex(D9CDevice* d, const D9CPresentParams* pp,
                                          const D9CDisplayModeEx* dm) {
  if (!pp) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto params = ppFromC(*pp);
  if (dm) {
    auto dmex = dmExFromC(*dm);
    const int32_t hr = d->iface->ResetEx(params, &dmex);
    if (hr == dxmt9::core::D3D_OK) {
      clearBoundRenderTargetCache(d);
    }
    return hr;
  }
  const int32_t hr = d->iface->ResetEx(params, nullptr);
  if (hr == dxmt9::core::D3D_OK) {
    clearBoundRenderTargetCache(d);
  }
  return hr;
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
  if (!d || !m) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  dxmt9::core::Matrix4x4 identity{};
  identity.m[0] = 1.0f;
  identity.m[5] = 1.0f;
  identity.m[10] = 1.0f;
  identity.m[15] = 1.0f;
  const auto key = transformStateFromD3D(state);
  const auto matrix = d->dev().state().transforms.valueOr(key, identity);
  std::memcpy(m->m, matrix.m.data(), sizeof(m->m));
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
    d->stateBlockRenderStateValues[s] = v;
    return dxmt9::core::D3D_OK;
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

extern "C" void dxmt9c_device_set_gamma_ramp(D9CDevice* d, const uint16_t* ramp) {
  // POD copy from a 768-u16 buffer into the core::Device shadow. The PE
  // side memcpys from the D3DGAMMARAMP it received; we trust the wire
  // shape and reinterpret as the layout-identical core::GammaRamp.
  if (!d || !ramp) return;
  const auto* typed = reinterpret_cast<const dxmt9::core::GammaRamp*>(ramp);
  d->dev().setGammaRamp(typed);
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
  const auto& current = d->dev().state().vsConst;
  const auto effectiveCount = clampedIntConstantCount(current, s, cnt);
  if (effectiveCount == 0u || intConstantsMatch(current, s, data, effectiveCount)) {
    return dxmt9::core::D3D_OK;
  }
  auto& consts = d->dev().mutableVertexShaderConstantsState().vsConst;
  writeIntConstants(consts, s, data, effectiveCount);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_ps_const_i(D9CDevice* d, uint32_t s, const int32_t* data,
                                                uint32_t cnt) {
  const auto& current = d->dev().state().psConst;
  const auto effectiveCount = clampedIntConstantCount(current, s, cnt);
  if (effectiveCount == 0u || intConstantsMatch(current, s, data, effectiveCount)) {
    return dxmt9::core::D3D_OK;
  }
  auto& consts = d->dev().mutablePixelShaderConstantsState().psConst;
  writeIntConstants(consts, s, data, effectiveCount);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_vs_const_b(D9CDevice* d, uint32_t s,
                                                const uint32_t* data, uint32_t cnt) {
  const auto& current = d->dev().state().vsConst;
  const auto effectiveCount = clampedBoolConstantCount(current, s, cnt);
  if (effectiveCount == 0u || boolConstantsMatch(current, s, data, effectiveCount)) {
    return dxmt9::core::D3D_OK;
  }
  auto& consts = d->dev().mutableVertexShaderConstantsState().vsConst;
  writeBoolConstants(consts, s, data, effectiveCount);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_ps_const_b(D9CDevice* d, uint32_t s,
                                                const uint32_t* data, uint32_t cnt) {
  const auto& current = d->dev().state().psConst;
  const auto effectiveCount = clampedBoolConstantCount(current, s, cnt);
  if (effectiveCount == 0u || boolConstantsMatch(current, s, data, effectiveCount)) {
    return dxmt9::core::D3D_OK;
  }
  auto& consts = d->dev().mutablePixelShaderConstantsState().psConst;
  writeBoolConstants(consts, s, data, effectiveCount);
  return dxmt9::core::D3D_OK;
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
