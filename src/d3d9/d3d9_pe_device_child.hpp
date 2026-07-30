#pragma once

#include "d3d9_pe.hpp"

#include "d3d9_pe_chunk_v2_builder.hpp"
#include "d3d9_pe_state_shadow.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define DXMT9_PE_CALLSITE_PC() (__builtin_return_address(0))
#else
#define DXMT9_PE_CALLSITE_PC() nullptr
#endif

static constexpr std::size_t D3D9PePresentCallStackDepth = 12;

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

struct D3D9PePresentCallToken {
  bool tracked = false;
  std::uint64_t ordinal = 0;
  std::uint32_t callCount = 0;
  std::int64_t returnNs = 0;
  std::int64_t entryNs = 0;
  const void *callerPc = nullptr;
  std::uint32_t threadId = 0;
  std::uint8_t callerStackCount = 0;
  std::array<const void *, D3D9PePresentCallStackDepth> callerStack{};
};

struct D3D9PeRecorderFlush {
  virtual HRESULT FlushPeRecorderForChild() = 0;
  virtual bool IsStateBlockRecordingForChild() const = 0;
  virtual void InvalidateStateBlockShadowForChild() = 0;
  virtual void AddDefaultPoolResourceRefForChild() = 0;
  virtual void ReleaseDefaultPoolResourceRefForChild() = 0;
  virtual bool IsChunkRecorderEnabledForChild() const = 0;
  // Query::Issue is the only child-side record. It takes a PeWireObjectRef
  // rather than opaque bytes because opaque legacy-record bytes cannot express
  // a V2 handle reference -- the builder needs the ref to append and retain it.
  // The former byte-oriented AppendRecordForChild died with the last legacy
  // child record.
  virtual HRESULT AppendQueryIssueForChild(
      std::uint32_t flags,
      const dxmt9::d3d9::pe::PeWireObjectRef &query) = 0;
  virtual HRESULT FlushPeRecorderForBufferHazardForChild(D9CBuffer *buffer) = 0;
  virtual D3D9PePresentCallToken NotifyPeFirstCallAfterPresentForChild(
      const char *callName, const void *callerPc = nullptr) noexcept {
    (void)callName;
    (void)callerPc;
    return {};
  }
  virtual void NotifyPeCallReturnAfterPresentForChild(
      const D3D9PePresentCallToken &token,
      const char *callName, HRESULT hr) noexcept {
    (void)token;
    (void)callName;
    (void)hr;
  }

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
                                             IDirect3DDevice9 *device,
                                             std::uint64_t hash);
IDirect3DPixelShader9 *CreatePePixelShader(D9CShader *shader,
                                           IDirect3DDevice9 *device,
                                           std::uint64_t hash);
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
const dxmt9::d3d9::pe::PeWireObjectRef &
D3D9PeWireSurface(IDirect3DSurface9 *surface);
// True when the PE wrapper currently has a successful Lock outstanding.
// Used by IDirect3DDevice9::UpdateSurface to enforce the wined3d invariant
// that the source surface must not be locked when the copy is initiated.
bool D3D9PeSurfaceIsLocked(IDirect3DSurface9 *surface);
D9CTexture *D3D9PeRawTexture(IDirect3DBaseTexture9 *texture);
const dxmt9::d3d9::pe::PeWireObjectRef &
D3D9PeWireTexture(IDirect3DBaseTexture9 *texture);
D9CBuffer *D3D9PeRawVertexBuffer(IDirect3DVertexBuffer9 *buffer);
D9CBuffer *D3D9PeRawIndexBuffer(IDirect3DIndexBuffer9 *buffer);
const dxmt9::d3d9::pe::PeWireObjectRef &
D3D9PeWireVertexBuffer(IDirect3DVertexBuffer9 *buffer);
const dxmt9::d3d9::pe::PeWireObjectRef &
D3D9PeWireIndexBuffer(IDirect3DIndexBuffer9 *buffer);
void D3D9PeInvalidateVertexBufferReadonlyCache(IDirect3DVertexBuffer9 *buffer);
D9CShader *D3D9PeRawVertexShader(IDirect3DVertexShader9 *shader);
D9CShader *D3D9PeRawPixelShader(IDirect3DPixelShader9 *shader);
const dxmt9::d3d9::pe::PeWireObjectRef &
D3D9PeWireVertexShader(IDirect3DVertexShader9 *shader);
const dxmt9::d3d9::pe::PeWireObjectRef &
D3D9PeWirePixelShader(IDirect3DPixelShader9 *shader);
std::uint64_t D3D9PeVertexShaderHash(IDirect3DVertexShader9 *shader);
std::uint64_t D3D9PePixelShaderHash(IDirect3DPixelShader9 *shader);
D9CVertexDecl *D3D9PeRawVertexDecl(IDirect3DVertexDeclaration9 *decl);
const dxmt9::d3d9::pe::PeWireObjectRef &
D3D9PeWireVertexDecl(IDirect3DVertexDeclaration9 *decl);
D9CQuery *D3D9PeRawQuery(IDirect3DQuery9 *query);
const dxmt9::d3d9::pe::PeWireObjectRef &
D3D9PeWireQuery(IDirect3DQuery9 *query);
