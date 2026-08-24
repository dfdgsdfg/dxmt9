/* src/d3d9/d3d9_pe_device_child_shader.cpp — PE-side child COM wrappers
 * for IDirect3DVertexShader9 and IDirect3DPixelShader9. */

#include "d3d9_pe_device_child.hpp"

#include <cstdint>
#include <new>

static inline HRESULT hr32(int32_t r) { return (HRESULT)r; }

template <typename T, typename... Args>
T* peNewNoexcept(Args&&... args) noexcept {
  try {
    return new (std::nothrow) T(std::forward<Args>(args)...);
  } catch (...) {
    return nullptr;
  }
}

/* ── VertexShader ───────────────────────────────────────────────────────────
 */

class D3D9VertexShaderImpl final : public IDirect3DVertexShader9 {
  ULONG refs_ = 1;
  D9CShader *s_;
  IDirect3DDevice9 *device_;
  D3D9PeRecorderFlush *recorder_;
  std::uint64_t hash_ = 0;
  dxmt9::d3d9::pe::ShaderRef wireObject_{};

public:
  D3D9VertexShaderImpl(D9CShader *s, IDirect3DDevice9 *device,
                       std::uint64_t hash, D3D9PeRecorderFlush *recorder)
      : s_(s), device_(device), recorder_(recorder), hash_(hash) {
    if (device_)
      device_->AddRef();
    dxmt9::d3d9::pe::cacheWireObjectRef(
        s_,
        dxmt9c_shader_get_wire_identity, wireObject_);
  }
  ~D3D9VertexShaderImpl() {
    if (recorder_)
      recorder_->NotifyRenderTapeObjectDestroyForChild(wireObject_);
    dxmt9c_shader_release(s_);
    if (device_)
      device_->Release();
  }

  D9CShader *raw() const { return s_; }
  IDirect3DDevice9 *ownerDevice() const { return device_; }
  const dxmt9::d3d9::pe::ShaderRef &wireObject() const {
    return wireObject_;
  }
  std::uint64_t hash() const { return hash_; }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    ULONG r = --refs_;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, IID_IDirect3DVertexShader9)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **pp) noexcept override {
    if (!pp)
      return D3DERR_INVALIDCALL;
    if (!device_) {
      *pp = nullptr;
      return D3DERR_INVALIDCALL;
    }
    device_->AddRef();
    *pp = device_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFunction(void *pData,
                                        UINT *pSize) noexcept override {
    if (!pSize)
      return D3DERR_INVALIDCALL;
    return hr32(dxmt9c_shader_get_bytecode(s_, pData, pSize));
  }
};

/* ── PixelShader ────────────────────────────────────────────────────────────
 */

class D3D9PixelShaderImpl final : public IDirect3DPixelShader9 {
  ULONG refs_ = 1;
  D9CShader *s_;
  IDirect3DDevice9 *device_;
  D3D9PeRecorderFlush *recorder_;
  std::uint64_t hash_ = 0;
  dxmt9::d3d9::pe::ShaderRef wireObject_{};

public:
  D3D9PixelShaderImpl(D9CShader *s, IDirect3DDevice9 *device,
                      std::uint64_t hash, D3D9PeRecorderFlush *recorder)
      : s_(s), device_(device), recorder_(recorder), hash_(hash) {
    if (device_)
      device_->AddRef();
    dxmt9::d3d9::pe::cacheWireObjectRef(
        s_,
        dxmt9c_shader_get_wire_identity, wireObject_);
  }
  ~D3D9PixelShaderImpl() {
    if (recorder_)
      recorder_->NotifyRenderTapeObjectDestroyForChild(wireObject_);
    dxmt9c_shader_release(s_);
    if (device_)
      device_->Release();
  }

  D9CShader *raw() const { return s_; }
  IDirect3DDevice9 *ownerDevice() const { return device_; }
  const dxmt9::d3d9::pe::ShaderRef &wireObject() const {
    return wireObject_;
  }
  std::uint64_t hash() const { return hash_; }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    ULONG r = --refs_;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppv) noexcept override {
    if (!ppv)
      return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, IID_IDirect3DPixelShader9)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **pp) noexcept override {
    if (!pp)
      return D3DERR_INVALIDCALL;
    if (!device_) {
      *pp = nullptr;
      return D3DERR_INVALIDCALL;
    }
    device_->AddRef();
    *pp = device_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFunction(void *pData,
                                        UINT *pSize) noexcept override {
    if (!pSize)
      return D3DERR_INVALIDCALL;
    return hr32(dxmt9c_shader_get_bytecode(s_, pData, pSize));
  }
};

/* =========================================================================
 * Public factory + raw-handle extractors for shader family.
 * ========================================================================= */

IDirect3DVertexShader9 *CreatePeVertexShader(D9CShader *shader,
                                             IDirect3DDevice9 *device,
                                             std::uint64_t hash,
                                             D3D9PeRecorderFlush *recorder) noexcept {
  return peNewNoexcept<D3D9VertexShaderImpl>(shader, device, hash, recorder);
}

IDirect3DPixelShader9 *CreatePePixelShader(D9CShader *shader,
                                           IDirect3DDevice9 *device,
                                           std::uint64_t hash,
                                           D3D9PeRecorderFlush *recorder) noexcept {
  return peNewNoexcept<D3D9PixelShaderImpl>(shader, device, hash, recorder);
}

D9CShader *D3D9PeRawVertexShader(IDirect3DVertexShader9 *shader) {
  return shader ? static_cast<D3D9VertexShaderImpl *>(shader)->raw() : nullptr;
}

namespace {
template <typename Impl, typename Interface>
HRESULT validateShader(Interface* object, const void* expectedOwnerDevice,
                       dxmt9::d3d9::pe::PeConcreteObjectKind kind,
                       D3D9PeValidatedShader* out) noexcept {
  if (out) *out = {};
  if (!object) return S_OK;
  auto* impl = dynamic_cast<Impl*>(object);
  if (!impl || static_cast<Interface*>(impl) != object) {
    return D3DERR_INVALIDCALL;
  }
  const auto& wire = impl->wireObject();
  const dxmt9::d3d9::pe::PeConcreteMemberIdentity identity{
      .kind = kind,
      .ownerDevice = impl->ownerDevice(),
      .publicIdentity = static_cast<Interface*>(impl),
      .wireIdentity = wire.identity,
  };
  if (!dxmt9::d3d9::pe::validateConcreteMemberIdentity(
          identity, kind, expectedOwnerDevice, object)) {
    return D3DERR_INVALIDCALL;
  }
  if (out) *out = {.raw = impl->raw(), .wire = wire};
  return S_OK;
}
}  // namespace

HRESULT D3D9PeValidateVertexShader(
    IDirect3DVertexShader9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedShader* out) noexcept {
  return validateShader<D3D9VertexShaderImpl>(
      object, expectedOwnerDevice,
      dxmt9::d3d9::pe::PeConcreteObjectKind::VertexShader, out);
}

HRESULT D3D9PeValidatePixelShader(
    IDirect3DPixelShader9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedShader* out) noexcept {
  return validateShader<D3D9PixelShaderImpl>(
      object, expectedOwnerDevice,
      dxmt9::d3d9::pe::PeConcreteObjectKind::PixelShader, out);
}

D9CShader *D3D9PeRawPixelShader(IDirect3DPixelShader9 *shader) {
  return shader ? static_cast<D3D9PixelShaderImpl *>(shader)->raw() : nullptr;
}

const dxmt9::d3d9::pe::ShaderRef &
D3D9PeWireVertexShader(IDirect3DVertexShader9 *shader) {
  static const dxmt9::d3d9::pe::ShaderRef empty{};
  return shader ? static_cast<D3D9VertexShaderImpl *>(shader)->wireObject()
                : empty;
}

const dxmt9::d3d9::pe::ShaderRef &
D3D9PeWirePixelShader(IDirect3DPixelShader9 *shader) {
  static const dxmt9::d3d9::pe::ShaderRef empty{};
  return shader ? static_cast<D3D9PixelShaderImpl *>(shader)->wireObject()
                : empty;
}

std::uint64_t D3D9PeVertexShaderHash(IDirect3DVertexShader9 *shader) {
  return shader ? static_cast<D3D9VertexShaderImpl *>(shader)->hash() : 0;
}

std::uint64_t D3D9PePixelShaderHash(IDirect3DPixelShader9 *shader) {
  return shader ? static_cast<D3D9PixelShaderImpl *>(shader)->hash() : 0;
}
