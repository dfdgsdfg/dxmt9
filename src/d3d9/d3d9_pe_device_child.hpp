#pragma once

#include "d3d9_pe.hpp"

#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_com_membership.hpp"
#include "d3d9_pe_diagnostic_observer.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_capture_layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

template <typename Raw, typename Wire>
struct D3D9PeValidatedObject {
  Raw* raw = nullptr;
  Wire wire{};
};

using D3D9PeValidatedSurface = D3D9PeValidatedObject<
    D9CSurface, dxmt9::d3d9::pe::SurfaceRef>;
using D3D9PeValidatedTexture = D3D9PeValidatedObject<
    D9CTexture, dxmt9::d3d9::pe::TextureRef>;
using D3D9PeValidatedBuffer = D3D9PeValidatedObject<
    D9CBuffer, dxmt9::d3d9::pe::BufferRef>;
using D3D9PeValidatedShader = D3D9PeValidatedObject<
    D9CShader, dxmt9::d3d9::pe::ShaderRef>;
using D3D9PeValidatedDeclaration = D3D9PeValidatedObject<
    D9CVertexDecl, dxmt9::d3d9::pe::DeclarationRef>;
using D3D9PeValidatedQuery = D3D9PeValidatedObject<
    D9CQuery, dxmt9::d3d9::pe::QueryRef>;

// Each implementation lives beside the concrete final wrapper class. RTTI
// proves concrete membership before any member access; the returned value is
// a kind-qualified POD snapshot suitable for device/recorder code.
HRESULT D3D9PeValidateSurface(
    IDirect3DSurface9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedSurface* out,
    dxmt9::d3d9::pe::PeSurfaceQualification qualification =
        dxmt9::d3d9::pe::PeSurfaceQualification::Any) noexcept;
HRESULT D3D9PeValidateTexture(
    IDirect3DBaseTexture9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedTexture* out) noexcept;
HRESULT D3D9PeValidateVertexBuffer(
    IDirect3DVertexBuffer9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedBuffer* out) noexcept;
HRESULT D3D9PeValidateIndexBuffer(
    IDirect3DIndexBuffer9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedBuffer* out) noexcept;
HRESULT D3D9PeValidateVertexShader(
    IDirect3DVertexShader9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedShader* out) noexcept;
HRESULT D3D9PeValidatePixelShader(
    IDirect3DPixelShader9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedShader* out) noexcept;
HRESULT D3D9PeValidateVertexDecl(
    IDirect3DVertexDeclaration9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedDeclaration* out) noexcept;
HRESULT D3D9PeValidateQuery(
    IDirect3DQuery9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedQuery* out) noexcept;

#if defined(__GNUC__) || defined(__clang__)
#define DXMT9_PE_CALLSITE_PC() (__builtin_return_address(0))
#else
#define DXMT9_PE_CALLSITE_PC() nullptr
#endif

// PE-side snapshot taken by D3D9StateBlockImpl::Capture(). Lives entirely in
// the PE process; never crosses the unix boundary. Holds enough state to
// restore the transforms / shader constants / vertex declaration the test
// suite checks via IDirect3DStateBlock9 round-trips. AddRef policy: vdecl is
// AddRef'd by the snapshot owner; release in the destructor and on overwrite.
class D3D9StateBlockShadow {
 private:
  FixedStateTable<kPeRenderStateSlots> renderStates_{};
  FixedStateMatrix<kPeTextureStageSlots, kPeTextureStageStateSlots>
      textureStageStates_{};
  FixedStateMatrix<kPeSamplerSlots, kPeSamplerStateSlots> samplerStates_{};
  FixedTransformTable transforms_{};
  // Fixed candidate categories beyond the four keyed scalar tables.  Pointer
  // entries are owned by this wrapper snapshot (copying the shadow AddRefs
  // them); the recorder candidate uses the same value shape but has its own
  // release boundary.
  StateBlockRecorded categories_{};
  PeStateBlockConstRecorded constants_{};
  bool hasVdecl_ = false;
  IDirect3DVertexDeclaration9 *vdecl_ = nullptr;

  // True once D3D9StateBlockImpl::ctor has populated the shadow once.
  // CaptureStateBlockShadowForChild uses this to distinguish the initial
  // snapshot (which fixes the tracked-keys set) from a later
  // D3D9StateBlockImpl::Capture() call (which only refreshes values of
  // already-tracked keys).
  bool initialized_ = false;

 public:
  class Writer {
   public:
    RenderStateTableView renderStates() noexcept {
      return RenderStateTableView(shadow_.renderStates_);
    }
    TssTableView textureStageStates() noexcept {
      return TssTableView(shadow_.textureStageStates_);
    }
    SamplerStateTableView samplerStates() noexcept {
      return SamplerStateTableView(shadow_.samplerStates_);
    }
    TypedTransformTableView transforms() noexcept {
      return TypedTransformTableView(shadow_.transforms_);
    }
    StateBlockRecorded::Writer categories() noexcept {
      return shadow_.categories_.writer();
    }
    PeStateBlockConstRecorded& constants() noexcept {
      return shadow_.constants_;
    }
    void setInitialized(bool initialized) noexcept {
      shadow_.initialized_ = initialized;
    }
    void replaceVdecl(bool tracked,
                      IDirect3DVertexDeclaration9 *value) noexcept {
      shadow_.releaseSavedVdecl();
      shadow_.hasVdecl_ = tracked && value != nullptr;
      if (tracked && value) {
        value->AddRef();
        shadow_.vdecl_ = value;
      }
    }
    void discardVdecl() noexcept {
      shadow_.releaseSavedVdecl();
      shadow_.hasVdecl_ = false;
    }

   private:
    explicit Writer(D3D9StateBlockShadow& shadow) noexcept : shadow_(shadow) {}
    D3D9StateBlockShadow& shadow_;
    friend class D3D9StateBlockShadow;
  };

  class Snapshot {
   public:
    ConstRenderStateTableView renderStates() const noexcept {
      return ConstRenderStateTableView(shadow_.renderStates_);
    }
    ConstTssTableView textureStageStates() const noexcept {
      return ConstTssTableView(shadow_.textureStageStates_);
    }
    ConstSamplerStateTableView samplerStates() const noexcept {
      return ConstSamplerStateTableView(shadow_.samplerStates_);
    }
    ConstTypedTransformTableView transforms() const noexcept {
      return ConstTypedTransformTableView(shadow_.transforms_);
    }
    const StateBlockRecorded& categories() const noexcept {
      return shadow_.categories_.snapshot();
    }
    const PeStateBlockConstRecorded& constants() const noexcept {
      return shadow_.constants_;
    }
    bool initialized() const noexcept { return shadow_.initialized_; }
    bool hasVdecl() const noexcept { return shadow_.hasVdecl_; }
    IDirect3DVertexDeclaration9 *vdecl() const noexcept {
      return shadow_.vdecl_;
    }

   private:
    explicit Snapshot(const D3D9StateBlockShadow& shadow) noexcept
        : shadow_(shadow) {}
    const D3D9StateBlockShadow& shadow_;
    friend class D3D9StateBlockShadow;
  };

  Writer writer() noexcept { return Writer(*this); }
  Snapshot snapshot() const noexcept { return Snapshot(*this); }

  D3D9StateBlockShadow() = default;
  D3D9StateBlockShadow(const D3D9StateBlockShadow& other) { *this = other; }
  D3D9StateBlockShadow& operator=(const D3D9StateBlockShadow& other) {
    if (this == &other) return *this;
    releaseSavedVdecl();
    releaseCategoryRefs();
    renderStates_ = other.renderStates_;
    textureStageStates_ = other.textureStageStates_;
    samplerStates_ = other.samplerStates_;
    transforms_ = other.transforms_;
    categories_ = other.categories_;
    constants_ = other.constants_;
    hasVdecl_ = other.hasVdecl_;
    initialized_ = other.initialized_;
    vdecl_ = other.vdecl_;
    if (vdecl_) vdecl_->AddRef();
    addCategoryRefs();
    return *this;
  }
  D3D9StateBlockShadow(D3D9StateBlockShadow&& other) noexcept {
    *this = std::move(other);
  }
  D3D9StateBlockShadow& operator=(D3D9StateBlockShadow&& other) noexcept {
    if (this == &other) return *this;
    releaseSavedVdecl();
    releaseCategoryRefs();
    renderStates_ = other.renderStates_;
    textureStageStates_ = other.textureStageStates_;
    samplerStates_ = other.samplerStates_;
    transforms_ = other.transforms_;
    categories_ = other.categories_;
    constants_ = other.constants_;
    hasVdecl_ = other.hasVdecl_;
    initialized_ = other.initialized_;
    vdecl_ = other.vdecl_;
    other.vdecl_ = nullptr;
    other.hasVdecl_ = false;
    other.initialized_ = false;
    other.categories_.writer().clear();
    return *this;
  }
  ~D3D9StateBlockShadow() {
    releaseSavedVdecl();
    releaseCategoryRefs();
  }

  void copyCategoriesFrom(const StateBlockRecorded& source) noexcept {
    releaseCategoryRefs();
    categories_ = source;
    addCategoryRefs();
  }
  void clearCategoriesOwned() noexcept { releaseCategoryRefs(); }

 private:
  static void addRef(void* value) noexcept {
    if (value) reinterpret_cast<IUnknown*>(value)->AddRef();
  }
  static void releaseRef(void*& value) noexcept {
    if (value) {
      reinterpret_cast<IUnknown*>(value)->Release();
      value = nullptr;
    }
  }
  void addCategoryRefs() noexcept {
    categories_.forEachOwnedComRef(
        [](void* value) { D3D9StateBlockShadow::addRef(value); });
  }
  void releaseCategoryRefs() noexcept {
    categories_.forEachOwnedComRef(
        [](void* value) { D3D9StateBlockShadow::releaseRef(value); });
    categories_.writer().clear();
  }

  void releaseSavedVdecl() noexcept {
    if (vdecl_) {
      vdecl_->Release();
      vdecl_ = nullptr;
    }
  }
};

struct D3D9PeRecorderFlush {
  // Child wrappers are owned by the D3D9 device/COM contract and therefore
  // use ordinary non-atomic ULONG refs in their PE implementation.  Backend
  // chunk pins are independent private retains.  D3D9StateBlockImpl is the
  // documented exception: its snapshot can be owned through independent
  // device/child paths and uses an atomic counter in its implementation.
  virtual HRESULT FlushPeRecorderForChild() = 0;
  virtual bool IsStateBlockRecordingForChild() const = 0;
  // Capture/Apply are cold, compound operations.  The device implementation
  // conditionally takes its existing recursive recorder mutex here (only for
  // MULTITHREADED/forced-lock devices); the pair deliberately keeps the
  // ordinary single-thread setter path untouched.
  virtual void LockStateBlockOperationForChild() noexcept = 0;
  virtual void UnlockStateBlockOperationForChild() noexcept = 0;
  virtual bool IsStateBlockRecorderPoisonedForChild() const noexcept = 0;
  virtual HRESULT PrepareStateBlockApplyForChild(
      const D3D9StateBlockShadow &shadow) = 0;
  virtual void CommitStateBlockApplyForChild(
      const D3D9StateBlockShadow &shadow) noexcept = 0;
  virtual void DiscardPreparedStateBlockApplyForChild() noexcept = 0;
  virtual void PoisonStateBlockRecorderForChild() noexcept = 0;
  virtual void InvalidateStateBlockShadowForChild() = 0;
  virtual void AddDefaultPoolResourceRefForChild() noexcept = 0;
  virtual void ReleaseDefaultPoolResourceRefForChild() noexcept = 0;
  virtual bool IsChunkRecorderEnabledForChild() const = 0;
  // Query::Issue is the only child-side record. It takes a PeWireObjectRef
  // rather than opaque bytes because opaque legacy-record bytes cannot express
  // a canonical handle reference -- the builder needs the ref to append and retain it.
  // The former byte-oriented AppendRecordForChild died with the last legacy
  // child record.
  virtual HRESULT AppendQueryIssueForChild(
      std::uint32_t flags,
      const dxmt9::d3d9::pe::QueryRef &query) = 0;
  virtual HRESULT FlushPeRecorderForBufferHazardForChild(D9CBuffer *buffer) = 0;
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
  virtual HRESULT CaptureStateBlockShadowForChild(
      D3D9StateBlockShadow &out,
      StateBlockCaptureDisposition disposition) = 0;

protected:
  ~D3D9PeRecorderFlush() = default;
};

class D3D9PeChildOperationGuard final {
public:
  explicit D3D9PeChildOperationGuard(
      D3D9PeRecorderFlush* recorder) noexcept : recorder_(recorder) {
    if (recorder_) recorder_->LockStateBlockOperationForChild();
  }
  ~D3D9PeChildOperationGuard() noexcept {
    if (recorder_) recorder_->UnlockStateBlockOperationForChild();
  }

  D3D9PeChildOperationGuard(const D3D9PeChildOperationGuard&) = delete;
  D3D9PeChildOperationGuard& operator=(
      const D3D9PeChildOperationGuard&) = delete;

private:
  D3D9PeRecorderFlush* recorder_ = nullptr;
};

// Enabled-only RAII scope for tracked child calls. Entry helpers branch on the
// wrapper's cached nullable concrete observer before this object's lifetime
// begins, so the disabled edge constructs no scope and performs no diagnostic
// token work or D3D9PeRecorderFlush virtual dispatch.
class D3D9PeChildCallScope {
public:
  D3D9PeChildCallScope(D3D9PeDiagnosticObserver &observer,
                       const char *callName,
                       const void *callerPc) noexcept {
    const D3D9PePresentCallSlot slot =
        observer.pushCallScope(callName, callerPc);
    if (slot == kD3D9PePresentCallSlotNone)
      return;
    observer_ = &observer;
    slot_ = slot;
  }
  D3D9PeChildCallScope(const D3D9PeChildCallScope &) = delete;
  D3D9PeChildCallScope &operator=(const D3D9PeChildCallScope &) = delete;
  ~D3D9PeChildCallScope() noexcept {
    if (observer_)
      observer_->popCallScope(slot_);
  }

  HRESULT finish(const char *callName, HRESULT hr) noexcept {
    if (observer_)
      observer_->notifyCallScopeReturn(slot_, callName, hr);
    return hr;
  }

private:
  D3D9PeDiagnosticObserver *observer_ = nullptr;
  D3D9PePresentCallSlot slot_ = kD3D9PePresentCallSlotNone;
};

struct D3D9PeNullChildCallScope {
  HRESULT finish(const char *, HRESULT hr) const noexcept {
    return hr;
  }
};

inline constexpr D3D9PeNullChildCallScope d3d9PeNullChildCallScope{};

template<typename Body>
__attribute__((always_inline))
inline HRESULT d3d9PeWithChildCallScope(
    D3D9PeDiagnosticObserver *observer, const char *callName,
    const void *callerPc, Body &&body) noexcept {
  if (!observer) {
    return std::forward<Body>(body)(d3d9PeNullChildCallScope);
  }
  D3D9PeChildCallScope peCall(*observer, callName, callerPc);
  return std::forward<Body>(body)(peCall);
}

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
                                   D3D9PeDiagnosticObserver *diagnostics = nullptr,
                                   bool trackDefaultPool = true,
                                   void *userMemory = nullptr,
                                   int32_t userMemoryPitch = 0) noexcept;
IDirect3DTexture9 *CreatePeTexture(D9CTexture *texture,
                                   IDirect3DDevice9 *device,
                                   D3D9PeRecorderFlush *recorder = nullptr,
                                   D3D9PeDiagnosticObserver *diagnostics = nullptr,
                                   void *userMemory = nullptr,
                                   int32_t userMemoryPitch = 0) noexcept;
IDirect3DVolumeTexture9 *
CreatePeVolumeTexture(D9CTexture *texture, IDirect3DDevice9 *device,
                      D3D9PeRecorderFlush *recorder = nullptr,
                      D3D9PeDiagnosticObserver *diagnostics = nullptr) noexcept;
IDirect3DCubeTexture9 *
CreatePeCubeTexture(D9CTexture *texture, IDirect3DDevice9 *device,
                    D3D9PeRecorderFlush *recorder = nullptr,
                    D3D9PeDiagnosticObserver *diagnostics = nullptr) noexcept;
IDirect3DVertexBuffer9 *
CreatePeVertexBuffer(D9CBuffer *buffer, IDirect3DDevice9 *device,
                     D3D9PeRecorderFlush *recorder = nullptr,
                     D3D9PeDiagnosticObserver *diagnostics = nullptr) noexcept;
IDirect3DIndexBuffer9 *
CreatePeIndexBuffer(D9CBuffer *buffer, IDirect3DDevice9 *device,
                    D3D9PeRecorderFlush *recorder = nullptr,
                    D3D9PeDiagnosticObserver *diagnostics = nullptr) noexcept;
IDirect3DVertexShader9 *CreatePeVertexShader(D9CShader *shader,
                                             IDirect3DDevice9 *device,
                                             std::uint64_t hash,
                                             D3D9PeRecorderFlush *recorder = nullptr) noexcept;
IDirect3DPixelShader9 *CreatePePixelShader(D9CShader *shader,
                                           IDirect3DDevice9 *device,
                                           std::uint64_t hash,
                                           D3D9PeRecorderFlush *recorder = nullptr) noexcept;
IDirect3DVertexDeclaration9 *CreatePeVertexDecl(D9CVertexDecl *decl,
                                                IDirect3DDevice9 *device,
                                                D3D9PeRecorderFlush *recorder = nullptr) noexcept;
IDirect3DQuery9 *CreatePeQuery(D9CQuery *query, IDirect3DDevice9 *device,
                               D3D9PeRecorderFlush *recorder = nullptr,
                               D3D9PeDiagnosticObserver *diagnostics = nullptr) noexcept;
IDirect3DStateBlock9 *
CreatePeStateBlock(D9CStateBlock *stateBlock, IDirect3DDevice9 *device,
                   D3D9PeRecorderFlush *recorder = nullptr,
                   D3D9PeDiagnosticObserver *diagnostics = nullptr,
                   StateBlockCaptureDisposition disposition =
                       StateBlockCaptureDisposition::All) noexcept;
IDirect3DSwapChain9Ex *
CreatePeSwapChain(D9CSwapChain *swapChain, IDirect3DDevice9 *device,
                  D3D9PeRecorderFlush *recorder = nullptr,
                  D3D9PeDiagnosticObserver *diagnostics = nullptr,
                  bool extended = false,
                  DWORD presentFlagsShadow = 0) noexcept;

D9CSurface *D3D9PeRawSurface(IDirect3DSurface9 *surface);
const dxmt9::d3d9::pe::SurfaceRef &
D3D9PeWireSurface(IDirect3DSurface9 *surface);
// True when the PE wrapper currently has a successful Lock outstanding.
// Used by IDirect3DDevice9::UpdateSurface to enforce the wined3d invariant
// that the source surface must not be locked when the copy is initiated.
bool D3D9PeSurfaceIsLocked(IDirect3DSurface9 *surface);
D9CTexture *D3D9PeRawTexture(IDirect3DBaseTexture9 *texture);
const dxmt9::d3d9::pe::TextureRef &
D3D9PeWireTexture(IDirect3DBaseTexture9 *texture);
D9CBuffer *D3D9PeRawVertexBuffer(IDirect3DVertexBuffer9 *buffer);
D9CBuffer *D3D9PeRawIndexBuffer(IDirect3DIndexBuffer9 *buffer);
const dxmt9::d3d9::pe::BufferRef &
D3D9PeWireVertexBuffer(IDirect3DVertexBuffer9 *buffer);
const dxmt9::d3d9::pe::BufferRef &
D3D9PeWireIndexBuffer(IDirect3DIndexBuffer9 *buffer);
void D3D9PeInvalidateVertexBufferReadonlyCache(IDirect3DVertexBuffer9 *buffer);
D9CShader *D3D9PeRawVertexShader(IDirect3DVertexShader9 *shader);
D9CShader *D3D9PeRawPixelShader(IDirect3DPixelShader9 *shader);
const dxmt9::d3d9::pe::ShaderRef &
D3D9PeWireVertexShader(IDirect3DVertexShader9 *shader);
const dxmt9::d3d9::pe::ShaderRef &
D3D9PeWirePixelShader(IDirect3DPixelShader9 *shader);
std::uint64_t D3D9PeVertexShaderHash(IDirect3DVertexShader9 *shader);
std::uint64_t D3D9PePixelShaderHash(IDirect3DPixelShader9 *shader);
D9CVertexDecl *D3D9PeRawVertexDecl(IDirect3DVertexDeclaration9 *decl);
const dxmt9::d3d9::pe::DeclarationRef &
D3D9PeWireVertexDecl(IDirect3DVertexDeclaration9 *decl);
D9CQuery *D3D9PeRawQuery(IDirect3DQuery9 *query);
const dxmt9::d3d9::pe::QueryRef &
D3D9PeWireQuery(IDirect3DQuery9 *query);
