#include "dxmt9/device_c.h"
#include "dxmt9/com.hpp"
#include "dxmt9/core.hpp"
#include <atomic>
#include <memory>
#include <vector>
#include <cstring>

/* ── format conversion ───────────────────────────────────────────────────── */

static dxmt9::core::Format fmtFromD3D(uint32_t d3d) {
  using F = dxmt9::core::Format;
  switch (d3d) {
    case 21:   return F::A8R8G8B8;
    case 22:   return F::X8R8G8B8;
    case 32:   return F::A8B8G8R8;
    case 33:   return F::X8B8G8R8;
    case 23:   return F::R5G6B5;
    case 25:   return F::A1R5G5B5;
    case 24:   return F::X1R5G5B5;
    case 26:   return F::A4R4G4B4;
    case 28:   return F::A8;
    case 20:   return F::R8G8B8;
    case 113:  return F::A16B16G16R16F;
    case 116:  return F::A32B32G32R32F;
    case 112:  return F::G16R16F;
    case 111:  return F::R16F;
    case 115:  return F::G32R32F;
    case 114:  return F::R32F;
    case 36:   return F::A16B16G16R16;
    case 34:   return F::G16R16;
    case 35:   return F::A2R10G10B10;
    case 31:   return F::A2B10G10R10;
    case 50:   return F::L8;
    case 81:   return F::L16;
    case 51:   return F::A8L8;
    case 60:   return F::V8U8;
    case 63:   return F::Q8W8V8U8;
    case 64:   return F::V16U16;
    case 117:  return F::CxV8U8;
    case 827611204: return F::DXT1;  /* MAKEFOURCC('D','X','T','1') */
    case 844388420: return F::DXT2;  /* MAKEFOURCC('D','X','T','2') */
    case 861165636: return F::DXT3;  /* MAKEFOURCC('D','X','T','3') */
    case 877942852: return F::DXT4;  /* MAKEFOURCC('D','X','T','4') */
    case 894720068: return F::DXT5;  /* MAKEFOURCC('D','X','T','5') */
    case 826889281: return F::ATI1;  /* MAKEFOURCC('A','T','I','1') */
    case 843666497: return F::ATI2;  /* MAKEFOURCC('A','T','I','2') */
    case 75:   return F::D24S8;
    case 77:   return F::D24X8;
    case 80:   return F::D16;
    case 71:   return F::D32;
    case 82:   return F::D32F_LOCKABLE;
    case 70:   return F::D16_LOCKABLE;
    case 73:   return F::D15S1;
    case 79:   return F::D24X4S4;
    case 83:   return F::D24FS8;
    case 85:   return F::S8_LOCKABLE;
    case 101:  return F::INDEX16;
    case 102:  return F::INDEX32;
    default:   return F::Unknown;
  }
}

static dxmt9::core::MultiSampleType msTypeFromD3D(uint32_t d3d) {
  using M = dxmt9::core::MultiSampleType;
  switch (d3d) {
    case 2:  return M::Two;
    case 4:  return M::Four;
    case 8:  return M::Eight;
    default: return M::None;
  }
}

static dxmt9::core::Pool poolFromD3D(uint32_t d3d) {
  using P = dxmt9::core::Pool;
  switch (d3d) {
    case 1:  return P::Managed;
    case 2:  return P::SystemMem;
    case 3:  return P::Scratch;
    default: return P::Default;
  }
}

static dxmt9::core::PrimitiveType ptFromD3D(uint32_t d3d) {
  using P = dxmt9::core::PrimitiveType;
  /* D3D is 1-indexed; dxmt9 is 0-indexed */
  if (d3d >= 1 && d3d <= 6)
    return static_cast<P>(d3d - 1);
  return P::TriangleList;
}

static dxmt9::core::IndexType idxTypeFromD3D(uint32_t d3d) {
  return (d3d == 102 /* D3DFMT_INDEX32 */)
    ? dxmt9::core::IndexType::UInt32
    : dxmt9::core::IndexType::UInt16;
}

static dxmt9::core::PresentParameters ppFromC(const D9CPresentParams& c) {
  dxmt9::core::PresentParameters p;
  p.backBufferWidth  = c.backBufferWidth;
  p.backBufferHeight = c.backBufferHeight;
  p.backBufferFormat = fmtFromD3D(c.backBufferFormat);
  p.backBufferCount  = c.backBufferCount;
  p.windowed         = (c.windowed != 0);
  p.enableAutoDepthStencil   = (c.enableAutoDepthStencil != 0);
  p.autoDepthStencilFormat   = fmtFromD3D(c.autoDepthStencilFormat);
  p.multiSampleType  = msTypeFromD3D(c.multiSampleType);
  p.deviceWindow     = dxmt9::core::Handle{c.deviceWindow};
  /* presentationInterval: 0=Immediate,1=Default,2+ */
  if (c.presentationInterval == 0)
    p.presentationInterval = dxmt9::core::PresentInterval::Immediate;
  else if (c.presentationInterval >= 2)
    p.presentationInterval = dxmt9::core::PresentInterval::Two;
  else
    p.presentationInterval = dxmt9::core::PresentInterval::Default;
  /* swapEffect: 2=FLIP → discard=false */
  p.discardSwapEffect = (c.swapEffect != 2);
  return p;
}

static dxmt9::core::DisplayModeEx dmExFromC(const D9CDisplayModeEx& c) {
  dxmt9::core::DisplayModeEx m;
  m.width       = c.width;
  m.height      = c.height;
  m.refreshRate = c.refreshRate;
  m.format      = fmtFromD3D(c.format);
  m.scanLineOrdering = static_cast<dxmt9::core::DisplayScanLineOrdering>(c.scanLineOrdering);
  return m;
}

/* ── D9CCaps from DeviceCaps ─────────────────────────────────────────────── */

static void fillCCaps(const dxmt9::core::DeviceCaps& src, D9CCaps* out) {
  std::memset(out, 0, sizeof(*out));
  out->deviceType              = static_cast<uint32_t>(src.deviceType);
  out->caps                    = src.caps;
  out->caps2                   = src.caps2;
  out->caps3                   = src.caps3;
  out->presentationIntervals   = src.presentationIntervals;
  out->rasterCaps              = src.rasterCaps;
  out->zCmpCaps                = src.zCmpCaps;
  out->srcBlendCaps            = src.srcBlendCaps;
  out->destBlendCaps           = src.destBlendCaps;
  out->alphaBlendCaps          = src.alphaBlendCaps;
  out->shadeCaps               = src.shadeCaps;
  out->textureCaps             = src.textureCaps;
  out->maxAnisotropy           = src.maxAnisotropy;
  out->maxUserClipPlanes       = src.maxUserClipPlanes;
  out->maxVertexW              = src.maxVertexW;
  out->guardBandLeft           = src.guardBandLeft;
  out->guardBandRight          = src.guardBandRight;
  out->guardBandTop            = src.guardBandTop;
  out->guardBandBottom         = src.guardBandBottom;
  out->extentsAdjust           = src.extentsAdjust;
  out->stencilCaps             = src.stencilCaps;
  out->vertexShaderVersion     = src.vertexShaderVersion;
  out->pixelShaderVersion      = src.pixelShaderVersion;
  out->maxVertexShaderConst    = src.maxVertexShaderConst;
  out->pixelShader1xMaxValue   = src.pixelShader1xMaxValue;
  out->ps20DynamicFlowControlDepth = src.ps20DynamicFlowControlDepth;
  out->ps20NumTemps            = src.ps20NumTemps;
  out->ps20StaticFlowControlDepth  = src.ps20StaticFlowControlDepth;
  out->ps20NumInstructionSlots = src.ps20NumInstructionSlots;
  out->vs20DynamicFlowControlDepth = src.vs20DynamicFlowControlDepth;
  out->vs20NumTemps            = src.vs20NumTemps;
  out->vs20StaticFlowControlDepth  = src.vs20StaticFlowControlDepth;
  out->maxTextureWidth         = src.maxTextureWidth;
  out->maxTextureHeight        = src.maxTextureHeight;
  out->maxVolumeExtent         = src.maxVolumeExtent;
  out->maxTextureRepeat        = src.maxTextureRepeat;
  out->maxAnisotropy           = src.maxAnisotropy;
  out->maxPointSize            = src.maxPointSize;
  out->maxPrimitiveCount       = src.maxPrimitiveCount;
  out->maxVertexIndex          = src.maxVertexIndex;
  out->maxStreams               = src.maxStreams;
  out->maxStreamStride         = src.maxStreamStride;
  out->numSimultaneousRTs      = src.numSimultaneousRTs;
  out->maxVertexBlendMatrices  = src.maxVertexBlendMatrices;
  out->maxVertexBlendMatrixIndex = src.maxVertexBlendMatrixIndex;
  out->fvfCaps                 = src.fvfCaps;
  out->textureAddressCaps      = src.textureAddressCaps;
  out->volumeTextureAddressCaps= src.volumeTextureAddressCaps;
  out->maxTextureAspectRatio   = src.maxTextureAspectRatio;
  out->vs20Caps                = src.vs20Caps;
  out->ps20Caps                = src.ps20Caps;
  out->maxSimultaneousTextures = src.maxSimultaneousTextures;
  out->maxActiveLights         = src.maxActiveLights;
  out->vertexProcessingCaps    = src.vertexProcessingCaps;
  out->maxTextureBlendStages   = 8;   /* fixed D3D9 max */
  out->devCaps                 = src.devCaps;
  out->devCaps2                = src.devCaps2;
  out->masterAdapterOrdinal    = src.masterAdapterOrdinal;
  out->adapterOrdinalInGroup   = src.adapterOrdinalInGroup;
  out->numberOfAdaptersInGroup = src.numberOfAdaptersInGroup;
}

/* ── ref-counted C wrappers ──────────────────────────────────────────────── */

template<typename T>
struct RefWrap {
  T                    obj;
  std::atomic<uint32_t> refs{1};

  template<typename... Args>
  explicit RefWrap(Args&&... args) : obj(std::forward<Args>(args)...) {}

  void addRef()           { refs.fetch_add(1); }
  uint32_t release()      { return refs.fetch_sub(1) - 1; }
};

/* factory wraps dxmt9::com::IDirect3D9Ex */
struct D9CFactory {
  dxmt9::com::IDirect3D9Ex*  iface;
  std::atomic<uint32_t>      refs{1};

  explicit D9CFactory(dxmt9::com::IDirect3D9Ex* i) : iface(i) {}
  ~D9CFactory() { if (iface) iface->Release(); }
};

/* device wraps the underlying core::Device via dxmt9::com::IDirect3DDevice9Ex */
struct D9CDevice {
  dxmt9::com::IDirect3DDevice9Ex* iface;
  std::atomic<uint32_t>           refs{1};

  explicit D9CDevice(dxmt9::com::IDirect3DDevice9Ex* i) : iface(i) {}
  ~D9CDevice() { if (iface) iface->Release(); }

  dxmt9::core::Device& dev() { return iface->coreDevice(); }
};

struct D9CSwapChain {
  dxmt9::com::IDirect3DSwapChain9* iface;
  std::atomic<uint32_t>            refs{1};

  explicit D9CSwapChain(dxmt9::com::IDirect3DSwapChain9* i) : iface(i) {}
  ~D9CSwapChain() { if (iface) iface->Release(); }
};

struct D9CTexture {
  std::shared_ptr<dxmt9::core::Texture> obj;
  D9CDevice*                            device; /* back-pointer, non-owning */
  std::atomic<uint32_t>                 refs{1};
};

struct D9CBuffer {
  std::shared_ptr<dxmt9::core::Buffer> obj;
  std::atomic<uint32_t>                refs{1};
};

struct D9CSurface {
  std::shared_ptr<dxmt9::core::Surface> obj;
  D9CTexture*                           ownerTex{nullptr}; /* if texture level */
  std::atomic<uint32_t>                 refs{1};
};

struct D9CShader {
  dxmt9::core::ShaderRef       ref;
  std::vector<uint32_t>        bytecodeWords;
  std::atomic<uint32_t>        refs{1};
};

struct D9CVertexDecl {
  std::vector<dxmt9::core::VertexElement> elements;
  std::vector<D9CVertexElement>           raw;
  std::atomic<uint32_t>                   refs{1};
};

struct D9CQuery {
  std::shared_ptr<dxmt9::core::Query> obj;
  D9CDevice*                          device; /* non-owning */
  std::atomic<uint32_t>               refs{1};
};

struct D9CStateBlock {
  std::shared_ptr<dxmt9::core::StateBlock> obj;
  D9CDevice*                               device; /* non-owning */
  std::atomic<uint32_t>                    refs{1};
};

/* ── factory ─────────────────────────────────────────────────────────────── */

extern "C" D9CFactory* dxmt9c_factory_create(void) {
  using namespace dxmt9;
  auto* ex = com::Direct3DCreate9Ex(com::D3D_SDK_VERSION,
                                     core::makeBackendDevice());
  if (!ex) return nullptr;
  return new D9CFactory(ex);
}

extern "C" void dxmt9c_factory_addref(D9CFactory* f) {
  if (f) f->refs.fetch_add(1);
}

extern "C" uint32_t dxmt9c_factory_release(D9CFactory* f) {
  if (!f) return 0;
  uint32_t r = f->refs.fetch_sub(1) - 1;
  if (r == 0) delete f;
  return r;
}

extern "C" uint32_t dxmt9c_factory_adapter_count(D9CFactory* f) {
  return static_cast<uint32_t>(f->iface->GetAdapterCount());
}

extern "C" int32_t dxmt9c_factory_get_adapter_identifier(D9CFactory* f,
                                                           uint32_t adapter,
                                                           D9CAdapterIdentifier* out) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  auto id = f->iface->GetAdapterIdentifier(adapter);
  std::memset(out, 0, sizeof(*out));
  std::strncpy(out->driver, id.driver.c_str(), sizeof(out->driver)-1);
  std::strncpy(out->description, id.description.c_str(), sizeof(out->description)-1);
  std::strncpy(out->deviceName, id.deviceName.c_str(), sizeof(out->deviceName)-1);
  out->driverVersion = id.driverVersion;
  out->vendorId = id.vendorId;
  out->deviceId = id.deviceId;
  out->subSysId = id.subSysId;
  out->revision = id.revision;
  return dxmt9::core::D3D_OK;
}

extern "C" uint32_t dxmt9c_factory_get_adapter_mode_count(D9CFactory* f,
                                                            uint32_t adapter,
                                                            uint32_t d3dFmt) {
  auto modes = f->iface->EnumAdapterModes(adapter, fmtFromD3D(d3dFmt));
  return static_cast<uint32_t>(modes.size());
}

extern "C" int32_t dxmt9c_factory_enum_adapter_modes(D9CFactory* f, uint32_t adapter,
                                                       uint32_t d3dFmt, uint32_t modeIdx,
                                                       uint32_t* outW, uint32_t* outH,
                                                       uint32_t* outRefresh, uint32_t* outFmt) {
  auto modes = f->iface->EnumAdapterModes(adapter, fmtFromD3D(d3dFmt));
  if (modeIdx >= modes.size()) return dxmt9::core::D3DERR_INVALIDCALL;
  auto& m = modes[modeIdx];
  if (outW)       *outW       = m.width;
  if (outH)       *outH       = m.height;
  if (outRefresh) *outRefresh = m.refreshRate;
  if (outFmt)     *outFmt     = d3dFmt;
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_factory_get_adapter_display_mode(D9CFactory* f, uint32_t adapter,
                                                             uint32_t* outW, uint32_t* outH,
                                                             uint32_t* outRefresh, uint32_t* outFmt) {
  auto m = f->iface->GetAdapterDisplayMode(adapter);
  if (outW)       *outW       = m.width;
  if (outH)       *outH       = m.height;
  if (outRefresh) *outRefresh = m.refreshRate;
  if (outFmt)     *outFmt     = 21; /* A8R8G8B8 as D3DFORMAT */
  (void)m.format;
  return dxmt9::core::D3D_OK;
}

extern "C" uint64_t dxmt9c_factory_get_adapter_monitor(D9CFactory* f, uint32_t adapter) {
  return static_cast<uint64_t>(f->iface->GetAdapterMonitor(adapter));
}

extern "C" int32_t dxmt9c_factory_check_device_type(D9CFactory* f, uint32_t adapter,
                                                      uint32_t devType, uint32_t adapterFmt,
                                                      uint32_t backFmt, uint32_t windowed) {
  return f->iface->CheckDeviceType(adapter,
                                   static_cast<dxmt9::core::DeviceType>(devType),
                                   fmtFromD3D(adapterFmt), fmtFromD3D(backFmt),
                                   windowed != 0);
}

extern "C" int32_t dxmt9c_factory_check_device_format(D9CFactory* f, uint32_t adapter,
                                                        uint32_t d3dFmt, uint32_t usage) {
  return f->iface->CheckDeviceFormat(adapter, fmtFromD3D(d3dFmt), usage);
}

extern "C" int32_t dxmt9c_factory_check_device_multisample(D9CFactory* f, uint32_t adapter,
                                                             uint32_t d3dFmt, uint32_t msType,
                                                             uint32_t windowed) {
  return f->iface->CheckDeviceMultiSampleType(adapter, fmtFromD3D(d3dFmt),
                                               msTypeFromD3D(msType));
  (void)windowed;
}

extern "C" int32_t dxmt9c_factory_get_caps(D9CFactory* f, uint32_t adapter, D9CCaps* out) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  fillCCaps(f->iface->GetDeviceCaps(adapter), out);
  out->adapterOrdinal = adapter;
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_factory_get_adapter_luid(D9CFactory* f, uint32_t adapter,
                                                     uint32_t* lowPart, int32_t* highPart) {
  dxmt9::core::Luid luid{};
  if (!f->iface->GetAdapterLUID(adapter, &luid)) return dxmt9::core::D3DERR_INVALIDCALL;
  if (lowPart)  *lowPart  = luid.lowPart;
  if (highPart) *highPart = luid.highPart;
  return dxmt9::core::D3D_OK;
}

extern "C" D9CDevice* dxmt9c_factory_create_device(D9CFactory* f, uint32_t adapter,
                                                     const D9CPresentParams* pp,
                                                     uint32_t behaviorFlags,
                                                     const D9CDisplayModeEx* fullscreen) {
  if (!pp) return nullptr;
  auto params = ppFromC(*pp);
  dxmt9::com::IDirect3DDevice9Ex* dev = nullptr;

  if (fullscreen) {
    auto dmex = dmExFromC(*fullscreen);
    dev = f->iface->CreateDeviceEx(adapter, params, &dmex, behaviorFlags);
  } else {
    dev = f->iface->CreateDeviceEx(adapter, params, nullptr, behaviorFlags);
  }
  if (!dev) return nullptr;
  return new D9CDevice(dev);
}

/* ── device ──────────────────────────────────────────────────────────────── */

extern "C" void dxmt9c_device_addref(D9CDevice* d) {
  if (d) d->refs.fetch_add(1);
}

extern "C" uint32_t dxmt9c_device_release(D9CDevice* d) {
  if (!d) return 0;
  uint32_t r = d->refs.fetch_sub(1) - 1;
  if (r == 0) delete d;
  return r;
}

extern "C" int32_t dxmt9c_device_get_caps(D9CDevice* d, D9CCaps* out) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  fillCCaps(d->iface->GetDeviceCaps(), out);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_test_cooperative_level(D9CDevice* d) {
  return d->iface->TestCooperativeLevel();
}
extern "C" int32_t dxmt9c_device_check_device_state(D9CDevice* d, uint64_t w) {
  return d->iface->CheckDeviceState({w});
}
extern "C" int32_t dxmt9c_device_reset(D9CDevice* d, const D9CPresentParams* pp) {
  if (!pp) return dxmt9::core::D3DERR_INVALIDCALL;
  return d->iface->Reset(ppFromC(*pp));
}
extern "C" int32_t dxmt9c_device_reset_ex(D9CDevice* d, const D9CPresentParams* pp,
                                           const D9CDisplayModeEx* dm) {
  if (!pp) return dxmt9::core::D3DERR_INVALIDCALL;
  auto params = ppFromC(*pp);
  if (dm) {
    auto dmex = dmExFromC(*dm);
    return d->iface->ResetEx(params, &dmex);
  }
  return d->iface->ResetEx(params, nullptr);
}
extern "C" int32_t dxmt9c_device_present(D9CDevice* d,
                                          const D9CRect* src, const D9CRect* dst,
                                          uint64_t destWindow, const void* dirty,
                                          uint32_t flags) {
  using R = dxmt9::core::Rect;
  R* ps = src ? new R{src->left,src->top,src->right,src->bottom} : nullptr;
  R* pd = dst ? new R{dst->left,dst->top,dst->right,dst->bottom} : nullptr;
  auto hr = d->iface->PresentEx(ps, pd, {destWindow}, dirty, flags);
  delete ps; delete pd;
  return hr;
}
extern "C" int32_t dxmt9c_device_begin_scene(D9CDevice* d) {
  return d->iface->BeginScene();
}
extern "C" int32_t dxmt9c_device_end_scene(D9CDevice* d) {
  return d->iface->EndScene();
}

extern "C" int32_t dxmt9c_device_clear(D9CDevice* d, uint32_t count,
                                        const D9CRect* rects, uint32_t flags,
                                        uint32_t colorARGB, float z, uint32_t stencil) {
  dxmt9::core::ClearDesc desc;
  desc.clearColor   = (flags & 1) != 0;   /* D3DCLEAR_TARGET = 1 */
  desc.clearDepth   = (flags & 2) != 0;   /* D3DCLEAR_ZBUFFER = 2 */
  desc.clearStencil = (flags & 4) != 0;   /* D3DCLEAR_STENCIL = 4 */
  /* ARGB → RGBA float */
  desc.color.a = ((colorARGB >> 24) & 0xff) / 255.0f;
  desc.color.r = ((colorARGB >> 16) & 0xff) / 255.0f;
  desc.color.g = ((colorARGB >>  8) & 0xff) / 255.0f;
  desc.color.b = ((colorARGB      ) & 0xff) / 255.0f;
  desc.depth   = z;
  desc.stencil = stencil;
  for (uint32_t i = 0; i < count; ++i) {
    desc.rects.push_back({rects[i].left, rects[i].top,
                          rects[i].right, rects[i].bottom});
  }
  return d->iface->Clear(desc);
}

extern "C" int32_t dxmt9c_device_set_viewport(D9CDevice* d, const D9CViewport* vp) {
  if (!vp) return dxmt9::core::D3DERR_INVALIDCALL;
  return d->iface->SetViewport({vp->x,vp->y,vp->width,vp->height,vp->minZ,vp->maxZ});
}
extern "C" void dxmt9c_device_get_viewport(D9CDevice* d, D9CViewport* vp) {
  /* no getter in internal interface; mirror from device state */
  (void)d; (void)vp;
}
extern "C" int32_t dxmt9c_device_set_scissor_rect(D9CDevice* d, const D9CRect* r) {
  if (!r) return dxmt9::core::D3DERR_INVALIDCALL;
  return d->iface->SetScissorRect({r->left,r->top,r->right,r->bottom});
}
extern "C" void dxmt9c_device_get_scissor_rect(D9CDevice* d, D9CRect* r) {
  (void)d; (void)r;
}

extern "C" int32_t dxmt9c_device_set_transform(D9CDevice* d, uint32_t state,
                                                const D9CMatrix* m) {
  if (!m) return dxmt9::core::D3DERR_INVALIDCALL;
  dxmt9::core::Matrix4x4 mat;
  std::memcpy(mat.m.data(), m->m, 16 * sizeof(float));
  return d->iface->SetTransform(state, mat);
}
extern "C" int32_t dxmt9c_device_get_transform(D9CDevice* d, uint32_t state,
                                                D9CMatrix* m) {
  (void)d; (void)state; (void)m;
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_material(D9CDevice* d, const D9CMaterial* m) {
  if (!m) return dxmt9::core::D3DERR_INVALIDCALL;
  dxmt9::core::Material mat;
  std::memcpy(&mat.diffuse,  &m->diffuse,  sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&mat.ambient,  &m->ambient,  sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&mat.specular, &m->specular, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&mat.emissive, &m->emissive, sizeof(dxmt9::core::ColorRGBA));
  mat.power = m->power;
  return d->iface->SetMaterial(mat);
}
extern "C" int32_t dxmt9c_device_get_material(D9CDevice* d, D9CMaterial* m) {
  (void)d; (void)m;
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_light(D9CDevice* d, uint32_t idx,
                                            const D9CLight* l) {
  if (!l) return dxmt9::core::D3DERR_INVALIDCALL;
  dxmt9::core::Light light;
  light.type = static_cast<dxmt9::core::LightType>(l->type);
  std::memcpy(&light.diffuse,  &l->diffuse,  sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&light.specular, &l->specular, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&light.ambient,  &l->ambient,  sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(light.position.data(),  l->position,  3*sizeof(float));
  std::memcpy(light.direction.data(), l->direction, 3*sizeof(float));
  light.range = l->range; light.falloff = l->falloff;
  light.attenuation0 = l->attenuation0;
  light.attenuation1 = l->attenuation1;
  light.attenuation2 = l->attenuation2;
  light.theta = l->theta; light.phi = l->phi;
  return d->iface->SetLight(idx, light);
}
extern "C" int32_t dxmt9c_device_light_enable(D9CDevice* d, uint32_t i, uint32_t en) {
  return d->iface->LightEnable(i, en != 0);
}

extern "C" int32_t dxmt9c_device_set_render_state(D9CDevice* d, uint32_t s, uint32_t v) {
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
extern "C" int32_t dxmt9c_device_set_sampler_state(D9CDevice* d, uint32_t s,
                                                    uint32_t type, uint32_t val) {
  return d->iface->SetSamplerState(s, type, val);
}
extern "C" uint32_t dxmt9c_device_get_sampler_state(D9CDevice* d, uint32_t s,
                                                      uint32_t type) {
  return d->iface->GetSamplerState(s, type);
}
extern "C" int32_t dxmt9c_device_set_clip_plane(D9CDevice* d, uint32_t idx,
                                                 const float plane[4]) {
  dxmt9::core::ClipPlane cp{plane[0],plane[1],plane[2],plane[3]};
  return d->iface->SetClipPlane(idx, cp);
}
extern "C" int32_t dxmt9c_device_get_clip_plane(D9CDevice* d, uint32_t idx,
                                                  float plane[4]) {
  (void)d; (void)idx; (void)plane;
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_fvf(D9CDevice* d, uint32_t fvf) {
  return d->iface->SetFVF(fvf);
}
extern "C" uint32_t dxmt9c_device_get_fvf(D9CDevice* d) {
  (void)d; return 0;
}
extern "C" int32_t dxmt9c_device_set_vertex_declaration(D9CDevice* d, D9CVertexDecl* vd) {
  if (!vd) return d->iface->SetVertexDeclaration({});
  return d->iface->SetVertexDeclaration(vd->elements);
}
extern "C" int32_t dxmt9c_device_set_stream_source(D9CDevice* d, uint32_t stream,
                                                    D9CBuffer* buf, uint32_t off,
                                                    uint32_t stride) {
  auto bufPtr = buf ? buf->obj : nullptr;
  return d->iface->SetStreamSource(stream, bufPtr, off, stride);
}
extern "C" int32_t dxmt9c_device_set_stream_source_freq(D9CDevice* d, uint32_t stream,
                                                          uint32_t freq) {
  (void)d; (void)stream; (void)freq;
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_indices(D9CDevice* d, D9CBuffer* buf) {
  auto bufPtr = buf ? buf->obj : nullptr;
  return d->iface->SetIndices(bufPtr);
}
extern "C" int32_t dxmt9c_device_set_texture(D9CDevice* d, uint32_t stage,
                                              D9CTexture* tex) {
  auto texPtr = tex ? tex->obj : nullptr;
  return d->iface->SetTexture(stage, texPtr);
}

extern "C" int32_t dxmt9c_device_set_vertex_shader(D9CDevice* d, D9CShader* s) {
  if (!s) return d->iface->SetVertexShader({});
  return d->iface->SetVertexShader(s->ref);
}
extern "C" int32_t dxmt9c_device_set_pixel_shader(D9CDevice* d, D9CShader* s) {
  if (!s) return d->iface->SetPixelShader({});
  return d->iface->SetPixelShader(s->ref);
}

static int32_t setVsConst(D9CDevice* d, uint32_t start, const float* data, uint32_t cnt,
                           bool ps) {
  auto& state = d->dev().mutableState();
  if (ps) {
    auto& consts = state.psConst;
    for (uint32_t i = 0; i < cnt && (start+i) < consts.float4.size(); ++i) {
      consts.float4[start+i][0] = data[i*4+0];
      consts.float4[start+i][1] = data[i*4+1];
      consts.float4[start+i][2] = data[i*4+2];
      consts.float4[start+i][3] = data[i*4+3];
    }
  } else {
    auto& consts = state.vsConst;
    for (uint32_t i = 0; i < cnt && (start+i) < consts.float4.size(); ++i) {
      consts.float4[start+i][0] = data[i*4+0];
      consts.float4[start+i][1] = data[i*4+1];
      consts.float4[start+i][2] = data[i*4+2];
      consts.float4[start+i][3] = data[i*4+3];
    }
  }
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_vs_const_f(D9CDevice* d, uint32_t s,
                                                  const float* data, uint32_t cnt) {
  return setVsConst(d, s, data, cnt, false);
}
extern "C" int32_t dxmt9c_device_get_vs_const_f(D9CDevice* d, uint32_t s,
                                                  float* data, uint32_t cnt) {
  auto& consts = d->dev().state().vsConst;
  for (uint32_t i = 0; i < cnt && (s+i) < consts.float4.size(); ++i) {
    data[i*4+0] = consts.float4[s+i][0];
    data[i*4+1] = consts.float4[s+i][1];
    data[i*4+2] = consts.float4[s+i][2];
    data[i*4+3] = consts.float4[s+i][3];
  }
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_ps_const_f(D9CDevice* d, uint32_t s,
                                                  const float* data, uint32_t cnt) {
  return setVsConst(d, s, data, cnt, true);
}
extern "C" int32_t dxmt9c_device_get_ps_const_f(D9CDevice* d, uint32_t s,
                                                  float* data, uint32_t cnt) {
  auto& consts = d->dev().state().psConst;
  for (uint32_t i = 0; i < cnt && (s+i) < consts.float4.size(); ++i) {
    data[i*4+0] = consts.float4[s+i][0];
    data[i*4+1] = consts.float4[s+i][1];
    data[i*4+2] = consts.float4[s+i][2];
    data[i*4+3] = consts.float4[s+i][3];
  }
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_vs_const_i(D9CDevice* d, uint32_t s,
                                                  const int32_t* data, uint32_t cnt) {
  auto& consts = d->dev().mutableState().vsConst;
  for (uint32_t i = 0; i < cnt && (s+i) < consts.int4.size(); ++i) {
    consts.int4[s+i][0] = data[i*4+0];
    consts.int4[s+i][1] = data[i*4+1];
    consts.int4[s+i][2] = data[i*4+2];
    consts.int4[s+i][3] = data[i*4+3];
  }
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_ps_const_i(D9CDevice* d, uint32_t s,
                                                  const int32_t* data, uint32_t cnt) {
  auto& consts = d->dev().mutableState().psConst;
  for (uint32_t i = 0; i < cnt && (s+i) < consts.int4.size(); ++i) {
    consts.int4[s+i][0] = data[i*4+0];
    consts.int4[s+i][1] = data[i*4+1];
    consts.int4[s+i][2] = data[i*4+2];
    consts.int4[s+i][3] = data[i*4+3];
  }
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_vs_const_b(D9CDevice* d, uint32_t s,
                                                  const uint32_t* data, uint32_t cnt) {
  auto& consts = d->dev().mutableState().vsConst;
  for (uint32_t i = 0; i < cnt && (s+i) < consts.bools.size(); ++i)
    consts.bools[s+i] = (data[i] != 0);
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_ps_const_b(D9CDevice* d, uint32_t s,
                                                  const uint32_t* data, uint32_t cnt) {
  auto& consts = d->dev().mutableState().psConst;
  for (uint32_t i = 0; i < cnt && (s+i) < consts.bools.size(); ++i)
    consts.bools[s+i] = (data[i] != 0);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_render_target(D9CDevice* d, uint32_t idx,
                                                    D9CSurface* surf) {
  return d->iface->SetRenderTarget(idx, surf ? surf->obj : nullptr);
}
extern "C" D9CSurface* dxmt9c_device_get_render_target(D9CDevice* d, uint32_t idx) {
  auto sw = d->iface->GetSwapChain(0);
  if (!sw) return nullptr;
  auto surf = (idx == 0) ? sw->backBuffer() : nullptr;
  if (!surf) return nullptr;
  auto* wrap = new D9CSurface{surf};
  return wrap;
}
extern "C" int32_t dxmt9c_device_set_depth_stencil(D9CDevice* d, D9CSurface* surf) {
  return d->iface->SetDepthStencilSurface(surf ? surf->obj : nullptr);
}
extern "C" D9CSurface* dxmt9c_device_get_depth_stencil(D9CDevice* d) {
  auto sw = d->iface->GetSwapChain(0);
  if (!sw) return nullptr;
  auto surf = sw->depthStencilSurface();
  if (!surf) return nullptr;
  return new D9CSurface{surf};
}

extern "C" int32_t dxmt9c_device_draw_primitive(D9CDevice* d, uint32_t type,
                                                  uint32_t startVertex, uint32_t count) {
  return d->iface->DrawPrimitive(ptFromD3D(type), count, startVertex);
}
extern "C" int32_t dxmt9c_device_draw_indexed_primitive(D9CDevice* d, uint32_t type,
                                                          int32_t baseVertex, uint32_t minV,
                                                          uint32_t numV, uint32_t startIdx,
                                                          uint32_t count) {
  (void)minV; (void)numV;
  auto& st = d->dev().state();
  return d->iface->DrawIndexedPrimitive(ptFromD3D(type), count, 0,
                                         baseVertex, startIdx, st.indexType);
}
extern "C" int32_t dxmt9c_device_draw_primitive_up(D9CDevice* d, uint32_t type,
                                                     uint32_t count, const void* data,
                                                     uint32_t stride) {
  /* approximate vertex count */
  size_t bytes = stride * (count + 2) * 3;
  auto sp = std::span<const dxmt9::core::u8>(
    reinterpret_cast<const dxmt9::core::u8*>(data), bytes);
  return d->iface->DrawPrimitiveUP(ptFromD3D(type), count, sp);
}
extern "C" int32_t dxmt9c_device_draw_indexed_primitive_up(D9CDevice* d, uint32_t type,
                                                             uint32_t minV, uint32_t numV,
                                                             uint32_t count,
                                                             const void* idxData,
                                                             uint32_t idxFmt,
                                                             const void* vtxData,
                                                             uint32_t stride) {
  size_t vtxBytes = stride * (minV + numV);
  uint32_t idxSize = (idxFmt == 102) ? 4 : 2;
  size_t idxBytes = idxSize * count * 3;
  auto vsp = std::span<const dxmt9::core::u8>(
    reinterpret_cast<const dxmt9::core::u8*>(vtxData), vtxBytes);
  auto isp = std::span<const dxmt9::core::u8>(
    reinterpret_cast<const dxmt9::core::u8*>(idxData), idxBytes);
  return d->iface->DrawIndexedPrimitiveUP(ptFromD3D(type), count, vsp, isp,
                                           idxTypeFromD3D(idxFmt));
}

extern "C" int32_t dxmt9c_device_update_surface(D9CDevice* d, D9CSurface* src,
                                                  const D9CRect*, D9CSurface* dst,
                                                  const D9CRect*) {
  if (!src || !dst) return dxmt9::core::D3DERR_INVALIDCALL;
  return d->iface->UpdateSurface(src->obj, dst->obj);
}
extern "C" int32_t dxmt9c_device_update_texture(D9CDevice* d, D9CTexture* src,
                                                  D9CTexture* dst) {
  if (!src || !dst) return dxmt9::core::D3DERR_INVALIDCALL;
  return d->iface->UpdateTexture(src->obj, dst->obj);
}
extern "C" int32_t dxmt9c_device_stretch_rect(D9CDevice* d, D9CSurface* src,
                                               const D9CRect* sr, D9CSurface* dst,
                                               const D9CRect* dr, uint32_t filter) {
  if (!src || !dst) return dxmt9::core::D3DERR_INVALIDCALL;
  dxmt9::core::Rect* ps = sr ? new dxmt9::core::Rect{sr->left,sr->top,sr->right,sr->bottom} : nullptr;
  dxmt9::core::Rect* pd = dr ? new dxmt9::core::Rect{dr->left,dr->top,dr->right,dr->bottom} : nullptr;
  auto hr = d->iface->StretchRect(src->obj, ps, dst->obj, pd, filter != 1);
  delete ps; delete pd;
  return hr;
}
extern "C" int32_t dxmt9c_device_color_fill(D9CDevice* d, D9CSurface* surf,
                                             const D9CRect* r, uint32_t colorARGB) {
  if (!surf) return dxmt9::core::D3DERR_INVALIDCALL;
  dxmt9::core::Rect* pr = r ? new dxmt9::core::Rect{r->left,r->top,r->right,r->bottom} : nullptr;
  dxmt9::core::ColorRGBA rgba{
    ((colorARGB>>16)&0xff)/255.0f,
    ((colorARGB>> 8)&0xff)/255.0f,
    ((colorARGB    )&0xff)/255.0f,
    ((colorARGB>>24)&0xff)/255.0f,
  };
  auto hr = d->iface->FillSurface(surf->obj, pr, rgba);
  delete pr;
  return hr;
}
extern "C" int32_t dxmt9c_device_get_render_target_data(D9CDevice* d, D9CSurface* rt,
                                                          D9CSurface* dst) {
  if (!rt || !dst) return dxmt9::core::D3DERR_INVALIDCALL;
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
extern "C" int32_t dxmt9c_device_check_device_multisample(D9CDevice* d,
                                                            uint32_t fmt, uint32_t msType,
                                                            uint32_t windowed) {
  return d->iface->CheckDeviceMultiSampleType(fmtFromD3D(fmt), msTypeFromD3D(msType));
  (void)windowed;
}
extern "C" D9CSwapChain* dxmt9c_device_get_swap_chain(D9CDevice* d, uint32_t idx) {
  auto* sw = d->iface->GetSwapChain(idx);
  if (!sw) return nullptr;
  return new D9CSwapChain(sw);
}
extern "C" uint32_t dxmt9c_device_get_swap_chain_count(D9CDevice* d) {
  return static_cast<uint32_t>(d->iface->GetSwapChainCount());
}
extern "C" D9CSwapChain* dxmt9c_device_create_additional_swap_chain(D9CDevice* d,
                                                                      const D9CPresentParams* pp) {
  if (!pp) return nullptr;
  auto* sw = d->iface->CreateAdditionalSwapChain(ppFromC(*pp));
  if (!sw) return nullptr;
  return new D9CSwapChain(sw);
}

/* ── resource creation ───────────────────────────────────────────────────── */

extern "C" D9CTexture* dxmt9c_device_create_texture(D9CDevice* d, uint32_t w, uint32_t h,
                                                      uint32_t levels, uint32_t usage,
                                                      uint32_t fmt, uint32_t pool) {
  dxmt9::core::TextureDesc desc;
  desc.width = w; desc.height = h; desc.levels = levels ? levels : 1;
  desc.format = fmtFromD3D(fmt);
  desc.pool = poolFromD3D(pool);
  desc.usage = usage;
  desc.type = dxmt9::core::TextureType::TwoD;
  auto tex = d->iface->CreateTexture(desc);
  if (!tex) return nullptr;
  return new D9CTexture{tex, d};
}
extern "C" D9CTexture* dxmt9c_device_create_cube_texture(D9CDevice* d, uint32_t size,
                                                           uint32_t levels, uint32_t usage,
                                                           uint32_t fmt, uint32_t pool) {
  dxmt9::core::TextureDesc desc;
  desc.width = size; desc.height = size; desc.levels = levels ? levels : 1;
  desc.format = fmtFromD3D(fmt);
  desc.pool = poolFromD3D(pool);
  desc.usage = usage;
  desc.type = dxmt9::core::TextureType::Cube;
  auto tex = d->iface->CreateTexture(desc);
  if (!tex) return nullptr;
  return new D9CTexture{tex, d};
}
extern "C" D9CTexture* dxmt9c_device_create_volume_texture(D9CDevice* d, uint32_t w,
                                                             uint32_t h, uint32_t depth,
                                                             uint32_t levels, uint32_t usage,
                                                             uint32_t fmt, uint32_t pool) {
  dxmt9::core::TextureDesc desc;
  desc.width = w; desc.height = h; desc.depth = depth;
  desc.levels = levels ? levels : 1;
  desc.format = fmtFromD3D(fmt);
  desc.pool = poolFromD3D(pool);
  desc.usage = usage;
  desc.type = dxmt9::core::TextureType::Volume;
  auto tex = d->iface->CreateTexture(desc);
  if (!tex) return nullptr;
  return new D9CTexture{tex, d};
}

extern "C" D9CBuffer* dxmt9c_device_create_vertex_buffer(D9CDevice* d, uint32_t len,
                                                           uint32_t usage, uint32_t /*fvf*/,
                                                           uint32_t pool) {
  dxmt9::core::BufferDesc desc{len, poolFromD3D(pool), usage};
  auto buf = d->iface->CreateBuffer(desc);
  if (!buf) return nullptr;
  return new D9CBuffer{buf};
}
extern "C" D9CBuffer* dxmt9c_device_create_index_buffer(D9CDevice* d, uint32_t len,
                                                          uint32_t usage, uint32_t /*fmt*/,
                                                          uint32_t pool) {
  dxmt9::core::BufferDesc desc{len, poolFromD3D(pool), usage};
  auto buf = d->iface->CreateBuffer(desc);
  if (!buf) return nullptr;
  return new D9CBuffer{buf};
}

extern "C" D9CSurface* dxmt9c_device_create_render_target(D9CDevice* d, uint32_t w,
                                                            uint32_t h, uint32_t fmt,
                                                            uint32_t msType, uint32_t /*msQ*/,
                                                            uint32_t /*lockable*/,
                                                            uint64_t* /*shared*/) {
  dxmt9::core::SurfaceDesc desc;
  desc.width = w; desc.height = h;
  desc.format = fmtFromD3D(fmt);
  desc.renderTarget = true;
  desc.multiSampleType = msTypeFromD3D(msType);
  auto surf = d->iface->CreateSurface(desc);
  if (!surf) return nullptr;
  return new D9CSurface{surf};
}
extern "C" D9CSurface* dxmt9c_device_create_depth_stencil(D9CDevice* d, uint32_t w,
                                                            uint32_t h, uint32_t fmt,
                                                            uint32_t msType, uint32_t /*msQ*/,
                                                            uint32_t /*discard*/,
                                                            uint64_t* /*shared*/) {
  dxmt9::core::SurfaceDesc desc;
  desc.width = w; desc.height = h;
  desc.format = fmtFromD3D(fmt);
  desc.depthStencil = true;
  desc.multiSampleType = msTypeFromD3D(msType);
  auto surf = d->iface->CreateSurface(desc);
  if (!surf) return nullptr;
  return new D9CSurface{surf};
}
extern "C" D9CSurface* dxmt9c_device_create_offscreen_surface(D9CDevice* d, uint32_t w,
                                                                uint32_t h, uint32_t fmt,
                                                                uint32_t pool,
                                                                uint64_t* /*shared*/) {
  dxmt9::core::SurfaceDesc desc;
  desc.width = w; desc.height = h;
  desc.format = fmtFromD3D(fmt);
  desc.pool = poolFromD3D(pool);
  auto surf = d->iface->CreateSurface(desc);
  if (!surf) return nullptr;
  return new D9CSurface{surf};
}

extern "C" D9CShader* dxmt9c_device_create_vertex_shader(D9CDevice* d,
                                                           const uint32_t* bytecode) {
  /* count dwords until end token 0xFFFF */
  size_t n = 0;
  while (bytecode[n] != 0xFFFF) ++n;
  ++n; /* include end token */
  dxmt9::core::ShaderBytecode bc;
  bc.bytes.assign(reinterpret_cast<const uint8_t*>(bytecode),
                  reinterpret_cast<const uint8_t*>(bytecode) + n*4);
  bc.hash = dxmt9::core::hashBytes(
    std::span<const std::byte>(reinterpret_cast<const std::byte*>(bc.bytes.data()),
                                bc.bytes.size()));
  dxmt9::core::ShaderRef ref;
  ref.kind = dxmt9::core::ShaderRef::Kind::Bytecode;
  ref.hash = bc.hash;
  ref.bytecode = std::move(bc);
  auto* s = new D9CShader;
  s->ref = std::move(ref);
  s->bytecodeWords.assign(bytecode, bytecode + n);
  return s;
}
extern "C" D9CShader* dxmt9c_device_create_pixel_shader(D9CDevice* d,
                                                          const uint32_t* bytecode) {
  (void)d;
  return dxmt9c_device_create_vertex_shader(d, bytecode); /* same logic */
}

extern "C" D9CVertexDecl* dxmt9c_device_create_vertex_declaration(
    D9CDevice* /*d*/, const D9CVertexElement* elems) {
  auto* vd = new D9CVertexDecl;
  for (const D9CVertexElement* e = elems;
       !(e->stream == 0xff && e->type == 17 /* D3DDECLTYPE_UNUSED */); ++e) {
    dxmt9::core::VertexElement ve;
    ve.stream = e->stream;
    ve.offset = e->offset;
    ve.type   = e->type;
    ve.method = e->method;
    ve.usage  = e->usage;
    ve.usageIndex = e->usageIndex;
    vd->elements.push_back(ve);
    vd->raw.push_back(*e);
  }
  /* sentinel */
  vd->raw.push_back({0xff,0,17,0,0,0});
  return vd;
}

extern "C" D9CQuery* dxmt9c_device_create_query(D9CDevice* d, uint32_t type) {
  dxmt9::core::QueryType qt;
  switch (type) {
    case 8:  qt = dxmt9::core::QueryType::Occlusion; break;
    case 9:  qt = dxmt9::core::QueryType::Timestamp; break;
    case 10: qt = dxmt9::core::QueryType::TimestampDisjoint; break;
    case 11: qt = dxmt9::core::QueryType::TimestampFreq; break;
    default: qt = dxmt9::core::QueryType::Event; break;
  }
  auto q = d->iface->CreateQuery(qt);
  if (!q) return nullptr;
  return new D9CQuery{q, d};
}

extern "C" D9CStateBlock* dxmt9c_device_create_state_block(D9CDevice* d, uint32_t /*type*/) {
  auto sb = d->iface->CreateStateBlock();
  if (!sb) return nullptr;
  return new D9CStateBlock{sb, d};
}
extern "C" int32_t dxmt9c_device_begin_state_block(D9CDevice* /*d*/) {
  return dxmt9::core::E_NOTIMPL;
}
extern "C" int32_t dxmt9c_device_end_state_block(D9CDevice* d, D9CStateBlock** out) {
  (void)d; (void)out;
  return dxmt9::core::E_NOTIMPL;
}

/* ── swap chain ──────────────────────────────────────────────────────────── */

extern "C" void dxmt9c_swapchain_addref(D9CSwapChain* s) {
  if (s) s->refs.fetch_add(1);
}
extern "C" uint32_t dxmt9c_swapchain_release(D9CSwapChain* s) {
  if (!s) return 0;
  uint32_t r = s->refs.fetch_sub(1) - 1;
  if (r == 0) delete s;
  return r;
}
extern "C" int32_t dxmt9c_swapchain_present(D9CSwapChain* s,
                                             const D9CRect*, const D9CRect*,
                                             uint64_t, const void*, uint32_t) {
  return s->iface->Present();
}
extern "C" D9CSurface* dxmt9c_swapchain_get_back_buffer(D9CSwapChain* s, uint32_t,
                                                          uint32_t) {
  auto surf = s->iface->backBuffer();
  if (!surf) return nullptr;
  return new D9CSurface{surf};
}
extern "C" D9CSurface* dxmt9c_swapchain_get_depth_stencil(D9CSwapChain* s) {
  auto surf = s->iface->depthStencilSurface();
  if (!surf) return nullptr;
  return new D9CSurface{surf};
}
extern "C" int32_t dxmt9c_swapchain_get_present_params(D9CSwapChain* s,
                                                         D9CPresentParams* out) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  auto& p = s->iface->presentParameters();
  std::memset(out, 0, sizeof(*out));
  out->backBufferWidth  = p.backBufferWidth;
  out->backBufferHeight = p.backBufferHeight;
  out->windowed         = p.windowed;
  return dxmt9::core::D3D_OK;
}

/* ── texture ─────────────────────────────────────────────────────────────── */

extern "C" void dxmt9c_texture_addref(D9CTexture* t) {
  if (t) t->refs.fetch_add(1);
}
extern "C" uint32_t dxmt9c_texture_release(D9CTexture* t) {
  if (!t) return 0;
  uint32_t r = t->refs.fetch_sub(1) - 1;
  if (r == 0) delete t;
  return r;
}
extern "C" int32_t dxmt9c_texture_lock_rect(D9CTexture* t, uint32_t level,
                                             D9CLockedRect* out, const D9CRect* r,
                                             uint32_t flags) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  dxmt9::core::Rect* pr = r ? new dxmt9::core::Rect{r->left,r->top,r->right,r->bottom} : nullptr;
  auto lr = t->obj->lockRect(level, pr, flags);
  delete pr;
  out->pitch = static_cast<int32_t>(lr.pitch);
  out->bits  = lr.data;
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_texture_unlock_rect(D9CTexture* t, uint32_t level) {
  t->obj->unlockRect(level);
  return dxmt9::core::D3D_OK;
}
extern "C" D9CSurface* dxmt9c_texture_get_surface_level(D9CTexture* t, uint32_t level) {
  auto surf = t->obj->surfaceLevel(level);
  if (!surf) return nullptr;
  auto* wrap = new D9CSurface{surf};
  wrap->ownerTex = t;
  t->refs.fetch_add(1); /* keep texture alive while surface is alive */
  return wrap;
}
extern "C" uint32_t dxmt9c_texture_get_level_count(D9CTexture* t) {
  return t->obj->levelCount();
}
extern "C" int32_t dxmt9c_texture_get_level_desc(D9CTexture* t, uint32_t level,
                                                   D9CSurfaceDesc* out) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  (void)level;
  auto& d = t->obj->desc();
  std::memset(out, 0, sizeof(*out));
  out->width  = d.width;
  out->height = d.height;
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_texture_generate_mip_sublevels(D9CTexture* /*t*/) {
  return dxmt9::core::D3D_OK;
}

/* ── buffer ──────────────────────────────────────────────────────────────── */

extern "C" void dxmt9c_buffer_addref(D9CBuffer* b) {
  if (b) b->refs.fetch_add(1);
}
extern "C" uint32_t dxmt9c_buffer_release(D9CBuffer* b) {
  if (!b) return 0;
  uint32_t r = b->refs.fetch_sub(1) - 1;
  if (r == 0) delete b;
  return r;
}
extern "C" int32_t dxmt9c_buffer_lock(D9CBuffer* b, uint32_t offset, uint32_t size,
                                       void** data, uint32_t flags) {
  if (!data) return dxmt9::core::D3DERR_INVALIDCALL;
  auto lr = b->obj->lock(offset, size ? size : b->obj->desc().size, flags);
  *data = lr.data;
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_buffer_unlock(D9CBuffer* b) {
  b->obj->unlock();
  return dxmt9::core::D3D_OK;
}

/* ── surface ─────────────────────────────────────────────────────────────── */

extern "C" void dxmt9c_surface_addref(D9CSurface* s) {
  if (s) s->refs.fetch_add(1);
}
extern "C" uint32_t dxmt9c_surface_release(D9CSurface* s) {
  if (!s) return 0;
  uint32_t r = s->refs.fetch_sub(1) - 1;
  if (r == 0) {
    if (s->ownerTex) dxmt9c_texture_release(s->ownerTex);
    delete s;
  }
  return r;
}
extern "C" int32_t dxmt9c_surface_lock_rect(D9CSurface* s, D9CLockedRect* out,
                                             const D9CRect* r, uint32_t flags) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  dxmt9::core::Rect* pr = r ? new dxmt9::core::Rect{r->left,r->top,r->right,r->bottom} : nullptr;
  auto lr = s->obj->lockRect(pr, flags);
  delete pr;
  out->pitch = static_cast<int32_t>(lr.pitch);
  out->bits  = lr.data;
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_surface_unlock_rect(D9CSurface* s) {
  s->obj->unlockRect();
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_surface_get_desc(D9CSurface* s, D9CSurfaceDesc* out) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  auto& d = s->obj->desc();
  std::memset(out, 0, sizeof(*out));
  out->width  = d.width;
  out->height = d.height;
  return dxmt9::core::D3D_OK;
}
extern "C" D9CTexture* dxmt9c_surface_get_container_texture(D9CSurface* s) {
  return s->ownerTex;
}

/* ── shader ──────────────────────────────────────────────────────────────── */

extern "C" void dxmt9c_shader_addref(D9CShader* s) {
  if (s) s->refs.fetch_add(1);
}
extern "C" uint32_t dxmt9c_shader_release(D9CShader* s) {
  if (!s) return 0;
  uint32_t r = s->refs.fetch_sub(1) - 1;
  if (r == 0) delete s;
  return r;
}
extern "C" int32_t dxmt9c_shader_get_bytecode(D9CShader* s, void* data, uint32_t* size) {
  uint32_t bytes = static_cast<uint32_t>(s->bytecodeWords.size() * 4);
  if (!data) { if (size) *size = bytes; return dxmt9::core::D3D_OK; }
  if (size && *size < bytes) return dxmt9::core::D3DERR_INVALIDCALL;
  std::memcpy(data, s->bytecodeWords.data(), bytes);
  if (size) *size = bytes;
  return dxmt9::core::D3D_OK;
}

/* ── vertex declaration ──────────────────────────────────────────────────── */

extern "C" void dxmt9c_vdecl_addref(D9CVertexDecl* v) {
  if (v) v->refs.fetch_add(1);
}
extern "C" uint32_t dxmt9c_vdecl_release(D9CVertexDecl* v) {
  if (!v) return 0;
  uint32_t r = v->refs.fetch_sub(1) - 1;
  if (r == 0) delete v;
  return r;
}
extern "C" int32_t dxmt9c_vdecl_get_declaration(D9CVertexDecl* v, D9CVertexElement* out,
                                                   uint32_t* count) {
  uint32_t n = static_cast<uint32_t>(v->raw.size());
  if (!out) { if (count) *count = n; return dxmt9::core::D3D_OK; }
  std::memcpy(out, v->raw.data(), n * sizeof(D9CVertexElement));
  if (count) *count = n;
  return dxmt9::core::D3D_OK;
}

/* ── query ───────────────────────────────────────────────────────────────── */

extern "C" void dxmt9c_query_addref(D9CQuery* q) {
  if (q) q->refs.fetch_add(1);
}
extern "C" uint32_t dxmt9c_query_release(D9CQuery* q) {
  if (!q) return 0;
  uint32_t r = q->refs.fetch_sub(1) - 1;
  if (r == 0) delete q;
  return r;
}
extern "C" int32_t dxmt9c_query_issue(D9CQuery* q, uint32_t flags) {
  return q->device->iface->IssueQuery(q->obj, (flags & 2) != 0);
}
extern "C" int32_t dxmt9c_query_get_data(D9CQuery* q, void* data, uint32_t size,
                                          uint32_t flags) {
  return q->device->iface->GetQueryData(q->obj, data, size, flags);
}
extern "C" uint32_t dxmt9c_query_get_data_size(D9CQuery* q) {
  switch (q->obj->type()) {
    case dxmt9::core::QueryType::Occlusion:  return 8;
    case dxmt9::core::QueryType::Timestamp:  return 8;
    case dxmt9::core::QueryType::TimestampFreq: return 8;
    default: return 0;
  }
}
extern "C" uint32_t dxmt9c_query_get_type(D9CQuery* q) {
  switch (q->obj->type()) {
    case dxmt9::core::QueryType::Occlusion:        return 8;
    case dxmt9::core::QueryType::Timestamp:        return 9;
    case dxmt9::core::QueryType::TimestampDisjoint: return 10;
    case dxmt9::core::QueryType::TimestampFreq:    return 11;
    default:                                        return 7; /* EVENT */
  }
}

/* ── state block ─────────────────────────────────────────────────────────── */

extern "C" void dxmt9c_stateblock_addref(D9CStateBlock* s) {
  if (s) s->refs.fetch_add(1);
}
extern "C" uint32_t dxmt9c_stateblock_release(D9CStateBlock* s) {
  if (!s) return 0;
  uint32_t r = s->refs.fetch_sub(1) - 1;
  if (r == 0) delete s;
  return r;
}
extern "C" int32_t dxmt9c_stateblock_capture(D9CStateBlock* s) {
  s->obj->capture(s->device->dev().state());
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_stateblock_apply(D9CStateBlock* s) {
  s->obj->apply(s->device->dev());
  return dxmt9::core::D3D_OK;
}
