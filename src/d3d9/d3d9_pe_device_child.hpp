#pragma once

#include "d3d9_pe.hpp"

#include "d3d9_pe_state_shadow.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// PE-side snapshot taken by D3D9StateBlockImpl::Capture(). Lives entirely in
// the PE process; never crosses the unix boundary. Holds enough state to
// restore the transforms / shader constants / vertex declaration the test
// suite checks via IDirect3DStateBlock9 round-trips. AddRef policy: vdecl is
// AddRef'd by the snapshot owner; release in the destructor and on overwrite.
struct D3D9StateBlockShadow {
  FixedTransformTable transforms{};
  std::vector<std::uint8_t> vsConstF;
  std::vector<std::uint8_t> vsConstI;
  std::vector<std::uint8_t> vsConstB;
  std::vector<std::uint8_t> psConstF;
  std::vector<std::uint8_t> psConstI;
  std::vector<std::uint8_t> psConstB;
  bool hasVdecl = false;
  IDirect3DVertexDeclaration9 *vdecl = nullptr;

  // True once D3D9StateBlockImpl::ctor has populated the shadow once.
  // CaptureStateBlockShadowForChild uses this to distinguish the initial
  // snapshot (which fixes the tracked-keys set) from a later
  // D3D9StateBlockImpl::Capture() call (which only refreshes values of
  // already-tracked keys).
  bool initialized = false;
};

struct D3D9PeRecorderFlush {
  virtual HRESULT FlushPeRecorderForChild() = 0;
  virtual bool IsStateBlockRecordingForChild() const = 0;
  virtual void InvalidateStateBlockShadowForChild() = 0;
  virtual void AddDefaultPoolResourceRefForChild() = 0;
  virtual void ReleaseDefaultPoolResourceRefForChild() = 0;
  virtual bool IsChunkRecorderEnabledForChild() const = 0;
  virtual HRESULT AppendRecordForChild(const void *data, size_t bytes) = 0;

  // PE-shadow stateblock support. Captures the device's current transform /
  // shader-constant / vdecl shadow into `out`, AddRef'ing any held COM
  // pointers. The caller (D3D9StateBlockImpl) owns the resulting shadow and
  // is responsible for Release on destruction.
  virtual void CaptureStateBlockShadowForChild(D3D9StateBlockShadow &out) = 0;

protected:
  ~D3D9PeRecorderFlush() = default;
};

// T4 (D3D9Ex shared-handle, SYSTEMMEM partial): when userMemory is non-null
// the wrapper aliases the caller-supplied buffer in LockRect and skips the
// staging path. userMemoryPitch is the row pitch the wrapper should report
// (0 means "unused"). This is only used for SYSTEMMEM 2D-texture and
// offscreen-plain-surface creation paths; all other call sites keep the
// defaults and behave exactly as before.
IDirect3DSurface9 *CreatePeSurface(D9CSurface *surface,
                                   IDirect3DDevice9 *device,
                                   IUnknown *container,
                                   D3D9PeRecorderFlush *recorder = nullptr,
                                   bool trackDefaultPool = true,
                                   void *userMemory = nullptr,
                                   int32_t userMemoryPitch = 0);
IDirect3DTexture9 *CreatePeTexture(D9CTexture *texture,
                                   IDirect3DDevice9 *device,
                                   D3D9PeRecorderFlush *recorder = nullptr,
                                   void *userMemory = nullptr,
                                   int32_t userMemoryPitch = 0);
IDirect3DVolumeTexture9 *
CreatePeVolumeTexture(D9CTexture *texture, IDirect3DDevice9 *device,
                      D3D9PeRecorderFlush *recorder = nullptr);
IDirect3DCubeTexture9 *
CreatePeCubeTexture(D9CTexture *texture, IDirect3DDevice9 *device,
                    D3D9PeRecorderFlush *recorder = nullptr);
IDirect3DVertexBuffer9 *
CreatePeVertexBuffer(D9CBuffer *buffer, IDirect3DDevice9 *device,
                     D3D9PeRecorderFlush *recorder = nullptr);
IDirect3DIndexBuffer9 *
CreatePeIndexBuffer(D9CBuffer *buffer, IDirect3DDevice9 *device,
                    D3D9PeRecorderFlush *recorder = nullptr);
IDirect3DVertexShader9 *CreatePeVertexShader(D9CShader *shader,
                                             IDirect3DDevice9 *device);
IDirect3DPixelShader9 *CreatePePixelShader(D9CShader *shader,
                                           IDirect3DDevice9 *device);
IDirect3DVertexDeclaration9 *CreatePeVertexDecl(D9CVertexDecl *decl,
                                                IDirect3DDevice9 *device);
IDirect3DQuery9 *CreatePeQuery(D9CQuery *query, IDirect3DDevice9 *device,
                               D3D9PeRecorderFlush *recorder = nullptr);
IDirect3DStateBlock9 *
CreatePeStateBlock(D9CStateBlock *stateBlock, IDirect3DDevice9 *device,
                   D3D9PeRecorderFlush *recorder = nullptr);
IDirect3DSwapChain9Ex *
CreatePeSwapChain(D9CSwapChain *swapChain, IDirect3DDevice9 *device,
                  D3D9PeRecorderFlush *recorder = nullptr,
                  bool extended = false,
                  DWORD presentFlagsShadow = 0);

D9CSurface *D3D9PeRawSurface(IDirect3DSurface9 *surface);
// True when the PE wrapper currently has a successful Lock outstanding.
// Used by IDirect3DDevice9::UpdateSurface to enforce the wined3d invariant
// that the source surface must not be locked when the copy is initiated.
bool D3D9PeSurfaceIsLocked(IDirect3DSurface9 *surface);
D9CTexture *D3D9PeRawTexture(IDirect3DBaseTexture9 *texture);
D9CBuffer *D3D9PeRawVertexBuffer(IDirect3DVertexBuffer9 *buffer);
D9CBuffer *D3D9PeRawIndexBuffer(IDirect3DIndexBuffer9 *buffer);
D9CShader *D3D9PeRawVertexShader(IDirect3DVertexShader9 *shader);
D9CShader *D3D9PeRawPixelShader(IDirect3DPixelShader9 *shader);
D9CVertexDecl *D3D9PeRawVertexDecl(IDirect3DVertexDeclaration9 *decl);
