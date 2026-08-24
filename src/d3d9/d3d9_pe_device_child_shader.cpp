/* src/d3d9/d3d9_pe_device_child_shader.cpp — PE-side child COM wrappers
 * for IDirect3DVertexShader9 and IDirect3DPixelShader9. */

#include "d3d9_pe_child_factories.hpp"
#include "d3d9_pe_child_validation.hpp"
#include "d3d9_pe_validated_object_writer.hpp"

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
  D3D9PeShaderDeclarationContext *context_;
  std::uint64_t hash_ = 0;
  dxmt9::d3d9::pe::ShaderRef wireObject_{};

public:
  D3D9VertexShaderImpl(D9CShader *s, IDirect3DDevice9 *device,
                       std::uint64_t hash, D3D9PeShaderDeclarationContext *recorder)
      : s_(s), device_(device), context_(recorder), hash_(hash) {
    if (device_)
      device_->AddRef();
    dxmt9::d3d9::pe::cacheWireObjectRef(
        s_,
        dxmt9c_shader_get_wire_identity, wireObject_);
  }
  ~D3D9VertexShaderImpl() {
    if (context_)
      context_->NotifyRenderTapeObjectDestroyForChild(wireObject_);
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
  D3D9PeShaderDeclarationContext *context_;
  std::uint64_t hash_ = 0;
  dxmt9::d3d9::pe::ShaderRef wireObject_{};

public:
  D3D9PixelShaderImpl(D9CShader *s, IDirect3DDevice9 *device,
                      std::uint64_t hash, D3D9PeShaderDeclarationContext *recorder)
      : s_(s), device_(device), context_(recorder), hash_(hash) {
    if (device_)
      device_->AddRef();
    dxmt9::d3d9::pe::cacheWireObjectRef(
        s_,
        dxmt9c_shader_get_wire_identity, wireObject_);
  }
  ~D3D9PixelShaderImpl() {
    if (context_)
      context_->NotifyRenderTapeObjectDestroyForChild(wireObject_);
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
 * Public factory + trusted reference helpers for shader family.
 * ========================================================================= */

IDirect3DVertexShader9 *CreatePeVertexShader(D9CShader *shader,
                                             IDirect3DDevice9 *device,
                                             std::uint64_t hash,
                                             D3D9PeShaderDeclarationContext *recorder) noexcept {
  return peNewNoexcept<D3D9VertexShaderImpl>(shader, device, hash, recorder);
}

IDirect3DPixelShader9 *CreatePePixelShader(D9CShader *shader,
                                           IDirect3DDevice9 *device,
                                           std::uint64_t hash,
                                           D3D9PeShaderDeclarationContext *recorder) noexcept {
  return peNewNoexcept<D3D9PixelShaderImpl>(shader, device, hash, recorder);
}

namespace {
template <typename Impl, typename Interface, typename Validated>
HRESULT validateShader(Interface* object, const void* expectedOwnerDevice,
                       dxmt9::d3d9::pe::PeConcreteObjectKind kind,
                       Validated* out) noexcept {
  D3D9PeValidatedObjectWriter::clear(out);
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
  D3D9PeValidatedObjectWriter::assign(
      out, object, expectedOwnerDevice, impl->raw(), wire, impl->hash(), kind);
  return S_OK;
}
}  // namespace

HRESULT D3D9PeValidateVertexShader(
    IDirect3DVertexShader9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedVertexShader* out) noexcept {
  return validateShader<D3D9VertexShaderImpl>(
      object, expectedOwnerDevice,
      dxmt9::d3d9::pe::PeConcreteObjectKind::VertexShader, out);
}

HRESULT D3D9PeValidatePixelShader(
    IDirect3DPixelShader9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedPixelShader* out) noexcept {
  return validateShader<D3D9PixelShaderImpl>(
      object, expectedOwnerDevice,
      dxmt9::d3d9::pe::PeConcreteObjectKind::PixelShader, out);
}
