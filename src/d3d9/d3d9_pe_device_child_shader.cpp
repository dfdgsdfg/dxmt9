/* src/d3d9/d3d9_pe_device_child_shader.cpp — PE-side child COM wrappers
 * for IDirect3DVertexShader9 and IDirect3DPixelShader9. */

#include "d3d9_pe_device_child.hpp"

#include <cstdint>

static inline HRESULT hr32(int32_t r) { return (HRESULT)r; }

/* ── VertexShader ───────────────────────────────────────────────────────────
 */

class D3D9VertexShaderImpl final : public IDirect3DVertexShader9 {
  ULONG refs_ = 1;
  D9CShader *s_;
  IDirect3DDevice9 *device_;
  std::uint64_t hash_ = 0;
  dxmt9::d3d9::pe::PeWireObjectRef wireObject_{};

public:
  D3D9VertexShaderImpl(D9CShader *s, IDirect3DDevice9 *device,
                       std::uint64_t hash)
      : s_(s), device_(device), hash_(hash) {
    if (device_)
      device_->AddRef();
    dxmt9::d3d9::pe::cacheWireObjectRef(
        s_, D9C_CHUNK_HANDLE_KIND_SHADER,
        dxmt9c_shader_get_wire_identity, wireObject_);
  }
  ~D3D9VertexShaderImpl() {
    dxmt9c_shader_release(s_);
    if (device_)
      device_->Release();
  }

  D9CShader *raw() const { return s_; }
  const dxmt9::d3d9::pe::PeWireObjectRef &wireObject() const {
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
  std::uint64_t hash_ = 0;
  dxmt9::d3d9::pe::PeWireObjectRef wireObject_{};

public:
  D3D9PixelShaderImpl(D9CShader *s, IDirect3DDevice9 *device,
                      std::uint64_t hash)
      : s_(s), device_(device), hash_(hash) {
    if (device_)
      device_->AddRef();
    dxmt9::d3d9::pe::cacheWireObjectRef(
        s_, D9C_CHUNK_HANDLE_KIND_SHADER,
        dxmt9c_shader_get_wire_identity, wireObject_);
  }
  ~D3D9PixelShaderImpl() {
    dxmt9c_shader_release(s_);
    if (device_)
      device_->Release();
  }

  D9CShader *raw() const { return s_; }
  const dxmt9::d3d9::pe::PeWireObjectRef &wireObject() const {
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
                                             std::uint64_t hash) {
  return new D3D9VertexShaderImpl(shader, device, hash);
}

IDirect3DPixelShader9 *CreatePePixelShader(D9CShader *shader,
                                           IDirect3DDevice9 *device,
                                           std::uint64_t hash) {
  return new D3D9PixelShaderImpl(shader, device, hash);
}

D9CShader *D3D9PeRawVertexShader(IDirect3DVertexShader9 *shader) {
  return shader ? static_cast<D3D9VertexShaderImpl *>(shader)->raw() : nullptr;
}

D9CShader *D3D9PeRawPixelShader(IDirect3DPixelShader9 *shader) {
  return shader ? static_cast<D3D9PixelShaderImpl *>(shader)->raw() : nullptr;
}

const dxmt9::d3d9::pe::PeWireObjectRef &
D3D9PeWireVertexShader(IDirect3DVertexShader9 *shader) {
  static const dxmt9::d3d9::pe::PeWireObjectRef empty{};
  return shader ? static_cast<D3D9VertexShaderImpl *>(shader)->wireObject()
                : empty;
}

const dxmt9::d3d9::pe::PeWireObjectRef &
D3D9PeWirePixelShader(IDirect3DPixelShader9 *shader) {
  static const dxmt9::d3d9::pe::PeWireObjectRef empty{};
  return shader ? static_cast<D3D9PixelShaderImpl *>(shader)->wireObject()
                : empty;
}

std::uint64_t D3D9PeVertexShaderHash(IDirect3DVertexShader9 *shader) {
  return shader ? static_cast<D3D9VertexShaderImpl *>(shader)->hash() : 0;
}

std::uint64_t D3D9PePixelShaderHash(IDirect3DPixelShader9 *shader) {
  return shader ? static_cast<D3D9PixelShaderImpl *>(shader)->hash() : 0;
}
