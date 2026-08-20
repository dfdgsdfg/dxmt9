#pragma once

#include "d3d9_pe.hpp"

#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_capture_layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define DXMT9_PE_CALLSITE_PC() (__builtin_return_address(0))
#else
#define DXMT9_PE_CALLSITE_PC() nullptr
#endif

// Opaque register-sized handle to one entry sample of the PE call-tracking
// diagnostic (DXMT9_PE_RECORDER_STATS). The sample itself is a ~96-byte record
// owned by d3d9_pe_device.cpp; only this index crosses a hot-path signature.
// See "Observer boundary" in agents/rules/codebase_conventions.rules.md.
using D3D9PePresentCallSlot = std::uint32_t;
static constexpr D3D9PePresentCallSlot kD3D9PePresentCallSlotNone =
    static_cast<D3D9PePresentCallSlot>(-1);

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
  // Query::Issue is the only child-side record. It takes a PeWireObjectRef
  // rather than opaque bytes because opaque legacy-record bytes cannot express
  // a canonical handle reference -- the builder needs the ref to append and retain it.
  // The former byte-oriented AppendRecordForChild died with the last legacy
  // child record.
  virtual HRESULT AppendQueryIssueForChild(
      std::uint32_t flags,
      const dxmt9::d3d9::pe::PeWireObjectRef &query) = 0;
  virtual HRESULT FlushPeRecorderForBufferHazardForChild(D9CBuffer *buffer) = 0;
  // PE call-tracking diagnostic (DXMT9_PE_RECORDER_STATS). No diagnostic
  // payload type crosses this interface: the fire-and-forget entry note returns
  // nothing, and a call that also wants the paired return log takes a
  // register-sized slot handle into the device's own sample storage. Every one
  // of these is a single cached-bool test when tracking is off.
  virtual void NotifyPeFirstCallAfterPresentForChild(
      const char *callName, const void *callerPc = nullptr) noexcept = 0;
  virtual D3D9PePresentCallSlot PushPeCallScopeForChild(
      const char *callName, const void *callerPc) noexcept = 0;
  virtual void NotifyPeCallScopeReturnForChild(D3D9PePresentCallSlot slot,
                                               const char *callName,
                                               HRESULT hr) noexcept = 0;
  virtual void PopPeCallScopeForChild(D3D9PePresentCallSlot slot) noexcept = 0;
  virtual void NotifyRenderTapeObjectDefineForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &object,
      std::span<const std::byte> descriptor,
      std::span<const std::byte> immutablePayload = {}) noexcept = 0;
  virtual void NotifyRenderTapeObjectDestroyForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept = 0;
  virtual void NotifyRenderTapeResourceMutationForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &object,
      dxmt9::d3d9::RenderTapeMutationKind kind, std::uint32_t subresource,
      std::uint64_t byteOffset, std::span<const std::byte> bytes,
      dxmt9::d3d9::RenderTapeBufferMutationDisposition bufferDisposition =
          dxmt9::d3d9::RenderTapeBufferMutationDisposition::Plain) noexcept = 0;
  virtual void NotifyRenderTapeOrderedControlForChild(
      const dxmt9::d3d9::RenderTapeOrderedControlHeader &fixed,
      std::span<const std::byte> payload) noexcept = 0;
  virtual bool IsRenderTapeCaptureActiveForChild() const noexcept = 0;
  virtual bool IsRenderTapeCaptureTrackingEnabledForChild() const noexcept = 0;
  virtual void AbortRenderTapeCaptureForChild() noexcept = 0;
  virtual void RejectRenderTapeCaptureForChild(
      dxmt9::d3d9::RenderTapeCaptureRejectionReason reason,
      const dxmt9::d3d9::pe::PeWireObjectRef &object,
      std::uint32_t subresource,
      const dxmt9::d3d9::RenderTapeCaptureLayoutDiagnostic &diagnostic =
          {}) noexcept = 0;
  virtual dxmt9::d3d9::RenderTapeFullSnapshotStatus
  RenderTapeFullSnapshotStatusForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &object,
      std::uint32_t subresource, std::uint32_t fullRowBytes,
      std::uint32_t fullRows, std::uint64_t fullBytes) const noexcept = 0;
  virtual void NotifyRenderTapeBlockMutationForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &object,
      std::uint32_t subresource,
      const dxmt9::d3d9::RenderTapeBlockLockLayout &layout,
      std::span<const std::byte> bytes) noexcept = 0;
  virtual void NotifyRenderTapeLinearMutationForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &object,
      std::uint32_t subresource,
      const dxmt9::d3d9::RenderTapeLinearLockLayout &layout,
      std::span<const std::byte> bytes) noexcept = 0;
  virtual void NotifyRenderTapeSurfaceAliasForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &surface,
      const dxmt9::d3d9::pe::PeWireObjectRef &parentTexture,
      std::uint32_t subresource,
      const D9CSurfaceDesc &descriptor) noexcept = 0;
  virtual void NotifyRenderTapeStandaloneSurfaceForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &surface,
      const D9CSurfaceDesc &descriptor) noexcept = 0;

  // PE-shadow stateblock support. Captures the device's current transform /
  // shader-constant / vdecl shadow into `out`, AddRef'ing any held COM
  // pointers. The caller (D3D9StateBlockImpl) owns the resulting shadow and
  // is responsible for Release on destruction.
  virtual void CaptureStateBlockShadowForChild(D3D9StateBlockShadow &out) = 0;

protected:
  ~D3D9PeRecorderFlush() = default;
};

// RAII scope for the child wrappers whose entry note is paired with a return
// log. Off (the default) it is one virtual call that answers "not tracked" from
// a cached bool, and the object is two words; nothing is constructed, copied,
// or torn down. On, the entry sample lives in the device's own storage and this
// holds only its slot handle, so the ~96-byte record never rides a wrapper's
// call frame or signature. The destructor releases the slot, so an early return
// that skips finish() leaks nothing; finish() is a no-op for an untracked
// scope, which reproduces the pre-scope behaviour of "no entry note, no return
// log". See "Observer boundary" in agents/rules/codebase_conventions.rules.md.
class D3D9PeChildCallScope {
public:
  D3D9PeChildCallScope(D3D9PeRecorderFlush *recorder, const char *callName,
                       const void *callerPc) noexcept {
    if (!recorder)
      return;
    const D3D9PePresentCallSlot slot =
        recorder->PushPeCallScopeForChild(callName, callerPc);
    if (slot == kD3D9PePresentCallSlotNone)
      return;
    recorder_ = recorder;
    slot_ = slot;
  }
  D3D9PeChildCallScope(const D3D9PeChildCallScope &) = delete;
  D3D9PeChildCallScope &operator=(const D3D9PeChildCallScope &) = delete;
  ~D3D9PeChildCallScope() noexcept {
    if (recorder_)
      recorder_->PopPeCallScopeForChild(slot_);
  }

  HRESULT finish(const char *callName, HRESULT hr) noexcept {
    if (recorder_)
      recorder_->NotifyPeCallScopeReturnForChild(slot_, callName, hr);
    return hr;
  }

private:
  D3D9PeRecorderFlush *recorder_ = nullptr;
  D3D9PePresentCallSlot slot_ = kD3D9PePresentCallSlotNone;
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
                                             std::uint64_t hash,
                                             D3D9PeRecorderFlush *recorder = nullptr);
IDirect3DPixelShader9 *CreatePePixelShader(D9CShader *shader,
                                           IDirect3DDevice9 *device,
                                           std::uint64_t hash,
                                           D3D9PeRecorderFlush *recorder = nullptr);
IDirect3DVertexDeclaration9 *CreatePeVertexDecl(D9CVertexDecl *decl,
                                                IDirect3DDevice9 *device,
                                                D3D9PeRecorderFlush *recorder = nullptr);
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
