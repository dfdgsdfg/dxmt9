#pragma once

#include "d3d9_pe.hpp"

#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_com_membership.hpp"
#include "d3d9_pe_diagnostic_observer.hpp"
#include "d3d9_pe_validated_object.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_capture_layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>


// Local COM ownership policies consume only category-qualified references.
// They preserve the exact interface subobject pointer used by the public D3D9
// call and make a cross-kind IUnknown reinterpret_cast unrepresentable.
struct D3D9PeRetainStateBlockRef {
  template<typename Ref>
  void operator()(Ref value) const noexcept {
    if (auto* object = value.raw()) object->AddRef();
  }
};

struct D3D9PeReleaseStateBlockRef {
  template<typename Ref>
  void operator()(Ref value) const noexcept {
    if (auto* object = value.raw()) object->Release();
  }
};

inline constexpr D3D9PeRetainStateBlockRef d3d9PeRetainStateBlockRef{};
inline constexpr D3D9PeReleaseStateBlockRef d3d9PeReleaseStateBlockRef{};

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
    D3D9PeValidatedVertexBuffer* out) noexcept;
HRESULT D3D9PeValidateIndexBuffer(
    IDirect3DIndexBuffer9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedIndexBuffer* out) noexcept;
HRESULT D3D9PeValidateVertexShader(
    IDirect3DVertexShader9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedVertexShader* out) noexcept;
HRESULT D3D9PeValidatePixelShader(
    IDirect3DPixelShader9* object, const void* expectedOwnerDevice,
    D3D9PeValidatedPixelShader* out) noexcept;
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
  void addCategoryRefs() noexcept {
    categories_.forEachOwnedComRef(d3d9PeRetainStateBlockRef);
  }
  void releaseCategoryRefs() noexcept {
    categories_.forEachOwnedComRef(d3d9PeReleaseStateBlockRef);
    categories_.writer().clear();
  }

  void releaseSavedVdecl() noexcept {
    if (vdecl_) {
      vdecl_->Release();
      vdecl_ = nullptr;
    }
  }
};

#include "d3d9_pe_recorder_flush_facade.inc.hpp"

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

// True when the PE wrapper currently has a successful Lock outstanding.
// Used by IDirect3DDevice9::UpdateSurface to enforce the wined3d invariant
// that the source surface must not be locked when the copy is initiated.
void D3D9PeInvalidateVertexBufferReadonlyCache(
    const D3D9PeValidatedVertexBuffer& buffer) noexcept;
